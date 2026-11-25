// M5.3a part 1, package P4 "view" (docs/ONLINE_MULTIPLAYER.md 2.5 and 3.3; design
// /var/tmp/mppm-online/m53a_design.md 6.2): observer LOD, constant simulation aspect, the
// --view-captain layout and the captain markers. Host-only file, built into the game library.
//
// Rule of this file: with more than 2 captains the simulation must not depend on what this peer
// presents. So for N > 2:
//  - Creature::updateLOD computes AILOD_IsVisible / IsMid / IsFar from the N simulation cameras
//    (p2_nplayer_camera); the presented Graphics viewports only add the viewport bits IsVisVP0/1
//    and mSoundVPID, which only draw and sound code read.
//  - Every play camera's mSimAspect (the aspect of its cull planes) is the constant of the stock
//    2-way split viewport, whatever viewport currently shows the camera or how big it is.
//  - The layout (stock split or one full-screen captain) and the markers touch presentation
//    state only.
// With at most 2 captains every function here returns the stock value or does nothing.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "types.h"
#include "System.h"
#include "Graphics.h"
#include "Viewport.h"
#include "Camera.h"
#include "Splitter.h"
#include "Sys/Sphere.h"
#include "Sys/Cylinder.h"
#include "Game/AILOD.h"
#include "Game/Creature.h"
#include "Game/Navi.h"
#include "Game/CameraMgr.h"
#include "Game/GameSystem.h"
#include "Game/MoviePlayer.h"
#include "Game/BaseGameSection.h"
#include "JSystem/J2D/J2DGrafContext.h"
#include "Dolphin/mtx.h"
#include "p2_host.h"

namespace {

// ---- Pure helpers (selftested) ---------------------------------------------------------------------

// Stock LOD level of one camera (creatureLOD.cpp:73-80).
int lod_level(float screenSize, float farSize, float closeSize)
{
	if (screenSize > farSize) {
		return AILOD_NULL;
	}
	if (screenSize > closeSize) {
		return AILOD_IsMid;
	}
	return AILOD_IsFar;
}

// The simulation aspect of a play camera when N > 2: the stock 2P split viewport (full width, half
// height) times the simulation k (9.6 at 640 x 480 and k 3.6).
float sim_aspect(float fbWidth, float efbHeight, float simK)
{
	const float h = efbHeight * 0.5f;
	if (fbWidth == 0.0f || h == 0.0f) {
		return 1.0f;
	}
	return fbWidth / h * simK;
}

// Projects a world point with a 3x4 view matrix and the camera's perspective into a viewport
// rectangle (x0, y0, w, h, in frame units). False when the point is behind the camera or outside
// the rectangle.
bool project_point(const Mtx view, float fovY, float aspect, float nearZ, float farZ, const float p[3], float x0, float y0,
                   float w, float h, float* outX, float* outY)
{
	float e[3];
	for (int r = 0; r < 3; r++) {
		e[r] = view[r][0] * p[0] + view[r][1] * p[1] + view[r][2] * p[2] + view[r][3];
	}
	Mtx44 proj;
	C_MTXPerspective(proj, fovY, aspect, nearZ, farZ);
	const float cw = proj[3][0] * e[0] + proj[3][1] * e[1] + proj[3][2] * e[2] + proj[3][3];
	if (!(cw > 1e-4f)) {
		return false; // behind the camera (GX cameras look down -Z: w = -z)
	}
	const float cx = proj[0][0] * e[0] + proj[0][1] * e[1] + proj[0][2] * e[2] + proj[0][3];
	const float cy = proj[1][0] * e[0] + proj[1][1] * e[1] + proj[1][2] * e[2] + proj[1][3];
	const float nx = cx / cw;
	const float ny = cy / cw;
	if (!(nx >= -1.0f && nx <= 1.0f && ny >= -1.0f && ny <= 1.0f)) {
		return false;
	}
	*outX = x0 + (nx + 1.0f) * 0.5f * w;
	*outY = y0 + (1.0f - ny) * 0.5f * h;
	return true;
}

// Viewport bits (0x10 << v) a ghost of `team` is hidden from: viewable viewports whose camera
// belongs to a captain of another team. camTeam[v] is -1 when the camera is no captain's.
unsigned ghost_bits(int vpCount, const bool* viewable, const int* camTeam, int team)
{
	unsigned bits = 0;
	for (int v = 0; v < vpCount && v < 2; v++) {
		if (viewable[v] && camTeam[v] >= 0 && camTeam[v] != team) {
			bits |= 0x10u << v;
		}
	}
	return bits;
}

// ---- Live helpers ----------------------------------------------------------------------------------

// The captain whose play camera `cam` is, else -1.
int captain_of_camera(const Camera* cam, int n)
{
	if (cam == nullptr) {
		return -1;
	}
	for (int i = 0; i < n; i++) {
		Game::PlayCamera* pc = p2_nplayer_camera(i);
		if (pc != nullptr && static_cast<const Camera*>(pc) == cam) {
			return i;
		}
	}
	return -1;
}

// Number of presented viewports the LOD and marker code looks at (the stock code has room for 2).
int presented_count(Graphics* gfx)
{
	if (gfx == nullptr) {
		return 0;
	}
	const int n = gfx->getViewportNum();
	return n < 2 ? n : 2;
}

// Inverse of p2_wide_map_x: frame X -> 2D X of a full-frame 640 x 480 ortho port.
float frame_to_2d_x(float x)
{
	return 304.0f + (x - 304.0f) * p2_wide_2d_k();
}

} // namespace

