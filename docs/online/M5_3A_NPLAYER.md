# M5.3a part 1: N captains

Milestone M5.3a of [ONLINE_MULTIPLAYER.md](../ONLINE_MULTIPLAYER.md) (section 8, the first "Step 3"
row; design in sections 3.1 to 3.7 and 2.5) makes the number of captains a property of the section
instead of the constant 2. Part 1 ends with a **headless 4-captain 2v2 Battle**: captains 0 and 2 on
the Red onyon, 1 and 3 on the Blue onyon, driven by a synthetic 4-port input log, deterministic
across replays and across presentation layouts. Part 2 (the 4-captain co-op cave dive, 3 teams, HUD
for N, local 4-way split) is listed at the end.

With 1 or 2 captains (story, Challenge, 2P Battle from the menus) nothing changes: every new helper
returns the stock value or does nothing while the running section has at most 2 captains, the
recordings stay version 3 byte for byte, and the golden checksum logs of `desync_ci.sh`
(`tools/net/golden`) still match. More than 2 captains exist only in a 2P Battle harness session
started with `--vs-stage S --vs-players 4`, or in the replay of a version 4 log.

The harness itself (sessions, ticks, file formats, the matrix) is described in
[M5_1A_HARNESS.md](M5_1A_HARNESS.md); every game-side hook line, M5.3a included, in
[M5_1B_HOOKS.md](M5_1B_HOOKS.md).

## Rules

| # | Topic | Rule |
|---|---|---|
| D1 | Identity | `mNaviIndex` = player slot = pad port = index in `naviMgr` (0 to N-1). Team = `i & 1` for N > 2 (0 = Red onyon, Olimar side; 1 = Blue onyon, Louie side), = `i` for N <= 2. Kind (model, voice, effects, cursor, scale) = team. A team's Pikmin colour is `1 - team` (Blue = 0, Red = 1). |
| D2 | Navi fields | `u8 mP2Player, mP2Team` on `Navi`, written in `NaviMgr::birth`. Helpers take the index (`p2_navi_team(idx)`), because many sites only have an index. |
| D3 | Piki bytes | `u8 mP2Owner` (navi index, `0xFF` none) and `s8 mP2Team` (from the colour) on `Piki`. Part 1 reads only the owner; the team stays the colour (2 teams = 2 colours). |
| D4 | Owners (N > 2) | Set on every formation join (`ActFormation::init`: whistle and touch joins, and the delayed join of a Pikmin whistled while flicked, blown away, knocked down, doped or drowning), on pluck, noisemaker pluck and rejoin. A touch joins only if the owner is none, this captain or a dead captain. Rejoin after an action (`searchOrima`) prefers the owner, else the nearest live teammate within 300 units. A whistle is unrestricted within the team (a handoff). A captain going down clears its owners. |
| D5 | Other navi | `GET_OTHER_NAVI` and `getAt(1 - idx)` become `idx ^ 1` (bounded) for N > 2; equal to `1 - idx` for 0 and 1. |
| D6 | Player mode | `PLAYERMODE_Olimar/Louie/Multiplayer` (0, 1, 2) are modes, never navi indices. A navi index passed as a `CameraMgr` camID goes through `p2_cam_id()`: index >= 2 becomes `0x100 \| i`, decoded in `isStartAndEnd` (`CAMNAVI_Both` = 2 would otherwise mean captain 2). |
| D7 | Cameras | One `PlayCamera` per captain; `CameraMgr` arrays sized N; `CAMNAVI_Both` means 0 to N-1. For N > 2 every camera gets a hidden full-screen simulation viewport in `CameraMgr::mViewportList`, independent of the presented viewports. |
| D8 | Observer LOD | For N > 2 `Creature::updateLOD` computes `AILOD_IsVisible`, `IsMid` and `IsFar` from the N simulation cameras; the viewport bits `IsVisVP0/1` come from the presented viewports and are read only by draw code. N <= 2 keeps the stock loop. |
| D9 | Sim aspect | For N > 2 every play camera's `mSimAspect` is the constant of the stock 2P split viewport (640 / 240 times sim k, 9.6 at k 3.6), whatever viewport shows it. |
| D10 | Presentation | Default: the stock 2-way split of captains 0 and 1. `--view-captain K`: one full-screen viewport showing camera K, re-applied before the tick's view calculation and at the start of the draw, skipped while a movie plays. Markers over each captain when N > 2: team 0 a plain vermillion square, team 1 a blue square in a white frame (Okabe-Ito colours plus a shape per team, plan 2.5), 1 to 4 white dots for the captain, grey (same shape) when down. |
| D11 | VS state | The `[2]` VS arrays stay team-indexed (onyon type and colour already are the team). Navi-indexed inputs into VS state (`GameMessageVsGetDoping`, `GameMessageVsUseCard`) pass the team; card effects on "the user" act on the captain who pressed Y, or on the team's lowest live captain if that one is down. |
| D12 | Death and winner | For N > 2, `onOrimaDown(i)`: if a teammate is alive, the captain is released (`informOrimaDead`, Pikmin dismissed, owners cleared) and play goes on; when the last captain of a team goes down, the team gets `VSLOSE_NaviDown`. The winner code reads lose causes per team. |
| D13 | getActiveNavi | In VS it returns captain 0 for N = 2 and N = 4 alike. The 12 enemy simulation sites call `p2_navi_sim_target(this)` (stock for N <= 2, the nearest live captain for N > 2, ties to the lower index); story, Challenge and presentation sites are untouched in part 1. |
| D14 | Log format | `.mpnl` version 4 only when N > 2 (version 3 plus byte 33 = captains); 1-2 captain recordings stay version 3. The engine reads 2, 3 and 4. `# players=N` in the checksum log only when N > 2. |
| D15 | Pikmin cap | `MAX_PIKI_COUNT` stays 100; the performance is measured at 100. |
| D16 | Teams in part 1 | Only N = 2 or 4 (`--vs-players 3` is refused, exit 2); teams fixed to `i & 1`. |

