// M5.3b input delay (D12, D13; docs/online/M5_3B_LOCKSTEP.md "Input delay"). Pure.
#pragma once

#include <cstdint>

namespace mppm::session {

// d = clamp(ceil((pathUs + 2 ms + 1 ms) / P), 1, 8). `wish` gets the uncapped value.
int delay_for_path(int64_t pathUs, int* wish = nullptr);

// One sender's delay. Recomputed every second from the latest path; an increase applies at once, a
// decrease waits for the next barrier release; a blame rate above 2% over 3 s adds 1.
class DelayPolicy {
public:
	enum Change { None, Init, Path, Stall, Boundary };
	void init(int d, int64_t nowNs);
	int current() const { return m_d; }
	int wish() const { return m_wish; }
	bool capped() const { return m_wish > m_cap; }
	// Once per second (the caller checks): pathUs < 0 = unknown (keeps the value).
	Change evaluate(int64_t nowNs, int64_t pathUs, int blamePermille);
	// At a barrier release: a lower target applies now.
	Change boundary();
	void set_cap(int cap) { m_cap = cap; }

private:
	int target() const;
	int m_d          = 2;
	int m_pathD      = 2;
	int m_wish       = 2;
	int m_blameExtra = 0;
	int m_cap        = 8;
	int64_t m_lastBlame = 0;
	bool m_lowBlame  = true;
};

} // namespace mppm::session
