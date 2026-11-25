// M5.3a part 1 (docs/ONLINE_MULTIPLAYER.md 3.1-3.6, design /var/tmp/mppm-online/m53a_design.md 6.1):
// 2P Battle rules for 2 teams of N/2 captains. Host-only file, built into the game library (not in
// the matching build).
//
// All VS state stays team-indexed ([2] arrays, lose causes, slot machines, handicaps). Game code
// reaches these helpers only through same-line PC_ONLY / PC_SWITCH edits, and every helper is the
// stock behaviour while the running section has at most 2 captains (p2_nplayer() == 0):
// - cards: the slot machine is per team; effects on "the user" act on the captain of that team who
//   last pressed Y, TEKI_ROCK / TEKI_BOMBOTAKRA land on the nearest alive enemy captain;
// - deaths: a captain going down while a teammate lives is released and play goes on; the last
//   captain of a team gives the team VSLOSE_NaviDown (the winner code reads lose causes per team);
// - enemies: EB_AttackingNavi1 ("hit once per tick" for captain 1) gets one bit per captain >= 2.
#include "Game/Navi.h"
#include "Game/VsGame.h"
#include "Game/VsGameSection.h"
#include "Game/EnemyBase.h"
#include "Game/GameSystem.h"
#include "Game/Entities/ItemOnyon.h"
#include "p2_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cstdlib>

namespace {

// Navi index of the captain of each team who last used the team's card (pressed Y); -1 = none.
int sUser[2] = { -1, -1 };

Game::Navi* navi_at(int idx)
{
	if (Game::naviMgr == nullptr || idx < 0 || idx >= p2_navi_count() || idx >= Game::naviMgr->getMax()) {
		return nullptr;
	}
	return Game::naviMgr->getAt(idx);
}

bool navi_alive(int idx)
{
	Game::Navi* navi = navi_at(idx);
	return navi != nullptr && navi->isAlive();
}

} // namespace

// ---- Pure rules (selftested below) ------------------------------------------------------------------

// What onOrimaDown does for N > 2 when captain `dying` goes down. `alive[dying]` is still true here
// (NaviDeadState::init calls gmOrimaDown before setAlive(false)), so it is skipped.
enum P2VsDownAction { P2VS_DOWN_NOTHING = 0, P2VS_DOWN_RELEASE = 1, P2VS_DOWN_TEAM_LOSES = 2 };

int p2_vs_down_action(const int* team, const bool* alive, int n, int dying, int subState)
{
	for (int j = 0; j < n; j++) {
		if (j != dying && team[j] == team[dying] && alive[j]) {
			return P2VS_DOWN_RELEASE;
		}
	}
	return subState ? P2VS_DOWN_NOTHING : P2VS_DOWN_TEAM_LOSES;
}

// The alive captain not on `myTeam` with the smallest d2; ties go to the lower index; -1 = none.
int p2_vs_pick_enemy(const float* d2, const bool* alive, const int* team, int n, int myTeam)
{
	int best = -1;
	for (int j = 0; j < n; j++) {
		if (!alive[j] || team[j] == myTeam) {
			continue;
		}
		if (best < 0 || d2[j] < d2[best]) {
			best = j;
		}
	}
	return best;
}

// Hit-once bits of captains >= 2 (EnemyBase::mP2NaviAttack, bit 1 << index).
bool p2_vs_attack_free(u8 bits, int idx) { return (bits & (1u << idx)) == 0; }
u8 p2_vs_attack_mark(u8 bits, int idx) { return (u8)(bits | (1u << idx)); }

// ---- Cards ------------------------------------------------------------------------------------------

int p2_vs_card_use(Game::Navi* navi)
{
	const int idx = navi->mNaviIndex;
	if (!p2_nplayer()) {
		return idx;
	}
	const int team = p2_navi_team(idx);
	sUser[team]    = idx;
	return team;
}