## Flags

| Flag | Environment | Effect |
|---|---|---|
| `--vs-players N` | `MPPM_VS_PLAYERS` | Captains of a `--vs-stage` session: 2 (default) or 4. Anything else prints `mppm: net: bad --vs-players '<v>' (2 or 4 captains; 3 teams are not supported yet); exiting 2` and exits 2. Without a VS stage the count is 2. A loaded replay takes the count from its header (version 4 byte 33, older versions 2); a flag that differs prints one warning (`--vs-players N differs from the replay header's M captains; using the header`). |
| `--view-captain K` | `MPPM_VIEW_CAPTAIN` | Full-screen view of captain K (0 to 3) instead of the stock split. Presentation only and never recorded: a replay under any layout gives the same checksum log. Used only when the section has more than 2 captains. A bad value prints `bad --view-captain '<v>' (0 to 3); ignored`. |

With 4 captains the start-up line ends in `, players=4)`, the session start line in ` players=4`,
the record header is version 4 and the checksum log has `# players=4` after `# build=` (and after
`# mods=` when mods are on). With 2 captains none of these appear.

The seam API (`port/include/p2_net.h`, block "M5.3a"):

```c
int p2_net_vs_players(void);      /* 2 or 4; header > flag > 2; 2 without a VS stage */
int p2_net_view_captain(void);    /* -1 = stock split, 0..3 */
void p2_net_perf_sim_begin(void); /* MPPM_PERF=1: begin/end of BaseGameSection::doUpdate */
void p2_net_perf_sim_end(void);
```

## Environment aids

| Environment | Effect |
|---|---|
| `MPPM_NPLAYER_STATS=1` | Read-only. Once per section `mppm: nplayer: captains=<N> heap free=<bytes>`; every 1800 session ticks one line per captain: `mppm: nplayer: tick=<T> captain=<i> team=<t> alive=<0\|1> walked=<int> squad=<n> squadmax=<n> x=<int> z=<int>` (walked: XZ distance since the session start; squadmax: largest squad so far). |
| `MPPM_PERF=1` | Read-only. Times every session tick's simulation (steady clock) and prints at the session end `mppm: net: perf ticks=<n> sim avg=<ms> p50=<ms> p99=<ms> max=<ms> ms, pass avg=<ms> ms`. `pass` = from one tick's simulation start to the next one's (update, draw-command building, pacing); passes that held a load barrier are left out. |
| `MPPM_VS_HANDICAP=N` | Perf aid, N = 1 to 10: both VS handicaps set to N in the direct VS boot (10 gives 50 + 50 = 100 Pikmin). Prints `mppm: net: perf aid: VS handicap <n> (checksums differ from a run without it)`. |
| `MPPM_NPLAYER_SELFTEST=core\|piki\|vs\|view` | Runs the pure-table self-test of one package before `main`, prints `mppm: nplayer selftest <part> PASS` or `FAIL` and exits 0 or 1. |

## Input log version 4

