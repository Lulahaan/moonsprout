// Dolphin SDK GX functions the game uses that Aurora does not implement.
#include "types.h"
#include "stl/math.h"

extern "C" {

// Draw-sync tokens let the CPU wait for the GPU to reach a point in the FIFO. Aurora submits
// frames as a whole, so the token is simply "reached" immediately.
static u16 s_drawSyncToken;
void GXSetDrawSync(u16 token) { s_drawSyncToken = token; }
u16 p2_GXGetDrawSyncToken(void) { return s_drawSyncToken; }
void GXWaitDrawDone(void) { }

// Indirect texture mask: only affects how the hardware treats indirect stages; Aurora derives
// that from the TEV setup.
void __GXSetIndirectMask(u32) { }

// From the decomp's src/Dolphin/gx/GXPixel.c (CC0).
void GXInitFogAdjTable(GXFogAdjTable* table, u16 width, const Mtx44 proj)
{
	f32 x, nearZ, scale, sideX, dist;
	u32 i;

	if (proj[3][3] == 0.0) {
		nearZ = proj[2][3] / (proj[2][2] - 1.0f);
		sideX = nearZ / proj[0][0];
	} else {
		sideX = 1.0f / proj[0][0];
		nearZ = M_SQRT3 * sideX;
	}

	scale = 2.0f / width;

	for (i = 0; i < ARRAY_SIZE(table->r); i++) {
		x = (i + 1) * 32;
		x *= scale;
		x *= sideX;
		dist        = dolsqrtf(1.0f + (x * x) / (nearZ * nearZ));
		table->r[i] = (u32)(dist * 256.0f) & 0xFFF;
	}
}

} // extern "C"
