# Online harness: game-side hook sites

Every edit the online work (M5.1a, M5.1b, M5.3a, M5.3b) makes in `src/` and `include/`, with the line it is on,
what it calls and where that is implemented. All of them are same-line edits behind `PC_ONLY`,
`PC_SWITCH` or `TARGET_PC` (no file changes its line count), so the matching build stays
byte-identical. Line numbers are those of branch `online/m51b-int` after the M5.1b fix round.
`S1` to `S34` are the site numbers of the M5.1b design; `A` marks an M5.1a site, `F` a site added
by the M5.1b integration or fix round.

The C bridge is declared in `port/include/p2_net.h` (simulation and harness) and
`port/include/p2_host.h` (host: pacing, heaps). [M5_1A_HARNESS.md](M5_1A_HARNESS.md) explains
what each mechanism does; this page is only the map.

## Tick, tick order and camera

| # | Site | Edit | Calls (implemented in) | Purpose |
|---|---|---|---|---|
| S5 | `src/sysGCU/section.cpp:181` | `PC_ONLY(p2_host_arm_tick(); p2_net_pass_begin(this);) beginRender();` | `p2_host_arm_tick` (`port/main.cpp`), `p2_net_pass_begin` (`port/game/net/tick.cpp`) | Arms the pacing hook for this `Section::main` pass and records the section for the tick order |
| S12 | `src/JSystem/JFramework/JFWDisplay.cpp:362` | `... idleEnd(); PC_ONLY(p2_net_pass_after_wait();)` | `p2_net_pass_after_wait` (`tick.cpp`) | Update-first: right after the pacing wait, runs `update()` of the armed section when it is the running `BaseGameSection` (vtable check) and `p2_host_update_first()` |
| S6 | `src/sysGCU/section.cpp:189` | `PC_ONLY(if (!p2_net_pass_updated())) update();` | `p2_net_pass_updated` (`tick.cpp`) | Skips the stock update when S12 already ran it |
| S18 | `src/plugProjectKandoU/baseGameSection.cpp:293` | `PC_ONLY(p2_net_sim_tick_begin(this);) SysShape::Model::cullCount = 0;` | `p2_net_sim_tick_begin` (`tick.cpp`) | Start of `doUpdate`: test events (`--debug-event`), then the camera step and the copy of each stepped view into `mCurViewMatrix` |
| S19, S20 | `baseGameSection.cpp:386`, `:391` | `PC_SWITCH((void)0, cameraMgr->update());` | - | The stock camera steps in `doDraw`, removed (moved to S18) |
| A | `baseGameSection.cpp:372` | `gameSystem->endFrame(); PC_ONLY(p2_net_sim_tick_end();)` | `p2_net_sim_tick_end` (`port/game/net/checksum.cpp`) | End of a simulation tick: the checksum (`C` lines) |
| A | `src/JSystem/JUtility/JUTGamePad.cpp:142` | `} PC_ONLY(p2_net_exchange(mPadStatus);)` | `p2_net_exchange` (`port/net/net.cpp`) | After `PADRead`: the tick seam (record, replay, later the network exchange) |

## Nonsim regions (loading loops)

All call `p2_net_nonsim_begin` / `p2_net_nonsim_end` (`port/game/net/tick.cpp`, counting in
`port/net/net.cpp`): pad reads inside are released and are not ticks; leaving the outermost region
reads the pads once more.

| # | Site | Region |
|---|---|---|
| S1, S2 | `src/sysGCU/section.cpp:122`, `:133` | `Section::loading` (`doLoadingStart` up to the DVD error loop) |
| S3, S4 | `section.cpp:142`, `:161` | `Section::fadeIn` |
| S7, S8 | `section.cpp:237`, `:242` | the wait loop of `Section::fadeOut` |
| S9, S10 | `section.cpp:264`, `:272` | the THP DVD wait after `fadeOut` in `Section::run` |
| S13, S14, S15 | `src/plugProjectKandoU/baseGameSection.cpp:182`, `:200`, `:208` | `waitSyncLoad` (both returns) |
| S16, S17 | `baseGameSection.cpp:238`, `:247` | the single frame before `waitSyncLoad(true)` |
| S21, S22 | `baseGameSection.cpp:2052`, `:2059` | the same before the inlined `waitSyncLoad(false)` |

## RNG streams

`rand()` picks its stream inside `p2_msl_rand` (`port/sdk/msl_compat.c`); these scopes mark
presentation code. The guards are `P2RngPres` / `P2RngDraw` (`port/include/p2_net.h:266-273`),
which call `p2_rng_pres_begin/end` and `p2_rng_draw_begin/end`.