// ---- Observer LOD -----------------------------------------------------------------------------------

int p2_lod_update(Game::Creature* c, Game::AILODParm& parm, Sys::Sphere& sphere, Sys::Cylinder& cylinder)
{
	if (!p2_nplayer()) {
		return 0;
	}
	const int n = p2_navi_count();

	// Simulation: the N simulation cameras.
	bool anyVisible = false;
	int level       = AILOD_IsFar;
	for (int i = 0; i < n; i++) {
		Game::PlayCamera* cam = p2_nplayer_camera(i);
		if (cam == nullptr) {
			continue;
		}
		const bool visible = parm.mIsCylinder ? cam->isCylinderVisible(cylinder) : cam->isVisible(sphere);
		if (visible) {
			anyVisible = true;
		}
		const int l = lod_level(cam->calcScreenSize(sphere), parm.mFar, parm.mClose);
		if (l < level) {
			level = l;
		}
	}

	// Presentation: viewport bits and the sound viewport (read only by draw and sound code).
	Graphics* gfx      = sys->getGfx();
	const int vpCount  = presented_count(gfx);
	unsigned vpBits    = 0;
	for (int v = 0; v < vpCount; v++) {
		Viewport* vp = gfx->getViewport(v);
		if (vp == nullptr || !vp->viewable() || vp->getCamera() == nullptr) {
			continue;
		}
		Camera* camera = vp->getCamera();
		if (parm.mIsCylinder ? camera->isCylinderVisible(cylinder) : camera->isVisible(sphere)) {
			vpBits |= 0x10u << v;
		}
	}

	// mSoundVPID: copy of creatureLOD.cpp:87-113.
	const int viewportCount = gfx ? gfx->getViewportNum() : 0;
	if (!(Game::gameSystem->isMultiplayerMode() && (2 <= viewportCount))) {
		c->mLod.mSoundVPID = 0;
	} else {
		Viewport* vp0 = gfx->getViewport(PLAYER1_VIEWPORT);
		Viewport* vp1 = gfx->getViewport(PLAYER2_VIEWPORT);
		if (!vp0->viewable()) {
			c->mLod.mSoundVPID = 1;
		} else if (!vp1->viewable()) {
			c->mLod.mSoundVPID = 0;
		} else if (vp0->getCamera() == nullptr || vp1->getCamera() == nullptr) {
			c->mLod.mSoundVPID = 0;
		} else {
			Vector3f pos0 = *vp0->getCamera()->getSoundPositionPtr();
			Vector3f pos1 = *vp1->getCamera()->getSoundPositionPtr();
			pos0          = pos0 - sphere.mPosition;
			pos1          = pos1 - sphere.mPosition;
			c->mLod.mSoundVPID = (pos0.sqrLength() < pos1.sqrLength()) ? 0 : 1;
		}
	}

	c->mLod.mFlags = anyVisible ? (u8)(level | AILOD_IsVisible | vpBits) : (u8)AILOD_IsFar;
	if (0 < c->getCellPikiCount()) {
		c->mLod.setFlag(AILOD_PikiInCell);
	}
	return 1;
}

// ---- Simulation aspect --------------------------------------------------------------------------------

float p2_cam_sim_aspect(Camera* camera, float stockSimAspect)
{
	if (!p2_nplayer() || captain_of_camera(camera, p2_navi_count()) < 0) {
		return stockSimAspect;
	}
	GXRenderModeObj* mode = System::getRenderModeObj();
	return sim_aspect((float)mode->fbWidth, (float)mode->efbHeight, p2_net_sim_k(p2_wide_k()));
}

// ---- Presented layout -----------------------------------------------------------------------------------

