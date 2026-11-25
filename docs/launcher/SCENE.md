# Live start-menu scene

Spec for the 3D scene behind the start menu: Olimar and three Pikmin idling at the landing site with
the ship behind them, rendered by the game engine from the player's own disc, ending in a take-off
when Play is pressed.

Paths are relative to the repo root. `disc:` means a path inside the disc image
(`build/GPVE01/disc/files/` in the dev extract). Every disc resource below was checked by listing the
archive; animation and particle facts were read from the files themselves (keyframes, STB blocks).
Values marked **tune** are first guesses to adjust while looking at the picture.

## Review notes

Changed in the cross-spec review (see `README.md` in this folder):

- 4.2: the bridge gained take-off cues, the ship's screen position and a fade colour, which the
  launcher UI choreography (DESIGN.md section 7) needs.
- 4.3: the menu's exit animation is DESIGN.md 7.2, not a flat 0.3 s fade. A failed scene keeps the
  section alive until Play instead of ending it (the title would otherwise start under the backdrop).
- 8.2: DESIGN.md 6.2 is the composition target; tune at 16:9 first.
- 11: one fade colour shared with the UI flash; cue points marked.
- 12: the settings are named (SETTINGS.md 9.1). "Still backdrop" means the game is not booted until
  Play, as SETTINGS.md says.

---

## 1. Decisions

1. **A new, small game section**, `LauncherScene::Section`, derived from `Game::BaseHIOSection`
   like `Demo::Section` (`src/sysGCU/demoSection.cpp:47-173`). It does not use `BaseGameSection`,
   `gameSystem`, `naviMgr`, `pikiMgr`, `mapMgr` or `playData`. Those need a save file, generators and
   the whole creature system, and none of it is needed to pose four characters.
2. **The rendering pattern is the title screen's** (`ebi::title::TTitleMgr`,
   `src/plugProjectEbisawaU/ebiP2Title.cpp:91-135, 790-848, 896-935`): two `J3DDrawBuffer`s, one
   `Viewport` + `LookAtCamera`, a `LightMgr` with `LightObj`s, GX fog, `calc/entry/viewCalc` per model.
3. **Models are loaded exactly as gameplay loads them** (same archives, same loader flags), because
   those paths are already proven on the PC renderer.
4. **The location is a real area map**, drawn without collision: Awakening Wood by default, with the
   ship at its real landing spot. Later: all four areas, picked by the real-world season.
5. **The take-off uses Nintendo's own authored animation from the disc**: `s02_ufo.bck` (ship
   rumble and lift-off, 199 frames) with the day-end jet, smoke and leaf particles
   (`disc:user/Mukki/movie/s02_dayend_result/demo.szs`). Olimar boards through the ship's tractor beam,
   as he does at sunset in the game (`s01_dayend` suck particles).
6. **Characters are driven by a small behaviour scheduler** (a director plus one state machine per
   actor) that plays clips from the shared Pikmin/captain animation set (67 clips in
   `pikis.szs/motion`).
7. **Code location: `port/game/scene/`**. It is picked up by the `PORT_SOURCES` glob
   (`CMakeLists.txt:91-93`), so it compiles with the decomp headers and `P2_DEFINES`, and it is not in
   `cmake/sources.cmake`/`configure.py`, so the MWCC matching build never sees it.
8. **Game-code edits are three small `#if TARGET_PC` hooks** (section 4). Everything else is new files.
9. **Flow**: Boot section (unchanged loading) → launcher scene → Play → take-off → fade → the
   normal title screen ("Press Start") → file select. `GameFlow` already continues to
   `SN_MainTitle` after any child section (`src/sysGCU/gameflow.cpp:171`).
10. **Shadows reuse the game's shadow cylinders** (`Game::ShadowCylinder2`) driven directly with
    `ShadowParam`s, without `Creature` objects.
11. **Music**: Piklopedia theme (`book.bms`) by default through an unused sound scene type; a setting
    offers other tracks or none.
12. **No Nintendo data is shipped.** Everything is read from the mounted disc at run time. Before a
    disc exists the launcher shows its own backdrop (host side, not part of this spec).

---

## 2. What the game already provides

### 2.1 Sections and flow

| Fact | Where |
|---|---|
| `GameFlow::run` loops: make heap, `setSection`, `init/run/exit`. Boot first, then `RootMenuSection` forever. | `src/sysGCU/gameflow.cpp:77-121` |
| `RootMenuSection` runs child sections in a loop; each gets a fresh `JKRExpHeap` that is destroyed afterwards. | `src/sysGCU/menuSection.cpp:19-58`, `rootMenuSection.cpp:26-29` |
| The child is chosen by `GameFlow::createSection` from `mActiveSectionFlag`, which is then reset to `SN_MainTitle`. | `src/sysGCU/gameflow.cpp:147-173` |
| `Section::run` = `loading()` (calls `doLoadingStart`, then `doLoading` each frame), `fadeIn()`, `main()` (draw + update until `mIsMainActive` is false, then starts the fade-out and stops the music), `fadeOut()`. | `src/sysGCU/section.cpp:120-271` |
| Minimal section template: constructor, `init` (display, controller, frame rate), `doLoadingStart` with a DVD-thread delegate, `doLoading`, `doUpdate`, `doDraw`, `doExit`. | `src/sysGCU/demoSection.cpp:47-173` |
| Title: sound scene creation, `doStartMainSeq`, resident archive mount (`pikis.szs`, `onyon/arc.szs` into `sys->mSysHeap`). | `src/sysGCU/titleSection.cpp:111-153, 675-712` |
| Boot does the system loads (`loadResourceFirst/Second`, memory card, ARAM) and shows the logos. | `src/sysGCU/bootSection.cpp:946-1007, 1141-1152` |
| The launcher UI is updated and drawn from `p2_host_present`, once per game frame, on the game thread. | `port/main.cpp:90-116` |

### 2.2 Models and animation

| Fact | Where |
|---|---|
| Olimar: `orima_model/orima1.bmd`, flags `J3DMLF_Material_PE_FogOff \| J3DMLF_UsePostTexMtx \| J3DMLF_UseImmediateMtx`, every shape `setTexMtxLoadType(0x2000)`, model = `new SysShape::Model(data, 0, 2)`. | `src/plugProjectKandoU/naviMgr.cpp:190-231` |
| Animation set: `SysShape::AnimMgr::load(texts, "animMgr.txt", model, arc, "motion")`. `NaviMgr::animMgr` is a static that the `NaviMgr` constructor resets, so the scene must own a separate `AnimMgr`. | `naviMgr.cpp:13, 35, 209`, `src/sysGCU/sysShapeAnimation.cpp:37-79` |
| Pikmin: `piki_model/piki_p2_{red,yellow,blue}.bmd`, flags `J3DMLF_UseSingleSharedDL \| J3DMLF_18 \| J3DMLF_UseImmediateMtx`, then `newSharedDisplayList` + `makeSharedDL`. Per model: `calcMaterial(); makeDL(); lock();`. | `src/plugProjectKandoU/pikiMgr.cpp:178-252` |
| Leaf/bud/flower: `happa_model/{leaf,bud,flower}.bmd`, drawn by hand at joint `happajnt3`. | `pikiMgr.cpp:196-223, 319-363`, `piki.cpp:170` |
| Captain and Pikmin share one skeleton (11 joints: `kosinull, legcentre, llegjnt, rlegjnt, sebonjnt, headjnt, happajnt1-3, lhandjnt, rhandjnt`) and one clip set, enum `IPikiAnims`. | `include/Game/FakePiki.h:19-90` |
| Per frame: `animator.animate(30 * dt)`, `animator.setModelCalc(model, jointIdx)`, copy the TR matrix into `mJ3dModel->mPosMtx`, `calc()`, later `entry()`. | `src/plugProjectKandoU/fakePiki.cpp:805-880, 783-799` |
| Head turning: a joint callback on `headjnt` multiplies in a yaw/pitch rotation. | `fakePiki.cpp:311-345`, `piki.cpp:1313-1314` |
| Ship: `disc:user/Kando/ufo/arc.szs` `ufo.bmd`, flags `J3DMLF_Material_PE_FogOff \| J3DMLF_Material_UseIndirect \| J3DMLF_18 \| J3DMLF_UsePostTexMtx \| J3DMLF_UseImmediateMtx`; material anims `ufo.brk`, `ufo_2.brk`; AnimMgr from `ufo/texts.szs` `animmgr.txt` (ids 0 `suction`, 1-3 `pmotion1-3`). | `src/plugProjectKandoU/onyonMgr.cpp:2255-2302` |
| Ship part motion: three extra `Animator`s on joints `pmotion1/2/3`, speeds 30/0/30. | `onyonMgr.cpp:1818-1848, 1883-1905` |
| Area map: `model.bmd` from `<folder>/arc.szs`, flags `J3DMLF_Material_PE_FogOff \| J3DMLF_UseSingleSharedDL`, shared DL, `new SysShape::Model(model, J3DMODEL_CreateNewDL, 2)`, `calc/calcMaterial/makeDL/lock`; `texanm_N.btk` texture animations. | `src/plugProjectKandoU/mapMgr.cpp:283-360` |
| Extra clips outside an `AnimMgr`: `AnimInfo::attach(J3DModelData*, void*)` + `Animator::startExAnim(AnimInfo*)`. | `include/SysShape/AnimInfo.h:44`, `include/SysShape/Animator.h:50` |

### 2.3 Light, fog, shadow, particles, sound

| Fact | Where |
|---|---|
| Light rig used by all gameplay materials: main `GX_LIGHT0` spot (spot fn off), sub `GX_LIGHT1`, specular `GX_LIGHT7` (`mKScale` 40), ambient, `FogMgr` linear. | `src/plugProjectYamashitaU/gameLightMgr.cpp:386-436` |
| Area colours per time of day (night/morning/day/evening: main, sub, specular, ambient, fog colour, fog near/far, shadow colour) are in `disc:user/Abe/map/<area>/light.ini`, parsed by `Game::GameLightMgrSetting::read`, which is a plain `Parameters` reader. | `gameLightMgr.cpp:152-175`, `include/Game/GameLight.h:245-259` |
| Sun position: angle between `mSunriseAngle` (135) and `mSunsetAngle` (45) in the XY plane at `mDistance` (25000). | `gameLightMgr.cpp:800-822` |
| `GameLightMgr` itself dereferences `mTimeMgr` and `gameSystem`, so it cannot be used here. | `gameLightMgr.cpp:488, 815` |
| Fog: `FogMgr::set(Graphics&)` reads `gfx.mCurrentViewport->mCamera`. | `src/sysGCU/fogMgr.cpp` |
| Shadow volumes: `CylinderBase::setCameraParms`, `makeSRT(mtx, ShadowParam)`, `getCylinderType`, `setShadowRect`, `setFilterTextureID`, `drawInit`, `drawCylinder`, `drawFinish`. `ShadowParam.mBoundingSphere.mPosition` is the light direction (shear), `.mRadius` the height, `mSize` the radius. | `src/plugProjectNishimuraU/ShadowCylinder.cpp:174-230`, `shadowMgr.cpp:210-310` |
| Shadow sizes in the game: captain radius 4 / height 10, Pikmin 2 / 10, both lifted 0.5. | `src/plugProjectKandoU/navi.cpp:42-51`, `piki.cpp:1152-1164` |
| `ShadowCylinder2` allocates per-viewport copy textures and tolerates `gameSystem == nullptr`. | `ShadowCylinder.cpp:380-410` |
| 3D particles: `ParticleMgr::globalInstance`, `createHeap`, `createMgr("user/Ebisawa/effect/game.jpc", 2000, 300, 0x80)`, `setViewport`, `update`, `draw(vp, group)`. Demo effects: `setDemoResourceManager(new JPAResourceManager(file, heap))` registers a second resource set as id 7, `createDemo(id, pos, group, 7)`. | `src/plugProjectEbisawaU/particleMgr.cpp:33-116, 198-265, 336-348`, `src/sysGCU/moviePlayer.cpp:456-464`, `src/plugProjectKandoU/baseGameSection.cpp:2081-2083, 1425-1454` |
| `ParticleMgr::update` calls `pkEffectMgr->resetContextS()`, so `particleMgr->Instance_TPkEffectMgr()` (a member function) must run first. | `particleMgr.cpp:119-122, 198-202` |
| Sound scene: `PikSceneMgr::newAndSetCurrentScene(info)` → `initMainBgm` (switch on scene type) → `newGameScene`. Types without a case in `newGameScene` become `Scene_NoObjects`. `TITLE_18` is unused. | `src/plugProjectHikinoU/PSGame.cpp:655-712, 823-1003`, `src/utilityU/PSMainSide_Factory.cpp:279-320`, `include/PSGame/PikScene.h:51` |

