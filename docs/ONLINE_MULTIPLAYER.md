# Online Multiplayer Plan

This plan covers online play for 2 or more players in mppm, and the new modes and content built on it. It extends M5 ("Online 2P") and M6 ("Co-op story") in [PORT_PLAN.md](PORT_PLAN.md).

How code is cited:
- Game files under `src/` and `include/` are cited by file name. Those names are unique in the repo, and the full path is given wherever it helps, e.g. `src/plugProjectNishimuraU/CameraMgr.cpp`.
- Files under `port/` and `extern/` are always cited with their full path.

The ground rules are the same as in PORT_PLAN.md:
- Port changes go behind `#if TARGET_PC` / `PC_ONLY`, or in `port/`.
- The MWCC matching build stays byte-identical.

## 1. Goals and non-goals

### Goals

- Online sessions with **2 or more players**. The launch target is up to 4 captains plus spectators, and at least one mode ships at 4 players on day one (§5). More than 4 seats comes later (§3.7).
- **New modes and content**, not only online 2P Battle. Most new modes are data (arenas, seeded caves, placements, cards, missions) plus a small C++ rules layer.
- A **data-mod overlay** that is the content pipeline for those modes. It works offline in local split screen before any netcode exists.
- Deterministic lockstep. It also gives replays, spectating and verified leaderboards at little extra cost.
- Each peer renders only its own captain, full screen.
- Same-binary, same-OS play first. Cross-OS play comes once the FP work in §4 is done.

### Non-goals (for now)

- Rollback netcode. §2.1 evaluates it and defers it.
- Joining as a player in the middle of a match. Players join at round, floor or day boundaries.
- A code-mod or scripting API before the first new modes ship (§7c).
- New enemy types, new map geometry or new models. Content reuses existing units, enemies and models.
- Cheat-proof hidden information. Under lockstep every peer holds the full state. Input spoofing for other players' slots is still in scope (§10).
- Matchmaking with strangers. Sessions are join-code and friend lobbies.
- Built-in voice chat. Players use Discord or similar. A quick-chat wheel and pings are in scope (§6).

## 2. Architecture

### 2.1 Netcode model

**Delay-based lockstep for N peers**, as PORT_PLAN.md:79-83 already plans:
- Peers exchange only controller input and host-stamped events (§2.9).
- Every peer simulates every captain.
- Each sender picks its own input delay, adapted to its measured path (§2.2).

**How ticks work today.** There is no logic tick decoupled from rendering:
- Each loop pass runs exactly one `update()` per `draw()` (`section.cpp:177-191`).
- Only the delta time is fixed: `mDeltaTime = mFrameRate/60` (`system.cpp:1190-1191`). Gameplay sets the frame rate to 2 (`baseGameSection.cpp:257`).
- Pacing comes from the retrace thread `timerMain` (`port/sdk/os_thread.cpp:276-343`; the retrace grid is stepped at `:306-318`, period `kRetracePeriod` at `:274`) and `waitForTick` (`JFWDisplay.cpp:465-487`).
- The FPS phase-1 hook `p2_host_tick_wait` (called at `JFWDisplay.cpp:479`, defined at `port/main.cpp:414`) sits inside `waitForTick` and decides when armed ticks become due.
- Phase 1 of the PORT_PLAN frame-rate work (PORT_PLAN.md:59-60) is implemented: the frame policy, present skipping (`aurora_end_frame_no_present`, `port/main.cpp:349-355`, inside `p2_host_present` at `:332-407`) and `p2_host_tick_wait`. Phase 2 stage 2a (Smooth Motion in the field and caves) is also in HEAD: in-between pictures are drawn from the HostFrames loop while waiting for the tick (`smooth_wanted`, `port/main.cpp:231`; loop `:448-521`). It re-runs `doDraw` with `gP2Present.active` set, which skips the camera step (`baseGameSection.cpp:386`, `:391`). `video.smoothMotion` is Forced off online (`port/launcher/settings.cpp:128`). Game-mode ticks still follow the original retrace rule (`port/main.cpp:522-536`), so the logic tick is still not decoupled from the retrace.

Lockstep counts **sim ticks**: one `beginFrame`…`endFrame` pass that runs gameplay `update()`. It does not count pad reads or presented frames. Later frame-rate phases must keep that rule.

**Headless tick runner.** Checksum CI, replay verification and late spectators all need to run ticks faster than real time without presenting. That is an M5.1a deliverable (§8). Under a `--headless-ticks` flag it makes every armed tick due at once in `p2_host_tick_wait` (the bypass point; no retrace wait), and it presents nothing (`aurora_end_frame_no_present`). Only the camera and LOD work must still run, because the simulation depends on them (§4 items 5–6).

**Rollback as a later upgrade.** The determinism audit found these obstacles to taking snapshots:

| Finding | Where |
|---|---|
| Floor state lives in one contiguous `JKRExpHeap` that takes all free space, mixed in with large static assets | `baseGameSection.cpp:2181`, `:1910`; 256 MiB arena (`kMem1Size`, `port/main.cpp:585`, passed as `config.mem1Size` at `:597`) |
| Absolute pointers plus ASLR mean a snapshot is valid only inside the same process | (determinism audit) |
| About 160 class statics, plus manager pointers, counters and function-local statics, live outside the heap | `gameSystem.cpp:19`, `naviMgr.cpp:12`, `generalEnemyMgr.cpp:83`, `port/sdk/msl_compat.c:15` |
| JAudio state points at game objects; `PSM::ObjMgr` (and `BossBgmFader::Mgr`) is created just before the floor heap, in the parent heap | `baseGameSection.cpp:2178-2183` |
| The DVD, memory-card and audio threads must be idle when a snapshot is taken | (determinism audit) |

At 30 Hz, rollback means copying tens of MB per tick, or tracking dirty pages.

**Verdict:** ship lockstep. Rollback stays in the PORT_PLAN "Stretch" row and is revisited only if latency is a measured problem.

Same-process heap snapshots for desync forensics and replay seeking are also **not free**. They need the same thread quiescence (DVD, memory card, audio) and handling of statics. Treat them as a post-launch tool. Desync reports at launch use the checksum and the input log (§2.7).

### 2.2 Transport, lobby, relay and latency

| Piece | Choice | Notes |
|---|---|---|
| Game transport | ENet via `FetchContent`, on a dedicated net thread | A small MIT C library with reliable and unreliable channels (PORT_PLAN.md:82). A plain `std::thread` owns the `ENetHost` and talks to the game thread through SPSC queues (ENet is not thread-safe). Each input packet is sent with `enet_host_flush` straight after queueing, and sent again half a tick later. Throttling is off on the input channel (`enet_peer_throttle_configure`). NAT traversal runs under ENet through libjuice (§2.11). Fallback with no new dependency: `borealis::net` UDP, which is vendored but switched off (`CMakeLists.txt:67`). |
| Topology | v1: star through the host. Later: hybrid, with a direct peer-to-peer fast path (§2.10) | The host collects slot inputs, stamps a confirmed bundle per tick and forwards it **as soon as the last slot for that tick arrives**, not on its own tick. Packets carry unacked inputs for the last 8 ticks. In the hybrid, peers also send inputs directly to each other where hole punching works; the host-forwarded stream stays the canonical copy, and each peer cross-checks the two (§10). |
| Lobby and join codes | `mppm-server` (§2.11), reached over `borealis::http` and `borealis::ws` | REST for create and join, WebSocket for signalling. `borealis::http` is built (default ON, `extern/borealis/CMakeLists.txt:13`) and reaches the launcher only transitively through `borealis::update` (`CMakeLists.txt:213-214`); `CMakeLists.txt:71` only selects the system curl provider. `borealis::ws` is forced off (`CMakeLists.txt:66`) and must be turned on. |
| NAT | ICE through libjuice: direct (host/srflx) first, TURN relay as fallback | The relay forwards ENet datagrams unchanged. Decided in §2.11: Cloudflare Realtime TURN officially, coturn or `mppm-server --relay` when self-hosted. |
| Time sync | Phase slew of the retrace grid, measured from tick timestamps | The period stays a `constexpr` (`port/sdk/os_thread.cpp:274`, duplicated as `kRetraceNs` at `port/main.cpp:57`). A new `std::atomic<int64_t> g_phaseSlewNs` is applied as a clamped step (±1 ms per retrace) where `nextRetrace` advances (`port/sdk/os_thread.cpp:313`). Details in §2.10. |

New code goes in `port/net/` (PORT_PLAN.md:21) and is built host-only, like `port/game/*`.

**Input delay.** Each sender chooses its own delay `d_i` and tags its input `T + d_i`. Receivers apply whatever tag says, so peers never have to agree on one delay.
- `d_i = max(1, ceil((path_p99 + ε + m) / P))`, where:
  - `P` = 33.37 ms (one gameplay tick, 2 retraces);
  - `path_p99` = the p99 one-way delay over the last 5 s on the sender's worst route to any peer (sender→host→peer in the star, direct or relayed in the hybrid), not RTT/2;
  - `ε` = time-sync phase error (about 2 ms);
  - `m` = forward and processing margin (about 1 ms).
- Floor 1, no "+1" slack tick. The +1 was the price of unsynced phase, which the phase slew removes (§2.10).
- A sender changes `d_i` on its own, with no host event: to increase, it sends tag G as a copy of G−1; to decrease, it drops one sample. Receivers just see contiguous tags. Decreases happen at boundaries. An increase may happen mid-round, and is the first response to a stall rate above 2% over 3 s.
- Cap: 8 ticks (266 ms). A player who would push their `d_i` above 8 gets a warning in the lobby. If the stall rate still stays above 5% for 10 s after an increase, they can be dropped by host vote.
- Fairness: with per-sender delay, better-connected players react faster. Competitive modes offer an **equal-delay** session option where every sender uses `max_i d_i` (open question, §11).

The full latency budget and every reduction are in §2.10. Where the lobby and relay run is in §2.11.

### 2.3 Session flow

1. **Lobby.** The host creates a session on `mppm-server` (§2.11) and gets a join code. Every peer pings every other peer and the relay before ready-up, and the lobby suggests the host that minimizes the worst client→host→client path (`argmin_h max_{a≠b}(owd_ah + owd_hb)`). Players take slots and pick teams, and spectators join without a slot. One machine may hold several slots (couch plus online; see §3.7).
2. **Config.** The host picks the mode, stage, mutators and handicaps. These go out reliably as a `SessionConfig`.
3. **Ready-up.** Each peer sends its `build_hash` and `content_hash` (§7b). A mismatch blocks ready and shows what differs.
   - `build_hash` is a SHA-256 of the running executable and its bundled shared libraries, plus a compiled-in string with the compiler version and the game and `aurora_mtx` flags.
   - A git SHA alone is not enough: two distro builds of one commit with different toolchains would pass the check and then desync.
4. **Start barrier.** The lobby enters the mode section directly, through a new `p2_mode_pending()` / `p2_mode_create()` hook. It is modelled on the shipped launcher-scene hook `p2_launcher_scene_pending()` / `p2_launcher_scene_create()` (`gameflow.cpp:149`; create at `port/game/scene/SceneSection.cpp:722-726`; pending check at `port/launcher/scene_bridge.cpp:569-571`).
   - **Stock menus are bypassed online.** The stock path goes through the J2D title, `vsSelect2D` and `challengeSelect2D`. Those screens read pads, check `mPadStatus[1].err` (`challengeSelect2D.cpp:1533`, `:1543`, `:1572`, `:1574`) and consume the RNG (`vsSelect2D.cpp:714`).
   - Instead, the lobby writes the `SessionConfig` into the same places `TitleState` fills on confirm (`vsGS_Title.cpp:324-351`):
     - handicaps into `mOlimarHandicap`/`mLouieHandicap`, which `check_VsSelect` sets at `:325`;
     - the stage data, file names, Pikmin container (`mContainer1`) and time limit into `VsGameSection`;
     - spray counts into `playData->setDopeCount` (`:346-347`), not into the section.
   - The barrier then does the following:
     - Calls `p2_msl_srand(sessionSeed)` before the section is created. This replaces the clock seed at `system.cpp:421` for the session.
     - Resets tick `T` to 0.
     - Applies the settings lock (§2.6).
     - Disables reset and long-press (`JUTGamePad.cpp:302-333`).
     - Steps the phase-slew time sync once with a hard jump onto the host's grid (§2.10).
     - Co-op story only: the host sends its save blob, and clients use a scratch card through the `TARGET_PC` CARD init in `MemoryCardMgr::init` (`memoryCard.cpp:165-166`, `CARDSetLoadType`/`CARDInit`).
5. **Load barrier.** In net sessions, DVD commands run inline (`dvdThread.cpp:183`).
   - The real loading loops are wrapped in `p2_net_nonsim_begin/end()`: `section.cpp:120-134`, `:264-273` and `waitSyncLoad` (`baseGameSection.cpp:178-208`, loop `:188-207`).
   - `Section::fadeOut` (`section.cpp:218-243`, loop `:221-235`) is **not** a loading loop: it calls `draw()` and `update()` (`:225`, `:228`), so it runs sim ticks. It stays outside `nonsim` and consumes input bundles like any other tick.
   - `end` is a barrier on a "loaded" bit in the input packets.
6. **Play.** Confirmed bundles are applied one per tick, and a checksum is exchanged every 30 ticks.
7. **Result.** Every peer computes the result from the simulation, so all peers agree on it by construction. The host posts it to the lobby server (leaderboards, cosmetics, stats) and offers **Rematch** or **Rematch, swap sides** (§6).

Floor, round and day transitions repeat step 5. Multi-floor modes reseed each floor from `hash(sessionSeed, floor)`.

### 2.4 Input injection seam

**The hook goes in `JUTGamePad::read()`, right after the `PADRead` and clamp calls (`JUTGamePad.cpp:133-142`).**

```cpp
#if TARGET_PC
	p2_net_exchange(mPadStatus); // no-op offline; online: overwrite every slot with confirmed inputs for tick T
#endif
```

Why it goes here:
- `read()` is called once per tick, from `System::beginFrame` (`system.cpp:882-886`). The only other callers are on the crash screen (`JUTException.cpp:605`, `:638`, `:648`).
- All game input reads the static `mPadStatus[]` array. That covers the 23 `new Controller(...)` sites in `src/` (20 use `JUTGamePad::PORT_n`, 3 use `Controller::PORT_n`; none in `port/`), the navis, `moviePlayer` and the 2D screens.
- Some code reads `mPadStatus[n].err` raw, so the hook overwrites `mPadStatus` itself, including `err`. The per-object `mPadReplay` hook (`JUTGamePad.cpp:178-186`) is not enough.

What `p2_net_exchange` does:
- Packs each local slot into about 8 bytes (`u16 button; s8 sx,sy,cx,cy; u8 tl,tr`) and tags it with `T + delay`.
- Writes every slot from bundle `T`. Empty slots get `PAD_ERR_NO_CONTROLLER` on every peer.
- Applies any `{tick, eventId, arg}` records in bundle `T` (§2.9).
- Is skipped while `nonsim` is active and when called from `JUTException`.
- Rejects any client packet that carries slots the client does not own (§10).

The launcher's `PADBlockInput` (`port/launcher/ui/input.cpp:922-933`; Aurora `extern/aurora/lib/dolphin/pad/pad.cpp:936-937`) runs before the hook. A player with an overlay open therefore sends neutral input, and the game keeps running for everyone else.

The hook is itself a game-side edit behind `#if TARGET_PC`. It keeps the MWCC build byte-identical, but it is not "no upstream diff".

