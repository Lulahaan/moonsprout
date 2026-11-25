/* M5.3a part 1: N captains (docs/ONLINE_MULTIPLAYER.md 3.1-3.6). Design: /var/tmp/mppm-online/m53a_design.md.
 *
 * Included by p2_host.h, so every C++ game file sees it through types.h. Game code calls these only
 * from same-line PC_ONLY / PC_SWITCH edits. Rule for every function: while the running section has
 * at most 2 captains (p2_nplayer() == 0) it returns the stock value or does nothing, so story,
 * Challenge and 2P Battle with 1-2 captains are unchanged. More than 2 captains exist only in a
 * 2P Battle harness session with --vs-players 4 (or a version 4 replay).
 *
 * Roster of an N-captain section (N = 4 in part 1): navi index i = player slot i = pad port i;
 * team = i & 1 (0 = Red onyon, Olimar side; 1 = Blue onyon, Louie side); kind (model, voice,
 * effects, cursor) = team. A team's Pikmin colour is 1 - team (Blue = 0, Red = 1).
 *
 * Owners: [P1] port/game/navi/navi_core.cpp, navi_section.cpp; [P2] port/game/navi/piki_team.cpp;
 * [P3] port/game/navi/vs_teams.cpp; [P4] port/game/navi/view.cpp.
 */
#ifndef P2_NAVI_H
#define P2_NAVI_H
#ifdef __cplusplus

#define P2_MAX_NAVI         4     // captains a section can hold (pad ports 0..3)
#define P2_CAMID_NAVI_BASE  0x100 // CameraMgr camID "only navi i" for i >= 2 (0, 1, 2 keep their CAMNAVI_* meaning)
#define P2_CAMID_NAVI(i)    (P2_CAMID_NAVI_BASE | (i))
#define P2_PIKI_NO_OWNER    0xFF  // Piki::mP2Owner of a Pikmin no captain owns
#define P2_NO_NAVI_BUCKET_N 6     // PikiNaviCounter bucket for "no navi" when N > 2 (bucket 2 otherwise)

// setPlayerMode / mPrevNaviIdx / CameraMgr mode values; never navi indices (see p2_cam_id).
enum P2PlayerMode { PLAYERMODE_Olimar = 0, PLAYERMODE_Louie = 1, PLAYERMODE_Multiplayer = 2 };

struct Camera;
struct Graphics;
struct Viewport;
struct Controller;
namespace Sys {
struct Sphere;
struct Cylinder;
} // namespace Sys
namespace Game {
struct Creature;
struct Navi;
struct Piki;
struct EnemyBase;
struct BaseGameSection;
struct VsGameSection;
struct CameraMgr;
struct PlayCamera;
struct AILODParm;
namespace VsGame {
struct GameState;
} // namespace VsGame
} // namespace Game

