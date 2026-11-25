// M5.3b time sync (D11, D12; docs/online/M5_3B_LOCKSTEP.md "Time sync and phase"). Pure: every time
// is passed in. Wire times are u32 microseconds of each side's steady clock (they wrap; the offset
// between two machines' clocks is kept modulo 2^32 us, differences are int32).
#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace mppm::session {

// NTP-style offset of a remote clock (remote - local, modulo 2^32 us), from the minimum-RTT sample
// of the last 2 s.
class OffsetEstimator {
public:
	// t0: our send time (echoed back), t1: remote receive time, t2: remote send time (all remote
	// times in remote us), t3: our receive time (local us); nowNs local steady ns.
	void add(int64_t nowNs, uint32_t t0, uint32_t t1, uint32_t t2, uint32_t t3);
	bool valid() const { return m_valid; }
	uint32_t offset_us() const { return m_offset; }   // remote = local + offset
	uint32_t rtt_us() const { return m_rtt; }
	uint32_t rtt_avg_us() const { return m_rttAvg; }
	// Local time (us) of a remote time.
	uint32_t to_local(uint32_t remoteUs) const { return remoteUs - m_offset; }
	uint32_t to_remote(uint32_t localUs) const { return localUs + m_offset; }

private:
	struct Sample {
		int64_t at;
		uint32_t rtt;
		uint32_t offset;
	};
	std::deque<Sample> m_samples;
	bool m_valid      = false;
	uint32_t m_offset = 0;
	uint32_t m_base   = 0;   // smoothed estimate = m_base + m_frac (us)
	double m_frac     = 0;
	uint32_t m_rtt    = 0;
	uint32_t m_rttAvg = 0;
};

// p99 of samples (us) over a sliding window (default 5 s).
class P99Tracker {
public:
	explicit P99Tracker(int64_t windowNs = 5000000000LL) : m_window(windowNs) { }
	void add(int64_t nowNs, int32_t us);
	bool valid() const { return !m_samples.empty(); }
	int32_t p99(int64_t nowNs); // 0 when empty
	size_t size() const { return m_samples.size(); }

private:
	struct Sample {
		int64_t at;
		int32_t us;
	};
	std::deque<Sample> m_samples;
	int64_t m_window;
	int64_t m_cacheAt = -1;
	int32_t m_cache   = 0;
};

// Phase controller (8.8). Samples: e = dueLocal(hostTick) - hostDueLocal (ns, not wrapped: whole
// ticks are corrected too, up to kPhaseMaxNs) and the slew still to be applied when dueLocal was taken. At most every 250 ms with
// at least 3 samples since the last update it returns a new remaining correction: the current one
// minus the median predicted final error (e + remaining at the sample); samples taken with no
// correction pending are preferred.
class PhaseController {
public:
	void add(int64_t nowNs, int64_t eNs, int64_t slewAtSampleNs);
	// True and the new remaining correction when an update is due.
	bool update(int64_t nowNs, int64_t slewNowNs, int64_t& newSlewNs);
	void reset() { m_pending.clear(); }

private:
	struct Sample {
		int64_t e;
		int64_t slew;
	};
	std::vector<Sample> m_pending;
	int64_t m_lastUpdate = 0;
	bool m_any = false;
};

// Wraps a phase error into [-P/2, P/2).
int64_t wrap_phase(int64_t eNs, int64_t periodNs);

} // namespace mppm::session
