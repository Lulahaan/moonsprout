// Online harness, M5.1a (docs/ONLINE_MULTIPLAYER.md 2.1, 4 item 8): the DVD wait. Host-only file,
// built into the game library (not in the matching build).
//
// The main thread outranks the DVD thread, and the host scheduler (port/sdk/os_thread.cpp) lets
// the DVD thread run at wall-clock dependent points. Without this wait a VS load would end on a
// different tick in each run, and cave generation (on the DVD thread) would interleave its rand()
// calls with the loading screen's. In a session, a command queued from the main thread is
// therefore finished before DvdThread::sendCommand returns.
#include "DvdThreadCommand.h"
#include "JSystem/JFramework/JFWSystem.h"
#include "JSystem/JKernel/JKRThread.h"
#include "Dolphin/os.h"
#include "p2_host.h"

#include <stdio.h>
#include <time.h>
#include <chrono>
#include <thread>

extern "C" void p2_host_block_begin(); // port/sdk/os_thread.cpp: other game threads may run between
extern "C" void p2_host_block_end();

namespace {

const long kWaitStepNs     = 50000; // 50 microseconds per sleep
const double kWarnAfterSec = 30.0;

double wall_seconds()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

extern "C" void p2_net_dvd_sync(void* p)
{
	if (!p2_net_session_active()) {
		return;
	}
	// Only the main game thread waits: a DVD callback that queues another command runs on the DVD
	// thread and must not wait for itself.
	if (JFWSystem::mainThread == nullptr || OSGetCurrentThread() != JFWSystem::mainThread->getThreadRecord()) {
		return;
	}

	DvdThreadCommand* cmd = (DvdThreadCommand*)p;
	// Wait for the result and for DvdThread::run to take the command off its list, so the DVD
	// thread is past its bookkeeping when the main thread goes on.
	volatile int* mode  = (volatile int*)&cmd->mMode;
	double start        = 0.0;
	static bool sWarned = false;
	// M5.1b: the DVD thread's rand() calls (cave generation) draw from the sim stream while the
	// main thread is blocked here, so they stay serialised with the simulation.
	p2_rng_handoff(1);
	while (*mode != DvdThreadCommand::CM_Completed || *(JSUPtrList* volatile*)&cmd->mLink.mList != nullptr) {
		if (start == 0.0) {
			start = wall_seconds();
		} else if (!sWarned && wall_seconds() - start > kWarnAfterSec) {
			sWarned = true;
			fprintf(stderr, "mppm: net: DVD wait over 30 s (deadlock?)\n");
			fflush(stderr);
		}
		p2_host_block_begin();
		std::this_thread::sleep_for(std::chrono::nanoseconds(kWaitStepNs));
		p2_host_block_end();
	}
	p2_rng_handoff(0);
}
