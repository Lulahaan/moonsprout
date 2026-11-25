// Video interface pieces Aurora leaves to the game. The retrace interrupt, VIWaitForRetrace and
// the retrace callbacks are in os_thread.cpp (they are scheduler events); frame presentation is
// p2_host_present() in port/main.cpp.
#include <dolphin/vi.h>

namespace {
void* s_nextFrameBuffer    = nullptr;
void* s_currentFrameBuffer = nullptr;
} // namespace

extern "C" {

void VISetNextFrameBuffer(void* fb)
{
	s_nextFrameBuffer    = fb;
	s_currentFrameBuffer = fb;
}
void* VIGetCurrentFrameBuffer(void) { return s_currentFrameBuffer; }
void VISetBlack(BOOL) { }
u32 VIGetNextField(void) { return 0; }
u32 VIGetDTVStatus(void) { return 0; } // no component cable: progressive prompt stays off

} // extern "C"