---

## 3. Disc resources

### 3.1 Archives

| Archive (`disc:user/…`) | Used entries | Size |
|---|---|---|
| `Kando/piki/pikis.szs` | `orima_model/orima1.bmd`; `piki_model/piki_p2_red.bmd`, `piki_p2_yellow.bmd`, `piki_p2_blue.bmd`; `happa_model/leaf.bmd`, `bud.bmd`, `flower.bmd`; `motion/*.bca` (78 files; 67 are listed in the AnimMgr table, `aogu.bca` and others are not) | 354 KB |
| `Kando/piki/texts.szs` | `animmgr.txt` (the game asks for `animMgr.txt`) | 2 KB |
| `Kando/ufo/arc.szs` | `ufo.bmd`, `ufo.brk`, `ufo_2.brk`, `pmotion1.bck` (100 f), `pmotion2.bck` (30 f), `pmotion3.bck` (30 f), `suction.bck` (30 f) | 123 KB |
| `Kando/ufo/texts.szs` | `animmgr.txt` | 1 KB |
| `Kando/map/forest/arc.szs` (default area) | `model.bmd` (1.9 MB unpacked), `texanm_1.btk` | 1.7 MB |
| `Abe/map/forest/light.ini` | time-of-day colours | text |
| `Mukki/movie/s02_dayend_result/demo.szs` | `s02_ufo.bck` (199 f), `efx.jpc` (9 emitters), optionally `s02_onyon{r,y,b}.bck`, `s02_bg.bmd` (sky backdrop, radius 3549) | 66 KB |
| `Mukki/movie/s01_dayend/demo.szs` | `efx.jpc` (5 emitters: tractor beam) | 83 KB |
| `Mukki/movie/s00_coursein/demo.szs` | `efx.jpc` (16 emitters: ambient petals/snow/leaves, dust); optional arrival intro `s00_ufo.bck` (261 f), `s00_alive1.bck` | 57 KB |
| `Ebisawa/effect/game.jpc` | walking dust `PID_PkS_WalkSmoke` (0x179), captain antenna light `PID_OrimaLight_Orima_1/2` (0x123, 0x126) | 555 KB |
| `Kando/onyon/arc.szs` (optional, phase 5) | `goal.bmd`, `kidou_{red,yellow,blue}.brk`, `flowwait.bck` | 30 KB+ |

Other areas use `Kando/map/{tutorial,yakushima,last}/arc.szs` and `Abe/map/<area>/light.ini`.

### 3.2 Ship joints (`ufo.bmd`, 25 joints)

`model, ctr, body_s (0,60,0), cocpit1_cov, cocpit2_cov, jet_eff (0,-50,0), loosy, orima (0,2,17),
pmotion1, base, piston3, piston4, rader, pmotion2, inexhaust, in1, outexhaust, out, potjnt, pot_top,
pmotion3, speaker, mikeeff, goal (0,122.8,15.3), start1 (0,0,137)`.

The ship is 137 units tall and about 78 wide. Olimar is 24 tall, a Pikmin 22 plus its leaf.
`start1` is the point on the ground in front of the ship where the tractor beam picks captains up.

### 3.3 Authored animations found in the demo archives

| File | Frames (30 fps) | Content (from keyframes) |
|---|---|---|
| `s02_ufo.bck` | 199 (6.6 s) | Frames 0-8: pistons pump, body shakes. Frames 8-199: `body_s` rises from Y 61 to 4650 while spinning three turns; radar dish folds. This is the lift-off. |
| `s00_ufo.bck` | 261 (8.7 s) | `ctr` flies in from (-685, 2112, 1411) to the origin by frame 184, body rights itself; cockpit covers swing open at frames 183-249. This is the landing. |
| `s01_ufo.bck` | 450 | Only the research pod hatch (`potjnt`, `pot_top`) opening at frames 320-407. Not used. |
| `s01_alive1.bck` | 450 | Captain's day-end acting; depends on the STB moving the actor. Not used. |
| `s00_alive1.bck` | 261 | Captain climbing out after landing; usable for the optional arrival intro. |

The anim header of `s02_ufo.bck` says 26 joints and the model has 25; see risk R4.

### 3.4 Particle IDs decoded from the demo STB files

Each STB particle object carries a draw group, an emitter id and a joint name.

| Archive | Emitter id (group) | STB name | Attach |
|---|---|---|---|
| `s02_dayend_result/efx.jpc` | 1 (6), 2 (7), 3 (8), 4 (7) | `ufojet_a…d` | ship joint `jet_eff` |
| | 5 (6) | `ufosmoke_in` | `in1` |
| | 6 (6) | `ufosmake_out` | `out` |
| | 7 (7) | `ufo_leaf` (leaves blown away) | `body_s` |
| | 0 (8), 8 (7) | onion sparkle, onion leaves | onion `flow` |
| `s01_dayend/efx.jpc` | 0 (7), 1 (7) | `ufo_suck_a/b` (tractor beam) | `start1` |
| | 2 (7) | `ufo_ptcl` | `pmotion3` |
| | 3 (7), 4 (7) | `ufo_gepu_a/b` (burp after boarding) | `pmotion3` |
| `s00_coursein/efx.jpc` | 13 (5) | `sakura@forest` (petals) | world |
| | 14 (5) | `snow@tutorial` | world |
| | 15 (5) | `momiji@last` (autumn leaves) | world |
| | 1 (4) | foot dust | ground under joint |
| | 4 (3), 5 (4), 6 (4) | ground light, ground smoke a/b | ship |
| | 7 (7), 8 (5), 9 (7), 10 (7) | landing jets `ufo_a…d` | `jet_eff` |
| | 11 (5), 12 (5) | `ufosmoke_a/b` | `in1`, `out` |

Groups 3-5 are drawn before and 6-8 after the main particle group in gameplay
(`baseGameSection.cpp:1439-1450`); the scene draws groups in the same order.

Sound effects the same STB files trigger: `PSSE_EV_ROCKET_FLY_AWAY` (0x300E, at `jet_eff`, frame 0 of
the lift-off), `PSSE_PL_S02_SCATTER_LEAF` (0x86F, frame 41), `PSSE_EV_S01_VACUUM_PLAYER` (0x381F,
tractor beam), `PSSE_EV_ROCKET_JET_LANDING` (0x3817), `PSSE_EV_ROCKET_LAND` (0x3818).

### 3.5 Landing sites (from `disc:user/Abe/map/<area>/defaultgen.txt`)

| Area | Folder | Ship position | Ship yaw (deg) | Season effect |
|---|---|---|---|---|
| Valley of Repose | `tutorial` | (-137.643, 0, 3033.014) | -121.22 | snow (id 14) |
| Awakening Wood | `forest` | (638.355, -70, 2639.886) | -91.22 | petals (id 13) |
| Perplexing Pool | `yakushima` | (-331.455, 80, 1228.324) | -171.525 | none on disc |
| Wistful Wild | `last` | (-3130.88, 0, -1043.1) | -41.22 | autumn leaves (id 15) |

The landing sites are flat: the onions next to the ship sit at the same Y (within 0.88). The scene
uses the ship's Y as ground height for everything it places, so no collision data is loaded.

Red, yellow and blue Pikmin models and all clips are in `pikis.szs` unconditionally. Nothing here
depends on story progress or a save file.

---

## 4. Integration

### 4.1 Hooks in game code (all `#if TARGET_PC`)

| # | File:line | Change |
|---|---|---|
| H1 | `src/sysGCU/gameflow.cpp:147-150` (top of `GameFlow::createSection`) | `if (p2_launcher_scene_pending()) { mActiveSectionFlag = SN_MainTitle; return p2_launcher_scene_create(heap); }` |
| H2 | `src/plugProjectHikinoU/PSGame.cpp:1001` (before `default:` in `initMainBgm`) | `case SceneInfo::TITLE_18:` → `bgm = p2_launcher_scene_bgm(this, soundInfo, wScene);` (implemented in `port/game/scene/SceneAudio.cpp`, section 9). `newGameScene` already falls back to `Scene_NoObjects` for this type. |
| H3 | `src/sysGCU/bootSection.cpp:1009-1090, 910-925` (`updateNintendoLogo`, Dolby state) | When `p2_launcher_scene_pending()`: skip the 1.5 s logo holds and go to `SID_EndState` as soon as `waitLoadResource()` is false. The opaque launcher backdrop covers the screen during boot, so the logos would not be seen anyway. See question Q3. |

`include/` is not touched. Declarations go in `port/include/p2_host.h`.

### 4.2 C bridge (`port/include/p2_host.h`)

```c
// Host -> game. Implemented in port/launcher/scene_bridge.cpp.
int  p2_launcher_scene_pending(void);      // 1 until the scene section has been created (one-shot, re-armable)
typedef struct P2SceneConfig {
	int area;        // 0 tutorial, 1 forest, 2 yakushima, 3 last
	int timeOfDay;   // 0 morning, 1 day, 2 evening, 3 night
	int music;       // 0 off, 1 Piklopedia, 2 world map, 3 challenge menu, 4 high scores
	int takeoff;     // 0 off (fade only), 2 full (1, the old short, also plays full)
	int reduceMotion;
	unsigned fadeColor; // 0xRRGGBB of the end fade: 0xFFF6E0 (DESIGN.md launch flash), 0x050B1A for Off / reduce motion
	int live;        // 1 = the scene runs; 0 = parked: not drawn, no music (Live Start Menu off)
} P2SceneConfig;
void p2_launcher_scene_config(P2SceneConfig* out);   // the handed config (see below)
unsigned p2_launcher_scene_config_gen(void);         // changes whenever the handed config changes
enum { P2_SCENE_CMD_NONE, P2_SCENE_CMD_PLAY, P2_SCENE_CMD_SKIP };
int  p2_launcher_scene_command(void);      // polled once per frame; reading clears it
void p2_launcher_scene_pointer(float* x, float* y, int* active); // cursor in 0..1 of the window, for glances
int  p2_launcher_scene_idle_seconds(void); // time since the last input the UI saw

// Game -> host.
enum { P2_SCENE_LOADING, P2_SCENE_READY, P2_SCENE_TAKEOFF, P2_SCENE_FADING, P2_SCENE_DONE, P2_SCENE_FAILED,
       P2_SCENE_PARKED };
void p2_launcher_scene_state(int state);
// doExit of a section that ends only to be created again (area change): re-arms pending, reports Loading.
void p2_launcher_scene_reloading(void);
// Take-off cues for the UI choreography (DESIGN.md 7.1). Each is sent once, at the times in section 11.
// DESIGN's `handoff` cue is the state P2_SCENE_DONE.
enum { P2_SCENE_CUE_LAUNCH_BEGIN, P2_SCENE_CUE_HOP, P2_SCENE_CUE_BOARD, P2_SCENE_CUE_IGNITE,
       P2_SCENE_CUE_LIFTOFF, P2_SCENE_CUE_EXIT_FRAME };
void p2_launcher_scene_cue(int cue);
// Every frame from IGNITE on: joint body_s projected to 0..1 of the content rectangle (y down).
void p2_launcher_scene_ship_screen(float x, float y, int onScreen);

// Game side, implemented in port/game/scene/SceneSection.cpp.
struct ISection* p2_launcher_scene_create(struct JKRHeap* heap);
```

