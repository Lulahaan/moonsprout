# mppm — Pikmin 2 native port plan

Goal: a native build of Pikmin 2 (USA, `GPVE01`) for desktop (macOS, Windows, Linux) and later mobile (iOS, Android). It should be playable with a controller or with keyboard and mouse, and support local and online multiplayer.

Base: the [projectPiki/pikmin2](https://github.com/projectPiki/pikmin2) decompilation (100% matched code, CC0). The full upstream history is kept, with the `upstream` remote pointing to that repo.

## Ground rules

- **No game assets in git, ever.** The player supplies their own disc image (`.iso`/`.gcm`/`.rvz`). The port verifies it as `GPVE01` and extracts it at first run. Disc images and extracted data are gitignored.
- **Keep the upstream diff small.** Port changes go behind `#if TARGET_PC`, or in a separate `port/` layer that replaces the Dolphin SDK. The original matching build (`configure.py` + ninja + MWCC) must keep working, so we can keep merging upstream.
- **Watch licenses.** Aurora, SDL3, Dawn and nod are permissive. Dolphin is GPL-2.0+: we can study it as a reference, but copying its code would make this project GPL. Default: clean reimplementation.

## Architecture

```
src/, include/          upstream decomp (game + JSystem + MSL), with minimal #if TARGET_PC patches
port/
  sdk/                  host versions of Dolphin SDK pieces: OS (threads, alarms, time, arena), DVD, CARD (saves -> files), AI/DSP shims
  audio/                software mixer that replaces the JAudio2 DSP ucode
  input/                SDL3 gamepad + keyboard/mouse -> PAD, plus mouse-pointer aiming
  net/                  lockstep netcode, lobby client
  assets/               disc extraction (nod) + load-time endian/pointer fixups
  main.cpp              host entry point, frame loop
extern/aurora           GX/VI/PAD over WebGPU (Dawn) + SDL3; Metal on Apple, Vulkan/D3D12 elsewhere
CMakeLists.txt          native build (the MWCC matching build stays as it is)
```

### Main technical problems

1. **Compiler/ABI differences (MWCC/PPC32 -> Clang/x86-64/arm64).** Handle inline `asm` (~25 files), paired-single math (PSMTX/PSVEC: switch to C versions), `__declspec(section)`, PPC intrinsics (`__fabs`, `__frsqrte`, `__cntlzw`), and MWCC-only C++ quirks.
2. **64-bit pointers.** Loaders relocate offsets into pointers in place inside 32-bit fields (J3D models, JKRArchive, J2D screens, JMessage BMG, particles). Each loader needs a host-side relocation step. Turn on `-Wpointer-to-int-cast` and `-Wshorten-64-to-32` to find the rest.
3. **Big-endian data.** Every binary resource format is big-endian. Byte-swap per format at load time, in the same pass as pointer relocation. JSystem is shared with other Nintendo GameCube decomps, so look for existing JSystem PC-port fixes we can reuse.
4. **Graphics.** Aurora provides the GX API that `J3D`/`J2D` call. Gaps to fill: GX features Pikmin 2 uses that Aurora doesn't cover yet, EFB copies (fog/blur/"pikmin light" effects), and widescreen/high-res.
5. **Audio.** JAudio2 does its synthesis on the DSP through custom ucode. Reimplement the DSP voice mixer (ADPCM decode, resampling, envelopes, reverb) in software behind the `JASDsp` interface.
6. **Determinism (for online play).** Build game code with `-ffp-contract=off` and no fast-math. Route math through the decomp's own MSL/JMath functions and tables instead of the host libm, so x86 and ARM produce bit-identical results.

## Milestones

| # | Milestone | Done when |
|---|-----------|-----------|
| M0 | Repo + plan | The repository holds the decomp and this plan ✅ |
| M1 | Host compile | CMake builds all of `src/` for macOS arm64 with SDK stubs, and it links. CI compile check runs on macOS (Linux experimental; Windows later) ✅ |
| M2 | Boots | Disc extraction works, archives and models load (endian + pointer fixups), Aurora renders the Nintendo logo and title screen |
| M3 | Playable single-player | Input, audio, saves (memory card -> file). Story is completable on macOS |
| M4 | Desktop parity + local MP | Frame rate option with interpolation, Windows/Linux builds, controller hotplug, keyboard/mouse with pointer aim, remapping, settings UI (ImGui), widescreen. Local 2P Challenge/Battle with two controllers or keyboard/mouse + controller |
| M5 | Online 2P | Delay-based lockstep input sync for 2P Challenge/Battle, lobby with join codes, periodic desync checksum (every 30 ticks). Extended to 2+ players and new modes in [ONLINE_MULTIPLAYER.md](ONLINE_MULTIPLAYER.md) |
| M6 | Co-op story | New mode: Player 2 controls Louie in the story, reusing the 2P split-screen/camera code. Works locally and online |
| M7 | Mobile | iOS (Metal), then Android (Vulkan). Touch controls + controller support |
| Stretch | | Rollback netcode (snapshot JKRHeap arenas + game globals), texture packs |

### Frame rate option

Settings → Video → **Frame Rate** (`video.frameRate`): `Original` (default) / `Match Display` / `60` / `90` / `120` / `144` / `165` / `240` / `Unlimited`, next to the VSync toggle. A second key, `video.smoothMotion`, turns on in-between pictures for the game itself. Reference: `docs/launcher/SETTINGS.md` 3.

The game paces itself by video retrace: `System::setFrameRate(n)` waits `n` VI retraces per frame (gameplay uses 2, so 30 fps). It sets `mDeltaTime = n / 60`, but much of the logic counts frames instead of using that delta. Just speeding up the loop would make the game run fast, so instead:

- **Logic stays on a fixed tick.** Game logic advances at the original rate (30 Hz in gameplay, 60 in menus). The host's 59.94 Hz retrace timer (`port/sdk/os_thread.cpp`) is the accumulator: ticks are counted against its grid, never against presented frames. This also keeps lockstep netcode deterministic.
- **Rendering is decoupled.** A frame policy (`port/launcher/frame_policy.cpp`) turns the setting, VSync and the display's refresh rate into a present cap; a cap below the tick rate skips presents and never slows ticks. Between ticks the renderer draws extra pictures with interpolated transforms (Smooth Motion).
- **Rollout, three phases:**
  1. The limiter, present skipping, the setting, and the start-menu scene running at the chosen rate (it is port-owned and time-based). The field still shows 30.
  2. Smooth Motion for J3D models and the camera in field sections, as an Experimental row.
  3. Direct-draw 3D, particles, HUD and cutscenes; then Smooth Motion becomes a normal row, default on.

### Default keyboard/mouse layout (M4)

The mouse moves the throw cursor directly, like pointer aiming, instead of emulating the stick.

| Action | Input |
|--------|-------|
| Move captain | WASD |
| Throw (A) | Left click |
| Whistle (B) | Right click (hold to grow) |
| Dismiss (X) | Space |
| Swap captain (Y) | Tab |
| Swarm (C-stick) | Middle mouse + drag, or arrow keys |
| Camera (L/R/Z) | Q, E, R |
| Pause / menu | Esc |

### Online design (M5–M6)

- **Model:** deterministic lockstep. Peers exchange only controller input per gameplay tick (30 Hz, 33.4 ms). Each sender picks its own input delay of 1–8 ticks from its measured one-way delay, and each tick of delay buys 33 ms of network slack. Details in [ONLINE_MULTIPLAYER.md](ONLINE_MULTIPLAYER.md) §2.2 and §2.10.
- **Transport:** ENet over UDP with NAT hole punching (libjuice) and a TURN relay fallback, plus a small self-hostable lobby server, `mppm-server` (ONLINE_MULTIPLAYER.md §2.11).
- **Sync safety:** same build hash and RNG seed at session start, and a checksum of selected sim state every 30 ticks to detect desyncs (ONLINE_MULTIPLAYER.md §2.7).

## Immediate next steps

M1 is done. See [PORT_STATUS.md](PORT_STATUS.md) for the M2 work list.

### Original M1 steps (done)

1. Add `extern/aurora` (submodule) and a top-level `CMakeLists.txt` that compiles `src/` with Clang and `TARGET_PC=1`.
2. Write `port/sdk` stubs until the build links: the OS arena backed by `malloc`, DVD backed by the host filesystem.
3. Add the extraction tool (nod-rs `nodtool`, or nod linked in directly), with a SHA-1 check against the known `GPVE01` hash in `sha1/`.
4. Iterate on compile errors module by module: `Dolphin` -> `JSystem` -> `sysCommonU`/`sysGCU` -> `plugProject*`.

## Fidelity notes

Places where the host build is knowingly not bit-exact with the GameCube. Revisit when polishing.

- **`frsqrte`/`fres` estimates.** The hardware gives about 12-bit estimates, which the game's approximate `sqrtf` (`include/sqrt.h`) and `dolsqrtf` refine. Host builds use exact IEEE results. They stay deterministic across platforms (what netplay needs), but distances differ from the original by tiny amounts. If that ever visibly matters, emulate the documented hardware estimate tables.

- **`RETURN_GARBAGE(v)`.** A few original functions `return;` without a value, and MWCC returns whatever is in `r3`. Host builds return `v` (0/false). Check these against the disassembly if any of them behaves oddly.

## Open questions

- ~~Frame rate~~: decided. Original by default, with a Frame rate option up to uncapped (see above).
- ~~Lobby/relay server~~: decided. `mppm-server` on Fly.io for the lobby, Cloudflare Realtime TURN as the relay, self-hostable with Docker and a custom server URL (see [ONLINE_MULTIPLAYER.md](ONLINE_MULTIPLAYER.md) §2.11).
- Mobile distribution: personal sideload/TestFlight only? App stores are not realistic for this project.
