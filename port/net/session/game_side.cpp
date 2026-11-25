// M5.3b game side of a link (see game_side.hpp).
#include "game_side.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace mppm::session {

namespace {
std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, f);
	std::vsnprintf(buf, sizeof(buf), f, ap);
	va_end(ap);
	return buf;
}
constexpr uint64_t kStallWindow = 900;
} // namespace

GameSide::GameSide(GameLink& link, GameHooks& hooks) : m_link(link), m_hooks(hooks) { }

void GameSide::push(const G2N& g)
{
	// The ring holds 4096 messages; the net thread drains it every millisecond. A backlog only
	// forms if that thread stalls, and keeps the order.
	while (!m_backlog.empty() && m_link.g2n.push(m_backlog.front())) m_backlog.erase(m_backlog.begin());
	if (!m_backlog.empty() || !m_link.g2n.push(g)) m_backlog.push_back(g);
}

void GameSide::poll()
{
	N2G m;
	while (m_link.n2g.pop(m)) {
		switch (m.kind) {
		case N2G::Bundle: {
			B b;
			b.mask = m.mask;
			std::memcpy(b.seat, m.seat, sizeof(b.seat));
			m_bundles[m.tick] = b;
			break;
		}
		case N2G::Release:
			m_releases[(uint16_t)m.count] = { m.tick, m.ns };
			while (m_releases.size() > 64) m_releases.erase(m_releases.begin());
			break;
		case N2G::Start:
			m_started = true;
			break;
		case N2G::End:
			m_ended = true;
			break;
		case N2G::Drop:
			m_drops[m.mask] = m.tick;
			break;
		}
	}
}

bool GameSide::ready(uint32_t tick)
{
	poll();
	if (!m_bundles.count(tick)) return false;
	if (m_pending && m_pendingTick < tick) {
		auto it = m_releases.lower_bound(m_pendingCount);
		if (it == m_releases.end()) return false;
		// The release of this barrier (or of a later one, which covers it).
		const int64_t now     = m_hooks.now();
		const int64_t g       = it->second.second;
		const int64_t before  = m_hooks.next_retrace_estimate();
		const uint32_t count  = m_hooks.grid_jump(g);
		m_hooks.align_tick(count);
		const int64_t at      = m_hooks.retrace_time(count);
		m_alignTick           = m_pendingTick + 1;
		m_alignNs             = at - m_hooks.margin_ns();
		const double waitedMs = (double)(now - m_pendingAt) / 1e6;
		m_hooks.log(fmt("barrier tick=%u waited_ms=%.1f jump_us=%lld", m_pendingTick, waitedMs, (long long)((at - before) / 1000)));
		m_st.barriers++;
		m_st.barrierMs += waitedMs;
		m_st.winBarrierMs += waitedMs;
		m_pending = false;
	}
	if (tick == m_alignTick && m_hooks.now() < m_alignNs) return false;
	return true;
}

bool GameSide::over(uint32_t tick)
{
	poll();
	if (m_ended) return true;
	bool soft;
	{
		std::lock_guard<std::mutex> lk(m_link.mu);
		soft = m_link.end.reason != kEndNone;
	}
	if (!soft) return false;
	poll(); // bundles pushed before the host was lost
	return !m_bundles.count(tick);
}

bool GameSide::take(uint32_t tick, Input out[kMaxSeats], uint8_t& mask)
{
	auto it = m_bundles.find(tick);
	if (it == m_bundles.end()) return false;
	mask = it->second.mask;
	std::memcpy(out, it->second.seat, sizeof(Input) * kMaxSeats);
	m_bundles.erase(it);
	if (tick % 256 == 0) {
		for (auto b = m_bundles.begin(); b != m_bundles.end();) {
			if (b->first < tick) b = m_bundles.erase(b);
			else ++b;
		}
	}
	return true;
}

void GameSide::begun()
{
	G2N g;
	g.kind = G2N::Begun;
	push(g);
}

void GameSide::input(uint32_t tick, const Input& in)
{
	G2N g;
	g.kind  = G2N::LocalInput;
	g.tick  = tick;
	g.input = in;
	push(g);
}

void GameSide::tick_due(uint32_t tick, int64_t dueNs, int64_t slewNs)
{
	G2N g;
	g.kind   = G2N::TickDue;
	g.tick   = tick;
	g.ns     = dueNs;
	g.slewNs = slewNs;
	push(g);
}

void GameSide::checksum(uint32_t tick, uint32_t total, const char* line)
{
	G2N g;
	g.kind  = G2N::Checksum;
	g.tick  = tick;
	g.total = total;
	if (line) std::snprintf(g.line, sizeof(g.line), "%s", line);
	push(g);
}

void GameSide::loaded(uint32_t tick)
{
	++m_loadCount;
	m_pending      = true;
	m_pendingTick  = tick;
	m_pendingCount = m_loadCount;
	m_pendingAt    = m_hooks.now();
	G2N g;
	g.kind  = G2N::Loaded;
	g.tick  = tick;
	g.count = m_loadCount;
	push(g);
}

void GameSide::leave(uint8_t byeReason)
{
	G2N g;
	g.kind  = G2N::Leave;
	g.count = byeReason;
	push(g);
}

void GameSide::done(uint32_t tick)
{
	G2N g;
	g.kind = G2N::Done;
	g.tick = tick;
	push(g);
}

void GameSide::note_read(uint32_t tick, int64_t waitedPastDueNs, bool barrier)
{
	if (m_preTick == tick) waitedPastDueNs = std::max(waitedPastDueNs, m_preNs);
	if (tick == m_alignTick) barrier = true; // the first tick after a release: its wait was the barrier's
	m_st.ticks++;
	m_st.winTicks++;
	if (!barrier && waitedPastDueNs > 0) {
		const double ms = (double)waitedPastDueNs / 1e6;
		m_st.stalled++;
		m_st.winStalled++;
		m_st.maxMs    = std::max(m_st.maxMs, ms);
		m_st.winMaxMs = std::max(m_st.winMaxMs, ms);
	}
	if (tick % kStallWindow == 0) {
		m_hooks.log(fmt("stall window_ticks=%llu stalled=%llu rate=%.2f max_ms=%.1f barrier_ms=%.1f", (unsigned long long)m_st.winTicks,
		                (unsigned long long)m_st.winStalled, m_st.winTicks ? 100.0 * (double)m_st.winStalled / (double)m_st.winTicks : 0.0,
		                m_st.winMaxMs, m_st.winBarrierMs));
		m_st.winTicks = m_st.winStalled = 0;
		m_st.winMaxMs = m_st.winBarrierMs = 0.0;
	}
}

void GameSide::wait_state(bool waiting, uint32_t tick, int64_t sinceNs, bool loading)
{
	m_waiting     = waiting;
	m_waitTick    = tick;
	m_waitSince   = sinceNs;
	m_waitLoading = loading;
	m_link.gameWait.store(waiting ? tick : 0, std::memory_order_relaxed);
}

bool GameSide::waiting(uint32_t& tick, int64_t& sinceNs, bool& loading) const
{
	tick    = m_waitTick;
	sinceNs = m_waitSince;
	loading = m_waitLoading;
	return m_waiting;
}

} // namespace mppm::session
