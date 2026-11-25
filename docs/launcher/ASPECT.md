# Resolution and aspect-ratio support

Spec for widescreen (16:10, 16:9, 21:9, any window shape) and internal-resolution scaling in the
native port. Paths are relative to the repo root; `A:` is `extern/aurora/`, `D:` is the Dusklight
clone. Phases 1 and 2 are implemented; section 8.1
lists what phase 3 has so far.

## Review notes

Changed in the cross-spec review (see `README.md` in this folder):

- Section 4: the four keys are declared in SETTINGS.md (3 and 9.1), which had a different name
  (`video.aspect`) and no HUD keys; SETTINGS.md now uses the names below. Default and netplay class
  of `video.aspectRatio` are stated here.
- Section 5: `port/sdk/wide.cpp` does not read the settings library; the launcher pushes values in
  through `p2_wide_configure`. A start-menu override keeps the launcher at the window's shape.
- Section 10: `MPPM_WINDOW` maps to the `video.windowWidth/Height` keys of SETTINGS.md.

## 1. Decisions

1. **The logical EFB stays 608x448 forever.** The render mode table (`src/sysGCU/system.cpp:33-90`)
   is not touched. Every hard-coded 608/640/448/480 coordinate, EFB-copy rectangle and boot-time
   texture size stays valid.
2. **Widescreen = Aurora "stretch" policy + compensation in the game.** Aurora maps the logical
   608x448 rectangle onto a content framebuffer that has the wanted aspect; the game multiplies
   every 3D camera aspect by `k` and shrinks every 2D viewport by `1/k`. This is Dusklight's model.
3. **`k = contentAspect / (608/448)`**, sampled once per frame, clamped to `[1, 3.6]`. One number
   drives everything.
4. **3D is Hor+**: vertical FOV unchanged, more is visible at the sides. Frustum culling follows
   automatically because it reads the same `mAspectRatio`.
5. **2D is pillarboxed by a viewport remap in one function** (`J2DGrafContext::setPort`), not by
   editing projection matrices. Screen-covering fills are widened by a rule in
   `J2DGrafContext::fillBox`.
6. **"Original" is 608:448 (19:14 = 1.357), square pixels**, labelled "Original (4:3)". A real
   GameCube shows this image at about 1.35:1, so a true 1.333 option is not offered.
7. **Scenes that are not verified wide fall back to Original for their duration** (aspect lock
   switched at scene change). First candidates: Piklopedia/Treasure Hoard, THP movies, boot logos.
8. **Internal resolution = `VISetFrameBufferScale(n)`**: height `448*n`, width follows the content
   aspect. 0 = Auto (content framebuffer = window pixels).
9. **HUD anchoring is per `.blo` screen**, an X offset applied in `P2DScreen::Mgr_tuning::draw`.
   The HUD is already built from one screen per element, so no pane surgery is needed.
10. **One small Aurora patch**, `port/patches/aurora/0002-vi-lock-aspect-ratio.patch`, implements
    the declared-but-missing `VILockAspectRatio`. Auto and Original work without it.

## 2. What exists today

### 2.1 Game

