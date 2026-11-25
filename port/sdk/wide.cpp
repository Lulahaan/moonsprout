// Widescreen state (port/include/p2_wide.h, docs/launcher/ASPECT.md 3 and 5).
//
// One number drives everything: k = contentAspect / (608 / 448), sampled once per frame from the
// size of Aurora's content framebuffer. The game multiplies every 3D camera aspect by k (Hor+) and
// remaps every 2D viewport about X = 304 by 1/k, so 2D keeps its shape in the middle 19:14 area.
// k = 1 makes every function here the identity, which is the original picture.
//
// This file never reads settings: mppm::video::apply() (port/launcher/video.cpp) decides the
// Aurora state and pushes the few values needed here with p2_wide_configure.
#include "p2_wide.h"

#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr float kWidth   = 608.0f;          // W0: logical EFB width (src/sysGCU/system.cpp)
constexpr float kHeight  = 448.0f;          // H0: logical EFB height
constexpr float kAspect  = kWidth / kHeight; // A0 = 19:14
constexpr float kCenterX = kWidth * 0.5f;   // CX
constexpr float kMaxK    = 3.6f;            // about 4.9:1; video.cpp locks wider windows to this

float s_k             = 1.0f; // sampled by p2_wide_update
int s_stretch         = 0;    // aspect mode Stretch: k forced to 1
int s_hudLayout       = 0;    // 0 centered, 1 edges, 2 stretched
float s_hudMaxAspect  = 0.0f; // edge anchoring limit (phase 2), 0 = unlimited
int s_rawDepth        = 0;    // p2_wide_raw_begin nesting
int s_sceneOriginal   = 0;    // p2_wide_scene_original nesting
int s_sceneApplied    = 0;    // scene-original state the host last saw (0 / 1)
void (*s_changeHook)(void) = nullptr;

// Wide canvas scope (p2_wide_canvas_begin): nesting depth, whether the outermost begin converted
// the GX state (and what to restore), and the X correction for projections loaded inside it.
int s_canvasDepth      = 0;
bool s_canvasConverted = false;
f32 s_canvasSavedViewport[6];
f32 s_canvasSavedProjection[7];
float s_canvasA = 1.0f; // NDC x' = a * x + b (clip space: x' = a * x + b * w)
float s_canvasB = 0.0f;

// Wide fill scope (p2_wide_fill_begin): the viewport to restore.
int s_fillDepth = 0;
f32 s_fillSavedViewport[6];

// Wide backdrop scope (p2_wide_backdrop_begin): a full-width picture drawn in a wide canvas.
int s_backdropDepth = 0;
constexpr int kMaxBackdropTexWidth = 64; // texels across; wider textures are pictures, not fills

// HUD anchors by .blo name (ASPECT 7.5). Left: both captains' life gauges and the sprays; right:
// the Pikmin counters (gr_pikmin.blo also holds the Day bubble) and the treasure gauge. The sun
// meter spreads (2): its track keeps its centre while its ends move out by the same amount as the
// left and right screens, so it still runs from the captains' side to the Day bubble
// (og::Screen::CallBack_SunMeter). 2P_challenge_poko.blo is a spread screen too: its Poko and
// dead-Pikmin counters stay in the middle while Challenge Mode 1P moves its TIME bubble (Nmenu01)
// out by the right-hand shift (og::newScreen::ObjChallenge1P::commonUpdate); in Challenge 2P
// nothing moves. The story-mode Poko counter and the two-player screens (which hold both sides
// of a split-screen HUD) stay centred, as do names not listed here.
struct Anchor {
	const char* name;
	int anchor;
};
constexpr Anchor kAnchors[] = {
	{ "orima.blo", -1 },       { "doping.blo", -1 }, { "gr_pikmin.blo", 1 },
	{ "cave_pikmin.blo", 1 },  { "sensor.blo", 1 },  { "sun_meter.blo", 2 },
	{ "grand_cave_poko.blo", 0 }, { "2P_challenge_poko.blo", 2 },
};

// MPPM_HUD_ANCHORS=name:L,name:R,... (test-only, SETTINGS 1.3): -2 when the name is not listed.
int env_anchor(const char* name)
{
	const char* env = std::getenv("MPPM_HUD_ANCHORS");
	if (env == nullptr || name == nullptr) {
		return -2;
	}
	const std::string list(env);
	const size_t len = std::strlen(name);
	size_t pos       = 0;
	while (pos < list.size()) {
		size_t end = list.find(',', pos);
		if (end == std::string::npos) {
			end = list.size();
		}
		const std::string item = list.substr(pos, end - pos);
		if (item.size() == len + 2 && item.compare(0, len, name) == 0 && item[len] == ':') {
			switch (item[len + 1]) {
			case 'L':
			case 'l':
				return -1;
			case 'R':
			case 'r':
				return 1;
			case 'S':
			case 's':
				return 2;
			default:
				return 0;
			}
		}
		pos = end + 1;
	}
	return -2;
}

