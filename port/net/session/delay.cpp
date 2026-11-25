// M5.3b input delay: the formula and the change rules (see delay.hpp).
#include "delay.hpp"
#include "types.hpp"

#include <algorithm>

namespace mppm::session {

namespace {
constexpr int kBlameAddPermille  = 20;  // 2%
constexpr int kBlameLowPermille  = 5;   // 0.5%: the blame extra may go at a boundary
constexpr int64_t kBlameWindowNs = 3000000000LL;
} // namespace

int delay_for_path(int64_t pathUs, int* wish)
{
	if (pathUs < 0) pathUs = 0;
	const int64_t num = (pathUs + 3000) * 1000; // ns
	int64_t d         = (num + kTickNs - 1) / kTickNs;
	if (d < 1) d = 1;
	if (wish) *wish = (int)std::min<int64_t>(d, 1000);
	return (int)std::min<int64_t>(d, kMaxDelay);
}

void DelayPolicy::init(int d, int64_t nowNs)
{
	m_d = m_pathD = m_wish = std::clamp(d, 1, m_cap);
	m_blameExtra = 0;
	m_lastBlame  = nowNs + 12000000000LL; // no blame step in the first 16 s: the stage load's long ticks are no link problem
	m_lowBlame   = true;
}

int DelayPolicy::target() const { return std::clamp(m_pathD + m_blameExtra, 1, m_cap); }

DelayPolicy::Change DelayPolicy::evaluate(int64_t nowNs, int64_t pathUs, int blamePermille)
{
	Change why = None;
	if (pathUs >= 0) {
		int wish = 0;
		m_pathD  = delay_for_path(pathUs, &wish);
		m_wish   = wish + m_blameExtra;
		if (m_pathD > m_cap) m_pathD = m_cap;
	}
	m_lowBlame = blamePermille <= kBlameLowPermille;
	// The blame rate covers the last 3 s: after any change it still counts bundles of the old delay,
	// so it is looked at again only once a whole window (and its trip back) has passed.
	if (blamePermille > kBlameAddPermille && nowNs - m_lastBlame >= kBlameWindowNs + 1000000000LL && target() < m_cap) {
		++m_blameExtra;
		m_lastBlame = nowNs;
		if (target() > m_d) {
			m_d = target();
			return Stall;
		}
	}
	if (target() > m_d) {
		m_d         = target();
		m_lastBlame = nowNs;
		why         = Path;
	}
	return why;
}

DelayPolicy::Change DelayPolicy::boundary()
{
	if (m_blameExtra > 0 && m_lowBlame) --m_blameExtra;
	if (target() < m_d) {
		m_d = target();
		return Boundary;
	}
	return None;
}

} // namespace mppm::session