Game::Navi* p2_vs_card_navi(int team)
{
	if (p2_nplayer() && team >= 0 && team < 2) {
		if (navi_alive(sUser[team])) {
			return navi_at(sUser[team]);
		}
		// The user is down (stop-slot mode fires later) or unknown: the team's lowest live captain.
		for (int j = 0; j < p2_navi_count(); j++) {
			if (p2_navi_team(j) == team && navi_alive(j)) {
				return navi_at(j);
			}
		}
	}
	return Game::naviMgr->getAt(team);
}

int p2_vs_card_user(int team) { return (team >= 0 && team < 2) ? sUser[team] : -1; }

Game::Navi* p2_vs_card_enemy_navi(int team)
{
	if (!p2_nplayer() || team < 0 || team > 1) {
		return Game::naviMgr->getAt(1 - team);
	}
	// Reference point: the card user, else the team's own onyon (Pikmin colour 1 - team), else the origin.
	Vector3f from(0.0f, 0.0f, 0.0f);
	if (navi_alive(sUser[team])) {
		from = navi_at(sUser[team])->getPosition();
	} else if (Game::ItemOnyon::mgr) {
		Game::Onyon* onyon = Game::ItemOnyon::mgr->getOnyon(1 - team);
		if (onyon) {
			from = onyon->getPosition();
		}
	}
	float d2[P2_MAX_NAVI];
	bool alive[P2_MAX_NAVI];
	int teams[P2_MAX_NAVI];
	int n = p2_navi_count();
	if (n > P2_MAX_NAVI) {
		n = P2_MAX_NAVI;
	}
	for (int j = 0; j < n; j++) {
		Game::Navi* navi = navi_at(j);
		alive[j]         = navi != nullptr && navi->isAlive();
		teams[j]         = p2_navi_team(j);
		d2[j]            = 0.0f;
		if (navi) {
			const Vector3f pos = navi->getPosition();
			const float dx = pos.x - from.x, dy = pos.y - from.y, dz = pos.z - from.z;
			d2[j] = dx * dx + dy * dy + dz * dz;
		}
	}
	const int pick = p2_vs_pick_enemy(d2, alive, teams, n, team);
	return pick >= 0 ? navi_at(pick) : Game::naviMgr->getAt(1 - team);
}

void p2_vs_match_reset(void)
{
	sUser[0] = -1;
	sUser[1] = -1;
}

// ---- Deaths -----------------------------------------------------------------------------------------

int p2_vs_orima_down(Game::VsGame::GameState* state, Game::VsGameSection* section, int naviIdx)
{
	(void)section;
	if (!p2_nplayer() || Game::gameSystem == nullptr || !Game::gameSystem->isVersusMode()) {
		return 0; // stock path (N > 2 outside VS does not exist in part 1)
	}
	int n = p2_navi_count();
	if (n > P2_MAX_NAVI) {
		n = P2_MAX_NAVI;
	}
	if (naviIdx < 0 || naviIdx >= n) {
		return 0; // the stock bounds assert reports it
	}
	int teams[P2_MAX_NAVI];
	bool alive[P2_MAX_NAVI];
	for (int j = 0; j < n; j++) {
		teams[j] = p2_navi_team(j);
		alive[j] = navi_alive(j);
	}
	const int team = teams[naviIdx];
	switch (p2_vs_down_action(teams, alive, n, naviIdx, state->mSubState)) {
	case P2VS_DOWN_RELEASE:
		Game::naviMgr->informOrimaDead(naviIdx); // dismisses its Pikmin
		p2_piki_release_owner(naviIdx);
		break;
	case P2VS_DOWN_TEAM_LOSES:
		state->setLoseCause(team, Game::VsGame::VSLOSE_NaviDown);
		break;
	default:
		break;
	}
	return 1;
}

// ---- Match start and the ghost card -----------------------------------------------------------------

void p2_vs_init_sprays(Game::VsGameSection* section)
{
	if (!p2_nplayer() || section == nullptr || section->mVsStageData == nullptr) {
		return;
	}
	for (int i = 2; i < p2_navi_count(); i++) {
		Game::Navi* navi = navi_at(i);
		if (navi) {
			navi->mSprayCounts[0] = section->mVsStageData->mStartNumSpicy;
			navi->mSprayCounts[1] = section->mVsStageData->mStartNumBitter;
		}
	}
}

