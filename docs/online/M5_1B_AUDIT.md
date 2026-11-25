# M5.1b audits: clock, audio, effects and RNG

Static audits for section 4 items 3, 4 and 10 of [ONLINE_MULTIPLAYER.md](../ONLINE_MULTIPLAYER.md).
They answer one question for each site: can its result reach the simulation, so that two machines
that see the same input could compute different game states? Line numbers are those of commit
`193d13937` (the M5.1b base), re-checked in the code for this document.

The runtime tools that back these audits are in [M5_1A_HARNESS.md](M5_1A_HARNESS.md): the
`audio-off`, `rng-perturb`, `paced` and frame-rate cells of the determinism matrix, and the
RNG counters (`MPPM_RNG_AUDIT=1`).

## (a) Clock reads

Search: `grep -rn "OSGetTime\|OSGetTick\|OSTicksToCalendarTime\|getTime()" src include` outside
`src/Dolphin`, plus `clock_gettime`, `SDL_GetTicks` and `chrono` in `port/game/`.

| Site | What it reads the clock for | Verdict |
|---|---|---|
| `gamePlayData.cpp:467` (`PlayData::reset`) | start of the play-time count, `mOsTime` | Save metadata. Read only by `calcPlayMinutes`. |
| `gamePlayData.cpp:552-557` (`PlayData::calcPlayMinutes`) | minutes since `mOsTime` | Save metadata and the story high-score table: callers `gamePlayCommonData.cpp:177` (high score 15, total play time) and `pikmin2MemoryCardMgr.cpp:1087` (card write). Never in VS. Item 12 (M6) material: the host's save is the only one written. |
| `gamePlayDataMemCard.cpp:227` (`PlayData::read`) | resets `mOsTime` on load | Same as above. |
| `memoryCard.cpp:595-596` (`MemoryCardMgr::doMakeHeader`) | date string in the card header | Save metadata (card file comment). |
| `pikmin2MemoryCardMgr.cpp:1993-1994` | date string in the card header | Save metadata. |
| `system.cpp:421` (`System::construct`) | boot seed for `srand` | Replaced by the session seed (`p2_net_boot_seed`, then `p2_net_session_begin` reseeds at the start barrier). |
| `system.cpp:743-745` (`System::getTime`) | milliseconds since boot | Debug timing only. Callers: `baseGameSection.cpp:344` (result discarded), `screenScene.cpp:64` (stored in `SceneBase::mSomeTime`, `screenObj.h:192`, which nothing reads), `screenScene.cpp:97` (result discarded). **Confirmed: no `getTime()` result reaches game logic.** |
| `JUTGamePad.cpp:307`, `:311` | B+X+START reset timer | Off in a session (M5.1a, `p2_net_pad_wallclock_off`). |
| `JUTGamePad.cpp:325`, `:329` | long-press timer | Off in a session (M5.1a). |
| `JFWDisplay.cpp:40`, `:357` | frame tick for the process bar and `mTickDifference` | Pacing only. `mTickDifference` feeds `calcCombinationRatio` (`:700-717`), which writes `mCombinationRatio`; **no code in `src/`, `include/` or `port/` reads `mCombinationRatio`** (`grep -rn mCombinationRatio` finds only the constructor, `:714-716` and the field). |
| `JFWDisplay.cpp:468-477` (`waitForTick`) | retrace pacing | Replaced by `p2_host_tick_wait` on PC; never read by game logic. |
| `JKRThread.cpp:158-176`, `JKRThread.h:71`, `:77` | per-thread CPU cost for the debug bar | Debug statistics; never read by game logic. |
| `JUTVideo.cpp:56`, `:87` | retrace interval statistics | Pacing statistics. |
| `JUTProcBar.h:25`, `:30` | process bar | Debug display. |
| `section.cpp:171` (`Section::main`) | `mOsTime` | Inside `#if defined(VERSION_US_DEMO)`: not built. |
| `THPPlayer.c:676-701` | `VIGetNextField` (field parity for interlaced movie frames) | Story movies only (M6: THP end barrier). |
| `system.cpp:286-293` (`retraceCallback`) | retrace counter for the CPU-lock detector | A panic only; the PC build resets the counter on every retrace (`PC_SWITCH(true, ...)`). |
| `JUTException.cpp:592-636`, `:833-836` | crash screen waits and its reset combo | Crash screen only (the seam stops at `p2_net_crash_stop`). |
| `sysTimers.cpp:88`, `:96` (`SysTimers::_start/_stop`) | (nothing) | Empty in the retail code. |
| `JASAiCtrl.cpp:141`, `JASProbe.cpp:37`, `:50` | audio DMA timing and audio profiling | Inside JAudio: audio thread only, see (b). |
| `port/game/net/dvd_sync.cpp:29` | the 30 s warning of the DVD wait | Port code; only prints a warning, never changes what the game does. |

