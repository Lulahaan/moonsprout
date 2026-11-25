// M5.3a part 1 (docs/ONLINE_MULTIPLAYER.md 3.1, 3.3, 2.5): the section roster of an N-captain
// section: controllers, play cameras and simulation viewports of captains 2..N-1, their spawn and
// binding, and the CameraMgr extension for N cameras. Host-only file. Design:
// /var/tmp/mppm-online/m53a_design.md, section 4.2.
//
// Captains 0 and 1 keep the section's stock objects (mControllerP1/P2, mOlimarCamera/mLouieCamera).
// Every hook does nothing while the section has at most 2 captains.
#include "Game/BaseGameSection.h"
#include "Game/CameraMgr.h"
#include "Game/GameSystem.h"
#include "Game/MapMgr.h"
#include "Game/Navi.h"
#include "Game/Piki.h"
#include "Game/Entities/ItemOnyon.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "Controller.h"
#include "Viewport.h"
#include "sysMath.h"
#include "p2_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

struct Roster {
	Game::BaseGameSection* section;
	int count; // latched in p2_nplayer_setup (naviMgr->resetMgr() sets it back before clear)
	Controller* ctrl[P2_MAX_NAVI];
	Game::PlayCamera* cam[P2_MAX_NAVI];
	Viewport* simVp[P2_MAX_NAVI];
};

Roster sRoster;

bool stats_on()
{
	static int on = -1;
	if (on < 0) {
		const char* v = getenv("MPPM_NPLAYER_STATS");
		on            = (v != nullptr && strcmp(v, "1") == 0) ? 1 : 0;
	}
	return on == 1;
}

int extra_count() { return sRoster.count > 2 ? sRoster.count : 2; }

} // namespace

void p2_nplayer_setup(Game::BaseGameSection* section)
{
	memset(&sRoster, 0, sizeof(sRoster));
	sRoster.section = section;
	sRoster.count   = p2_navi_count();
	sRoster.ctrl[0] = section->mControllerP1;
	sRoster.ctrl[1] = section->mControllerP2;
	sRoster.cam[0]  = section->mOlimarCamera;
	sRoster.cam[1]  = section->mLouieCamera;
	for (int i = 2; i < sRoster.count; i++) {
		sRoster.ctrl[i] = new Controller((JUTGamePad::EPadPort)i);
		sRoster.cam[i]  = new Game::PlayCamera(Game::naviMgr->getAt(i));
	}
}

void p2_cam_attach(Game::BaseGameSection* section)
{
	(void)section;
	if (sRoster.count <= 2 || Game::cameraMgr == nullptr) {
		return;
	}
	for (int i = 0; i < sRoster.count; i++) {
		Viewport* vp = new Viewport; // full screen, never drawn: the simulation's view of camera i
		if (i < 2) {
			// The presented viewport already set this camera's aspect; keep it (presentation).
			vp->mCamera = sRoster.cam[i];
		} else {
			vp->setCamera(sRoster.cam[i]);
		}
		sRoster.simVp[i]                   = vp;
		Game::cameraMgr->mViewportList[i]  = vp;
		Game::cameraMgr->mCameraObjList[i] = sRoster.cam[i];
	}
}

void p2_nplayer_spawn(Game::BaseGameSection* section)
{
	if (stats_on()) {
		JKRExpHeap* heap = section->mTheExpHeap;
		printf("mppm: nplayer: captains=%d heap free=%u\n", sRoster.count > 2 ? sRoster.count : p2_navi_count(),
		       heap != nullptr ? (unsigned)heap->getFreeSize() : 0u);
		fflush(stdout);
	}
	if (sRoster.count <= 2) {
		return;
	}
	const f32 rot = roundAng(Game::mapMgr->getMapRotation());
	for (int i = 2; i < sRoster.count; i++) {
		Game::Navi* navi = Game::naviMgr->birth();
		if (navi == nullptr || (int)navi->mNaviIndex != i) {
			JUT_PANICLINE(__LINE__, "nplayer spawn: navi %d\n", i);
		}
		navi->init(nullptr);
		navi->mFaceDir = rot;
		navi->setCamera(sRoster.cam[i]);
		navi->setController(sRoster.ctrl[i]);

		const int team    = p2_navi_team(i);
		Vector3f position = Vector3f(-40.0f, 0.0f, 2.0f);
		Game::Onyon* own   = Game::ItemOnyon::mgr ? Game::ItemOnyon::mgr->getOnyon(team == 0 ? Game::Red : Game::Blue) : nullptr;
		Game::Onyon* enemy = Game::ItemOnyon::mgr ? Game::ItemOnyon::mgr->getOnyon(team == 0 ? Game::Blue : Game::Red) : nullptr;
		if (own != nullptr) {
			position = own->getPosition();
			if (enemy != nullptr) {
				Vector3f ep    = enemy->getPosition();
				const f32 frac = 0.06f * (f32)(i / 2);
				position.x += (ep.x - position.x) * frac;
				position.y += (ep.y - position.y) * frac;
				position.z += (ep.z - position.z) * frac;
			}
		}
		Vector3f velocity = Vector3f(0.0f);
		navi->setPosition(position, false);
		navi->setVelocity(velocity);
	}
}

