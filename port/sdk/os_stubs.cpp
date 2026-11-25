// Dolphin OS system queries Aurora leaves to the game.
//
// Threads, interrupts, mutexes, condition variables, message queues, alarms and the retrace live
// in os_thread.cpp; this file has the remaining system queries.
#include <dolphin/os.h>

#include <cstring>

#include "p2_host.h"

extern "C" {

// ---- system settings / misc
static u32 s_progressive = 0, s_soundMode = 1 /* stereo */;
u32 OSGetProgressiveMode(void) { return s_progressive; }
void OSSetProgressiveMode(u32 on) { s_progressive = on; }
u32 OSGetSoundMode(void) { return s_soundMode; }
void OSSetSoundMode(u32 mode) { s_soundMode = mode; }
void OSSetEuRgb60Mode(u32) { }
BOOL OSGetResetSwitchState(void) { return FALSE; }
// The game's soft reset (ResetManager, src/sysGCU/reset.cpp) ends here once the sound has stopped
// and the card is idle. The GameCube reboots the disc; the port restarts the process straight into
// the game (or into the start menu for the "back to the system menu" reset), see p2_host.h.
void OSResetSystem(int, u32, BOOL forceMenu) { p2_host_reset_system(forceMenu ? 1 : 0); }
void OSSetSaveRegion(void*, void*) { }
void OSProtectRange(u32, void*, u32, u32) { }
OSErrorHandler OSSetErrorHandler(OSError, OSErrorHandler handler) { return handler; }
u32 __OSFpscrEnableBits = 0;

// ---- ROM font (TODO(M2): the game reads the IPL font for some text; load or bundle one)
u16 OSGetFontEncode(void) { return 0; /* OS_FONT_ENCODE_ANSI */ }
BOOL OSInitFont(OSFontHeader*) { return FALSE; }
char* OSGetFontTexture(const char* string, void** image, s32* x, s32* y, s32* width)
{
	if (image) *image = nullptr;
	if (x) *x = 0;
	if (y) *y = 0;
	if (width) *width = 0;
	return (char*)(string && *string ? string + 1 : string);
}
char* OSGetFontWidth(const char* string, s32* width)
{
	if (width) *width = 0;
	return (char*)(string && *string ? string + 1 : string);
}

// ---- PowerPC machine-state registers
u32 PPCMfmsr() { return 0; }
void PPCMtmsr(u32) { }
void PPCSync() { }

} // extern "C"

// ---- GameCube low-memory state the game reads or writes at fixed addresses
#include "System.h"
RenderModeInfo gRenderInfoStore; // DOL_ADDR_LIMIT on the GameCube