void p2_nplayer_present_layout(Game::BaseGameSection* section)
{
	if (!p2_nplayer() || section == nullptr) {
		return;
	}
	const int k = p2_net_view_captain();
	if (k == -1) {
		return; // stock split of captains 0 and 1
	}
	const int n = p2_navi_count();
	if (k < 0 || k >= n) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			std::fprintf(stderr, "mppm: --view-captain %d: no such captain (%d captains), showing the split view\n", k, n);
		}
		return;
	}
	if (Game::moviePlayer && Game::moviePlayer->isFlag(Game::MVP_IsActive)) {
		return;
	}
	Game::PlayCamera* cam = p2_nplayer_camera(k);
	if (cam == nullptr || section->mSplitter == nullptr || sys->mGfx == nullptr) {
		return;
	}
	section->mSplitter->split2(1.0f);
	Viewport* vp = sys->mGfx->getViewport(PLAYER1_VIEWPORT);
	if (vp != nullptr) {
		vp->setCamera(cam);
	}
}

// ---- Captain markers ---------------------------------------------------------------------------------------

void p2_nplayer_draw_markers(Graphics& gfx)
{
	if (!p2_nplayer() || Game::naviMgr == nullptr) {
		return;
	}
	const int n       = p2_navi_count();
	const int vpCount = presented_count(&gfx);

	J2DOrthoGraph graf(0.0f, 0.0f, 640.0f, 480.0f, -1.0f, 1.0f);
	graf.setPort();

	const float box = 16.0f;
	const float dot = 3.0f;
	for (int v = 0; v < vpCount; v++) {
		Viewport* vp = gfx.getViewport(v);
		if (vp == nullptr || !vp->viewable() || vp->getCamera() == nullptr) {
			continue;
		}
		Camera* cam  = vp->getCamera();
		Matrixf* mtx = cam->getViewMatrix(true);
		if (mtx == nullptr) {
			continue;
		}
		Rectf& b = vp->mBounds2;
		for (int i = 0; i < n && i < Game::naviMgr->getMax(); i++) {
			Game::Navi* navi = Game::naviMgr->getAt(i);
			if (navi == nullptr) {
				continue;
			}
			const Vector3f pos = navi->getPosition();
			const float p[3]   = { pos.x, pos.y + 30.0f, pos.z };
			float fx, fy;
			if (!project_point(mtx->mMatrix.mtxView, cam->mViewAngle, cam->mAspectRatio, cam->getNear(), cam->getFar(), p, b.p1.x,
			                   b.p1.y, b.getWidth(), b.getHeight(), &fx, &fy)) {
				continue;
			}
			const float x = frame_to_2d_x(fx) - box * 0.5f;
			const float y = fy - box;

			// Team by colour and by shape (plan 2.5, colour-blind safe): Okabe-Ito vermillion, plain square
			// for team 0 (Red onyon); Okabe-Ito blue, square in a 2-pixel white frame for team 1 (Blue
			// onyon). A downed captain keeps the shape and turns grey.
			const int team = p2_navi_team(i);
			JUtility::TColor colour(150, 150, 150, 255); // down
			if (navi->isAlive()) {
				colour = team == 0 ? JUtility::TColor(213, 94, 0, 255) : JUtility::TColor(0, 114, 178, 255);
			}
			if (team != 0) {
				J2DFillBox(x - 2.0f, y - 2.0f, box + 4.0f, box + 4.0f, JUtility::TColor(255, 255, 255, 255));
			}
			J2DFillBox(x, y, box, box, colour);

			// i + 1 white dots in a row (2 x 2 grid for 3 and 4).
			const JUtility::TColor white(255, 255, 255, 255);
			const int dots = i + 1;
			for (int d = 0; d < dots && d < 4; d++) {
				const int col = d & 1;
				const int row = d >> 1;
				const float dx = (dots == 1) ? 6.5f : (col == 0 ? 3.5f : 9.5f);
				const float dy = (dots <= 2) ? 6.5f : (row == 0 ? 3.5f : 9.5f);
				J2DFillBox(x + dx, y + dy, dot, dot, white);
			}
		}
	}
}

// ---- Ghost card -------------------------------------------------------------------------------------------

unsigned p2_vs_ghost_vpbits(int team)
{
	if (!p2_nplayer()) {
		return team == 1 ? AILOD_IsVisVP0 : AILOD_IsVisVP1;
	}
	Graphics* gfx     = sys->getGfx();
	const int vpCount = presented_count(gfx);
	const int n       = p2_navi_count();
	bool viewable[2]  = { false, false };
	int camTeam[2]    = { -1, -1 };
	for (int v = 0; v < vpCount; v++) {
		Viewport* vp = gfx->getViewport(v);
		if (vp == nullptr) {
			continue;
		}
		viewable[v]   = vp->viewable();
		const int cap = captain_of_camera(vp->getCamera(), n);
		camTeam[v]    = cap >= 0 ? p2_navi_team(cap) : -1;
	}
	return ghost_bits(vpCount, viewable, camTeam, team);
}

