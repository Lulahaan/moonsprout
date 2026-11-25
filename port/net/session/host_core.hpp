// M5.3b host state machine (docs/online/M5_3B_LOCKSTEP.md "Host"). Pure: time is passed in, every
// output goes through CoreEnv. Members, Hello, the lobby room, START, bundle assembly with
// forward-on-complete, per-peer redundancy and acks, barriers, drops, the desync compare, blame and
// spectators.
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
#include <set>
#include <string>
#include <vector>

namespace mppm::session {

struct HostParams {
	Mode mode            = Mode::Battle;
	int stage            = 0;
	uint8_t handicap[2]  = { 0, 0 };
	int hostSlot         = 0;      // the host's seat; -1 = the lowest free one
	bool hostReady       = false;  // --ready: the host is ready ...
	bool autoStart       = false;  // ... and starts as soon as it can
	int expect           = 0;      // with autoStart: members connected first (host and spectators included); 0 = the seat count
	int maxMembers       = 8;
	std::string name     = "Player";
	uint8_t build[32]    = {};
	uint8_t content[32]  = {};
	std::string mods;
	std::string syncedJson = "{}";
	uint32_t syncedHash  = 0;
	uint32_t seed        = 1;
	uint32_t sessionTag  = 1;      // nonzero
	uint8_t tickOrder    = 1;
	float simK           = 3.6f;
	int initialDelay     = 0;      // 0 = from the measured paths (2 when unknown)
};

class HostCore {
public:
	HostCore(CoreEnv& env, const HostParams& p);

	// Lobby membership (mppm-server, or the test bots).
	void add_member(uint32_t id, const std::string& name, bool spectator, uint64_t auth, int64_t now);
	void remove_member(uint32_t id, uint8_t reason, int64_t now); // wire::DropReason
	void set_member_link(uint32_t id, uint8_t link);
	// Transport.
	// `source` names where the connection comes from (pending_source_key of the transport's remote) for
	// the per-source cap on unverified connections; "" = unknown (no per-source cap).
	void on_connected(uint32_t peer, uint32_t connectData, int64_t now, const std::string& source = {});
	void on_disconnected(uint32_t peer, const std::string& reason, int64_t now);
	void on_datagram(uint32_t peer, const uint8_t* p, size_t n, int64_t recvNs, int64_t now);
	// The host's own game thread.
	void on_game(const G2N& m, int64_t now);
	// Launcher and command line.
	void set_config(Mode mode, int stage, int h0, int h1);
	void set_ready(bool ready);
	void take_slot(int slot);
	bool start(int64_t now);
	void kick(uint32_t id, int64_t now);
	void drop_slot(int slot, int64_t now);
	void leave(int64_t now); // the host closes the session
	void tick(int64_t now);

	// State for the session and the UI.
	bool started() const { return m_started; }
	bool can_start() const;
	bool ended() const { return m_ended; }
	bool linger_done() const { return m_lingerDone; }
	const SessionConfig& config() const { return m_cfg; }
	wire::Lobby lobby_view() const;
	std::string status_line() const;
	int connected_count() const;
	int local_seat() const { return m_hostSlot; }
	uint32_t completed() const { return m_completed; }
	int delay_of_seat(int seat) const;
	int my_delay() const { return m_delay.current(); }
	uint8_t wait_mask() const { return m_waitMask; }
	std::string member_name(uint32_t id) const;
	uint32_t seat_member(int seat) const;
	const HostParams& params() const { return m_p; }
	uint64_t bytes_out() const { return m_bytesOut; }
	uint64_t bytes_in() const { return m_bytesIn; }
	uint32_t rtt_us_max() const;

private:
	struct Member {
		uint32_t id = 0;
		std::string name;
		bool spectator = false;
		bool known     = false; // MemberJoined seen (auth valid)
		uint64_t auth  = 0;
		uint32_t peer  = 0;
		bool connected = false; // Hello accepted
		bool self      = false;
		bool gone      = false; // left after START (kept for its name)
		int slot       = -1;
		bool chose     = false;   // the seat came from its own REQ (auto-seated members may be swapped)
		bool ready     = false;
		bool contentOk = true;
		std::string note;
		uint8_t link   = wire::kLinkUnknown;
		// time sync
		OffsetEstimator sync;
		P99Tracker up;
		uint16_t downP99 = 0;        // 0.1 ms
		uint32_t lastRecvTSend = 0;
		int64_t lastRecvAt = 0;
		// stream
		uint32_t ackBundle = 0;
		int64_t lastSend   = 0;
		int64_t dupAt      = 0;
		int64_t lastInput  = 0;
		int64_t lastPacket = 0;
		uint16_t loadCount = 0;
		uint8_t delay      = 0;   // its d, from its INPUT
		uint32_t cksTick = 0, cksTotal = 0;
		uint32_t cksDoneTick = 0;
		bool desynced = false;
		bool byeDone  = false;    // said BYE after its last tick (--exit-after-ticks): no drop notice
		int invalidWin = 0;
		int64_t invalidAt = 0;
		int64_t helloAt = 0;
		int64_t inputWinAt = 0;   // INPUT rate cap (kInputRateLimit per second)
		int inputWin = 0;
		int64_t lastSampleAt = 0; // the last INPUT taken as a time-sync sample
		int64_t lobbyGoneAt = 0;  // the lobby server said left/timeout after START; the P2P link decides
		uint8_t lobbyGoneReason = 0;
	};
	// A transport peer before its Hello is verified (section 10): the connectData it claims names a
	// member, but it is bound to that member (Member::peer) only by a Hello with the member's join
	// secret, so a peer cannot take another member's seat by claiming its id.
	struct PendingPeer {
		uint32_t member = 0;
		int64_t at      = 0;
		std::string source; // pending_source_key; "" = not capped per source
		std::vector<uint8_t> hello; // a Hello that arrived before the member's MemberJoined
	};
	struct Seat {
		bool used = false;
		uint32_t member = 0;
		std::array<Input, 128> in{};
		std::array<uint32_t, 128> tagOf{};
		std::array<int64_t, 128> at{};
		uint32_t contig = 0;     // highest contiguous tag received
		bool dropped = false;
		uint32_t dropE = 0;
		std::deque<std::pair<int64_t, bool>> blame; // completion time, blamed
	};