| # | Site | Edit | Scope |
|---|---|---|---|
| S11 | `src/sysGCU/section.cpp:352` | `PC_ONLY(P2RngDraw p2RngDraw;) doDraw(gfx);` | the whole draw of a section |
| S23 | `src/sysGCU/screenMgr.cpp:400` | `{ PC_ONLY(P2RngPres p2RngPres;)` | 2D screen update (`Screen::Mgr::update`) |
| S24 | `screenMgr.cpp:471` | `{ PC_ONLY(P2RngPres p2RngPres;)` | 2D screen draw (`Screen::Mgr::draw`) |
| S25 | `src/plugProjectEbisawaU/particle2dMgr.cpp:202` | `{ PC_ONLY(P2RngPres p2RngPres;)` | 2D particle update (`TParticle2dMgr::update`) |
| A | `src/sysGCU/system.cpp:421` | `srand(PC_SWITCH(p2_net_boot_seed(OSGetTick()), OSGetTick()));` | the boot seed (`port/net/net.cpp`) |

## Simulation frustum

The cull planes use `mSimAspect` (the aspect times `p2_net_sim_k`), the picture keeps
`mAspectRatio`.

| # | Site | Edit |
|---|---|---|
| S26 | `include/Camera.h:52` | `getFOV` uses `PC_SWITCH(mSimAspect, mAspectRatio)` |
| S27 | `include/Camera.h:56` | the field `PC_ONLY(f32 mSimAspect;)` after `mAspectRatio` |
| S28 | `src/sysCommonU/camera.cpp:70` | `PC_ONLY(mSimAspect = mAspectRatio;)` (constructor) |
| S29 | `camera.cpp:216` | `PC_ONLY(mSimAspect = camera->mSimAspect;)` (copy) |
| S30 | `src/sysGCU/graphics.cpp:199` | `PC_ONLY(mCamera->mSimAspect = ... x / y * p2_net_sim_k(p2_wide_k());)`; `p2_net_sim_k` in `port/net/net.cpp`, `p2_wide_k` in `port/sdk/wide.cpp` |

## Build determinism

| # | Site | Edit | Calls |
|---|---|---|---|
| S31, S32, S33 | `include/trig.h:25`, `:41`, `:74` | `PC_ONLY(__attribute__((always_inline)))` on the table `sinf`, `cosf`, `acosf` | - (an unoptimised build could emit them out of line, and the libm rename would turn that into a second `p2m_sinf`; [M5_1B_BUILD.md](M5_1B_BUILD.md) section 1) |
| S34 | `src/JSystem/JKernel/JKRHeap.cpp:204` | `return PC_SWITCH(p2_heap_zero(do_alloc(...), byteCount), do_alloc(...));` | `p2_heap_zero` (`port/sdk/heap_zero.cpp`): zero on alloc |
| F | `src/JSystem/JKernel/JKRExpHeap.cpp:48` | `(u8*)(PC_ONLY(p2_heap_zero_limit(expHeapSize), ) JKRAllocFromHeap(...))` | `p2_heap_zero_limit` (`heap_zero.cpp`): a new heap's block is cleared only up to its header |
| F | `src/JSystem/JKernel/JKRSolidHeap.cpp:26` | the same with `solidHeapSize` | `p2_heap_zero_limit` |

## Loads, VS boot, input and crash

| # | Site | Edit | Calls (implemented in) | Purpose |
|---|---|---|---|---|
| A | `src/sysGCU/dvdThread.cpp:191` | `} PC_ONLY(p2_net_dvd_sync(cmd);)` | `p2_net_dvd_sync` (`port/game/net/dvd_sync.cpp`) | In a session a DVD command returns only when the DVD thread has finished it |
| A | `src/sysGCU/titleSection.cpp:379` | `if (mMainTitleMgr.isFinish() PC_ONLY(\|\| p2_net_vs_title_pending())) {` | `p2_net_vs_title_pending` (`port/game/net/vs_boot.cpp`) | `--vs-stage`: leave the title by itself |
| A | `titleSection.cpp:380` | `int id = PC_SWITCH(p2_net_vs_title_select(...), ...);` | `p2_net_vs_title_select` (`vs_boot.cpp`) | picks 2P Battle |
| A | `src/plugProjectKandoU/vsGS_Title.cpp:275` | `PC_ONLY(if (p2_net_vs_inject(section, this)) { return; })` | `p2_net_vs_inject` (`vs_boot.cpp`) | fills the VS config as the stage select does |
| A | `src/JSystem/JUtility/JUTGamePad.cpp:302`, `:318` | `PC_ONLY(\|\| p2_net_pad_wallclock_off())` | `p2_net_pad_wallclock_off` (`port/net/net.cpp`) | reset combo and long press (wall clock) off in a session |
| A | `src/JSystem/JUtility/JUTException.cpp:591` | `bool result = false; PC_ONLY(p2_net_crash_stop();)` | `p2_net_crash_stop` (`port/net/net.cpp`) | `JUTException::readPad`: the crash screen owns the pads; closes the logs, the seam becomes a no-op, a headless run exits 5 |
| F | `src/utilityU/PSMainSide_Scene.cpp:1194`, `:1205` | `PC_ONLY(if (se != nullptr)) se->setVolume(...)` | - | `PSMuteSE_on2D` / `PSMuteOffSE_on2D`: the BGM handle is null with audio off ([M5_1B_AUDIT.md](M5_1B_AUDIT.md) (b)) |