**Handed config (live settings).** The start-menu settings apply while the menu is open. The host
builds the wanted config every launcher frame and hands it through a stored copy; every change bumps
the generation (`mppm: scene: config gen N ...`). Time of day, music, take-off, reduce motion and the
fade colour are handed at once. `area` and `live` are structural: while the scene is on screen
(state Ready under an open menu) the host first asks the menu for its backdrop
(`scene::cover_wanted()`), hands the change 300 ms later (the backdrop fades in over 0.25 s), and
keeps the cover until the scene reports a new state or reloads (1 s at most). Off screen (loading,
parked, failed) the change is handed at once; during the take-off never. `scene::busy()` (cover or
loading) disables Play ("Landing...").

A game booted behind the start menu always runs the scene section (`arm()` no longer looks at Live
Start Menu); with Live off it is parked.

### 4.3 Launcher side (`port/launcher/`)

- `wants_prelaunch()` (`launcher.cpp:89-94`) already decides whether the start menu is shown.
  `p2_launcher_scene_pending()` returns that value until `p2_launcher_scene_create` runs.
  A disc given on the command line or `MPPM_DISC` skips the scene, so headless tests are unchanged.
- `show_prelaunch()` is still called before `p2_game_main()` (`port/main.cpp:178-180`). The start
  menu keeps its opaque backdrop until `P2_SCENE_READY`, then fades it out over 1 s (Dusklight does
  the same with `prelaunch-background`). If the state becomes `P2_SCENE_FAILED` the backdrop stays,
  and the section keeps running empty (it draws only the clear colour) until `P2_SCENE_CMD_PLAY`,
  then ends at once. It must not end earlier: the title section would start, with its music, under
  the backdrop.
- Play no longer closes the menu directly. It sets `P2_SCENE_CMD_PLAY` at once and runs the UI
  timeline of DESIGN.md 7.2, driven by the cues above, and hides the document after `P2_SCENE_DONE`. Any confirm input during the
  take-off sets `P2_SCENE_CMD_SKIP`.
- While the menu or a settings window is open, pad input to the game is already blocked by the UI
  layer. The scene section does not read a `Controller` at all.
- The planned title-screen gate on `mGoToDemoTimer` (`titleSection.cpp:351`) is no longer needed for
  the start menu, since the menu is gone before the title section starts.

### 4.4 Room for later features

- `p2_launcher_scene_pending()` can be re-armed, so "Return to launcher" from the title or pause
  menu can bring the scene back (set the flag, end the current section with
  `GameFlow::mActiveSectionFlag = SN_MainTitle`).
- The director takes its cast from a table (section 7.1). A mod or an online lobby can add actors
  (a second captain per connected player, more Pikmin colours) without touching the renderer.
- Stage data (area, positions, camera) is one struct per area in `SceneStage.cpp`.

---

## 5. Files and classes (`port/game/scene/`)

| File | Contents |
|---|---|
| `SceneSection.h/.cpp` | `LauncherScene::Section : Game::BaseHIOSection`; section virtuals; top-level state machine; `p2_launcher_scene_create`. |
| `SceneAssets.cpp` | Archive mounts and model/animation loading (section 6.2). |
| `SceneActor.h/.cpp` | `Actor` (captain or Pikmin), `ShipActor`, `MapActor`; per-frame calc/entry; neck callback; leaf drawing. |
| `SceneDirector.h/.cpp` | Behaviour scheduler: clip table, per-actor state machines, interaction events, take-off script. |
| `SceneCamera.cpp` | `SceneCamera : LookAtCamera`; idle drift, pointer parallax, take-off tracking. |
| `SceneLight.cpp` | `SceneLightMgr : LightMgr` (main/sub/spec + `FogMgr`), `light.ini` reader, sky clear, shadow pass. |
| `SceneEffects.cpp` | Particle manager setup, demo JPC registration, emitters that follow joints. |
| `SceneAudio.cpp` | Sound scene creation, `p2_launcher_scene_bgm`, sound effects. |
| `SceneStage.cpp` | `StageInfo` table for the four areas (section 8). |
| `SceneDebug.cpp` | Clip viewer and free camera behind `MPPM_SCENE_TUNE=1`. |

Host side: `port/launcher/scene_bridge.cpp` (the C bridge, settings to `P2SceneConfig`).

```cpp
namespace LauncherScene {
struct Section : public Game::BaseHIOSection {
	enum State { ST_Load, ST_Idle, ST_Takeoff, ST_Fade, ST_Done };
	Section(JKRHeap*);
	virtual ~Section();
	virtual void init();
	virtual void doLoadingStart();
	virtual bool doLoading();
	virtual bool doUpdate();
	virtual void doDraw(Graphics&);
	virtual void doExit();
	virtual bool forceReset() { return false; }
	void loadResource();          // DVD thread

	int mState;
	DvdThreadCommand mThreadCommand;
	P2SceneConfig mConfig;
	const StageInfo* mStage;
	J3DDrawBuffer* mOpaBuffer; J3DDrawBuffer* mXluBuffer;
	Viewport mViewport; SceneCamera mCamera;
	SceneLightMgr* mLightMgr;
	Game::ShadowCylinder2* mShadow; Game::ShadowParms mShadowParms; Color4 mShadowColor;
	Assets* mAssets; MapActor* mMap; ShipActor* mShip;
	Actor* mActors[4];            // 0 Olimar, 1 red, 2 yellow, 3 blue
	Director* mDirector; Effects* mEffects;
};
}
```

---

## 6. Section lifecycle

### 6.1 `init()` (main thread)

Follow `Demo::Section::init` (`demoSection.cpp:65-92`):

1. `initHIO(new HIORootNode(this))`.
2. `setDisplay(JFWDisplay::createManager(nullptr, mDisplayHeap, JUTXfb::DoubleBuffer, false), 1)`.
3. `sys->setFrameRate(1)` (60 fps, as the title does at `titleSection.cpp:173`).
4. `PSM::ObjMgr::newInstance()` (`titleSection.cpp:209`).
5. `mTimeStep = 0.0f` so `fadeIn`/`fadeOut` in `Section::run` take no time; the scene does its own
   fades. `JUTProcBar` visibility off as in the title.
6. Draw buffers, camera and viewport exactly as `TTitleMgr::init` (`ebiP2Title.cpp:93-128`):
   two `J3DDrawBuffer(0x400)`, `setZSort()` on the translucent one, `sys->mGfx->allocateViewports(1)`,
   viewport rect = `fbWidth x efbHeight`, `addViewport`.
7. `p2_launcher_scene_config(&mConfig)`, pick `mStage`, `p2_launcher_scene_state(P2_SCENE_LOADING)`.

### 6.2 `loadResource()` (DVD thread, started from `doLoadingStart`)

Order matters only where noted.

1. **Sound scene** (as `titleSection.cpp:700-710`): `PSGame::SceneInfo info; info.mSceneType =
   TITLE_18; info.mCameras = 0;` → `newAndSetCurrentScene(info)` → `scene1stLoadSync()`.
   With `mConfig.music == 0` the scene is still created (sound effects need it) and the sequence is
   muted; see section 9.
2. **Character archive**: `JKRMountArchive("/user/Kando/piki/pikis.szs", EMM_Mem, sys->mSysHeap,
   EMD_Head)`, the same call `NaviMgr::load` makes (`naviMgr.cpp:197`). The title's later
   `loadResident` finds it already mounted. `System::SF_LoadResident` is not touched.
   `texts.szs` is mounted on the current heap with `EMD_Tail` and unmounted after the AnimMgr is built.
3. **Olimar**: load and prepare `orima1.bmd` as in 2.2. `mAnimMgr = SysShape::AnimMgr::load(texts,
   "animMgr.txt", olimarData, arc, "motion")`, owned by the scene.
4. **Pikmin**: three model datas as `PikiMgr::loadBmd`; one `SysShape::Model(data, 0x20000, 1)` each,
   then `calcMaterial(); makeDL(); lock();` (`pikiMgr.cpp:252-257`).
   Happa models with the flags at `pikiMgr.cpp:196-203` and the shared-DL setup at `:211-223`.
5. **Ship**: mount `user/Kando/ufo/arc.szs` on the current heap; load `ufo.bmd` as in 2.2;
   `SysShape::Model::enableMaterialAnim(data, 0)`; attach `ufo.brk`/`ufo_2.brk` to two
   `Sys::MatTevRegAnimation`; `mShipAnimMgr = AnimMgr::load(ufoTexts, "animmgr.txt", data, ufoArc,
   nullptr)`. Use `arc->getResource(name)` everywhere. Do not use `JKRFileLoader::getGlbResource`
   with a null loader: `model.bmd`, `efx.jpc` and `animmgr.txt` exist in several mounted archives.
6. **Extra clips**: mount the three demo archives on the current heap. Build `AnimInfo`s with
   `attach(shipData, s02Arc->getResource("s02_ufo.bck"))`. Optional: `aogu.bca` for the Pikmin.
7. **Map**: mount `<stage folder>/arc.szs`, load `model.bmd` and the `.btk` files as
   `ShapeMapMgr::load` does (`mapMgr.cpp:300-360`), without `Farm::farmMgr`, sea, route or collision.
8. **Lights**: read `<abe folder>/light.ini` with `JKRDvdRipper::loadToMainRAM` into a
   `Game::GameLightMgrSetting` (`RamStream`, text mode).
9. **Particles**: `ParticleMgr::globalInstance(); particleMgr->createHeap(0x100000);
   particleMgr->createMgr("user/Ebisawa/effect/game.jpc", 500, 100, 0x80);
   particleMgr->Instance_TPkEffectMgr();` Then three `JPAResourceManager`s from the demo `efx.jpc`
   files. Only one can sit in slot 7 at a time: register `s00` (ambient) during idle and swap to
   `s01` then `s02` during the take-off, or patch in more slots (see R6).
10. **Shadows**: `mShadow = new Game::ShadowCylinder2(&mShadowParms, &mShadowColor)`.

All of this lives in the section's child heap and is released when `MenuSection::runChildSection`
destroys it (`menuSection.cpp:54`). Mounted archives are `JKRDisposer`s and unmount with their heap.
Expected use is under 12 MB; the arena is 256 MB (`port/main.cpp:132`).

### 6.3 `doLoading()`

Returns true until `sys->dvdLoadSyncNoBlock(&mThreadCommand)` succeeds. Then, on the main thread:
create `J3DModel`-dependent GX state that must not be made on the DVD thread if any shows up in
testing (R2), build actors, `PSMGetSceneMgrCheck()->doStartMainSeq()`, state `ST_Idle`,
`p2_launcher_scene_state(P2_SCENE_READY)`.

With `live = 0` the DVD thread only creates the sound scene, and `doLoading` ends in `ST_Parked`
(report PARKED): the area's assets load when the scene is first unparked, through a reload.

### 6.4 `doUpdate()` per frame

**Live settings.** In `ST_Idle`, `ST_Parked` and `ST_Failed`, `applyConfig()` runs first whenever
`p2_launcher_scene_config_gen()` changed:

1. Live, and the area differs from the loaded one (or nothing is loaded and the section has not
   failed): `mReload = true`, `ST_Done`; the section ends and `doExit` calls
   `p2_launcher_scene_reloading()` instead of reporting DONE. `GameFlow::createSection` (H1) builds
   the next scene section while pending is set; no flow code is involved. An area change while
   parked is only recorded.