	Member* by_id(uint32_t id);
	const Member* by_id(uint32_t id) const;
	Member* by_peer(uint32_t peer);
	Member& self();
	void pending_hello(uint32_t peer, int64_t now);
	void drop_pending(uint32_t peer, uint8_t reason, const std::string& text);
	void handle_hello(Member& m, const uint8_t* p, size_t n, int64_t now);
	void handle_input(Member& m, const wire::InputPkt& in, int64_t recvNs, int64_t now);
	void invalid(Member& m, int64_t now);
	void malformed(Member& m, int64_t now);
	void broadcast_lobby();
	void send_control(uint32_t peer, const std::vector<uint8_t>& d);
	void send_all_control(const std::vector<uint8_t>& d, bool spectatorsToo = true);
	void send_bundle(Member& m, int64_t now);
	void store_input(int seat, uint32_t tag, const Input& in, int64_t now);
	void try_complete(int64_t now);
	void try_release(int64_t now);
	uint8_t loading_mask(uint16_t n) const;
	void check_stuck(int64_t now);
	void drop_seat(int seat, uint8_t reason, int64_t now);
	void member_lost(Member& m, uint8_t reason, int64_t now);
	void compare_cks(Member& m, int64_t now);
	void desync(Member& m, uint32_t tick, uint32_t hostTotal, uint32_t peerTotal, int64_t now);
	void end(int reason, const std::string& logReason, const std::string& text);
	void host_input(uint32_t t, const Input& in, int64_t now);
	int64_t due_of(uint32_t tick) const;
	int64_t max_down_us() const;
	int blame_permille(int seat, int64_t now);
	void maybe_auto_start(int64_t now);
	void changed() { ++m_generation; m_lobbyDirty = true; }

	CoreEnv& m_env;
	HostParams m_p;
	std::vector<Member> m_members; // [0] = the host itself
	std::map<uint32_t, PendingPeer> m_pending; // transport peer -> claim, until its Hello is verified
	std::set<uint32_t> m_banned;               // kicked or banned member ids: never pending, never added again
	int m_seats = 2;
	int m_hostSlot = 0;
	uint32_t m_generation = 1;
	bool m_lobbyDirty = true;
	bool m_started = false;
	bool m_ended   = false;
	SessionConfig m_cfg;
	Seat m_seat[kMaxSeats];
	std::vector<std::array<Input, kMaxSeats>> m_bundles; // [tick - 1]
	uint32_t m_completed = 0;
	uint8_t m_waitMask = 0;
	std::vector<wire::Event> m_events;
	uint16_t m_eventSeq = 0;
	// host's own input
	DelayPolicy m_delay;
	uint32_t m_lastTag = 0;
	Input m_prevInput;
	int64_t m_lastDelayEval = 0;
	bool m_cappedNoted = false;
	uint32_t m_hostRead = 0;     // the newest tick the host's game read (it waits for that bundle)
	bool m_hostReadLate = false; // the host's newest read came after its due time (its own long tick)
	// host's own ticks
	std::deque<std::pair<uint32_t, int64_t>> m_due; // (tick, due ns)
	// barriers
	std::map<uint16_t, uint32_t> m_barrierTick; // count -> B (the host's own)
	std::map<uint16_t, int64_t> m_barrierAt;    // count -> when the host reached it
	std::array<int64_t, kMaxSeats> m_blockSince{}; // seat -> holding up every bundle since (0 = not)
	uint16_t m_released = 0;
	uint32_t m_releaseTick = 0;
	int64_t m_releaseNs = 0;
	// checksums
	std::map<uint32_t, uint32_t> m_ownCks;
	std::deque<std::pair<uint32_t, std::string>> m_ownLines;
	// desync / end
	int64_t m_desyncEndAt = 0;
	EndInfo m_pendingEnd;
	// linger
	bool m_lingering = false;
	bool m_lingerDone = false;
	uint32_t m_lingerTick = 0;
	int64_t m_lingerUntil = 0;
	uint64_t m_bytesOut = 0, m_bytesIn = 0;
	int64_t m_lastKeepalive = 0;
};

// Caps on unverified connections (plan section 10): at most this many pending (before their Hello is
// verified) in all, below the transport's 8 peer slots so a real member can always connect ...
constexpr size_t kMaxPendingTotal     = 4;
// ... at most this many from one source address (loopback is not capped: it is the host's own machine) ...
constexpr size_t kMaxPendingPerSource = 2;
// ... and at most this many claiming one member id.
constexpr size_t kMaxPendingPerMember = 2;
// The per-source key of a transport peer's remote ("1.2.3.4:5000" -> "1.2.3.4"; an ICE or relay path
// is one per member, so it keeps its port); "" for loopback, which is not capped.
std::string pending_source_key(const std::string& remote);

// "Louie has different mods: ..." (D20); empty when both hashes match.
std::string content_note(const std::string& name, const std::string& hostMods, const std::string& peerMods, bool buildSame,
                         bool contentSame);
// net_synced_hash of the launcher (settings.cpp) over a JSON object's text: FNV-1a 32 over the
// sorted "key=<value dump>\n" lines. 2166136261 for "{}" or text that is not an object.
uint32_t synced_hash_of(const std::string& json);

} // namespace mppm::session
