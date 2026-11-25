// Ground detail for the start-menu scene (docs/launcher/SCENE.md 10.4). See SceneGround.h.
#include "SceneGround.h"
#include "SceneStage.h"

#include "Viewport.h"
#include "JSystem/J3D/J3DShape.h"
#include "Dolphin/gx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace LauncherScene {
namespace {

const int kTexSize   = 128; // texels; the grain tiles every kTileUnits of ground
const int kMipLevels = 5;   // 128, 64, 32, 16, 8
const f32 kTileUnits = 14.0f;

const int kGridCells = 40;      // per side
const f32 kGridHalf  = 370.0f;  // units around the eye's foot
const f32 kFullDist  = 100.0f;  // full strength up to here from the eye...
const f32 kZeroDist  = 360.0f;  // ...none from here
const f32 kLift      = 0.12f;   // above the landing site's height

// Mip chain of the I8 grain, GameCube tiled (8 x 4 texel blocks), 32-byte aligned.
u8 sTexData[(kTexSize * kTexSize * 4) / 3 + 64] ATTRIBUTE_ALIGN(32);
GXTexObj sTexObj;
bool sTexMade = false;

u32 hash(u32 x, u32 y, u32 seed)
{
	u32 h = x * 374761393u + y * 668265263u + seed * 2246822519u;
	h     = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

f32 rnd(u32 x, u32 y, u32 seed) { return (f32)(hash(x, y, seed) & 0xFFFF) / 65535.0f; }

// Value noise that tiles over `period` cells.
f32 valueNoise(f32 x, f32 y, int period, u32 seed)
{
	int x0 = (int)floorf(x), y0 = (int)floorf(y);
	f32 fx = x - (f32)x0, fy = y - (f32)y0;
	fx     = fx * fx * (3.0f - 2.0f * fx);
	fy     = fy * fy * (3.0f - 2.0f * fy);
	u32 ax = (u32)(((x0 % period) + period) % period), bx = (u32)((((x0 + 1) % period) + period) % period);
	u32 ay = (u32)(((y0 % period) + period) % period), by = (u32)((((y0 + 1) % period) + period) % period);
	f32 a = rnd(ax, ay, seed), b = rnd(bx, ay, seed), c = rnd(ax, by, seed), d = rnd(bx, by, seed);
	return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

void storeI8(u8* level, int width, int x, int y, u8 value)
{
	int tilesPerRow = width / 8;
	int tile        = (y / 4) * tilesPerRow + (x / 8);
	level[tile * 32 + (y % 4) * 8 + (x % 8)] = value;
}

// Soil grain with a few blades: several octaves of tiling value noise plus fine speckle, mean
// mid grey (so the blend leaves the average colour alone), about +-20% contrast. Each mip is a box
// filter of the one above, so the grain fades to flat grey in the distance instead of shimmering.
void makeTexture()
{
	static f32 level0[kTexSize * kTexSize];
	f32 sum = 0.0f;
	for (int y = 0; y < kTexSize; y++) {
		for (int x = 0; x < kTexSize; x++) {
			f32 u = (f32)x, v = (f32)y;
			f32 n = 0.40f * valueNoise(u / 16.0f, v / 16.0f, kTexSize / 16, 11) + 0.28f * valueNoise(u / 8.0f, v / 8.0f, kTexSize / 8, 23)
			      + 0.20f * valueNoise(u / 4.0f, v / 4.0f, kTexSize / 4, 37) + 0.16f * valueNoise(u / 2.0f, v / 2.0f, kTexSize / 2, 41)
			      + 0.12f * rnd((u32)x, (u32)y, 53);
			level0[y * kTexSize + x] = n;
			sum += n;
		}
	}
	// Strokes: short thin segments, most darker (twigs, cracks, the shade between blades), some
	// lighter (lit blade edges).
	for (int i = 0; i < 320; i++) {
		f32 cx  = rnd(i, 1, 71) * kTexSize, cy = rnd(i, 2, 71) * kTexSize;
		f32 ang = rnd(i, 3, 71) * 3.14159265f;
		f32 len = 2.5f + rnd(i, 4, 71) * 6.0f;
		f32 dk  = (i % 3 == 2) ? -(0.05f + rnd(i, 5, 71) * 0.07f) : (0.05f + rnd(i, 5, 71) * 0.10f);
		f32 dx = cosf(ang), dy = sinf(ang);
		for (f32 t = -len * 0.5f; t <= len * 0.5f; t += 0.5f) {
			int px = ((int)floorf(cx + dx * t) % kTexSize + kTexSize) % kTexSize;
			int py = ((int)floorf(cy + dy * t) % kTexSize + kTexSize) % kTexSize;
			level0[py * kTexSize + px] -= dk;
			sum -= dk;
		}
	}
	f32 mean = sum / (f32)(kTexSize * kTexSize);
	f32 var  = 0.0f;
	for (int i = 0; i < kTexSize * kTexSize; i++) {
		f32 d = level0[i] - mean;
		var += d * d;
	}
	f32 sd    = sqrtf(var / (f32)(kTexSize * kTexSize));
	f32 scale = sd > 0.0f ? 0.10f / sd : 0.0f; // one standard deviation = 10% of full scale (13% buried the contact shadows)

	static f32 work[kTexSize * kTexSize];
	for (int i = 0; i < kTexSize * kTexSize; i++) {
		work[i] = 0.5f + (level0[i] - mean) * scale;
	}
	u8* out   = sTexData;
	int width = kTexSize;
	for (int lv = 0; lv < kMipLevels; lv++) {
		for (int y = 0; y < width; y++) {
			for (int x = 0; x < width; x++) {
				f32 c = work[y * width + x];
				c     = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
				storeI8(out, width, x, y, (u8)(c * 255.0f + 0.5f));
			}
		}
		out += width * width;
		int half = width / 2;
		for (int y = 0; y < half; y++) {
			for (int x = 0; x < half; x++) {
				work[y * half + x] = 0.25f
				                   * (work[(2 * y) * width + 2 * x] + work[(2 * y) * width + 2 * x + 1] + work[(2 * y + 1) * width + 2 * x]
				                      + work[(2 * y + 1) * width + 2 * x + 1]);
			}
		}
		width = half;
	}
	DCFlushRange(sTexData, sizeof(sTexData));
	GXInitTexObj(&sTexObj, sTexData, kTexSize, kTexSize, GX_TF_I8, GX_REPEAT, GX_REPEAT, GX_TRUE);
	// Anisotropic where Texture Filtering allows it; the negative bias keeps the grain crisp at the
	// grazing angles of the far ground when it does not (the fall-off hides the shimmer it costs).
	GXInitTexObjLOD(&sTexObj, GX_LIN_MIP_LIN, GX_LINEAR, 0.0f, (f32)(kMipLevels - 1), -1.0f, GX_FALSE, GX_FALSE, GX_ANISO_4);
	sTexMade = true;
}

} // namespace

void GroundDetail::draw(Viewport* vp, const StageFrame& frame, const Vector3f& eye, f32 strength)
{
	if (strength <= 0.0f) {
		return;
	}
	if (!sTexMade) {
		makeTexture();
	}
	const f32 groundY = frame.mOrigin.y + kLift;
	const f32 height  = eye.y - groundY;
	if (height > kZeroDist) {
		return; // the aerial shots: nothing near enough
	}

	Mtx& view = vp->getMatrix(false)->mMatrix.mtxView;
	GXSetNumChans(1);
	GXSetNumTexGens(1);
	GXSetNumTevStages(1);
	GXSetNumIndStages(0);
	GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GXLoadTexObj(&sTexObj, GX_TEXMAP0);
	GXSetTevDirect(GX_TEVSTAGE0);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	// Colour = lerp(mid grey, grain, vertex alpha): grey where the strength has fallen to nothing.
	GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_HALF, GX_CC_TEXC, GX_CC_RASA, GX_CC_ZERO);
	GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
	// 2 x source x destination: the grain darkens and brightens the drawn ground around its colour.
	GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTCLR, GX_BL_SRCCLR, GX_LO_SET);
	GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
	GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
	GXSetCullMode(GX_CULL_NONE);
	GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, (GXColor) { 0, 0, 0, 0 });
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GXLoadPosMtxImm(view, GX_PNMTX0);
	GXSetCurrentMtx(GX_PNMTX0);

	// A square of ground around the eye's foot, snapped to whole cells so the grid's vertices (and
	// with them the fall-off) do not slide as the camera drifts. Texture coordinates come from the
	// world position, so the grain itself never moves.
	const f32 cell = 2.0f * kGridHalf / (f32)kGridCells;
	const f32 x0   = floorf((eye.x - kGridHalf) / cell) * cell;
	const f32 z0   = floorf((eye.z - kGridHalf) / cell) * cell;
	const f32 inv  = 1.0f / (kZeroDist - kFullDist);
	for (int r = 0; r < kGridCells; r++) {
		GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, (kGridCells + 1) * 2);
		for (int c = 0; c <= kGridCells; c++) {
			for (int k = 0; k < 2; k++) {
				f32 x  = x0 + cell * (f32)c;
				f32 z  = z0 + cell * (f32)(r + k);
				f32 dx = x - eye.x, dz = z - eye.z;
				f32 d  = sqrtf(dx * dx + dz * dz + height * height);
				f32 t  = 1.0f - (d - kFullDist) * inv;
				t      = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
				t      = t * t * (3.0f - 2.0f * t) * strength;
				GXPosition3f32(x, groundY, z);
				GXColor4u8(255, 255, 255, (u8)(t * 255.0f));
				GXTexCoord2f32(x / kTileUnits, z / kTileUnits);
			}
		}
		GXEnd();
	}
	J3DShape::resetVcdVatCache();
}

} // namespace LauncherScene