// ---- [P1] roster rules: port/game/navi/navi_core.cpp ------------------------------------------------
int p2_navi_team_n(int idx, int n);   // n <= 2: idx; n > 2: idx & 1
int p2_navi_kind_n(int idx, int n);   // n <= 2: idx; n > 2: idx & 1
int p2_navi_other_n(int idx, int n);  // n <= 2: 1 - idx; n > 2: idx ^ 1 when < n, else 0
int p2_cam_id_n(int idx);             // idx < 2: idx; else P2_CAMID_NAVI(idx)
int p2_cam_range_n(int* range, int camID, int count); // isStartAndEnd extension: 1 = range set, 0 = stock path
int p2_color_team(int pikiKind);      // Red (1) -> 0, Blue (0) -> 1, other colours -> -1
int p2_navi_count(void);              // captains of the running section: naviMgr->getMax() when > 2, else 2
int p2_nplayer(void);                 // p2_navi_count() > 2
int p2_navi_alloc_count(void);        // setupFloatMemory: p2_net_vs_players() in a 2P Battle harness session, else 2
int p2_navi_team(int idx);            // the _n functions with n = p2_navi_count()
int p2_navi_kind(int idx);
int p2_navi_other(int idx);
int p2_cam_id(int idx);
void p2_navi_on_birth(Game::Navi* navi);   // NaviMgr::birth: mP2Player = index, mP2Team = p2_navi_team(index)
void p2_navi_rappa_fix(Game::Navi* navi);  // after PSM::Navi::init(kind): index >= 2 hands sRappa[kind] back to captain kind
Game::Navi* p2_navi_alive_orima(int type); // NaviMgr::getAliveOrima for N > 2
Game::Navi* p2_navi_sim_target(Game::Creature* from); // N <= 2: naviMgr->getActiveNavi(); N > 2: nearest alive navi
// ---- [P1] section roster: port/game/navi/navi_section.cpp --------------------------------------------
void p2_nplayer_setup(Game::BaseGameSection* section);   // setupFloatMemory: controllers + cameras of navis 2..N-1
void p2_cam_attach(Game::BaseGameSection* section);      // initViewports: simulation viewports, CameraMgr lists
void p2_nplayer_spawn(Game::BaseGameSection* section);   // initGenerators case 0: birth and place navis 2..N-1
void p2_nplayer_bind(Game::BaseGameSection* section, int playerMode); // end of setCamController
void p2_nplayer_fix_near_far(bool isFixed, float nearZ, float farZ);
void p2_nplayer_del_cameras(void);                       // clearHeap, beside mLouieCamera->del()
void p2_nplayer_clear(void);                             // clearHeap, after the P1/P2 controllers are deleted
Controller* p2_nplayer_controller(int idx);              // 0, 1: mControllerP1/P2; 2..N-1: extra; else nullptr
Game::PlayCamera* p2_nplayer_camera(int idx);            // 0, 1: mOlimarCamera/mLouieCamera; 2..N-1: extra; else nullptr
int p2_cam_count(void);                                  // CameraMgr array size: p2_navi_count()
int p2_cam_range(int* range, int camID);                 // p2_cam_range_n(range, camID, p2_cam_count())
void p2_cam_update_extra(Game::CameraMgr* mgr);          // single-camera mode: steps cameras 2..N-1
int p2_cam_extra_change_state(Game::CameraMgr* mgr);     // OR of mChangePlayerState of cameras 2..N-1
void p2_cam_multi_extra(Game::CameraMgr* mgr);           // changePlayerMode(Multiplayer) for cameras 2..N-1
// ---- [P2] Pikmin teams and owners: port/game/navi/piki_team.cpp --------------------------------------
int p2_vs_enemy_piki(Game::Piki* piki, Game::Navi* navi); // (Red && navi team 1) || (Blue && navi team 0)
void p2_piki_set_owner(Game::Piki* piki, Game::Navi* navi);
int p2_piki_touch_ok(Game::Piki* piki, Game::Navi* navi); // N <= 2: 1
Game::Navi* p2_piki_search_orima(Game::Piki* piki);       // Brain::searchOrima for N > 2
int p2_piki_free_bucket(void);                            // 2, or P2_NO_NAVI_BUCKET_N when N > 2
void p2_piki_release_owner(int naviIdx);                  // owners naviIdx -> P2_PIKI_NO_OWNER
// ---- [P3] VS teams: port/game/navi/vs_teams.cpp --------------------------------------------------------
int p2_vs_card_use(Game::Navi* navi);               // remembers the card user of its team; returns the slot (team)
Game::Navi* p2_vs_card_navi(int team);              // the captain a card acts on (stock: naviMgr->getAt(team))
int p2_vs_card_user(int team);                       // last card user of the team (navi index), -1 = none; checksum
Game::Navi* p2_vs_card_enemy_navi(int team);        // the enemy captain a card targets (stock: getAt(1 - team))
void p2_vs_match_reset(void);                       // GameState::clearLoseCauses
int p2_vs_orima_down(Game::VsGame::GameState* state, Game::VsGameSection* section, int naviIdx); // 1 = handled
void p2_vs_init_sprays(Game::VsGameSection* section);
void p2_vs_ghost_tick(Game::VsGameSection* section, int team);
int p2_enemy_navi_free(Game::EnemyBase* enemy, Game::Navi* navi); // stock: !isEvent(0, EB_AttackingNavi1)
void p2_enemy_navi_mark(Game::EnemyBase* enemy, Game::Navi* navi); // stock: enableEvent(0, EB_AttackingNavi1)
void p2_enemy_navi_clear(Game::EnemyBase* enemy);
// ---- [P4] observers and presentation: port/game/navi/view.cpp -------------------------------------------
int p2_lod_update(Game::Creature* creature, Game::AILODParm& parm, Sys::Sphere& sphere, Sys::Cylinder& cylinder); // 1 = done
float p2_cam_sim_aspect(Camera* camera, float stockSimAspect);
void p2_nplayer_present_layout(Game::BaseGameSection* section);
void p2_nplayer_draw_markers(Graphics& gfx);
unsigned p2_vs_ghost_vpbits(int team); // stock: team 1 -> AILOD_IsVisVP0, team 0 -> AILOD_IsVisVP1

#endif /* __cplusplus */
#endif /* P2_NAVI_H */
