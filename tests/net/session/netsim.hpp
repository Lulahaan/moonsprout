// M5.3b netsim: a virtual-clock simulation of a lockstep session (host, clients, spectators) over
// MemTransport, each endpoint with a fake game loop on its own retrace grid (slew, grid jump, tick
// alignment, late sampling) that pushes synthetic inputs, consumes bundles, runs load barriers and
// computes checksums from the consumed stream. Used by p2net_selftest (tests/net/session/selftest.cpp).
#pragma once

#include "../../../port/net/session/endpoint.hpp"
#include "../../../port/net/session/game_side.hpp"
#include "../../../port/net/session/mem_transport.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace netsim {

using namespace mppm::session;

struct PeerSpec {
	bool spectator     = false;
	int64_t clockOffsetNs = 0;              // this peer's steady clock = true time + offset
	int64_t phaseNs    = 0;                 // initial retrace grid phase (true time)
	mppm::net::Impair impair;               // send side
	std::map<uint32_t, int64_t> loads;      // barrier tick -> load duration (ns)
	uint32_t desyncAt  = 0;                 // checksums differ from this tick on
	uint32_t vanishAt  = 0;                 // the endpoint disappears after consuming this tick
	int64_t vanishNoticeNs = 0;             // ... and the others' transports notice it after this long (an ENet timeout)
	uint8_t contentXor = 0;                 // lobby test: a different content hash
	bool wrongSecret   = false;             // lobby test: a Hello with a wrong join secret
	uint32_t leaveAt   = 0;                 // leaves gracefully (BYE) after consuming this tick
	uint32_t doneAt    = 0;                 // says BYE "done" (its --exit-after-ticks) after consuming this tick
	uint32_t crashAt   = 0;                 // the crash screen after this tick: GameLink::leaveReq, the game stops
	uint32_t freezeAt  = 0;                 // the game stops after consuming this tick; the net side runs on
	int64_t hangLoadAt = -1;                // the load at this barrier tick never ends; the net side runs on
	uint32_t lobbyHostLeftAt = 0;           // client: the lobby server says "closed host_left" after this tick
};

struct Options {
	Mode mode          = Mode::Battle;
	uint32_t ticks     = 18000;
	std::vector<PeerSpec> peers;            // [0] = the host
	int64_t stepNs     = 500000;            // simulation step
	int64_t maxTimeNs  = 0;                 // 0 = ticks * P * 1.5 + 60 s
	bool verbose       = false;
	// delaychange: at true time `impairChangeAt`, peer `impairPeer` gets `impairNew`
	int64_t impairChangeAt = 0;
	int impairPeer     = -1;
	mppm::net::Impair impairNew;
	int64_t impairChange2At = 0;            // and back to its spec at this time
	bool longStart     = false;             // a START far above one datagram (Synced JSON and mod list)
	uint32_t kickAt    = 0;                 // the host removes member `kickMember` after consuming this tick
	uint32_t kickMember = 0;
};

struct PeerResult {
	uint32_t consumed = 0;
	uint64_t streamHash = 0;                // FNV over every consumed bundle up to `ticks`
	std::vector<uint64_t> hashAt;           // hash after each 1000 ticks
	StallStats stats;
	int endReason = kEndNone;
	uint32_t desyncTick = 0;
	uint8_t desyncSlot = kNoSlot;
	uint32_t endTick = 0;
	std::map<int, uint32_t> drops;
	std::vector<int64_t> releaseRun;        // true time each barrier's B+1 tick ran
	std::vector<uint32_t> releaseTicks;
	bool consumedBeforeRelease = false;
	std::vector<int32_t> phaseAbsUs;
	int delay = 0;
	std::vector<std::pair<int64_t, int>> delayLog; // (true time, d) changes
	bool neutralFromDrop = true;            // every bundle >= E had neutral input for the dropped seat
	uint32_t lastTagGapCheck = 0;
	std::vector<std::string> logs;
	size_t syncedLen = 0;                   // the Synced JSON of the START this endpoint got
	std::string mods;
};

struct Result {
	bool finished = false;
	bool hostStarted = false;
	std::vector<std::string> notes;         // the host's lobby notes, one per member
	std::vector<PeerResult> peers;
	int64_t endTimeNs = 0;
};

Result run(const Options& o);

// Synthetic input of `seat` at tick t (held for a few ticks, like a player).
Input gen_input(int seat, uint32_t t);

} // namespace netsim