`System::mDeltaTime` is not a clock read: `setFrameRate` (`system.cpp:1190-1191`) sets it from the
fixed frame-rate factor, so every tick advances game time by the same amount.

## (b) Audio coupling

Search, in every directory except `src/JSystem/JAudio` and `src/plugProjectHikinoU` (the sound
system itself):

```sh
grep -rn "PSGetDirectedMainBgm\|checkChildScene\|checkAudioStopStatus\|checkScene()\|PSMGetSceneMgrCheck" src include port
grep -rn "spSysIF" src/plugProject* src/sysGCU src/sysCommonU
grep -rn "JAISound\b\|JAISound\*" src/plugProject* src/sysGCU src/sysCommonU
grep -rn "getHandleP\|JAIStream\|JAISequence\|isPlaying\|getSeqData\|PSMGetPikminNumD" src/plugProject* src/sysGCU src/sysCommonU
grep -rln "\brand()\|randFloat\|randInt(" src/utilityU src/plugProjectHikinoU src/JSystem/JAudio*
```

`src/utilityU/PSMainSide_*.cpp` (the `PSM` sound glue) reads sound state everywhere, but only to
choose which sound to play; it is sound code and is covered by the rows that call into it. The last
search finds **no `rand()` in any sound code** (`utilityU`, `plugProjectHikinoU`, `JAudio`), so
starting or skipping a sound never moves the RNG.

With `MPPM_AUDIO=0` every JAI entry point returns early (`JAIBasic.cpp:251-508`,
`JAISeMgr.cpp:640`, `JAISequenceMgr.cpp:816`, `JAIStreamMgr.cpp:83`, `PSSeq.cpp:463`, `:576`,
`JAIBasic.h:137`), so sound handles are null instead of live. A read below is safe when the
result only decides whether to call the sound system.

