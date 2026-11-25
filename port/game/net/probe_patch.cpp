// Latency probe, game side (docs/ONLINE_MULTIPLAYER.md 2.10, M5.1a). Built with the game so it can
// read the pad state and draw with J2D; the host side is port/net/probe.cpp.
#include "JSystem/J2D/J2DGrafContext.h"
#include "JSystem/JUtility/JUTGamePad.h"
#include "JSystem/JUtility/TColor.h"
#include "p2_host.h"

// Port 0's button word as this pass's JUTGamePad::read left it, with the main stick past half
// deflection (36 of 72 after the clamp) as bits 16 (right), 17 (left), 18 (up) and 19 (down),
// and the C-stick as bits 20 to 23. M5.3b: in an online session, this machine's seat (its input
// reaches the game in that port's slot of the bundle, d ticks later).
extern "C" unsigned p2_net_probe_pad_buttons(void)
{
	const int slot     = p2_net_local_slot();
	const PADStatus& s = JUTGamePad::mPadStatus[slot >= 0 && slot < 4 ? slot : 0];
	unsigned bits      = s.button;
	const int kHalf    = 36;
	bits |= s.stickX >= kHalf ? 1u << 16 : 0u;
	bits |= s.stickX <= -kHalf ? 1u << 17 : 0u;
	bits |= s.stickY >= kHalf ? 1u << 18 : 0u;
	bits |= s.stickY <= -kHalf ? 1u << 19 : 0u;
	// M5.1b: the C-stick past half deflection (30 of 59) as bits 20 (right), 21 (left), 22 (up)
	// and 23 (down), so a camera turn counts as a press.
	const int kHalfC = 30;
	bits |= s.substickX >= kHalfC ? 1u << 20 : 0u;
	bits |= s.substickX <= -kHalfC ? 1u << 21 : 0u;
	bits |= s.substickY >= kHalfC ? 1u << 22 : 0u;
	bits |= s.substickY <= -kHalfC ? 1u << 23 : 0u;
	return bits;
}

// A white 64 by 64 square at the top left of the game's 640 by 480 space, drawn over the finished
// game frame before Aurora ends it (GXCopyDisp is empty in Aurora, so it is in the presented
// picture). As ResetManager::draw.
extern "C" void p2_net_probe_draw_patch(void)
{
	J2DOrthoGraph graf(0.0f, 0.0f, 640.0f, 480.0f, -1.0f, 1.0f);
	graf.setPort();
	J2DFillBox(0.0f, 0.0f, 64.0f, 64.0f, JUtility::TColor(255, 255, 255, 255));
}