**Stall loop, late sampling and tick order.** Stock order: one pass runs `beginFrame` (the pad read for tick T, `system.cpp:884`), then the pacing wait (`beginRender` → `waitForTick`), then draw, then update, then `endFrame` → `p2_host_present()` (`section.cpp:180-191`). With update-before-draw (§2.10) the draw and update swap places in `BaseGameSection`.
- The stall loop sits at the end of `p2_host_present()` (`port/main.cpp:375-486`), after the `hold_game` loop (`:378-405`). It waits for **bundle T+1**, the bundle the next `read()` will consume.
- It is merged with **late sampling** (§2.10): it waits until `tickDue − m` (m ≈ 1 ms) **and** `bundle_ready(T + 1)`, then calls `SDL_PumpEvents` so the next `read()` sees fresh pad state. `tickDue` is stored from `p2_vi_retrace_time_ns(*nextCount)` after the Game-mode receive in `p2_host_tick_wait` (`port/main.cpp:529-535`). The sleep is skipped in FreeRun and HostFrames (`:430-446`, `:451-521`) and wrapped in `p2_host_block_begin/end` so the audio and DVD threads keep running.
- Send time and deadline move together, so the network slack stays exactly `d·P`.
- If the bundle is still missing at `tickDue`, the body becomes a hold loop like `port/main.cpp:392-404`: `aurora_end_frame`, `beginHostFrame` (`:309-317`, which pumps events through `pumpEvents` at `:285-307`), `launcher::update()`, and a "Waiting for P3…" overlay with a drop button. The window is always serviced.
- `p2_net_exchange` keeps its own blocking fallback, so a missed wait can only cost time and never correctness.
- `read()` stays at its stock place and runs exactly once per sim tick. Two reads would double the repeat and long-press counters and lose button-down edges.