| Site | What it reads | Verdict |
|---|---|---|
| `enemyBase.cpp:705`, `:733` | `PSGetDirectedMainBgm()` non-null | Gates `mSoundObj->battleOff/On()` only. |
| `enemyBase.cpp:1006`, `:1028`, `:1228`, `:1393`, `:2984` | `PSGetDirectedMainBgm()` non-null | Gates `battleOff()` only; the state changes around it (`mSfxEmotion`, `doBecomeCarcass`, `kill`) run either way. |
| `vsGS_Game.cpp:266-273` | `PSMGetPikminNumD()` non-null | Gates the low-Pikmin music director only. The condition itself (`getMapPikmins_exclude_Me`, `DeathMgr::mSoundDeathCount`) is game state. |
| `vsGS_Game.cpp:714-719` | the child sound scene (`isCave`) | Challenge Mode: gates `startPollutUpSe()` only. |
| `vsGS_Game.cpp:796-798` | `checkScene()` | Challenge Mode floor start: an assert on the scene manager, then `startMainSeq()`. Sound only. |
| `singleGS_MainGame.cpp:233-241` | `checkScene()` | Story: assert, then BGM calls. Sound only. |
| `singleGameSection.cpp:191-199` | `PSMGetSceneMgrCheck()` null, child scene | Story cave day end: returns early before `stopAllSound` when there is no sound scene manager; the pause and fader above it run first. Sound only. |
| `singleGS_Zukan.cpp:1399`, `:1403` | `checkScene()` | Piklopedia, story only. Sound only. |
| `blackMan.cpp:393-397` | `checkScene()`, `getSeqData(mgr, 0)` | Story (Waterwraith, cave `y_04`): asserts the sequence exists, stops it, then starts the movie. Sound only, but an `MPPM_AUDIO=0` run would hit the `P2ASSERTLINE(489, seqBase)` if the sequence data were missing. M6 note. |
| `blackMan.cpp:1681-1686` | `checkScene()`, `getSeqData(mgr, 1)` | Same, chase BGM. Sound only (same M6 note). |
| `blackMan.cpp:1975-1979` | `PSSystem::checkChildScene(scene)` non-null | Gates `startMainSeq()` only. |
| `blackMan.cpp:1030-1033` | `JAISound*` handle non-null | Gates `setPitch` on that handle only. |
| `ogObjKantei.cpp:487-491`, `ogObjSpecialItem.cpp:122-127` | child scene | 2D screens (treasure appraisal, special item): BGM restart only. |
| `challengeSelect2D.cpp:121-126`, `:180-185` | `JAISound*` handle non-null | Menu screen: gates `setPan` only. The menu is bypassed online (plan 2.3). |
| `titleSection.cpp:367-376`, `:480-524` | `getSeqData` | Title screen: stops the title BGM. Never in a session. |
| `bootSection.cpp:994-1001` | the global stream handle is null | Boot only: decides when the boot sequence moves on. Never in a session. |
| `reset.cpp:117`, `:183-186` (`ResetManager::isSoundSystemStopped`) | `spSysIF`, `JAIBasic::checkAudioStopStatus()` | Soft reset only; the reset is off in a session (M5.1a). |
| `system.cpp:920`, `commonSaveData.cpp:262`, `:287` | `spSysIF` non-null | Apply the volume and sound-mode options to the sound system. Sound only. |
| `hurryUp2D.cpp:178`, `dayEndCount.cpp:194` | `spSysIF` | Asserts in story 2D screens. |
| `moviePlayer.cpp:431-433` | `spSysIF` | Creates the JStudio audio object of a cutscene. The cutscene advances by frames (`mSecondsPerFrame` fixed at `:437`), not by sound position. Story only. |
| `pikmin2THPPlayer.cpp:214`, `:230`; `:390` (`isFinishLoading`) | the movie's audio track count and the sound mode; the DVD load | Track choice is sound only; `isFinishLoading` waits on the DVD command, which the M5.1a DVD wait makes deterministic. Story movies (M6 THP end barrier). |
| `port/game/ui_sound.cpp:66`, `:84` | `spSysIF`, `p2_audio_enabled()` | Launcher menu sounds. Not game code. |

**Result: no site gates simulation state on audio state.** No same-line fix is proposed. The
`audio-off` cells of the matrix (L10 and Le, the latter through a match end and a second load)
check this at run time; the M5.1a baseline was already identical with `MPPM_AUDIO=0` over 3600 and
18000 ticks. Two notes for M6, not M5.1b: the Waterwraith asserts above, and the THP and cutscene
paths, which belong to the story barriers.

**Integration finding (crash, not a desync).** The first integrated matrix run crashed in the
`audio-off` cell of `Le` (SIGSEGV at tick 2430, the 2P Battle result screen):
`kh::Screen::SceneWinLoseReason::doCreateObj` (`khWinLoseReason.cpp:189`) calls `PSMuteSE_on2D`
(`PSMainSide_Scene.cpp:1190-1195`), which calls `setVolume` through the BGM handle of sequence 0
without a null check. With `MPPM_AUDIO=0` no sequence plays, so the handle is null. On the
GameCube a write through address 0 does no harm; on PC it faults. `PSMuteOffSE_on2D` (`:1201-1206`)
has the same pattern. Both lines got a same-line guard, `PC_ONLY(if (se != nullptr))`. With audio on
the handle is live there, so behaviour and checksums do not change (the golden logs are identical
before and after the fix), and the `audio-off` cell of `Le` passes.