Version 4 is the version 3 layout ([M5_1A_HARNESS.md](M5_1A_HARNESS.md#input-log-mpnl-binary-little-endian))
with one more field:

| Offset | Size | Field |
|---|---|---|
| 4 | 2 | version = 4 |
| 33 | 1 | captains = 4. 0 and 2 are read as 2; any other value fails the start-up (exit 2) |

Everything else (header size 144 to 1152, the hashes, the mod list, 48-byte records of 4 slots) is
as in version 3; a 4-captain log fills all four slots, port i drives captain i. The engine writes
version 4 only when `p2_net_vs_players()` is above 2, so 1-2 captain recordings stay version 3 byte
for byte, and an engine before M5.3a refuses a 4-captain log ("unsupported version") instead of
playing it with 2 captains.

`netlog.py`:

```sh
python3 tools/net/netlog.py synth --seed 20261122 --synth-seed 7 --vs-stage 0 --ticks 18000 --players 4 -o /var/tmp/v4.mpnl
python3 tools/net/netlog.py synth --seed 20261122 --synth-seed 7 --vs-stage 0 --ticks 9000 --players 4 \
    --event 2400:3 --event 4800:1 -o /var/tmp/le4.mpnl
```

- `synth --players 4` writes version 4 and fills ports 0 to 3 from `PortGen(synth_seed, port)`;
  ports 0 and 1 equal the 2-player synth of the same seeds. `--players 3` is refused, and
  `--format-version` below 4 with `--players 4` is an error. The default output stays version 3.
- `dump` prints `players`; `cmp-input` fails on a different captain count (a different version
  alone is still only a note); `mutate --port 2|3` works on a 4-player log (and is refused on a
  2-player one); `selftest` covers the version 4 round trip, byte 33, events 3 and 4 and the
  refusal of 3 players.

## Test events 3 and 4

The test event byte (byte 11 of port 0's slot) gains two codes: 3 gives captain 2 lethal damage, 4
gives captain 3 lethal damage, in the pass of checksum tick T for `--event T:CODE`. A code above the
log's player count is refused by `synth`; the engine ignores a code whose captain does not exist.

## Done test

```sh
MPPM_TEST_DISC=/path/to/your/disc.iso tools/net/m53a_test.sh [-b build-pc] [-d 86] [-o /var/tmp/mppm-online/m53a] \
    [-B 61aebcb69] [-q] [-c CHECK,...] [-l]
```

Run from the repo root. Each run gets its own home directory, `MPPM_SKIP_LAUNCHER=1`, no mods
(`MPPM_MODS_DIR`/`MPPM_NO_MODS` unset), the Xvfb lock wait, one retry on `x11 not available`, the
timeout `MPPM_M53A_TIMEOUT` (1500 s), an exit-code file and core clean-up (the wrapper of
`desync_ci.sh`). One `PASS` or `FAIL` line per check; exit 0 only if all pass. `-q` runs
`desync_ci.sh --quick` for the golden check; `-l` lists the checks. Only `lines` needs no disc.

Logs (`netlog.py synth --seed 20261122 --synth-seed 7 --vs-stage 0` plus): `V4` `--ticks 18000
--players 4`; `Vs4` `--ticks 3600 --players 4`; `Le4` `--ticks 9000 --players 4 --event 2400:3
--event 4800:1`; `L2` `--ticks 18000` (2 captains, the perf baseline).

| Check | Pass when |
|---|---|
| `lines` | every file changed in `src/` or `include/` since `-B` has its line count at `-B`, no file was added or removed there, every added line contains `PC_ONLY(` or `PC_SWITCH(`; `tools/net/golden` unchanged |
| `selftest` | `netlog.py selftest`; `MPPM_RNG_SELFTEST=1`; `MPPM_NPLAYER_SELFTEST=core`, `piki`, `vs`, `view` each print PASS and exit 0 |
| `golden` | `desync_ci.sh` (`--quick` with `-q`) exits 0 with `MPPM_GOLDEN=strict` |
| `v4` | `v4_r1` (record, stats, perf) and `v4_r2` (`--checksum-verify`) exit 0; their checksum logs are identical; E line at 18000; at least 600 C lines, all with `n=4/`; `# players=4`; `cmp-input V4 v4_r1.mpnl` = 0 and `dump` shows version 4, players 4; `other-in-session=0`; more than 100 distinct `navi` hashes; Pikmin counted; no `PANIC`, `Failed assertion` or `crash screen` |
| `activity` | `v4_r1`'s tick-18000 stats list captains 0 to 3, each `walked >= 300` and `squadmax >= 1`; a `captains=4 heap free=` line |
| `deaths` | `le4_r1` (stats) and `le4_r2` (verify) exit 0 with identical logs; no `L` line in ticks 2401-4800 (captain 2 down does not end the match); an `L` line in 4801-9000 (Red out, result screen); `floor 1 reseed`; a C line with `n=4/` after the result screen; the tick-3600 stats show captain 2 `alive=0` and captain 0 `alive=1` |
| `view` | `Vs4` replayed with the stock split, `--view-captain 0` and `--view-captain 3`: identical checksum logs |
| `neg` | `netlog.py mutate V4 --tick 9000 --port 2 --ticks 9600` replayed: matches `v4_r1` up to tick 9000, then differs between two C lines |
| `shots` | three paced runs of `Vs4` held at tick 1500 (`MPPM_HOLD_AT_TICK`; stock split, `--view-captain 2`, `--view-captain 3`), screenshotted with `magick import`: not black (mean above 3%), vc2 and vc3 differ in more than 5% of the pixels, the split and vc2 differ; the files stay for review (markers) |
| `perf` | a table of sim avg/p50/p99/max and pass avg for `l2_perf`, `v4_r1` and `v4_h10` (`MPPM_VS_HANDICAP=10`), with the largest Pikmin count (`n=`) and the heap free lines (`l2_perf` and `v4_r1` run with `MPPM_NPLAYER_STATS=1`, so the 2- and 4-captain figures are compared); fails when either heap line is missing, when `v4_h10`'s sim avg is above 8.0 ms or its largest Pikmin count below 90 |

Expected time: about 20 min for the golden matrix plus about 10 min for the rest.

## Code map

Every M5.3a site in `src/` and `include/` is a same-line edit (no line added or removed, so
`__LINE__` and the line numbers below, those of `61aebcb69`, stay valid) behind `PC_ONLY` or
`PC_SWITCH` (`PC_SWITCH(pc, gc)`, pc first). The helpers are declared in `port/include/p2_navi.h`
(included by `p2_host.h`) and `port/include/p2_net.h`. `KIND(n)` below stands for the written-out
`PC_SWITCH(p2_navi_kind(n->mNaviIndex), n->mNaviIndex)`, `TEAM(x)` for
`PC_SWITCH(p2_navi_team(x), x)`, `CAM` for `PC_SWITCH(p2_cam_id(navi->mNaviIndex), navi->mNaviIndex)`,
`CNT` for `PC_SWITCH(p2_cam_count(), 2)`.

### Captains, controllers, cameras, spawn (P1 "spine")

| Site | Change | Why |
|---|---|---|
| `include/Game/Navi.h:40` | `GET_OTHER_NAVI` through `p2_navi_other` | D5; `navi.cpp:1960` (dismiss) and `naviState.cpp:1331` (punch) are reachable in VS |
| `Navi.h:306` | `u8 mP2Player; u8 mP2Team;` after `mCommandOn2` | D2 |
| `Navi.h:357` | `mNaviDeadFlags[PC_SWITCH(P2_MAX_NAVI, 2)]` | dead flags per captain |
| `naviMgr.cpp:63` | `PSM::DirectorUpdator(.., p2_navi_count(), ..)` | one life director per captain |
| `naviMgr.cpp:108` | `createModel(KIND)` | `(&mOlimarModel)[2]` would read `mCursorModelData` |
| `naviMgr.cpp:128` | `p2_navi_on_birth(navi)` | writes `mP2Player`, `mP2Team` |
| `naviMgr.cpp:131` | `mSoundObj->init(KIND)`, `p2_navi_rappa_fix(navi)` | `Rappa::init` asserts 0/1 and sets `sRappa[id]` |
| `naviMgr.cpp:333` | `getAliveOrima`: `p2_navi_alive_orima(type)` for N > 2 | alive queries over N |
| `naviMgr.cpp:427-430` | ghost card: team instead of index, `p2_vs_ghost_vpbits` | ghost hides a team from the other team's viewports |
| `navi.cpp:153, 164, 170, 184, 626` | `KIND` for effects type, Louie branch, cursor/marker anims, radar icon, scale | kind = team |
| `navi.cpp:301` | sprout colour check with the team | VS plucking |
| `navi.cpp:314` | `getAt(p2_navi_other(..))` | D5 |
| `navi.cpp:427` | `GameMessageVsGetDoping(TEAM(mNaviIndex), ..)` | `getGetDopeCount` asserts 0/1 |
| `navi.cpp:924` | `GameMessageVsUseCard(p2_vs_card_use(this))` | slot machines are per team; remembers the user |
| `navi.cpp:1685, 1721` | `startVibration(.., p2_cam_id(mNaviIndex))` | D6 |
| `naviState.cpp:655, 970, 1017, 3558, 3574, 3620` | `KIND == NAVIID_Olimar` | kind = team (3574: pellet scale, simulation) |
| `naviState.cpp:749, 1155` | `naviIdx = KIND` | indexes 3-entry `se_*` tables |
| `naviState.cpp:2795-2796, 2866-2867` | `controllerLock/startDemoCamera/controllerUnLock/finishDemoCamera(CAM, ..)` | egg drop drinking is reachable in VS; D6 |
| `baseGameSection.cpp:324, 382` | `p2_nplayer_present_layout(this)` | D10: before the view calculation and at the start of the draw |
| `baseGameSection.cpp:406` | `p2_nplayer_draw_markers(gfx)` | D10 markers |
| `baseGameSection.cpp:567` | `p2_cam_attach(this)` | D7 simulation viewports, `CameraMgr` lists |
| `baseGameSection.cpp:933` | `p2_nplayer_spawn(this)` at the end of `case 0` | birth and place captains 2..N-1 by their team's onyon |
| `baseGameSection.cpp:1147` | `p2_nplayer_fix_near_far(..)` | near/far feed `calcScreenSize`, i.e. LOD |
| `baseGameSection.cpp:1218` | `p2_nplayer_bind(this, mPrevNaviIdx)` | cameras and controllers of 2..N-1, off in movies |
| `baseGameSection.cpp:1936, 1972` | `p2_nplayer_del_cameras()`, `p2_nplayer_clear()` | before the heap goes (`JUTGamePad` unlinks in its destructor) |
| `baseGameSection.cpp:2203` | `naviMgr->alloc(p2_navi_alloc_count())` | N captains in a 2P Battle harness session |
| `baseGameSection.cpp:2211` | `p2_nplayer_setup(this)` | controllers (port i) and `PlayCamera`s of 2..N-1 |
| `CameraMgr.cpp:31-33, 39, 75, 122, 144, 334` | arrays and loops sized `CNT` | N cameras |
| `CameraMgr.cpp:142` | `p2_cam_update_extra(this)` | single-camera mode steps cameras 2..N-1 |
| `CameraMgr.cpp:150` | `\| p2_cam_extra_change_state(this)` | wait for every camera |
| `CameraMgr.cpp:316` | `p2_cam_multi_extra(this)` | Multiplayer mode for cameras 2..N-1 |
| `CameraMgr.cpp:377` | `if (p2_cam_range(naviIDs, type)) return true;` | `isStartAndEnd`: `0x100 \| i` and Both = 0..N-1 |
| `vsGS_Game.cpp:460, 681` | end-stick loops `i < PC_SWITCH(p2_navi_count(), 2)` | captains 2..N-1 are unstuck before the give-up movie (fix round 1; 681 is Challenge only) |
| `umiMushi.cpp:295, 744, 767, 1140` | `KIND(mTargetNavi) == NAVIID_Olimar` | Bloyster's search sound, tail colour and weak-point effects follow the target's team (presentation; fix round 1) |
| `src/utilityU/PSMainSide_ObjCalc.cpp:63` | `if (i >= 2) break;` before `P2ASSERTLINE(66, i < 2)` | `ObjCalc_2PGame::getPlayerNo` (sound listener) asserted on captain 2 within the first second of every 4-captain session; found by the P1 smoke run, no audit listed it. The sound side still picks the nearer of captains 0 and 1 (presentation) |

Port code: `port/game/navi/navi_core.cpp` (roster rules, `p2_navi_sim_target`, selftest `core`),
`port/game/navi/navi_section.cpp` (section roster: controllers, cameras, simulation viewports,
spawn, stats lines), `port/game/net/tick.cpp` (test events 1 to 4, camera view copy for all N,
`p2_net_perf_sim_begin`, `MPPM_NPLAYER_STATS`).

### Pikmin teams and owners (P2 "piki")

| Site | Change | Why |
|---|---|---|
| `include/Game/Piki.h:291` | `u8 mP2Owner; s8 mP2Team;` after `mNavi` | D3 |
| `piki.cpp:134, 141, 1293` | owner none on init; team from the colour on init and `changeShape` | D3 |
| `pikiAI.cpp:254, 627` | `TEAM(navi->mNaviIndex) == getKind()` / `== mPikiKind` | colour = team |
| `interactPiki.cpp:63, 196` | `p2_vs_enemy_piki(piki, navi) \|\|` before the stock expression | an enemy captain by team (identical for N <= 2) |
| `interactPiki.cpp:205` | `p2_piki_set_owner` on whistle join | D4 |
| `interactPiki.cpp:236` | bitter spray: `getKind() == TEAM(navi->getNaviID())` | colour = team |
| `itemPikihead.cpp:659, 673` | sprout colour with the team; owner on pluck | D4 |
| `aiAction.cpp:100, 125` | owner on rejoin | D4 |
| `aiFormation.cpp:72` | `p2_piki_set_owner(mParent, mParent->mNavi)` in `ActFormation::init` | D4: every formation join; `InteractFue::actPiki` sets the owner only for callable states, while flick, blow, koke damage, dope and drown store the whistler in `onFlute` and join later (fix round 1) |
| `aiAction.cpp:201` | `searchOrima`: `p2_piki_search_orima(mPiki)` for N > 2 | stock maps index 2 to Blue |
| `aiFree.cpp:171` | `\|\| !p2_piki_touch_ok(p, navi)` | touch join respects the owner (D4) |
| `pikiState.cpp:578` | owner on pluck (`nukareArg->mNavi`) | D4 |
| `gameStat.cpp:170, 190` | `id = p2_piki_free_bucket()` | captain 2 would share the "no navi" bucket 2 |
| `pikiMgr.cpp:461, 463` | `p2_vs_ghost_vpbits(1/0)` | ghost card viewport bits |
| `fakePiki.cpp:992` | `getOnyon(1 - TEAM(..))` | a captain falling out of the arena: index 2 PANICs "illegal onyon index" |

Port code: `port/game/navi/piki_team.cpp` (selftest `piki`).

### VS rules, enemies, checksum (P3 "vs")

| Site | Change | Why |
|---|---|---|
| `include/Game/VsGame.h:373` | `p2_vs_ghost_tick(section, naviIndex)` | ghost card expiry restores captains >= 2 of the team |
| `vsGS_Game.cpp:92` | `p2_vs_match_reset()` | card users per team reset each match |
| `vsGS_Game.cpp:870` | `if (p2_vs_orima_down(this, section, naviIdx)) return;` before the bounds assert | D12 |
| `vsGameSection.cpp:826` | `p2_vs_init_sprays(this)` | captains 2..N-1 get the start sprays |
| `vsCardMgr.cpp:276, 283, 290` | `p2_vs_card_navi(user)` | D11: the captain who pressed Y |
| `vsCardMgr.cpp:395` | `p2_vs_card_enemy_navi(user)` | the nearest live enemy captain |
| `include/Game/EnemyBase.h:623, 656` | `u8 mP2NaviAttack`, cleared in `resetEvents` | hit-once bits of captains >= 2 |
| `enemyBase.cpp:483` | `p2_enemy_navi_clear(enemy)` | |
| `enemyInteractBattle.cpp:67, 69` | `p2_enemy_navi_free/mark` | `EB_AttackingNavi1` for captain 1, bits for 2..N-1 |
| `include/Game/gamePlayData.h:450` | `mNaviLifeMax[PC_SWITCH(P2_MAX_NAVI, 2)]` | per captain |
| `DangoMushi.cpp:414, 538, 563`; `blackMan.cpp:361, 871, 1181, 1208`; `Queen.cpp:360`; `shijimiChou.cpp:793`; `umiMushi.cpp:851`; `Rock.cpp:373`; `umiMushiState.cpp:393` | `p2_navi_sim_target(this)` for `getActiveNavi()` | D13 |

Port code: `port/game/navi/vs_teams.cpp` (selftest `vs`), `port/game/net/checksum.cpp` (cameras
hashed over `p2_cam_count()`; for N > 2 the card users `p2_vs_card_user(0/1)` in the navi hash and
`Piki::mP2Owner` in the piki hash, so 1-2 captain hashes are unchanged; `p2_net_perf_sim_end`), `port/game/net/vs_boot.cpp`
(`MPPM_VS_HANDICAP`).

### Observers and presentation (P4 "view")

| Site | Change | Why |
|---|---|---|
| `creatureLOD.cpp:46` | `if (p2_lod_update(this, parm, lodSphere, lodCylinder)) return;` | D8 |
| `graphics.cpp:199` | `mSimAspect = p2_cam_sim_aspect(mCamera, <stock>)` | D9 |

Port code: `port/game/navi/view.cpp` (LOD for N > 2, sim aspect, `--view-captain` layout, markers,
ghost bits; selftest `view`).

### Harness (P5)

`port/net/net.cpp` and `port/include/p2_net.h` (no `src/` edit): `--vs-players`, `--view-captain`,
version 4 read and write, `# players=`, the start lines, `MPPM_PERF`. `tools/net/netlog.py`
(`--players`, version 4, events 3 and 4), `tools/net/m53a_test.sh` (the done test).

## Results

The done test after fix round 1 (`tools/net/m53a_test.sh -d 91 -B 61aebcb69`, full golden matrix,
no `-q`) on the integration branch: **`m53a_test: PASS in 1469 s`**, every check PASS, on this machine
(Ryzen 9 7900X3D, Linux, headless Xvfb at 1280x960). `desync_ci.sh --quick` (display 91) passes on its own as
well: 50 PASS lines, 0 FAIL, 379 s.

| Check | Result |
|---|---|
| `lines` | 36 files changed in `src/` and `include/` since `61aebcb69`, line counts equal, every added line guarded; `tools/net/golden` unchanged |
| `selftest` | `netlog.py`, rng and nplayer `core`, `piki`, `vs`, `view` PASS |
| `golden` | full `desync_ci.sh` with `MPPM_GOLDEN=strict`: 70 PASS lines, 0 FAIL, 1033 s |
| `v4` | `v4_r1` and `v4_r2` identical; 600 C lines, all `n=4/`; recording equals the input, version 4, players 4; 596 distinct navi hashes; up to 20 Pikmin |
| `activity` | tick 18000: walked 21548, 31285, 31372, 29658 and squadmax 10, 10, 6, 8 for captains 0 to 3 |
| `deaths` | `le4_r1` and `le4_r2` identical; no `L` line in 2401-4800; `L` lines from tick 5130 (Red out); floor 1 reseed; an `n=4/` C line at 5160; tick 3600: captain 2 `alive=0`, captain 0 `alive=1` |
| `view` | stock split, `--view-captain 0` and `--view-captain 3`: identical (120 C lines) |
| `neg` | port 2 changed at record 9000: matches before, first differs at tick 9030 (rng, navi, piki, cam) |
| `shots` | means 26.7 / 20.3 / 25.5 %; vc2 and vc3 differ in 14.0 % of the pixels, split and vc2 in 20.4 % |
| `perf` | table below |

Performance (`MPPM_PERF=1`, steady clock, milliseconds per session tick; `pass` = one tick's
simulation start to the next one's, including draw-command building and pacing in a headless run):

| Run | Captains | Pikmin (max) | Ticks | sim avg | p50 | p99 | max | pass avg | heap free (bytes) |
|---|---|---|---|---|---|---|---|---|---|
| `l2_perf` (L2) | 2 | 21 | 17996 | 0.212 | 0.207 | 0.333 | 1.937 | 1.162 | 232796264 |
| `v4_r1` (V4) | 4 | 20 | 17996 | 0.235 | 0.230 | 0.362 | 1.892 | 1.363 | 232769320 |
| `v4_h10` (V4, `MPPM_VS_HANDICAP=10`) | 4 | 100 | 17996 | 0.385 | 0.381 | 0.583 | 2.140 | 1.899 | 232769320 |

- The plan's budget (4 captains, 100 Pikmin, a full VS roster, at most 8 ms per tick) holds with a
  wide margin here: 0.385 ms average, 0.583 ms p99. It is not measured on the plan's reference
  machine (a 4-core x86_64 from 2018); that run is part 2.
- Heap: the game heap (`BaseGameSection::mTheExpHeap`) after spawning has 232796264 bytes free with 2
  captains and 232769320 with 4, so captains 2 and 3 cost 26944 bytes there. The line is printed
  when the captains spawn, before the handicap Pikmin exist, so it is the same for `v4_r1` and
  `v4_h10`; `pikiMgr->alloc(MAX_PIKI_COUNT)` reserves the 100 Pikmin slots either way.
- The integration run before fix round 1 gave 0.209 / 0.214 / 0.359 ms sim avg for the three runs
  (`m53a_test: PASS in 1465 s`). Fix round 1 hashes one more byte per Pikmin and the two card users for N > 2; the rest of
  the difference is run-to-run noise.

## Audits

The plan row asks for a nearest-navi audit of the 47 `getActiveNavi` sites; with it are the uses of
the captain index and the other places that assume 2 captains. Line numbers are those of
`61aebcb69`. P1 to P4 are the packages of the code map above (spine, piki, vs, view).

### `getActiveNavi()` (47 call sites)

`src/` and `include/` hold 49 hits: the declaration (`Navi.h:332`), the definition
(`naviMgr.cpp:151`) and the 47 call sites below. In VS `getActiveNavi` returns captain 0 (it has a
controller) for N = 2 and N = 4 alike, so no VS site changes behaviour unless it is wrapped. Classes:
ENEMY = simulation, wrapped with `p2_navi_sim_target(this)` (stock for N <= 2, the nearest live
captain for N > 2, ties to the lower index); MODE = simulation in story or Challenge code, not
reachable with N > 2 in part 1 (part 2); PRES = presentation (part 2: the local player's captain);
CAM = `CameraMgr` single-camera mode.

| Site | Function | Class | Part 1 |
|---|---|---|---|
| `DangoMushi.cpp:414`, `:538`, `:563` | `rollingMove`, `getFallEggNum`, `getFallPosition` | ENEMY | wrapped (P3) |
| `blackMan.cpp:361`, `:871`, `:1181`, `:1208` | `doUpdate`, `walkFunc`, `findNextRoutePoint` x2 | ENEMY | wrapped (P3) |
| `Queen.cpp:360` | `isRollingAttackLeft` | ENEMY | wrapped (P3) |
| `shijimiChou.cpp:793` | `leaveInit` | ENEMY | wrapped (P3) |
| `umiMushi.cpp:851` | `isChangeNavi` (non-two-player branch) | ENEMY | wrapped (P3) |
| `umiMushiState.cpp:393` | `StateTurn::exec` | ENEMY | wrapped (P3) |
| `Rock.cpp:373` | `updateMoveVelocity` (non-multiplayer branch) | ENEMY | wrapped (P3) |
| `CameraMgr.cpp:126` | `update`, single-camera mode (movies in VS) | CAM | kept; cameras 2..N-1 are stepped by `p2_cam_update_extra` |
| `naviMgr.cpp:462` | `doSimulation`, life BGM (VS returns earlier, `mPSMMgr`) | PRES | none |
| `navi.cpp:1461` | `control`, story rappa | PRES | none |
| `vsGS_Game.cpp:392`, `:612`, `:947` | key demo (Challenge), Pikmin zero (not VS), Challenge 1P HUD | MODE/PRES | none |
| `singleGS_MainGame.cpp:343`, `:359`, `:381`, `:418`, `:423`, `:582` | demo timers, movies | MODE | none |
| `singleGS_DayEnd.cpp:128`; `singleGS_CaveGame.cpp:155`, `:173`, `:521` | day end, cave state | MODE | none |
| `singleGameSection.cpp:353`, `:1043`, `:1125`, `:1224`, `:1318` | camera controller, radar, HUD | MODE/PRES | none |
| `aiFormation.cpp:158` | Meet-Red-Pikmin demo | MODE | none |
| `itemCave.cpp:219`, `:298` | cave entrance sound culling, AI | PRES/MODE | none |
| `farmMgr.cpp:60` | farm (story) | MODE | none |
| `RumbleMgr.cpp:91` | rumble (not multiplayer) | PRES | none |
| `newGame2DMgr.cpp:355` | `open_SMenu` gate | PRES | none |
| `hurryUp2D.cpp:502` | HUD | PRES | none |
| `ogObjSMenuMap.cpp:191`, `:235`, `:613` | pause map | PRES | none |
| `PSMainSide_Director.cpp:375`; `PSMainSide_Factory.cpp:162`, `:868`; `PSMainSide_Se.cpp:303` | audio | PRES | none |

Count: 12 ENEMY, 1 CAM, 34 MODE or PRES.

### `mNaviIndex` (93 uses), `getNaviID()` (3), `GET_OTHER_NAVI` (9)

**Changed** (code map above): `naviMgr.cpp:108, 128, 131, 333, 427, 429`; `navi.cpp:153, 164, 170, 184,
301, 314, 427, 626, 924, 1685, 1721`; `naviState.cpp:655, 749, 970, 1017, 1155, 2795, 2796, 2866, 2867,
3558, 3574, 3620`; `pikiAI.cpp:254, 627`; `interactPiki.cpp:63, 196 (+197), 236`; `itemPikihead.cpp:659`;
`fakePiki.cpp:992`; `gameStat.cpp:170, 190` (`getNaviID`); `aiAction.cpp:222, 237, 242` (bypassed by
the N > 2 early return at `:201`); `enemyInteractBattle.cpp:62` (index 0 keeps `EB_AttackingNavi0`,
the others go through the hooks on `:67` and `:69`); `umiMushi.cpp:295, 744, 767, 1140` (fix round 1:
presentation by team); the `GET_OTHER_NAVI` users `navi.cpp:1960, 2020`, `interactNavi.cpp:257`,
`naviState.cpp:488, 918, 985, 1331, 2494, 3599` through the macro (`1331`, punch, and `1960`, dismiss
with X, are reachable in VS and read `getAt(-1)` for captain 2 without the change).

**Unchanged on purpose**, and why:

| Sites | Why |
|---|---|
| `naviMgr.cpp:134` (president), `navi.cpp:906` (cave menu), `:1210` (dead laydown), `naviState.cpp:79, 89` (captain swap) | story only, or not multiplayer |
| `naviMgr.cpp:140` | the dead flags, already sized `P2_MAX_NAVI` |
| `naviMgr.cpp:450, 452` | `PSM` director ids, valid below 8 |
| `navi.cpp:1686, 1722`; rumble calls `aiFree.cpp:176`, `pikiState.cpp:641`, `naviState.cpp:2272, 2387, 3005, 3049, 3147, 3245, 3303, 3337, 3377` | rumble id 2 is `RUMBLEID_Both`: captain 2 rumbles pads 0 and 1 locally, presentation only (part 2 remaps rumble to local pads) |
| `naviState.cpp:2595-2630` | formation counts, valid below 7 |
| `naviState.cpp:2930` | `gmOrimaDown(index)`, handled by `p2_vs_orima_down` (P3) |
| `vsGameSection.cpp:855` | receives the team already |
| `vsGS_Game.cpp:950`, `singleGS_DayEnd.cpp:131`, `singleGS_CaveGame.cpp:523`, `singleGameSection.cpp:728, 1046, 1227, 1321`, `navi_demoCheck.cpp:131`, `aiFormation.cpp:175`, `interactPiki.cpp:74, 77` | story or Challenge only (part 2) |
| `CameraMgr.cpp:128` | single-camera mode, index 0 or 1 |
| `ogObjSMenuMap.cpp:33, 236, 613`, `PSMainSide_Se.cpp:305` | presentation (pause map, sound) |
| `Navi.h:151` (`getCreatureID`), `GameMessage.h:78, 86` (field name) | no captain-count assumption |

### Other two-captain assumptions

| Site | Status |
|---|---|
| `CameraMgr` arrays `[2]`, `id ^= 1` (`CameraMgr.cpp:137`), `isStartAndEnd` | P1: N cameras; the `^ 1` copy only in single-camera mode, plus the extra step |
| `mCameraParms[2]` (`CameraMgr.h:418`) | unchanged: the 1P and 2P parameter sets, not one per captain |
| `BlendCamera(2, ..)`, `ShadowMgr(2)`, `HorizonalSplitter`, `particleMgr->setViewport`, `mPlayerMode = 2` view counts | unchanged: they count presented viewports, at most 2 in part 1 |
| `vpStats[2]` (`creatureLOD.cpp:48`), `P2ASSERTBOUNDSLINE(558, .., 2)` (`creature.cpp:365`), `AILOD_IsVisVP0..3` | at most 2 presented viewports; the N > 2 LOD path is P4 (D8) |
| `mNaviDeadFlags[2]`, `getAliveOrima` | P1 |
| `mCursorAnims[2]`, `mMarkerAnims[2]`, `(&mOlimarModel)[id]` | indexed by kind (P1) |
| `PSM::DirectorUpdator(.., 2, ..)`, `Rappa::init` assert, `Rappa::sRappa[2]` | P1 |
| `EB_AttackingNavi0/1` | P3 (`mP2NaviAttack` bits) |
| `PikiNaviCounter` bucket 2 = no navi | P2 (`p2_piki_free_bucket`) |
| `mNaviLifeMax[2]` | P3, sized `P2_MAX_NAVI` |
| `mOlimarData[2]` | unchanged: save data, `[1]` unused |
| `mSprayCount[2]`, `mBerryCount[2]` | unchanged: indexed by spray type, not captain |
| `mDeadNaviID` | unchanged: a u8 of bits |
| `VsGameSection.h:129-172` `[2]`, `mLoseCauses[2]`, `mSlotMachines[2]`, `mOlimarHandicap/mLouieHandicap`, `VSPlayerColor`, `calcVsScores` | unchanged (D11): already team-indexed (onyon type and colour), and part 1 has 2 teams; per-team arrays for more teams are part 2 |
| `vsGS_Game.cpp:870` death assert, winner `:205-235` | P3 (D12) |
| `mControllerP1/P2`, `setCamController`, spawn `switch (naviCount)` | P1 (roster, bind and spawn hooks) |
| `GameMessageVsGetDoping/UseCard` from a navi index | P1 sends the team; P3 resolves the user |
| `vsCardMgr.cpp:276-418` (`getAt(user)`, `getAt(1 - user)`, `1 - user` colours) | P3 for the navi lookups; the colour and onyon `1 - user` are already team-correct |
| `vsGS_Game.cpp:460, 681` end-stick loops | fix round 1: `p2_navi_count()` |
| `pikiState.cpp:3100` (`for (i < 2)` nearest-navi fallback) | deterministic, considers captains 0 and 1 only: part 2 |
| `DispMemberVs`, card slot positions, J2D VS HUD | captains 0 and 1 only: part 2 (HUD for N) |
| `JSTFindCreature.cpp:61, 66` (`orimaAlive/Dead` in movie scripts) | through `getAliveOrima` (P1) |
| `JUTGamePad` ports, `PAD_MAX_CONTROLLERS = 4`, `.mpnl` 4 slots | already 4 |

## Known limits

- Only 2 or 4 captains, teams fixed to `i & 1`; no lobby-chosen teams.
- Stock menus still give 1 or 2 captains: 4 exist only in a harness session (`--vs-stage` plus
  `--vs-players 4`) or a version 4 replay. `MPPM_VS_PLAYERS=4` with a version 2 or 3 replay plays 2
  captains (the header wins, with a warning).
- At most 2 presented viewports: the stock split of captains 0 and 1, or one captain full screen.
  With `--view-captain` the 2P HUD of captains 0 and 1 (and its split line) is still drawn over
  the single view.
- The VS HUD, card slots and radar show captains 0 and 1 only; one slot machine per team (effects
  act on the captain who pressed Y, or the team's lowest live captain if that one is down).
- A downed captain whose teammate lives is released (D12) but its model stays where it fell, upright,
  with a grey marker, for the rest of the match. Stock VS never shows a downed captain while play goes
  on, so there is no reference look; presentation only.
- VS scoring and the result stay 2-team: `calcVsScores` is the stock 2-onyon score, the winner is
  P1/P2/Draw on the stock J2D screen, and the handicaps stay the scalar pair (D11).
- Presentation callers of `getActiveNavi` still use captain 0; one sound listener per viewport;
  captain 2's rumble id is `RUMBLEID_Both` (both local pads).
- `pikiState.cpp:3100` (nearest-navi fallback) considers captains 0 and 1 only.
- Ghost card: when the timer runs out, `VsGame.h:351-370` (stock) still restores the effects of
  captain `team` (0 or 1) even if the card hit captain 2 or 3; `p2_vs_ghost_tick` restores only the
  captains >= 2 whose effects the card saved. Presentation only.
- `MAX_PIKI_COUNT` stays 100; the heap is reported, not resized.
- Performance is measured on this machine only (Ryzen 9 7900X3D), not the plan's reference machine.

## Part 2

- The 4-captain co-op cave dive: story and Challenge paths (`singleGS_*`, `singleGameSection`, hole
  and geyser menus as a ready-check, the MODE `getActiveNavi` sites, single-camera story flows,
  `pikiState.cpp:3100`, story deaths and movies, Lifeline) and its replay log.
- 3 teams (Yellow onyon, `FIXNODE`, `setVersusOnyon`, `RandMapScore`, the bedama sentinel,
  `1 - onyonType`) and 4-team free-for-all (the Piki team byte becomes the reader; tints);
  `--vs-players 3`; lobby-chosen teams.
- Raising the Pikmin cap (`P2_MAX_PIKI`) and per-mode heap budgets.
- `FourSplitter` and local 3-4-way split screen (`particleMgr` viewport bound, draw passes for more
  than 2 viewports).
- HUD for N (RmlUi scoreboard, name tags, per-captain life and sprays, card slots per captain),
  radar icons for captains 3 and 4; the plan 2.5 team palette and per-team icon in that HUD (part 1
  has only the colour-plus-shape markers).
- Plan 3.6 for more than 2 teams: the handicap pair becomes an array, `calcVsScores` gives a score
  per team with a "leader" query, and the winner becomes a ranked result on an RmlUi results screen.
- Presentation callers of `getActiveNavi` become the local player's captain; sound with one local
  listener; rumble remapped to local pads.
- Per-captain card slots; ghost card rules for more than 2 presented viewports.
- More than 4 captains (`P2_PAD_PORTS`).
- The plan's matrix A run and the perf budget on the reference machine (2018 4-core).
