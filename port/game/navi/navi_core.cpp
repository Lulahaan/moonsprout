// M5.3a part 1 (docs/ONLINE_MULTIPLAYER.md 3.1-3.2): roster rules of an N-captain section.
// Host-only file. Design: /var/tmp/mppm-online/m53a_design.md, section 4.2.
//
// Every live function returns the stock value while the running section has at most 2 captains
// (p2_nplayer() == 0), so story, Challenge and 2P Battle with 1-2 captains are unchanged.
#include "Game/Navi.h"
#include "Game/CameraMgr.h"
#include "Game/GameSystem.h"
#include "PSGame/PSSe.h"
#include "PSSystem/PSMainSide_ObjSound.h"
#include "p2_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static_assert(PLAYERMODE_Olimar == NAVIID_Olimar && PLAYERMODE_Olimar == Game::CAMNAVI_Olimar, "player mode");
static_assert(PLAYERMODE_Louie == NAVIID_Louie && PLAYERMODE_Louie == Game::CAMNAVI_Louie, "player mode");
static_assert(PLAYERMODE_Multiplayer == NAVIID_Multiplayer && PLAYERMODE_Multiplayer == Game::CAMNAVI_Both,
              "player mode");
static_assert(P2_MAX_NAVI < P2_NO_NAVI_BUCKET_N, "no-navi bucket must not be a navi index");
static_assert(P2_MAX_NAVI <= 4, "pad ports");

// ---- pure rules ------------------------------------------------------------------------------------

int p2_navi_team_n(int idx, int n) { return n > 2 ? (idx & 1) : idx; }
int p2_navi_kind_n(int idx, int n) { return n > 2 ? (idx & 1) : idx; }
int p2_navi_other_n(int idx, int n)
{
	if (n <= 2) return 1 - idx;
	const int o = idx ^ 1;
	return o < n ? o : 0;
}
int p2_cam_id_n(int idx) { return idx < 2 ? idx : P2_CAMID_NAVI(idx); }
int p2_cam_range_n(int* range, int camID, int count)
{
	if (camID >= P2_CAMID_NAVI_BASE) {
		const int i = camID - P2_CAMID_NAVI_BASE;
		if (i < 2 || i >= count) return 0; // stock path: unknown camID, isStartAndEnd returns false
		range[0] = i;
		range[1] = i + 1;
		return 1;
	}
	if (camID == PLAYERMODE_Multiplayer && count > 2) { // CAMNAVI_Both
		range[0] = 0;
		range[1] = count;
		return 1;
	}
	return 0;
}
int p2_color_team(int kind) { return kind == 1 ? 0 : (kind == 0 ? 1 : -1); } // Game::Red, Game::Blue

// The navi with the smallest squared distance among the alive ones; ties go to the lower index.
// -1 when none is alive.
static int p2_pick_nearest(const float* d2, const bool* alive, int n)
{
	int best = -1;
	for (int i = 0; i < n; i++) {
		if (!alive[i]) continue;
		if (best < 0 || d2[i] < d2[best]) best = i;
	}
	return best;
}

// ---- live roster ------------------------------------------------------------------------------------

int p2_navi_count(void)
{
	Game::NaviMgr* mgr = Game::naviMgr;
	if (mgr == nullptr) return 2;
	const int max = mgr->getMax();
	return max > 2 ? (max > P2_MAX_NAVI ? P2_MAX_NAVI : max) : 2;
}

int p2_nplayer(void) { return p2_navi_count() > 2 ? 1 : 0; }

int p2_navi_alloc_count(void)
{
	if (Game::gameSystem == nullptr || !Game::gameSystem->isVersusMode() || p2_net_vs_stage() < 0) {
		return 2;
	}
	const int n = p2_net_vs_players();
	return (n >= 3 && n <= P2_MAX_NAVI) ? n : 2;
}

int p2_navi_team(int idx) { return p2_navi_team_n(idx, p2_navi_count()); }
int p2_navi_kind(int idx) { return p2_navi_kind_n(idx, p2_navi_count()); }
int p2_navi_other(int idx) { return p2_navi_other_n(idx, p2_navi_count()); }
int p2_cam_id(int idx) { return p2_cam_id_n(idx); }

void p2_navi_on_birth(Game::Navi* navi)
{
	if (navi == nullptr) return;
	navi->mP2Player = (u8)navi->mNaviIndex;
	navi->mP2Team   = (u8)p2_navi_team(navi->mNaviIndex);
}

void p2_navi_rappa_fix(Game::Navi* navi)
{
	// Rappa::init(kind) made captain >= 2 the lip-sync owner of its kind; captains 0 and 1 are born
	// first, and the audio track callbacks keep reading them.
	if (navi == nullptr || !p2_nplayer() || navi->mNaviIndex < 2) return;
	const int kind = p2_navi_kind(navi->mNaviIndex);
	Game::Navi* owner = Game::naviMgr->getAt(kind);
	if (owner != nullptr && owner->mSoundObj != nullptr) {
		PSGame::Rappa::sRappa[kind] = &owner->mSoundObj->mRappa;
	}
}

Game::Navi* p2_navi_alive_orima(int type)
{
	const int n = p2_navi_count();
	Game::Navi* active = nullptr;
	for (int i = 0; i < n; i++) {
		Game::Navi* navi = Game::naviMgr->getAt(i);
		if (navi != nullptr && navi->isAlive() && navi->mController1 != nullptr) {
			active = navi;
			break;
		}
	}
	if (active == nullptr) {
		for (int i = 0; i < n; i++) {
			Game::Navi* navi = Game::naviMgr->getAt(i);
			if (navi != nullptr && navi->isAlive()) {
				active = navi;
				break;
			}
		}
	}
	if (type == 0) return active; // ALIVEORIMA_Active
	if (active == nullptr) return nullptr;
	for (int i = 0; i < n; i++) { // ALIVEORIMA_Inactive: the first other alive captain
		Game::Navi* navi = Game::naviMgr->getAt(i);
		if (navi != nullptr && navi != active && navi->isAlive()) return navi;
	}
	return nullptr;
}

