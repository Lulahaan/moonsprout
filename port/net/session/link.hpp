// M5.3b game link: the messages between a game loop (the real game thread, a test bot or a netsim
// fake game) and its net endpoint, and the interface the pure cores use for their outputs.
#pragma once

#include "spsc.hpp"
#include "types.hpp"
#include "wire.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace mppm::session {

// Game -> net.
struct G2N {
	enum Kind : uint8_t {
		LocalInput, // `tick` read, `input` sampled (players only)
		TickDue,    // `tick` is due at `ns` (steady), `slewNs` = remaining grid correction when it was taken
		Checksum,   // `tick`, `total`, `line`
		Loaded,     // outermost nonsim leave at `tick`; `count` = barriers since the session start
		Begun,      // p2_net_session_begin ran (tick 0)
		Leave,      // leave the session (`count` = wire::ByeReason)
		Done,       // --exit-after-ticks reached at `tick`
	};
	Kind kind     = LocalInput;
	uint32_t tick = 0;
	uint32_t count = 0;
	uint32_t total = 0;
	int64_t ns     = 0;
	int64_t slewNs = 0;
	Input input;
	char line[wire::kMaxCks + 1] = {};
};

// Net -> game.
struct N2G {
	enum Kind : uint8_t {
		Bundle,  // `tick`, `mask`, `seat[]`
		Release, // barrier `tick` number `count` released at `ns` (local steady)
		Start,   // the SessionConfig is in GameLink::config
		End,     // GameLink::end says why
		Drop,    // seat `mask` dropped from `tick` on
	};
	Kind kind     = Bundle;
	uint32_t tick = 0;
	uint32_t count = 0;
	uint8_t mask  = 0;
	int64_t ns    = 0;
	Input seat[kMaxSeats];
};

struct EndInfo {
	int reason = kEndNone;
	std::string text;            // one or two sentences for the UI
	std::string logReason;       // done|desync|host_left|dropped|left|error
	uint32_t desyncTick = 0;
	uint8_t desyncSlot  = kNoSlot;
	uint32_t hostTotal = 0, peerTotal = 0;
	std::string hostLine;        // the host's checksum line for desyncTick when known
	std::vector<std::string> peerLines; // host: the lines the others sent for desyncTick
};

// Wait state for the overlay (net thread writes, game thread reads).
struct WaitInfo {
	uint8_t waitMask   = 0;      // seats the host is waiting for
	bool hostIsCause   = false;  // client: no bundle because the host itself is late
};

struct GameLink {
	Spsc<G2N, 4096> g2n;
	Spsc<N2G, 8192> n2g;
	std::mutex mu;               // the rare payloads below
	SessionConfig config;
	int localSeat = -1;          // -1 spectator
	EndInfo end;
	std::atomic<uint32_t> waitMask{ 0 };
	std::atomic<uint32_t> loadingMask{ 0 }; // seats whose load the host still waits for (barrier)
	// Set from any thread (the crash screen, whose thread must not push to g2n): the net thread
	// leaves the session with this wire::ByeReason as soon as it sees it (0 = none).
	std::atomic<uint8_t> leaveReq{ 0 };
	// The tick whose bundle the game thread holds for (the stall loop, past its due time), 0 = none.
	std::atomic<uint32_t> gameWait{ 0 };
};

// What a core reports to its session (net thread).
struct CoreNote {
	enum Kind {
		LobbyChanged, // the lobby state (members, seats, config) changed
		Started,      // START sent or received: GameLink::config is set
		Ended,        // the session is over (EndInfo in GameLink::end)
		Notice,       // a toast: `kind2` = Notice kind, `title`, `text`
		Welcomed,     // client: the host accepted the Hello
		PeerGone,     // host: member `member`'s transport peer should be released
	};
	Kind kind = LobbyChanged;
	int kind2 = 0;
	uint32_t member = 0;
	std::string title;
	std::string text;
};

// The outputs of a pure core. Implemented by Endpoint (port/net/session/endpoint.cpp).
class CoreEnv {
public:
	virtual ~CoreEnv() = default;
	virtual bool send(uint32_t peer, bool control, const std::vector<uint8_t>& data, bool flush) = 0;
	virtual void disconnect(uint32_t peer, uint32_t reason) = 0;
	virtual void to_game(const N2G& m) = 0;
	virtual void note(const CoreNote& n) = 0;
	virtual int64_t slew_get() = 0;
	virtual void slew_set(int64_t ns) = 0;
	// Moves the game's tick grid `ticks` whole ticks earlier (< 0 later); false when not supported.
	virtual bool tick_shift(int ticks) = 0;
	virtual void log(const std::string& line) = 0; // without the "mppm: net: " prefix
	virtual GameLink& link() = 0;
};

} // namespace mppm::session
