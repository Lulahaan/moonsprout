// Bounded single-producer single-consumer ring (M5.3b: game thread <-> net thread). Lock-free; one
// thread pushes, one thread pops. Capacity is a power of two.
#pragma once

#include <atomic>
#include <cstddef>

namespace mppm::session {

template <typename T, size_t N>
class Spsc {
	static_assert((N & (N - 1)) == 0, "capacity must be a power of two");

public:
	bool push(const T& v)
	{
		const size_t h = m_head.load(std::memory_order_relaxed);
		const size_t t = m_tail.load(std::memory_order_acquire);
		if (h - t >= N) return false;
		m_buf[h & (N - 1)] = v;
		m_head.store(h + 1, std::memory_order_release);
		return true;
	}
	bool pop(T& out)
	{
		const size_t t = m_tail.load(std::memory_order_relaxed);
		const size_t h = m_head.load(std::memory_order_acquire);
		if (t == h) return false;
		out = m_buf[t & (N - 1)];
		m_tail.store(t + 1, std::memory_order_release);
		return true;
	}
	bool empty() const { return m_tail.load(std::memory_order_acquire) == m_head.load(std::memory_order_acquire); }
	void clear()
	{
		T tmp;
		while (pop(tmp)) { }
	}

private:
	T m_buf[N];
	alignas(64) std::atomic<size_t> m_head{ 0 };
	alignas(64) std::atomic<size_t> m_tail{ 0 };
};

} // namespace mppm::session
