# Launcher: index and implementation plan

Four specs describe the launcher. This file says which spec owns what, records the decisions that
cut across them, and splits the work into packages that separate coding agents can take.

Paths are relative to the repo root. Nothing here is implemented yet.

## 1. The specs

| File | Covers | Read it for |
|---|---|---|
| [DESIGN.md](DESIGN.md) | Name and logo, colour and type tokens, motion, every UI component, the start-menu layout, the pre-disc backdrop, the UI side of the launch animation, the in-game bar, accessibility | How anything looks or moves |
| [SETTINGS.md](SETTINGS.md) | Every setting key, its default, range, help text and hook; overrides; the bridge struct to game code; controller window; save manager; restart flow; first-run preset; in-game menu entries; netplay classes | What a setting is called and what it does |
| [SCENE.md](SCENE.md) | The live 3D scene behind the start menu: game section, assets from the disc, characters and behaviour, camera, light, audio, take-off, the C bridge to the launcher | Anything drawn by the game engine before the title screen |
| [ASPECT.md](ASPECT.md) | Widescreen and internal resolution: the `k` model, Aurora state, game-code edits, HUD anchoring, the Aurora patch | Window shapes and resolution |

When two specs disagree, this order decides: **SETTINGS.md** for key names, defaults, ranges and
menu entries; **DESIGN.md** for visuals, copy style and UI timing that does not depend on the game;
**SCENE.md** for anything timed by animation data; **ASPECT.md** for Aurora viewport state.

## 2. Decisions made in the review

Each changed spec lists its own edits under "Review notes" at its top.

| # | Conflict | Decision |
|---|---|---|
| 1 | Aspect key: `video.aspect` (SETTINGS) against `video.aspectRatio` (ASPECT), with different values and defaults | `video.aspectRatio`, values `auto`, `original`, `16:10`, `16:9`, `21:9`, `stretch`. Ships as `original` with only `original` and `stretch` listed; the ASPECT phase 1 change flips the default to `auto` and lists the rest |
| 2 | Internal resolution described as multiples of 640x480 (SETTINGS) and of 448 lines (ASPECT) | 448 lines, width from the aspect |
| 3 | Reduce motion: `interface.reducedMotion` / class `reduced-motion` (SETTINGS) against `interface.reduceMotion` / `reduce-motion` (DESIGN, SCENE) | `interface.reduceMotion`, class `reduce-motion` |
| 4 | Keys used by DESIGN, SCENE and ASPECT but missing from SETTINGS | Added to SETTINGS 9.1: `interface.hudLayout`, `interface.hudMaxAspect`, `interface.highContrast`, `interface.glassEffects`, `interface.launchAnimation`, `interface.sceneArea`, `interface.sceneTime`, `interface.sceneMusic`. `prelaunch.launchAnimation` (DESIGN) no longer exists |
| 5 | Launch timing: DESIGN planned 4.45 s with a hatch; the disc animation gives 6.35 s with a tractor beam | SCENE's times win. DESIGN 7.1 to 7.3 were re-timed and are cue-driven. Full is about 7.1 s to the title, Short about 4.9 s |
| 6 | The UI needed cues and the ship's screen position; the bridge only had coarse states | SCENE 4.2 gained `p2_launcher_scene_cue`, `p2_launcher_scene_ship_screen` and `P2SceneConfig.fadeColor` |
| 7 | Two end fades in two colours (scene: white-blue, UI: warm white) | One colour, owned by DESIGN (`#FFF6E0`; navy `#050B1A` for Off and reduce motion). The UI flash is on top; the scene fades to the same colour underneath as a safety net |
| 8 | Composition: DESIGN put the pilot's head at (0.60, 0.50) and the ship at x 0.74; the camera in SCENE gives (0.59, 0.36) and 0.61 | DESIGN 6.2 is the contract, with numbers the camera can meet. SCENE tunes at 16:9 first |
| 9 | With aspect Original on a wide window the UI is confined to a pillarbox; DESIGN assumed it could draw in the pillars | While the start menu is open the aspect is forced to follow the window (ASPECT 5). The live scene is correct at any shape |
| 10 | `port/sdk/wide.cpp` was to read settings, against "game side never sees the config library" | The launcher pushes values in with `p2_wide_configure` |
| 11 | "Live Start Menu" off: SCENE kept the game booted behind the menu, SETTINGS said it boots on Play | Boots on Play. The menu then runs standalone over the backdrop |
| 12 | A failed scene ended its section, which would start the title and its music under the backdrop | The section idles empty until Play |
| 13 | In-game bar entries: DESIGN (Resume, Settings, Tools, Reset, Quit with a popover) against SETTINGS (registry with Controllers, Start Menu, reserved Mods and Online) | SETTINGS 13. DESIGN 8.1 styles that list |
| 14 | Menu Size range: 50-200 step 10 against 75-200 step 5 plus Automatic | DESIGN 9.3; since 2026-11-22 the stepper stops at 130 (the layout caps at 133%), pending sign-off |
| 15 | Boot-logo skip was specified twice (SCENE hook H3, SETTINGS `game.skipBootLogos`) | One edit in `bootSection.cpp`, one condition |
| 16 | `gP2Config.launcherOpen` title gate (SETTINGS) against "not needed" (SCENE) | Not built in v1; the title never runs behind the menu |