### Audio callbacks and `JKRHeap::sCurrentHeap`

Item 10 also asks whether JAudio callbacks allocate from `sCurrentHeap` (added in the M5.1b fix
round; the first pass left it out). `sCurrentHeap` is one global: nothing calls
`JKRThreadSwitch::createManager` or `OSSetSwitchThreadCallback`, so no thread has its own current
heap. A bare `new` on the audio thread would land in whatever heap the main thread had current at
that safe point, at a wall time that differs per machine.

What runs off the main thread: the host raises the audio DMA and DSP mail interrupts from SDL's
audio thread (`port/audio/audio.cpp:202`, `:600`), and the scheduler runs them at the next game
thread safe point (`port/sdk/os_thread.cpp:237` for alarms, `:249` for these interrupts).
`dmaInterrupt` (`audio.cpp:121`) only calls the AI DMA callback, which wakes `JASAudioThread`. That
thread runs the driver's callbacks: the sequencer (`JASTrack::rootCallback`, `JASTrack.cpp:1798`,
registered at `:1005`), which calls the sequence CPU callback (`seqCpuSync`, `PSGame.cpp:1029`,
registered through `PSSystemIF.cpp:137`, or `JAIBasic::setParameterSeqSync`, `JAIBasic.cpp:189`),
the stream callbacks (`JASAramStream::channelProcCallback` and `dvdErrorCheck`,
`JASAramStream.cpp:30`, `:163`) and `JAIBasic::stopCallBack` (`JAIBasic.cpp:780`, `:813`).

```sh
grep -rnE "(=|return|\() *new +[A-Za-z_:<>]+" src/JSystem/JAudio src/plugProjectHikinoU src/utilityU/PS*.cpp | grep -v "new ("
grep -rn "sCurrentHeap\|JKRGetCurrentHeap" src/JSystem/JAudio src/plugProjectHikinoU src/utilityU
grep -rn "JKRThreadSwitch::createManager\|OSSetSwitchThreadCallback" src port
```

| Allocation reached from those callbacks | Heap |
|---|---|
| A child track, `JASTrack::openChild` (`JASTrack.cpp:1162`) | `JASTrack::operator new` (`JASTrack.h:297`) takes a track from `sFreeList`, filled once from `JASDram` (`JASTrack.cpp:1858-1865`) |
| A channel (`JASBankMgr.cpp:156`, `:204`; `JASAramStream.cpp:792`) | `JASPoolAllocObject` pools: `JASGenericMemPool::alloc` (`JASHeapCtrl.cpp:521`) never grows; the pools are filled from `JASDram` (`:505`), and the pool object itself is created from `JASDram` (`JASGenericMemPool.h:46`) |
| DSP channels (`JASChannel.cpp:243`, `:260`) | a fixed table made at init from `JASDram` (`JASDSPChannel.cpp:64`) |
| `seqCpuSync`, `setParameterSeqSync`, `stopCallBack`, the AutoBgm and Rappa paths they call | none: they read and write existing objects (`seqCpuSync` reads one story flag, `PSGame.cpp:1049`, to choose music) |

The sound code that does allocate from `sCurrentHeap` (bare `new`, `JKRGetCurrentHeap()`) is all
scene and object setup on the main thread: `PSGame.cpp:70`, `:76`, `:234`; `PSSeq.cpp:147`, `:278`,
`:302`, `:721`, `:732`, `:932` (BGM and sequence objects, `DirectedBgm::init`); `PSAutoBgm.cpp:667`;
`PSDirector.cpp:22`, `:23`, `:283`; `PSMainSide_DirectorMgr.cpp` (the directors of a new scene);
`PSMainSide_TrackMap.cpp:19`; `PSSe.cpp:28-35`, `:476`; the JAI pools filled at start-up
(`JAISeMgr.cpp:51` through `SeSound::makeSeSound`, `PSMainSide_Sound.cpp:42`). These run in a fixed
order with the simulation.