Game::Navi* p2_navi_sim_target(Game::Creature* from)
{
	if (!p2_nplayer() || from == nullptr) return Game::naviMgr->getActiveNavi();
	const int n = p2_navi_count();
	float d2[P2_MAX_NAVI];
	bool alive[P2_MAX_NAVI];
	Vector3f pos = from->getPosition();
	for (int i = 0; i < n; i++) {
		Game::Navi* navi = Game::naviMgr->getAt(i);
		alive[i]         = navi != nullptr && navi->isAlive();
		d2[i]            = 0.0f;
		if (alive[i]) {
			Vector3f np = navi->getPosition();
			d2[i]       = pos.sqrDistance(np);
		}
	}
	const int best = p2_pick_nearest(d2, alive, n);
	return best >= 0 ? Game::naviMgr->getAt(best) : nullptr;
}

// ---- selftest (MPPM_NPLAYER_SELFTEST=core), runs before main: libc only -------------------------------

namespace {

struct CoreSelftest {
	CoreSelftest()
	{
		const char* v = getenv("MPPM_NPLAYER_SELFTEST");
		if (v == nullptr || strcmp(v, "core") != 0) return;
		int fails = 0;
#define P2_CHECK(cond)                                                              \
	do {                                                                            \
		if (!(cond)) {                                                              \
			fprintf(stderr, "mppm: nplayer selftest core: failed: %s\n", #cond);    \
			fails++;                                                                \
		}                                                                           \
	} while (0)
		// team / kind / other, n = 2 (stock) and n = 4
		P2_CHECK(p2_navi_team_n(0, 2) == 0 && p2_navi_team_n(1, 2) == 1);
		P2_CHECK(p2_navi_kind_n(0, 2) == 0 && p2_navi_kind_n(1, 2) == 1);
		P2_CHECK(p2_navi_other_n(0, 2) == 1 && p2_navi_other_n(1, 2) == 0);
		const int team4[4] = { 0, 1, 0, 1 }, other4[4] = { 1, 0, 3, 2 };
		for (int i = 0; i < 4; i++) {
			P2_CHECK(p2_navi_team_n(i, 4) == team4[i]);
			P2_CHECK(p2_navi_kind_n(i, 4) == team4[i]);
			P2_CHECK(p2_navi_other_n(i, 4) == other4[i]);
		}
		P2_CHECK(p2_navi_other_n(2, 3) == 0); // idx ^ 1 = 3 out of range
		// camera ids
		P2_CHECK(p2_cam_id_n(0) == 0 && p2_cam_id_n(1) == 1);
		P2_CHECK(p2_cam_id_n(2) == 0x102 && p2_cam_id_n(3) == 0x103);
		// isStartAndEnd extension
		int r[2] = { -1, -1 };
		P2_CHECK(p2_cam_range_n(r, 2, 2) == 0);                                // Both, 2 cameras: stock
		P2_CHECK(p2_cam_range_n(r, 2, 4) == 1 && r[0] == 0 && r[1] == 4);      // Both, 4 cameras
		P2_CHECK(p2_cam_range_n(r, 0x102, 4) == 1 && r[0] == 2 && r[1] == 3);
		P2_CHECK(p2_cam_range_n(r, 0x103, 4) == 1 && r[0] == 3 && r[1] == 4);
		P2_CHECK(p2_cam_range_n(r, 0x104, 4) == 0);
		P2_CHECK(p2_cam_range_n(r, 0x100, 4) == 0 && p2_cam_range_n(r, 0x101, 4) == 0);
		P2_CHECK(p2_cam_range_n(r, 0x102, 2) == 0);
		P2_CHECK(p2_cam_range_n(r, 0, 4) == 0 && p2_cam_range_n(r, 1, 4) == 0); // stock path
		// colour to team
		P2_CHECK(p2_color_team(1) == 0 && p2_color_team(0) == 1);
		P2_CHECK(p2_color_team(2) == -1 && p2_color_team(3) == -1 && p2_color_team(-1) == -1);
		// nearest pick
		{
			const float d[4]  = { 5.0f, 3.0f, 3.0f, 1.0f };
			const bool a[4]   = { true, true, true, false };
			const bool n0[4]  = { false, false, false, false };
			const bool a2[4]  = { true, false, false, true };
			const float e[4]  = { 2.0f, 2.0f, 2.0f, 2.0f };
			P2_CHECK(p2_pick_nearest(d, a, 4) == 1);  // tie 1/2 -> lower index; 3 is dead
			P2_CHECK(p2_pick_nearest(d, n0, 4) == -1);
			P2_CHECK(p2_pick_nearest(d, a2, 4) == 3);
			P2_CHECK(p2_pick_nearest(e, a, 4) == 0);
			P2_CHECK(p2_pick_nearest(d, a, 1) == 0);
		}
#undef P2_CHECK
		printf("mppm: nplayer selftest core %s\n", fails == 0 ? "PASS" : "FAIL");
		fflush(stdout);
		fflush(stderr);
		_Exit(fails == 0 ? 0 : 1);
	}
};

CoreSelftest sCoreSelftest;

} // namespace
