// M5.3a part 1 [P2]: Pikmin teams and owners (docs/ONLINE_MULTIPLAYER.md 3.1, design section 5).
// Host-only file, built into the game library (not in the matching build).
//
// Two teams = two colours: team t's Pikmin are colour 1 - t (Blue = 0, Red = 1). Piki::mP2Owner is
// the navi index of the last captain that plucked or called the Pikmin (P2_PIKI_NO_OWNER = none).
// Every function returns the stock value or does nothing while the section has at most 2 captains
// (p2_nplayer() == 0); the owner byte is always written, but only read for N > 2.
#include "Game/Navi.h"
#include "Game/Piki.h"
#include "Game/PikiMgr.h"
#include "p2_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

// ---- Pure rules (selftested below) ------------------------------------------------------------------

// The VS "enemy colour" test of vsFlute / InteractFue::actPiki, by team instead of navi index.
int enemy_rule(int kind, int team) { return (kind == Game::Red && team == 1) || (kind == Game::Blue && team == 0); }

// ActFree::collisionCallback touch-join: N <= 2 always; else owner none, this captain, or a dead one.
int touch_rule(int nplayer, int owner, int naviIdx, int ownerAlive)
{
	if (!nplayer) return 1;
	return owner == P2_PIKI_NO_OWNER || owner == naviIdx || !ownerAlive;
}

int bucket_rule(int n) { return n > 2 ? P2_NO_NAVI_BUCKET_N : 2; }

// Brain::searchOrima for N > 2: candidates are alive navis of `team` with dist < radius. The owner wins
// if it is a candidate, else the nearest; ties go to the lower index. -1 when there is no candidate.
int search_pick(int team, int owner, const float* dist, const int* naviTeam, const bool* alive, int n, float radius)
{
	if (team < 0) return -1;
	int best     = -1;
	float bestD  = radius;
	for (int i = 0; i < n; i++) {
		if (!alive[i] || naviTeam[i] != team || !(dist[i] < radius)) continue;
		if (i == owner) return i;
		if (dist[i] < bestD) {
			bestD = dist[i];
			best  = i;
		}
	}
	return best;
}

// ---- Selftest: MPPM_NPLAYER_SELFTEST=piki ----------------------------------------------------------

struct PikiSelftest {
	PikiSelftest()
	{
		const char* v = getenv("MPPM_NPLAYER_SELFTEST");
		if (!v || strcmp(v, "piki") != 0) return;
		int fails = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			fprintf(stderr, "mppm: nplayer selftest piki: failed %s:%d: %s\n", __FILE__, __LINE__, #c); \
			fails++;                                                                 \
		}                                                                            \
	} while (0)
		const int NO = P2_PIKI_NO_OWNER;
		// touch table: N <= 2 always allowed
		CHECK(touch_rule(0, NO, 0, 1) == 1);
		CHECK(touch_rule(0, 1, 0, 1) == 1);
		CHECK(touch_rule(0, 2, 3, 1) == 1);
		// N > 2
		CHECK(touch_rule(1, NO, 0, 1) == 1); // no owner
		CHECK(touch_rule(1, 2, 2, 1) == 1);  // own Pikmin
		CHECK(touch_rule(1, 0, 2, 1) == 0);  // teammate's Pikmin, teammate alive
		CHECK(touch_rule(1, 0, 2, 0) == 1);  // teammate down
		CHECK(touch_rule(1, 3, 1, 1) == 0);
		CHECK(touch_rule(1, 3, 1, 0) == 1);
		// enemy table: kinds 0..2, teams 0/1
		CHECK(enemy_rule(Game::Blue, 0) == 1);
		CHECK(enemy_rule(Game::Blue, 1) == 0);
		CHECK(enemy_rule(Game::Red, 0) == 0);
		CHECK(enemy_rule(Game::Red, 1) == 1);
		CHECK(enemy_rule(Game::Yellow, 0) == 0);
		CHECK(enemy_rule(Game::Yellow, 1) == 0);
		// enemy rule with team = navi index equals the stock navi-index expression for N <= 2
		for (int kind = 0; kind <= 2; kind++) {
			for (int idx = 0; idx <= 1; idx++) {
				const int stock = (kind == Game::Red && idx == NAVIID_Louie) || (kind == Game::Blue && idx == NAVIID_Olimar);
				CHECK(enemy_rule(kind, p2_navi_team_n(idx, 2)) == stock);
			}
		}
		// buckets
		CHECK(bucket_rule(2) == 2);
		CHECK(bucket_rule(4) == P2_NO_NAVI_BUCKET_N);
		CHECK(P2_NO_NAVI_BUCKET_N >= P2_MAX_NAVI);
		// search pick: navis 0..3, teams 0,1,0,1
		const int team4[4]   = { 0, 1, 0, 1 };
		const bool allA[4]   = { true, true, true, true };
		{ // owner in range wins over a nearer teammate
			const float d[4] = { 50.0f, 10.0f, 200.0f, 10.0f };
			CHECK(search_pick(0, 2, d, team4, allA, 4, 300.0f) == 2);
		}
		{ // no owner: nearest of the team
			const float d[4] = { 120.0f, 10.0f, 80.0f, 10.0f };
			CHECK(search_pick(0, NO, d, team4, allA, 4, 300.0f) == 2);
		}
		{ // owner out of range: nearest teammate in range
			const float d[4] = { 120.0f, 10.0f, 300.0f, 10.0f };
			CHECK(search_pick(0, 2, d, team4, allA, 4, 300.0f) == 0);
		}
		{ // owner dead: nearest alive teammate
			const float d[4]    = { 120.0f, 10.0f, 50.0f, 10.0f };
			const bool alive[4] = { true, true, false, true };
			CHECK(search_pick(0, 2, d, team4, alive, 4, 300.0f) == 0);
		}
		{ // tie goes to the lower index
			const float d[4] = { 80.0f, 10.0f, 80.0f, 10.0f };
			CHECK(search_pick(0, NO, d, team4, allA, 4, 300.0f) == 0);
			const float e[4] = { 10.0f, 40.0f, 10.0f, 40.0f };
			CHECK(search_pick(1, NO, e, team4, allA, 4, 300.0f) == 1);
		}
		{ // everything out of range
			const float d[4] = { 300.0f, 10.0f, 999.0f, 10.0f };
			CHECK(search_pick(0, NO, d, team4, allA, 4, 300.0f) == -1);
		}
		{ // owner on the wrong team is ignored; only enemies in range -> none
			const float d[4] = { 400.0f, 10.0f, 400.0f, 20.0f };
			CHECK(search_pick(0, 1, d, team4, allA, 4, 300.0f) == -1);
			CHECK(search_pick(0, 3, d, team4, allA, 4, 300.0f) == -1);
		}
		{ // team -1 (a colour no captain has)
			const float d[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			CHECK(search_pick(-1, NO, d, team4, allA, 4, 300.0f) == -1);
		}
		// colour/team mapping used for mP2Team: team t's colour is 1 - t
		CHECK(p2_color_team(Game::Red) == 0);
		CHECK(p2_color_team(Game::Blue) == 1);
		CHECK(p2_color_team(Game::Yellow) == -1);
#undef CHECK
		fprintf(stderr, "mppm: nplayer selftest piki %s\n", fails ? "FAIL" : "PASS");
		fflush(stderr);
		std::_Exit(fails ? 1 : 0);
	}
};
PikiSelftest sPikiSelftest;

} // namespace

