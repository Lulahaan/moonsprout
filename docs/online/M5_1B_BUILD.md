# M5.1b build package: vendored libm, zero init, -Wreturn-type audit

Part of milestone M5.1b in `docs/ONLINE_MULTIPLAYER.md` (determinism checklist items 11 and 13).
It covers three things. The game's math goes to a vendored libm, so every machine computes the
same bits. Game heap memory and uninitialised locals read as zero. And there is an audit of the
functions that fall off their end without returning a value.

## 1. Vendored libm (`port/libm`, target `p2libm`)

**Source.** musl 1.2.5 `src/math` (MIT), byte for byte. Origin, sha256, the file list and the few
changes are in `port/libm/README.md`, and the licence is in `port/libm/COPYRIGHT`. Every function
is exported as `p2m_<name>`:
`sinf cosf tanf asinf acosf atanf atan2f expf logf log10f powf fmodf floorf ceilf lrintf sqrtf sin
cos tan asin acos atan atan2 exp log log10 pow fmod floor ceil ldexp frexp modf sqrt`
(`port/libm/names.txt`). `sqrt` and `sqrtf` are the IEEE instruction. The library is built as C
with `-ffp-contract=off -fno-builtin -fno-fast-math -fno-math-errno`. It never takes an FMA path
(`__FP_FAST_FMA` is undefined), has no long double and does no CPU dispatch.

**Routing the game.** The game library is routed at link level, with no `src/` edit. The pairs
`@P@<name> @P@p2m_<name>` in `cmake/p2_new_syms.txt.in` are applied to `libp2game.a` by the
existing `objcopy --redefine-syms` step, and this catches every way a call is spelled:
- C calls
- `std::` overloads
- libstdc++'s `__builtin_sinf`
- calls the compiler emits itself.

One game object depends on the generated list, so editing the list rebuilds and renames the
archive again. `P2_OPTIONS` gains:
- `-fmath-errno`, so that `__builtin_sinf` stays a call and does not become an `llvm.sin`
  intrinsic that LLVM folds with the build machine's libm;
- `-fno-builtin-<name>` for every inexact function (`sinf cosf tanf asinf acosf atanf atan2f expf
  logf log10f powf` and the double ones), so the compiler neither folds constant calls nor merges
  `sin` and `cos` into `sincos`.

**Routing Aurora's matrix library.** `aurora_mtx` is reached by simulation code: `MTXRotRad`,
`MTXRotAxisRad`, the quaternions and the camera's cull planes. It gets the forced header
`port/libm/p2m_route_mtx.h`, which reads `<math.h>` first and then defines `sinf` as `p2m_sinf`
and so on. It also gets `-ffp-contract=off` and links `p2libm`. The brief asked for
`-Dsinf=p2m_sinf` on the command line. A forced header has the same effect and cannot rename
names inside glibc's own `<math.h>` macros.

`fmaf` (`vec.c`) stays the host's. IEEE-754 requires it to be correctly rounded, so every libm
returns the same bits.

**`include/trig.h:25`, `:41`, `:74`.** These lines get `PC_ONLY(__attribute__((always_inline)))`.
Gameplay `sinf`, `cosf` and `acosf` are table look-ups defined inline there. Without this, an
unoptimised build could emit an out-of-line `sinf`, and the rename would turn it into a second
`p2m_sinf`.

**Build check.** `cmake/p2_libm_check.cmake` runs after the objcopy step, through the
`p2libm_check` target that `mppm` depends on. It fails the build in two cases:
- `libp2game.a` or `libaurora_mtx.a` imports any C99 `<math.h>` function (double, float and long
  double forms, plus `sincos`, `exp10` and other GNU extras; the list is
  `cmake/p2_libm_names.txt`);
- `libp2game.a` defines one, or defines a `p2m_` name.

The check reads plain `nm` output, which works with GNU, LLVM and Apple nm. It takes about 0.5 s.

**A/B option.** `MPPM_VENDORED_LIBM=OFF` (default ON) gives back the system libm and the old
compiler flags, for A/B runs only.

**Self-test.** Build it with `ninja -C build-pc p2libm_selftest`, then run
`./build-pc/p2libm_selftest port/libm/selftest_expected.txt`. For every function it prints:
- `p2m <name> <hash>`, an FNV-1a 32 hash of the vendored function's result bits over 4,000,000
  fixed inputs (the generator of `m51b_libm_probe.c`);