// Called from GameState::updateNavi while the team's ghost timer runs, after it was decremented. When
// it has just expired, the effects of the team's captains >= 2 come back as VsGame.h:351-370 restores
// captain `team`'s. Only captains whose effects the card saved (NAVIFX_IsSaved) are touched, so a
// captain the card did not hit does not get a second light.
void p2_vs_ghost_tick(Game::VsGameSection* section, int team)
{
	if (!p2_nplayer() || section == nullptr || team < 0 || team > 1 || section->mGhostIconTimers[team] > 0.0f) {
		return;
	}
	for (int i = 2; i < p2_navi_count(); i++) {
		Game::Navi* navi = navi_at(i);
		if (navi == nullptr || p2_navi_team(i) != team || navi->mEffectsObj == nullptr) {
			continue;
		}
		efx::TNaviEffect* naviEffect = navi->mEffectsObj;
		if (!naviEffect->isFlag(efx::NAVIFX_IsSaved)) {
			continue;
		}
		naviEffect->mFlags = naviEffect->mSavedFlags;
		naviEffect->resetFlag(efx::NAVIFX_IsSaved);

		bool inWater = naviEffect->isFlag(efx::NAVIFX_InWater);
		if (naviEffect->isFlag(efx::NAVIFX_InWater)) {
			naviEffect->setFlag(efx::NAVIFX_InWater);
			naviEffect->updateHamon_();
			if (!inWater) {
				efx::createSimpleDive(naviEffect->mHamonPosition);
			}
		}
		if (naviEffect->isFlag(efx::NAVIFX_LightOn)) {
			naviEffect->createLight();
		}
	}
}

// ---- Enemy "attacked by captain this tick" bookkeeping -----------------------------------------------

int p2_enemy_navi_free(Game::EnemyBase* enemy, Game::Navi* navi)
{
	const int idx = navi->mNaviIndex;
	if (!p2_nplayer() || idx < 2 || idx >= 8) {
		return !enemy->isEvent(0, Game::EB_AttackingNavi1);
	}
	return p2_vs_attack_free(enemy->mP2NaviAttack, idx) ? 1 : 0;
}

void p2_enemy_navi_mark(Game::EnemyBase* enemy, Game::Navi* navi)
{
	const int idx = navi->mNaviIndex;
	if (!p2_nplayer() || idx < 2 || idx >= 8) {
		enemy->enableEvent(0, Game::EB_AttackingNavi1);
		return;
	}
	enemy->mP2NaviAttack = p2_vs_attack_mark(enemy->mP2NaviAttack, idx);
}

void p2_enemy_navi_clear(Game::EnemyBase* enemy) { enemy->mP2NaviAttack = 0; }

// ---- Selftest: MPPM_NPLAYER_SELFTEST=vs (runs before main; libc only) ----------------------------------