**Result: no audio callback allocates from `sCurrentHeap`.** The audio thread allocates only from
`JASDram`, the sound system's own heap, and from pools made at start-up. No edit.

## (c) Effects and the RNG

```sh
F="$(git ls-files | grep -E '(^|/)efx[^/]*\.cpp$') src/plugProjectEbisawaU/particleMgr.cpp \
   src/plugProjectEbisawaU/particle2dMgr.cpp $(git ls-files 'src/JSystem/JParticle/*.cpp')"
grep -ln "\brand()\|randFloat\|randInt\|randWeightFloat" $F       # 27 files: no match
grep -rln "\brand()\|randFloat\|randInt\|randWeightFloat" include/efx/   # no match
grep -n "JPARandom\|setSeed" include/JSystem/JParticle/JPAMath.h src/JSystem/JParticle/JPAEmitter.cpp
grep -rn "getParticleNumber\|isEnableDeleteEmitter\|getEmitterNumber" src include | grep -v JParticle
```

- **No effect code calls the global `rand()`.** The 27 files are the ten `efx*.cpp`
  (`efx2dBase`, `efx2dEffect`, `efxBase`, `efxEnemy`, `efxEnemyBoss`, `efxEnemyGeneral`,
  `efxObject`, `efxPikmin`, `efxTPkEffectMgr` in `plugProjectEbisawaU`, `efxModelObjects` in
  `plugProjectKandoU`), `particleMgr.cpp`, `particle2dMgr.cpp` and the 15 files of
  `src/JSystem/JParticle`. `include/efx/` has no call either.
- **JPA has its own generator.** `JPARandom` (`JPAMath.h:19-24`) is an LCG whose seed starts at 0
  (`JPAMath.h:20`). Each emitter is seeded from its manager's work-data generator
  (`JPAEmitter.cpp:41`: `mRandom.setSeed(mManager->mWorkData->mRndm.getRandU32())`); the work data
  is created with the manager (`JPAEmitterManager.cpp:33`). Nothing in it touches the MSL stream,
  so creating, skipping or culling an emitter consumes no simulation randomness, even where the
  creation is gated on LOD.
- **No simulation read of emitter state.** The only reader of an emitter's end state outside
  JParticle is `JStudio_JParticle/object-particle.cpp:225` (cutscenes). Game objects keep emitter
  pointers to move or fade them, but no update decision reads a particle count or end flag.

Result: design 4.2 holds. At run time, `MPPM_RNG_AUDIT=1` on the integrated build must list no
caller inside `particleMgr->update()` (done test, step 2d).

## (d) RNG call sites by stream

Search (calls of `rand` and of the `rand.h` helpers that wrap it):

```sh
grep -rnoE '\b(rand|randFloat|randInt|randWeightFloat|randEbisawaFloat|RAND_FLOAT_RANGE|RAND_FLOAT_BETWEEN)[ ]*\(' \
  src include --include=*.cpp --include=*.h --include=*.c | grep -v 'Dolphin/rand.h\|PowerPC_EABI\|src/Dolphin'
```

This finds **641 call sites in 146 files** of game code, plus 16 in `port/game/scene` (the
launcher scene). (The design's figure of 659 counts with a slightly different pattern; the plan's
633 predates the launcher scene.) Each site was put in a class by its file and enclosing function.
The stream it uses follows from where it runs (design D1 to D3):