## Host side of the pass

Not game code, but where the hooks above end up: `port/main.cpp` `p2_host_tick_wait` (the pacing
wait, edit G1 in `waitForTick`), `p2_host_present` (end of pass, from `JFWDisplay::endFrame`;
ends with the late-sampling wait `pre_read_wait`), `p2_host_update_first` (the wanted tick order,
latched per pass). `port/net/probe.cpp` (`--latency-probe`) reads
`p2_net_pass_ran_update_first` to know which order a pass really ran in.

## M5.3a (N captains)

The M5.3a part 1 sites, as designed plus the ones found at integration and in fix round 1 (marked; line numbers of `61aebcb69`; same-line edits keep them valid).
`TEAM`, `KIND` and `CAM` are the written-out `PC_SWITCH(p2_navi_team(..), ..)`,
`PC_SWITCH(p2_navi_kind(..), ..)` and `PC_SWITCH(p2_cam_id(..), ..)`; every helper is the stock value
or a no-op while the section has at most 2 captains. The helpers are declared in
`port/include/p2_navi.h`; [M5_3A_NPLAYER.md](M5_3A_NPLAYER.md) has the rules (D1-D16) and the reason
for each site. Owner: P1 `port/game/navi/navi_core.cpp` and `navi_section.cpp`, P2 `piki_team.cpp`,
P3 `vs_teams.cpp`, P4 `view.cpp`.