Not edited, but to know:

- **Product name.** DESIGN proposes "Sproutlight"; SETTINGS writes "mppm" in all its copy. Package W0
  adds one constant (`port/launcher/branding.hpp`: name, version string, releases URL) and no string
  in code spells the name. Until the name is chosen the constant says `mppm`.
- **Copy.** Where both DESIGN and SETTINGS word the same message (restart banner, disc formats),
  use SETTINGS' sentence in DESIGN's style rules (sentence case, verbs on buttons).
- **Labels.** The start-menu item is "Multiplayer"; the in-game bar entry is "Online". Both open the
  same future feature. Pick one word when it is built.

## 3. Shared interfaces

These are the only places where packages touch each other. W0 creates all of them, as stubs where
the real code belongs to a later package.

| Interface | File | Defined in | Stub behaviour in W0 |
|---|---|---|---|
| Settings vars and `settings_meta()` | `port/launcher/settings.{hpp,cpp}` | SETTINGS 1 to 9 | complete: every key of every spec |
| `gP2Config` | `port/include/p2_config.h`, `port/sdk/os_report.cpp` | SETTINGS 1.4 | complete |
| Video state: `mppm::video::apply()` | `port/launcher/video.{hpp,cpp}` | ASPECT 5, SETTINGS 3 | fullscreen, vsync, resampler, frame-buffer scale, `FIT` / `STRETCH` |
| Wide state: `p2_wide_*` | `port/include/p2_wide.h`, `port/sdk/wide.cpp` | ASPECT 5 | `k = 1`, every map is the identity |
| Scene bridge: `p2_launcher_scene_*` | `port/include/p2_host.h`, `port/launcher/scene_bridge.cpp` | SCENE 4.2 | `pending()` returns 0 |
| Scene state for the UI: `launcher::scene::state()`, `on_cue(fn)`, `ship_screen()`, `send(Command)` | `port/launcher/scene_bridge.hpp` | this file | reports "no scene" |
| Menu registries: start-menu items and in-game bar entries, each `{id, label, order, visible(), open()}` | `port/launcher/menu_registry.{hpp,cpp}` | SETTINGS 13, DESIGN 5.3 | complete |
| Body classes: `reduce-motion`, `high-contrast`, `glass-off` / `glass-full`, `scene-ready`, `launching` | set in `port/launcher/ui/document.cpp` from the vars | DESIGN 6.9, 7.4, 9 | complete for the three setting classes |
| Restart: `launcher::request_restart(target)`, `settings_restart_pending()` | `port/launcher/restart.{hpp,cpp}` | SETTINGS 11 | declared; implemented in W4 |
| Start-menu applier: `live::preinit/init/update/restore_ui/on_game_booted`, `relaunching()`, `pending_relaunch()`, `busy(var)`; `MenuApply` / `menu_action()` | `port/launcher/live_apply.{hpp,cpp}`, `settings.{hpp,cpp}` | SETTINGS 1.2, 11; live_options_design 3.5, 3.8 | complete (package LA): changes on the start menu apply at once or through a short relaunch back to the same Settings row |
| Product name | `port/launcher/branding.hpp` | section 2 | complete |

