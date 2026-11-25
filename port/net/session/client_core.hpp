// M5.3b client (and spectator) state machine (docs/online/M5_3B_LOCKSTEP.md "Client"). Pure: time is
// passed in, every output goes through CoreEnv. Hello, the lobby view, START, input tagging with the
// delay rules, bundle intake and acks, releases, time sync, the phase controller, desync and end.
#pragma once

#include "clock_sync.hpp"
#include "delay.hpp"
#include "link.hpp"
#include "types.hpp"
#include "wire.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace mppm::session {

struct ClientParams {
	uint32_t member     = 0;
	uint64_t secret     = 0;
	std::string name    = "Player";
	bool spectator      = false;
	uint8_t build[32]   = {};
	uint8_t content[32] = {};
	std::string mods;
	int wantSlot        = -1;    // --slot
	bool autoReady      = false; // --ready
	bool phaseControl   = true;  // false for the test bots (they follow the host's game directly)
	int initialDelay    = 0;     // 0 = from the measured path (2 when unknown)
};

class ClientCore {
public:
	ClientCore(CoreEnv& env, const ClientParams& p);

	void on_connected(uint32_t peer, int64_t now);
	void on_disconnected(uint32_t peer, const std::string& reason, int64_t now);
	void on_datagram(uint32_t peer, const uint8_t* p, size_t n, int64_t recvNs, int64_t now);
	void on_game(const G2N& m, int64_t now);
	void set_ready(bool ready);
	void take_slot(int slot);
	void leave(int64_t now, uint8_t byeReason);
	void tick(int64_t now);
	// The lobby server says the host is gone (Closed host_left) or the connection failed.
	void lost(int reason, const std::string& logReason, const std::string& text);
	// The lobby server says the host left (Closed host_left). Before START that ends the session; after
	// START the P2P link decides, as on the host (HostCore::remove_member): the match goes on while the
	// host's packets keep coming and ends once the link is silent for kLobbyGoneGraceNs (the host's
	// lobby WebSocket can drop while its game plays on).
	void lobby_host_left(int64_t now);

	bool welcomed() const { return m_welcomed; }
	bool started() const { return m_started; }
	bool ended() const { return m_ended; }
	bool host_gone() const { return m_hostGone; }
	const SessionConfig& config() const { return m_cfg; }
	const wire::Lobby& lobby() const { return m_lobby; }
	int local_seat() const { return m_seat; }
	uint32_t member() const { return m_p.member; }
	int my_delay() const { return m_delay.current(); }
	bool delay_capped() const { return m_delay.capped(); }
	uint32_t rtt_us() const { return m_sync.valid() ? m_sync.rtt_us() : 0; }
	uint32_t ack() const { return m_ack; }
	uint8_t delay_of_seat(int s) const { return s >= 0 && s < kMaxSeats ? m_seatDelay[s] : 0; }
	// Phase statistics: |e| samples taken at least 1 s after the newest release.
	const std::vector<int32_t>& phase_samples() const { return m_phaseAbs; }
	uint64_t bytes_out() const { return m_bytesOut; }
	uint64_t bytes_in() const { return m_bytesIn; }
	uint8_t wait_mask() const { return m_waitMask; }

private:
	void send_input(int64_t now);
	void send_req(uint8_t what, int8_t value);
	void handle_bundle(const wire::BundlePkt& b, int64_t recvNs, int64_t now);
	void handle_release(uint32_t tick, uint16_t count, uint32_t hostUs, int64_t now);
	void store_tag(uint32_t tag, const Input& in);
	void end(int reason, const std::string& logReason, const std::string& text);
	int64_t due_local(uint32_t tick, int64_t& slewAt) const;
	void evaluate_delay(int64_t now);
	void request_lobby_wishes(int64_t now);

	CoreEnv& m_env;
	ClientParams m_p;
	uint32_t m_peer   = 0;
	bool m_helloSent  = false;
	bool m_welcomed   = false;
	bool m_started    = false;
	bool m_ended      = false;
	bool m_hostGone   = false;
	bool m_spectator  = false;
	SessionConfig m_cfg;
	wire::Lobby m_lobby;
	int m_seat        = -1;
	int64_t m_reqAt   = 0;
	int64_t m_slotReqAt = INT64_MIN; // the last REQ slot sent for the --slot wish (rate limit)
	// sync
	OffsetEstimator m_sync;
	P99Tracker m_down;
	uint32_t m_lastRecvTSend = 0;
	int64_t m_lastRecvAt = 0;
	int64_t m_lastPacket = 0;
	int64_t m_lastSend   = 0;
	int64_t m_dupAt      = 0;
	int64_t m_lastSyncLog = 0;
	int64_t m_lobbyGoneAt = 0; // the lobby said the host left after START; the P2P link decides
	// phase
	PhaseController m_phase;
	std::deque<std::array<int64_t, 3>> m_due; // tick, due ns, slew ns
	int64_t m_lastPhaseE = 0;
	int64_t m_shiftUntil = 0; // after a whole-tick grid move: no phase samples until then
	std::vector<int32_t> m_phaseAbs;
	int64_t m_releaseAt = 0;  // when the newest release was taken
	uint32_t m_releasedTick = 0;
	bool m_haveRelease = false;
	// inputs
	DelayPolicy m_delay;
	bool m_cappedNoted = false;
	int64_t m_lastDelayEval = 0;
	uint32_t m_lastTag = 0;
	Input m_prevInput;
	std::array<Input, 128> m_tagIn{};
	std::array<uint32_t, 128> m_tagOf{};
	// bundles
	std::map<uint32_t, bool> m_have; // received ticks above the contiguous ack
	uint32_t m_ack = 0;
	uint16_t m_releaseCount = 0;
	uint8_t m_up[kMaxSeats]   = { 255, 255, 255, 255 };
	uint8_t m_downT[kMaxSeats] = { 255, 255, 255, 255 };
	uint8_t m_blame[kMaxSeats] = { 0, 0, 0, 0 };
	uint8_t m_seatDelay[kMaxSeats] = { 0, 0, 0, 0 };
	uint8_t m_waitMask = 0;
	// barriers and checksums from the game
	uint32_t m_loadedTick = wire::kNoLoaded;
	uint16_t m_loadCount  = 0;
	uint32_t m_cksTick = 0, m_cksTotal = 0;
	std::deque<std::pair<uint32_t, std::string>> m_lines;
	uint32_t m_lastTickRead = 0;
	// desync wait
	int64_t m_desyncEndAt = 0;
	EndInfo m_pendingEnd;
	uint64_t m_bytesOut = 0, m_bytesIn = 0;
	uint32_t m_stallPermille = 0;
};

} // namespace mppm::session