| Site | Edit | Calls (implemented in) |
|---|---|---|
| `include/Game/Navi.h:40` | `GET_OTHER_NAVI` → `PC_SWITCH(p2_navi_other(..), 1 - ..)` | `p2_navi_other` (P1) |
| `Navi.h:306`, `:357` | `PC_ONLY(u8 mP2Player; u8 mP2Team;)`; `mNaviDeadFlags[PC_SWITCH(P2_MAX_NAVI, 2)]` | - |
| `src/plugProjectKandoU/naviMgr.cpp:63`, `:108`, `:128`, `:131` | director count, `createModel(KIND)`, `p2_navi_on_birth`, `init(KIND)` + `p2_navi_rappa_fix` | P1 |
| `naviMgr.cpp:333` | `if (p2_nplayer()) return p2_navi_alive_orima(type);` | P1 |
| `naviMgr.cpp:427-430` | ghost card by `TEAM`, `p2_vs_ghost_vpbits` | P1, P4 |
| `navi.cpp:153`, `:164`, `:170`, `:184`, `:626` | `KIND` | P1 |
| `navi.cpp:301`, `:314`, `:427` | `TEAM` sprout colour, `p2_navi_other`, `GameMessageVsGetDoping(TEAM)` | P1 |
| `navi.cpp:924` | `GameMessageVsUseCard(p2_vs_card_use(this))` | P3 |
| `navi.cpp:1685`, `:1721` | `startVibration(.., p2_cam_id(..))` | P1 |
| `naviState.cpp:655`, `:749`, `:970`, `:1017`, `:1155`, `:3558`, `:3574`, `:3620` | `KIND` | P1 |
| `naviState.cpp:2795`, `:2796`, `:2866`, `:2867` | camera lock and demo camera with `CAM` | P1 |
| `baseGameSection.cpp:324`, `:382` | `p2_nplayer_present_layout(this)` | P4 |
| `baseGameSection.cpp:406` | `p2_nplayer_draw_markers(gfx)` | P4 |
| `baseGameSection.cpp:567`, `:933`, `:1147`, `:1218`, `:1936`, `:1972`, `:2211` | `p2_cam_attach`, `p2_nplayer_spawn`, `p2_nplayer_fix_near_far`, `p2_nplayer_bind`, `p2_nplayer_del_cameras`, `p2_nplayer_clear`, `p2_nplayer_setup` | P1 |
| `baseGameSection.cpp:2203` | `naviMgr->alloc(PC_SWITCH(p2_navi_alloc_count(), 2))` | P1 (`p2_net_vs_players`, `port/net/net.cpp`) |
| `src/plugProjectNishimuraU/CameraMgr.cpp:31-33`, `:39`, `:75`, `:122`, `:144`, `:334` | `PC_SWITCH(p2_cam_count(), 2)` | P1 |
| `CameraMgr.cpp:142`, `:150`, `:316`, `:377` | `p2_cam_update_extra`, `p2_cam_extra_change_state`, `p2_cam_multi_extra`, `p2_cam_range` | P1 |
| `include/Game/Piki.h:291`; `piki.cpp:134`, `:141`, `:1293` | `PC_ONLY(u8 mP2Owner; s8 mP2Team;)`; owner none, `p2_color_team` | P1, P2 |
| `pikiAI.cpp:254`, `:627`; `interactPiki.cpp:236`; `itemPikihead.cpp:659`; `fakePiki.cpp:992` | `TEAM` | P1 |
| `interactPiki.cpp:63`, `:196` | `PC_ONLY(p2_vs_enemy_piki(..) \|\|)` | P2 |
| `interactPiki.cpp:205`; `itemPikihead.cpp:673`; `aiAction.cpp:100`, `:125`; `pikiState.cpp:578` | `p2_piki_set_owner` | P2 |
| `aiAction.cpp:201` | `if (p2_nplayer()) return p2_piki_search_orima(mPiki);` | P2 |
| `aiFree.cpp:171` | `\|\| !p2_piki_touch_ok(p, navi)` | P2 |
| `gameStat.cpp:170`, `:190` | `id = PC_SWITCH(p2_piki_free_bucket(), 2)` | P2 |
| `pikiMgr.cpp:461`, `:463` | `p2_vs_ghost_vpbits(1/0)` | P4 |
| `include/Game/VsGame.h:373` | `p2_vs_ghost_tick(section, naviIndex)` | P3 |
| `vsGS_Game.cpp:92`, `:870` | `p2_vs_match_reset()`; `if (p2_vs_orima_down(..)) return;` | P3 |
| `vsGameSection.cpp:826` | `p2_vs_init_sprays(this)` | P3 |
| `vsCardMgr.cpp:276`, `:283`, `:290`, `:395` | `p2_vs_card_navi(user)`, `p2_vs_card_enemy_navi(user)` | P3 |
| `include/Game/EnemyBase.h:623`, `:656`; `enemyBase.cpp:483`; `enemyInteractBattle.cpp:67`, `:69` | `mP2NaviAttack`; `p2_enemy_navi_clear/free/mark` | P3 |
| `include/Game/gamePlayData.h:450` | `mNaviLifeMax[PC_SWITCH(P2_MAX_NAVI, 2)]` | - |
| `DangoMushi.cpp:414`, `:538`, `:563`; `blackMan.cpp:361`, `:871`, `:1181`, `:1208`; `Queen.cpp:360`; `shijimiChou.cpp:793`; `umiMushi.cpp:851`; `Rock.cpp:373`; `umiMushiState.cpp:393` | `PC_SWITCH(p2_navi_sim_target(this), naviMgr->getActiveNavi())` | P1 |
| `src/utilityU/PSMainSide_ObjCalc.cpp:63` (integration) | `PC_ONLY(if (i >= 2) { break; })` before `P2ASSERTLINE(66, i < 2)` in `ObjCalc_2PGame::getPlayerNo` | - (the sound listener asserted on captain 2) |
| `aiFormation.cpp:72` (fix round 1) | `PC_ONLY(p2_piki_set_owner(mParent, mParent->mNavi);)` in `ActFormation::init` | P2 (owner on every formation join, also from non-callable states: flick, blow, koke damage, dope, drown) |
| `vsGS_Game.cpp:460`, `:681` (fix round 1) | end-stick loops `i < PC_SWITCH(p2_navi_count(), 2)` | P1 |
| `umiMushi.cpp:295`, `:744`, `:767`, `:1140` (fix round 1) | `KIND` of `mTargetNavi` for the search sound, tail colour and weak-point effects | P1 (presentation) |
| `creatureLOD.cpp:46` | `if (p2_lod_update(this, parm, lodSphere, lodCylinder)) return;` | P4 |
| `graphics.cpp:199` | `mSimAspect = p2_cam_sim_aspect(mCamera, <stock>)` (S30 extended) | P4 |