All game-side sites (every one behind `#if TARGET_PC`):
- `JUTGamePad.cpp:142`
- `system.cpp:421`
- `dvdThread.cpp:183`
- the three loading loops (§2.3 step 5)
- update-before-draw (`section.cpp:183-189`, or a `BaseGameSection` override) and the camera move (`baseGameSection.cpp:386-391`) (§2.5, §2.10)
- the prediction hook in `doDraw` and the sound mute sites (§2.10)
- if more than 4 ports are added (§3.7): `PAD_MAX_CONTROLLERS` (`include/Dolphin/pad.h:30`; Aurora's copy at `extern/aurora/include/dolphin/pad.h:28`), `EPadPort` (`JUTGamePad.h:21-26`) and the 4-entry channel masks (`JUTGamePad.cpp:4-5`)

### 2.5 Per-peer camera and HUD

Every peer simulates all N captains and all N `PlayCamera`s. Each peer **presents** one full-screen viewport bound to its local captain's camera.

| Concern | Rule | Code |
|---|---|---|
| Camera step | Move `cameraMgr->update()` out of `doDraw` to the **start** of `doUpdate`, before `doAnimation` (`doUpdate` is `baseGameSection.cpp:291-374`, first statement `:293`), for all N cameras. Pair it with update-before-draw (§2.10); on its own the move adds one tick (33 ms) of camera latency. | It runs in draw today (`doDraw`, `baseGameSection.cpp:380-410`; the calls are at `:386` and `:391`), but navi movement reads camera vectors (`navi.cpp:1506-1508`, `:2086-2088`). |
| Camera input | C-stick and L/R/Z travel in the packet like any other input. | already part of `PADStatus` |
| Sim frustum | LOD uses N **sim cameras** with a fixed logical frustum and a session-constant `k`, never the window size. | `Creature::updateLOD` (`creatureLOD.cpp:39-130`); `p2_wide_k()` (`graphics.cpp:197`, `port/sdk/wide.cpp:124-151`, `:165-168`); M5.1b: done. The cull planes use `Camera::mSimAspect` (the aspect times `p2_net_sim_k`, 3.6 in a session), so `video.aspectRatio` is now Local (`port/launcher/settings.cpp:132-135`); sites in [online/M5_1B_HOOKS.md](online/M5_1B_HOOKS.md#simulation-frustum) |
| Render viewport | One full-screen viewport, as `SceneSection` does. | `Graphics::allocateViewports(1)` pattern, `port/game/scene/SceneSection.cpp:117-123` |
| Local-only cameras | Spectator free cam, killcam and replay cam are never Viewports that `updateLOD` iterates. | `creatureLOD.cpp:39-130` |
| Draw passes | Particles and gauges iterate the active viewports instead of hardcoding VP0/VP1. | `baseGameSectionDraw.cpp:40-58` |
| Team visibility (ghost card and similar) | A render-only decision per local viewer. It must not write `mLod` bits that AI reads. | `pikiMgr.cpp:421-463`, `naviMgr.cpp:427-430`; `EnemyBase.h:269` reads `AILOD_IsVisible` |
| Sound | `mCameras = 1`, using the local camera position. | `baseGameSection.cpp:1260-1266`, `creatureLOD.cpp:87-112` |
| Rumble | Local slots only, remapped to the physical pads on that machine. Logical slots ≥4 never reach `PADControlMotor`. | `RumbleMgr.cpp:38-106`; `extern/aurora/lib/dolphin/pad/pad.cpp:974` |
| HUD | An RmlUi HUD per mode: the single-player-style panel for the local captain, an N-player scoreboard strip, and a name tag above each captain. | The J2D VS HUD is 2-player only (`ogObjVs.cpp:70-117`, `DispMember.h:699-735`) |
| Team colours | Tints come from a colour-blind-safe palette with a shape or icon per team, not colour alone. | render path only |

### 2.6 Menus, pause, movies and settings

| Thing | Today | Online |
|---|---|---|
| Settings lock | Per-setting metadata exists: `Net::Synced` ("the host's value is forced on every peer") and `Net::Forced` with `netValue` (`port/launcher/settings.hpp:263-264`, `:281`; table in `build_meta()` at `port/launcher/settings.cpp:98-185`). M5.1b: `net_lock_settings` is implemented (`port/launcher/settings.cpp:489`, specified at `docs/launcher/SETTINGS.md:1169`); harness runs lock with the defaults, M5.3b will pass the host's Synced values (D15 in [online/M5_1A_HARNESS.md](online/M5_1A_HARNESS.md#deviations-from-the-plan)). | Write `net_lock_settings`. At the start barrier, Forced settings take `netValue`. Synced settings take the host's value, which travels in `SessionConfig`; the lobby pushes it and does not block on a mismatch. The launcher greys out locked settings until the session ends. Cheats are read inside the sim and applied instantly by the `mirror` lambda in `bind_game_config` (`port/launcher/settings.cpp:456-486`, cheat mirrors at `:462-466`; called from `port/launcher/launcher.cpp:260`), so those mirrors are gated too. |
| `hold_game` | Pauses only this peer. Its redraw path deliberately **returns into the game** (`s_redrawFrames` / `aurora_preserve_frame`, `port/main.cpp:376-391`), and each return runs a full `update()`, so it is an extra sim tick. | Hard-disabled in code while `net::active()`: `hold_game()` returns false. Forcing the setting is not enough. |
| Other host-local pauses | `pauseInMenu`, `pauseInBackground` and `fastForward`, all Forced with `netValue` "false" (`port/launcher/settings.cpp:143-148`) | Forced by the lock, and also checked in code (M5.1b: `pauseInBackground` is ANDed with `!p2_net_active()` where it reaches Aurora, `port/launcher/launcher.cpp:147`, `:187`; `fastForward` is not implemented yet, so nothing reads it). A minimized window must not block keepalives: `beginHostFrame` loops on `SDL_Delay(5)` until `aurora_begin_frame` succeeds (`port/main.cpp:309-317`). The net thread (§2.2) keeps ENet serviced during that wait. |
| In-game pause menu | Either controller opens it, and it pauses everyone (`vsGS_Game.cpp:474-500`; story `singleGS_MainGame.cpp:557-564`). | Competitive modes: a local RmlUi overlay that does not pause. Co-op: a pause **vote**, applied as a host event. Online 2P Battle and Challenge as-is: left alone, because it is driven by synced input. |
| Cave and geyser menus | Pause both players (`vsGameSection.cpp:520-524`, `:551-553`, `:563-567`). | Ready-check instead: all living captains stand within range of the hole or geyser for 3 s. |
| Movies | Global sim events. `onMovieStart` calls `setPlayerMode` (`vsGameSection.cpp:680-686`); `moviePlayer->update(P1, P2)` (`baseGameSection.cpp:362-367`). | Movies still run identically on every peer, because `movie_begin` and freezes are sim state. Only the layout switch becomes local. Mid-game movie loads (`moviePlayer.cpp:526-541`) go through the load barrier. |
| Captain-down movie | In 2P Challenge, `s03_orimadown` freezes everyone (`vsGS_Game.cpp:879-891`). In Versus, `onOrimaDown` only records `VSLOSE_NaviDown` and returns (`:872-877`). Whole handler: `vsGS_Game.cpp:868-891`. | In modes with Lifeline (§5), a local overlay cue instead. |
| THP movies (story) | Polled (`pikmin2THPPlayer.cpp:390`). | Barrier at the end of the movie, or force-skip online. |
| Reset combo | Wall-clock B+X+Start (`JUTGamePad.cpp:11`, `:303-313`); the launcher's Reset Game sets the same flag (`port/game/reset.cpp:9-13`). | Disabled online. Reset becomes a lobby command. |

### 2.7 Desync detection and recovery

- **Checksum**, computed every 30 ticks at the end of `BaseGameSection::doUpdate` (`baseGameSection.cpp:372`). It covers:
  - sim RNG state (`port/sdk/msl_compat.c:15`) and tick `T`;
  - navi, piki and enemy positions and HP (`naviMgr`, `pikiMgr`, `generalEnemyMgr`);
  - `gameSystem` timers;
  - mode state (scores, lose causes, ledgers).
- Each peer piggybacks its checksum on input packets. In the star the host compares them. In the hybrid (§2.10) every peer sees every checksum and can detect a mismatch, and the host decides what happens.
- In the hybrid, a direct input copy that differs from the host-forwarded copy for the same tag is treated as equivocation by its sender, not as a desync (§10).
- **On mismatch:**
  - Every peer writes a report with the input log, `build_hash`, `content_hash`, seed, tick, and a per-subsystem checksum breakdown.
  - The session returns to the lobby showing "desync at tick T".
- **Recovery:** there is no mid-match resync from raw memory (§2.1). Multi-floor and multi-round modes restart at the next boundary from serialized mode state (scores, poko ledger, seed), not from a heap copy.
- **Debug builds:** a full checksum every tick, and an assert when the sim RNG is used during `draw()` or off the main thread (§4).

### 2.8 Disconnect, late join and spectators

| Case | Rule |
|---|---|
| Player drops | Their slot gets neutral input from the next confirmed tick on, and the captain stands idle. In co-op with Lifeline, the captain lies down as a carryable body (§5). In competitive modes, the team keeps its score and its Pikmin keep acting on their own. |
| Rejoin | Only at the next boundary (round, floor, day, wave break). The player takes back their old slot. |
| Late join as a player | Boundaries only. For modes that support it, navis for every slot are allocated at match start and driven with neutral input. A joiner attaches to an existing navi, so nothing is allocated mid-match. |
| Spectators | Receive the confirmed bundle stream and simulate locally, so they cost the sim nothing. The free cam is not a sim Viewport. |
| Late spectator | Downloads the bundle log from tick 0 plus the config, and catches up with the headless tick runner (§2.1). |
| Host drops | The session ends. The host is the sequencer and the canonical relay, in the v1 star and in the later hybrid alike. In the hybrid, only the sequencer role would have to migrate, which makes host migration cheaper; it stays an open question (§11). |
| Slot drop in the hybrid | Peers may disagree on the last tick they got directly from slot k. Each peer reports it to the host, which emits `slotDrop(k)` effective at max(reports)+1. A peer missing inputs at or below that tick fetches them from the host's canonical log. |

### 2.9 Host authority for non-simulation things

These go over the reliable channel and never touch the sim:
- chat, quick-chat, emotes and pings (§6);
- ready state, kicks, slot and team changes;
- votes on map, mutators and mode;
- spectator predictions (§6).

Some host decisions do change the sim: a pause vote, a spectator event, a mutator draw, or a Ship's Navigator drop. For those, the host inserts a `{tick, eventId, arg}` record into the confirmed bundle for a future tick, and every peer applies it at that tick. The records ride in the **host's own slot stream**: its input for tick X carries `eventSeq` plus the events for X. Every peer already waits for the host's input before simulating X, so events add no new dependency and no latency, and the order is total by (tick, seq). This holds unchanged in the hybrid. A client request (vote, pause) goes client → host, and the host schedules it at `T_host + d_host`.

Lobby-side randomness (map shuffles, mutator draws) never calls the sim `rand()`. `vsSelect2D.cpp:714` shows how UI randomness leaks into the sim stream today.

### 2.10 Input latency

Lockstep adds `d·P` to every press. The local pipeline adds more than that today, and most of it can be removed without touching what the sim sees. This section is the decided latency stack.

Parameters for every number below:
- `P` = 33.37 ms, one gameplay tick = 2 retraces of `kRetracePeriod` (`port/sdk/os_thread.cpp:274`).
- `W` ≈ 5 ms of game-thread work per pass. GPU ≈ 3 ms.
- 60 Hz display, fullscreen with direct scanout, VSync on (FifoRelaxed, `extern/aurora/lib/webgpu/gpu.cpp:253-256`), 1 kHz pad.
- Figures are for turning, aiming and the cursor, press to photon (mid-screen). They are estimates from the code, not measurements; §8 adds the harness that measures them.

**Input-to-photon path, offline.**

| Stage | What happens | Code | Stock (ms) | After (ms) |
|---|---|---|---|---|
| Device | USB polling | hardware | 1 | 1 |
| Sample quantisation | A press waits for the next pad read, once per tick (average P/2) | `JUTGamePad::read` from `System::beginFrame`, `system.cpp:884` | 16.7 | 16.7 |
| Stale latch | SDL is pumped only at the end of the previous pass (`port/main.cpp:359` → `beginHostFrame`, `:309-317`). `PADRead` reads that cached state (`extern/aurora/lib/dolphin/pad/pad.cpp:711-730`). Then the pass blocks in the pacing wait (`JFWDisplay.cpp:479` → `port/main.cpp:522-536`). | | 28.4 (P−W) | 1.5 (late sampling) |
| Tick start to submit | Draw runs before update (`section.cpp:180-191`), so pad T first shows in the next pass | | 38.4 (P+W) | 5 (W, update-before-draw) |
| GPU, vblank, scanout | GPU ≈ 3, wait for vblank ≈ 8.3 on average, scanout to mid-screen plus panel ≈ 11 | `gpu.cpp:253-256`; `present_due`, `port/main.cpp:260-273` | 22.3 | 22.3 (≈ 14 with Immediate present) |
| **Offline total** | | | **107** | **47** |
| Online | add `d·P` (§2.2) | | | |

- Movement adds one more tick on top of these figures. The position is integrated in `doSimulation` (`baseGameSection.cpp:349`) after the navi's matrices are built (`navi.cpp:706-713`).
- Camera (C-stick, L): stock 74 ms, because the camera steps in `doDraw` right after the latch (`baseGameSection.cpp:386-391`); after: 47 ms.
- Whistle and throw SFX also go through the audio path, which keeps about two DMA buffers queued (`port/audio/audio.cpp:171-175`). With the local cue (item 5): about 55–80 ms, or 45–60 ms with one DMA buffer (not measured).

**Reductions.** Only these are adopted. Each one is lockstep-safe because what every peer feeds into the sim stays the confirmed bundle; only local timing or local presentation changes.

| # | Reduction | Where | Saving | Why it is lockstep-safe |
|---|---|---|---|---|
| 1 | **Late sampling, merged with the stall loop.** At the end of `p2_host_present`, sleep until `tickDue − m` (m ≈ 1 ms) and `bundle_ready(T+1)`, then `SDL_PumpEvents` only (keyboard and gamepad state, read at `pad.cpp:717`, `:808`). `pumpEvents` and the launcher callbacks stay where they are. Skip in FreeRun and HostFrames. Wrap the sleep in `p2_host_block_begin/end`. | `port/main.cpp` only: the sleep after the `hold_game` loop (`:378-405`), `tickDue` stored after the Game-mode receive (`:529-535`) from `p2_vi_retrace_time_ns` (`port/sdk/os_thread.cpp:926`) | P−W−m ≈ 27 ms on every channel, offline too | Still one `read()` per tick at its stock place; tags come from the tick counter, not wall time. The only wall-time reads in `read()` are the reset and long-press timers (`JUTGamePad.cpp:307`, `:311`, `:325`, `:329`), which are off online. Every peer shifts its send time and its deadline together, so slack stays `d·P`. No new `src/` edit. |
| 2 | **Net transport.** Dedicated net thread owning the `ENetHost` (SPSC queues to the game thread), `enet_host_flush` right after each send, a duplicate send half a tick later, the host forwarding each bundle as soon as it is complete, throttling off on the input channel. | `port/net/` | Removes up to one tick per relay hop that a once-per-pass ENet service would add. One lost packet costs about P/2 instead of a stall. | Only arrival times change. Bundle contents are identical. |
| 3 | **Phase-slew time sync and the new delay formula.** Each packet carries `(tick X, sender's steady_clock at the start of X, an echoed timestamp)`. Clock offset is estimated NTP-style (minimum-RTT sample over 2 s). Everyone converges on the host's schedule by slewing the grid: `step = clamp(slew, ±1 ms); nextRetrace += kRetracePeriod + step`. One hard jump is allowed at the load barrier. Delay `d_i = max(1, ceil((path_p99 + ε + m)/P))` per sender, no +1 (§2.2). | `port/sdk/os_thread.cpp:313` (slew), `port/net/` (estimator) | One tick (33 ms) of delay at most RTTs, because the +1 slack and the floor of 2 go away. A 16 ms phase error converges in about 16 retraces (0.27 s). | No sim code reads wall time: the only reads are `section.cpp:171` (US demo only) and the reset timers above. Alarms do not use `kRetracePeriod` (it is used only at `os_thread.cpp:279`, `:313-315` and `:926`), and the long-term tick rate is unchanged, so audio does not drift. Receivers apply each input at its tag, whatever `d_i` the sender chose. |
| 4 | **Camera step at the start of `doUpdate`, plus update-before-draw** in `BaseGameSection`. Not for other `Section`s: 2D screens may advance animations in draw. A session constant (in `SessionConfig` and the replay header) until a checksum A/B run proves draw has no sim side effects. | `#if TARGET_PC`: `section.cpp:183-189` or a `BaseGameSection` override; camera move out of `baseGameSection.cpp:386-391` to before `doAnimation` (`:301`) | One tick (33 ms) for turning, cursor and camera. Movement keeps its extra tick. | The sim sees the camera stepped with this tick's pad, the same camera → pad → sim order as stock. Every peer runs the same order because it is a session constant. |
| 5 | **Prediction for the local captain, cursor and whistle.** A read-only kinematic predictor sums the local pending inputs (tags P..P+d, from the send ring) through the `makeVelocity` math (`navi.cpp:1480-1530`, camera basis at `:1506-1508`). The offset is based on the sim position after `doSimulation`, clamped to navi speed (`mNaviParms`) × (d+2)·P, used only in walk-type states, and blended out over about 3 ticks. It is applied to the local navi, cursor and marker **draw** matrices in a `PC_ONLY` hook in `doDraw` before `draw3D`, never in viewCalc (`baseGameSection.cpp:325-330`) and never to world matrices. Local whistle and throw cue on the raw press; the sim's own call still runs (e.g. `navi.cpp:1467`, `:1472`) and its sound is muted at the JAI handle. No render-only camera yaw in v1, because LOD uses the sim camera's frustum (`creatureLOD.cpp:39-128`). | `port/game/`; `#if TARGET_PC` hook in `doDraw`; sound mute sites | Own captain and cursor: about 47 ms at any RTT. Others' actions still take the full figure. | Collision is derived from the world matrices (`navi.cpp:710-713`), which prediction never touches. Sim calls are never skipped, only muted, so `PSM` object state stays the same on every peer. Keyed on the local slot, which is presentation-only; a debug assert checks that "local" never reaches sim code. Getters must be const; `mapMgr->getMinY` (`MapMgr.h:73`) is checked for caches before use. Presentation RNG stream only (§4 item 3). |
| 6 | **Options** (launcher settings, off by default): Immediate present ("VSync: Off (tear)"; today VSync off picks Mailbox first, `gpu.cpp:257-264`, which saves nothing at 30 presents/s); X11 compositor bypass in fullscreen only (Aurora forces it to "0" at `extern/aurora/lib/window.cpp:419-420`; find out why first); one DMA buffer (`audio.cpp:171-175`); sticky edges as an Aurora `PADRead` option that peeks pending `*_DOWN` events (`SDL_PeepEvents` with `SDL_PEEKEVENT`) so taps shorter than a tick are not lost. Docs: a 120/144 Hz display. | `extern/aurora`, `port/audio`, launcher | Immediate ≈ 8 ms (tearing); bypass 0–16.7 ms on composited X11; one DMA buffer ≈ 10–25 ms on SFX (not measured); 144 Hz ≈ 10 ms | Presentation and audio output only. Sticky edges change how the local value is produced before it is sent; replays log what was sent. |
| 7 | **Later: hybrid mesh fast path.** Direct peer-to-peer input where hole punching works, with the host stream kept as the canonical copy and cross-checked (§10). Scheduled after the star works. | `port/net/` | One tick at internet RTTs (at 80 ms RTT: star needs 80+8+2+1 = 91 ms of slack, d=3; direct needs 40+8+2+1 = 51 ms, d=2). Nothing on LAN. | Inputs are still applied at their tags. Equivocation (different values for one tag to different peers) is detected by the cross-check and blamed on the sender. |

Frames in flight are already minimal: `present_due` keeps at most one present on the render worker (`port/main.cpp:269-272`). No change there.

**Smooth Motion interaction.** Smooth Motion (stage 2a, §2.1) draws in-between pictures from the HostFrames loop, and a tick can become due inside that loop. Both items were handled in M5.1b ([online/M5_1A_HARNESS.md](online/M5_1A_HARNESS.md#late-sampling), D16 and D19 there):
- Item 1: the late-sampling wait at the end of `p2_host_present` (`pre_read_wait`, `port/main.cpp`) now runs the HostFrames and Smooth Motion loop body itself, so the in-between pictures are drawn in that wait and the pad read follows them; the tick wait then has almost nothing left to do. Measured with Smooth Motion on and off (the latency tables in the harness doc).
- Item 4: the camera step moved to the start of `doUpdate` (`baseGameSection.cpp:293`; the stock calls at `:386`, `:391` are removed), and Smooth Motion interpolates the camera states the ticks stored. With Smooth Motion in effect the pass stays draw-first (D16). Smooth Motion stays forced off online.

**Rejected:** display phase-lock online (the phase slew owns the grid phase, and displays at different rates would give different grid rates); a Mailbox present or Fifo latency 1 (saves nothing at 30 presents/s); rebuilding navi matrices after sim (`mCollTree->update()` follows `calc()`, `navi.cpp:711-713`, so it changes collision); latching between draw and update; rollback-lite (sim RNG, collision tree, no snapshot); a 60 Hz sim (frame-counted logic).

**End-to-end budget**, turning and cursor, press to photon (ms):

| Scenario | Before: plan as first written (stock pipeline, star, formula with +1, floor 2) | Items 1–3 only (star) | **After: items 1–5, star (v1)** | Later: + hybrid mesh (item 7) | Own captain / cursor with prediction |
|---|---|---|---|---|---|
| Offline | 107 | 80 | **47** | – | 47 |
| LAN (path ≈ 1 ms) | d=2 → 174 | d=1 → 113 | **d=1 → 80** | d=1 → 80 | 47 |
| 30 ms RTT | d=2 → 174 | d=2 → 147 | **d=2 → 114** | d=1 → 80 (Wi-Fi: d=2 → 114) | 47 |
| 80 ms RTT | d=4 → 240 | d=3 → 180 | **d=3 → 147** | d=2 → 114 | 47 |

- Assumptions: the same RTT for every pair; jitter p99 of 8 ms at 80 ms RTT and 5 ms at 30 ms; ε = 2 ms; m = 1 ms.
- At 30 ms RTT the hybrid d=1 case has only about 10 ms to spare (33.4 − (15 + 5 + 2 + 1)), so measure stall rates before relying on it.
- Immediate present takes about 8 ms off every cell, with tearing. A 144 Hz display takes about 10 ms more.
- Movement adds one tick to every cell except the predicted column.

### 2.11 Servers and hosting (decided)

The lobby runs on a small hosted server, and relaying uses a hosted TURN service. Players host the game sessions themselves; no server runs game logic.

**Decision.**
- **Lobby:** `mppm-server`, one Rust binary (tokio and axum, static musl build), in a new top-level `server/` crate. It is not game code, so the matching build is unaffected. The official instance runs on **Fly.io**: one shared-cpu-1x machine in one region (`iad`, US East), plus a 1 GB volume for SQLite.
- **Relay:** **Cloudflare Realtime TURN** (anycast), with free STUN at `stun.cloudflare.com`.
- **Client NAT layer:** **libjuice** (ICE, STUN and TURN client, MPL-2.0, no dependencies) under ENet, through a socket shim `port/net/enet_juice.c`. Each remote peer gets a synthetic address so ENet's peer table works. The host uses libjuice mux mode, so all agents share one UDP port. Direct candidates are preferred, and the TURN relay is used automatically when they fail.
- **Fallback** (Cloudflare unusable, or the libjuice spike fails): the same binary with `--relay` on 2–4 small VPSes (Hetzner or Vultr). It is a port-mapped UDP relay: a client registers from its ENet socket with a token and gets a dedicated relay port, so the host sees each relayed peer as a distinct address. The client hole-punches on ENet's own socket.

**Components.**

| Component | Role |
|---|---|
| REST | `POST /sessions` returns `{code, host_token}`; `POST /sessions/{code}/join`. Codes are ≥40-bit Crockford base32, expire with the session, and may carry a password. Rate-limited per IP. |
| WSS signalling | ICE candidate exchange between host and clients, kick and ban, host-election hint (§2.3). |
| Relay credentials | `relay = cloudflare` mints short-lived TURN credentials through the Cloudflare API (up to 48 h); `relay = shared-secret` uses the coturn `use-auth-secret` scheme; `relay = builtin` uses `--relay`. Clients receive an `iceServers` list and never hold provider credentials. |
| Storage (phase 2) | Leaderboards in SQLite on the volume. Replays in S3-compatible storage: Cloudflare R2 officially, a local directory or MinIO when self-hosted. Replay verification runs the headless runner as a separate worker (§6). |
| `--relay` mode | The fallback relay above. Forwards bytes unchanged, with per-session bandwidth and lifetime caps (§10). |

*(As built, M5.3b part 1: the game talks to `mppm-server` over **one WebSocket** (`GET /v1/ws`) that carries create, join, the member list and the signalling; the REST endpoints exist too (`POST /v1/sessions`, `POST /v1/sessions/{code}/join`, `GET /v1/health`) and return a token that a WebSocket `attach` redeems, for tools and other clients. The reason is `borealis::http`, which only speaks HTTPS, so the game could not reach a local plaintext test server over REST, and the signalling needs the socket anyway; `ws://` is allowed only on loopback. The server knows only membership: seats, teams, ready, mode, stage and the content check live in the host and travel over ENet. Relay modes `none`, `static`, `shared-secret` and `builtin` work; `cloudflare` refuses to start until the spike below is done. The shim is `port/net/transport/enet_juice.c`. The client setting is `online.serverUrl` (alias `online.server_url`), empty by default until the official server is deployed. Part 1 is local only: nothing was deployed and no Cloudflare account was used. See [online/M5_3B_NETCODE.md](online/M5_3B_NETCODE.md) and `server/PROTOCOL.md`.)*

**Regions.**

| Piece | Official deployment |
|---|---|
| Lobby | One Fly region (`iad`). Signalling latency does not matter. |
| Relay | Cloudflare anycast covers NA East, NA West, EU and Asia with no configuration. Relay addresses are IPv4 only. |
| Fallback relay | Hetzner Falkenstein (EU), Hetzner Ashburn (NA East), Hetzner Hillsboro or Vultr Los Angeles (NA West), Hetzner Singapore or Vultr Tokyo (Asia). The lobby picks the one with the lowest RTT measured by the host. |

**Cost** (late 2026 prices). A relayed client in a 4-player session uses about 12 KB/s, about 44 MB per hour, at 30 Hz. That figure is for single sends; the duplicate send (§2.2) is assumed to cancel out against run-length encoding of the 8-tick redundancy block (about 40% of the raw size). The relayed share is assumed to be 15% of non-host peers; measure it in M5.3b, and double the relay row if it is 30%.

| Scenario | Lobby (Fly) | Relay (Cloudflare TURN) | Storage | Total |
|---|---|---|---|---|
| Idle | $2.19 machine + $0.15 volume, waived (personal orgs under $5/month pay nothing) | $0 (1 TB/month free, then $0.05/GB) | R2 $0 (10 GB free) | **≈ $0–1/month** (the dollar is the domain) |
| 100 concurrent sessions, about 4 h/day | about $0 | ≈ 240 GB/month, inside the free 1 TB | $0 | **≈ $0–5/month** |
| 100 concurrent sessions, 24/7 | ≈ $3.50 | ≈ 1.0–1.4 TB billed → $0–22 | $0–2 | **≈ $5–28/month** |
| Fallback relay on 3–4 VPSes | as above | 4 × €6–7 including IPv4; included traffic covers it | – | **≈ €25–30/month fixed** |

**Self-hosting and custom server URL.**
- The repo ships `server/`, a `Dockerfile`, a `fly.toml`, and a `docker-compose.yml` with `mppm-server` and `coturn`. The compose file shares one `TURN_SECRET` between them and opens UDP 3478 plus 49152–65535 on coturn.
- `docker compose up -d` on any VPS or home box, or `fly launch`. TLS comes from a Caddy sidecar or the Fly edge.
- Relay choice: `relay=shared-secret` (coturn), `relay=cloudflare` (your own Cloudflare account), or `relay=builtin` (`--relay`, no coturn).
- The client setting `online.server_url` defaults to the official server and can be changed in the launcher settings and on the CLI. Every STUN and TURN address comes from that server, so a fork points at its own server and changes nothing else. Join codes, leaderboards and replays live on whichever server is used.
- Maintainer ops: `fly deploy` from GitHub Actions on tag, nightly SQLite backups to R2, and the Cloudflare TURN usage dashboard.

**Rejected.** Steam Datagram Relay (not reachable from open-source GameNetworkingSockets, and needs a Steam release). Epic EOS P2P (licence and product-identifier terms conflict with an open-source build anyone can fork). Cloudflare Workers + Durable Objects for the lobby (a good fit, but a second TypeScript codebase and an awkward self-host path).

**Early check.** Cloudflare documents TURN only with WebRTC examples. The first M5.3b task is a spike: a libjuice agent against `turn.cloudflare.com` carrying ENet traffic. If it fails, the official relay switches to coturn or `--relay`, with no client change. *(M5.3b part 1 proved the same client path, ENet over a libjuice relay candidate, against libjuice's own TURN server on 127.0.0.1; the spike against Cloudflare needs an account and is part 2.)*

## 3. Engine work: from 2 captains to N

The real limit is the game content, not the input layer. In Versus, the team **is** the Pikmin colour: navi 0 means Red and navi 1 means Blue, and that mapping is coded inline in about 30 places.

### 3.1 Player, team and owner identity

Add these under `TARGET_PC`:
- `mPlayerId` and `mTeam` on `Navi`.
- On `Piki`, a **team byte** (which side) and an **owner byte** (which navi plucked or whistled it last).
  - The owner byte matters in 2v2. Two Red captains share `getKind()`, and whistle and pluck filter by colour, so without an owner they would steal each other's squads.
  - Whistling within a team still works (a deliberate squad handoff). Auto-pluck and spawn ownership use the owner byte.

Keep `mNaviIndex` 0 and 1 for model, voice and effect choice. Then replace every inline colour-as-team check:

| Site | What it does today |
|---|---|
| `navi.cpp:301` | Plucks only if `mColor == 1 - mNaviIndex` |
| `interactPiki.cpp:58-65`, `:194-199` | Whistle rejects the enemy colour by literal |
| `interactPiki.cpp:233-240` | Bitter spray hits `getKind() == getNaviID()` |
| `itemPikihead.cpp:658-661` | Noisemaker auto-pluck |
| `pikiAI.cpp:252-253`, `:627` | Pikmin attack a navi when `mNaviIndex == getKind()` |
| `pikiAI.cpp:117`, `:371`, `:657` | No carrying your own marble. Yellow bedama has colour 2 (`pelletOtakara.cpp:46-48`; Red=1 at `:40-42`, Blue=0 at `:43-45`), so it needs a sentinel such as -2 |
| `fakePiki.cpp:992` | Revive warps to `getOnyon(1 - mNaviIndex)` |
| `piki.cpp:788-790` | VS damage is normalized to Red damage |
| `aiPrimitives.cpp:600-605`, `:1084-1100` | Carry pathing has only `PATHFLAG_VsBlue/VsRed` |
| `pelletOtakara.cpp:76-110` | Carry-sound goal logic is Red/Blue only |
| `onyonMgr.cpp:484-520` | Marble and cherry messages use player = `1 - onyonType`, which gives -1 for Yellow |
| `vsCardMgr.cpp:258-418`, `:540-556` | Card effects hard-code 2-player player-to-colour maps: `1 - user` for the user's own colour (`:258`, `:267`, `:314`, `:326`) and the enemy navi (`:395`), `getOnyon(user)` for the opponent's onyon (`:362`, `:418`), and `(!user) ? 4 : 5` (`:416`). DOPE_BLACK, DOPE_RED and PIKMIN_XLU use `naviMgr->getAt(user)` (`:276`, `:283`, `:290`). |

Already N-safe:
- Pikmin-vs-Pikmin combat checks only "different kind" (`pikiAI.cpp:69-73`, `:647-650`). It moves to the team byte so that two teams with the same colour also fight.
- Kill recharge goes to `getOnyon(getKind())` (`piki.cpp:190-195`).

### 3.2 NaviMgr and Navi

| Blocker | Where | Fix |
|---|---|---|
| Fixed 2 navis | `naviMgr->alloc(2)` (`baseGameSection.cpp:2203`; also `singleGS_Zukan.cpp:1423`) | `alloc(N)` |
| Index 2 is a mode value | `NAVIID_Multiplayer = NAVIID_Both = 2` (`Navi.h:28-33`) | A separate `PLAYERMODE_*` enum for `setPlayerMode` |
| "The other navi" | `GET_OTHER_NAVI = 1 - mNaviIndex` (`Navi.h:40`), or an inline `naviMgr->getAt(1 - mNaviIndex)` (`navi.cpp:314`). The macro is used at `navi.cpp:1960`, `:2020`, `interactNavi.cpp:257`, `naviState.cpp:488`, `:918`, `:985`, `:1331`, `:2494`, `:3599` | Per site: nearest teammate, or not applicable |
| Dead flags overflow | `mNaviDeadFlags[2]` (`Navi.h:357`) in `informOrimaDead` (`naviMgr.cpp:298-307`); `getAliveOrima` assumes 0, 1 or 2 dead (`naviMgr.cpp:331-366`) | Size to N; return an alive count |
| Model read out of bounds | `(&mOlimarModel)[naviID]` (`naviMgr.cpp:236`); naviID 2 reads `mCursorModelData` | `mNaviIndex % 2`, plus a tint |
| Anims | `mCursorAnims[2]`, `mMarkerAnims[2]` (`Navi.h:359-360`; used at `navi.cpp:170-171`) | Size to N |
| Sound director | `PSM::DirectorUpdator(.., 2, ..)` (`naviMgr.cpp:63-65`, `:443-456`) | Size to N |
| "0 is Olimar, anything else is Louie" | `navi.cpp:153`, `:164`, `:184-189`, `:626`, `:1215`; `naviMgr.cpp:131-136` | Keep, keyed on `mNaviIndex % 2` |
| Active navi | `getActiveNavi()` returns Olimar in multiplayer (`naviMgr.cpp:151-165`). It has 47 call sites in 25 files, including enemy AI (Queen, blackMan, Rock, DangoMushi, shijimiChou), CameraMgr and RumbleMgr. | Sim callers: a deterministic nearest navi, following `umiMushi.cpp:848-852`. Presentation callers: the local player's navi. Audit every site. |
| Enemy attack bookkeeping | `EB_AttackingNavi0/1` (defined at `include/Game/EnemyBase.h:75-76`, cleared each update at `src/plugProjectYamashitaU/enemyBase.cpp:482-483`) | Size to N |
| Squad counter | `PikiNaviCounter` bucket 2 means "no navi" (`gameStat.cpp:169-173`, `:189-193`) | Give "no navi" its own bucket |
| Play data | `mNaviLifeMax[2]`, `mDeadNaviID`, `mOlimarData[2]` (`gamePlayData.h:450-458`) | Size to N |
| Spawning | `switch (naviCount) {0,1,2}` (`baseGameSection.cpp:841-974`) | A loop over N: the team's onyon plus a per-player offset |
| Controllers | `mControllerP1/P2` (`BaseGameSection.h:286-287`), bound in `setCamController` (`baseGameSection.cpp:1154-1218`) | `Controller* mCtrl[N]`, with P1/P2 aliased to [0]/[1] |
| Death assert | `P2ASSERTBOUNDSLINE(.., naviIdx, 2)` (`vsGS_Game.cpp:870`) | N |

### 3.3 Cameras, viewports and LOD

- `CameraMgr` allocates its arrays with `new X[2]` (`src/plugProjectNishimuraU/CameraMgr.cpp:30-42`) and has `mCameraParms[2]` (`CameraMgr.h:418`). `update()` copies the active camera into the other one with `id ^= 1` (`src/plugProjectNishimuraU/CameraMgr.cpp:139`). Online, it must update all N cameras every tick.
- Replace `mOlimarCamera`/`mLouieCamera` with `PlayCamera* mCams[N]`, and keep the old fields aliased.
- `BlendCamera(2, ..)` and `ShadowMgr(2)` (`baseGameSection.cpp:2199`, `:2217`; `shadowMgr.h:104-106`) are sized to the presented viewports, not to N.
- LOD is hard-wired to 2 viewports:
  - `vpStats[2]` (`creatureLOD.cpp:53`);
  - `P2ASSERTBOUNDSLINE(...,2)` (`creature.cpp:365`);
  - only 4 viewport bits, `AILOD_IsVisVP0..3` (`AILOD.h:12-15`).
- **Design:** a sim "observer" list of N cameras feeds LOD.
  - `AILOD_IsVisible` means "visible to any observer".
  - The per-viewport bits stay presentation-only.
  - Sim visibility no longer depends on the on-screen layout.
- **Online N-captain play does not need `FourSplitter`**, because each peer presents one viewport.
- Local 3–4-player split screen is a separate, optional item. It needs `FourSplitter`, which is currently a stub (`graphics.cpp:89-98`). `particleMgr->setViewport` asserts a viewport count below 4, so at most 3 (`particleMgr.cpp:357`; the bound is exclusive, `P2Macros.h:10-12`). 4-way split screen also needs this bound raised.

### 3.4 Pikmin cap and heap budget

**Pikmin cap.** `MAX_PIKI_COUNT (100)` (`Piki.h:10`) is a global, hard limit. It is used in:
- `pikiMgr->alloc(MAX_PIKI_COUNT)` (`baseGameSection.cpp:2244`);
- stack arrays at `pikiMgr.cpp:596`, `:655`, `:679`, `:715`, `:755`;
- spawn cap checks at `pikiMgr.cpp:104`, `:123`;
- `ModelMgr(..., MAX_PIKI_COUNT, ..., viewNum)` (`pikiMgr.cpp:207`).

With 4 captains on the stock cap, each gets 25 Pikmin. Two options:

| Option | Cost |
|---|---|
| Keep 100 and design modes around it (squads of 25–33, faster Onion recharge) | None. This is the launch default. |
| Raise it to `P2_MAX_PIKI` (e.g. 160) under `TARGET_PC`, for net and mode sessions only | Floor-heap budget, a larger `ModelMgr`, larger stack arrays, CPU for AI and collision. Profile it in M5.3a. Changing it alters behaviour, so it is part of `SessionConfig` and the content hash. |

Co-op story's "squad cap = floor(100 / living captains)" is a consequence of this limit, not a free design choice.

**Heap budget.** Each extra navi adds a model instance, anims, a `PSM` director slot and camera objects. The floor heap takes all free space (§2.1), but some sub-heaps are fixed, e.g. `ENEMY_HEAP_SIZE_VS` (`generalEnemyMgr.cpp:756-760`) and `SYSTEM_HEAP_SIZE` (`BuildSettings.h:18`).
- Budget per mode: 4 navis, the enemy roster, and the Pikmin cap.
- Bounty Swap and Last Light register larger rosters, so they need a per-mode enemy heap size.
- MEM1 is 256 MiB (`kMem1Size`, `port/main.cpp:583-585`, used at `:597`), so there is room. The fixed sizes are what need changing.

### 3.5 N teams: Pikmin colours and onyons

| Teams | Approach | Blockers |
|---|---|---|
| 2 (1v1, 2v2) | The existing Red/Blue machinery, plus the owner byte for 2v2 | §3.1 only |
| 3 | Red, Blue and Yellow onyons. `ItemOnyon::mOnyons[ONYON_TYPE_MAX=3]` already exists (`ItemOnyon.h:14-18`, `:221`), and `getPelletGoal` already chooses among 3 colours (`pelletMgr.cpp:1943-1980`). | the bedama colour sentinel; `1 - onyonType` (`onyonMgr.cpp:484-520`); only `FIXNODE_VsRedOnyon/VsBlueOnyon` exist (`Node.h:17-21`); `setVersusOnyon` places only 2 (`RandMapScore.cpp:313-338`); the 1-D Red↔Blue versus score (`RandMapScore.cpp:173-180`, `:408-418`); `getStartPosition` (`RandMapMgr.cpp:182-193`); Red/Blue cases in `RandItemUnit.cpp:504-507`, `:581-606` and `RandEnemyUnit.cpp:334-340`, `:544`, `:771`, `:944-947`, `:1123`; the onyon 50-Pikmin cap (`onyonMgr.cpp:1202-1211`) |
| 4 (FFA) | A team byte on `Piki` plus a colour tint, and Onion-less goals (pod or beacon) for the 4th team | everything above, plus a render tint path |

General changes:
- K-way farthest-point onyon placement.
- A per-team distance field in place of the 1-D versus score: drop weight = normalized distance to the nearest team compared with the others. It feeds `getItemDropPosition` (`RandMapMgr.cpp:201-210`).
- Per-team `PATHFLAG`s.

**How 4 players map onto at most 3 Onions** depends on the mode. §5 lists each mode's supported shapes:
- 2v2: two teams on Red and Blue, two captains each.
- 1v1v1 / 2v1v1: three Onions.
- 4-player FFA: tinted team bytes and Onion-less goals.

### 3.6 Mode state sized to N

All the `[2]` arrays in `VsGameSection.h:119-172` and `VsGame.h` (`mSlotMachines` at `:220`, `mLoseCauses` at `:420`) become indexed by player or team, along with `mMarbleCount` and scores. The two scalar handicaps `mOlimarHandicap`/`mLouieHandicap` (`VsGameSection.h:150-151`) become an array. The 2-value `VSPlayerColor` enum (`VsGame.h:56-59`, `VSPLAYER_Red=0`, `VSPLAYER_Blue=1`) becomes a team index.

`calcVsScores` (`vsGameSection.cpp:1373-1548`) is a 2-onyon sigmoid. It becomes a per-team score with a "leader" query.

Winner resolution handles only P1/P2/Draw (`vsGS_Game.cpp:205-235`, `:345-381`). It becomes a ranked result shown on an RmlUi results screen.

### 3.7 HUD, radar, input slots and local seats

- **HUD:** RmlUi per mode, not J2D.
  - `DispMemberVs` (`DispMember.h:699-735`, fields at `:730-731`) and `DispMemberChallenge2P` (`DispMember.h:214-240`, fields at `:232-233`) have fixed Olimar/Louie fields.
  - Card slots draw at fixed y=115 and y=315 (`vsCardMgr.cpp:176-185`). Online they always draw at the local origin.
- **Radar:** the story radar (`singleGameSection.cpp:1143`) shows N captain icons. Captains 3 and up get a tinted variant of the icon at `navi.cpp:184-189`. The radar draws from the presentation RNG stream (§4).
- **Input slots:** 4 captains fit the existing 4 ports. The limits are:
  - `PAD_CHANMAX`/`PAD_MAX_CONTROLLERS = 4` (`include/Dolphin/pad.h:30`; `extern/aurora/include/dolphin/pad.h:17`, `:28`);
  - `EPadPort` `PORT_0..3` (`JUTGamePad.h:21-26`);
  - static arrays sized `PAD_MAX_CONTROLLERS` (`JUTGamePad.h:304-312`, `JUTGamePad.cpp:4-17`), including the hardcoded 4-entry channel masks (`JUTGamePad.cpp:4-5`);
  - long-press state and timers checked against `PAD_MAX_CONTROLLERS` (`JUTGamePad.cpp:321-333`).

  More than 4 captains would need a `TARGET_PC` `P2_PAD_PORTS` that resizes all of these, while `PADRead` still fills only 0..3. That is more than a day of work and is **not launch scope**. Seats that have no navi, such as Ship's Navigator and spectators, do not need pad ports: their actions are host-stamped events (§2.9).
- **Local plus online seats.** One machine may hold several slots, e.g. 2 local and 2 remote.
  - The lobby assigns physical pads to slots on each machine.
  - `p2_net_exchange` packs every local slot.
  - That machine presents a 2-way split (`HorizonalSplitter`, `graphics.cpp:18-53`) of its local captains' cameras. Those cameras are presentation views, not sim observers. The sim observers are always the N sim cameras.

## 4. Determinism hardening checklist

Items are in priority order, and every one is in `port/` or `PC_ONLY`. Items 1–11 are required for same-OS online play. Item 12 (save data) is required only for modes that read story save state, starting with M6. Items 13–14 gate cross-architecture play.

| # | Item | Why | Where | Fix |
|---|---|---|---|---|
| 1 | Checksum, input-replay harness and headless tick runner | Every other item needs a way to see failures | end of `doUpdate` (`baseGameSection.cpp:372`); `waitForTick` (`JFWDisplay.cpp:464-487`) and its hook `p2_host_tick_wait` (`JFWDisplay.cpp:479`, `port/main.cpp:493`) | CRC as in §2.7. A headless runner that bypasses retrace pacing in `p2_host_tick_wait`. One input log replayed on two machines must give the same checksums. |
| 2 | Seed the RNG per session | Clock seed | `srand(OSGetTick())` at `system.cpp:421`; LCG at `port/sdk/msl_compat.c:15-23` | `p2_msl_srand(seed)` at the start barrier and on every floor |
| 3 | Split RNG streams | One stream feeds the sim, the UI and the DVD thread: 633 call sites in 147 files | HUD every frame: `ogCounterRV.cpp:258`, `ogCounterSlot.cpp:156`, `ogOtakaraSensor.cpp:125`, `:408`, `:416`, `ogObjFloor.cpp:389`, `singleGameSection.cpp:1143`. Menus: `vsSelect2D`, `challengeSelect2D`. Launcher scene: `port/game/scene/SceneTakeoff.cpp:1320`; `port/game/scene/SceneDirector.cpp:124`, `:136`, `:230`, `:515`, `:628`, `:662`, `:731-765`, `:785`, `:894`, `:1366` | `p2_msl_rand` picks a stream by thread and by a presentation flag set around `Screen::Mgr`/`Game2DMgr`, `particle2dMgr` and `port/game/scene`. Only the sim stream is checksummed. Assert on sim-stream use in `draw()` or off the main thread. `getPelletGoal` breaks ties with `randInt` (`pelletMgr.cpp:1965`), so carry outcomes depend on this too. |
| 4 | Effects spawned from sim code | `efx::*` and `particleMgr` emitters are created inside `update()`. JPA has its own RNG seeded from a manager RNG (`JPAMath.h:19-24`, `JPAEmitter.cpp:41`), but emitter setup code may call the global `rand()`, and emitter creation can be gated on LOD. | `efx::*`, `JPAEmitter.cpp` | Audit every `rand()` reached from an emitter create. Route those calls to the presentation stream, unless the result feeds back into the sim. |
| 5 | Sim frustum separated from render | LOD gates enemy collision and animation (`enemyBase.cpp:410-415`, `:490-492`, `:1684-1724`, `:1831-1838`), Qurione's fly-kill (`Qurione.cpp:273`), pellet physics (`pelletMgr.cpp:1322-1342`), piki joints (`fakePiki.cpp:829-839`, `piki.cpp:1200`), `shijimiChou.cpp:218`, `:768`, and `pikiMgr.cpp:453`, `:461-463`, `:475` | `creatureLOD.cpp:39-130`, `graphics.cpp:197`, `port/sdk/wide.cpp:124-151`, `:165-168` | A fixed logical frustum, a constant `k` from the session, and N sim observer cameras (§3.3) |
| 6 | Camera stepped in the tick | Movement reads camera vectors, but the camera updates in draw | `baseGameSection.cpp:380-410` (calls at `:386`, `:391`); `navi.cpp:1506-1508`, `:2086-2088` | Under `TARGET_PC`, update all N cameras at the start of `doUpdate`, paired with update-before-draw (§2.10 item 4). Hard-disable the `hold_game` redraw returns online (`while (mppm::launcher::hold_game())` with the `s_redrawFrames`/`s_redrawn` early returns, `port/main.cpp:456-484`). |
| 7 | Load barrier | Async DVD completion lands on different ticks on each peer, and cave generation runs on the DVD thread (`baseGameSection.cpp:2173`, `:2273`; `vsGS_Load.cpp:27`, `:91`) | `system.cpp:788-794`; polls at `vsGS_Load.cpp:101`, `:110`, `singleGS_Load.cpp:97-101`, `moviePlayer.cpp:526-541`; `waitSyncLoad` at `baseGameSection.cpp:178-208` | Inline DVD commands in net sessions (`dvdThread.cpp:183`), `nonsim` wrappers, and a barrier on a "loaded" bit. Index ticks by sim frame, not by pad reads. |
| 8 | Input at the sim tick; wall-clock pad logic off | Reset and long-press use `OSGetTime` | `PADRead` at `JUTGamePad.cpp:133`, hook at `:142`; `OSGetTime` at `:307`, `:311` (reset) and `:325`, `:329` (long-press), in `:302-333`; `reset.cpp:39+`; `port/game/reset.cpp:9-13` | §2.4; clear `sResetPattern` online |
| 9 | Settings lock, implemented | The metadata exists but nothing enforces it. Cheats are read in the sim: `navi.cpp:413`, `:1673`, `:1706`, `interactNavi.cpp:176`, `timeMgr.cpp:167`, `singleGameSection.cpp:1048`, `gamePlayData.cpp:1142`, `gamePlayCommonData.cpp:188` | `port/launcher/settings.hpp:263-264`, `:281`; per-setting net classes at `port/launcher/settings.cpp:98-185` (e.g. cheats `.net = S` at `:150-155`); netplay rules at `docs/launcher/SETTINGS.md:61-63` and §14 "Netplay readiness" at `:1151` | Write `net_lock_settings` (§2.6). `hideHud` is safe to leave unlocked (`baseGameSection.cpp:1465`, `:1888-1893`). |
| 10 | Clock and audio coupling | Clock reads in play data: `gamePlayData.cpp:467`, `:552`, `gamePlayDataMemCard.cpp:227`. Sim code may also wait on audio state (BGM or stream end, `PSM` director callbacks, movie and THP audio sync). The audio thread finishes at a different wall time on each peer. | `gamePlayData.cpp`, `PSGame.cpp:1029+`, `pikmin2THPPlayer.cpp:390`, `port/sdk/os_thread.cpp:236-242` | Confirm the clock reads only feed save metadata. List every sim branch on an audio query and replace each one with a tick count, or barrier it. Verify whether JAudio callbacks allocate from `sCurrentHeap`. |
| 11 | Vendored libm | Most gameplay `sinf`/`cosf`/`acosf` calls are not libm calls: `include/trig.h:25`, `:41`, `:74` redefine them inline as look-ups in `JMath::sincosTable_` and `asinAcosTable_` for every file that includes it (through `Game/Creature.h`, `Game/EnemyBase.h`), and those tables are built at start-up from the host's double `sin` and `cos` (`JMath.h:217-223`). The real libm imports of the game library are few: `sinf`/`cosf` (`jstudio-math`, `object-light`, `texCaster`, `port/game/scene`), `tanf`, `atanf`, `atan2f` (`camera`, `JUTGamePad`, `panModoki`, J2D), `expf`, `log10f`, `pow`, `powf`, the double `sin`/`cos`/`asin`/`atan` of `JMATrigonometric` (the tables), and `aurora_mtx`'s `MTXRotRad`/`MTXRotAxisRad` (`mtx.c:410-411`, `:484-485`), which `camera.cpp:149-161` (the cull planes) and several enemies reach. **This is not just a cross-OS issue.** glibc picks FMA variants of `sinf`/`cosf`/`sin` at runtime through ifunc on x86_64, so two Linux machines with different CPUs may disagree. | `nm -u build-pc/libp2game.a` and `libaurora_mtx.a`; the per-symbol list is in [online/M5_1B_BUILD.md](online/M5_1B_BUILD.md) | Vendor one fixed-algorithm libm (`port/libm/`) and route the game's and `aurora_mtx`'s libm symbols to it under `TARGET_PC` (a symbol rename at link level, since macros cannot reach the inline `trig.h` names, JMath members or `std::` overloads). Compile it with `-ffp-contract=off`. |
| 12 | Save data | The sim and audio read story flags (`PSGame.cpp:1050`) | Save-load path: the play-data read in `gamePlayDataMemCard.cpp`; scratch card via the `TARGET_PC` CARD init (`memoryCard.cpp:165-166`). Clock reads in the card-write path (item 10 material): `memoryCard.cpp:595`, `pikmin2MemoryCardMgr.cpp:1993` | The host sends its own blob or a neutral blob. Clients use a scratch card. |
| 13 | Zero-on-alloc and uninitialized state | `JKRExpHeap` never fills blocks (`JKRHeap.cpp:25`, `:57`), so contents depend on history; `-fno-strict-return` (`CMakeLists.txt:111-115`) | `p2_heap_alloc` at `JKRHeap.cpp:480-489`; `JKRHeap::alloc(u32, int)` at `:199-205` | Zero blocks in net builds, add `-ftrivial-auto-var-init=zero`, audit `-Wreturn-type`, and do one MSan or Valgrind pass over a VS match |
| 14 | FP flags on `aurora_mtx` (cross-arch) | No `-ffp-contract=off`, so FMA contraction on arm64; host trig at `mtx.c:410-411`, `:484-485` | `extern/aurora/cmake/aurora_mtx.cmake:1-12`; `MTX_USE_PS` at `CMakeLists.txt:100` | Use the same flags as the game (`CMakeLists.txt:108`). Check MXCSR/FPCR at every tick. |

Item 13 is listed after the same-OS set because it is a hardening pass. It still goes into M5.1b, because uninitialized fields can diverge even on identical machines.

## 5. Game modes and content

Each mode has two judge scores out of 10: **feasibility (F)** and **fun**. Where one entry covers two rulesets, both are listed, e.g. "F 7 (1v1) / 4 (3-team)". The scores are inputs, and the tiers below also weigh dependencies.

Effort scale: S is days, M is 1–2 weeks, L is 3–6 weeks, XL is more.

Because the milestone order lifts the 2-captain limit (step 3) before new modes (step 4), Tier 1 modes ship with N-captain support from day one.

Every new mode shares one framework:
- the `p2_mode_pending()` / `p2_mode_create()` section hook. It is new, modelled on the shipped `p2_launcher_scene_pending()` / `p2_launcher_scene_create()` (`gameflow.cpp:149`; `port/game/scene/SceneSection.cpp:723`). The lobby enters the mode directly and skips the stock J2D menus (§2.3);
- a `GameState` subclass following `VSState` (`vsGS_VSGame.cpp:29`; overrides at `VsGame.h:314-335`);
- GameMessage handlers via `VsGameSection::sendMessage` (`vsGameSection.cpp:842`);
- a countdown taken out of the Challenge-only branch, because VS has no timer today (`vsGS_Game.cpp:265-311`);
- an RmlUi HUD;
- arenas, caves, cards and missions delivered as overlay data (§7a).

Modes are entered from the Moonsprout start menu's Multiplayer slot (`kMultiplayer = 20`, `port/launcher/prelaunch.hpp:35`; mentioned in the comment at `port/launcher/menu_registry.hpp:5`) and the reserved Online bar entry (`port/launcher/menu_bar.cpp:114`). The fixed 6-entry J2D title menu (`ebiScreenTitleMenu.cpp:50-77`) still leads only to stock VS.

### Tier 0: proof of netcode (not new content)

**Online 2P Battle and 2P Challenge as-is.** Lobby slot i becomes logical port i, and the existing `mControllerP1/P2` binding drives the two navis. This proves the lockstep core before any mode work.

It is not free:
- The lobby has to inject the stage and handicaps instead of running `vsSelect2D`/`challengeSelect2D` (§2.3).
- The settings lock has to be written (§2.6).
- The `hold_game` redraw path has to be disabled (§2.6).

### Tier 1: launch

| Mode | Shapes at launch | Shapes later |
|---|---|---|
| 2v2 Battle | 1v1, 2v2 | — |
| Heave! | 1v1, 2v2 | 1v1v1 (Tri-Onion) |
| Bounty Swap | 1v1, 2v2 | 4-player FFA |
| Daily Dig + Lifeline | 1–4 co-op | — |

#### 2v2 Battle. F 6, fun 7 (new entry, from the critique)

- **Pitch:** stock Battle with two captains per side, sharing one Onion and one colour.
- **Loop:** the stock marble and yellow-marble rules. The team's captains split the work: one escorts the marble, the other raids or collects cards. Pikmin belong to whoever plucked them (owner byte). A whistle can take a teammate's Pikmin as a deliberate handoff.
- **Why fun:** it is the cheapest true 4-player mode. Roles appear right away, and it shows that the N-captain engine works.
- **Content:** a team scoreboard strip and per-captain name tags. Stock arenas, plus 2v2 cards (below).
- **Engine hooks:**
  - reuses every Red/Blue onyon, PATHFLAG and score path;
  - needs `alloc(4)`, the owner byte, N cameras and `mLoseCauses` indexed by team (§3).
- **Net notes:** a team loses when both of its captains are down, or when its Pikmin are extinct.
- **Effort:** M after M5.3a.

#### Heave! / Tri-Onion Scramble (one base, two rulesets). F 7 (1v1) / 4 (3-team), fun 8 / 8

- **Pitch:** one huge pellet in the middle with Onions around it. The team with the most weight on it pulls it home. The 3-team ruleset adds home marbles, elimination and feral Pikmin.
- **Loop:**
  - *Heave:* steer the big pellet by carrier majority, bitter-spray rival carriers into seeds, and bank small pellets for reinforcements. Best of 3. At the time limit, the tiebreak is a weighted distance to each Onion.
  - *Scramble:* bank 4 yellow marbles, or haul a rival's marble into your Onion.
    - The loser's Onion goes dormant.
    - The loser's Pikmin become free agents that any captain can whistle.
    - The loser's captains become scavengers who can still play cards.
- **Why fun:** easy to read from anywhere. Anchor, raider and forager roles appear naturally. With 3 teams, the trailing teams gang up on the leader, which is the comeback mechanic.
- **Content:**
  - 2- and 3-Onion arenas (caveinfo plus `_edit.txt` from existing units) and a centre fixed node;
  - a heavy "tug" pellet (a high-weight otakara);
  - a tiebreak bar and a best-of-3 FSM;
  - a Turncoat card.
- **Engine hooks:**
  - majority-carrier goal choice across 3 colours already exists (`pelletMgr.cpp:1943-1980`);
  - combat (`pikiAI.cpp:69-80`);
  - bitter spray (`interactPiki.cpp:233-240`);
  - the onyon suck handler (`onyonMgr.cpp:484-520`);
  - 3-team fixes in §3.5. Free agents need team = none in the whistle filter (`interactPiki.cpp:58-65`).
- **Net notes:** pellets switch between rigid-body and simple physics by LOD (`pelletMgr.cpp:1322-1342`), so checklist item 5 is mandatory.
- **Effort:** Heave 1v1 and 2v2 is M. 3-team is L and follows after launch.

#### Bounty Swap. F 7 (1v1) / about 5 (4-team), fun 8

- **Pitch:** carry a slain enemy into your Onion, and a copy of it hatches next to a rival's Onion.
- **Loop:**
  - Kill wild enemies in tiers and deliver the carcasses for seeds plus a "send".
  - Sends target the leader, are tinted with your colour and despawn after 50 s.
  - Last team standing wins. At 6 minutes, the team with the most Pikmin wins.
- **Why fun:** every kill is a counterattack, one economy covers attack and defence, and sends aimed at the leader self-correct.
- **Content:** a send table (species → tier), tiered-spawn arenas, and an "incoming bounty" ping.
- **Engine hooks:**
  - carcasses go through onyon suck and `PelletDead` (`pelletMgr.cpp:619`). In VS they fall through the VS-only block to the generic `PelletType::Carcass` branch (`onyonMgr.cpp:573-577`, `mToBirth += max`), because `:524-537` skip only the poko and Challenge paths;
  - the roster is registered via `GameMessageVsAddEnemy` (`baseGameSection.cpp:2277-2297`) and spawned with `TekiMgr::birth(idx,pos,timed)` (`vsTekiMgr.cpp:59`);
  - per-mode heap via the `ENEMY_HEAP_SIZE_VS` pattern (`generalEnemyMgr.cpp:756-760`);
  - spawn point from `getItemDropPosition` (`RandMapMgr.cpp:201`), leader from `calcVsScores` (`vsGameSection.cpp:1373`).
- **Net notes:** the TekiMgr node index is the registration order, so every peer registers in the same order. Enemy targeting depends on the `getActiveNavi` audit, and enemy behaviour on observer LOD.
- **Effort:** M for 2 teams. 4-team FFA is L.

#### Daily Dig, with the Lifeline rule. F 6, fun 8 (Dig); F 6, fun 7 (Lifeline) (merges #21, #2, #1)

- **Pitch:** each day, everyone gets the same seeded cave, starting squad and mutator. Dive co-op and post a score. A downed captain becomes a rescue, not a game over.
- **Loop:**
  - 5 floors with per-floor timer extensions. Score = pokos + time bonus − Pikmin lost.
  - The geyser banks the run early. The hole needs a ready-check.
  - **Lifeline:** at 0 HP a captain enters the lie-down pellet state. Pikmin carry the body to the pod or an Onion, or a teammate revives them with nectar or ultra-spicy spray. Bleed-out is 60 s. The run is lost only when every captain is down at once.
  - A weekly 15-floor "Deep Dig". The 10-floor, crew-scaled "Daily Descent" variant comes as a follow-up.
- **Why fun:** a shared seed fuels route arguments and gives a daily reason to come back. Lifeline turns failures into hero moments.
- **Content:** a pool of about 30 remixed caveinfos (overlay data), a daily config endpoint, a leaderboard screen, and a route ghost on the results map.
- **Engine hooks:**
  - seeded `srand` before `createRandomMap` (`baseGameSection.cpp:2271`);
  - the Challenge multi-floor flow: `goNextFloor`/`openCaveMoreMenu` (`vsGameSection.cpp:500-560`), LoadState (`vsGS_Load.cpp:72-95`), the countdown with extensions (`vsGS_Game.cpp:276-310`);
  - `addChallengeScore` (`vsGameSection.cpp:833`) and a per-floor `getCaveFilename()` (`BaseGameSection.h:129`);
  - shared sprays (`vsGS_Game.cpp:1041-1053`), with the captain-down-ends-run rule lifted (`vsGS_Game.cpp:817-829`);
  - Lifeline: lie-down is already live outside story mode (`naviState.cpp:470-478`), through `becomePellet` (`naviState.cpp:3553-3585`). Redirect `onOrimaDown` (`vsGS_Game.cpp:868-893`) and `informOrimaDead` (`naviMgr.cpp:297`).
    - **Gap:** the onyon suck handler has no case for a navi pellet. `onyonMgr.cpp:484-520` handles only marbles, yellow bedama and cherries. Delivering a body to the pod or an Onion is new code: a `GameMessageCaptainRescued` handler and the redeploy.
  - Separate multiplayer highscores already exist (`vsGS_Result.cpp:71-76`).
- **Net notes:** for the leaderboard, the server replays the input log. That needs the headless runner (item 1) and the vendored libm (item 11).
  - Until both exist, scores are tagged "unverified".
  - After that, they are verified on x86_64. arm64 verification waits for item 14.
- **Effort:** Dig M. Lifeline M. Crew scaling L.

#### Cheap new content at launch

| Content | Delivered as |
|---|---|
| New VS cards: Turncoat, Anchor, Quake, Lockdown, Decoy, Rally (teammate speed boost) | One `eCardType` entry, one texture and one case each in `usePlayerCard` (`vsCardMgr.cpp:240-460`, textures at `:30-35`). Weights become data (`vsCardMgr.cpp:43-48`). |
| New Challenge-style missions | A co-op mission list as an alternate `stages.txt` (≥30 entries: `CHALLENGE_COURSE_COUNT = 30`, `BuildSettings.h:14`; `vsGS_Title.cpp:110-113` loops over 30 and `getStageData` asserts at `vsStageData.cpp:101`) over the overlay, the same way `KFesVersion` swaps lists (`vsGameSection.cpp:447-449`) |
| New treasure sets and the heavy tug pellet | `pelletlist_us.szs` entries through in-archive overrides (§7a), reusing existing models |
| New mutators | Overlay data (§6) |
| New arenas | caveinfo plus `_edit.txt` built from existing units |

### Tier 2: next

| Mode | F / fun | Pitch and loop | Why fun | Content | Engine hooks | Shapes | Effort |
|---|---|---|---|---|---|---|---|
| **Tri-Onion Scramble** (#9) | 4 / 8 | The 3-team Heave ruleset above | Alliances; feral windfall; scavengers stay in the game | 3-Onion arenas | §3.5 | 1v1v1, 2v1v1 | L |
| **Onion Heist** (#14) | 5 / 8 | Capture the flag where treasures sit in your Onion's vault. Rob a vault by holding the whistle with 10+ Pikmin under it for 4 s, and the Onion spits out its best treasure. Richest team at 7 min wins. | No lead is safe; the trailing team has the most to steal | Hoard list, rob meter, Lockdown/Decoy cards, symmetric arenas, treasure sets | Eject via `PELSTATE_Return`/`ScaleAppear` (`vsCardMgr.cpp:325-360`, `onyonMgr.cpp:484-520`); log on `VsGetOtakara` (`onyonMgr.cpp:511-513`); 32-slot otakara pool (`PelletOtakara.h:9`); per-team PATHFLAGs | 1v1, 2v2 | L |
| **Sprout Throne** (#10) | 7 / 7 | King of the hill. The team with a clear majority of its Pikmin in the ring scores. The ring jumps every 45 s, with a ghost preview. | Easy to read; holder and ferrier roles | Ring effect, score bar, Anchor/Quake cards | Per-tick `pikiMgr` count in `update`; `getItemDropPosition`; kill recharge (`onyonMgr.cpp:1151-1164`); catch-up cherries (`vsGameSection.cpp:1168-1250`) | 1v1, 2v2, 4 FFA | M |
| **Last Light** (#6) | 6 / 7 | Hold-the-pod survival: 8–12 waves. Push out between waves for beacon parts; sky-drop reinforcements arrive. | Flank roles; greed vs. safety | Wave tables as data, spawn markers, dusk lighting via caveinfo | `TekiMgr::birth` waves; `createFallPikmins` (`vsGameSection.cpp:709-744`); `checkPikminZero` (`vsGS_Game.cpp:607`); per-mode enemy heap | 1–4 co-op | M |
| **Delve Derby** (+ Sundown Scramble as an overworld variant) (#11, #17) | 4 / 8 | Competitive seeded dive. The first jump into the hole starts a 30 s collapse for everyone, and treasure still in transit is lost. | The hole is a bluff button; carrier ambushes | Race-density caveinfo, collapse HUD, poko ledger | Combines the Challenge multi-floor flow with VS teams (untangle the `isVersusMode` branches); majority-carrier theft (`pelletMgr.cpp:1943-1980`); the overworld variant needs `createManagers(2)` (`baseGameSection.cpp:2230-2237`) and TimeMgr (`timeMgr.cpp:25-70`) | 1v1, 2v2 | L / XL |
| **Ship's Navigator** (#7) | 6 / 6 | Seats 5–6 play as the ship overhead: radar, waypoints and drops paid from a fuel meter. Layers onto Daily Dig and Last Light. | A role for extra or less confident friends | RmlUi radar and map, drop pod, new card types | The navigator has no navi and no pad port. Their actions are host-stamped `{tick, eventId, arg}` events (§2.9) applied via the card switch (`vsCardMgr.cpp:240-460`) and `createFallPikmins`. The radar uses the presentation RNG. | +1–2 seats | L |
| **Titan Raids** (#5), first as boss floors in Daily Dig | 4 / 7 | Scaled bosses with add waves and an enrage timer; lure, DPS, warden and medic roles | Aggro and kiting matter with 4 captains | Boss parms through in-archive overrides (§7a), phase controller, raid HUD | Adds via `TekiMgr`; boss `getActiveNavi` callers retargeted; `EB_AttackingNavi` sized to N; force culling off for active bosses (`enemyBase.cpp:1831-1838`) | 2–4 co-op | L |
| **Hocotate Crew: co-op story** (#0, **M6**) | 2 / 8 | The full story with 2–4 captains: shared day, shared Onions, squad cap from §3.4, host save | Pikmin 2's time management done in parallel and planned out loud | Tinted captains 3–4, crew HUD, map vote, daySpeed in the session config | `onPlayerJoin` seam (`baseGameSection.cpp:1032-1039`); President option (`naviMgr.cpp:133-135`, `:247-248`); all of §3; save, THP and overworld barriers (§4 items 7, 10, 12) | 2–4 co-op | XL; own milestone (§8) |

### Tier 3: experimental

| Mode | F / fun | Pitch | Why it waits | Effort |
|---|---|---|---|---|
| **The Beast Below** (#13) | 3 / 8 | One player drives a boss-sized enemy while the captains haul 5 treasures out of its lair | Needs a "possessed" FSM per species, forced culling-off, and a beast camera that is a sim camera; `TekiMgr` (`vsTekiMgr.cpp:22`, `:59`) | XL |
| **Salvage Contracts** (#8) | 3 / 7 | Co-op contract campaign; a container for Bucket Brigade relays (#3), Swarm Conductors puzzle floors (#4) and a raid finale | A wrapper over Tier 1–2 modes; 30-slot stage list (`CHALLENGE_COURSE_COUNT = 30`, `BuildSettings.h:14`; loop at `vsGS_Title.cpp:110-113`; assert at `vsStageData.cpp:101`; `StageList::read` at `vsStageData.cpp:80-92`; `gamePlayCommonData.cpp:444`) | XL |
| **Mole in the Colony** (#15) | 3 / 7 | Shared-team co-op with a secret saboteur and a vote at the end of each floor | Lockstep cannot hide the mole from memory; needs 4–6 captains; the meeting is a synced pause | L |
| **Fuse Frenzy** (#26), as a mutator or minigame | 4 / 6 | Bomb-rock hot potato | The most latency-sensitive mode (precise throws); in the stock game only Yellows carry bombs | M |

### Idea bank

| Idea | Disposition |
|---|---|
| Bucket Brigade relay hauling (#3) | A contract type inside Salvage Contracts |
| Swarm Conductors split-party puzzles (#4) | A few special floors; needs new geometry |
| Spore Tide royale (#12) | Deferred: 8 sim cameras, AILOD beyond 4 bits (`AILOD.h:12-15`), more than 4 pad ports |
| Peanut Gallery spectator meddling (#20) | Event voting becomes the ghost and spectator role (§6); betting is cut |
| Onion Circuit party board (#19) | Becomes "party playlists" (§6) that chain real modes |
| Sprout Commons persistent garden (#23) | Deferred: no world-state serializer exists |
| Hocotate Exchange trading and museum (#24) | Last priority: a backend with no swarm play |
| Unit Loom arena editor (#25) | A year-two tool that writes overlay mods (§7a) |
| Onion-colour economy race (judge gap) | Open design: race to grow a target colour mix |
| Ranked 1v1 (judge gap) | Heave 1v1 or Bounty Swap 1v1 with a rating, once replays verify results |

## 6. Shared systems

| System | Design | Code |
|---|---|---|
| **Mutators** (Odd Seasons) | A deck of rule cards. The host draws and each player can veto one. Applied only at the start barrier. Each mutator is overlay data (`pikiParms`, `naviParms`, `time.ini`, `PikiContainer`, card weights) or a `PC_ONLY` toggle. The active set is part of the content hash. Seasons bundle mutators with caves later. | `pikiMgr.cpp:164`, `naviMgr.cpp:174`, `gameSystem.cpp:72`, `vsStageData.cpp:129-157`, `vsCardMgr.cpp:43-48`, `MapCreator.cpp:20`, `port/include/p2_config.h:20-28`. Enemy-stat mutators need in-archive overrides (§7a). Giant Pikmin and gravity are cut at first. |
| **Handicaps** | Per player in every mode: starting Pikmin, captain HP, spray count. Part of `SessionConfig`. | stock VS handicap = ×5 Pikmin (`vsGameSection.cpp:771-773`) |
| **Seeded daily runs** | The server serves `{date, seed, caveinfo pool ids, mutator}` | `srand` before `baseGameSection.cpp:2271`; `borealis::http` |
| **Quick-chat, emotes and pings** | Host-relayed and outside the sim. A wheel of preset lines ("Need help", "Incoming", "Bank it"). World pings carry quantized map coordinates and draw as overlays. Voice is left to Discord. | RmlUi overlay (`port/launcher/overlay.cpp`) |
| **Spectating** | Receive the bundle stream and simulate locally. A camera picker over the N sim cameras (read-only) or a free cam. Eliminated players become spectators. | §2.8 |
| **Ghost role** | Eliminated players and spectators vote on a gallery event every 45 s (cherry rain, seed shower, a Bulborb wakes). The host stamps the winner as `{tick, eventId, arg}`. Spectators can also make predictions (who wins, first marble) with no betting. | `dropCard` (`vsGameSection.cpp:1263`), `createFallPikmins` (`:709`), `TekiMgr::birth` (`vsTekiMgr.cpp:59`) |
| **Pikmin gift** | An eliminated player can send one sky-drop of 5 Pikmin to their team, once per match. | `createFallPikmins` (`vsGameSection.cpp:709-744`) |
| **Replays** | An input log with a header `{build_hash, content_hash, seed, SessionConfig, mutators}`. Playback runs the same seam with the recorded bundles. Also used for desync repro, late-spectator catch-up and leaderboard verification. | `JUTGamePad.cpp:142`; headless runner (§2.1) |
| **Post-match stats and highlights** | Built from a headless re-run of the replay: MVP, carries, Pikmin lost, cards used, longest haul. Top 3 moments are marked by tick. | replay plus GameMessage counters (`pelletMgr.cpp:619`, `onyonMgr.cpp:511`). `GameMessageVsPikminDead` is sent only in Challenge mode (`piki.cpp:204-206`), so "Pikmin lost" in VS and new modes needs that condition widened under `TARGET_PC`, or a new message. |
| **Killcam** | When a captain goes down, an optional local instant replay of the last 5 s from a local camera. Render-only. Built by re-simulating from the input log in a second process, or skipped until snapshots exist. | presentation only |
| **Rematch** | Results screen: Rematch, or Rematch with sides swapped. The same session goes straight back to the start barrier with a new seed. | §2.3 |
| **Party playlists** | The host queues 3-minute rounds of different modes (Heave, Throne, Fuse Frenzy) with a running scoreboard. Each round is its own lockstep session. | §2.3 |
| **Progression and cosmetics** | A local profile plus the lobby server: captain tints, titles, radar icons. Render-only, so nothing changes collision or scale. | tint in the render path only |

## 7. Mods and content pipeline (decided)

### 7a. Data-mod overlay, built before netcode

The new modes are mostly data: arenas, seeded caves, placements, missions and rule variants. This overlay is their content pipeline. Modes can be prototyped offline in local split screen before netcode exists.

**Hook.**
- The game's `DVDOpen`, `DVDConvertPathToEntrynum` and `DVDOpenDir` are macro'd to `p2_` wrappers (`port/include/p2_sdk_compat.h:163-173`: declarations `:163-166`, macros `:170-173`).
- The wrappers live in `port/sdk/dvd_fixes.cpp`.
  - They normalize the path with `sdkPath`: "a/b//c/d" becomes "/c/d" (`port/sdk/dvd_fixes.cpp:5-18`).
  - They then call Aurora (`port/sdk/dvd_fixes.cpp:22-25`).
- Every loose-file loader goes through them, including `JKRDvdRipper::loadToMainRAM`, `DVDConvertPathToEntrynum` (`baseGameSection.cpp:665`) and `JKRMountArchive`.
- The overlay is added in `port/sdk/dvd_fixes.cpp`, or in a neighbouring `port/sdk/dvd_overlay.cpp` that it calls:
  - After `aurora_dvd_open` (`port/main.cpp:945`), the mod loader resolves the enabled mod set.
  - It registers each winning file with Aurora's overlay API (`aurora_dvd_overlay_callbacks` / `aurora_dvd_overlay_files`, `extern/aurora/include/aurora/dvd.h:99`, `:116`), using callbacks backed by `SDL_IOFromFile`.
  - Lookups are case-insensitive (`extern/aurora/lib/dolphin/dvd/fst.cpp:~293`). That matters because the game opens both `/user/abe/vs` and `/user/Abe/vs`.
  - New files get fresh EntryNums. Overlaid files are read through `CommandDataOverlay` (`extern/aurora/lib/dolphin/dvd/dvd.cpp:76-91`, `:1081`).
  - The file set can be replaced at runtime (`extern/aurora/include/aurora/dvd.h:105`), so the lobby can swap mod sets at a section boundary.
- **As built (M5.2):** the overlay is a new module, `port/mods/` (scan, manifest, load order, validators, hashes, the loose overlay and the archive registry; pure parts in `port/mods/pure/`). `port/sdk/dvd_fixes.cpp` is unchanged: Aurora's overlay sits below `DVDOpen` and `DVDConvertPathToEntrynum`, so the existing `p2_` wrappers already reach it. The overlay serves each winning file from port-owned memory, not through `SDL_IOFromFile`. See [online/M5_2_MODS.md](online/M5_2_MODS.md).
- Everything is in `port/` (and the existing Aurora overlay API). **There is no new upstream diff:** no edit to `src/` or `include/`. (Those trees already carry the port's existing `PC_ONLY` hooks, e.g. the RARC conversion calls below.)

**What it covers on day one:** loose disc files.

| Target | Path |
|---|---|
| Cave info (85 files) | `/user/Mukki/mapunits/caveinfo/*.txt` |
| Cave unit lists | `/user/Mukki/mapunits/units/*.txt` |
| VS stage list (≤10 shown on the stock screen) | `/user/abe/vs/stages.txt` |
| VS edit layouts | `/user/Abe/vs/vs_N_*_edit.txt` |
| Challenge list (≥30 entries, `BuildSettings.h:14`, `vsGS_Title.cpp:110-113`) | `/user/Matoba/challenge/stages.txt` |
| Overworld gen files | `/user/Abe/map/<area>/{defaultgen,plantsgen,initgen}.txt`, `nonloop/*`, `loop/*` |
| Tuning | `/user/Abe/piki/*Parms.txt`, `/user/Abe/item/*Parms.txt`, `/user/Abe/time/time.ini` |
| Textures | those shipped as loose disc files |
| New mode data | `/user/mppm/<mode>/...` (new files with fresh EntryNums) |

**Second step, still inside M5.2: files inside `.szs` archives.** Examples are `enemyParms.szs`, `pelletlist_us.szs` and archived textures.
- **Approach: splice in memory inside the existing RARC conversion.** It is all in `port/`, with **no new upstream diff**. No `JKRArchive::getResource` hook is needed (`JKRArchivePub.cpp:164`, `:179` stay untouched).
- **The mount hook already exists.** Every archive type calls `PC_ONLY(p2HostTables())` once its table pointers are set and before anything reads the file table (`JKRMemArchive.cpp:170`, `:190`; `JKRDvdArchive.cpp:99`; `JKRAramArchive.cpp:117`; `JKRCompArchive.cpp:123`, `:169`). Declared at `include/JSystem/JKernel/JKRArchive.h:190-195`; body in `port/assets/rarc.cpp:37-89`. These lines are already in HEAD.
- **`p2HostTables` already builds the file table the game reads.** It allocates a host `SDIFileEntry[]` (`port/assets/rarc.cpp:78-79`), copies each entry with `mData = nullptr` (`:80-87`) and points `mFileEntries` at it (`:88`).
- **v1, replace existing entries.** For an overridden entry, `p2HostTables` sets `mData` to the mod's bytes, `mSize` to its length, and clears the compressed and YAZ0 attributes (`mFlag &= ~((0x04|0x80) << 24)`, `JKRArchive.h:53-55`). Every `fetchResource` returns a preset `mData` as is (`JKRMemArchive.cpp:215`, `:236`; `JKRDvdArchive.cpp:155`, `:186`; `JKRAramArchive.cpp:187`, `:218`; `JKRCompArchive.cpp:221`, `:268`), and `getResSize` returns `mSize`. No game code calls `removeResource`, so an override is never undone.
- **v2, add new files.** Copy the directory and string tables, move the target directory's file range to the end of the host table followed by the new entries, and update `mFirstIdx`/`mNum` and the counts in `mDataInfo`. Old slots stay as duplicates, so every existing index and id stays valid.
- **Finding the archive.** Path mounts know their disc path through `DVDConvertEntrynumToPath(mEntryNum, …)` (`mEntryNum` is set before `open()`, `JKRArchivePri.cpp:33`). Buffer mounts do not: `enemyParms.szs` is loaded through ARAM and mounted with `JKRMountArchive(void*)` (`generalEnemyMgr.cpp:775-779` → `loadResource.cpp:154`, `:200-201`). So archives are keyed by **content**: on entry to `p2HostTables` the info block is still raw disc bytes, and its hash is matched against the same hash computed by the mod loader for each `archives/<disc path>` (read through Aurora `DVDOpen`/`DVDRead`, Yaz0-decoded with the existing `JKRDecompress`). `mEntryNum` breaks ties for path mounts. Re-mounts of an already converted buffer (`rarc.cpp:73-75`) look up a port-side `mDataInfo → override set` map. The same scheme covers nested archives.
- **Yaz0 needs no hook.** Whole archives are decompressed before `p2HostHeader`/`p2HostTables` run (they act only on data that starts with "RARC", `rarc.cpp:31`).
- **Memory.** `EMM_Mem` archives (both targets) never free `mData`, so their override buffers can be port-owned. Dvd, Aram and Comp archives allocate overrides from `mHeap`, because `JKRCompArchive::removeResourceAll` frees `mData` there (`JKRCompArchive.cpp:316`). Add the missing null check after the allocation at `rarc.cpp:79`.
- The override registry and the disc-side hashing go in `port/sdk/dvd_overlay.cpp`, next to the loose-file overlay. *(As built: `port/mods/archive_mount.cpp` and `port/mods/pure/archive_index.cpp`. Re-mounts of a converted buffer find their set through an id byte in the info block's padding, not a pointer map; adding files (v2), nested archives and the repack fallback are deferred. See [online/M5_2_MODS.md](online/M5_2_MODS.md).)*
- **Fallback, for heavy edits or debugging:** repack the archive at startup into an uncompressed RARC under `<userdir>/mods/.cache/<content_hash>/` and serve it as a whole-file Aurora overlay (`aurora_dvd_overlay_files`, `extern/aurora/include/aurora/dvd.h:116`). It works for every mount mode with no hook, but needs a full RARC writer, cache invalidation, and more memory on the ARAM path.
- Mods use it through an `archives/` subtree, and the content hash treats those files like any other.

**Folder layout.**

```
<userdir>/mods/
  load_order.txt            one mod id per line; top = lowest priority, bottom = highest
  <mod-id>/
    mod.json                manifest
    files/                  mirrors the disc root
      user/Mukki/mapunits/caveinfo/dig_daily_01.txt
      user/abe/vs/stages.txt
    archives/               in-archive overrides (second step)
      enemy/parm/enemyParms.szs/<inner path>
```

The launcher lists mods, toggles them and edits `load_order.txt`.

**Manifest (`mod.json`).**

```json
{
  "id": "heave-arenas",
  "version": "1.2.0",
  "name": "Heave! arenas",
  "mppm": ">=0.6",
  "files": [
    "files/user/Mukki/mapunits/caveinfo/heave_01.txt",
    "files/user/abe/vs/vs_11_heave_edit.txt"
  ]
}
```

- `id`: `[a-z0-9-]+`, unique.
- `version`: semver.
- `mppm`: the minimum build.
- `files`: the complete list of shipped files. Files on disk that are not listed are ignored. A listed file that is missing fails the mod.

**Load order and conflict rule.**
1. Read `load_order.txt`, and skip disabled or missing mods.
2. Normalize each listed path: strip `files/`, apply the `sdkPath` rule, use forward slashes, lowercase.
3. Walk the mods in order. A later mod's file replaces an earlier one with the same normalized path (**last wins**).
4. The launcher shows every conflict as "X overrides Y". File contents are never merged.
5. Validate known formats before mounting:
   - VS `stages.txt`: at most 10 entries when the stock select screen is used (`vsSelect2D.cpp:728-736`). Modes launched from the lobby skip that screen.
   - Challenge `stages.txt`: at least 30 entries (`CHALLENGE_COURSE_COUNT`, `BuildSettings.h:14`; `vsGS_Title.cpp:110-113` reads 30, and `vsStageData.cpp:101` asserts).

**Content hash.**

```
content_hash = SHA-256(
  "mppm-content-v1" ‖ build_hash ‖
  for each enabled mod, in load order:
      id ‖ version ‖
      for each listed file, sorted by normalized path:
          normalized_path ‖ size ‖ SHA-256(file bytes)
)
```

- Load order is part of the hash, because it decides which file wins.
- With no mods enabled, the hash covers only the build, so stock peers match.
- The hash is computed once at mount time and shown on the launcher's about/status line. *(As built: shown in the Mods window's Overview; with no mod on it is computed lazily, when a recording, the harness or the Mods window needs it. Deviation D17 in [online/M5_2_MODS.md](online/M5_2_MODS.md).)*

**Offline prototyping.** Once the overlay exists, Heave, Bounty Swap and Daily Dig can be built as overlay data plus C++ rules. They can be played in local split screen on the 2-captain engine between M5.2 and M5.3.

### 7b. Content hash in the lockstep handshake (part of the netcode milestone)

Lockstep needs identical content on every peer.
- At ready-up, each peer sends `{build_hash, content_hash, enabled mod list (id, version)}`.
- The session starts only if every `build_hash` and `content_hash` matches the host's. This sits next to the build hash and RNG seed already required by PORT_PLAN.md:83.
- On a mismatch, the lobby shows the mod-list diff.
- The same `content_hash` goes into desync reports (§2.7) and replay headers (§6). A replay refuses to play against different content. *(As built in M5.2: the replay gate compares the mods hash, so a different build is only a warning and the determinism corpus still replays across builds; the content hash is stored in the header for M5.3b. Deviation D7 in [online/M5_2_MODS.md](online/M5_2_MODS.md).)*
- **Follow-up, not launch scope:** a mismatched peer downloads the host's mod pack before the start barrier. That needs bounds-checked parsers first, because the game's parsers assert on bad input (`vsStageData.cpp:101`).

### 7c. Code mods and scripting API, deferred

This is deferred until **after** the first new modes ship, for three reasons:
- A script VM would have to be bit-deterministic across x86 and ARM: no wall clock, no host RNG, no FP drift.
- Native plugins cannot guarantee that.
- Running code from strangers' lobbies is a security risk.

The plan is to build the first modes in C++ plus overlay data. Then the hooks those modes actually needed become the API:
- win conditions;
- scoring from GameMessage events;
- spawn rules (`TekiMgr` waves, drops);
- HUD widgets.

## 8. Milestones

These extend PORT_PLAN.md and follow the decided order:
1. determinism and the input seam;
2. the data-mod overlay;
3. N captains, then netcode;
4. new modes;
5. the scripting API.

M6 (co-op story) is the last item of step 4 and has its own milestone. M7 (mobile) is unchanged.

**Test matrix** used by the "done when" criteria:
- **A:** two x86_64 Linux machines with different CPU vendors and generations (one with FMA, one without, or Intel vs AMD).
- **B:** A plus one Windows x86_64 machine, once item 11 lands.
- **C:** B plus arm64 macOS, once item 14 lands.

| Step | # | Milestone | Done when |
|---|---|---|---|
| 1 | **M5.1a** | Harness, seed, input seam | `p2_net_exchange` hook at `JUTGamePad.cpp:142` (a no-op offline). Session seed. Input-log record and playback. Per-30-tick checksum. Headless tick runner. Reset and long-press off. **Latency probe** (`--latency-probe`, port-only): on a pad button-down edge it draws a white corner patch in the first frame that shows the effect, tagged by channel (sim tick or local prediction). It is measured two ways: press-to-photon with a rig (a microcontroller posing as a USB pad presses the button and times a photodiode on the patch), and press-to-present in software for CI (SDL event timestamp to the patch frame's present). A recorded 10-minute 2P Battle log replays on one machine to identical checksums, twice in a row, headless. The probe records the offline baseline (expected ≈ 107 ms median, turning, 60 Hz fullscreen). |
| 1 | **M5.1b** | Fix the divergences | Split RNG streams with the sim-stream assert. Effects RNG audit. Inline-DVD load barrier. Camera stepped in the tick and a fixed sim frustum. `hold_game` hard-disabled online. Settings lock implemented. Clock and audio coupling audit. Vendored libm. Zero-on-alloc in net builds. Latency items 1 and 4 (§2.10): late sampling, and the camera step at the start of `doUpdate` paired with update-before-draw in `BaseGameSection`. The same log matches on matrix A, and a desync CI job is running (§9). The same log gives identical checksums with draw-first and update-first order. Offline press-to-photon on the probe rig: median ≤ 55 ms for turning and cursor (target 47), ≤ 55 ms for the camera, on the reference spec at 60 Hz fullscreen. |
| 2 | **M5.2** | Data-mod overlay + manifest + content hash | `mods/` overlay mounted through `port/sdk/dvd_fixes.cpp` and the Aurora overlay API, with manifest, load order, last-wins conflicts and validators. Launcher mod list. `content_hash` shown. A mod that replaces a VS arena and a caveinfo loads offline. In-archive overrides through the `p2HostTables` splice in `port/assets/rarc.cpp` (no new `src/` or `include/` edit) for `enemyParms.szs` (buffer mount) and `pelletlist_us.szs` (path mount). The same replay with the same mod gives the same checksum. *(As built: the overlay lives in `port/mods/` and `port/sdk/dvd_fixes.cpp` is unchanged, because Aurora's overlay sits below the `p2_` wrappers; files are served from port-owned memory. Deviations in [online/M5_2_MODS.md](online/M5_2_MODS.md) section 12.)* |
| — | *(between 2 and 3)* | Local mode prototypes | Heave 1v1, Bounty Swap 1v1 and Daily Dig run in local 2P split screen from overlay data plus C++ rules. Not shipped. |
| 3 | **M5.3a** | Lift the 2-captain limit / local N-player | §3.1–3.6: `alloc(N)`, PLAYERMODE enum, sized arrays, owner and team bytes, nearest-navi audit of the 47 `getActiveNavi` sites, N cameras in the tick, observer-based LOD, Pikmin cap decision, heap budget. A headless 4-captain 2v2 Battle and a 4-captain co-op dive driven by recorded input logs run 10 minutes without asserts, and their checksums match on matrix A. **Performance budget:** 4 captains, 100 Pikmin and a full VS roster simulate at ≤8 ms per tick on the reference spec (4-core x86_64 from 2018, integrated GPU) while rendering at 60 fps. `FourSplitter` and local 4-way split screen are optional and do not block this milestone. |
| 3 | **M5.3b** | Lockstep netcode with the content hash in the handshake | First task: the libjuice-against-Cloudflare-TURN spike (§2.11). ENet transport on a net thread with immediate flush, duplicate send and throttling off; libjuice NAT layer; `mppm-server` on Fly.io with join codes and Cloudflare TURN credentials; `online.server_url`; the `docker compose` self-host stack. Star topology with forward-on-complete. Per-sender delay `d_i` (floor 1, no +1) and phase-slew time sync (§2.10 items 2–3). Stall loop merged with late sampling, waiting for T+1. Own-captain, cursor and whistle prediction with local cues (item 5). Latency options (item 6) may land here or later and do not block. The handshake checks `build_hash` + `content_hash` + seed. Desync checksum, report and abort. Disconnect becomes neutral input. Spectators. Packet validation (§10). Done when: Tier 0 (online 2P Battle and Challenge, with the lobby injecting the stage) plays 30 minutes with no desync on matrix A over a 100 ms simulated WAN, and then 4 peers play 2v2 Battle for 30 minutes; one session runs through Cloudflare TURN and one through a self-hosted compose stack; the relayed share of peers is measured. **Latency, measured with the probe rig on every peer under netem:** median press-to-photon ≤ 85 ms on LAN (target 80), ≤ 120 ms at 30 ms RTT (target 114), ≤ 155 ms at 80 ms RTT (target 147); own-captain and cursor on the prediction channel ≤ 55 ms at 80 ms RTT; phase error ≤ 2 ms p99 one second after the start barrier; stall rate < 2% at the chosen `d_i`; checksums identical with prediction on and off. |
| 4 | **M5.4** | New modes as C++ rules + overlay data | Each mode is done when it has: its listed player shapes supported, a replay that re-verifies headless, its HUD and results screen, at least 3 arenas or a cave pool, and one 30-minute online session with no desync. Order: 2v2 Battle → Heave 1v1/2v2 → Bounty Swap 1v1/2v2 → Daily Dig + Lifeline (1–4). With them: mutators, replays, rematch, stats, quick-chat, and the new cards and missions. Alongside, the hybrid mesh fast path (§2.10 item 7), done when a 4-peer wired session at 30 ms RTT runs at `d_i = 1` with a stall rate < 2% and median press-to-photon ≤ 85 ms, and an injected equivocating peer is detected and named. **Tier 1 = "first new modes shipped".** Then Tier 2: Tri-Onion, Onion Heist, Sprout Throne, Last Light, Ship's Navigator, Delve Derby. |
| 4 | **M6** | Hocotate Crew co-op story | Prerequisites: M5.3b; save blob and scratch card (item 12); THP end barrier; overworld `TimeMgr` with daySpeed in `SessionConfig`; day-transition load barrier; `onPlayerJoin` at the morning landing; squad cap from §3.4. Done when 2–4 players finish day 1 through day 3 online with no desync, and the host's save advances correctly. |
| 5 | **M5.5** | Scripting/mod API shaped by M5.4 | Starts after Tier 1 ships, and may overlap M6. A deterministic, sandboxed API covering the hooks Tier 1 modes used (win conditions, scoring, spawn rules, HUD widgets). One Tier 1 mode is re-expressed through it with an identical replay checksum. Mod-pack download for mismatched peers. |

**M5.1a status:** the harness, its flags, file formats, deviations from this plan and open items are in [online/M5_1A_HARNESS.md](online/M5_1A_HARNESS.md).

**M5.1b status:** the determinism harness at version 2 (tick definition, nonsim regions, formats, flags, the determinism matrix and its CI job, the latency procedure) is in [online/M5_1A_HARNESS.md](online/M5_1A_HARNESS.md); the clock, audio, effects and RNG audits are in [online/M5_1B_AUDIT.md](online/M5_1B_AUDIT.md); the vendored libm, zero on alloc, the `-Wreturn-type` audit and their test results are in [online/M5_1B_BUILD.md](online/M5_1B_BUILD.md); the map of the game-side hook sites is [online/M5_1B_HOOKS.md](online/M5_1B_HOOKS.md). **Not met yet** (open items in the harness doc): the same log on matrix A (two machines with different CPUs; the `no-fma` cell is only a stand-in), a desync CI job that has actually run (the workflow exists but is inert until a self-hosted runner is set up), the MSan or Valgrind pass of item 13, the press-to-photon rig and a real-display latency run.

**M5.2 status:** the data-mod overlay (loose files and in-archive overrides, v1), the manifest, the load order, the validators, the mods hash and the content hash, `.mpnl` version 3 with the content gate and the launcher's Mods window are described in [online/M5_2_MODS.md](online/M5_2_MODS.md), whose section 12 lists every deviation from plan 7a: D1 (module in `port/mods/`, `dvd_fixes.cpp` unchanged), D6 (files served from port-owned memory, not `SDL_IOFromFile` callbacks), one mount per process (no runtime set swap yet), D5 (disc-dependent failures drop the mod), strict paths instead of `sdkPath` normalization, the framed hash serialization with a separate mods hash, D7 (the replay gate compares the mods hash), D8 (synthetic replays not checked), D12 (re-mount id byte instead of a pointer map), D13 (eager Dvd/Aram/Comp copies), D14 (v2, nested archives and the repack fallback deferred), the own Yaz0 decoder, load order between a whole-archive replacement and inner files, D15 (stricter validators), the manifest and size limits, D16 (restart on Play) and D17 (hash shown in the Mods window, computed lazily); the guide for mod authors is [mods/README.md](mods/README.md). The done test is `tools/mods/mods_test.sh` (13 checks since fix round 3) with test mods made from your own disc by `tools/mods/make_test_mods.py`. Integrated on `online/m52-int`: all 12 checks, the selftests and `desync_ci.sh --quick` pass, with no `src/` or `include/` edit (results in the M5.2 doc). The first verification found check 4's picture rule flaky (the onion's beam phase made the stock-to-stock noise exceed the limit); fix round 1 leaves the noise pixels out of the compare, and check 4 now passes in all three phase cases. The second verification found that a loose file replacing a mixed-case disc file (e.g. `AudioRes/PSound.aaf`) could break the boot; fix round 2 registers overlays under the disc's spelling, and the identity test mod now covers it. Fix round 3 cleared the final review's minor items (Mods window scroll, warnings for a bad folder name and a missing off mod, the content hash and scans off the UI thread, Quit and Play no longer wait for a scan, a restart rule keyed on the folder's hash at boot, load order between a whole-archive replacement and inner files, docs); all 13 checks, the selftests and `desync_ci.sh --quick` pass.

**M5.3a status (part 1 of 2):** part 1 lifts the 2-captain limit for a **headless 4-captain 2v2 Battle**; the rules (D1-D16), flags, `.mpnl` version 4, the code map, the `getActiveNavi`/`mNaviIndex`/two-captain audits, the results and the known limits are in [online/M5_3A_NPLAYER.md](online/M5_3A_NPLAYER.md). Delivered: `naviMgr->alloc(N)` for a 2P Battle harness session (`--vs-stage S --vs-players 4`; teams fixed to `i & 1`, Red = captains 0 and 2, Blue = 1 and 3), `PLAYERMODE` kept as modes with captain camera ids through `p2_cam_id`, the navi-indexed arrays sized `P2_MAX_NAVI` (the team-indexed VS `[2]` state stays, D11), the player/team bytes on `Navi` and the owner/team bytes on `Piki` with the owner rules of 3.1, the nearest-navi wrap of the 12 enemy `getActiveNavi` sites (the other 35 are story, Challenge, presentation or camera sites, left for part 2 with reasons), N `PlayCamera`s stepped in the tick with N simulation viewports and observer-based LOD (2.5), a per-team death rule (a captain down with a living teammate is released; the last one loses the team the match), a `--view-captain K` presentation and colour-plus-shape captain markers, the Pikmin cap decision (stays 100, D15) and the heap figure for 2 and 4 captains. 1-2 captain play is unchanged: every hook is the stock value while a section has at most 2 captains, recordings stay version 3, and the golden checksum logs still match. The done test is `tools/net/m53a_test.sh` (10 checks: line discipline, selftests, the full golden matrix, a 10-minute 4-captain log replayed twice, captain activity, deaths, layout independence, a negative control, screenshots, perf). On `online/m53a-int` after fix round 1 every check passes (1469 s), and `desync_ci.sh --quick` passes too. Perf on this machine: with 4 captains and 100 Pikmin the simulation averages 0.385 ms per tick (p99 0.583 ms), against 0.212 ms for 2 captains, and 4 captains take 26944 bytes more of the game heap. Fix round 1 set the owner on every formation join (Pikmin whistled while flicked, blown, knocked down, doped or drowning had kept the old owner), added this paragraph, the audits and the results to the docs, measured the 2-captain heap, gave the markers a shape per team, made the card target fall back to a live teammate, hashed the card users and Pikmin owners for N > 2, and fixed two small two-captain sites (the VS end-stick loops, and Bloyster's colour and sound by team). **Moved to part 2:** the 4-captain co-op cave dive and its log, so the row's "co-op dive" criterion is open; matrix A; the perf budget on the reference machine; 3 teams and free-for-all (`--vs-players 3` is refused); per-team scores, the ranked result and the handicap array (3.6); raising the Pikmin cap and per-mode heap budgets; `FourSplitter` and local 3-4-way split; the HUD for N with the 2.5 palette and icons; presentation `getActiveNavi` callers, sound and rumble for the local captain; per-captain card slots.

**M5.3b status (part 1 of 2):** part 1 is **local only**: everything runs and is tested on one machine over 127.0.0.1, with a lobby server started by the test and delay, jitter and loss simulated inside the client (no Fly.io, no Cloudflare, no account). The flags, settings, log lines, exit codes, the impairment, the done test and the deviations (D1-D30) are in [online/M5_3B_NETCODE.md](online/M5_3B_NETCODE.md), with the parts in `server/PROTOCOL.md`, [online/M5_3B_TRANSPORT.md](online/M5_3B_TRANSPORT.md) and [online/M5_3B_LOCKSTEP.md](online/M5_3B_LOCKSTEP.md). Part 1 consists of: `mppm-server` (Rust; WebSocket plus REST, 40-bit join codes, rate limits, ICE modes none/static/shared-secret, the builtin UDP relay; `Dockerfile`, `docker-compose.yml` and `fly.toml` as files only), the lobby client over `borealis::ws`, the ENet transport on a net thread with the `enet_juice.c` shim (direct, ICE through libjuice, the builtin relay) and an in-process impairment stage, the lockstep session (star with forward-on-complete, per-sender `d_i`, network load barriers with a grid jump, phase-slew time sync, the stall loop merged with late sampling, checksums every 30 ticks with desync reports, drops to neutral input, lobby-only spectators, the handshake with `build_hash`, `content_hash` and the mod list), online 2P Battle, 2P Challenge (`--ch-stage`, `p2_net_ch_inject` behind the only `src/` edit, `vsGS_Title.cpp:192`, and `.mpnl` version 5) and 4-peer 2v2 Battle, the launcher's Multiplayer window, wait overlay, end messages and Settings ONLINE tab (`online.serverUrl`, `online.playerName`). The done test is `tools/net/m53b_test.sh` (19 checks: line discipline, build, self-tests, the golden matrix through `m53a_test.sh`, 2P Battle, 2v2 and 2P Challenge at a simulated 100 ms RTT with offline replays of the recordings, relay, ICE and TURN, 2% loss, drop, graceful leave and a stuck game, desync, spectator, the UI flow with screenshots, software latency at RTT 0/30/80 ms, and the report). **Results:** after the integration of the five packages on `online/m53b-int` the full done test passes (all 18 checks, 4291 s): over the simulated 100 ms RTT WAN (mean; the minimum-RTT estimate reads 93 ms), 2P Battle stalls 0.10% of ticks on the client with a phase error p99 of 276 us, 2v2 at most 0.21%, Challenge 0.41%, 2% loss 2.69%; relay, ICE and TURN, drop, leave, desync, spectator, the UI flow and the latency limits pass (results table in the M5.3b doc). Fix round 1 closed a seat hijack (a connection is now bound to a member only by a HELLO with its join secret), set a fixed maximum packet size in ENet, made a session from the standalone start menu send the folder's real content hash (and leave after the boot if the mounted mods differ), and fixed the join screenshot of the done test. Fix round 2 drops a seat by whether its game still makes progress (a game thread stuck mid-match for 10 s, or still loading 30 s after the host reached a load barrier, 60 s at the start; new check `stuck`), makes the crash screen say BYE at once, offers the host's Drop at a load barrier, and tightened chunk and name checks. Fix round 3 made the sample-line rule independent of the session seed (C plus L lines; a seed that ends a Battle round inside the run had left bat2 one C line short), stopped a REQ/LOBBY feedback loop on a taken seat, re-checks the hashes of a reconnecting member, drops a seat after START on a lobby `member_left` only once its P2P link is silent too, caps INPUT datagrams, and fixed two server races; after it the full done test passes again on the final commit (all 19 checks, 4451 s, golden included) and `desync_ci.sh --quick` passes. The launcher's older update check (`interface.checkForUpdates`, on by default) is the only socket an offline launcher boot opens. **Moved to part 2 or the user:** the Cloudflare TURN spike and the Fly deployment, matrix A and the 30-minute sessions, a session through a self-hosted compose stack, the relayed share of real peers, own-captain prediction and its checksum check, the latency options, press-to-photon figures on the rig under netem, late spectators, rejoin, host migration, the in-process return to the lobby, and the transport on Windows and macOS (list in the M5.3b doc, "Deferred").

**First playable vertical slice** (end of M5.3b plus the first two M5.4 modes):
- Four players on four Linux machines (matrix A) join a lobby by code.
- They pass the handshake with one overlay mod enabled, a 2v2 Heave arena.
- They play **2v2 Battle**, then **Heave! 2v2**, online.
- Each peer renders its own captain full screen, with an RmlUi team score bar and name tags.
- Each match runs best of 3 with no desync, saves a replay that re-verifies headless, and a fifth peer watches as a spectator.

Out of the slice: 3-team play (it follows as Tri-Onion) and cross-OS play.

## 9. Testing and CI

- **Replay corpus:** input logs for each mode, at 2 and 4 players, covering floor transitions, movies, cards and Lifeline. New modes add logs as they land.
- **Desync CI job:** on every commit, replay the corpus headless on two runner types (x86_64 with FMA and without), and compare checksums against the stored ones and across runners. Add a Windows runner at matrix B and an arm64 runner at matrix C.
- **Bisect tool:** given two diverging runs, find the first tick whose checksum differs, then dump the per-subsystem breakdown (RNG, navis, piki, enemies, pellets, mode state) for that tick.
- **Soak tests:** nightly 30-minute 4-peer sessions over a simulated WAN (latency, jitter, 2% loss) using recorded bot inputs. They log stall rate, chosen `d_i` and phase error.
- **Latency regression:** CI runs the software press-to-present probe (§8, M5.1a) offline and over netem at LAN, 30 ms and 80 ms RTT, and fails on a median more than 10 ms above the §2.10 budget. The press-to-photon rig is run by hand at each milestone.
- **Matching build:** the MWCC CI job stays green. Any game-side change outside `TARGET_PC` fails it.

## 10. Security and abuse

- **Slot ownership:** the host accepts input only for slots a peer owns, and clients accept bundles only from the host. A peer cannot send input for another player's slot. In the hybrid (§2.10), every peer also checks direct packets against the lobby's connection-to-slot map, so the host is no longer the only filter.
- **Equivocation (hybrid):** a sender could send different values for one tag to different peers. Each peer cross-checks the direct copy against the host-forwarded canonical copy; a mismatch drops the sender and names them in the report.
- **Packet bounds:** a fixed maximum packet size, a capped redundancy window, and tick numbers validated against the current window. Malformed packets drop the sender.
- **Relay:** Cloudflare TURN with short-lived credentials minted by `mppm-server`, or coturn / `--relay` when self-hosted (§2.11). Clients never hold provider credentials. `--relay` is rate-limited per session and per IP, with a cap on session lifetime and bandwidth. It forwards only, with no game logic.
- **Join codes:** random and unguessable (≥40 bits), expiring when the session ends. Optional lobby password. Host kick and ban for the session.
- **Mods:** local only at launch. Mod-pack download (follow-up) needs bounds-checked parsers and size limits first. In-archive overrides are spliced only into archives whose content hash matches a known disc archive (§7a).
- **Lobby server:** `mppm-server`, HTTPS and WSS only, rate-limited per IP. It parses untrusted internet traffic, which is one reason it is written in Rust (§2.11). Leaderboard submissions are verified by replay (§6). Unverified scores are labelled as such.

## 11. Risks and open questions

### Risks

| Risk | Mitigation |
|---|---|
| The LOD-dependent simulation is wider than audited (`EnemyBase` culling, pellets, piki joints, effects) | Observer-based LOD; force culling off for bosses and possessed enemies; checksum, CI and bisect catch the rest |
| The 47 `getActiveNavi` sites hide behaviour that only works with one human | Classify each site as sim or presentation; nearest-navi following `umiMushi.cpp:848-852`; replay tests per site |
| Draw changes sim state beyond the camera | `doUpdate` calls `j3dSetView` for every viewport (`baseGameSection.cpp:325-330`); audit it, and assert no sim writes in draw under a debug flag. Update-before-draw (§2.10 item 4) stays a session constant until a checksum A/B run of both orders matches |
| Host libm differs across CPUs on one OS (glibc ifunc FMA variants) | Vendored libm in M5.1b; matrix A uses mixed CPUs |
| Uninitialized fields diverge across peers | Zero-on-alloc and auto-var-init (M5.1b); the MSan or Valgrind pass of item 13 is still open ([online/M5_1B_BUILD.md](online/M5_1B_BUILD.md#memory-checker-pass-section-4-item-13)) |
| Builds of the same commit with different toolchains | `build_hash` covers the binary and the flags string (§2.3) |
| The 100-Pikmin cap makes 4-captain modes feel thin | Design squads around 25–33 Pikmin; profile a raised cap in M5.3a (§3.4) |
| Fixed sub-heaps overflow with 4 navis and larger rosters | Per-mode heap budget (§3.4) |
| Star topology adds a hop for players far from the host | Lobby suggests the minimax host (§2.3); forward-on-complete and a net thread (§2.10); per-sender `d_i` capped at 8 ticks with a mid-round increase before a drop vote; the hybrid fast path later |
| Mesh equivocation in the hybrid: one sender, different values per peer | Cross-check every direct copy against the host's canonical copy; drop and name the sender (§10) |
| Update-before-draw exposes sim side effects in draw | Session constant; checksum A/B run of both orders before it is default (§2.10 item 4) |
| Prediction or local cues leak into the sim | Draw matrices only, never world matrices; sim sound calls muted, never skipped; debug assert on the local slot (§2.10 item 5) |
| Cloudflare TURN does not work with a non-WebRTC client, or Fly changes its pricing | Spike first in M5.3b; the client speaks plain TURN, so coturn or `--relay` on VPSes drops in with no client change; the server is one Docker image (§2.11) |
| An archive override is larger than the original and overflows a fixed heap | Port-owned buffers for `EMM_Mem` archives; `mHeap` with a null check for the others (§7a) |
| Sim code waits on audio timing | Item 10 audit |
| Hostile overlay text crashes peers | Mods are local-only at launch; bounds-checked parsers before pack download |
| Simulating N cameras' LOD costs CPU at 4 players | Performance budget in M5.3a |
| A host drop ends the session | Short rounds and serialization at floor boundaries limit what is lost |
| Story co-op touches save, THP, overworld and audio state | Its own milestone (M6), built last |

### Open questions

- Host migration: is it worth building, or does the session end when the host drops?
- Smooth Motion (stage 2a is in HEAD, §2.1) and later frame-rate phases (PORT_PLAN.md:57-62) on top of the existing retrace accumulator: the seam stays keyed to sim ticks, and `video.smoothMotion` stays forced off online (`port/launcher/settings.cpp:128`). Is that still the right call once prediction exists?
- Competitive modes: per-sender delay (default) or the equal-delay option (§2.2)?
- Should net sessions raise the Pikmin cap, and to what?
- 4-team FFA: tinted Pikmin on one kind, or a 4th goal object?
- When more than 4 captains (`P2_PAD_PORTS`) would be worth the cost.
- Pause policy per mode: a vote, or no pause at all?
- Killcam: wait for in-process snapshots, or re-simulate in a helper process?
- Leaderboards before matrix C: x86_64-only boards, or arm64 scores flagged as unverified?
