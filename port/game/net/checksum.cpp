// Online harness, M5.1a (docs/ONLINE_MULTIPLAYER.md 2.7): the per-subsystem checksum at the end of
// BaseGameSection::doUpdate. Host-only file, built into the game library (not in the matching
// build).
//
// One FNV-1a 32 hash per subsystem (M5.1b adds the cameras and the sim-relevant gP2Config fields). Floats are hashed by bit pattern, pointers and addresses are
// never hashed, objects are walked in manager order, and every manager pointer is checked.
#include "Game/Navi.h"
#include "Game/Piki.h"
#include "Game/PikiMgr.h"
#include "Game/EnemyBase.h"
#include "Game/generalEnemyMgr.h"
#include "Game/pelletMgr.h"
#include "Game/GameSystem.h"
#include "Game/TimeMgr.h"
#include "Game/VsGameSection.h"
#include "Game/VsGame.h"
#include "Game/gamePlayData.h"
#include "Game/CameraMgr.h"
#include "Camera.h"
#include "p2_host.h"
#include "p2_config.h"

#include <string.h>

namespace {

struct Fnv {
	u32 mHash = 2166136261u;

	void bytes(const void* p, size_t n)
	{
		const u8* b = (const u8*)p;
		for (size_t i = 0; i < n; i++) {
			mHash ^= b[i];
			mHash *= 16777619u;
		}
	}
	void u(u32 v) { bytes(&v, sizeof(v)); }
	void i(int v) { u((u32)v); }
	void f(f32 v)
	{
		u32 bits;
		memcpy(&bits, &v, sizeof(bits));
		u(bits);
	}
	void vec(const Vector3f& v)
	{
		f(v.x);
		f(v.y);
		f(v.z);
	}
};

u16 clamp16(int n) { return n > 0xFFFF ? 0xFFFF : (u16)n; }

void hash_navi(P2NetChecksum& out)
{
	Fnv h;
	int n = 0;
	if (Game::naviMgr) {
		Iterator<Game::Navi> it(Game::naviMgr);
		CI_LOOP(it)
		{
			Game::Navi* navi = *it;
			h.u(navi->mNaviIndex);
			h.vec(navi->getPosition());
			h.f(navi->mHealth);
			h.u(navi->isAlive() ? 1 : 0);
			h.i(navi->getStateID());
			h.i(navi->mSprayCounts[0]);
			h.i(navi->mSprayCounts[1]);
			n++;
		}
	}
	if (p2_nplayer()) { // M5.3a: the VS card users steer card targets for N > 2 (1-2 captain hashes unchanged)
		h.i(p2_vs_card_user(0));
		h.i(p2_vs_card_user(1));
	}
	out.navi      = h.mHash;
	out.naviCount = clamp16(n);
}

void hash_piki(P2NetChecksum& out)
{
	Fnv h;
	int n = 0;
	if (Game::pikiMgr) {
		Iterator<Game::Piki> it(Game::pikiMgr);
		CI_LOOP(it)
		{
			Game::Piki* piki = *it;
			h.vec(piki->getPosition());
			h.u(piki->getKind());
			h.u(piki->getHappa());
			h.i(piki->getStateID());
			h.u(piki->isAlive() ? 1 : 0);
			if (p2_nplayer()) { // M5.3a: the owner steers rejoin and touch joins for N > 2
				h.u(piki->mP2Owner);
			}
			n++;
		}
	}
	out.piki      = h.mHash;
	out.pikiCount = clamp16(n);
}

void hash_enemy(P2NetChecksum& out)
{
	Fnv h;
	int n = 0;
	if (Game::generalEnemyMgr) {
		GeneralMgrIterator<Game::EnemyBase> it(Game::generalEnemyMgr);
		CI_LOOP(it)
		{
			Game::EnemyBase* enemy = it.getObject();
			h.i((int)enemy->getEnemyTypeID());
			h.vec(enemy->getPosition());
			h.f(enemy->mHealth);
			h.u(enemy->isAlive() ? 1 : 0);
			n++;
		}
	}
	out.enemy      = h.mHash;
	out.enemyCount = clamp16(n);
}

void hash_pellet(P2NetChecksum& out)
{
	Fnv h;
	int n = 0;
	if (Game::pelletMgr) {
		Game::PelletIterator it;
		CI_LOOP(it)
		{
			Game::Pellet* pellet = *it;
			h.u(pellet->getKind());
			h.vec(pellet->getPosition());
			n++;
		}
	}
	out.pellet      = h.mHash;
	out.pelletCount = clamp16(n);
}

void hash_timers(P2NetChecksum& out)
{
	Fnv h;
	Game::GameSystem* gs = Game::gameSystem;
	if (gs) {
		h.u(gs->mFrameTimer);
		h.i((int)gs->mMode);
		h.u(gs->mFlags.typeView);
		h.u(gs->mIsPaused);
		h.u(gs->mIsPausedSoft ? 1 : 0);
		h.u(gs->mIsMoviePause ? 1 : 0);
		h.u(gs->mIsFrozen ? 1 : 0);
		if (gs->mTimeMgr) {
			h.f(gs->mTimeMgr->mCurrentRealTime);
			h.f(gs->mTimeMgr->mCurrentTimeOfDay);
			h.u(gs->mTimeMgr->mDayCount);
		}
	}
	// M5.1b: the host settings the simulation reads (locked in a session; a lock that failed
	// shows at the first C line).
	h.f(gP2Config.daySpeed);
	h.i(gP2Config.sunsetOnDayOne);
	h.i(gP2Config.unlockChallenge);
	h.i(gP2Config.invincibleCaptains);
	h.i(gP2Config.infiniteSprays);
	out.timers = h.mHash;
}

void hash_mode(P2NetChecksum& out)
{
	Fnv h;
	Game::GameSystem* gs = Game::gameSystem;
	if (gs) {
		h.i((int)gs->mMode);
		if (gs->isVersusMode() && gs->mSection) {
			Game::VsGameSection* vs = static_cast<Game::VsGameSection*>(gs->mSection);
			h.f(vs->mTimeLimit);
			h.f(vs->mGhostIconTimers[0]);
			h.f(vs->mGhostIconTimers[1]);
			h.i(vs->mPokoCount);
			h.i(vs->mCurrentFloor);
			h.i(vs->mVsWinner);
			for (int i = 0; i < 2; i++) {
				h.f(vs->mRedBlueYellowScore[i]);
				h.f(vs->mCherryScore[i]);
				h.f(vs->mYellowScore[i]);
				h.f(vs->mRedBlueScore[i]);
				h.i(vs->mDopeCounts[i][0]);
				h.i(vs->mDopeCounts[i][1]);
				h.i(vs->mMarbleCount[i]);
				h.i(vs->mYellowMarbleCounts[i]);
			}
			h.i(vs->mPlayer1Cherries);
			h.i(vs->mPlayer2Cherries);
			h.i(vs->mCardCount);
			h.f(vs->mSpawnTimer);
			h.i(vs->mDeadPikiCount);
			h.i(vs->mOlimarHandicap);
			h.i(vs->mLouieHandicap);
			h.i(Game::VsGameSection::mRedWinCount);
			h.i(Game::VsGameSection::mBlueWinCount);
			h.i(Game::VsGameSection::mDrawCount);
			// Lose causes (plan 2.7) live in the Game state of the section's state machine.
			Game::VsGame::State* st = vs->mCurrentState;
			h.i(st ? st->mId : -1);
			if (st && st->mId == Game::VsGame::VGS_Game) {
				Game::VsGame::GameState* game = static_cast<Game::VsGame::GameState*>(st);
				h.i(game->mLoseCauses[0].typeView);
				h.i(game->mLoseCauses[1].typeView);
			}
		}
	}
	if (Game::playData) {
		h.i(Game::playData->mSprayCount[0]);
		h.i(Game::playData->mSprayCount[1]);
	}
	out.mode = h.mHash;
}

// M5.1b: both play cameras (they step in the tick now): view matrix and view angle. 0 when there
// is no camera manager.
void hash_camera(P2NetChecksum& out)
{
	Game::CameraMgr* mgr = Game::cameraMgr;
	if (mgr == nullptr || mgr->mCameraObjList == nullptr) {
		out.camera = 0;
		return;
	}
	Fnv h;
	// M5.3a: one PlayCamera per captain (p2_cam_count() is 2 for 1-2 captains, so the hash is unchanged).
	for (int i = 0; i < p2_cam_count(); i++) {
		Camera* cam = mgr->mCameraObjList[i];
		if (cam == nullptr || cam->mViewMatrix == nullptr) {
			h.u(0);
			continue;
		}
		const f32* m = &cam->mViewMatrix->mMatrix.mtxView[0][0];
		for (int e = 0; e < 12; e++) {
			h.f(m[e]);
		}
		h.f(cam->mViewAngle);
	}
	out.camera = h.mHash;
}

} // namespace

extern "C" void p2_net_sim_tick_end(void)
{
	p2_net_perf_sim_end();
	if (!p2_net_checksum_due()) {
		return;
	}
	P2NetChecksum sum;
	memset(&sum, 0, sizeof(sum));
	sum.rng = p2_msl_rand_state();
	hash_navi(sum);
	hash_piki(sum);
	hash_enemy(sum);
	hash_pellet(sum);
	hash_timers(sum);
	hash_mode(sum);
	hash_camera(sum);
	p2_net_checksum_submit(&sum);
}
