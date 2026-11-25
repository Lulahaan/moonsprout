// Online harness, M5.1b (docs/ONLINE_MULTIPLAYER.md 2.3 step 5, 2.5, 2.10 item 4): the load
// barrier wrappers, the camera step at the start of the tick, the test events and the tick order.
// Host-only file, built into the game library (not in the matching build).
//
// Nonsim regions. The real loading loops (Section::loading, fadeIn, the wait loop of fadeOut, the
// loop after fadeOut in Section::run, waitSyncLoad and the single frames before it) only wait for a
// load. Their number of passes depends on the machine, so in a session their pad reads are not
// ticks: the seam releases every slot there and consumes no record. Leaving the outermost region
// reads the pads once more (still released), so the button edges the next tick sees never depend
// on whether the loop ran zero passes or several.
//
// Camera step. Stock Pikmin 2 steps the play cameras at the top of BaseGameSection::doDraw, which
// runs right before doUpdate in the same pass, with the pad state of that pass. It now runs at the
// top of doUpdate with the same pad state, so the simulation sees the same order (pad read, camera
// step, update) whichever of update and draw comes first. Camera::update leaves the view from
// before the step in mCurViewMatrix (right for the stock place, where the draw ran after the last
// view calculation); the draw reads that matrix (getMatrix(true)) and must now see the view the
// tick's view calculation used, so it is copied over after the step.
//
// Tick order. With update-first, Section::main's update() runs right after the pacing wait in
// JFWDisplay::beginRender (before the GX frame set-up), so the picture of a pass shows the
// update of the same pass. The cycle draw, sound frame, update is unchanged; only where the pad
// read and the present fall in it moves.
#include "Section.h"
#include "System.h"
#include "GameFlow.h"
#include "Graphics.h"
#include "Viewport.h"
#include "Camera.h"
#include "Game/BaseGameSection.h"
#include "Game/GameSystem.h"
#include "Game/CameraMgr.h"
#include "Game/Navi.h"
#include "Game/CPlate.h"
#include "JSystem/JUtility/JUTGamePad.h"
#include "p2_host.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "p2_vtable.h"

