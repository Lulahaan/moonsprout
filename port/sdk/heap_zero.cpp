// Zero on alloc for the game heaps (port/include/p2_host.h, docs/online/M5_1B_BUILD.md).
// JKRHeap::alloc (src/JSystem/JKernel/JKRHeap.cpp) returns every block through p2_heap_zero, so
// game code that reads a field it never wrote sees 0 instead of what the block held before. On the
// GameCube such reads gave whatever the previous owner left; here they would differ between
// machines, runs and replays.
#include <cstring>

#ifndef MPPM_ZERO_INIT
#define MPPM_ZERO_INIT 1
#endif

namespace {
thread_local unsigned long t_limit = ~0UL; // p2_heap_zero_limit, for the next block only
}

extern "C" void p2_heap_zero_limit(unsigned long bytes) { t_limit = bytes; }

extern "C" void* p2_heap_zero(void* memory, unsigned long size)
{
	const unsigned long limit = t_limit;
	t_limit                   = ~0UL;
#if MPPM_ZERO_INIT
	if (size > limit) {
		size = limit; // a new heap's block: only its header (see p2_host.h)
	}
	if (memory != nullptr && size != 0) {
		std::memset(memory, 0, size);
	}
#else
	(void)size;
	(void)limit;
#endif
	return memory;
}