- `host <name> <hash>`, the same for the host function;
- `ulp <name> max= one= more=`, the distance in ulps between the two.

It exits 1 when a `p2m` hash differs from the file.

**Offline effect.** The listed functions, and the JMath sine, cosine and arcsine tables built from
them at start-up, may differ from glibc's in the last bit. This changes simulation results offline
(in the 18000-tick log, the captain's position hash first differs at tick 2160). A difference of
one ulp is far below anything a player can see or feel, so this is behaviourally equivalent.

## 2. Zero on alloc and auto-variable init

- `src/JSystem/JKernel/JKRHeap.cpp:204` (same-line edit):
  `return PC_SWITCH(p2_heap_zero(do_alloc(byteCount, padding), byteCount), do_alloc(byteCount, padding));`.
  `p2_heap_zero` is declared in `port/include/p2_host.h` and defined in `port/sdk/heap_zero.cpp`
  (a `memset` when the block is not null). Every game heap allocation passes through
  `JKRHeap::alloc`, which covers `operator new` (`p2_heap_alloc`) and raw `JKRHeap::alloc` calls.
- `-ftrivial-auto-var-init=zero` is added to `P2_OPTIONS` when `check_cxx_compiler_flag` accepts
  it. clang 18 and later, and Xcode 16, take it without the old enable flag.
- **Blocks that become heaps** are cleared only up to their header (M5.1b fix round).
  `JKRExpHeap::create` and `JKRSolidHeap::create` call `p2_heap_zero_limit(header size)` right
  before they allocate their block from the parent (same-line edits `JKRExpHeap.cpp:48`,
  `JKRSolidHeap.cpp:26`), and the next `p2_heap_zero` on that thread clears only that much. The
  rest of the block is handed out only by the new heap's own `alloc`, which zeroes each block
  again, and the heap code writes its own block headers. Before this, every heap the game makes
  with `getFreeSize()` (each section in `gameflow.cpp:87`, `mDisplayHeap` in `section.cpp:54`,
  `:57`, the cave, file select, Piklopedia and VS title and result heaps) cleared the whole free
  arena, up to 256 MiB, and committed every page of it. The golden logs are unchanged.
- `MPPM_ZERO_INIT` (default ON) switches both parts. With OFF, `heap_zero.cpp` is built with
  `MPPM_ZERO_INIT=0` and returns the block as it is.

**Offline effect.** Memory from the game heaps and uninitialised locals now read as zero. The
previous contents (whatever an earlier owner left) were never something the game meant to read, so
this is behaviourally equivalent. See section 4 for the one place where it shows.

## 3. -Wreturn-type audit (design D18)

The list has 119 sites (clang 23.1.5): [M5_1B_WRETURN.txt](M5_1B_WRETURN.txt). The 69 sites
outside JAudio, JMessage and JStudio are below; the 50 in those three libraries follow in
[their own subsection](#jaudio-jmessage-and-jstudio) (added in the M5.1b fix round: the first pass
left them out, although JStudio drives cutscenes, which are simulation events). "Callers"
means calls of this exact function and overload in `src/`, `include/` and `port/game/`. A call
through another overload or class with the same name does not count.

Result: **no site has a reachable simulation caller that uses the returned value.** Two sites are
used by simulation code but fall off only after `JUT_PANIC`, which suspends the thread and never
returns. One draw-side use is harmless. So no `PC_ONLY(return x;)` edit is needed, and none is
applied.

### Sites with callers

| Site | Function | Caller uses the value? | Simulation? | Verdict |
|---|---|---|---|---|
| `gamePlayData.cpp:903` | `PlayData::isPelletEverGot(Pellet*)` | no caller of this overload | - | unused |
| `gamePlayData.cpp:920` | `PlayData::isPelletEverGot(u8, u8)` | yes, `enemyBase.cpp:1163` (`setOtakaraCode`) | yes | falls off only after `JUT_PANICLINE` (type neither Upgrade nor Treasure): never reached. No edit |
| `itemMgr.cpp:654` | `BaseItemMgr::loadPlatAttacher` | yes, `itemBridge.cpp:840-852` | load | falls off only after `JUT_PANICLINE` (file missing). No edit |
| `sysGCU/graphics.cpp:256` | `Viewport::setJ3DViewMtx` | `baseGameSectionDraw.cpp:74`: `vp->setJ3DViewMtx(true)->setViewCalcModeImm()` | draw | `setViewCalcModeImm` is `static` (`SysShape/Model.h:43`): the pointer is never dereferenced. The other 6 callers discard it. No edit |
| `system.cpp:254` | `System::assert_fragmentation` | no, 9 statement calls | init | no edit |
| `vsGameSection.cpp:1026` | `GameMessageVsBirthTekiTreasure::actVs` | through `VsGameSection::sendMessage`; its only sender `itemTreasure.cpp:151` discards the result | VS sim | no edit |
| `singleGS_MainGame.cpp:244` | `SingleGame::GameState::gameStart` (`unknown` = u32) | no, `:741` statement | story | no edit |
| `gamePlayData.cpp:1570` | `PlayData::doneWorldMapEffect` | no, `singleGS_WorldMap.cpp:316` statement | story | no edit |
| `include/Game/VsGame.h:408` | `VsGame::GameState::getMarbleLoss` | no, `vsGS_Game.cpp:1024` statement (outputs go through the references) | VS | no edit |
| `ogObjVs.cpp:169` | `og::Screen::ObjVs::startGetBdama` | no, `:277`, `:297` statements | 2D | no edit |
| `ebiP2Title.cpp:171` | `TTitleMgr::setStartPosToPiki` (returns `Vector2f`) | no, `:478` statement | title | no edit |
| `ebiP2Title.cpp:390` | `TTitleMgr::inViewField(Vector2f&, f32)` | no, `:399` statement | title | no edit |
| `ebiP2Title.cpp:784` | `TTitleMgr::update` | no caller uses it | title | no edit |

### Sites with no caller

Each of these is either a stub marked `// UNUSED FUNCTION` in the decomp, or has no call of that
exact overload. A grep of `src/`, `include/` and `port/game/` found no call.

| Site | Function | Note |
|---|---|---|
| `J2DPane.cpp:551` | `J2DPane::getRotate` | stub |
| `JFWDisplay.cpp:124`, `:133`, `:142` | `JFWDisplay::createManager` (3 overloads) | every caller uses the 4-argument overload |
| `linklist.cpp:113` | `TNodeLinkList::Find` | stub; the template wrapper `linklist.h:305` is never called |
| `std-list.cpp:144`, `:155` | `TList_pointer_void::operator=`, `TList::operator=` | stub; no list is assigned |
| `JKRArchivePri.cpp:118` | `JKRArchive::findTypeResource(u32, u32)` | callers use `(u32, const char*)` |
| `JKRArchivePub.cpp:16` | `JKRArchive::check_mount_already(s32)` | callers use `(s32, JKRHeap*)` |
| `JKRFileCache.cpp:166`, `:221` | `getRelResource`, `readRelResource` | stub |
| `JPAEmitter.cpp:217` | `JPABaseEmitter::getDrawCount` | stub |
| `JPAParticle.cpp:366`, `:375` | `JPABaseParticle::getWidth/getHeight(const JPABaseEmitter*)` | stub |
| `JUTException.cpp:883` | `JUTException::getFpscr` | stub |
| `ebiCardMgr.cpp:577` | `ebi::Card::TMgr::isFinish` | stub; the `isFinish` calls go to other classes |
| `ebiGeometry.cpp:14` | `EGEBox2f::isIn` | stub |
| `ebiMainTitleMgr.cpp:214` | `TMainTitleMgr::isAnyKey` | stub |
| `ebiOmakeMgr.cpp:351` | `TMgr::isTouchAnyKey_` | stub |
| `ebiScreenMemoryCard.cpp:619` | `TMemoryCard::isDecide` | stub; callers use the title-menu classes |
| `efx2dEffect.cpp:49` | `T2DSensorAct_forVS::create` | stub |
| `baseGameSection.cpp:1228`, `:1237`, `:2167` | `getNumWindows`, `getActivePlayerID`, `isAllocHalt` | stub |
| `creatureStick.cpp:205` | `Creature::isStickLeader` | stub; `aiTransport.cpp` calls `ActTransport::isStickLeader` |
| `gameGenerator.cpp:557` | `GeneratorMgr::isRootMgr` | stub |
| `gamePlayData.cpp:1033` | `PlayData::getTekiCarcassMoney` | stub |
| `gamePlayData.cpp:1176`, `:1462` | `isCaveFirstReturn`, `closeCourse` | empty |
| `itemBridge.cpp:451`, `:715` | `ItemBridge::Item::getStageDepth`, `getWorkRadius` | stub, non-virtual |
| `itemUjamushi.cpp:1098`, `itemWeed.cpp:339` | `Mgr::birth()` | stub, non-virtual; nothing calls it |
| `naviMgr.cpp:280` | `NaviMgr::getSurviveNavi` | stub |
| `pathfinder.cpp:152` | `Pathfinder::makepath(u32, s16*, int)` | callers use `(u32, PathNode**)` |
| `pelletMgr.cpp:3487` | `PelletMgr::getMgrByIndex` | stub; `genItem.cpp` calls `ItemMgr::getMgrByIndex` |
| `vsCardMgr.cpp:225`, `:656` | `CardMgr::getPlayerCard`, `SlotMachine::goodPlace` | empty |
| `challengeSelect2D.cpp:440` | `TChallengePanel::canSelect` | stub |
| `jigumo.cpp:992` | `Jigumo::Obj::isUnitePos` | stub |
| `zukan2D.cpp:3016` | `TItemZukan::isCategoryComplete` | stub |
| `ContRumble.cpp:68` | `RumbleNode::isSameLabel` | stub |
| `geomCylinder.cpp:55` | `Cylinder::intersect(const Plane&)` | stub; `mapMgrTraceMove.cpp` uses the triangle overload |
| `geometry.cpp:896` | `Triangle::intersectOptimistic` | stub |
| `geomOBBTree.cpp:54`, `:63`, `:72`, `:81` | `OBB::calcPointDist`, `OBB::intersect` (3) | stub |
| `stream.cpp:524` | `Stream::readFixedString` | no caller |
| `sysMath.cpp:123` | `CRSpline` | stub |
| `graphics.cpp:396` | `Graphics::getToken` | stub |
| `moviePlayer.cpp:366` | `MoviePlayer::hasSuspendedContext` | stub |
| `pikmin2AramMgr.cpp:119` | `PSGame::Aram::Mgr::isEntryOnly` | stub |
| `sysShape.cpp:500` | `AnimMgr::load(Stream&, Model*, JKRFileLoader*, char*)` | callers use other overloads |
| `titleSection.cpp:851` | `TitleDummy::Section::doUpdate` | stub |
| `PSMainSide_ObjSound.cpp:87` | `PSM::Creature::isVisible` | stub; `pikiAI.cpp:727` calls `ItemTreasure::Item::isVisible` |

### JAudio, JMessage and JStudio

The same check over the other 50 sites. 45 are stubs (an empty body, usually `// UNUSED
FUNCTION`); five have a real body that falls off its end. Result: **no site has a caller that uses
the returned value**, so nothing is edited here either.

| Sites | Functions | Callers |
|---|---|---|
| `JStudio/functionvalue.cpp:118`, `:127`, `:136`, `:145`; `JStudio/stb.cpp:36` | `interpolateValue_BSpline_uniform_first1/first2/last1/last2`, `stb::TObject::toString_status` | none (stubs). The cutscene paths use the other interpolation functions, which return a value |
| `JAudio/JAI/JAIBankWave.cpp:223`, `:232`; `JAIGlobalParameter.cpp:633`, `:642`, `:651`, `:808`, `:827`, `:836`, `:965`, `:974`; `JAISystemInterface.cpp:283`, `:292` | `BankWave::getWaveGroupNumber/getWaveLoadStatus`, eight `getParam*` getters, `getAudioThreadPointer`, `getDvdThreadPointer` | none (stubs) |
| `JAudio/JAS/JASBankMgr.cpp:43`, `:245`; `JASCallback.cpp:47`; `JASCmdStack.cpp:26`, `:35`; `JASDriverIF.cpp:124`; `JASDSPInterface.cpp:368`, `:378`; `JASHeapCtrl.cpp:159`, `:226`, `:235`, `:449`, `:629`, `:638`; `JASTrack.cpp:1881`, `:1968`, `:1977`; `JASWSParser.cpp:130` | `registBank`, `getUsedHeapSize`, `JASCallbackMgr::reject`, `addPortCmdStay`, `cancelPortCmdStay`, `getDSPLevel`, `TChannel::getBlockCounter/getRemainSamples`, `JASHeap::allocAll/getTotalFreeSize/getFreeSize`, `JASSolidHeap::alloc`, `getAramFreeSize`, `getAramSize`, `JASTrack::getFreeMemCount/getChannelCount/getReleaseChannelCount`, `JASWSParser::getUsedHeapSize` | none (stubs). The callers of similar names go to `registBankBNK`, `getDSPLevel_f32` and `JKRHeap` |
| `JAudio/JAS/JASTrack.cpp:187` | `JASTrack::inherit` (real body) | `JASTrack.cpp:1173` (`openChild`), as a statement: the value is dropped. Sound only |
| `JMessage/processor.cpp:94`, `:171`, `:181`, `:483`, `:493`, `:589`, `:623`, `:633`, `:853`, `:863` | `popCurrent`, `on_select_end`, `on_select_separate`, `toString_status`, `toValue_status`, `on_jump_register`, `on_branch_register`, `on_branch_query`, `process_setMessageIndex_reserved_`, `process_setMessageCode_` | none (stubs); `processor.cpp:537` calls `on_branch_queryResult`, which returns a value |
| `JMessage/processor.cpp:161`, `:315`, `:815`, `:1020` | `TProcessor::on_select_begin`, the three `do_tag_` (real bodies) | `processor.cpp:307`, `:310` and `TProcessor.h:160`, as statements: the value is dropped. Text boxes, not simulation |

The build keeps `-fno-strict-return` (`CMakeLists.txt`). A function that falls off its end returns
an unspecified value instead of running into the next function, which already makes all of these
sites safe.

## 4. Test results (clang 23.1.5, Ryzen 7900X3D, glibc 2.44)

The log is `netlog.py synth --seed 20261122 --synth-seed 7 --ticks 18000 --vs-stage 0` (version
1). All runs are headless at 1280x960 with audio on.

| Build | Final checksum | Against the base build (`193d13937`) |
|---|---|---|
| base | `6d78be60` | - |
| vendored libm + zero init (default) | `8f56e583`, the same in 4 runs and with `GLIBC_TUNABLES=glibc.cpu.hwcaps=-AVX2,-FMA` | differs at tick 2160, field `navi` |
| zero init only (`MPPM_VENDORED_LIBM=OFF`) | `6d78be60` | identical (601 lines) |
| vendored libm only (`MPPM_ZERO_INIT=OFF`) | `64ccbefe`, the same in 2 runs | differs at tick 2160, field `navi` |
| vendored libm + heap zero, no auto-var-init (temporary build) | `8f56e583` | the same as the default |

So the vendored libm causes the difference from the base. The last-bit results of the math
functions move the captain at tick 2160. Zero init alone changes nothing on the base trajectory.

On the vendored-libm trajectory, however, `MPPM_ZERO_INIT=OFF` differs from ON at **tick 8310,
field `piki`**. The heap-only build matches ON, so the difference comes from **heap memory**, not
from stack locals. Pikmin code reads a field of a heap block it never wrote; in an OFF build that
field holds whatever the previous owner of the block left. Each OFF build repeats itself, because
the allocation sequence is the same, but two machines or two builds need not agree. Zero on alloc
removes this. Finding the exact field is left open: see [the memory checker pass](#memory-checker-pass-section-4-item-13)
below.

Headless wall time for the 18000 ticks: ON 22.3, 25.0, 27.3 and 23.9 s; OFF 22.9 and 25.8 s. The
run-to-run spread is larger than the cost of zeroing.

### Memory checker pass (section 4 item 13)

**Not done; open.** Item 13 asks for one MSan or Valgrind pass over a VS match. Neither tool can
run here: the build container has no Valgrind and clang's MSan runtime is not installed, and MSan
would also need SDL, Aurora, the Vulkan loader and every other dependency rebuilt with it. Valgrind
alone would not find the `piki` read of tick 8310 either: the game heaps are one 256 MiB arena, and
memcheck sees a block reused inside it as defined unless the allocator marks it (a
`VALGRIND_MAKE_MEM_UNDEFINED` client request in `p2_heap_zero` for an `MPPM_ZERO_INIT=OFF` build
would do that). Zero on alloc makes the read return 0 on every machine, so it is not a desync in
the default build; the pass is still owed to find the field. Recorded in the open items of
[M5_1A_HARNESS.md](M5_1A_HARNESS.md#recorded-figures-m51b).