// The visible width of the HUD in units of the 608-wide frame: k, capped by interface.hudMaxAspect.
float hud_k()
{
	float k = p2_wide_2d_k();
	if (s_hudMaxAspect > 0.0f) {
		k = std::min(k, s_hudMaxAspect / kAspect);
	}
	return std::max(k, 1.0f);
}

} // namespace

extern "C" {

void p2_wide_update(void)
{
	// A scene asked for (or released) Original since the last frame: let the host re-lock the
	// content shape first, so the size sampled below already has it.
	const int scene = s_sceneOriginal > 0 ? 1 : 0;
	if (scene != s_sceneApplied) {
		s_sceneApplied = scene;
		if (s_changeHook != nullptr) {
			s_changeHook();
		}
	}

	u32 width  = 0;
	u32 height = 0;
	AuroraGetRenderSize(&width, &height);
	float k = 1.0f;
	if (!s_stretch && !scene && width > 0 && height > 0) {
		k = (static_cast<float>(width) / static_cast<float>(height)) / kAspect;
		// Within 0.2% of 19:14 is 19:14: integer framebuffer sizes never give exactly 1, and a
		// k of 1.0007 would nudge every 2D viewport by a fraction of a pixel.
		if (k < 1.002f) {
			k = 1.0f;
		}
		k = std::min(k, kMaxK);
	}
	s_k = k;
}

void p2_wide_configure(int stretch, int hudLayout, float hudMaxAspect)
{
	s_stretch      = stretch;
	s_hudLayout    = hudLayout;
	s_hudMaxAspect = hudMaxAspect;
}

void p2_wide_set_change_hook(void (*hook)(void))
{
	s_changeHook = hook;
}

float p2_wide_k(void)
{
	return s_k;
}

float p2_wide_2d_k(void)
{
	if (s_rawDepth > 0 || s_hudLayout == 2) {
		return 1.0f;
	}
	return s_k;
}

float p2_wide_map_x(float x)
{
	const float k = p2_wide_2d_k();
	if (k == 1.0f) {
		return x; // exactly the original value, not a rounded round trip through 304
	}
	return kCenterX + (x - kCenterX) / k;
}

float p2_wide_min_x(void)
{
	return kCenterX * (1.0f - p2_wide_2d_k());
}

float p2_wide_max_x(void)
{
	return kCenterX * (1.0f + p2_wide_2d_k());
}

void p2_wide_scissor(unsigned int x, unsigned int y, unsigned int width, unsigned int height)
{
	if (p2_wide_2d_k() != 1.0f) {
		// Edges at the frame's border stay there, so a full-width scissor keeps covering the sides.
		const float x0  = static_cast<float>(x);
		const float x1  = x0 + static_cast<float>(width);
		const float m0  = x0 <= 0.0f ? 0.0f : p2_wide_map_x(x0);
		const float m1  = x1 >= kWidth ? x1 : p2_wide_map_x(x1);
		const u32 left  = static_cast<u32>(std::floor(std::max(m0, 0.0f)));
		const u32 right = std::max(static_cast<u32>(std::ceil(m1)), left);
		x               = left;
		width           = right - left;
	}
	GXSetScissor(x, y, width, height);
}

void p2_wide_raw_begin(void)
{
	s_rawDepth++;
}

void p2_wide_raw_end(void)
{
	if (s_rawDepth > 0) {
		s_rawDepth--;
	}
}

void p2_wide_scene_original(int on)
{
	if (on) {
		s_sceneOriginal++;
	} else if (s_sceneOriginal > 0) {
		s_sceneOriginal--;
	}
}

int p2_wide_scene_original_active(void)
{
	return s_sceneOriginal > 0;
}

float p2_wide_hud_shift(int anchor)
{
	// ASPECT 7.4: a left or right screen moves out by the extra width on its side, so it sits as far
	// from the window edge as it does from the 4:3 edge at Original. Pane offsets are applied after
	// the 0.95 tuning scale (J2DPane::makeMatrix), so the shift is in frame units as is.
	if (s_hudLayout != 1 || anchor == 0 || anchor > 1) {
		return 0.0f; // centred, or spread (the screen stays; its panes move, p2_wide_hud_spread)
	}
	const float k = hud_k();
	if (k == 1.0f) {
		return 0.0f;
	}
	return static_cast<float>(anchor < 0 ? -1 : 1) * kCenterX * (k - 1.0f);
}

float p2_wide_hud_spread(void)
{
	return p2_wide_hud_shift(1);
}

int p2_wide_anchor_for_blo(const char* name)
{
	const int overridden = env_anchor(name);
	if (overridden != -2) {
		return overridden;
	}
	if (name != nullptr) {
		for (const Anchor& a : kAnchors) {
			if (std::strcmp(a.name, name) == 0) {
				return a.anchor;
			}
		}
	}
	return 0;
}

void p2_wide_canvas_begin(void)
{
	if (s_canvasDepth++ > 0) {
		return;
	}
	s_canvasConverted = false;
	s_canvasA         = 1.0f;
	s_canvasB         = 0.0f;
	const float k     = p2_wide_2d_k();
	if (k == 1.0f) {
		return;
	}
	// The current viewport is a remapped (narrowed) 2D port: centre c, width w. The same picture
	// comes from the port's unmapped viewport (centre c', width w * k) with NDC x squeezed by 1/k
	// and moved by b. Anything that is not such a port is left alone.
	GXGetViewportv(s_canvasSavedViewport);
	const f32* vp   = s_canvasSavedViewport;
	const float c   = vp[0] + vp[2] * 0.5f;
	const float c2  = kCenterX + (c - kCenterX) * k;
	const float w2  = vp[2] * k;
	const float x0  = c2 - w2 * 0.5f;
	if (vp[2] <= 0.0f || x0 < -0.5f || x0 + w2 > kWidth + 0.5f) {
		return;
	}
	GXGetProjectionv(s_canvasSavedProjection);
	s_canvasA = 1.0f / k;
	s_canvasB = 2.0f * (c - c2) / w2;
	f32 p[7];
	std::memcpy(p, s_canvasSavedProjection, sizeof(p));
	// Row 0 becomes a * row0 + b * row3; row 3 is (0, 0, 0, 1) for ortho, (0, 0, -1, 0) for perspective.
	p[1] *= s_canvasA;
	p[2] = p[2] * s_canvasA + (p[0] != 0.0f ? s_canvasB : -s_canvasB);
	GXSetProjectionv(p);
	GXSetViewport(x0, vp[1], w2, vp[3], vp[4], vp[5]);
	s_canvasConverted = true;
}

void p2_wide_canvas_end(void)
{
	if (s_canvasDepth == 0 || --s_canvasDepth > 0) {
		return;
	}
	if (s_canvasConverted) {
		const f32* vp = s_canvasSavedViewport;
		GXSetProjectionv(s_canvasSavedProjection);
		GXSetViewport(vp[0], vp[1], vp[2], vp[3], vp[4], vp[5]);
	}
	s_canvasConverted = false;
	s_canvasA         = 1.0f;
	s_canvasB         = 0.0f;
}

int p2_wide_canvas_active(void)
{
	return s_canvasDepth > 0 && p2_wide_2d_k() != 1.0f;
}

void p2_wide_canvas_port(float x0, float x1)
{
	const float k = p2_wide_2d_k();
	const float w = x1 - x0;
	if (k == 1.0f || w <= 0.0f) {
		s_canvasA = 1.0f;
		s_canvasB = 0.0f;
		return;
	}
	const float c2 = (x0 + x1) * 0.5f;
	s_canvasA      = 1.0f / k;
	s_canvasB      = 2.0f * (p2_wide_map_x(c2) - c2) / w;
}

void p2_wide_set_projection(const void* mtx44, int type)
{
	if (!p2_wide_canvas_active() || (s_canvasA == 1.0f && s_canvasB == 0.0f)) {
		GXSetProjection(mtx44, static_cast<GXProjectionType>(type));
		return;
	}
	f32 m[4][4];
	std::memcpy(m, mtx44, sizeof(m));
	for (int j = 0; j < 4; j++) {
		m[0][j] = m[0][j] * s_canvasA + m[3][j] * s_canvasB;
	}
	GXSetProjection(m, static_cast<GXProjectionType>(type));
}

int p2_wide_fill_begin(float x0, float x1)
{
	if (p2_wide_2d_k() == 1.0f || p2_wide_canvas_active() || s_fillDepth > 0 || x0 > 0.0f || x1 < kWidth) {
		return 0;
	}
	GXGetViewportv(s_fillSavedViewport);
	const f32* vp  = s_fillSavedViewport;
	const float m0 = p2_wide_map_x(0.0f);
	const float m1 = p2_wide_map_x(kWidth);
	if (std::fabs(vp[0] - m0) > 0.5f || std::fabs(vp[0] + vp[2] - m1) > 0.5f) {
		return 0; // not a full-width 2D port: leave it alone
	}
	GXSetViewport(0.0f, vp[1], kWidth, vp[3], vp[4], vp[5]);
	s_fillDepth = 1;
	return 1;
}

float p2_wide_backdrop_scale(void)
{
	// Measured at 1720x720 and 2560x720: the seasonal ground ends at about k = 1.49.
	constexpr float kBackdropCover = 1.45f;
	return std::max(s_k / kBackdropCover, 1.0f);
}

int p2_wide_backdrop_begin(const void* posMtx34, float* x0, float* x1, short* s00, short* s10, short* s01, short* s11, int texWidth, int texWrapS)
{
	const float k = p2_wide_2d_k();
	if (k == 1.0f || s_canvasDepth > 0 || s_fillDepth > 0 || s_backdropDepth > 0 || posMtx34 == nullptr || *x1 <= *x0) {
		return 0;
	}
	// View-space X of the quad's edges (the pane must be flat and unrotated), then NDC X through
	// the 2D graph's projection: ortho for J2DOrthoGraph, perspective for J2DPerspGraph (whose
	// camera puts z = 0 panes at depth 836). NDC -1..1 is the centre 608 units; the window is -k..k.
	const auto* m = static_cast<const f32(*)[4]>(posMtx34);
	if (m[0][0] <= 0.0f || std::fabs(m[0][1]) > 1e-4f || std::fabs(m[1][0]) > 1e-4f || std::fabs(m[2][0]) > 1e-4f ||
	    std::fabs(m[2][1]) > 1e-4f) {
		return 0;
	}
	f32 p[7];
	GXGetProjectionv(p);
	const bool ortho = p[0] != 0.0f;
	const float vz   = m[2][3];
	if (p[1] == 0.0f || (!ortho && vz >= -1.0f)) {
		return 0;
	}
	const auto ndc = [&](float vx) { return ortho ? p[1] * vx + p[2] : p[1] * vx / -vz - p[2]; };
	const auto viewX = [&](float n) { return ortho ? (n - p[2]) / p[1] : (n + p[2]) * -vz / p[1]; };
	const float vx0 = m[0][0] * *x0 + m[0][3];
	const float vx1 = m[0][0] * *x1 + m[0][3];
	// Within 3% of each edge counts as reaching it: the file select's grids are authored a few
	// units short of the left frame edge (NDC -0.987 / -0.994), hidden by the 4:3 border on a TV.
	if (ndc(vx0) > -0.97f || ndc(vx1) < 0.97f) {
		return 0;
	}
	// Only a 2D port over the whole frame (the remapped 0..608 port): there NDC +-k is the window.
	f32 vp[6];
	GXGetViewportv(vp);
	const float c = vp[0] + vp[2] * 0.5f;
	if (std::fabs(kCenterX + (c - kCenterX) * k - kCenterX) > 1.0f || std::fabs(vp[2] * k - kWidth) > 1.0f) {
		return 0;
	}
	// What lies beyond the frame edge comes from the texture's wrap mode. A repeating texture (grids)
	// tiles on, and a narrow texture stretched across the frame (the gradients and bands menus are
	// built from) continues smoothly. A clamped picture would smear its edge column into streaks:
	// it keeps its authored size and only shows what it already draws past the 608 frame (the
	// world map's area image and the 2-P Battle backdrop are authored wider than the frame).
	const bool extend = texWrapS == GX_REPEAT || texWrapS == GX_MIRROR || texWidth <= kMaxBackdropTexWidth;
	p2_wide_canvas_begin();
	if (!s_canvasConverted) {
		p2_wide_canvas_end();
		return 0;
	}
	if (!extend) {
		s_backdropDepth = 1;
		return 1;
	}
	const float nx0  = std::min(*x0, (viewX(-k) - m[0][3]) / m[0][0]);
	const float nx1  = std::max(*x1, (viewX(k) - m[0][3]) / m[0][0]);
	const float w    = *x1 - *x0;
	const float t0   = (nx0 - *x0) / w; // <= 0
	const float t1   = (nx1 - *x0) / w; // >= 1
	const auto lerp  = [](short a, short b, float t) {
		const float v = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t;
		return static_cast<short>(std::clamp(std::lround(v), -32768L, 32767L));
	};
	const short a00 = *s00, a10 = *s10, a01 = *s01, a11 = *s11;
	*s00 = lerp(a00, a10, t0);
	*s10 = lerp(a00, a10, t1);
	*s01 = lerp(a01, a11, t0);
	*s11 = lerp(a01, a11, t1);
	*x0  = nx0;
	*x1  = nx1;
	s_backdropDepth = 1;
	return 1;
}

void p2_wide_backdrop_end(void)
{
	if (s_backdropDepth == 0) {
		return;
	}
	s_backdropDepth = 0;
	p2_wide_canvas_end();
}

void p2_wide_fill_end(void)
{
	if (s_fillDepth == 0) {
		return;
	}
	s_fillDepth   = 0;
	const f32* vp = s_fillSavedViewport;
	GXSetViewport(vp[0], vp[1], vp[2], vp[3], vp[4], vp[5]);
}

} // extern "C"