| Topic | Where | Notes |
|---|---|---|
| Render modes | `src/sysGCU/system.cpp:33-90` | All four are `fbWidth 608`, `efbHeight 448`. `getRenderModeObj` at `:945`. |
| 3D viewport | `src/sysGCU/graphics.cpp:102-115` (`Viewport::Viewport`), `:206-214` (`refresh`), `:230-237` (`setViewport`) | Bounds come from the render mode; split-screen halves them (`HorizonalSplitter`, `:18-52`). |
| Camera aspect | `src/sysGCU/graphics.cpp:185-200` (`Viewport::updateCameraAspect`) | `mCamera->mAspectRatio = width / height`. **The single source of 3D aspect.** |
| Projection | `src/sysCommonU/camera.cpp:320-328` (`Camera::setProjection`) | `C_MTXPerspective(mViewAngle, mAspectRatio, near, far)`; `mViewAngle` is vertical. |
| Culling | `src/sysCommonU/camera.cpp:133-160` (`CullFrustum::updatePlanes`), `include/Camera.h:52` (`getFOV`) | Side planes use `atan(mAspectRatio * tan(fovY/2))`. Follows the aspect with no change. |
| Other readers of `mAspectRatio` | `src/plugProjectEbisawaU/particleMgr.cpp:220`, `src/sysGCU/JSTObjectCamera.cpp:287`, `src/sysCommonU/camera.cpp:390` | All correct once the stored value includes `k`. `calcProperDistance` takes the max of the horizontal and vertical fit, so it does not zoom in at wide aspects. |
| Shared 2D contexts | `src/sysGCU/graphics.cpp:886-903` (ortho), `:909-922` (persp) | `mOrthoGraph`: 0..608 x 0..448. `mPerspGraph`: fov 30, same bounds. **Most HUD and menus draw through `mPerspGraph`.** 173 `setPort()` calls. |
| 2D port setup | `src/JSystem/J2D/J2DGrafContext.cpp:22-41` (`setPort`), `:93-110` (`setScissor`), `:196` (`fillBox`) | All 2D viewports and scissors pass through here. |
| 2D projections | `src/JSystem/J2D/J2DOrthoGraph.cpp:29-35`, `src/JSystem/J2D/J2DPerspGraph.cpp:46-51` | Left unchanged. |
| HUD composition | `src/plugProjectOgawaU/ogObjGround.cpp:66-72`, `ogObjCave.cpp:75-80`, `ogObjChallenge1P.cpp:61-65`, `ogObjVs.cpp:85-86` | One `Mgr_tuning` screen per element: `sun_meter.blo`, `doping.blo`, `orima.blo` (x2), `gr_pikmin.blo` / `cave_pikmin.blo`, `sensor.blo`, `grand_cave_poko.blo`. |
| HUD draw choke point | `src/sysGCU/P2DScreen.cpp:105-115` (`Mgr_tuning::draw`), `src/plugProjectOgawaU/ogBloGroup.cpp:38-53` (`addBlo`) | `setOffset(mTranslationX, mTranslationY)` every draw. |
| Cutscene letterbox | none | Pikmin 2 draws no letterbox bars; nothing to disable. |

### 2.2 Aurora

- `AuroraSetViewportPolicy` (`A:include/dolphin/gx/GXAurora.h:122-132`, applied `A:lib/gx/gx.cpp:226-236`):
  `FIT` makes the content framebuffer the logical aspect; `STRETCH` makes it the window aspect;
  `NATIVE` disables scaling. mppm never calls it, and `g_frameBufferAspectFit` starts `false`
  (`A:lib/window.cpp:47`), so **today the game is stretched to whatever shape the window has.**
- Viewport, scissor and EFB-copy source are scaled per axis by `target / logical`
  (`A:lib/gx/gx.cpp:242-297`). Copy destinations are scaled the same way
  (`A:lib/dolphin/gx/GXFrameBuffer.cpp:16-32`), so copies gain resolution and shape with the target.
- `VISetFrameBufferScale` (`A:include/dolphin/vi.h:42-46`, `A:lib/window.cpp:570-579`, used `:457-463`):
  `scale > 0` gives height `efbHeight * scale` and a width from the window aspect; `0` = window pixels.
- `VILockAspectRatio` / `VIUnlockAspectRatio` are declared (`A:include/dolphin/vi.h:54,59`) and
  **have no definition** anywhere in `A:lib/`.
- Presentation letterboxes or pillarboxes the content framebuffer inside the window
  (`A:lib/webgpu/gpu.cpp:337-363`). RmlUi draws inside that same rectangle (`A:lib/rmlui.cpp:57-61`).
- `AuroraGetRenderSize` (`A:lib/dolphin/gx/GXAurora.cpp:39-48`) returns the content framebuffer size.
- `GXSetViewportRender` / `GXSetScissorRender` (`A:lib/dolphin/gx/GXAurora.cpp:55-73`) take
  render-target pixels and skip the logical mapping. Not needed for the MVP.

### 2.3 Dusklight (reference)

- Twilight Princess already has a Wii wide mode: `mDoGph_gInf_c` keeps `m_minX`, `m_width`, `m_aspect`
  for a logical screen wider than the 608x448 framebuffer (`D:src/m_Do/m_Do_graphic.cpp:566-597`).
- Dusklight sets the wide width from the real render size every frame:
  `l_tvSize[1].width = renderW / renderH * 448` (`D:src/m_Do/m_Do_graphic.cpp:801-808`, called from
  `D:src/m_Do/m_Do_main.cpp:283`), with `AURORA_VIEWPORT_STRETCH` (`D:src/m_Do/m_Do_main.cpp:728-731`).