2. `live` flipped: `park()` (music fade 0.25 s, `ST_Parked`, draw buffers cleared, report PARKED;
   assets stay loaded) or `unpark()` (`ST_Idle`; READY and the music follow the first drawn frame).
3. Time of day: `SceneLightMgr::retarget(..., 0.6 s)` (10.1).
4. Music: a swap (9).
5. Take-off, reduce motion and fade colour are copied.

A part that cannot apply during a music swap (structural, or another track) waits for it to end.
`ST_Parked` treats Play or Skip like `ST_Failed`: the section ends and the title follows. The free
run is requested in `ST_Idle`, `ST_Takeoff` and `ST_Parked`.

```
dt = sys->mDeltaTime
cmd = p2_launcher_scene_command()
mDirector->update(dt, cmd)            // behaviour, movement, take-off script
mCamera.update(dt)                    // sets position/look-at, Camera::update()
j3dSys.setViewMtx(mCamera view)
mLightMgr->update()
for each actor: animate, set joint calcs, build TR matrix, mPosMtx, calc(), entry()
map: texture anims, entry();  ship: pmotions, entry()
mEffects->update(dt); particleMgr->update()
BaseHIOSection::doUpdate()
return mIsMainActive
```

`mIsMainActive` becomes false when the scene's own fade has finished (`ST_Done`). `Section::main`
then calls `doStopMainSeqCheck` (`section.cpp:207`).

### 6.5 `doDraw(Graphics&)`

Following `TTitleMgr::draw` (`ebiP2Title.cpp:896-935`) and `newdraw_drawAll`
(`baseGameSectionDraw.cpp:68-134`):

1. Sky: fill the screen through `gfx.mPerspGraph` with a vertical gradient (`J2DGrafContext::fillBox`
   with four corner colours: zenith colour on top, fog colour at the bottom). Optional sky dome later.
2. `vp->setViewport(); vp->setProjection(); mCamera.setProjection();`
   `gfx.mCurrentViewport = vp;` (needed by `FogMgr::set`).
3. `SysShape::Model::setViewCalcModeImm();` then `setCurrentViewNo(0)` and `viewCalc()` on each model.
4. `mLightMgr->set(gfx)` (lights, ambient, fog).
5. `j3dSys.drawInit(); j3dSys.mDrawMode = SYSDRAW_Unk3; mOpaBuffer->draw();`
6. Leaves: the loop from `PikiMgr::doSimpleDraw` (`pikiMgr.cpp:319-363`) over the three Pikmin.
7. `j3dSys.mDrawMode = SYSDRAW_Unk4; mXluBuffer->draw();`
8. Shadow pass (section 10.3), then `vp->setViewport(); vp->setProjection();`.
9. Particles: fog off, groups 0, 3, 4, 5, 1, 6, 7, 8, 2 in that order; fog back on.
10. `j3dSys.reinitGX(); mOpaBuffer->frameInit(); mXluBuffer->frameInit();`
11. Scene fade: a full-screen `fillBox` with the fade colour and current alpha.

### 6.6 `doExit()`

`PSMGetSceneMgrCheck()->deleteCurrentScene()` (as `demoSection.cpp:138`), `particleMgr->reset()`
equivalent teardown: kill all emitters, `clearDemoResourceManager()`, `particleMgr->deleteInstance_TPkEffectMgr()`,
then `ParticleMgr::deleteInstance()`. Set `j3dSys` draw buffers back to null.
`p2_launcher_scene_state(P2_SCENE_DONE)`, or `p2_launcher_scene_reloading()` when the section
ends for a reload (6.4).

---

## 7. Characters and behaviour

### 7.1 Cast

Positions are in the **stage frame**: origin at the ship's ground position, +Z pointing out of the
ship's front (toward `start1`), +X to the ship's left as seen from the front camera, Y up. The stage
frame is rotated into the world by the stage azimuth (section 8). All values **tune**.

| Actor | Model | Home (x, z) | Faces | Leaf | Shadow (radius/height) |
|---|---|---|---|---|---|
| Olimar | `orima1.bmd` | (9.1, 293.0) | camera, 20° to its left | — | 4 / 10 |
| Red Pikmin | `piki_p2_red.bmd` | (21.5, 290.0) | between Olimar and the camera | leaf | 2 / 10 |
| Yellow Pikmin | `piki_p2_yellow.bmd` | (-3.2, 284.0) | between Olimar and the camera | leaf | 2 / 10 |
| Blue Pikmin | `piki_p2_blue.bmd` | (13.4, 264.3) | between Olimar and the camera | leaf | 2 / 10 |
| Ship | `ufo.bmd` | (0, 0), model yaw = gen yaw | — | — | 30 / 100, lifted 60 (as the pod at `onyonMgr.cpp:377-381`) |

Scale 1.0 for all. Red and Olimar are nearest the camera (about 62-64 units), yellow 72, blue 92
(values as tuned on screen in phase 4; the code in `SceneDirector.cpp` is the reference).

### 7.2 Actor

```cpp
struct Actor {
	SysShape::Model* mModel; SysShape::Animator mAnimator;  // one animator, joint 0; joint 4 calc = nullptr
	Vector3f mPos; f32 mFaceDir, mTargetFaceDir, mScale;
	f32 mNeckTheta, mNeckPhi; Vector3f mLookAt; bool mHasLookAt;   // eased toward target, clamp ±60°/±30°
	int mClip, mNextClip; f32 mClipTime; bool mLoop;
	Vector3f mMoveTarget; f32 mMoveSpeed;                      // 0 = standing
	int mHappa;                                               // 0 leaf, 1 bud, 2 flower
	BehaviourState mState; f32 mStateTimer, mNextFidget;
	SysShape::Joint* mHeadJoint; SysShape::Joint* mLeafJoint;
};
```

- Animation: `mAnimator.mAnimMgr = scene AnimMgr; startAnim(id, &listener)`; each frame
  `animate(30.0f * dt)`; `setModelCalc(model, 0)`; joint 4 `setMtxCalc(nullptr)` (the game does the
  same in its LOD branch, `fakePiki.cpp:834-835`). The three Pikmin have separate model datas and
  Olimar his own, so setting the calc on the model data just before each `calc()` is safe.
- Looping: `IPikiAnims` clips have key events (0 loop start, 1 loop end, 2/3 action marks, 200/201
  footsteps; see `animmgr.txt`). A `SysShape::MotionListener` receives them; one-shot clips end on
  `KEYEVENT_END` (1000) and fall back to `WAIT`.
- Transitions: clips cut. If cuts look harsh, switch to `SysShape::BlendAnimator`
  (`include/SysShape/Animator.h:116-135`) with a 0.15 s linear blend.
- Head: a scene-owned joint callback on `headjnt` (copy of `FakePiki::sNeckCallback` without the
  `OptimiseController` test). The callback reads two statics set just before each actor's `calc()`.
- Movement: straight lines on the flat ground at `mMoveSpeed`; facing eases toward the travel
  direction with the game's formula (`fakePiki.cpp:852-853`). Walking plays `WALK` (30) under 40
  units/s, `RUN2` (29) above. Foot dust `PID_PkS_WalkSmoke` on key events 200/201 while running.
- Transform: `mBaseTrMatrix.makeSRT(scale, (0, faceDir, 0), pos)` → `mJ3dModel->mPosMtx`.

### 7.3 Clip vocabulary

Names come from the decomp's enum comments and are not verified visually. Phase 2 starts with the
clip viewer to confirm each one and fix this table.

| Use | Clip (`IPikiAnims`) | Type |
|---|---|---|
| Stand | `WAIT` 31 | loop, 20 frames |
| Look around | `SAGASU` 53, `SAGASU2` 54 | one-shot |
| Look over shoulder | `FURIMUKU` 11 | one-shot |
| Yawn | `AKUBI` 0 | one-shot |
| Chat | `CHATTING` 3 | one-shot |
| Nod / cheer | `GATTU` 13 | one-shot |
| Notice | `KIZUKU` 32 | one-shot |
| Impatient | `IRAIRA` 21 | one-shot |
| Step in place | `ASIBUMI` 1 | loop |
| Hop | `JUMP` 50 | one-shot |
| Trip | `KOROBU` 26, get up `GETUP` 14 | one-shot pair |
| Sigh | `GAKKARI` 12 | one-shot |
| Sit | `SUWARU` 56 | intro then loop 30-59 |
| Sleep | `NERU` 57 | intro then loop 30-59 |
| Whistle (Olimar) | `FUE` 10 | loop 1-3 |
| Walk / run | `WALK` 30, `RUN2` 29 | loop 10-30 |
| Grow (leaf → bud → flower) | `GROWUP1` 18, `GROWUP2` 19 | one-shot, swap the happa model at key event 2 |
| Look up | `aogu.bca` (not in the table; loaded as an extra clip) | to verify |