Boot flow, as `port/main.cpp` must end up (SCENE 4.3, SETTINGS 9.1, DESIGN 6.9):

| Start condition | Flow |
|---|---|
| Skip Start Menu (`MPPM_SKIP_LAUNCHER=1`) or `--play`, with a disc known | Boot the game; no menu, no scene; logos per `game.skipBootLogos` |
| Disc on the command line or `MPPM_DISC` | The start menu with that disc (the user's choice, 2026-11-21: the start menu always comes first unless Skip Start Menu is on) |
| No disc known | Standalone menu over the backdrop. Picking a disc: with Live Start Menu on, return and boot with the menu kept (document recreated with `intro-skip`); with it off, stay until Play |
| Disc known, Live Start Menu on | Boot behind the opaque backdrop; scene section; backdrop fades at `P2_SCENE_READY`; Play runs the take-off; title |
| Disc known, Live Start Menu off | Standalone menu; Play boots the game straight to the title |

On every start menu a change in Settings takes effect as it is chosen (live_options_design): the
scene rows, Texture Filtering and MSAA at once; the disc, Save Format and Enable Audio before the
boot just wait for it; the graphics backend, Allow Software Rendering and, once the game has booted
behind the menu, the boot-time keys relaunch the app for a moment behind a navy veil and come back
on the same Settings row (`MPPM_RESUME`, SETTINGS 11).

## 4. Work packages

W0 goes first and alone, because it owns every edit to files that more than one spec touches.
After it, W1 to W10 have disjoint files and can run in parallel, except where "after" says
otherwise. W2 needs nothing and can start at once.

Rules for every package:

- Game-source edits are `PC_ONLY(...)`, `PC_SWITCH(...)` or `#if TARGET_PC` only. The MWCC matching
  build must stay byte-identical; check it before handing in.
- Build and run natively (`build-pc/mppm`), headless under Xvfb, never on the desktop.
- Do not edit a file another package owns. If one is needed, ask for the interface in section 3 to
  grow instead.
- No Nintendo data in the repo. Third-party assets need a licence file next to them.

### W0. Skeleton and settings model (serial, first)

- **Goal:** every key, bridge and call site exists, so later packages only fill in their own files.
- **Spec:** SETTINGS 1, 15 steps 1 and 2; section 3 of this file.
- **Owns:** `port/launcher/settings.{hpp,cpp}`, `launcher.cpp`, `include/mppm/launcher.hpp`,
  `video.{hpp,cpp}`, `menu_registry.{hpp,cpp}`, `branding.hpp`, `restart.hpp`,
  `scene_bridge.{hpp,cpp}` (stub), `ui/document.cpp` (body classes); `port/main.cpp`;
  `port/include/p2_host.h`, `p2_config.h`, `p2_wide.h`; `port/sdk/wide.cpp` (stub),
  `port/sdk/os_report.cpp`; the environment-variable call sites of SETTINGS 1.3
  (`src/sysGCU/system.cpp:129`, `src/JSystem/JUtility/JUTException.cpp:166`,
  `src/plugProjectKandoU/gamePlayCommonData.cpp:188`, `singleGameSection.cpp:1048`,
  `src/plugProjectYamashitaU/timeMgr.cpp:167`, `port/audio/audio.cpp`, `port/sdk/os_thread.cpp`).
- **Provides:** everything in section 3. `port/main.cpp` already calls `p2_wide_update()`,
  `mppm::video::apply()` and follows the boot-flow table.
- **Done when:** all keys of the four specs load, save and survive a restart; `--cvar` and every
  `MPPM_*` variable override as in SETTINGS 1.3; `AuroraConfig` is filled from settings;
  `p2_env_flag` / `p2_env_float` are gone; the existing headless scripts pass unchanged; the matching
  build is identical.

### W1. Theme and components

- **Goal:** the design system on every existing control.
- **Spec:** DESIGN 3, 4, 5, 9.
- **Owns:** `port/launcher/res/rml/{theme,global,controls,tabbing,window,popover,overlay,popup}.rcss`,
  `res/fonts/`, `ui/ui.cpp` (font list, automatic scale), `ui/bool_button.cpp`,
  `ui/number_button.cpp`, `ui/tab_bar.cpp`, `ui/select_button.cpp`.
- **Uses:** body classes from W0. **Provides:** the element and class names of DESIGN 5 and 8, which
  W3 to W7 build their DOM from.
- **Done when:** a test window with one of each control matches DESIGN 5 at 100% and 200% scale;
  reduce motion and high contrast work; Fira Sans is removed; DESIGN open question 4 (RCSS details)
  is answered in the spec; the contrast pairs of DESIGN 3.3 hold on screenshots.

### W2. Art

- **Goal:** the images the UI needs, all original.
- **Spec:** DESIGN 1.3, 1.4, 6.4.
- **Owns:** `tools/launcher_art/`, `port/launcher/art/`, `port/launcher/res/img/`.
- **Provides:** `stars-far.png`, `stars-near.png`, `ship.png`, icon PNGs; `wordmark.png` and
  `wordmark-leaf.png` once the name is chosen.
- **Done when:** the generator script reproduces its outputs from a fixed seed; the star tiles are
  seamless; the ship follows the "must not" list in DESIGN 6.4.

### W3. Start menu

- **Goal:** the start menu, the pre-disc backdrop, the update notice and the UI half of the launch.
- **Spec:** DESIGN 5.3, 6, 7; SETTINGS 9.1 (Check for Updates).
- **Owns:** `port/launcher/prelaunch.{cpp,hpp}`, `res/rml/prelaunch.rcss`,
  `update_check.{hpp,cpp}`.
- **Uses:** `scene_bridge.hpp` (state, cues, ship position, commands), the start-menu registry,
  `branding.hpp`, images from W2 (live-text wordmark and shipless backdrop until they exist).
- **Done when:** every layout row of DESIGN 6.6 is correct in screenshots at 1920x1080, 1280x720,
  1280x800, 1024x768, 2560x1080, 3840x1080 and at 200% scale; with the stub bridge, Play runs the
  whole timeline on the fallback timers and reaches the title; skip works; the update pill shows
  all four states against a mocked result and stays quiet when GitHub returns 404.

### W4. Settings window, restart, first run

- **Goal:** all eight tabs with every v1 row, and the flows around them.
- **Spec:** SETTINGS 1.5, 2 to 7, 9, 11, 12.
- **Owns:** `port/launcher/ui/config_bind.hpp`, `settings_window.{cpp,hpp}`, `restart.cpp`,
  `preset.{cpp,hpp}`.
- **Uses:** vars and meta from W0, `video::apply()`. **Provides:** `SettingsWindow(prelaunch)` for
  W3 and W5; `request_restart`.
- **Done when:** every v1 row edits its var and shows lock, restart and experimental badges;
  runtime video and audio rows take effect at once; restart-only changes raise the banner and
  "Restart now" comes back with the change applied; the preset screen appears once.

### W5. In-game menu and overlay

- **Goal:** the F1 bar, its scrim, and the passive overlay (FPS, cheats badge, shader spinner,
  toasts, menu hint).
- **Spec:** SETTINGS 13; DESIGN 5.14, 8.
- **Owns:** `port/launcher/menu_bar.{cpp,hpp}`, `overlay.{cpp,hpp}`.
- **Uses:** the bar registry, `SettingsWindow` (W4), `ControllerConfigWindow` (W6), `request_restart`.
- **Done when:** F1, Back and R+Start open and close it in gameplay; game input is blocked while it
  is open; Start Menu and Quit work with their confirmations; a dummy entry registered from another
  file appears in order without editing the bar.

### W6. Controller window

- **Spec:** SETTINGS 8, 4.
- **Owns:** `port/launcher/controller_config.{cpp,hpp}`, `input_defaults.cpp`,
  `res/rml/controller.rcss`.
- **Done when:** device assignment, press-to-bind for buttons, sticks and keys, swap-on-conflict,
  dead zones, rumble test and Restore Defaults work, and bindings survive a restart.

### W7. Disc verification and save manager

- **Spec:** SETTINGS 2 (Verify Disc), 10.
- **Owns:** `port/launcher/disc.{cpp,hpp}`, `saves_window.{cpp,hpp}`.
- **Done when:** verification runs on a worker thread with progress and cancel, and its result is
  remembered by fingerprint; import, export, backup, restore and delete work for both save formats,
  each destructive action taking an automatic backup first.

### W8. Widescreen (two steps, in order)

- **Goal:** correct picture at any window shape and internal resolution.
- **Spec:** ASPECT.
- **Owns:** `port/sdk/wide.cpp`, `port/include/p2_wide.h`, the aspect part of
  `port/launcher/video.cpp` (handed over by W0), `port/patches/aurora/0002-*.patch`, and the game
  files of ASPECT 6 and 7 (`src/sysGCU/graphics.cpp`, `src/JSystem/J2D/J2DGrafContext.cpp`,
  `src/sysGCU/P2DScreen.cpp`, `include/P2DScreen.h`, `src/sysGCU/THPDraw.c`, the Morimura and Ogawa
  2D files listed there).
- **W8a = phase 1:** 3D Hor+, 2D centred, scene fallback, start-menu override, default flipped to
  `auto`. **W8b = phase 2:** HUD anchoring, default `hudLayout = edges`.
- **Done when:** the test list of ASPECT 10 passes at Original, 16:9 and 21:9; Original at 1216x896
  is pixel-identical to before; the matching build is identical.

### W9. Live scene (three steps)

- **Goal:** the scene of SCENE.md.
- **Owns:** `port/game/scene/*` (new directory), `port/launcher/scene_bridge.cpp` (handed over by
  W0), hooks H1 to H3 (`src/sysGCU/gameflow.cpp`, `src/plugProjectHikinoU/PSGame.cpp`,
  `src/sysGCU/bootSection.cpp`, including the `game.skipBootLogos` condition).
- **W9a = phases 1 to 3:** section, characters, ship, map, light, camera, music. Its composition
  pass needs W8a and W3 on screen.
- **W9b = phase 4** (behaviour, in `SceneDirector.*`) and **W9c = phase 5** (take-off; put the
  script in its own `SceneTakeoff.cpp` so it does not collide with W9b) can then run in parallel.
  Phase 6 follows.
- **Done when:** headless `MPPM_FORCE_LAUNCHER=1 MPPM_SCENE_AUTOPLAY=3` reaches the title without
  asserts; the composition meets DESIGN 6.2 at 16:9 and holds at 19:14, 16:10 and 21:9; cues arrive
  at the times of SCENE 11 and the W3 timeline runs on them, not on fallbacks; a missing disc file
  gives `P2_SCENE_FAILED` and a working Play; all four behaviours the request names are visible
  (idle loop, Pikmin fidgets, Olimar reacting to them, take-off).

### W10. Game hooks for settings

- **Goal:** the v1.1 rows of SETTINGS, one commit each.
- **Spec:** SETTINGS 15 step 9.
- **Owns:** `src/sysGCU/commonSaveData.cpp`, `src/plugProjectEbisawaU/ebiScreenOption.cpp`,
  `src/sysGCU/titleSection.cpp`, `src/sysGCU/memoryCard.cpp`, `src/sysGCU/reset.cpp`,
  `src/plugProjectKandoU/{navi,gamePlayData,baseGameSection}.cpp`,
  `src/plugProjectNishimuraU/RumbleMgr.cpp`, and a new `port/launcher/game_options.cpp` for the host
  half of the options sync and pause-in-menu.
- **Done when:** each row does what its help text says; volume and rumble stay in step with the
  game's Options screen in both directions; Reset Game passes the 20-resets test or stays hidden;
  the matching build is identical.

### Order

```
W2 ──────────────────────────────────────────────►
W0 ─┬─ W1 ─────────────►
    ├─ W3 ─────────────►            (final timing check after W9c)
    ├─ W4 ─► W5 (needs SettingsWindow; W6 window optional at first)
    ├─ W6
    ├─ W7
    ├─ W8a ─► W8b
    ├─ W9a (composition after W8a + W3) ─┬─ W9b
    │                                    └─ W9c ─► phase 6
    └─ W10
```

A first release needs W0 to W5, W8a and W9a to W9c. W6, W7, W8b and W10 can follow.

## 5. Room for mods and online play

Nothing in the four specs blocks either. What is already reserved:

| Area | Where |
|---|---|
| Setting namespaces `mods.*` and `net.*`, per-key netplay class, the netplay overlay | SETTINGS 1.1, 14 |
| Start-menu and in-game bar entries through the registries | section 3; DESIGN 5.3, 8.1; SETTINGS 13 |
| Extra actors in the start-menu scene (second captain, lobby players) | SCENE 4.4 |
| HUD anchors for mod screens; one entry point for the render aspect | ASPECT 11 |
| Data mods (M5.2): the Mods window, its start-menu item and in-game bar entry, the pending restart, the version label | SETTINGS 17; `port/launcher/mods_window.cpp` |

Mods now exist (milestone M5.2): data mods in `<data>/mods/` replace or add disc files and files
inside archives, in the order of `<mods>/load_order.txt`. The launcher's part is the Mods window
(SETTINGS 17): editable on the start menu, read-only in game and during harness runs, and a
pending restart when the list changes after the game has booted. Enable states are kept in
`load_order.txt`, not in a `mods.*` setting (that namespace stays reserved).

Two things the netplay design must settle, both noted in the specs: the simulation must not depend
on each player's frustum (ASPECT 11; until then `video.aspectRatio` is host-synced), and the
pause-in-menu loop in `p2_host_present` must never run online (SETTINGS 6, class X).

## 6. Decisions from the user (2026-11-21)

| Question | Decision |
|---|---|
| Name | **Moonsprout** (window title, wordmark, release notes; repo and binary stay `mppm`). No collision found in a web search. Update `branding.hpp` and the DESIGN wordmark (sprout motif under alien moons). |
| Launch length | Superseded (2026-11-21, take-off rework): the take-off is **On** (the full take-off, about 19 s to the title, every launch) or **Off**. Any button skips it after its first 0.6 s, with a "Press any button to skip" hint. Stored `auto` and `short` read as `full`; `interface.fullLaunchSeen` is gone. Reduce Motion keeps the full timeline with still cameras |
| Default aspect / modern vs original | Copy Dusklight's first-run preset (`D:src/dusk/ui/preset.cpp`): on first start the player picks **Modern** (widescreen `auto`, HUD at edges, quality-of-life defaults) or **Original** (4:3, GameCube-faithful defaults). The preset sets `video.aspectRatio` and the other preset-owned keys (SETTINGS 12) |
| Sound before a disc exists | Silent; UI sounds come from the game once the disc is loaded |
| Other open questions below | Use the recommended defaults in the specs |

## 7. Open questions (answered by defaults unless the user says otherwise)

The specs list more; these are the ones that change what gets built first.

1. **Name.** "Sproutlight", another from DESIGN 1.1, or keep "mppm" as the product name? W2 cannot
   draw the wordmark until this is settled.
2. **Launch length.** Settled: the full take-off (about 19 s to the title) on every start, skippable with any
   button, or Off (section 6).
3. **End of the launch.** The warm-white flash clears onto the title screen's own fade-in from
   black. Acceptable, or should the title's fade be skipped on this path?
4. **Boot logos.** Starting through the menu skips the Nintendo and Dolby logos. Keep that?
5. **Default aspect.** `auto` (wide when the window is wide) once widescreen works, or stay on
   Original until the user opts in?
6. **In-game menu with Original on a wide window.** It is confined to the 4:3 picture. Accept, or
   patch Aurora so the UI uses the whole window?
7. **Volume and rumble ownership.** `config.json` wins over the memory card (SETTINGS 5.1). Confirm.
8. **Update check.** `owallzz/moonsprout` was private while this was written, so the check got a 404. It stays quiet until
   the repository or a releases mirror is public.
9. **Menu Size default.** 100% today. Switch to Automatic with the television-friendly rule of
   DESIGN 9.3?
10. **Sound before a disc exists.** The launcher is silent there. Add CC0 UI sounds, or leave it?
