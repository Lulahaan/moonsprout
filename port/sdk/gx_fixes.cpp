// Fixes for Aurora GX entry points whose behaviour differs from the Dolphin SDK in ways the game
// depends on. The game calls these through macros in p2_sdk_compat.h.
#include "../../extern/aurora/lib/dolphin/gx/__gx.h"

namespace {

// Merge `color` into the packed RGBA register value the way the SDK does: GX_COLORn replaces RGB
// and keeps alpha, GX_ALPHAn replaces alpha and keeps RGB, GX_COLORnAn replaces everything.
GXColor merge(u32 reg, GXChannelID id, GXColor color)
{
	GXColor cur = { u8(reg >> 24), u8(reg >> 16), u8(reg >> 8), u8(reg) };
	switch (id) {
	case GX_COLOR0:
	case GX_COLOR1:
		color.a = cur.a;
		break;
	case GX_ALPHA0:
	case GX_ALPHA1:
		cur.a = color.a;
		color = cur;
		break;
	default:
		break;
	}
	return color;
}

int slot(GXChannelID id) { return (id == GX_COLOR1 || id == GX_ALPHA1 || id == GX_COLOR1A1) ? 1 : 0; }

} // namespace

extern "C" void p2_GXSetChanMatColor(GXChannelID id, GXColor color)
{
	int i = slot(id);
	GXSetChanMatColor(i ? GX_COLOR1A1 : GX_COLOR0A0, merge(__gx->matColor[i], id, color));
}

extern "C" void p2_GXSetChanAmbColor(GXChannelID id, GXColor color)
{
	int i = slot(id);
	GXSetChanAmbColor(i ? GX_COLOR1A1 : GX_COLOR0A0, merge(__gx->ambColor[i], id, color));
}