- 2D orthos use `getMinXF()` / `getWidthF()`; cameras use `getAspect()` (`D:src/d/d_camera.cpp:11618`).
- HUD groups are pinned with `ScaleHUDXLeft` / `ScaleHUDXRight` (`D:include/m_Do/m_Do_graphic.h:135-136`)
  against a safe area from `SDL_GetWindowSafeArea` (`D:src/m_Do/m_Do_graphic.cpp:720-799`).
- HUD scale keeps a named corner fixed (`dAnchorHudScale`, `D:src/d/d_meter2_draw.cpp:45-66`).
- "Lock 4:3" is just `AURORA_VIEWPORT_FIT`; internal resolution is `VISetFrameBufferScale`
  (`D:src/dusk/settings.cpp:237`). Dusklight has no fixed 16:9 / 21:9 choices.
- Pikmin 2 has no wide mode in the game, so mppm must add the equivalent of `mDoGph_gInf_c`'s state.

## 3. Model

Constants: `W0 = 608`, `H0 = 448`, `A0 = W0/H0 = 1.35714`, `CX = 304`.

```
contentAspect = renderW / renderH          (AuroraGetRenderSize)
k             = clamp(contentAspect / A0, 1, 3.6)      k = 1 when aspect mode is Stretch
3D camera     : mAspectRatio = (vpW / vpH) * k
2D viewport   : x' = CX + (x - CX) / k      (applied to both edges of the port rectangle)
visible 2D X  : minX = CX * (1 - k),  maxX = CX * (1 + k),  width = W0 * k
```

| Aspect | k | 3D aspect | 2D viewport (logical px) | 2D visible X range | 1x internal size |
|---|---|---|---|---|---|
| Original 19:14 | 1.000 | 1.357 | 0..608 | 0..608 | 608x448 |
| 16:10 | 1.179 | 1.600 | 46.1..561.9 | -54.4..662.4 | 717x448 |
| 16:9 | 1.310 | 1.778 | 71.9..536.1 | -94.2..702.2 | 796x448 |
| 21:9 (64:27) | 1.747 | 2.370 | 130.0..478.0 | -227.0..835.0 | 1062x448 |
| 32:9 | 2.620 | 3.556 | 188.0..420.0 | -492.5..1100.5 | 1593x448 |

Why a viewport remap for 2D instead of widening projections: both give the same picture, but the
remap is one function for ortho and perspective graphs, leaves every matrix the game reads back
untouched, and code that sets its own viewport and projection (shadow mask, EFB clear, screen
copies) keeps covering the whole frame without being touched.

Windows narrower than 19:14 keep `k = 1`. In `auto`, windows from 4:3 to 19:14 show the 19:14
frame slightly squeezed (at most 1.8%, as on a 4:3 TV); only windows narrower than 4:3 get bars
above and below.

## 4. Settings

Added to `port/launcher/settings.hpp` / `settings.cpp` (existing key style: `interface.uiScale`).

| Key | Type, default | Values | Applies |
|---|---|---|---|
| `video.aspectRatio` | string, `"auto"` | `auto`, `original`, `16:10`, `16:9`, `21:9`, `stretch` | runtime |
| `video.internalResolution` | int, `0` | `0` Auto, `1`..`8` (x448 lines) | runtime |
| `interface.hudLayout` | string, `"edges"` (`"centered"` until phase 2 ships) | `centered`, `edges`, `stretched` | runtime |
| `interface.hudMaxAspect` | string, `"16:9"` | `16:9`, `21:9`, `unlimited` | runtime |

