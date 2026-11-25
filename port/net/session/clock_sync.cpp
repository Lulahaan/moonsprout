// M5.3b time sync: offset estimator, one-way p99 trackers, phase controller, delay formula.
#include "clock_sync.hpp"
#include "types.hpp"

#include <algorithm>
#include <cmath>

namespace mppm::session {

namespace {
constexpr int64_t kOffsetWindowNs   = 2000000000LL;
constexpr int64_t kPhaseIntervalNs  = 250000000LL;
constexpr size_t kPhaseMinSamples   = 3;
} // namespace

void OffsetEstimator::add(int64_t nowNs, uint32_t t0, uint32_t t1, uint32_t t2, uint32_t t3)
{
	// rtt = (t3 - t0) - (t2 - t1); offset = ((t1 - t0) + (t2 - t3)) / 2, the two halves kept
	// modular so a large clock difference between machines does not overflow.
	const int32_t total = us_diff(t3, t0);
	const int32_t hold  = us_diff(t2, t1);
	int32_t rtt         = total - hold;
	if (total < 0 || hold < 0 || rtt < 0) return; // a stale echo or a clock step: ignore
	const uint32_t a    = t1 - t0;           // offset + forward delay
	const uint32_t b    = t2 - t3;           // offset - backward delay
	const uint32_t off  = b + (uint32_t)(us_diff(a, b) / 2);
	while (!m_samples.empty() && nowNs - m_samples.front().at > kOffsetWindowNs) m_samples.pop_front();
	m_samples.push_back({ nowNs, (uint32_t)rtt, off });
	const Sample* best = &m_samples.front();
	uint64_t sum = 0;
	for (const Sample& s : m_samples) {
		if (s.rtt < best->rtt) best = &s;
		sum += s.rtt;
	}
	// The window's estimate: the mean offset of the samples within 1.5 ms of the minimum RTT (their
	// one-way asymmetry averages out), then smoothed (time constant about 30 samples, 1 s at 30 Hz): the clocks
	// of two machines drift by parts per million, the queueing asymmetry is what moves.
	double acc = 0;
	int n      = 0;
	for (const Sample& s : m_samples) {
		if (s.rtt > best->rtt + 1500) continue;
		acc += us_diff(s.offset, best->offset);
		++n;
	}
	const double w = (double)(int64_t)best->offset + acc / n; // may leave the u32 range: wrapped below
	if (!m_valid) {
		m_base = best->offset;
		m_frac = w - (double)(int64_t)best->offset;
	} else {
		const double target = (double)us_diff((uint32_t)(int64_t)std::llround(w), m_base) + (w - std::llround(w));
		m_frac += (target - m_frac) * 0.03;
	}
	const int64_t whole = (int64_t)std::floor(m_frac);
	m_base += (uint32_t)whole;
	m_frac -= (double)whole;
	m_offset = m_base + (m_frac >= 0.5 ? 1u : 0u);
	m_rtt    = best->rtt;
	m_rttAvg = (uint32_t)(sum / m_samples.size());
	m_valid  = true;
}

void P99Tracker::add(int64_t nowNs, int32_t us)
{
	while (!m_samples.empty() && nowNs - m_samples.front().at > m_window) m_samples.pop_front();
	m_samples.push_back({ nowNs, us });
	m_cacheAt = -1;
}

int32_t P99Tracker::p99(int64_t nowNs)
{
	while (!m_samples.empty() && nowNs - m_samples.front().at > m_window) m_samples.pop_front();
	if (m_samples.empty()) return 0;
	if (m_cacheAt >= 0 && nowNs - m_cacheAt < 100000000LL) return m_cache;
	std::vector<int32_t> v;
	v.reserve(m_samples.size());
	for (const Sample& s : m_samples) v.push_back(s.us);
	const size_t k = (size_t)((double)(v.size() - 1) * 0.99 + 0.5);
	std::nth_element(v.begin(), v.begin() + (ptrdiff_t)k, v.end());
	m_cache   = v[k];
	m_cacheAt = nowNs;
	return m_cache;
}

int64_t wrap_phase(int64_t eNs, int64_t periodNs)
{
	int64_t r = eNs % periodNs;
	if (r < 0) r += periodNs;
	if (r >= periodNs / 2) r -= periodNs;
	return r;
}

void PhaseController::add(int64_t nowNs, int64_t eNs, int64_t slewAtSampleNs)
{
	if (!m_any) {
		m_any        = true;
		m_lastUpdate = nowNs - kPhaseIntervalNs;
	}
	if (m_pending.size() < 256) m_pending.push_back({ eNs, slewAtSampleNs });
}

bool PhaseController::update(int64_t nowNs, int64_t slewNowNs, int64_t& newSlewNs)
{
	if (!m_any || nowNs - m_lastUpdate < kPhaseIntervalNs || m_pending.size() < kPhaseMinSamples) return false;
	std::vector<int64_t> settled, all;
	for (const Sample& s : m_pending) {
		all.push_back(s.e + s.slew);
		if (s.slew == 0) settled.push_back(s.e);
	}
	std::vector<int64_t>& use = settled.size() >= kPhaseMinSamples ? settled : all;
	std::nth_element(use.begin(), use.begin() + (ptrdiff_t)(use.size() / 2), use.end());
	const int64_t median = use[use.size() / 2];
	m_pending.clear();
	m_lastUpdate = nowNs;
	// A residual under 0.2 ms is left alone (no chatter from the offset estimate's noise).
	if (median > -200000 && median < 200000 && slewNowNs == 0) return false;
	newSlewNs = slewNowNs - median;
	newSlewNs = std::clamp<int64_t>(newSlewNs, -kPhaseMaxNs, kPhaseMaxNs);
	return true;
}

} // namespace mppm::session