namespace {

int sFails = 0;

void expect(bool ok, const char* what)
{
	if (!ok) {
		fprintf(stderr, "mppm: nplayer selftest vs: FAILED %s\n", what);
		sFails++;
	}
}

void selftest_vs()
{
	// Team-down decision table, 4 captains, teams i & 1.
	const int t4[4] = { 0, 1, 0, 1 };
	{
		const bool a[4] = { true, true, true, true };
		expect(p2_vs_down_action(t4, a, 4, 0, 0) == P2VS_DOWN_RELEASE, "0 down, 2 alive -> release");
		expect(p2_vs_down_action(t4, a, 4, 2, 0) == P2VS_DOWN_RELEASE, "2 down, 0 alive -> release");
		expect(p2_vs_down_action(t4, a, 4, 3, 1) == P2VS_DOWN_RELEASE, "release ignores the sub-state");
	}
	{
		const bool a[4] = { true, true, false, true }; // 2 already down
		expect(p2_vs_down_action(t4, a, 4, 0, 0) == P2VS_DOWN_TEAM_LOSES, "0 down, 2 down -> Red loses");
		expect(p2_vs_down_action(t4, a, 4, 0, 1) == P2VS_DOWN_NOTHING, "team down in a sub-state -> nothing");
		expect(p2_vs_down_action(t4, a, 4, 1, 0) == P2VS_DOWN_RELEASE, "1 down, 3 alive -> release");
	}
	{
		const bool a[4] = { false, true, true, false };
		expect(p2_vs_down_action(t4, a, 4, 1, 0) == P2VS_DOWN_TEAM_LOSES, "1 down, 3 down -> Blue loses");
		expect(p2_vs_down_action(t4, a, 4, 2, 0) == P2VS_DOWN_TEAM_LOSES, "2 down, 0 down -> Red loses");
	}
	{
		// Two captains of a team going down in one tick: the first sees the second alive.
		bool a[4] = { true, true, true, true };
		expect(p2_vs_down_action(t4, a, 4, 1, 0) == P2VS_DOWN_RELEASE, "same tick, first");
		a[1] = false;
		expect(p2_vs_down_action(t4, a, 4, 3, 0) == P2VS_DOWN_TEAM_LOSES, "same tick, second");
	}
	{
		// N = 2 table for reference: a captain alone on its team.
		const int t2[2]   = { 0, 1 };
		const bool a2[2]  = { true, true };
		expect(p2_vs_down_action(t2, a2, 2, 0, 0) == P2VS_DOWN_TEAM_LOSES, "n=2 0 down -> Red loses");
	}

	// Enemy pick.
	{
		const bool a[4]   = { true, true, true, true };
		const float d[4]  = { 0.0f, 50.0f, 1.0f, 50.0f };
		expect(p2_vs_pick_enemy(d, a, t4, 4, 0) == 1, "tie -> lower index (1)");
		expect(p2_vs_pick_enemy(d, a, t4, 4, 1) == 0, "Blue picks Red 0");
		const float d2[4] = { 9.0f, 70.0f, 4.0f, 60.0f };
		expect(p2_vs_pick_enemy(d2, a, t4, 4, 0) == 3, "nearest Blue (3)");
		expect(p2_vs_pick_enemy(d2, a, t4, 4, 1) == 2, "nearest Red (2)");
		const float d3[4] = { 4.0f, 70.0f, 4.0f, 60.0f };
		expect(p2_vs_pick_enemy(d3, a, t4, 4, 1) == 0, "Red tie -> 0");
	}
	{
		const bool a[4]  = { true, true, true, false };
		const float d[4] = { 0.0f, 50.0f, 1.0f, 10.0f };
		expect(p2_vs_pick_enemy(d, a, t4, 4, 0) == 1, "dead 3 skipped");
		const bool none[4] = { true, false, true, false };
		expect(p2_vs_pick_enemy(d, none, t4, 4, 0) == -1, "no enemy alive -> -1");
	}

	// Attack bits.
	{
		u8 bits = 0;
		expect(p2_vs_attack_free(bits, 2) && p2_vs_attack_free(bits, 3), "fresh bits free");
		bits = p2_vs_attack_mark(bits, 2);
		expect(bits == 0x04, "mark 2 -> 0x04");
		expect(!p2_vs_attack_free(bits, 2) && p2_vs_attack_free(bits, 3), "2 marked, 3 free");
		bits = p2_vs_attack_mark(bits, 3);
		expect(bits == 0x0C, "mark 3 -> 0x0C");
		bits = p2_vs_attack_mark(bits, 3);
		expect(bits == 0x0C, "mark is idempotent");
		expect(p2_vs_attack_free(bits, 1) && p2_vs_attack_free(bits, 0), "bits 0/1 never used");
	}
}

struct SelftestVs {
	SelftestVs()
	{
		const char* v = getenv("MPPM_NPLAYER_SELFTEST");
		if (v == nullptr || strcmp(v, "vs") != 0) {
			return;
		}
		selftest_vs();
		printf("mppm: nplayer selftest vs %s\n", sFails ? "FAIL" : "PASS");
		fflush(stdout);
		fflush(stderr);
		std::_Exit(sFails ? 1 : 0);
	}
} sSelftestVs;

} // namespace