namespace {

Section* sArmed = nullptr;
bool sUpdated   = false; // this pass's update ran in p2_net_pass_after_wait (consumed by Section::main)
bool sRanFirst  = false; // the same, kept until the next pass_begin (the probe reads it)

void apply_debug_event()
{
	const int ev = p2_net_debug_event();
	if (ev < 1 || ev > P2_MAX_NAVI) { // 1, 2: captains 0, 1; M5.3a: 3, 4: captains 2, 3
		return;
	}
	const int idx = ev - 1;
	if (Game::naviMgr == nullptr || idx >= Game::naviMgr->getMax()) {
		return;
	}
	Game::Navi* navi = Game::naviMgr->getAt(idx);
	if (navi != nullptr && navi->isAlive()) {
		navi->addDamage(10000.0f, false);
	}
}

void step_cameras()
{
	Game::CameraMgr* mgr = Game::cameraMgr;
	if (mgr == nullptr || Game::gameSystem == nullptr || Game::gameSystem->isZukanMode()) {
		return;
	}
	if (!mgr->isCameraUpdateOn()) {
		return; // CameraMgr::update would return at once: no camera steps, nothing to copy
	}
	if (Game::gameSystem->paused()) {
		mgr->controllerLock(Game::CAMNAVI_Both);
		mgr->update();
		mgr->controllerUnLock(Game::CAMNAVI_Both);
	} else {
		mgr->update();
	}

	// M5.3a: with more than 2 captains every play camera gets the copy, whichever viewport shows it,
	// so the draw of any presented layout sees the tick's view.
	if (p2_nplayer()) {
		const int n = p2_cam_count();
		for (int i = 0; mgr->mCameraObjList != nullptr && i < n; i++) {
			Camera* cam = mgr->mCameraObjList[i];
			if (cam != nullptr && cam->mViewMatrix != nullptr) {
				PSMTXCopy(cam->mViewMatrix->mMatrix.mtxView, cam->mCurViewMatrix.mMatrix.mtxView);
			}
		}
		return;
	}

	// Each stepped play camera that a viewport shows: the draw's view is the tick's view.
	Graphics* gfx = sys->mGfx;
	for (int i = 0; i < gfx->mActiveViewports; i++) {
		Viewport* vp = gfx->getViewport(i);
		if (vp == nullptr || vp->mCamera == nullptr || vp->mCamera->mViewMatrix == nullptr) {
			continue;
		}
		Camera* cam = vp->mCamera;
		if (mgr->mCameraObjList == nullptr
		    || (cam != mgr->mCameraObjList[0] && cam != mgr->mCameraObjList[1])) {
			continue;
		}
		PSMTXCopy(cam->mViewMatrix->mMatrix.mtxView, cam->mCurViewMatrix.mMatrix.mtxView);
	}
}

// M5.3a aid MPPM_NPLAYER_STATS=1: per captain walked distance and largest squad, printed every
// 1800 session ticks. Read-only.
struct NplayerStats {
	Game::NaviMgr* mgr;
	int count;
	uint32_t lastTick;
	bool havePos[P2_MAX_NAVI];
	float x[P2_MAX_NAVI], z[P2_MAX_NAVI];
	double walked[P2_MAX_NAVI];
	int squadMax[P2_MAX_NAVI];
};
NplayerStats sStats;

bool nplayer_stats_on()
{
	static int on = -1;
	if (on < 0) {
		const char* v = getenv("MPPM_NPLAYER_STATS");
		on            = (v != nullptr && strcmp(v, "1") == 0) ? 1 : 0;
	}
	return on == 1;
}

void nplayer_stats()
{
	Game::NaviMgr* mgr = Game::naviMgr;
	if (!nplayer_stats_on() || mgr == nullptr) {
		return;
	}
	const int n = p2_navi_count();
	if (mgr != sStats.mgr || n != sStats.count) {
		memset(&sStats, 0, sizeof(sStats));
		sStats.mgr   = mgr;
		sStats.count = n;
	}
	for (int i = 0; i < n && i < mgr->getMax(); i++) {
		Game::Navi* navi = mgr->getAt(i);
		if (navi == nullptr || !navi->isAlive()) {
			continue;
		}
		Vector3f pos = navi->getPosition();
		if (sStats.havePos[i]) {
			const float dx = pos.x - sStats.x[i], dz = pos.z - sStats.z[i];
			sStats.walked[i] += sqrtf(dx * dx + dz * dz);
		}
		sStats.havePos[i] = true;
		sStats.x[i]       = pos.x;
		sStats.z[i]       = pos.z;
		const int squad   = navi->mCPlateMgr != nullptr ? navi->mCPlateMgr->mSlotCount : 0;
		if (squad > sStats.squadMax[i]) {
			sStats.squadMax[i] = squad;
		}
	}
	const uint32_t tick = p2_net_tick();
	if (tick == 0 || tick % 1800 != 0 || tick == sStats.lastTick) {
		return;
	}
	sStats.lastTick = tick;
	for (int i = 0; i < n && i < mgr->getMax(); i++) {
		Game::Navi* navi = mgr->getAt(i);
		const int alive  = (navi != nullptr && navi->isAlive()) ? 1 : 0;
		const int squad  = (navi != nullptr && navi->mCPlateMgr != nullptr) ? navi->mCPlateMgr->mSlotCount : 0;
		printf("mppm: nplayer: tick=%u captain=%d team=%d alive=%d walked=%d squad=%d squadmax=%d x=%d z=%d\n",
		       (unsigned)tick, i, p2_navi_team(i), alive, (int)sStats.walked[i], squad, sStats.squadMax[i], (int)sStats.x[i],
		       (int)sStats.z[i]);
	}
	fflush(stdout);
}

// Itanium C++ ABI: an object's first word points two slots into its class's vtable (the game has
// no RTTI). Game::gameSystem is never cleared when a game section ends and its memory is reused by
// the next section's heap, so it is read only once the armed section is known to be the running
// game section (the same check as Present.cpp's section_kind).
#if defined(__GNUC__) || defined(__clang__)
P2_VTABLE(_ZTVN4Game17SingleGameSectionE, "??_7SingleGameSection@Game@@6B@")
P2_VTABLE(_ZTVN4Game13VsGameSectionE, "??_7VsGameSection@Game@@6B@")
bool is_game_section(Section* s)
{
	void* vptr = *reinterpret_cast<void**>(s);
	return vptr == P2_VPTR(_ZTVN4Game17SingleGameSectionE) || vptr == P2_VPTR(_ZTVN4Game13VsGameSectionE);
}
#else
bool is_game_section(Section*) { return false; }
#endif

// The armed section is GameFlow's running section, a BaseGameSection, and its game system's.
bool running_game_section(Section* armed)
{
	if (armed == nullptr || sys == nullptr || sys->mGameFlow == nullptr) {
		return false;
	}
	if (static_cast<Section*>(sys->mGameFlow->getCurrentSection()) != armed || !is_game_section(armed)) {
		return false;
	}
	return Game::gameSystem != nullptr && static_cast<Section*>(Game::gameSystem->mSection) == armed;
}

} // namespace

static bool sInUpdateFirst = false; // inside the update that p2_net_pass_after_wait runs
static bool sNestedSeen    = false; // a frame loop already started inside that update

extern "C" void p2_net_nonsim_begin(void)
{
	// Every frame loop that can run inside an update (waitSyncLoad, BaseGameSection::init,
	// setupFixMemory) enters here first. Under update-first the outer pass has not drawn yet, so
	// its frame must not be presented by the loop's leading endFrame().
	if (sInUpdateFirst && !sNestedSeen) {
		sNestedSeen = true;
		p2_host_skip_next_present();
	}
	p2_net_nonsim_enter();
}

extern "C" void p2_net_nonsim_end(void)
{
	if (p2_net_session_active() && p2_net_nonsim() == 1) {
		JUTGamePad::read(); // released by the seam; not a tick
	}
	p2_net_nonsim_leave();
}

extern "C" void p2_net_sim_tick_begin(void* section)
{
	p2_net_perf_sim_begin();
	(void)section;
	apply_debug_event();
	step_cameras();
	nplayer_stats();
}

extern "C" void p2_net_pass_begin(void* section)
{
	sArmed    = static_cast<Section*>(section);
	sUpdated  = false;
	sRanFirst = false;
}

extern "C" void p2_net_pass_after_wait(void)
{
	Section* armed = sArmed;
	sArmed         = nullptr; // a nested beginRender (waitSyncLoad inside the update) finds none
	if (!running_game_section(armed) || !p2_host_update_first()) {
		return;
	}
	sInUpdateFirst = true;
	sNestedSeen    = false;
	armed->update();
	sInUpdateFirst = false;
	sUpdated  = true;
	sRanFirst = true;
}

extern "C" int p2_net_pass_updated(void)
{
	const bool updated = sUpdated;
	sUpdated           = false;
	return updated ? 1 : 0;
}

extern "C" int p2_net_pass_ran_update_first(void) { return sRanFirst ? 1 : 0; }