// ---- Live part --------------------------------------------------------------------------------------

int p2_vs_enemy_piki(Game::Piki* piki, Game::Navi* navi)
{
	return enemy_rule(piki->getKind(), p2_navi_team(navi->mNaviIndex));
}

void p2_piki_set_owner(Game::Piki* piki, Game::Navi* navi)
{
	piki->mP2Owner = navi ? (u8)navi->mNaviIndex : (u8)P2_PIKI_NO_OWNER;
}

int p2_piki_touch_ok(Game::Piki* piki, Game::Navi* navi)
{
	if (!p2_nplayer()) return 1;
	const int owner  = piki->mP2Owner;
	int ownerAlive   = 0;
	if (owner != P2_PIKI_NO_OWNER && owner < p2_navi_count() && Game::naviMgr) {
		Game::Navi* o = Game::naviMgr->getAt(owner);
		ownerAlive    = o && o->isAlive();
	}
	return touch_rule(1, owner, navi->mNaviIndex, ownerAlive);
}

Game::Navi* p2_piki_search_orima(Game::Piki* piki)
{
	if (!Game::naviMgr) return nullptr;
	int n = p2_navi_count();
	if (n > P2_MAX_NAVI) n = P2_MAX_NAVI;
	float dist[P2_MAX_NAVI];
	int team[P2_MAX_NAVI];
	bool alive[P2_MAX_NAVI];
	Vector3f pos = piki->getPosition();
	for (int i = 0; i < n; i++) {
		Game::Navi* navi = Game::naviMgr->getAt(i);
		alive[i]         = navi && navi->isAlive();
		team[i]          = p2_navi_team(i);
		dist[i]          = 1.0e30f;
		if (alive[i]) {
			Vector3f naviPos = navi->getPosition();
			dist[i]          = pos.distance(naviPos);
		}
	}
	const int pick = search_pick(p2_color_team(piki->getKind()), piki->mP2Owner, dist, team, alive, n, 300.0f);
	return pick >= 0 ? Game::naviMgr->getAt(pick) : nullptr;
}

int p2_piki_free_bucket(void) { return bucket_rule(p2_navi_count()); }

void p2_piki_release_owner(int naviIdx)
{
	if (!p2_nplayer() || !Game::pikiMgr) return;
	Iterator<Game::Piki> it(Game::pikiMgr);
	CI_LOOP(it)
	{
		Game::Piki* piki = *it;
		if (piki->mP2Owner == (u8)naviIdx) piki->mP2Owner = P2_PIKI_NO_OWNER;
	}
}