| Class | Sites | Where | Stream |
|---|---|---|---|
| Simulation | 432 | object and AI updates, VS game logic, item and pellet logic | SIM (main thread, outside every scope) |
| Cave generation | 55 | `plugProjectNishimuraU/RandMapMgr`, `RandMapUnit`, `RandMapScore`, `RandEnemyUnit`, `RandItemUnit`, `RandGateUnit`, `RandCapEnemyUnit`, `MapUnitGenerator` | DVD thread during a load: SIM under the hand-off of `p2_net_dvd_sync`, else OTHER |
| 2D screens and HUD | 79 | `Screen::Mgr` screens: `og*` HUD, `vsSelect2D`, `challengeSelect2D`, `challengeResult2D`, `hiScore2D`, `zukan2D`, `kh*` result screens | PRES (`Screen::Mgr::update/draw` scope) |
| Piklopedia | 20 | `singleGS_Zukan.cpp` (camera shake, enemy demo) | SIM, story only; never in a session |
| Outside any session | 53 | boot (`bootSection.cpp`, 20), title (`ebiP2Title*`, `ebiScreenTitleMenu`, `ebiScreenE3TitleMenu`, `ebiScreenPushStart`, `ogTitleMsg`, `TTitle.h`, `titleSection.cpp:721`), `demoSection.cpp`, cutscene text (`movieMessage.cpp`, 11), `ebi2DCallBack` | as their thread and scope say; never inside the deterministic window |
| Draw functions | 2 | `bootSection.cpp:720` (`drawNintendoLogo`) | PRES (`Section::draw` scope), boot only |

By directory:

| Directory | Sites | Simulation | Cave generation | Screens and HUD | Other |
|---|---|---|---|---|---|
| `plugProjectKandoU` | 215 | 195 | | | 20 Piklopedia |
| `plugProjectNishimuraU` | 212 | 157 | 55 | | |
| `plugProjectMorimuraU` | 119 | 54 | | 65 | |
| `sysGCU` | 35 | | | | 33 boot, title, cutscene; 2 boot draw |
| `plugProjectYamashitaU` | 26 | 26 | | | |
| `plugProjectEbisawaU` | 18 | | | | 18 title |
| `plugProjectKonoU` | 8 | | | 8 | |
| `plugProjectOgawaU` | 7 | | | 6 | 1 title |
| `include/ebi` | 1 | | | | 1 title |

The sites the plan names (section 4 item 3):

| Site | Runs in | Stream |
|---|---|---|
| `ogCounterRV.cpp:258`, `ogCounterSlot.cpp:156` | HUD counters, `Screen::Mgr` | PRES |
| `ogOtakaraSensor.cpp:125`, `:408`, `:416` | treasure sensor HUD, `Screen::Mgr` | PRES |
| `ogObjFloor.cpp:389` | floor title screen, `Screen::Mgr` | PRES |
| `singleGameSection.cpp:1143` (`calcOtakaraLevel`) | story update; the value is discarded | SIM, the same on every peer (deliberately not scoped) |
| `vsSelect2D.cpp:714` and the other 15 `vsSelect2D` sites; `challengeSelect2D` (14) | menus, `Screen::Mgr`; bypassed online | PRES |
| `pelletMgr.cpp:1963` (`Pellet::getPelletGoal`, the plan's `:1965`) | carry decision in the update | SIM (outcome-relevant: must stay SIM) |
| `port/game/scene/SceneTakeoff.cpp:1338` (the plan's `:1320`), `SceneDirector.cpp:124`, `:136`, `:230`, `:515`, `:628`, `:662`, `:731-765`, `:785`, `:894`, `:1366` | launcher scene, `SceneSection::doUpdate` | PRES (`P2RngPres` in port code) |
| `vsCardMgr.cpp:79-692` (11: card choice, slot machine) | VS game logic in the update | SIM (deliberately not scoped) |
| `vsGameSection.cpp:721-787`, `:1019`, `:1243-1281` | Pikmin fall and VS spawn, card drops | SIM |

Two limits of a static list. First, a sim function reached from a draw call (through a callee)
uses PRES at run time and is counted as `draw-unflagged`; the static list cannot see that, which
is why the RNG counters exist. Second, the class of a few screens depends on their caller
(`ogObjFloor` and the result screens run inside `Screen::Mgr`, which is what matters). The run-time
check is: the `mppm: net: rng` line of every matrix run shows `other-in-session=0`, the
`rng-perturb` cells (PRES and OTHER seeds changed) give identical checksums, and
`MPPM_RNG_AUDIT=1` on `ref_L10` lists the `draw-unflagged` callers, each of which must be drawing
or HUD code (done test, step 2d).