void p2_nplayer_bind(Game::BaseGameSection* section, int playerMode)
{
	(void)section;
	if (sRoster.count <= 2 || Game::naviMgr == nullptr) {
		return;
	}
	for (int i = 2; i < sRoster.count; i++) {
		Game::Navi* navi = Game::naviMgr->getAt(i);
		if (playerMode == PLAYERMODE_Multiplayer) {
			navi->setCamera(sRoster.cam[i]);
			navi->setController(sRoster.ctrl[i]);
		} else {
			navi->disableController();
		}
	}
}

void p2_nplayer_fix_near_far(bool isFixed, float nearZ, float farZ)
{
	for (int i = 2; i < extra_count(); i++) {
		if (sRoster.cam[i] != nullptr) {
			sRoster.cam[i]->setFixNearFar(isFixed, nearZ, farZ);
		}
	}
}

void p2_nplayer_del_cameras(void)
{
	for (int i = 2; i < extra_count(); i++) {
		if (sRoster.cam[i] != nullptr) {
			sRoster.cam[i]->del();
		}
	}
}

void p2_nplayer_clear(void)
{
	// Before the section heap is destroyed: a JUTGamePad unlinks itself from mPadList.
	for (int i = 2; i < extra_count(); i++) {
		delete sRoster.ctrl[i];
	}
	memset(&sRoster, 0, sizeof(sRoster));
}

Controller* p2_nplayer_controller(int idx)
{
	if (idx >= 0 && idx < sRoster.count && idx < P2_MAX_NAVI && sRoster.section != nullptr) {
		if (idx == 0) return sRoster.section->mControllerP1;
		if (idx == 1) return sRoster.section->mControllerP2;
		return sRoster.ctrl[idx];
	}
	return nullptr;
}

Game::PlayCamera* p2_nplayer_camera(int idx)
{
	if (idx >= 0 && idx < sRoster.count && idx < P2_MAX_NAVI && sRoster.section != nullptr) {
		if (idx == 0) return sRoster.section->mOlimarCamera;
		if (idx == 1) return sRoster.section->mLouieCamera;
		return sRoster.cam[idx];
	}
	if (sRoster.section == nullptr && idx >= 0 && idx < 2 && Game::cameraMgr != nullptr && Game::cameraMgr->mCameraObjList != nullptr) {
		return Game::cameraMgr->mCameraObjList[idx]; // no roster (outside a game section's setup)
	}
	return nullptr;
}

// ---- CameraMgr for N cameras -------------------------------------------------------------------------

int p2_cam_count(void) { return p2_navi_count(); }

int p2_cam_range(int* range, int camID) { return p2_cam_range_n(range, camID, p2_cam_count()); }

void p2_cam_update_extra(Game::CameraMgr* mgr)
{
	const int n = p2_cam_count();
	for (int i = 2; i < n; i++) {
		if (mgr->mCameraObjList[i] != nullptr) {
			mgr->mCameraObjList[i]->update();
		}
	}
}

int p2_cam_extra_change_state(Game::CameraMgr* mgr)
{
	int state   = 0;
	const int n = p2_cam_count();
	for (int i = 2; i < n; i++) {
		if (mgr->mCameraObjList[i] != nullptr) {
			state |= mgr->mCameraObjList[i]->mChangePlayerState;
		}
	}
	return state;
}

void p2_cam_multi_extra(Game::CameraMgr* mgr)
{
	const int n = p2_cam_count();
	for (int i = 2; i < n; i++) {
		if (mgr->mCameraObjList[i] != nullptr) {
			mgr->mCameraObjList[i]->setCameraParms(mgr->mCameraParms[1]);
			mgr->mCameraObjList[i]->changePlayerMode(false);
		}
	}
}