**Olimar (FPS/scene wave, reworked in TP).** Olimar shares the Pikmin skeleton, whose shoulder
pivots sit ±1.52 from the spine, inside his wider torso. Gameplay draws him with the same model,
clips and load flags, and a close zoom in the field (Valley of Repose, story save) shows his hanging
arms merge into the torso exactly as authored. So the hanging pose gets no help: the earlier fix
(pivot 1.1 out, full weight on a hanging arm) opened a notch between sleeve and torso in the front
view. `Actor::shoulderCallback` now moves an arm's pivot out along the torso's side axis by at most
0.6 units, weighted by how far the arm points along the torso's forward axis (none below 0.45 of
it, full from 0.85), so only an arm swung well forward or back, where the glove can enter the belly
or the backpack, moves. Fue keeps its authored pose: the hand comes up to the mouth, which is inside
the helmet, and that is how gameplay shows the whistle too. `Actor::wave(seconds, side)` raises one
arm sideways to about 100 degrees from hanging (the glove clear of the helmet's outline) and swings
it ±22 degrees at 2.4 Hz, on top of any clip, rising and lowering over 0.28 s; the take-off's goodbye
uses it. Before/after close-ups of every whitelisted clip, front and side, were checked with the
clip viewer (`MPPM_SCENE_TUNE=1` plus `MPPM_SCENE_CAM`). `Actor::startClip` keeps him on a
whitelist: Wait, Walk, Run2, Asibumi, Fue, Furimuku, Akubi, Sagasu2, Suwaru, Mizunomi, Kizuku, Getup
(Jump only in the beam shot). Sagasu plays Sagasu2; Gattu becomes `nod()` (two 12° head dips);
Gakkari becomes `sigh()`; Neru keeps the Suwaru loop with the head hanging; anything else plays Wait
and logs once. The characters' `.bca` clips round to whole frames (no interpolation), so they show
30 poses a second at any frame rate. `kOlimarScale` stays 1.0 (gameplay draws captains at 1.3).

### 7.4 Director

Three layers, highest priority first:

1. **Script** (take-off, optional arrival). Owns every actor while it runs.
2. **Events**: one interaction at a time involving two or more actors. A new event is rolled every
   18-35 s (uniform) while no event or script runs.
3. **Fidgets**: each actor on its own timer. Olimar every 6-14 s, each Pikmin every 3-9 s.
   An actor inside an event skips its fidget.

Base state for everyone is `WAIT` at home, facing its default direction.

**Fidgets (weights):**

| Olimar | w | Pikmin | w |
|---|---|---|---|
| Look around (`SAGASU`) | 30 | Look around (`SAGASU`/`SAGASU2`) | 28 |
| Look at the camera 2.5 s (neck only), then `GATTU` | 20 | Hop (`JUMP`) | 14 |
| Look over shoulder at the ship (`FURIMUKU`) | 18 | Wander: walk to a random point within 18 units of home, 22 units/s | 22 |
| Look down at the nearest Pikmin 2 s (neck) | 20 | Step in place 1.5 s (`ASIBUMI`) | 10 |
| Yawn (`AKUBI`) | 6 | Look at Olimar 2 s (neck) | 14 |
| Sigh (`GAKKARI`) | 6 | Yawn (`AKUBI`) | 6 |
| | | Impatient (`IRAIRA`) | 6 |

No fidget repeats twice in a row for the same actor.

**Events (weights):**

| Event | w | Steps |
|---|---|---|
| Pester | 25 | A Pikmin runs to 9 units from Olimar, hops twice. Olimar turns to it (0.4 s), looks down, `GATTU`. Pikmin hops, walks home. Olimar turns back. |
| Chat | 20 | Two Pikmin turn to each other and play `CHATTING`. Olimar `FURIMUKU` toward them halfway through. |
| Roll call | 15 | Olimar `FUE` for 0.8 s (whistle particles optional). All Pikmin `KIZUKU`, run to a line 12 units in front of him, stand 2 s looking at him. He nods. They walk home. |
| Trip | 10 | A wandering Pikmin runs and plays `KOROBU`, then `GETUP`. The others `KIZUKU` and look. Olimar looks, `GAKKARI`. |
| Yawn chain | 10 | One Pikmin yawns, the next 0.8 s later, then the third, then Olimar. |
| Grow | once | At 90 s of scene time the red Pikmin plays `GROWUP1` and becomes a bud; the others look and hop. At 240 s it flowers. Yellow and blue follow 30 s later each. |
| Watch the cursor | 10 | Only when the pointer is active and over the right 60% of the window: all four look toward the point on the ground under the cursor for 3 s. |

**Long idle** (`p2_launcher_scene_idle_seconds()`): after 120 s Olimar sits (`SUWARU`); after 180 s
the Pikmin sit one by one and then sleep (`NERU`). Any input wakes them: `KIZUKU`, stand up, a short
look at the camera. Events are not rolled while sitting.

`reduceMotion`: no wandering, no trip, no camera drift, no pointer parallax; fidget intervals doubled.

Randomness uses the game's `randFloat()`; nothing here needs to be deterministic.

**As built (phase 4, `SceneDirector.cpp`).** Each actor runs a queue of steps (clip, loop for a
time, hold, walk, face, look, change happa); events are beats that queue steps and wait for the
cast to finish. Differences from the plan above, all found on screen:

- `JUMP` lifts a Pikmin more than its own height, which leaves the picture for the near ones. A
  "hop" plays `JUMP` only when the top of its arc projects inside the frame, else `KIZUKU` (a small
  startled hop).
- Pikmin fidgets also include sit a while (`SUWARU`, then `GETUP`), doze off (sit, `NERU`, wake with
  `KIZUKU` and a look at the player), a somersault (`ROLLJUMP` 35) and a crouch (`MIZUNOMI` 55).
  When one hops or flips, the free others may glance at it. Wander targets stay in the subject zone.
- Events: pester, chat, roll call (Olimar whistles, they run to his sides, he nods), chase (two
  Pikmin run round the blue one's spot; the chaser trips, `KOROBU` + `GETUP`), yawn chain, hop wave,
  visit (Olimar walks to a Pikmin beside or in front of him and kneels, `MIZUNOMI`), watch the cursor,
  and bloom (from 80 s, `GROWUP1` with the leaf becoming a bud, later a flower). An event is rolled
  every 6-13 s after the last; while one is due nobody starts a new fidget.
- Long idle: Olimar sits at 120 s, the Pikmin sit at 150/165/180 s and doze at 200/214/228 s,
  Olimar dozes at 245 s (his `NERU` starts at its loop: the lead-in has him flop backwards). Until the
  host has seen any input it reports 0 idle seconds, so the scene clock stands in.
- Test switches: `MPPM_SCENE_IDLE_SCALE=<k>` brings long idle and blooming k times sooner;
  `MPPM_SCENE_EVENTS=bloom,chase,...` plays those events first, in order. Events are logged as
  `scene: event <name> at <t> s`.

### 7.5 Ship during idle

`pmotion1` (id 1) at 30 frames/s and `pmotion3` (id 3) at 30, `pmotion2` (id 2) stopped, as
`Onyon::init_pmotions` sets them (`onyonMgr.cpp:1818-1848`). Each has its own `Animator` bound to its
joint with `setModelCalc`. `ufo.brk` runs on the material animation. No particles while idle.

---

## 8. Stage and camera

### 8.1 `StageInfo`

```cpp
struct StageInfo {
	const char* mapFolder;  const char* abeFolder;
	Vector3f shipPos; f32 shipYawDeg;      // from defaultgen.txt (section 3.5)
	f32 azimuthDeg;                         // stage frame rotation; default = shipYawDeg
	u16 ambientEfx;                         // s00 efx id, 0xFFFF = none
	Vector3f ambientEfxOffset;              // stage frame; default (0, 90, 200)
	u8 wScene; const char* envNote;
};
```

Default stage is `forest`. `azimuthDeg` exists because what stands behind the ship differs per area;
if the view past the ship is an ugly map edge, rotate the whole staging around the ship (the ship
model keeps its own yaw).

### 8.2 Idle camera

All in the stage frame, **tune**.

| Parameter | Value |
|---|---|
| Position | (20, 11, 354): 11 units above ground, about Olimar's chest height |
| Aim | yaw 12° to the left of the ship at 19:14, plus 8° per unit of extra width (Hor+), at most 20°; pitch 3° up; 200 units along that ray |
| Vertical FOV (`mViewAngle`) | 36° |
| Near / far (`setFixNearFar(true, …)`) | 5 / 12000 |
| Up vector (`mLookAtRotation`) | (0, 1, 0) |

Resulting picture at the logical 608:448 aspect: ship in the upper right-centre (centre at 65% of
the width, top near 3% of the height, base at 69%), Olimar in the foreground at 61% of the width with
his head at 35% and feet at 87% of the height, red Pikmin close on the right (81%), yellow near the
centre (48%), blue behind Olimar. The left 45% stays calm for the menu column and its dark gradient.

Aspect ratio comes from `Viewport::updateCameraAspect`, which the aspect work multiplies by
`p2_wide_k()` (`docs/launcher/ASPECT.md` section 6.1). The scene is Hor+ with no extra code. To keep
the group from drifting toward the centre on wide windows, the aim yaw is
`8° + 4° * (k - 1)`, clamped to 14°. At 16:9 the ship centre lands at about 61% of the width.

The composition target is DESIGN.md 6.2, stated for 16:9: nothing but ground, sky and foliage left
of x = 0.30; all four characters inside x 0.40-0.94; ship centre between x 0.60 and 0.74; horizon
between y 0.55 and 0.65 (this camera gives 0.63). Tune at 16:9 first, then check 19:14, 16:10 and
21:9. While the start menu is open the frame always has the window's shape (ASPECT.md 5).

**Life**:
- Drift: position offset `(1.2 sin(2πt/17), 0.5 sin(2πt/23), 0)`.
- Pointer parallax: yaw ±1.5°, pitch ±0.8° from the cursor position, low-passed with a 0.35 s time
  constant. Off when the pointer is inactive (pad or keyboard navigation).
- Depth: GX has no depth of field. Depth is suggested by fog (section 10.2) and by the menu's own
  backdrop blur on the left. A real blur is a later Aurora post-process (Q6).

### 8.3 Debug

`MPPM_SCENE_TUNE=1`: free camera on the right stick/IJKL, clip viewer on the selected actor
(next/previous clip, pause, frame step), on-screen print of camera position, look-at, FOV and
actor positions in the stage frame. `MPPM_SCENE_AREA`, `MPPM_SCENE_TIME` override the config.
`MPPM_SCENE_AUTOPLAY=<seconds>` issues `P2_SCENE_CMD_PLAY` after that time, for headless runs
(`MPPM_FORCE_LAUNCHER=1` already forces the start menu with a command-line disc).
Clip viewer: `]`/`[` clip, `P` pause, `.`/`,` frame step, `Tab` actor, `B` toggles Olimar's
shoulder offset; `T`/`G` pitch the free camera. `MPPM_SCENE_HOLD=<s>`, `MPPM_SCENE_SPEED=<k>`,
`MPPM_SCENE_CAM="ex,ey,ez,tx,ty,tz,fov[,ux,uy,uz]"` and `MPPM_SCENE_CLIPLOG=1` are the take-off
tuning switches (11.4).

**Time base.** Each frame's `dt` is the host's tick length while the host runs the scene free
(Frame Rate above Original: `p2_host_tick_dt() > 0`), otherwise the retrace count since the last
frame times 1001/60000 (capped at 4 retraces). The scene renews `p2_host_free_run(1)` every frame
while idle or taking off, so no later section inherits FreeRun. Everything (actors, camera, ship,
Onions, particles through a 60/30 Hz step accumulator, fades) moves by `dt`, so the take-off has
the same timing and look at any frame rate. `sys->mDeltaTime` follows the host's tick length for
the material animators and goes back to 1/60 when the scene ends.

### 8.4 Locations offered (Q3)

Each area was checked with a full take-off sheet at 16:9 (Awakening Wood also at 21:9 and 4:3).

| Area | Offered | Per-area settings (`StageInfo`, `SceneStage.cpp`) |
|---|---|---|
| Awakening Wood | yes (default) | defaults |
| Valley of Repose | yes | `kValleyShots`: S2/S2b top edge 14 deg (the wall facing away from the ship is bare white snow under a blue sky); ground grain 0.7 |
| Wistful Wild | yes | `kWildShots`: S3/S5 top edge 19 deg, S2/S2b 15 deg (backdrop corner and sky); maple leaves at 0.45 size, near cull 150 |
| Perplexing Pool | **hidden** (`mHidden`) | the ship stands in a walled concrete pit: from 11 units up every heading (azimuth sweep 0-315) shows concrete, the pit wall and the pale backdrop |

The Location list drops Perplexing Pool (not greyed); Follow the Season resolves June-August to
Awakening Wood (`resolve_area`), and `getStage` still falls back to Awakening Wood for a hidden area.
`MPPM_SCENE_ALL_AREAS=1` shows it anyway; `MPPM_SCENE_AZIMUTH=<deg>` turns the staging round the ship
for such sweeps.

---

## 9. Audio

- **Scene**: `SceneInfo::TITLE_18` → `Scene_NoObjects`. Hook H2 supplies the main sequence:

| `music` | Sequence | Wave scene |
|---|---|---|
| 1 Piklopedia (default) | `newBgmSeq("book.bms", info)` | `WSCENE36_Piklopedia` |
| 2 World map | `newBgmSeq("worldmap.bms", info)` | `WSCENE16_WorldMap` |
| 3 Challenge menu | `newBgmSeq("c_menu.bms", info)` | `WSCENE20_ChallengeSelect` |
| 4 High scores | `newBgmSeq("hiscore.bms", info)` | `WSCENE21_HighScores` |
| 0 Off | same as 1, with the sequence volume set to 0 after `doStartMainSeq` | |

  These are the same calls the game makes for those screens (`PSGame.cpp:966-988, 781`). The title
  stream and the file-select stream are left out because the player hears them right afterwards.
- **Silent-music bug (fixed in W9c):** `TSinCosTable::sinRadian` masked its index with `(1 << 2048)
  - 1`, undefined in C++; clang's result made every lookup 0 or NaN, so `JASChannel::updateMixer`
  gave every voice on the manual bus mixer a volume of 0. The menu sequences use that mixer (the
  title's streams and the high-score voices use the automatic mixer, which does not call it).
  `include/JSystem/JMath.h` now masks with `LENGTH - 1` on PC.
- **Fade**: on Play the music fades over 1.5 s (`seq->stopSeq(frames)`, as `titleSection.cpp:371-380`).
- **Effects**: played with `PSSystem::spSysIF->playSystemSe(id, 0)` first (no position). Candidates:
  whistle and Pikmin voices (`PSSE_PK_VC_SAY_PIKMIN` 0x1823, `PSSE_PK_VC_AKUBI` 0x2834,
  `PSSE_PK_VC_SLEEP` 0x283E), tractor beam 0x381F, lift-off 0x300E, leaves 0x86F. Whether non-system
  ids play through `playSystemSe` without their creature sound objects, and whether their waves are
  in the loaded wave scenes, has to be tested (R7). If not, the scene stays music-only in phase 3 and
  effects get proper `PSM` objects later.
- UI sounds stay with the launcher's own `ui::play_sound` hook.
- **Live swap** (`Section::stepMusicSwap`): a new `music` on the running scene fades the track out
  over 0.25 s, `SceneAudio::destroy()` on the main thread, `createScene(next)` on the DVD thread
  (`mSwapCommand`), then `startMusic()` if the scene is idle and READY was sent. One swap at a time;
  the latest request is picked up when it ends. A Play or Skip that arrives meanwhile is held until
  the swap ends, so the section never ends while the DVD thread builds the sound scene. Parked, the
  scene keeps its sound scene (the menu's sound effects use it) with the main sequence stopped;
  unparking restarts it.

---

## 10. Light, fog, sky, shadows

### 10.1 `SceneLightMgr : LightMgr`

Constructed like `GameLightMgr` (`gameLightMgr.cpp:386-415`): main `LightObj("main", GX_LIGHT0,
TYPE_Spot, white)` with `mSpotFn = GX_SP_OFF`, sub `GX_LIGHT1`, specular `GX_LIGHT7` with
`mKScale = 40`, plus a `FogMgr` (`GX_FOG_LINEAR`).

Colours come from the area's `GameLightMgrSetting::mSunLight` time blocks. The scene picks one block
by `timeOfDay` (morning default) and copies main, sub, specular, ambient, fog colour and shadow
colour with the `getColor` accessors the game uses (`gameLightMgr.cpp:495-530`). A time of day
changed on the start menu blends over 0.6 s (`retarget` / `step`, smoothstep): the light, ambient,
fog and shadow colours, the fog range, the three sky colours and the sun direction are lerped from
where the scene was; the light positions follow the lerped sun. While nothing is drawn it snaps.

Sun direction: the game's formula (`gameLightMgr.cpp:806-812`) with a fixed sun ratio per time of
day (morning 0.2, day 0.5, evening 0.85, night 0.5), then rotated about Y so that in the stage frame
the sun sits behind the camera's left shoulder: key light on the faces, shadows falling to the right
and away. Main and specular at `lightPos`, sub at `-lightPos`.

`SceneLightMgr::set(gfx)` = `LightMgr::set(gfx)` + `mFogMgr->set(gfx)`.

### 10.2 Fog and sky

- Fog: colour from `light.ini`; near 180, far 2600 (**tune**; the game uses 0-4500 for a camera
  that sits 400+ units away). Olimar's model is loaded with fog off, like in the game.
- Sky: overworld maps have no sky model (the `vrbox` archives are cave backgrounds,
  `gameMapParts.cpp:864-892`). The game shows fog colour above the horizon. The scene draws a
  gradient: top = fog colour scaled toward a blue zenith `(110, 160, 235)` at morning/day, toward
  `(40, 30, 70)` at evening, `(8, 10, 28)` at night; bottom = fog colour. Phase 5 can try
  `s02_bg.bmd` as a dome. Since the take-off's sky shots (FPS/scene wave) the gradient is an 8 by 12
  grid coloured by each corner's view-ray elevation: fog colour at -1.5° and below, `mSkyTop` at
  21°, `mSkyZenith` straight up. In the idle pose this is the old two-band picture. The grid sets
  the whole frame's viewport itself, since only a single full-width box is widened back on a wide
  picture.
- Ambient particles: the area's `s00` emitter (petals, snow or leaves) created once with
  `createDemo(id, pos, 5, 7)` at `ambientEfxOffset`.

### 10.3 Shadows

Per frame, in update:

```
mShadow->setCameraParms(&mCamera, 0);
for each caster: fill ShadowParam (position + 0.5 up, radius, height,
                 mBoundingSphere.mPosition = light direction with y normalised to 1 → sheared shadow)
                 mShadow->makeSRT(world, param); PSMTXConcat(view, world, caster.shadowMtx);
                 caster.type = mShadow->getCylinderType(param, 0);
```

In draw (after the translucent buffer), the body of `ShadowMgr::draw` (`shadowMgr.cpp:275-310`):
`setShadowRect(vp->mBounds2); setFilterTextureID(0); drawInit(); vp->setProjection();
vp->setViewport(); drawCylinder(mtx, type) per caster; drawFinish();`. `mShadowColor` from
`light.ini` (morning: 0, 0, 50, alpha 65).

With the light direction sheared the cylinder gives each character a slanted soft blob, the same
look as gameplay. `makeSRT` only shears when the direction's Y is strictly between 0 and 1
(`ShadowCylinder.cpp:194`), so pass the direction scaled to Y = 0.99.

### 10.4 Ground grain (Q3, `SceneGround.{h,cpp}`)

The maps' ground textures (256-512 texels, no mips) span tens of units; seen from the idle lens 11
units up each texel covers dozens of pixels and the ground in front of the cast smeared into soft
horizontal streaks. `GroundDetail::draw` lays a procedural I8 grain (128 px, five box-filtered mips,
tiling every 14 units, mean mid grey, one standard deviation 10% of full scale: 13% buried the contact shadows) over the ground around the eye: a 40 x 40 grid at the landing
site's height + 0.12, depth-tested against the opaque pass, blended as 2 x src x dst, strength 1 up
to 100 units from the eye falling to 0 at 360 (vertex alpha lerps the grain toward grey). It runs
after the opaque buffer, before the leaves and the translucent buffer. Sampling: anisotropic when
Texture Filtering allows, LOD bias -1 otherwise. Strength per area (`mGroundDetail`),
`MPPM_SCENE_DETAIL` overrides (0 = off). Mipmapped map textures (none in Awakening Wood but the light
shafts, which are skipped) also get anisotropic trilinear sampling and a -0.6 bias.

### 10.5 Water (Q3, `SceneWater.{h,cpp}`)

`<map>/texts.szs waterbox.txt` boxes, drawn with the two-player piece (`waterbox2p.szs 2p.bmd` +
`2p.btk`) scaled per box as `AABBWaterBox::calcMatrix`; the single-player piece needs gameplay's
frame-buffer copy. Left out in the sky-only shot. `texts.szs` is unmounted once the boxes are read.

### 10.6 Area status after the Q3 check (2026-11-22)

Only Awakening Wood is offered. Valley of Repose, Wistful Wild and Follow the Season are greyed in
the Location list again (`kAllSceneAreas` false); a stored value still loads the area, and the
staging, shots and fixes above stay in place for when the open defects are solved:

- **Wistful Wild:** the backdrop's hard vertical edge and flat sky at the top right of the beam and
  ignition shots (`kWildShots`); Olimar and the Pikmin sunk to the ankles in a ground-coloured layer
  at idle; several large leaves still over the logo and the ship.
- **Valley of Repose:** on about half of launches the rock ridge behind the cast renders as vertical
  streaks (as if its texture were clamped in V). The same build alternates between clean and streaky
  runs; it is constant within a run and also happens on `70b7111e6`. The map's texture matrices are
  byte-identical in clean and streaky runs (checked by logging every material's texture matrix), so
  the cause is below J3D (Aurora's texture or sampler state) and is not yet found.
- Follow the Season resolves Dec-Feb to Valley, Mar-Aug to Wood and Sep-Nov to Wild.

---

## 11. Take-off

Starts on `P2_SCENE_CMD_PLAY`. About 17.7 s (Play to `P2_SCENE_DONE`), eight shots, with the Onions.
Built in PC2 (`SceneTakeoff.{h,cpp}`, `SceneOnion.{h,cpp}`) and reworked in TP for more character and a
departure that visibly leaves the planet; everything runs on scene seconds (`Section::frameDt`), so it
plays at the same speed at any frame rate. Older notes further down describe earlier take-offs; where
they disagree, this part wins.

### 11.1 Timeline

"T" is the script clock (seconds from Play). It pauses at a gate. After ignition the clock of everything
that flies is the frame of `s02_ufo.bck` (`mClipFrame`), advanced at 20 frames/s below frame 10,
ramping to 30 at frame 28, then 30. The beats are the table `Takeoff::kBeats` (time, function, gate,
longest wait); the clip-frame beats are `Takeoff::flightBeats`.

| T / clip frame | Shot | Beat | Cue |
|---|---|---|---|
| 0.00 | S1 | Sitting or sleeping actors play `GETUP` at 45 f/s first. Olimar faces the lens; Pikmin turn to him, `KIZUKU` staggered 0.12 s | `LAUNCH_BEGIN` |
| 0.35 | S1 | Olimar nods: two dips of the head (look-at pitch), looking at the player | |
| 1.15 | S1 | Olimar turns to the Pikmin and whistles (`FUE`, sound `PSSE_PL_SHUGO`) until 1.95 | |
| 1.95 | S1 | The cheer, staggered 0.15 s: red somersaults (`ROLLJUMP` 35), yellow and blue hop (`JUMP`); `YATTA` at 0.3 volume, at most two | |
| 2.75 | S1 | Pikmin `RUN2` at 120 u/s to the nearest foot of their own Onion (detour 12 units lens-side if the run passes within 10 of Olimar) | |
| 2.80 | S1 | Olimar looks at the lens once more and nods (the beat before the dash) | |
| 3.15 | S1 | Olimar turns to the ship | |
| 3.30 | S1 | Olimar runs to `start1`, speed `clamp(dist/1.3, 110, 160)` | `HOP` |
| 3.60 | **S2** | Wide reverse: Onions, Pikmin arriving, Olimar running toward the camera | |
| run | S2 | Yellow trips 55% into its run: `KOROBU` (≤0.55 s), `GETUP` brisk (≤0.35 s), `PSSE_PK_VC_LAND`, then dashes on at 115% | |
| arrival | S2 | Each Pikmin climbs its leg (`HNOBORU`, 1.0 s along `OnionActor::legPoint`, shrinking to 0.6 over the last third), is hidden, the Onion gulps (`in.bck`) | |
| 4.70 | **S2b** | Close on the yellow and red legs: Pikmin climbing in | |
| 5.90 | S2b | **Gate**: all Pikmin aboard and Olimar at `start1` (at most 1.0 s). Olimar turns to the ship | |
| 6.00 | **S3** | Medium shot by the ship: Olimar turns round to the lens and looks at it | |
| 6.15 | S3 | He waves goodbye for 1.25 s (`Actor::wave`) | |
| 7.50 | S3 | `JUMP`, ship `suction`, beam emitters 0-2, `0x381F`; the camera leans 75% toward him and the lens opens 40 → 50° as he rides | `BOARD` |
| 7.70-9.00 | S3 | Beam ride: Bezier into `orima`, 1.5 turns, scale to 0.3 | |
| 9.00 | S3 | Olimar hidden, beam stopped, burp A, `0x381E`; pistons 30 → 90 over 0.8 s | |
| 9.10 | | Propellers spin up 0 → 800°/s over 1 s | |
| 9.35 | | Burp B | |
| 9.80 / 9.95 / 10.10 | **S4** | Red, blue, yellow rise: sparkle (s02 emitter 0) on `flow`, leaves (emitter 8) under red and blue; legs fold over 0.9 s from +0.2 s; propeller spread 1 → 1.8; `0x3010` | |
| 11.05 | | Music fades over 2 s | |
| 11.10 | **S5** | The ship trembles (±0.4 at 9 Hz) and starts to rock on its legs (lean about the ground point, 2.6 / 3.3 Hz, building to 0.7°); plumes 5/6 at alpha 60, shake builds; ambient petals fade out | |
| 12.45 | S5 | Clip frame 0: lift-off clip, jets 1-4, leaves 7, plumes to alpha 120, roar `0x300E`; vapour trails (the exhaust trail, emitter 1, at 0.42) start on the three Onions' bodies; Onions switch to the clip's height | `IGNITE` |
| frame 8 | S5 | Body leaves the ground, shake 1.2, the rocking peaks at 2.2° and dies away by frame 40 | `LIFTOFF` |
| frame 20 | **S6** | Aerial: above the convoy looking down past the ship and the Onions at the forest; the map is drawn, fogged | |
| frame 41, 48 | S6 | Leaf rustle `0x86F`; plumes and leaves stop | |
| frame 70 | **S7** | Wide, from far off and low: the four craft climb on their trails; jets' glow down to alpha 110; the sky darkens toward space and stars come out | |
| frame 120 | S7 | Fade 0.75 s in `fadeColor`, every pooled sound fades with it | `EXIT_FRAME` |
| ~17.7 | | Section ends | `P2_SCENE_DONE` |

Measured headless (Xvfb, `MPPM_SCENE_SPEED=0.5`, halved): cues at 0, 3.30, 7.64, 12.59, 12.99,
16.89 s, done at 17.66 s. Every cue keeps its order; the UI follows the cues (DESIGN 7.1). The launcher's
fallback `kFullTimes` (`prelaunch.cpp`, used only when the scene hangs) still holds the PC2 times.

Under the fade (normal or skip) motion continues but no beat, cut, sound or emitter starts. Skip:
0.3 s fade, both demo sets stopped, music faded if still up (checked: skip 4 s in, done 0.28 s later).
On Play the scene re-reads `takeoff`, `reduceMotion` and `fadeColor`. `takeoff` 1 (the old Short) plays
the full take-off; 0 is the 0.4 s fade. If the Onions or `s02_ufo.bck` are missing the take-off falls back
to the fade. The log line `scene: take-off: Pikmin <n> aboard at <T> s` helps tune the gate.

### 11.2 Onions (`SceneOnion.{h,cpp}`, `SceneAssets::loadOnions`, `SceneStage`)

- `goal.bmd` with `J3DMLF_UseUniqueMaterials | J3DMLF_UseSingleSharedDL`, `kidou_red/yellow/blue.brk`
  on the one model data, clips from `texts.szs` `onyonanimmgr.txt` (loop markers kept).
- Three unlocked `SysShape::Model(data, 0, 2)`. Each sets its colour animation to the last frame just
  before its `entry()` (`Onyon::changeMaterial`). The shared materials are flagged
  `J3DMP_IsChanged`: otherwise the material-sorted draw buffer merges the three packets into the first
  model's display list and all Onions come out red.
- Layout (stage frame, all areas; `StageInfo::mOnions` can override per area): red (-215, 450) yaw -56
  (moved from its authored (-167, 203), which would fill the left of the idle picture), yellow (19, 513)
  180, blue (166, 268) -90. None is in the idle picture at 4:3, 16:9 or 21:9 (checked on screen).
- Flight clips `s02_onyon{r,y,b}.bck` are only sampled for the root height (`clipY`). Before ignition
  the rise is a Hermite curve from the rise start that meets the clip's height and climb rate at frame 0.
  Heights (logged): the ship's body climbs ~24 units a frame from frame 12 (376 at frame 20, 1587 at
  70, 2780 at 120, holding 4650 from frame 200); the Onions start ~270 up at ignition, lag (880 at
  frame 60, 2035 at 120) and catch the ship at about frame 175.
- Joint callbacks: `flow` spun and spread; legs (joints 2-10) blended from the `flowwait` pose to the
  flight clip's folded pose under their parents' final matrices.
- Shadow disc 50 units, gone by 150 units of lift. Pikmin off the ground (climbing) cast none.

### 11.3 Shots (`kShots`, stage frame, tuned on screen with `MPPM_SCENE_HOLD` + `MPPM_SCENE_CAM`)

| Shot | Eye | Target | FOV | Move |
|---|---|---|---|---|
| S1 | idle eye → +(-4, 1, -14) over 2.4 s | idle target, 35% toward Olimar's head, spring ω 4 | idle × 1 → 33/36 | |
| S2 | (60, 40, 40), drifting -10 X | (-40, 30, 430) | 50 | |
| S2b | (100, 45, 180), creeping 6% in (both Onions stay whole in frame) | (-70, 46, 470) | 44 | |
| S3 | (-38, 16, 200) | (-6, 17, 120), 75% toward Olimar while he rides, spring ω 3 | 40 → 50 from `BOARD` | |
| S4 | (70, 40, 800), past the Onions back at the ship, all three apart | (0, 100 + 0.5 × mean lift, 250) | 48 | |
| S5 | (-110, 4, 240) | (0, 75, 0), then `body_s` with spring ω 5.5 | 44 | shake, rocking ship |
| S6 | (-170, h + 600, -300), turning 14° round the site; h follows 82% of the convoy's climb since the cut | (40, mid, 300) | 52 → 46 | shake dies in 0.5 s |
| S7 | (-800, 450 + 35% of the climb since the cut, -200) | (0, h0 + 70% of the climb since the cut + 90, 250): the aim lags, so the craft rise through the frame | 40 → 30 | sky darkens, stars |

`mid` is the middle height of the convoy (`Takeoff::convoyHeight`: the ship's body and the Onions'
mean body), stage frame; `h0` is `mid` at the cut.

- A cut resets the look-at spring to the new target at rest, sets particles to 30 steps/s and forces a
  step.
- Ground shots (S1-S5): below 16:10 the 16:10 horizontal view (S1 inherits it from the idle lens); top
  edge capped per shot (`kShotSkyEdge`: 40°, S2 20°, S2b 20°, S4 36°). Facing +Z the backdrop rim is
  only 15-20° up. S6 and S7 keep the 16:10 horizontal view too.
- Per area (`StageInfo::mShots`, `SceneStage.cpp` `kOpenShots`): in Valley of Repose, Perplexing Pool
  and Wistful Wild the wall behind the ship is low (its rim and the sky, or Perplexing Pool's bare white
  fog wall, filled S4), so S4 looks down from high behind the Onions instead: eye (-120, 260, 820),
  target (0, 60, 300), FOV 46, the ground behind them. S3 and S5 there keep the top edge under 24°.
  Awakening Wood keeps the defaults (its forest wall stands tall).
- S6 draws the map (the forest seen from above, in the area's fog). S7 leaves it out
  (`Takeoff::skyShot`): from far off and low its backdrop would stand in the lower picture.
- The sky in S7: the section draws its gradient from `SceneLightMgr::mSkyTop` / `mSkyZenith`; the
  take-off saves them at Play and, from frame 70 to 120, eases them toward space (`kSpaceTop`
  (52, 86, 160), `kSpaceZenith` (8, 14, 44)) in `Takeoff::updateSky` and puts the area's own back when it
ends (`Takeoff::init` forgets them, so a new section takes its own). Stars: `SceneEffects::setStars(alpha)`, 420 fixed points above
  8° drawn first in `SceneEffects::draw` as additive soft dots (hexagon fans, alpha falling to 0 at the rim) 9000 units out (depth-tested
  behind the craft), fading toward the horizon, alpha `smoothstep(1.5u - 0.35)`.
- Reduce Motion: same beats and cues; S1-S5 hold their calm pose (start eye, start target, calm FOV),
  no drift, spring, roll, shake or ship tremble and rocking. S6 and S7 still follow the convoy (they would
  lose it at once) but without orbit, lens change, spring or shake.
- The launcher's bloom gets the ship's screen position only in S5; at the S6 cut it is told off-screen.

### 11.4 Q3 changes

- S1: the whistle turns Olimar from the lens toward the Pikmin by at most 40 deg (`kWhistleTurn`), his
  head does the rest; at the cheer (1.95) he squares up to the lens again, head still on the Pikmin.
  He no longer shows the player his back before the dash to the ship.
- S6: the map's light shafts (materials `*hikari*`) are hidden in the aerial shots (`MapActor::showShafts`),
  and the fog is pushed back by the eye's height above the site (`SceneLightMgr::mFogPush`), so the
  forest seen from above is no longer washed out white.
- Visit idle event: Olimar's MIZUNOMI (a drink: face in the ground) became a kneel: frames 0-5 down in
  0.35 s, held, back up in 0.4 s, 1.9 s in all (`Actor::mKneelTime`, time-based).
- Arms re-checked in the clip viewer, front and side, every whitelisted clip: no glove inside the torso,
  none detached.

### 11.9 History (pre-PC2)

**As built (W9c, `SceneTakeoff.cpp`, `SceneEffects.cpp`):**
- R4 checked: the 26 tracks of `s02_ufo.bck` line up one to one with the 25 joints of `ufo.bmd`
  (body_s, pistons, radar where the model has them); the 26th is never asked for. The clip turns
  the root joint `model` by 135° (the movie's actor angle); the script undoes that with
  `ShipActor::mSpin`. At ignition the part-motion animators let go of `pmotion1-3` so the clip
  drives pistons and radar. `ShipActor` sets fractional clip frames (smooth at 60 fps).
- The three demo effect sets live in emitter-manager slots 7 (s00), 5 (s01) and 6 (s02) at the same
  time (R6, second option). Emitters are immortal until the script fades them, and follow their
  joint's matrix after the models are posed each frame.
- Departures from the movie, for a camera a few ship-heights away in daylight: the smoke plumes and
  the leaves stay where the ship stood (launch cloud, leaves torn off the ground) and are drawn at
  0.5-0.6 size; the exhaust trail at 0.55. Their authored colours are dark (made for dusk).
- `EXIT_FRAME` comes when the ship's jets leave the top of the picture or at clip frame 84 (Full) /
  60 (Short), whichever is first. No cue is sent after it.

**F2 fixes (QA round 1):**
- Tilt: the map backdrop is a cylinder that ends about 46-47° above the take-off eye's horizon, with
  flat sky above it. The pitch is now clamped so the picture's top edge stays below 44°
  (`kSkyEdgeDeg - fovY / 2`, about 21° with the 46° lens), so the ship climbs out of an all-forest
  picture. The ship therefore leaves the top sooner: `EXIT_FRAME` now usually comes from the
  ship-gone test at about T 4.3-4.6 (clip frame ~45-55) rather than at frame 84.
- Smoke: the exhaust trail and both ground plumes draw at reduced alpha (110-120 of 255,
  `DemoEmitter::mAlpha`), so the dusk-dark smoke reads as smoke, not a black column.
- **Cue timing contract for the UI** (F1 makes the UI cue-driven): `LAUNCH_BEGIN`, `HOP`, `BOARD`,
  `IGNITE` and `LIFTOFF` keep the fixed times above (scene-clock seconds, so under a slow frame rate
  they come later in wall time, never earlier relative to each other). `EXIT_FRAME` is *variable*:
  it is the one cue the UI must wait for before starting its flash, never a timer. It is always
  sent once, after `LIFTOFF` (or at once on Off, and on a skip), and it is always followed by
  `P2_SCENE_FADING` in the same frame and `P2_SCENE_DONE` after the fade (0.75 s Full / Short,
  0.40 s Off, 0.30 s skip). Nothing after `EXIT_FRAME` is sent as a cue.
- Ambient particles: the area emitter (s00 sakura etc.) has a near cull; particles closer than 95
  units to the eye are not drawn (`SceneEffects::update(eye)`).
- Leaves: drawn with happajnt3's position and orientation but the actor's uniform scale and an
  orthonormal normal matrix (Kizuku and others stretch the stem joint up to 2x), tilted no further
  than 60° from upright.
- Personal space: `Director::separate` eases actors apart (Olimar 5.5 + Pikmin 3.75 units radius);
  a walk that ends inside another actor stops at arm's length.
- 4:3: below 16:10 the idle lens keeps the 16:10 horizontal view (Vert+) and the aim swings up to
  4° further, so the cast stays right of the menu column (yellow from x 0.45 at 1024x768).
- Sounds: the roar 0x300E and the leaf rustle 0x86F play; the Pikmin cheer 0x2831 plays at 0.3
  volume; the beam 0x381F and Olimar's jump are inaudible (their waves are not in the loaded
  scenes, R7). Sounds still playing at the fade are faded with it.

**G2 fixes (QA round 2):**
- Skip: `Takeoff::skip` no longer ignores a skip before scene-clock 0.5 s. The UI gates the skip on
  its own wall clock and sends the command once; with the scene clock behind (slow frames, a hitch
  as the take-off starts) the command was dropped and the UI sat behind its flash and the navy
  loader for the whole hidden take-off. Bridge ABI unchanged: one `P2_SCENE_CMD_SKIP` is enough,
  in `ST_Idle` (it starts the Off fade) or in `ST_Takeoff` (0.3 s fade, `EXIT_FRAME` at once).
- Lift-off colours: the s02 set is made of sunset silhouettes. `ufo_leaf` (ids 7/8) is drawn in its
  primary colour alone (TEV colour mode 5) and that colour is black; the ground plumes (5/6) blend
  between two near-blacks; the exhaust trail (1) animates to dark navy and its first puffs stay on
  the ground where the ship stood. `Takeoff::init` recolours these resources once
  (`SceneEffects::tint`, colour animations keep a quarter of their own shading): olive leaves, tan
  dust, light grey exhaust. The ship's shadow disc is now gone 120 units up (was 400), so it no
  longer lies on the ground after the ship has left the picture.
- Take-off camera: the eye steps left (idle eye + (-35, -1, -4)) instead of pulling back to
  (-20, 7, 384); lens 40 deg (Vert+ below 16:10, like the idle lens); the aim leans 22 units toward
  the group and keeps at least 8 deg up, so the horizon sits in the lower third, the Pikmin's backs
  fill the bottom and the ship stands about half the picture tall. Sky edge clamp 44 -> 40 deg: no
  sky strip at the top corners at 4:3, 16:9 or 21:9.
- The "horizontal seam across the backdrop" during the tilt is not in the picture: it is the
  headless capture tearing (an `import` of the Xvfb root window while a frame is presented). The same
  line cuts the ship in two, with the lower half from the next frame. It only shows while the camera
  moves fast, always at about the same height.
- Visit: Olimar kneels 18 units from the Pikmin (was 12.5); his helmet stays ~5 units clear.
- Leaves: the blade (normals along local +Y) is lit from whichever side the sun is on and never
  turned more than 63 deg from it for lighting (`leafNormalMatrix`), so a leaf seen from behind or
  edge-on to the sun is no longer near-black. The stair-stepped silhouette is the leaf texture's
  alpha-test cut-out (original asset), left as it is.

After the section ends, `GameFlow::createSection` builds `Title::Section` as usual
(`gameflow.cpp:154-156`). Nothing in the title changes.

**Optional arrival intro (phase 5)**: on the first frame after loading, play `s00_ufo.bck` from
frame 60 (ship descending) with the landing jets and ground smoke from `s00/efx.jpc`, cockpit covers
open at frames 183-249, Olimar appears at `start1` with `s00_alive1.bck`, the Pikmin run in from the
right. Menu items fade in when the ship touches down (frame 186). Off by default until it is tuned.

---

## 12. Settings this scene reads

Declared in SETTINGS.md 9.1 (INTERFACE). `port/launcher/scene_bridge.cpp` maps them to `P2SceneConfig`.

| Setting | Key | Values (first = default) | `P2SceneConfig` |
|---|---|---|---|
| Live Start Menu | `interface.liveBackground` | `true`, `false` | `live` (parked when off) |
| Location | `interface.sceneArea` | `forest`, `tutorial`, `yakushima`, `last`, `season` (Dec-Feb Valley, Mar-Aug Wood, Sep-Nov Wild; Pool is not offered) | `area`; `season` is resolved by the host |
| Time of Day | `interface.sceneTime` | `morning`, `day`, `evening`, `night`, `clock` | `timeOfDay`; `clock` is resolved by the host |
| Music | `interface.sceneMusic` | `piklopedia`, `worldmap`, `challenge`, `hiscore`, `off` | `music` |
| Take-off | `interface.launchAnimation` | `full`, `off` (`auto`/`short` read as `full`, P3) | `takeoff`; the scene plays the full take-off for any non-zero value (11.1) |
| Reduce Motion | `interface.reduceMotion` | `false`, `true` | `reduceMotion`, `fadeColor` |

All of them apply while the start menu is open (4.2, 6.4): Location reloads the scene behind the
backdrop, Time of Day blends, Music swaps, Take-off and Reduce Motion are copied, Live Start Menu
parks or unparks the scene.

With Live Start Menu off and no disc on the command line the game is not booted behind the menu: the
start menu runs on its own over the launcher's backdrop and the game boots on Play. Turning Live
Start Menu on there (with a usable disc), or picking a disc with it on, boots the game in place
behind the same menu (`run_standalone_menu`, `keepMenu`; `show_prelaunch` reuses the document
through `Prelaunch::game_started`). A game booted behind the menu with Live off (a disc on the
command line) runs the scene parked: the backdrop shows, its ship takes off on Play, and the title
loads after Play.

---

## 13. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | Loader flags that work inside `BaseGameSection` may rely on state set elsewhere (j3dSys draw buffers per layer, `Sys::DrawBuffers`, light mask setup). | Phase 1 goal is one lit Olimar on screen. Compare against the title scene's simpler flags (`J3DMLF_Material_PE_FogOff \| J3DMLF_21`, `ebiP2TitlePikmin.cpp:99`) if a model renders wrong. |
| R2 | Models are loaded on the DVD thread; on the host that is a cooperative thread, but display-list or texture creation may still expect the main thread. | The title and gameplay load the same way. If a model misbehaves, move `makeDL`/`lock` to `doLoading`'s completion step. |
| R3 | The map viewed from 11 units up shows things the gameplay camera never sees: missing backfaces, the edge of the world, low-detail far scenery. | `azimuthDeg` per stage, fog near/far, sky gradient, and as a last resort a different area as default. Check all four in phase 4 before enabling "By season". |
| R4 | `s02_ufo.bck` declares 26 joints, `ufo.bmd` has 25. J3D indexes animation joints by joint number, so the extra entry should be ignored, but the PC anim converter (`port/assets/j3d_anim.cpp`) may assume equal counts. | Test in phase 3 first thing. Fallback: procedural lift-off (ease-in Y, three turns of spin, pmotion speed-up) using the keyframe values in 3.3. |
| R5 | Clip meanings are guessed from names. | Clip viewer first; the behaviour table is data. |
| R6 | Only one demo particle resource set fits in slot 7 (`setDemoResourceManager`), and three are wanted. | Swap between phases as specified (ambient emitters must be killed before the swap), or call `mEmitterManager->entryResourceManager(mgr, n)` directly for other free slots: the manager was created with 9 group ids and 8 resource slots (`particleMgr.cpp:109`). |
| R7 | Sound effects outside the system category may need creature sound objects or unloaded wave banks. | Music first; effects are a separate step with a fallback of none. |
| R8 | Creating and destroying `ParticleMgr`/`TPkEffectMgr` before the game does. Both are global singletons that `BaseGameSection` creates itself (`baseGameSection.cpp:2081`). | Delete both in `doExit` so the pointers are null again, as `baseGameSection.cpp:152` does. |
| R9 | `ShadowCylinder2` copies the EFB alpha into a texture (the PC fix in commit `164ada6ee`). It was only exercised with the gameplay viewport. | If it fails here, use `ShadowCylinder3` (screen filter, no copy) or flat blob quads. |
| R10 | Heap: the section heap is created from all free space of the root menu heap and destroyed afterwards; `pikis.szs` is deliberately mounted on the system heap. Mounting it earlier than the title changes allocation order on the system heap. | Host arena is 256 MB. The matching build is unaffected. |
| R11 | Boot time until the scene appears (boot section + map load). | Opaque backdrop with a loading hint until `P2_SCENE_READY`; H3 removes the logo holds. Measure; the map is the only large file. |
| R12 | Frame cost of a full area model at 60 fps on weak GPUs. | It is what gameplay draws at 30 fps with far more on top; if needed run the scene at `setFrameRate(2)`. |
| R13 | A wrong or damaged disc region (only GPVE01 is supported) lacks a file. | Every mount and `getResource` is checked; on failure report `P2_SCENE_FAILED` and idle empty until Play (section 4.3); the menu keeps its backdrop. No asserts on this path. |
| R14 | New sounds (`0x7` whistle, `0x2815`, `0x281D`, `0x3010`, `0x381E`) may be silent in the title's sound scene. | Not checked by ear (headless, dummy audio). Silent ones stay silent; all are pooled and faded. |
| R15 | Red's moved spot or the S4/S2b eyes may meet uneven ground or rocks in other areas. | `StageInfo::mOnions` per-area override; only Awakening Wood was checked on screen. |
| R16 | Pikmin climb upright (actors turn about Y only); they sit on the leg's centreline. | Reads as climbing at S2b's distance; a lean would need an actor pitch. |

---

## 14. Phases

1. **Skeleton** — hooks H1 and the bridge; empty section that clears to a gradient, reports READY,
   reacts to PLAY with a fade and ends; flow into the title verified, including headless
   (`MPPM_FORCE_LAUNCHER=1 MPPM_SCENE_AUTOPLAY=3`).
2. **Characters** — assets, Olimar + three Pikmin + leaves on a flat colour ground, lights, camera,
   `WAIT` loops. Debug clip viewer; fix the clip table. Shadows.
3. **Stage** — ship with part motions, area map, `light.ini`, fog, sky, ambient particles. Camera
   composition tuned at 4:3, 16:9, 21:9 and 16:10. Music (H2).
4. **Behaviour** — director, fidgets, events, long-idle, pointer glances. Reduce-motion.
5. **Take-off** — `s02_ufo.bck` (R4 check), particles, camera, fade, skip, variants. Fast boot (H3).
6. **Polish** — sound effects, the other three areas and "By season", time of day from the clock,
   growth stages, onions lifting off beside the ship (`s02_onyon*.bck`), arrival intro, sky dome.

Phases 1-3 give a usable live background; 4 and 5 are the parts the request describes in detail.

---

## 15. Open questions

1. **Default area.** Awakening Wood (green, petals) is proposed. Valley of Repose is the first area
   of the game but is snow. Should the default be "By season" once all four are tuned?
2. **Take-off length.** The full sequence is about 6.4 s on every start. Is Full the right default,
   or Short (4 s) with Full on first run only?
3. **Boot logos.** With the scene enabled the Nintendo and Dolby logos and the health warning are
   skipped behind the backdrop. Keep them skipped, or show them after the take-off before the title?
4. **Fade colour.** Settled in review: the UI flash colour `#FFF6E0` (DESIGN.md 7.2), navy for Off and
   reduce motion. Still open: the flash clears onto the title's own fade-in from black; is that
   white-to-black step acceptable?
5. **Who else is on screen.** Only Olimar is specified. Louie, the President after the debt is paid
   (needs save data), or the three onions beside the ship are all cheap to add. Wanted?
6. **Depth of field.** A real background blur needs a post-process pass in Aurora. Is that worth a
   patch, or is fog plus the menu's own blur enough?
7. **Sound effects.** If creature voices cannot be played without the gameplay sound objects, is a
   music-only scene acceptable for the first release?
8. **Pikmin 2 spoilers.** "By season" can show Wistful Wild to a player who has not unlocked it.
   Restrict to unlocked areas (needs reading the save) or accept?
