// M5.3b game side of a link (docs/online/M5_3B_LOCKSTEP.md "Ticks and readiness", "Barriers"): the
// bundle table, readiness, barrier releases (grid jump and tick alignment), stall statistics. Runs
// on the game thread (the real seam) or in a netsim fake game. No game headers.
#pragma once

#include "link.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace mppm::session {

class GameHooks {
public:
	virtual ~GameHooks() = default;
	virtual int64_t now() = 0;
	virtual uint32_t grid_jump(int64_t localNs) = 0;    // p2_vi_grid_jump
	virtual void align_tick(uint32_t count) = 0;        // p2_host_align_tick
	virtual int64_t retrace_time(uint32_t count) = 0;   // p2_vi_retrace_time_ns
	virtual int64_t next_retrace_estimate() = 0;        // before a jump, for the log
	virtual int64_t margin_ns() = 0;                    // late-sampling margin
	virtual void log(const std::string& line) = 0;      // without "mppm: net: "
};

struct StallStats {
	uint64_t ticks = 0, stalled = 0;
	double maxMs = 0.0;
	uint64_t barriers = 0;
	double barrierMs = 0.0;
	// the current 900-tick window
	uint64_t winTicks = 0, winStalled = 0;
	double winMaxMs = 0.0, winBarrierMs = 0.0;
};

class GameSide {
public:
	GameSide(GameLink& link, GameHooks& hooks);
	void poll();
	bool started() const { return m_started; }
	bool ended() const { return m_ended; }
	// Bundle `tick` present, no unreleased barrier before it, and at a barrier the release time
	// reached (applies a release when the game takes it).
	bool ready(uint32_t tick);
	// The session cannot deliver `tick` any more (ended, or the host is gone and the bundle missing).
	bool over(uint32_t tick);
	// Copies bundle `tick` (after ready) and forgets older ones.
	bool take(uint32_t tick, Input out[kMaxSeats], uint8_t& mask);
	bool have(uint32_t tick) const { return m_bundles.count(tick) != 0; }

	void begun();
	void input(uint32_t tick, const Input& in);
	void tick_due(uint32_t tick, int64_t dueNs, int64_t slewNs);
	void checksum(uint32_t tick, uint32_t total, const char* line);
	void loaded(uint32_t tick);         // outermost nonsim leave in the session
	void leave(uint8_t byeReason);
	void done(uint32_t tick);
	bool barrier_pending() const { return m_pending; }
	uint32_t barrier_tick() const { return m_pendingTick; }
	uint16_t load_count() const { return m_loadCount; }

	// Statistics: one call per consumed tick; waitedNs = how long the read waited past its due time.
	void note_read(uint32_t tick, int64_t waitedPastDueNs, bool barrier);
	// The late-sampling wait (before the read) held past the tick's due time for readiness.
	void note_prewait(uint32_t tick, int64_t waitedPastDueNs)
	{
		m_preTick = tick;
		m_preNs   = waitedPastDueNs;
	}
	const StallStats& stats() const { return m_st; }
	// The wait the overlay shows.
	void wait_state(bool waiting, uint32_t tick, int64_t sinceNs, bool loading);
	bool waiting(uint32_t& tick, int64_t& sinceNs, bool& loading) const;
	// Dropped seats (from DROP): seat -> effective tick.
	const std::map<int, uint32_t>& drops() const { return m_drops; }

private:
	void push(const G2N& g);
	GameLink& m_link;
	GameHooks& m_hooks;
	bool m_started = false;
	bool m_ended   = false;
	struct B {
		uint8_t mask;
		Input seat[kMaxSeats];
	};
	std::unordered_map<uint32_t, B> m_bundles;
	std::map<uint16_t, std::pair<uint32_t, int64_t>> m_releases; // count -> (tick, local ns)
	bool m_pending        = false;
	uint32_t m_pendingTick = 0;
	uint16_t m_pendingCount = 0;
	int64_t m_pendingAt   = 0;
	uint16_t m_loadCount  = 0;
	uint32_t m_alignTick  = 0;
	int64_t m_alignNs     = 0;
	StallStats m_st;
	bool m_waiting = false;
	uint32_t m_waitTick = 0;
	int64_t m_waitSince = 0;
	bool m_waitLoading = false;
	std::map<int, uint32_t> m_drops;
	std::vector<G2N> m_backlog; // G2N messages the full ring did not take
	uint32_t m_preTick = 0;
	int64_t m_preNs    = 0;
};

} // namespace mppm::session