// ---- Selftest (MPPM_NPLAYER_SELFTEST=view; runs before main, pure helpers only) ------------------------------

namespace {

struct ViewSelftest {
	ViewSelftest()
	{
		const char* v = std::getenv("MPPM_NPLAYER_SELFTEST");
		if (v == nullptr || std::strcmp(v, "view") != 0) {
			return;
		}
		int fails = 0;
		auto check = [&fails](bool ok, const char* what) {
			if (!ok) {
				std::fprintf(stderr, "mppm: nplayer selftest view: FAILED %s\n", what);
				fails++;
			}
		};

		// LOD level table (AILODParm defaults: far 0.07, close 0.02).
		check(lod_level(0.10f, 0.07f, 0.02f) == AILOD_NULL, "lod near");
		check(lod_level(0.05f, 0.07f, 0.02f) == AILOD_IsMid, "lod mid");
		check(lod_level(0.02f, 0.07f, 0.02f) == AILOD_IsFar, "lod close edge is far");
		check(lod_level(0.01f, 0.07f, 0.02f) == AILOD_IsFar, "lod far");
		check(lod_level(0.07f, 0.07f, 0.02f) == AILOD_IsMid, "lod far edge is mid");

		// Projection: identity view (camera at the origin looking down -Z), viewport 640 x 240 at y 240.
		Mtx view = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
		float x = 0.0f, y = 0.0f;
		const float ahead[3]  = { 0.0f, 0.0f, -100.0f };
		const float behind[3] = { 0.0f, 0.0f, 100.0f };
		const float side[3]   = { 1000.0f, 0.0f, -100.0f };
		const bool okAhead    = project_point(view, 30.0f, 640.0f / 240.0f, 1.0f, 5000.0f, ahead, 0.0f, 240.0f, 640.0f, 240.0f, &x, &y);
		check(okAhead && std::fabs(x - 320.0f) < 0.01f && std::fabs(y - 360.0f) < 0.01f, "ahead lands at the centre");
		check(!project_point(view, 30.0f, 640.0f / 240.0f, 1.0f, 5000.0f, behind, 0.0f, 240.0f, 640.0f, 240.0f, &x, &y), "behind rejected");
		check(!project_point(view, 30.0f, 640.0f / 240.0f, 1.0f, 5000.0f, side, 0.0f, 240.0f, 640.0f, 240.0f, &x, &y), "off screen rejected");
		const float up[3] = { 0.0f, 10.0f, -100.0f };
		check(project_point(view, 30.0f, 640.0f / 240.0f, 1.0f, 5000.0f, up, 0.0f, 240.0f, 640.0f, 240.0f, &x, &y) && y < 360.0f, "up is up");

		// Ghost bits. Split: viewport 0 shows captain 0 (team 0), viewport 1 captain 1 (team 1).
		bool split[2]    = { true, true };
		int splitTeam[2] = { 0, 1 };
		check(ghost_bits(2, split, splitTeam, 1) == AILOD_IsVisVP0, "split ghost team 1");
		check(ghost_bits(2, split, splitTeam, 0) == AILOD_IsVisVP1, "split ghost team 0");
		// Single view of captain 3 (team 1); viewport 1 has height 0.
		bool single[2]    = { true, false };
		int singleTeam[2] = { 1, 0 };
		check(ghost_bits(2, single, singleTeam, 1) == 0, "single view own team");
		check(ghost_bits(2, single, singleTeam, 0) == AILOD_IsVisVP0, "single view other team");
		int movieTeam[2] = { -1, -1 };
		check(ghost_bits(2, split, movieTeam, 0) == 0, "no captain camera");

		// Aspect: the stock 2P split viewport at sim k 3.6 is 9.6, at k 1 it is 640 / 240.
		check(std::fabs(sim_aspect(640.0f, 480.0f, 3.6f) - 9.6f) < 1e-4f, "aspect k 3.6");
		check(std::fabs(sim_aspect(640.0f, 480.0f, 1.0f) - 640.0f / 240.0f) < 1e-5f, "aspect k 1");

		std::printf("mppm: nplayer selftest view %s\n", fails == 0 ? "PASS" : "FAIL");
		std::fflush(stdout);
		std::_Exit(fails == 0 ? 0 : 1);
	}
};

ViewSelftest sViewSelftest;

} // namespace