Port-side hooks (no `src/` line): `p2_net_perf_sim_begin` first in `p2_net_sim_tick_begin`
(`port/game/net/tick.cpp`, S18), `p2_net_perf_sim_end` first in `p2_net_sim_tick_end`
(`port/game/net/checksum.cpp`), test events 1 to 4 in `apply_debug_event` (`tick.cpp`), the camera
hash over `p2_cam_count()` and, for N > 2 only, the VS card users (`p2_vs_card_user`) and
`Piki::mP2Owner` (`checksum.cpp`), `MPPM_VS_HANDICAP` in `p2_net_vs_inject`
(`port/game/net/vs_boot.cpp`).

To list the sites again: `git diff -U0 main -- src include | grep '^+[^+]'`.

## M5.3b (online sessions)

Part 1 of M5.3b adds **one** `src/` line (line number of `aa6b6985e`; the edit keeps it). Everything
else of the online session is port-side. [M5_3B_NETCODE.md](M5_3B_NETCODE.md) is the overview,
[M5_3B_LOCKSTEP.md](M5_3B_LOCKSTEP.md) explains each mechanism.

| Site | Edit | Calls (implemented in) | Purpose |
|---|---|---|---|
| `src/plugProjectKandoU/vsGS_Title.cpp:192` | `if (section->mDvdThreadCommand.mMode == DvdThreadCommand::CM_Completed) { PC_ONLY(if (p2_net_ch_inject(section, this)) { return; })` in `TitleState::execChallenge`, case `VSTITLE_PrepareDisp` (the same pattern as the VS hook at `:275`) | `p2_net_ch_inject` (`port/game/net/vs_boot.cpp`) | `--ch-stage`, a version 5 replay or an online Challenge session: fills the 2P Challenge config as the course select does on confirm (`GSM_TWO_PLAYER_CHALLENGE`, the course's stage data, the cave info file, both spray counts), begins the session and the floor and goes to `VGS_Load`; 0 (stock path) when no course is set |

Port-side hooks of M5.3b (no `src/` line):

| Where | Hook | Purpose |
|---|---|---|
| `port/game/net/vs_boot.cpp` | `p2_net_vs_title_pending` / `p2_net_vs_title_select` (the existing sites `titleSection.cpp:379-380`) | pending also for a Challenge course; select returns `Select_Challenge` (1) for a course, `Select_Vs` (2) otherwise, so the title leaves by itself into the right mode after `START` |
| `port/game/net/vs_boot.cpp` | `p2_net_vs_inject` (the existing site `vsGS_Title.cpp:275`) | takes stage, seats and handicaps from the session's `SessionConfig` (ignores `MPPM_VS_HANDICAP` online) |
| `port/net/net.cpp` | `p2_net_exchange` (site `JUTGamePad.cpp:142`) | the online branch: sample port 0 (or the `--net-input` record), tag it t + d, wait for bundle t (hold loop fallback), write it into the four `PADStatus` slots, record |
| `port/net/net.cpp` | `p2_net_nonsim_leave` (the nonsim sites) | at depth 0 online: report the load barrier tick (`Loaded(B)`) |
| `port/net/net.cpp`, `port/game/net/checksum.cpp` | `p2_net_checksum_submit` | online: a checksum every 30 ticks even without a log, kept in a 64-line ring and sent to the host |
| `port/main.cpp` | `pre_read_wait` (end of `p2_host_present`), `p2_host_net_hold`, `p2_host_next_tick_due_ns`, `p2_host_align_tick`, `p2_host_tick_shift`, `next_tick_count`, `p2_net_game_poll` | the stall loop merged with late sampling (wait for `due(T+1) - 1 ms` and readiness of T+1, then the hold loop with UI frames and the overlay), the barrier alignment, the fixed online tick grid (a long tick under 4 ticks is caught up, one tick per retrace) and whole-tick grid shifts from the client's phase controller, `START`/`END` on the game thread |
| `port/sdk/os_thread.cpp` | `timerMain` where the grid advances (`nextRetrace += kRetracePeriod;`, `:313`), `p2_vi_phase_slew_set/get`, `p2_vi_grid_jump` | the phase slew (at most 1 ms per retrace) and the grid jump at a barrier release; never called offline |
| `port/net/probe.cpp`, `port/game/net/probe_patch.cpp` | `--latency-probe` | online the probe reads the local seat (`mPadStatus[p2_net_local_slot()]`) with a 16-pass edge limit |
| `port/launcher/online_hooks.cpp` | `p2_launcher_net_synced_json`, `p2_launcher_net_lock`, `p2_launcher_net_unlock`, `p2_launcher_online_started` | the settings lock with the host's Synced values and the take-off when a session starts from the UI |