The keys are declared once, in SETTINGS.md (section 3 for `video.*`, 9.1 for `interface.*`).
`video.aspectRatio` ships with the default `original` and only the values `original` and `stretch`
listed; the phase 1 change set flips the default to `auto` and lists the rest. Netplay class: **S**
(host's value) until the simulation frustum of section 11 exists, then **L**. The HUD keys are **L**.

Menu placement: aspect ratio and internal resolution under VIDEO; HUD layout and HUD limit under
INTERFACE. Internal resolution rows are labelled `Nx (W x H)` from the current content aspect, as
Dusklight does.

Meaning of `video.aspectRatio`:

| Value | Aurora state | k |
|---|---|---|
| `auto` | unlock; content = window shape (lock to 19:14 when the window is narrower than 4:3) | from content |
| `original` | lock 19:14 | 1 |
| `16:10`, `16:9`, `21:9` | lock that ratio; bars if the window differs | from content |
| `stretch` | unlock | forced 1 (classic stretch, everything distorted) |

Meaning of `interface.hudLayout`: `centered` = 2D in the middle 19:14 area; `edges` = same, with
left/right HUD screens shifted to the edges; `stretched` = 2D viewport remap off, so all 2D fills the
width and is `k` times wider.

## 5. Host state

New `port/include/p2_wide.h` (C-compatible, included by game files under `#if TARGET_PC`) and
`port/sdk/wide.cpp`.

```c
void p2_wide_update(void);            // once per frame; samples AuroraGetRenderSize
void p2_wide_configure(int stretch, int hudLayout, f32 hudMaxAspect); // pushed by mppm::video::apply();
                                      // hudLayout 0 centered, 1 edges, 2 stretched; hudMaxAspect 0 = unlimited
f32  p2_wide_k(void);                 // 1.0 when off
f32  p2_wide_2d_k(void);              // k, or 1.0 when hudLayout is "stretched" or inside a raw scope
f32  p2_wide_map_x(f32 x);            // 304 + (x - 304) / p2_wide_2d_k()
f32  p2_wide_min_x(void);             // 304 * (1 - k2d)
f32  p2_wide_max_x(void);             // 304 * (1 + k2d)
void p2_wide_raw_begin(void);         // nestable: 2D remap off (capture passes)
void p2_wide_raw_end(void);
void p2_wide_scene_original(int on);  // nestable: scene wants Original; applied at the next frame
f32  p2_wide_hud_shift(int anchor);   // -1 left, 0 centre, +1 right -> X offset in 2D units
```

- `p2_wide_update` is called in `port/main.cpp` right after `beginHostFrame()` in `p2_host_present`
  (`port/main.cpp:113`) and once before `p2_game_main()` (`port/main.cpp:190`). Aurora handles a
  resize inside `aurora_begin_frame`, so the value is stable for the whole game frame.
- `mppm::video::apply()` (new, in `port/launcher/`) runs on startup after `aurora_initialize`
  (`port/main.cpp:151`) and from each setting's change callback: `AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH)`
  always, `VISetFrameBufferScale(n)`, `VILockAspectRatio(w, h)` or `VIUnlockAspectRatio()`.
- The scene-original counter overrides the lock to 19:14 while it is non-zero.
- `port/sdk/wide.cpp` never includes `borealis/config.hpp` or `port/launcher/settings.hpp`
  (SETTINGS.md decision 4). `mppm::video::apply()` is the only reader of the four keys and calls
  `p2_wide_configure`.
- **Start-menu override.** While the start menu is open (`launcher::prelaunch_open()`, both the
  pre-disc backdrop and the live scene), `mppm::video::apply()` behaves as if
  `video.aspectRatio = auto`: unlocked, `k` from the content shape. RmlUi is confined to the content
  rectangle, so without this an `original` or fixed-ratio setting would squeeze the menu into a
  pillarbox. The live scene is 3D plus two full-width fills, so it is correct at any `k`. The user's
  setting is applied when the menu closes, which happens under the opaque launch flash
  (DESIGN.md 7.2).
- **In-game launcher windows take no override.** The F1 bar, Settings, Controllers and the
  confirmation modals draw over the whole window whatever shape the game picture has: Aurora
  patch `0007-rmlui-full-window.patch` sizes the RmlUi context and its render target to the
  surface, seeds a glass backdrop with the scene inside its present rectangle (black around it),
  and composites the UI over the whole surface. Opening or closing the bar therefore never
  changes the picture (Original, fixed ratios and Stretch used to flip shape on every F1, and a
  scene asking for Original was drawn wide while a window was open).

## 6. Phase 1 (MVP): correct 3D at any aspect, 2D centred

All game edits are `PC_ONLY(...)` / `PC_SWITCH(...)` / `#if TARGET_PC` (`include/types.h:144-150`).

### 6.1 Core, four edits

| # | File:line | Change |
|---|---|---|
| 1 | `src/sysGCU/graphics.cpp:197` | `aspect = x / y PC_ONLY(* p2_wide_k());` |
| 2 | `src/sysGCU/graphics.cpp:134-139` (`Viewport::setProjection`) | `PC_ONLY(updateCameraAspect();)` before `mCamera->setProjection()`, so a resize reaches every camera that draws, including viewports outside `Graphics::mViewports` (treasure-get `baseGameSection.cpp:570-584`, title `ebiP2Title.cpp:123`). Culling planes lag one frame after a resize only. |
| 3 | `src/JSystem/J2D/J2DGrafContext.cpp:40` (`setPort`) | Host: `x0 = p2_wide_map_x(x0); x1 = p2_wide_map_x(x1);` before `GXSetViewport`. |
| 4 | `src/JSystem/J2D/J2DGrafContext.cpp:196` (`fillBox`) | Host: if `box.i.x <= 0 && box.f.x >= 608`, draw with X = `p2_wide_min_x()..p2_wide_max_x()`. Covers `JUTFader` (`JUTFader.cpp:74`), wipes (`wipe.cpp:112,179,289`), loading black (`moviePlayer.cpp:900`), `J2DFillBox` and the ~25 menu fades listed by `grep fillBox`. |

Split-screen needs nothing: each half's viewport aspect is multiplied by the same `k`.

### 6.2 Scissors tied to 2D coordinates

`J2DGrafContext::setScissor` (`J2DGrafContext.cpp:93-110`): map each X edge with `p2_wide_map_x`,
except an edge at `<= 0` stays 0 and an edge at `>= 608` stays 608 (so the default full-screen
scissor still covers the side areas). Pane clip rectangles come through here
(`J2DPane.cpp:379-389`).

Raw `GXSetScissor` calls fed with pane coordinates need the same mapping via a helper
`p2_wide_scissor(x, y, w, h)`:

- `src/plugProjectMorimuraU/mrUtil.cpp:14`, `:394`
- `src/plugProjectMorimuraU/challengeSelect2D.cpp:1041`, `:1052`, `:1085`
- `src/plugProjectMorimuraU/zukan2D.cpp:921`, `:3924`, `:3961`
- `src/plugProjectMorimuraU/vsSelect2D.cpp:266`, `:1533`

Full-width or Y-only scissors need no change: `khFinalResult.cpp:88,782`, `khDayEndResult.cpp:722`,
`khCaveResult.cpp:420`, `gameOver2D.cpp:85,97`, and the `(0, 0, 640, 480)` resets.

### 6.3 Code with its own projection that follows 2D positions

- `src/plugProjectOgawaU/ogObjSMenuMap.cpp:1000` and `:1038`: Z-mask quads positioned from pane
  matrices but drawn with a private `C_MTXOrtho(0..608)` over the current (remapped) viewport. They
  stay aligned because the viewport is what moved. Verify on the pause-menu map; no edit expected.
- `src/plugProjectMorimuraU/gameOver2D.cpp:87-93`: quad drawn at `0..fbWidth` to set destination
  alpha. Widen X to `min_x..max_x`.
- `src/plugProjectEbisawaU/particle2dMgr.cpp:216-229`: projected-texture matrices for 2D particles;
  correct as is, because the viewport is remapped and the matrices are not.

### 6.4 EFB-copy effects

| Effect | Where | At wide aspect | Action |
|---|---|---|---|
| Shadow mask (EFB alpha) | `src/plugProjectNishimuraU/ShadowCylinder.cpp:320-346`, `:507-515` | Own ortho, viewport, scissor, full-screen quads, copy of `0..608 x 0..448`. Whole frame in, whole frame out. | None. Check edge-of-screen shadows. |
| Frame copy for water, treasure and Waterwraith materials | capture `baseGameSectionDraw.cpp:116`, `baseGameSection.cpp:1492`; use `gameSeaMgr.cpp:214-236`, `pelletOtakara.cpp:225-241`, `blackMan.cpp:756-780` | Sampled through the camera's own projection matrix. | None. |
| Projected-texture particles | `particleMgr.cpp:218-226` | Uses `mCamera->mAspectRatio`. | None. |
| 2D screen copy for cave results | `singleGS_CaveResult.cpp:91-94`, capture `baseGameSection.cpp:1489` | Copy holds the full wide frame; it is shown again through a 2D picture sized 608x448. | Draw that picture over `min_x..max_x`. Find the pane at implementation time. |
| Radar map render-to-texture | `RandMapMgr.cpp:260-273`, `:391-418` | Draws at `0..texSize` after `mOrthoGraph.setPort()`, then copies `(0,0)`. The remap would move the drawing away from the copy rectangle. | Wrap `captureRadarMap` in `p2_wide_raw_begin/end`. |
| Indirect panes (hi-score, VS select) | `hiScore2D.cpp:649`, `vsSelect2D.cpp:1522` | Same pattern as the radar. | Raw scope around the capture pass. |
| Piklopedia / Treasure Hoard | `singleGS_Zukan.cpp:1043-1160` (captures), `:1329-1362` (3D window inside a 2D frame) | Several coupled captures plus a sub-rectangle 3D viewport. | `p2_wide_scene_original(1)` in `ZukanState::init` (`:513`), `(0)` in `cleanup` (`:1945`). Proper support in phase 3. |
| THP movies | `src/sysGCU/THPDraw.c:32-47`, `pikmin2THPPlayer.cpp:374-380` | Own ortho and full viewport: the movie would be stretched. | In `THPGXYuv2RgbSetup`, host: `GXSetViewport(p2_wide_map_x(0), 0, p2_wide_map_x(width) - p2_wide_map_x(0), height, 0, 1)`. Sides stay the cleared black. |
| EFB clear | `JFWDisplay.cpp:620-631` | Full frame. | None. |
| Fog range adjust | `fogMgr.cpp:47`, `ebiP2TitleFog.cpp:26-29` | Built from the real projection matrix. | None. Check fog at screen edges at 21:9. |
| Depth of field / blur | none found by grep for EFB copies | No full-screen blur pass turned up; the pause menu uses the frame copy above. | None expected; confirm on the pause menu. |
| Cave darkness | lights, not EFB | | None. |

### 6.5 Side areas behind full 2D screens

With 2D centred, the strips left and right show whatever was drawn before: the 3D scene, or the
clear colour. For screens with an opaque 2D background that are not on top of a 3D scene, the
strips are black, which is the intended pillarbox. Each screen in the test list (section 10) is
checked for a strip showing something it should not; the fix per screen is a `fillBox` of the
strips before the screen draws, or `p2_wide_scene_original`.

### 6.6 Deliverables

- `port/include/p2_wide.h`, `port/sdk/wide.cpp`, `mppm::video::apply()`.
- Settings keys `video.aspectRatio` (`auto`, `original`, `stretch`) and `video.internalResolution`.
- Edits 6.1 to 6.4.
- Aurora patch 0002 (section 9), which adds the fixed 16:10 / 16:9 / 21:9 choices and scene fallback.
  Without the patch: `auto` = `AURORA_VIEWPORT_STRETCH`, `original` = `AURORA_VIEWPORT_FIT`, and the
  scene fallback switches between those two policies.

## 7. Phase 2: HUD anchored to the screen edges

1. `include/P2DScreen.h:54` (`Mgr_tuning`): add `#if TARGET_PC s8 mWideAnchor; #endif`, initialised
   to 0 in the constructor.
2. `src/plugProjectOgawaU/ogBloGroup.cpp:46` (`addBlo`): `PC_ONLY(screen->mWideAnchor = p2_wide_anchor_for_blo(screenName);)`.
3. `src/sysGCU/P2DScreen.cpp:112`: `setOffset(mTranslationX PC_ONLY(+ p2_wide_hud_shift(mWideAnchor)), mTranslationY);`
4. `p2_wide_hud_shift(a) = a * 304 * (kHud - 1)`, with `kHud = min(k, hudMaxAspect / A0)`; 0 unless
   `hudLayout == edges`. At 16:9 this is 94.2 units; with the default 16:9 limit the HUD stops
   moving outward on wider screens.
5. Anchor table in `port/sdk/wide.cpp`, keyed by file name:

| Screen | Anchor | Confidence |
|---|---|---|
| `orima.blo` (both captains' life gauges) | left | high |
| `doping.blo` (sprays) | left | high |
| `gr_pikmin.blo`, `cave_pikmin.blo` (Pikmin counters) | right | high |
| `sensor.blo` (treasure gauge) | right | check on screenshot |
| `sun_meter.blo` (sun meter; the Day bubble is in `gr_pikmin.blo`) | spread (2) | its panes spread from the track centre by the edge shift (`CallBack_SunMeter::wideSpread`), so the track runs from the captains to the Day bubble |
| `grand_cave_poko.blo`, `2P_challenge_poko.blo` | centre until checked | check on screenshot |
| `challenge_1P.blo`, `challenge_2P.blo` | centre | they hold both sides of a split-screen HUD |

   Unknown names get centre. A debug env var `MPPM_HUD_ANCHORS=name:L,name:R` overrides the table so
   the values can be tuned from screenshots without rebuilding.
6. Whether the offset is applied before or after the 0.95 tuning scale (`P2DScreen.cpp:84-87`) decides
   if the shift needs dividing by 0.95. Measure with a 16:9 screenshot: the life gauge must sit as
   far from the left edge as it does at Original.
7. Effects that read pane positions (treasure gauge sparkle, `ogObjGround.cpp:132-135`) follow the
   panes; verify once.
8. Flip the default of `interface.hudLayout` to `edges`.

Implemented by W8b with one change to the plan: a shifted screen lies partly outside the centre
608 units, which the remapped viewport clips. Anchored screens (and `TParticle2dMgr::draw`, so HUD
2D effects follow) therefore draw inside a *wide canvas* scope (`p2_wide_canvas_begin/end`): the
port's unmapped viewport with the projection's X squeezed by `1/k` (`p2_wide_set_projection`,
used by `J2DOrthoGraph`/`J2DPerspGraph::setPort`). Same placement, no clipping. The offset is
post-scale (`J2DPane::makeMatrix`), so no 0.95 correction. `grand_cave_poko.blo` stays centred
like the Challenge Poko counter; the story cave HUD was not reached in testing.

## 8. Phase 3: polish

- Piklopedia / Treasure Hoard wide: map the 3D window rectangle with `p2_wide_map_x`
  (`singleGS_Zukan.cpp:1329-1362`), raw scopes around its captures, then drop the scene fallback.
- Menus that should fill the width: pause-menu dim, world-map and results backgrounds, title screen.
- Sun meter stretched across the top (scale the track pane, keep the end caps).
- HUD scale 50-150% keeping each screen's anchor corner fixed (Dusklight's `dAnchorHudScale` idea).
- Safe-area insets from `SDL_GetWindowSafeArea` for notched or rounded displays.
- Vertical split for 2-player on wide screens. `VerticalSplitter` is a stub in the decomp
  (`graphics.cpp:58-78`), so this is new host code.
- Sub-pixel scissors through `GXSetScissorRender` if integer logical pixels show seams.

### 8.1 Implemented (Q4)

- **Full-width backdrop pictures** (`p2_wide_backdrop_begin/end`, called from
  `J2DPicture::drawTexCoord` and `J2DPictureEx::drawTexCoord`). A picture quad that covers the
  608-wide 2D port without rotation, to within 3% of each edge (tested in NDC through the graph's
  own ortho or perspective projection, so it works for both J2D graphs; the file select's grids
  stop at NDC -0.987 / -0.994 and needed the margin), is drawn in a wide canvas:
  - texture repeating (`GX_REPEAT` / `GX_MIRROR`) or at most 64 texels wide (the stretched
    gradients and bands menus are built from): the quad is extended to the window edges with its
    S coordinates extrapolated, so the centre stays pixel-identical and the sides continue the
    texture (file select band and grid, Piklopedia / Treasure Hoard backdrops, world-map grid);
  - other clamped pictures keep their authored size and only show what they already draw past
    the frame (the world map's area image and the 2-P Battle backdrop are authored wider than
    608 units), so nothing is smeared.
  This also widens the pause-menu dim and every other full-screen 2D fill. HUD screens in an
  edge-anchored canvas are left alone.
- **Day-end take-off and results**: the scene fallback in `DayEndState` / `MainResultState` is
  gone. The only model that ended at the 4:3 edges is the demo actor `bg` (tree silhouettes,
  cloud layer, star sky; `JSTObjectActor.cpp`), whose screen-horizontal axis is its local Z:
  `ObjectActor::update` scales that axis by `k`, so the sky fills any window and the ground shots
  stay wide. Scaling X (or X and Z) moved its parts and was rejected after screenshots.
- Still on the scene fallback: none of the scenes above. The Piklopedia / Treasure Hoard state
  (`singleGS_Zukan.cpp`) keeps its lock for the captured 3D window.

## 9. Aurora patch

`port/patches/aurora/0002-vi-lock-aspect-ratio.patch` (applied by `CMakeLists.txt:38-50`). About 30 lines.

- `A:lib/window.cpp`: `float g_lockedAspect = 0.f;` and `set_frame_buffer_locked_aspect(float)`,
  which calls `request_frame_buffer_resize()` on change.
- `A:lib/window.cpp:454-472` (`get_window_size`): `target = g_lockedAspect > 0 ? g_lockedAspect : native_w / native_h`.
  With a scale: `scale_frame_buffer_to_aspect(base, scale, target)`. Without: if locked,
  `fit_frame_buffer_to_aspect(native_w, native_h, g_lockedAspect)`.
- `A:lib/window.hpp:52`: declaration.
- `A:lib/dolphin/vi/vi.cpp:54`: define `VILockAspectRatio(w, h)` and `VIUnlockAspectRatio()`.
- No GX change: under `STRETCH` the logical rectangle already maps onto whatever the content
  framebuffer is, and presentation already centres it (`A:lib/webgpu/gpu.cpp:337-363`).

Check the upstream Aurora history first; if a newer commit defines these functions, bump the
submodule instead of carrying the patch.

## 10. Testing

Runs are headless under Xvfb on display :99, with the existing `play.sh` key scripts and
`magick import` screenshots.

- Window shapes: Xvfb screens `1216x896` (19:14), `1280x800`, `1280x720`, `1720x720`, `2560x720`, and
  `800x900` (narrower than 19:14). The window size comes from `AuroraConfig.windowWidth/Height`, which
  `preinit` fills from `video.windowWidth` / `video.windowHeight` (SETTINGS.md 3); `MPPM_WINDOW=WxH`
  is the environment override for those two keys (SETTINGS.md 1.3).
- Settings are forced per run through the config key overrides (`video.aspectRatio`,
  `video.internalResolution`, `interface.hudLayout`).
- Geometry check: a debug env var `MPPM_WIDE_TEST=1` draws, after 2D, a 3D-projected circle and a 2D
  circle of known size at screen centre. Both must measure round (width = height within 1 px) in the
  screenshot at every shape. This catches a wrong `k` in either path.
- Scene list, each at Original, 16:9 and 21:9, internal resolution Auto and 1x:
  boot logos; opening THP movie; title; file select; world map; day start cutscene; ground gameplay
  with HUD, water and shadows; pause menu (all three pages, including the map); treasure collect
  zoom; day-end results; cave entry and floor title; cave gameplay with radar; cave results;
  game over; Piklopedia and Treasure Hoard; Challenge select and results; 2-player VS select, split
  screen and results; hi-scores; options; memory-card messages; fades and wipes between all of them.
- Per scene: no stretched 3D, no 2D outside its area, side strips are either live 3D or black,
  fades cover the full width, shadows and water refraction line up, no pop-in at the left and right
  edges while rotating the camera.
- Resize test: script `xdotool windowsize` during gameplay and during a fade; no crash, the next
  frame is correct.
- Internal resolution: 1x, 2x, 4x at 16:9; compare shadow mask, radar, line widths and text.
- Regression: Original at 1216x896 against a screenshot from before the change. Phase 1 must be
  pixel-identical there, since `k = 1` makes every edit a no-op.
- Matching build: the MWCC build must still match byte for byte (all edits are
  host-only).

## 11. Room for mods and online play

- Mods read screen geometry only through `p2_wide.h`; new HUD screens register an anchor by `.blo`
  name in the same table.
- **Netplay risk:** frustum visibility feeds simulation (creature LOD and off-screen behaviour), and
  the frustum now depends on each player's aspect ratio. For deterministic online play the
  simulation must use one agreed frustum (fixed aspect, for example 16:9) while rendering uses the
  local one. That needs a second aspect value on `CullFrustum` for simulation queries; it is not part
  of this work, but `p2_wide_k()` must stay the only place the render aspect enters the game so the
  split is easy later.

## 12. Open questions

1. Default for `interface.hudMaxAspect`: stop the HUD at 16:9 (proposed) or follow the screen edge at 21:9?
2. Should `stretch` be offered at all? It is free to support, but it is never the better picture.
3. ~~Should the in-game overlay use the whole window?~~ Done: Aurora patch 0007 (section 5).
4. Hor+ shows more of the world at wide aspects, which also keeps more creatures in their "on
   screen" state. Accept as is for single player, or use the simulation frustum from section 11
   from the start?
5. Carry patch 0002, or wait for / request `VILockAspectRatio` upstream?
6. Anchors for `sensor.blo` and the cave Poko counter need a screenshot to settle.
7. Cap for internal resolution: 8x (3584 lines) proposed; Dusklight allows 12x.
