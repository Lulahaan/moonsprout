# Determinism harness

Online play in mppm will use lockstep (see [ONLINE_MULTIPLAYER.md](../ONLINE_MULTIPLAYER.md)): every
machine runs the whole game and only controller input travels over the network. That works only
if the game is deterministic, meaning the same seed and the same input always give the same game
state. The harness tests this on one machine. You record or generate an input log, replay
it, and compare per-tick checksums of the game state between runs.

This document started with milestone M5.1a and now describes the state after M5.1b (the file
name is kept so that links stay valid). M5.1b changed the tick definition (loading passes are no
longer ticks), moved both file formats to version 2, split the RNG into streams, fixed the
simulation frustum, locks the settings in a harness run, moved the camera step into the tick, runs
the update before the draw, samples the pads late, and added the determinism matrix with its CI
job. Logs made by M5.1a builds (version 1) do not replay on M5.1b builds. The audits of clock,
audio, effects and RNG use are in [M5_1B_AUDIT.md](M5_1B_AUDIT.md); the vendored libm, zero on
alloc and the `-Wreturn-type` audit in [M5_1B_BUILD.md](M5_1B_BUILD.md); every game-side hook site
(file, line, what it calls) in [M5_1B_HOOKS.md](M5_1B_HOOKS.md).

M5.2 (data mods) moved the input log to version 3: its header stores the mods hash, the content
hash and the mod list of the recording run, and a replay refuses a run whose mods differ (see
[Content](#content-mods-and-the-replay-gate)). Version 2 logs still replay as stock content. The
mods themselves (folder, manifest, overlay, hashes) are described in
[M5_2_MODS.md](M5_2_MODS.md).

M5.3a part 1 (N captains) added `--vs-players 4` (a 2v2 Battle of four captains, harness only),
`--view-captain K`, input log version 4 (written only with more than 2 captains), test events 3 and
4 and `netlog.py synth --players 4`. With 1 or 2 captains every output is as before. The rules,
the code map and the done test are in [M5_3A_NPLAYER.md](M5_3A_NPLAYER.md).

M5.3b part 1 (online play, local) added `--ch-stage N` (a 2P Challenge harness session), input log
version 5 (written only for Challenge or non-stock VS handicaps: start mode 2, the course, the
handicaps at bytes 34 and 35), flags bit 1 (recorded in an online session) and `netlog.py synth
--ch-stage`, `--handicap` and `--online`. 1-2 captain Battle recordings stay version 3 byte for byte.
The online flags (`--server`, `--host`, `--join`, `--net-input`, `--net-impair`, ...), their log
lines and exit codes are in [M5_3B_NETCODE.md](M5_3B_NETCODE.md); an online session is built on the
harness (its ticks, checksums and recordings are the ones described here).

## Flags

Each flag can also be set with an environment variable. Usage: `build-pc/mppm DISC FLAGS`.

| Flag | Environment | Effect |
|---|---|---|
| `--seed N` | `MPPM_SEED` | Session seed (a u32). A replay always uses the seed in the log header; a `--seed` that differs from it is ignored with a warning. Without either, the clock seeds the session. |
| `--record FILE` | `MPPM_RECORD` | Write every tick's input for all four ports to FILE. |
| `--replay FILE` | `MPPM_REPLAY` | Read input from FILE instead of the pads. The process exits by itself when the log runs out. |
| `--checksum-log FILE` | `MPPM_CHECKSUM_LOG` | Write the checksum log. |
| `--checksum-verify FILE` | `MPPM_CHECKSUM_VERIFY` | Compare each checksum line with FILE as the run goes. On the first mismatch the game prints both lines and exits with code 3. |
| `--vs-stage N` | `MPPM_VS_STAGE` | Go straight to 2P Battle stage N (0 to 254) with no menus. The session and tick 0 start there. A larger value is rejected with a message and ignored. |
| `--headless-ticks` | `MPPM_HEADLESS_TICKS=1` | Run ticks as fast as possible and present nothing. |
| `--exit-after-ticks N` | `MPPM_EXIT_AFTER_TICKS` | Exit after N session ticks. Without the harness (see below) N counts pad reads since boot instead. |
| `--latency-probe` | `MPPM_LATENCY_PROBE=1` | Print the press-to-present latency figure (see [Latency probe](#latency-probe)). |
| `--tick-order update\|draw` | `MPPM_TICK_ORDER` | The order of a gameplay pass: `update` (the default) runs the update before the draw, `draw` the stock draw-then-update. A session constant: a replay takes it from the log header, and a flag given with a replay overrides the header with one notice (the A/B test). Without the harness it is read from the environment only. See [Tick order](#tick-order). |
| `--sim-k X` | `MPPM_SIM_K` | The widescreen factor of the simulation frustum, 1.0 to 3.6 (default 3.6). A session constant, also stored in the log header. Used only while the harness or an online session runs; see [Simulation frustum](#simulation-frustum). |
| | `MPPM_CHECKSUM_INTERVAL` | Write a checksum every this many ticks (default 30, 1 = every tick). |
| `--mods-dir DIR` | `MPPM_MODS_DIR` | Use DIR as the mods folder for this run instead of `<userdir>/mods` (M5.2). Mods change what a replay does: a version 3 log records the mods hash and is refused (exit 2) against other mods. See [M5_2_MODS.md](M5_2_MODS.md) section 2. |
| `--no-mods` | `MPPM_NO_MODS=1` | Mount no mod for this run (stock content). |
| `--vs-players N` | `MPPM_VS_PLAYERS` | M5.3a: captains of a `--vs-stage` session, 2 (default) or 4 (teams `i & 1`: 0 and 2 Red, 1 and 3 Blue). Any other value prints a message and exits 2 (3 teams are part 2). A replay takes the count from its header (version 4: byte 33; older: 2); a flag that differs prints one warning and the header wins. Without a VS stage the count is 2. With 4 captains the recording is version 4, and the checksum log and the start lines say `players=4`. |
| `--view-captain K` | `MPPM_VIEW_CAPTAIN` | M5.3a: show captain K (0 to 3) full screen instead of the stock split of captains 0 and 1. Presentation only: never recorded, the checksums do not change. Used only with more than 2 captains. A bad value is ignored with a message. |
| `--ch-stage N` | `MPPM_CH_STAGE` | M5.3b: go straight to 2P Challenge course N (0 to 29) with no menus, as `--vs-stage` does for Battle (the session and tick 0 start at the Challenge start barrier). The recording is version 5 with start mode 2 and course N. A replay takes the course from a version 5 header; a flag that differs prints one warning and the header wins. Not together with `--vs-stage`. |
| `--server`, `--host`, `--join`, `--spectate`, `--net-input`, `--net-impair`, `--net-mode`, ... | `MPPM_SERVER_URL`, `MPPM_NET_*` | M5.3b online sessions; see [M5_3B_NETCODE.md](M5_3B_NETCODE.md#command-line-and-environment). An online run is a harness run (its ticks, checksum log and recording are the ones of this page) in real time: `--replay` and `--headless-ticks` cannot be combined with `--host` or `--join`. Online exit codes: 7 desync, 8 session ended, 9 lobby failure. |

Numbers are whole decimal (or `0x` hex) values from 0 to 4294967295. A sign, trailing text or a
larger value is rejected with a message, never wrapped. An output log may not be on the path of
another log of the same run (for example `--record X --replay X`, or `--checksum-log X
--checksum-verify X`): the run stops with exit code 2 before anything is overwritten.

Debug aids, read only in the runs they apply to:

| Environment | Effect |
|---|---|
| `MPPM_HEADLESS_SHOW_EVERY=N` | With `--headless-ticks`, present every Nth pass so a screenshot shows progress. Determinism is unaffected (a replay with N=1 gives the same checksum log). |
| `MPPM_PROBE_HOLD_FRAMES=N` | With `--latency-probe`, keep the white patch for N presented frames per sample instead of 3 (a screenshot aid). |
| `MPPM_HOLD_AT_TICK=N` | In a harness session, when tick N is reached, present that pass's picture and keep presenting it for 3 s (one line `mppm: net: holding tick N`), so a screenshot shows exactly tick N. Use it with a paced replay. |
| `MPPM_RNG_PERTURB=pres=<u32>,other=<u32>` | XOR the given values into the seeds of the PRES and OTHER RNG streams. A test: the checksums must not change (the `rng-perturb` matrix cell). |
| `MPPM_RNG_AUDIT=1` | At the end of a session, print the first 32 distinct return addresses (with counts) of `draw-unflagged` and `other-in-session` RNG draws. Resolve them with `addr2line -e build-pc/mppm ADDRESS`. |
| `MPPM_RNG_STRICT=1` | Exit with code 6 when an OTHER-stream draw happened during the session. |
| `MPPM_RNG_SELFTEST=1` | Run the RNG stream self-test at start-up, print `mppm: net: rng selftest PASS` or `FAIL`, and exit 0 or 1 (needs no disc and no display). |
| `MPPM_PERF=1` | M5.3a: time every session tick's simulation (`BaseGameSection::doUpdate`, steady clock) and print `mppm: net: perf ticks=<n> sim avg=<ms> p50=<ms> p99=<ms> max=<ms> ms, pass avg=<ms> ms` at the session end. `pass` is the time from one tick's simulation start to the next one's; passes that held a load barrier are left out. Read-only. |
| `MPPM_NPLAYER_STATS=1`, `MPPM_VS_HANDICAP=N`, `MPPM_NPLAYER_SELFTEST=core\|piki\|vs\|view` | M5.3a aids: per-captain stats every 1800 ticks, both VS handicaps set to N (changes the checksums), the N-captain self-tests. See [M5_3A_NPLAYER.md](M5_3A_NPLAYER.md). |

Two latency settings apply to every run, with or without the harness (see
[Late sampling](#late-sampling)):

| Environment | Effect |
|---|---|
| `MPPM_LATE_SAMPLE=0` | Turn late sampling off: the pads are read right after the present, as in M5.1a. |
| `MPPM_LATE_SAMPLE_US=N` | The margin before the tick's retrace at which the pads are read (default 1000 µs). |

With none of the harness flags set, the game behaves as before for the player: no input is
replaced, nothing is written and the clock seed is used. Two things differ from M5.1a in every
run, harness or not: the pacing samples the pads late and the gameplay pass runs its update before
its draw (both lower the latency, see [Tick order](#tick-order)), and `rand()` draws from three
streams, so UI and draw calls no longer move the simulation's sequence. Each run with any flag prints one
`mppm: net:` line at start-up. It says `harness on` when one of `--record`, `--replay`,
`--checksum-log`, `--checksum-verify` or `--vs-stage` is given (the **harness**), and
`harness off, flags only` for runs with only `--seed`, `--headless-ticks`, `--exit-after-ticks`
or `--latency-probe`.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | The run ended normally: the window was closed, `--exit-after-ticks` was reached, or the replay log ran out, and every checksum line matched. |
| 2 | Start-up failure: the replay log cannot be opened or is malformed (too short, bad magic, version, header size or record layout, a bad tick order or sim k, a version 3 mod list without a NUL, larger than 1 GiB), a replay log that is not synthetic was recorded with other mods than this run's (see [Content](#content-mods-and-the-replay-gate)), the replay log is version 1 (made for M5.1a: its records include loading passes; make a new one with `netlog.py synth`), an output log (`--record`, `--checksum-log`) cannot be created or is on the path of another log, the verify log cannot be opened, or the `--vs-stage` inject finds no stage data (the start barrier can then never be reached). The reason is printed. |
| 3 | `--checksum-verify` found a difference. This includes the last `L` line and the `E` line, so a run that is shorter or longer than the verify log also fails. |
| 4 | A write, flush or close of the record or checksum log failed (a full disk, an I/O error), so that log is short. One `write error` line names the log. A mismatch (3) takes precedence. |
| 5 | The crash screen came up in a `--headless-ticks` run. Its logs are closed at that point and the process exits instead of waiting on a screen nobody reads. (With the default panic setting the game calls `abort()` instead, and the shell reports a signal.) |
| 6 | `MPPM_RNG_STRICT=1` was set and an OTHER-stream RNG draw happened during the session (see [RNG streams](#rng-streams)). Codes 3 and 4 take precedence. |

### What a harness run changes

While the harness is on, a few things that depend on wall-clock time or would add sim ticks are
turned off, and some waits become synchronous. None of this happens without a harness flag.

- The B+X+START reset combo and the long-press timers do nothing (two `PC_ONLY` guards in
  `JUTGamePad::update`), and the launcher's **Reset Game** is ignored with one log line
  (`port/game/reset.cpp`). `sResetPattern` itself is left alone: at 0 it would match whenever no
  button is held.
- **Settings are locked** (M5.1b). Right after the environment overrides are applied, the launcher
  locks every setting the plan classes as Forced or Synced (`net_lock_settings`, plan 2.6):
  the Forced ones (Smooth Motion, Pause While Menu Is Open, Pause in Background, fast-forward) take
  their net value (off), the Synced ones (the six `cheat.*` settings) take their defaults. The lock
  is an overlay above `config.json`, `--cvar` and `MPPM_*`, so neither the user's configuration
  nor a flag can change them; the Settings window shows them disabled with "Locked while a replay
  or harness session runs." One line says `mppm: net: settings locked (4 forced, 6 synced)`. The
  hash of the Synced values goes into the record header. The aspect ratio is no longer Synced: the
  simulation frustum does not use it.
- **The simulation frustum uses a constant** (M5.1b): the widescreen factor of the cull planes is
  the session's sim k (default 3.6), not the window's. See [Simulation frustum](#simulation-frustum).
- **Pause While Menu Is Open** never holds the game (`launcher::hold_game` returns false), because
  every return from the hold loop runs an extra sim tick. (The settings lock also forces it off.)
- **Smooth Motion** is off. In M5.1a this was because its re-run draw called `rand()`; with the RNG
  streams that no longer matters, but online play keeps it off until its purity check passes
  (`smooth_wanted` still keys on `p2_net_active()`).
- In a session, every DVD command sent from the main thread waits until the DVD thread has
  finished it (`p2_net_dvd_sync`). While it waits, the DVD thread's `rand()` calls (cave
  generation) use the simulation stream; the main thread is blocked, so the calls are serialised.
- With `--vs-stage`, every pad read before the start barrier is released (buttons, sticks and
  triggers 0; the connection state stays live). The boot needs no input there, and it makes the
  button state carried into tick 0 the same in a recording and in its replay.
- Closing the window during a session quits at the end of the current pass, not inside the pacing
  wait, so the logs never end on a tick that was read but not simulated. If the pass does not end
  within 2 s, the next event pump quits anyway.
- The checksum log is flushed after every line and the record log after every tick, so a run that
  is killed, hangs or crashes (a panic's `abort()` drops unflushed stdio buffers) keeps every line
  and every record up to the last one written.
- A restart from the launcher (a setting that needs one, the menu bar's restart, a soft reset)
  ends the session as a window close does (last `L` line, `E` line, tick count) and quits
  instead of starting a new process: the new process would get the same `--record` and
  `--checksum-log` and truncate this run's logs.

## Sessions and ticks

A **session** is the stretch of play that has to be deterministic. With `--vs-stage` it starts at
the VS start barrier, when the Battle stage is entered. At that point the RNG is reseeded with the
session seed and the tick counter is set to 0. Gameplay runs at 30 ticks per second, so 18000 ticks
is 10 minutes.

A **tick** is one controller read by the game (`JUTGamePad::read()` from `System::beginFrame`)
inside the session and **outside a nonsim region** (M5.1b; plan 2.1 and section 4 item 7).
`p2_net_tick()` is the number of ticks consumed, so it is 1 during the first pass.

A **nonsim region** is a loop that only waits for a load and runs no `update()`:

| Region | Where |
|---|---|
| `Section::loading` | `section.cpp:122-133` |
| `Section::fadeIn` (draw only) | `section.cpp:142-161` |
| the second loop of `Section::fadeOut` (waits until the section can finish) | `section.cpp:237-242` |
| the movie-reader wait after `fadeOut` in `Section::run` | `section.cpp:264-272` |
| `BaseGameSection::waitSyncLoad` and the single frame before it in `init` and `setupFixMemory` | `baseGameSection.cpp:182-208`, `:238-247`, `:2052-2059` |

The first loop of `fadeOut` runs `update()` and stays a sim loop. A pad read inside a nonsim region
sees every port released, consumes no input record, writes none and does not advance the tick, so
the number of loading passes may differ between machines (one is faster at loading) without
changing anything that is checksummed. When the outermost region ends, one more released pad read
runs inside it, so the button state the next tick starts from (edges, repeat counters) never
depends on how many passes the loop ran. The polls of `vsGS_Load.cpp:101` and `:110` run inside
`Section::main` passes and stay ticks; with the DVD wait they end on a fixed tick.

**Each further match** in one session (a second match after a match end, a later floor) reseeds
the simulation stream from `hash(sessionSeed, match index)` (one line `mppm: net: floor N reseed`;
plan 2.3). The first match keeps the session seed.

### Tick order

A gameplay pass is one `Section::main` iteration of the running `BaseGameSection`. Stock order is
draw, then update, so a press read in one pass is first drawn in the next pass. M5.1b runs the
**update before the draw** by default: the update runs in `JFWDisplay::beginRender` right after
the pacing wait and before the frame's GX set-up, and `Section::main` skips its own update. The
cycle draw, sound frame, update is the same in both orders; only the places of the pad read and of
the present in that cycle move. Both orders therefore give identical checksums (the `draw-first`
matrix cell), and the update-first picture of a tick already shows that tick's input.

With **Smooth Motion** in effect (offline only) the pass stays draw-first: an interpolated
picture shows `lerp(S(k-1), S(k))`, so it is one tick behind by construction, and draw-first
makes the tick's own picture the `alpha = 1` end point of the interpolation.

The **camera steps in the tick** (plan 2.5): `cameraMgr->update()` moved from `doDraw` to the
start of `doUpdate`, and each stepped view is copied into `mCurViewMatrix`, so the draw and the
direct-draw markers (life gauges, counters, Pikmin) use the view the tick computed. Smooth Motion
interpolates the camera states the ticks stored.

### Late sampling

Outside the headless runner, the host now waits **before** reading the pads instead of after:
the rest of the pacing wait (with Smooth Motion's in-between pictures, or the UI frames) moves to
the end of `p2_host_present`, up to `MPPM_LATE_SAMPLE_US` (1 ms) before the retrace the next tick
is due at, and then the pads are read. The press-to-read part of the latency drops from about half
a tick to about 1 ms plus the pump interval. `MPPM_LATE_SAMPLE=0` restores the M5.1a order. Pacing
and determinism are unchanged (the `paced` and frame-rate cells).

The wait runs the HostFrames loop body itself (D19): Smooth Motion's in-between pictures and the UI
frames over the running game are drawn there, and the tick wait has about 1 ms left. A frame may
start whenever it fits before the **tick's retrace**, as in the tick wait; only the sleeps stop at
the pre-read deadline. So a frame that ends inside the last millisecond is still drawn and the pad
read follows it. (The first M5.1b build stopped drawing at the deadline itself; with Smooth Motion
and a 120 fps cap it drew up to 9% fewer pictures than `main`, and none at all at 1280x960 under
Xvfb, where the present is slow. Fixed in the fix round; figures in
[Recorded figures (M5.1b)](#recorded-figures-m51b).)

### RNG streams

`rand()` draws from one of three LCG streams with the same step (`port/sdk/msl_compat.c`):

| Stream | Used by | In the checksum |
|---|---|---|
| SIM | the main game thread outside every presentation scope and outside `Section::draw`; the DVD thread while the main thread waits for it (`p2_net_dvd_sync`) | yes (`rng=`) |
| PRES | the main game thread inside a presentation scope (`Screen::Mgr::update` and `draw`, which cover every HUD and menu screen, `TParticle2dMgr::update`, the launcher scene) or inside `Section::draw` | no |
| OTHER | every other thread | no |

A `rand()` inside `Section::draw` but outside a presentation scope still goes to PRES and is
counted as `draw-unflagged` (plan 2.7's "assert on sim-stream use in draw"): it would have been a
simulation draw before. An OTHER draw during a session is counted as `other-in-session`; it could
feed the simulation, so it is a hazard. Each session ends with one line:

```
mppm: net: rng sim=<n> handoff=<n> pres=<n> draw-unflagged=<n> other=<n> other-in-session=<n>
```

`MPPM_RNG_AUDIT=1` prints the callers of the two audited kinds and `MPPM_RNG_STRICT=1` makes a
non-zero `other-in-session` exit 6. Offline the simulation now draws a different sequence than
before (UI and draw calls no longer advance it); the distribution is the same.

### Simulation frustum

Creature LOD (which gates enemy collision and animation, pellet physics and more; plan section 4
item 5) uses the cameras' cull planes. Those planes used the window's aspect times the widescreen
factor k, so a different window size changed the simulation (the M5.1a baseline differed at tick
510 for a 1920x1080 window). The cameras now carry a second aspect for the cull planes
(`mSimAspect`): while the harness or an online session runs, its k is the session constant sim k,
3.6 by default (the renderer's widest k, so the simulation frustum contains every render frustum
and draw culling stays correct); offline it is the window's k, so offline play culls exactly as
before. Draw culling uses the same planes. `--sim-k` changes the constant (a value below the
window's k culls objects at the screen edge while they are visible).

### Test events

A synthetic log can carry a **test event** in byte 11 of port 0's slot. The code applies at the
start of the update of the pass whose checksum tick is the record index + 1: 1 gives captain 0
lethal damage, 2 gives captain 1 lethal damage (`addDamage(10000)`). It is how the `Le` log ends a
2P Battle at a known tick to cover the match end, the result screen, the second load, the nonsim
regions and the reseed. Logs recorded from live input carry 0; a recording made while replaying
copies the replayed code. M5.3a: 3 gives captain 2 and 4 captain 3 lethal damage; they exist only
in 4-captain logs (`netlog.py synth` refuses a code above `--players`). The `Le4` log of the M5.3a
done test uses `--event 2400:3 --event 4800:1`: captain 2 goes down (the match goes on, captain 0
still plays for Red), then captain 0 (Red is out).

## Input log (`.mpnl`, binary, little-endian)

The engine writes version 3 (M5.2), version 4 (M5.3a) when the session has more than 2 captains, or
version 5 (M5.3b) for a 2P Challenge session or non-stock VS handicaps, and reads versions 2 to 5. A
version 3, 4 or 5 header is 128 fixed bytes followed by the mod list area; version 2's header is the
first 64 bytes alone:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `MPNL` |
| 4 | 2 | version = 3 (M5.2), 4 (M5.3a, more than 2 captains), 5 (M5.3b, Challenge or handicaps); 2 is still read, 1 is refused |
| 6 | 2 | header size: versions 3 to 5 = 128 + the list area (144 to 1152); version 2 = 64 |
| 8 | 4 | seed |
| 12 | 4 | tick count. `0xFFFFFFFF` means a recording that was not closed; the count is then `(file size - header size) / 48`. |
| 16 | 8 | build id (0 = any build) |
| 24 | 1 | slots = 4 |
| 25 | 1 | record size = 48 |
| 26 | 1 | start mode: 0 = boot, 1 = VS start barrier, 2 = Challenge start barrier (version 5 only) |
| 27 | 1 | VS stage, or the Challenge course (0 to 29) with start mode 2 (`0xFF` = none) |
| 28 | 4 | flags: bit 0 = synthetic, bit 1 = recorded in an online session (M5.3b; informational, also in online version 3 and 4 logs) |
| 32 | 1 | tick order: 0 = draw-first, 1 = update-first |
| 33 | 1 | versions 4 and 5: captains = 4 (0 and 2 are read as 2, anything else fails with exit 2); versions 2 and 3: zero |
| 34 | 1 | version 5: VS handicap of team 0 (Red), 1 to 10, 0 = stock; older versions: zero |
| 35 | 1 | version 5: VS handicap of team 1 (Blue), 1 to 10, 0 = stock; older versions: zero |
| 36 | 4 | sim k as IEEE-754 float bits; 0 = the default 3.6, otherwise 1.0 to 3.6 |
| 40 | 4 | hash of the Synced settings (FNV-1a 32 of the locked values); 0 = not recorded |
| 44 | 20 | zero |
| 64 | 32 | version 3: mods hash (SHA-256 of the mounted mod set, [M5_2_MODS.md](M5_2_MODS.md)); 32 zero bytes = stock content |
| 96 | 32 | version 3: content hash (`p2_mods_content_hash`: the build id and the mod set; informational, M5.3b's handshake compares it) |
| 128 | 16..1024 | version 3: the mod list `id@version,id@version` in load order, NUL-terminated, zero-padded to a multiple of 16 (stock: 16 zero bytes) |

Version 4 is version 3 with the captain count at byte 33 and nothing else: the engine writes it only
when `p2_net_vs_players()` is above 2, so a 1-2 captain recording stays version 3 byte for byte,
and an older engine refuses a 4-captain log instead of playing it with 2 captains. The records of a
4-captain log fill all four slots (port i drives captain i).

Version 5 is version 4 with start mode 2 (2P Challenge: byte 27 is the course, the log has 2
captains) and the two VS handicaps at bytes 34 and 35. The engine writes it only for a Challenge
session or when a handicap is not stock, so every other recording stays version 3 or 4 byte for byte
and an older engine refuses a Challenge log instead of playing it as Battle. `netlog.py` refuses a
start mode 2 below version 5, a course above 29, a Challenge log with 4 captains and a handicap above
10.

A version 2 log (M5.1b) is read as stock content: mods hash zero, empty list. Version 1 (M5.1a) had
zeros at 32 to 63; its records include loading passes, so it cannot line up with version 2 ticks.

Reading a version 3 log checks that the header size is 144 to 1152 and that the list area holds a
NUL; otherwise the start-up fails (exit 2). The list is printed with anything but printable ASCII
replaced by `?`. A list longer than 1023 bytes is cut in the header (the mods hash, which is what
the gate compares, is always complete).

Records start at the header size (144 for a stock version 3 log, 64 for version 2). Each one is 48 bytes per tick: 4 slots of 12 bytes, port 0 first.
A slot holds: u16 button, s8 stickX, s8 stickY, s8 substickX, s8 substickY, u8 triggerLeft,
u8 triggerRight, u8 analogA, u8 analogB, s8 err, u8 event. `event` is the test event code (see
[Test events](#test-events)) in port 0's slot and 0 in the others. `err` is 0 when a controller is
present. It is -1 when there is no controller, and then every other field is 0. The values are
the ones the game sees after its stick clamp (`PADClamp`, Aurora's octagon): the main stick is at
most 72 on one axis, or 40 and 40 on a diagonal; the C-stick at most 59, or 31 and 31; the
triggers 0 to 150. A keyboard direction gives the full 72.

The button bits are: LEFT `0x0001`, RIGHT `0x0002`, DOWN `0x0004`, UP `0x0008`, Z `0x0010`,
R `0x0020`, L `0x0040`, A `0x0100`, B `0x0200`, X `0x0400`, Y `0x0800`, START `0x1000`.

## Content: mods and the replay gate

`p2_mods_mount` (port/mods/, after the disc is open and before the game starts) reports the mounted
mod set to the harness once with `p2_net_set_content(modsHash, list)`, also for stock content (zero
hash, empty list). The harness then:

- refuses a loaded replay that is **not synthetic** (flags bit 0 clear) and whose mods hash differs
  from this run's (a version 2 log counts as stock). One line names both sides and the process
  exits 2:

  ```
  mppm: net: the replay was recorded with other content: replay has <list|none> (mods <hash8>), this run has <list|none> (mods <hash8>); turn on the same mods in the same order
  ```

  `<hash8>` is the first 8 hex digits of the mods hash. The build id is not part of this check: a
  different build is still only a warning, so the determinism corpus replays across builds.
- does not check a **synthetic** replay (random input is valid for any content). With mods on it
  says so: `mppm: net: synthetic replay: content not checked (this run: <list>)`.
- with the harness on and mods on, prints `mppm: net: content: <n> mods <hash8> (<list>)`.
- stores the mods hash and the list for the version 3 record header (written at the session
  start) and the `# mods=` line of the checksum log.

With stock content nothing of this is printed and the checksum log is unchanged. Until
`p2_net_set_content` is called the content is stock.

`p2_net_build_id()` is the build id (FNV-1a 64 of the executable, the value at offset 16 and in
`# build=`), computed once under `std::call_once`, so the launcher may ask for it (for the content
hash) from a worker thread.

## Checksum log (`.cksum`, text)

```
# mppm-cksum v2 seed=<u32> interval=<n>
# build=<016x>
# mods=<64 hex> <list>          (only when mods are mounted)
# players=<N>                   (only with more than 2 captains, M5.3a)
C <tick> <total> rng=.. navi=.. piki=.. enemy=.. pellet=.. time=.. mode=.. cam=.. n=<navi>/<piki>/<enemy>/<pellet>
L <tick> <total> rng=..
E <tick> <rolling> lines=<count>
```

- `C` is a gameplay tick. It has one hash per subsystem and the object counts. `rng` is the
  SIM stream only. `time` also covers the simulation-relevant settings (`daySpeed`,
  `sunsetOnDayOne`, `unlockChallenge`, `invincibleCaptains`, `infiniteSprays`), so a settings lock
  that fails shows at the first `C` line. `cam` (version 2) hashes both play cameras' view matrix
  and view angle.
- `L` is a due tick on which no gameplay update ran (a load screen or the results screen).
- `E` is the end line.

The log has no timestamps and nothing that differs between matrix cells (tick order, window,
frame rate), so two runs that agree give byte-identical files and plain `cmp` is enough to check
them. Version 1 (M5.1a) had no `cam=` field. The `# mods=` comment (M5.2) follows `# build=` only
when the run's content is not stock: the full mods hash and the mod list. A stock run writes the
same file as before M5.2, and `cmp-cksum` ignores comments. `# players=<N>` (M5.3a) follows them only
when the session has more than 2 captains; the `C` lines then count all of them in `n=<navi>/...`.

## Tools (`tools/net/netlog.py`)

The keyboard drives only port 0, so the two-player test input comes from a generator:

```sh
python3 tools/net/netlog.py synth --seed 20261122 --synth-seed 7 --ticks 18000 --vs-stage 0 -o /var/tmp/vs10.mpnl
python3 tools/net/netlog.py synth --seed 20261122 --synth-seed 7 --ticks 9000 --vs-stage 0 --event 2400:2 -o /var/tmp/le.mpnl
python3 tools/net/netlog.py dump /var/tmp/vs10.mpnl --from 0 --count 40
```

`synth` writes version 3 (stock content unless told otherwise). Its header options:
`--tick-order update|draw` (default update), `--sim-k X` (default 0, the engine's 3.6) and
`--event T:CODE` (repeatable) for a test event in the pass of checksum tick T (the code goes into
record T-1). `--mods-hash HEX` (64 hex digits) and `--mods-list TEXT` (`id@version,...`) fill the
version 3 content fields (defaults: stock, zero hash and empty list; a list needs a hash). A
synthetic log is never content-checked, so these are for tests of the tools and of the messages.
`--format-version 2` writes the M5.1b layout (`desync_ci.sh` uses it, so its inputs and golden
logs stay as they were), and `--format-version 1` the M5.1a layout for tests against old builds
(no events, no header options).

`synth --ch-stage C` (M5.3b) writes version 5 with start mode 2 and course C (0 to 29), 2 players;
it is refused together with `--vs-stage`, `--players 4`, `--handicap` or a `--format-version` below
5. `synth --handicap R,B` writes version 5 with the VS handicaps of Red and Blue (each 0 = stock or 1
to 10; `0,0` stays version 3). `--online` sets flags bit 1 as an online recording would. The defaults
are unchanged (version 3, and `desync_ci.sh` keeps `--format-version 2`).

`synth --players 4` (M5.3a) writes version 4 (byte 33 = 4) and fills all four ports; 3 is refused
(3 teams are part 2), and `--format-version` below 4 with `--players 4` is an error. Port p comes
from its own generator stream (`PortGen(synth_seed, p)`), so ports 0 and 1 of a 4-player log equal
a 2-player log of the same seeds. `--event` codes 3 and 4 need `--players 4`.

`synth` (2 players, the default) fills ports 0 and 1 and leaves 2 and 3 empty. It holds the sticks in random directions, at
full or half deflection (full is the clamp's limit above), taps A and B often and the other
buttons rarely, and never presses START. It uses its own
xorshift32 generator, so the same arguments give the same bytes on every machine and every Python
version. A different `--synth-seed` gives a different match.

The other subcommands:

- `mutate FILE --tick T [--port P] [--ticks N] -o OUT` copies an input log with port P (default 0;
  2 and 3 only in a 4-player log) changed at tick T: A flipped and the main stick mirrored (pushed full right when neutral).
  `--ticks` cuts the copy. The done test's negative control uses it.
- `dump FILE [--from R] [--count N] [--events]` prints the header (with tick order, sim k,
  settings hash and the list of events in version 2; in version 3 also the header size and
  `mods hash`, `mods` and `content hash`, or `content: stock`; `players`, 2 for versions below 4;
  M5.3b: `course` instead of `vs stage` for a Challenge log, `handicaps` in version 5, and the
  names of the flags, `synthetic` and `online`)
  and the records; the left column is the
  record index R (record R is checksum tick R+1), and a record with an event is marked
  `<< event CODE at tick T`. `--events` prints only the header.
- `cmp-input A B` compares the seed, start mode, stage (or course) and every record byte of two input logs,
  events included, and fails on a different captain count (M5.3a) or different handicaps (M5.3b). It ignores the build id, the flags, the content hash and an open tick count,
  and only notes (without failing) a different format version, tick order, sim k, settings hash
  or mods hash (`note: mods hash differs (A stock, B mods 5a5a5a5a (a@1.0.0))`). Records are read
  at each log's header size, so a version 2 input and its version 3 recording compare.
- `cmp-cksum A B [--limit N]` compares two checksum logs and ignores the `#` lines (it notes a
  different format version). Fields are parsed generically as `key=value`, so `cam=` and any later
  column are named in its report.
- `stats FILE` summarises a checksum log: format version, line counts, last tick, distinct values
  per hash column (`cam` included) and the largest object counts.
- `mutate` keeps the whole header (version 3: the hashes and the mod list too) and the events.
- `selftest` checks the tool itself, including version 3 round trips (stock, with mods, a long
  list), a bad version 3 header size, a mod list without a NUL, and (M5.3a) version 4 round trips,
  byte 33, events 3 and 4, `mutate --port 2|3` and the refusal of 3 players; (M5.3b) version 5
  round trips (Challenge course, handicaps, the online flag), bad version 5 headers, `dump`,
  `cmp-input` and `mutate` on version 5, and the refusal of `--ch-stage` with `--players 4`,
  `--vs-stage` or `--handicap`.

`cmp-input`, `cmp-cksum` and `selftest` exit 0 when everything matches and 1 on a difference.

## Recording and replaying by hand

Always run the game under Xvfb, with an isolated home, never on your desktop and never with your
real save. For example:

```sh
T=/var/tmp/mytest; mkdir -p "$T/home"
env -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR GDK_BACKEND=x11 DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
  SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy HOME="$T/home" XDG_DATA_HOME="$T/home/.local/share" \
  MPPM_SKIP_LAUNCHER=1 xvfb-run -n 95 -s "-screen 0 1280x960x24" \
  ./build-pc/mppm "/path/to/your/disc.iso" --replay /var/tmp/vs10.mpnl --record "$T/r1.mpnl" \
  --checksum-log "$T/r1.cksum" --headless-ticks
```

Run the same command a second time with `r2` file names and `--checksum-verify "$T/r1.cksum"`
added, then compare the results with `cmp "$T/r1.cksum" "$T/r2.cksum"` and
`netlog.py cmp-input /var/tmp/vs10.mpnl "$T/r1.mpnl"`. Replaying one log while recording another
is allowed; the new log gets the running build's id, and its records must equal the input's.

## The M5.1a done test

```sh
tools/net/m51a_replay_test.sh [-b build-pc] [-d 96] [-o /var/tmp/mppm-online/verify] [-t 18000] [-s 0] [-a on|off] [-f 1|2]
```

Run it from the repo root. The script writes the 10-minute log and replays it twice under Xvfb,
with a home directory inside the output folder. Each run has a 1500 s timeout. The script then
checks that:

- both runs exit 0 and the two checksum logs are byte-identical;
- the E line is at the last tick, and enough C lines were written;
- the captains moved and Pikmin existed (more than 100 distinct `navi` hashes and a Pikmin count
  above 0);
- the record path reproduced the input.

As a negative control, it replays the same log with port 0 changed at one tick in the middle
(`netlog.py mutate`, tick `-t / 2`), cut 600 ticks later. That run's checksum log must match the
first run's line for line up to the changed tick and then differ between two `C` lines: a real
divergence of the simulation, not a load screen of another length or a log that ends early. Each check prints PASS or FAIL, and
the script exits 0 only if all of them pass. `-a off` sets `MPPM_AUDIO=0`, and `MPPM_TEST_DISC`
gives the disc path (required). `-f` picks the input log format: 2 (the default) for M5.1b builds,
1 for M5.1a builds. `-c` skips the game runs and checks the files already in the output folder.
The determinism matrix below covers this test and much more.

## Determinism matrix

```sh
tools/net/desync_ci.sh [-b build-pc] [-d 97] [-o /var/tmp/mppm-desync] [-c CELL[,CELL...]] [--quick] [--update-golden] [--format 1|2]
```

Run it from the repo root with `MPPM_TEST_DISC` set. Every run gets an isolated home and the
script unsets `MPPM_MODS_DIR` and `MPPM_NO_MODS` (as does `m51a_replay_test.sh`), so the matrix
always runs stock content whatever the caller's environment holds. It writes three synthetic 2P Battle logs
(`netlog.py synth --seed 20261122 --synth-seed 7 --vs-stage 0`) and replays each in many
configurations. Every cell's checksum log must be byte-identical (`cmp`) to the reference run of
the same log, and every run must exit 0:

| Log | Ticks | What |
|---|---|---|
| `L10` | 18000 | 10 minutes of one match |
| `Ls` | 3600 | 2 minutes, for the real-time (paced) cells |
| `Le` | 9000 | `--event 2400:2`: captain 1 falls at tick 2400, the match ends, the result screen passes and a second match is loaded and played |

| Cell | Logs | How it differs from the reference |
|---|---|---|
| `ref` | L10, Ls, Le | the reference: `--headless-ticks`, Xvfb 1280x960, audio on, update-first; also records the input (`--record`) |
| `repeat` | L10, Le | the same again |
| `paced` | Ls | real time (no `--headless-ticks`) |
| `fr-original`, `fr-120`, `fr-unlimited` | Ls | paced, `MPPM_FRAME_RATE=original`, `120`, `unlimited`, with `MPPM_VSYNC=0` |
| `audio-off` | L10, Le | `MPPM_AUDIO=0` |
| `win-1080` | L10 | Xvfb and window 1920x1080 (aspect Auto) |
| `win-1080-original`, `win-1080-stretch` | L10 | the same with `--cvar video.aspectRatio=original`, `stretch` |
| `win-ultrawide` | L10 | Xvfb and window 2560x1080, `--cvar video.aspectRatio=21:9` |
| `win-640` | L10 | window 640x480 |
| `draw-first` | L10, Le | `--tick-order draw` |
| `no-fma` | L10, Le | `GLIBC_TUNABLES=glibc.cpu.hwcaps=-AVX2,-FMA`: glibc's non-FMA `sinf`, `cosf` and `sin` (a stand-in for a second CPU) |
| `settings` | L10 | cheats and Pause While Menu Is Open by `--cvar`, `MPPM_TIME_SCALE=2 MPPM_SMOOTH_MOTION=1 MPPM_ALLOW_SUNSET=1`, and a `config.json` in the cell's home with the same cheats: the settings lock must hold |
| `rng-perturb` | L10, Le | `MPPM_RNG_PERTURB=pres=305419896,other=2596069104`: UI randomness must not reach the simulation |
| `show-every` | Ls | `MPPM_HEADLESS_SHOW_EVERY=1` |

Extra checks: the E line of each reference is at the last tick and its header is version 2; the
recording of each reference equals its input (`cmp-input`); `ref_L10` has at least 400 `C` lines
and more than 100 distinct `navi` hashes; `ref_Le` printed `mppm: net: floor 1 reseed` (a second
match ran; if not, the event tick or the log length needs adjusting); every run's
`mppm: net: rng` line says `other-in-session=0`; a negative control (`L10` mutated at record 9000,
cut 600 ticks later) matches the reference up to that tick and then differs between two `C` lines;
and when `build-pc/p2libm_selftest` exists, it passes with and without the `no-fma` tunable, its
vendored hashes do not change, and its `host sinf` line does (otherwise the tunable does nothing on
this glibc and the `no-fma` cells prove nothing).

**Golden logs.** `tools/net/golden/L10.cksum`, `Ls.cksum` and `Le.cksum` are the reference logs
of a known-good build. Each reference run is compared with its golden log (`netlog.py
cmp-cksum`, which ignores the `#` lines). `MPPM_GOLDEN=strict` (the default) fails on a
difference or a missing file, `warn` only reports it, `skip` skips the check. The script prints
matrix failures and golden failures under separate headings: a golden difference with a clean
matrix usually means a different compiler or toolchain, not a bug. `--update-golden` copies the
references there (after checking the change).

Options: `-c` runs only the named cells (plus the references they need; `neg` and `libm` name
the extra checks), `-l` lists the cells, `--quick` runs the headless `L10` cells, the `Ls` and `Le`
references, the negative control, the libm check and the golden check (about 15 runs; meant for
every commit), `--format 1` tests a build from before M5.1b (version 1 logs, no `Le`, no
`draw-first`, no RNG line, libm or golden check). Each cell has its own home directory under the
output folder; runs use the same wrapper as the done test (Xvfb lock wait, one retry on
`x11 not available`, a timeout of `MPPM_DESYNC_TIMEOUT` seconds, 1500 by default, exit code
files, core and cache clean-up). The script prints one `PASS` or `FAIL` line per check, a table of
the cells, and exits 0 only when every check passed. A full run takes about 25 minutes (the paced
cells are real time).

### CI job

The matrix needs a disc image, which cannot be in a public CI, so no workflow runs it. Run
`tools/net/desync_ci.sh --quick` locally before pushing netcode changes, and the full matrix
(without `--quick`) before a release. It needs `MPPM_TEST_DISC` (the path to your own disc
image), Xvfb, `xvfb-run`, `xdotool`, python3, cmake, ninja and clang, and uploads nothing: the
`.cksum` and `.log` files stay in the output folder.

### Baseline at the M5.1b base commit

The same logs replayed with the M5.1a build (`193d13937`, version 1 logs) showed which cells the
M5.1b work had to fix: every cell was identical except a 1920x1080 window with aspect Auto
(k = 1.31; first difference at tick 510, field `pellet`) and the cheats (tick 210, field `navi`);
`desync_ci.sh --format 1` against that build fails exactly those cells plus the ultrawide window
(k is not 1 there either) and passes all others: on `L10`, `win-1080` and `win-ultrawide` first
differ at tick 180 (`pellet`), `settings` (which adds a `config.json` and Pause While Menu Is
Open) at tick 390 (`navi`); the full run took 951 s. The glibc tunable did change glibc's `sinf`, `cosf` and `sin`, but the
game log was not affected because gameplay code hardly calls libm.

## Latency probe

`--latency-probe` measures software press-to-present on the live input path (plan 2.10). It is
off with `--headless-ticks`, and it is not meant to be combined with `--replay`. The procedure
and the M5.1b figures are under [Latency procedure](#latency-procedure).

How it works:

1. A key press or gamepad button press (SDL event) starts a sample when none is running.
2. At the end of each pass the probe reads port 0's pad. The first pass whose read shows a new
   button bit, the main stick crossing half deflection (36 of 72) in a new direction, or (M5.1b)
   the C-stick crossing half deflection (30 of 59), is the pad edge: that pass's update used the
   press. So a turn (a direction key or a stick push) and a camera move are measured as well as a
   button. A press with no edge within 10 passes is dropped (a key that is
   not bound, or a tap shorter than one pad read).
3. In an update-first pass (the M5.1b default) the picture of the pass that read the press
   already shows its effect, so that pass draws a white 64 by 64 square at the top left (in 640 by
   480 game space). In a draw-first pass (stock order, `--tick-order draw`, or Smooth Motion on)
   the first picture that can show the effect is drawn in the next pass, which draws the square.
4. With Smooth Motion on, the pass that saw the edge has also captured its state for the
   in-between pictures, so the first in-between picture drawn after it already moves toward the
   effect (by its alpha). That picture draws the square and ends the sample instead; the sample
   line says `in-between picture`. A preserved frame (a menu over the game with Smooth Motion off)
   presents the tick's picture again, with its square.
5. When that frame's present has completed, the sample ends. The patch stays for 3 presented
   frames in total (for a photodiode rig), in-between pictures included, so it does not flicker.

Output on stderr, one line per sample and a summary every 10 samples and at exit:

```
mppm: probe: on (sim channel, update-first, late sample 1000 us)
mppm: probe: sim press->present 110.19 ms (press->read 49.92, read->present 60.27, tick picture; press estimated, pump interval 33.36 ms)
mppm: probe: n=30 median 110.29 ms min 110.00 max 110.94 (estimated press times: 30, ended by an in-between picture: 0)
```

`press->read` runs to the end of the pass that saw the edge, `read->present` from there to the
completed present of the patch frame. `sim` is the channel; `pred` is reserved for local
prediction in M5.3b.

**Press time.** Wayland and the gamepad backends stamp an event when it happens. SDL 3's X11
backend stamps it when the event queue is drained (`X11_GetEventTimestamp` returns
`SDL_GetTicksNS()`, marked FIXME in SDL), which would leave out the wait from the press to the
next event pump, on average half a pass (about 17 ms in 2P Battle). The probe detects such a
dispatch stamp (a stamp inside the pump that drained it) and then counts the press as the middle
of the interval since the previous pump, which is unbiased for a press at a random time. With late
sampling there are two pumps per pass (one at the end of the present, one right before the pad
read); the probe keeps the start and end of the last pumps and uses the interval since the pump
before the one that drained the event. Those samples say `press estimated` and give the pump interval. Each single sample
is then only accurate to plus or minus half that interval; the median over many presses is not
biased. Figures from different backends are comparable only through this correction.

**Smoke test.** A press must be held for at least one tick, or its down and up both fall between
two pad reads and no edge is seen. Inject presses as `xdotool keydown space; sleep 0.1; xdotool
keyup space`, not `xdotool key space`. Any CI injector needs the same hold.

### Recorded baseline (M5.1a)

Software press-to-present under Xvfb (1280x960, X11, SDL dummy audio, default settings, Smooth
Motion off), measured at commit time of the M5.1a fix round, 30 held presses each with random
gaps, all with estimated press times:

| Scene | Flags | Median | Min | Max |
|---|---|---|---|---|
| 2P Battle, stage 0 (30 Hz ticks) | `--latency-probe --vs-stage 0`, `space` (A) | 110.3 ms | 110.0 | 110.9 |
| Title screen (no harness) | `--latency-probe`, `Shift_L` (B) | 75.9 ms | 63.3 | 82.0 |

Smooth Motion off and on (plan 2.10 asks for both). `--vs-stage` forces Smooth Motion off, so
this pair is measured in the story field (Day 3 of a test save, no harness) with
`MPPM_FRAME_RATE=60 MPPM_VSYNC=0 MPPM_WINDOW=640x480` and `MPPM_SMOOTH_MOTION=0` or `1`, 31 held
presses each (fix round 2). The small window is needed under Xvfb: at 1280x960 a present is
still in flight for most of the tick, so the Smooth loop draws almost no in-between pictures.

| Field, 30 Hz ticks, 60 Hz cap | Press | Smooth Motion off | Smooth Motion on |
|---|---|---|---|
| Median (min to max) | `space` (A) | 92.4 ms (92.2 to 92.6), tick picture | 67.8 ms (67.3 to 84.6), all 31 in-between pictures |
| Median (min to max) | `d` (turn right) | 92.3 ms (92.0 to 92.7), tick picture | 84.0 ms (67.2 to 84.5), all 31 in-between pictures |

With Smooth Motion on, the pacing loop pumps events at 60 Hz, so the press estimate uses a 16 ms
pump interval; the samples fall in two groups (about 67 and 84 ms) by where in the tick the press
lands. The in-between figure is the first picture that moves toward the effect, at a small alpha;
the full effect still shows in the next tick's own picture.

These are not display latencies: Xvfb scans nothing out, so the display stages of plan 2.10
(compositor, scan-out, panel) are missing, and the plan's ~107 ms estimate includes them. The
2P Battle figure has no spread because every press time is the midpoint of the same 33 ms pump
interval.

### Latency procedure

```sh
tools/net/latency_probe.sh [-b build-pc] [-d 98] [-o /var/tmp/mppm-latency] [-m vs|cave] [-k KEY] [-n COUNT] [-- NAME=VALUE ...]
```

`-m vs` starts 2P Battle stage 0 (`--latency-probe --vs-stage 0`, Xvfb 1280x960), waits 25 s, then
injects COUNT held presses of KEY (`xdotool keydown`, 0.1 s, `keyup`, 0.7 s), stops the game and
prints the last `mppm: probe: n=` line and the median, minimum and maximum of the last COUNT
samples. `-m cave` follows the menu route of the M5.1a fix-round probe from the title into the game
of a save card of your own (`MPPM_TEST_CARD`, a `.gci` file outside the repo; with the reference
save it lands in the Day 3 field, where the M5.1a Smooth Motion figures were taken), with
`MPPM_UNLOCK_CHALLENGE=1 MPPM_FRAME_RATE=60 MPPM_VSYNC=0 MPPM_WINDOW=640x480` (the small window is
needed under Xvfb for Smooth Motion to draw in-between pictures). `MPPM_SMOOTH_MOTION`,
`MPPM_TICK_ORDER`, `MPPM_LATE_SAMPLE` and `MPPM_LATE_SAMPLE_US` are passed through; more
assignments go after `--`. Keys: `space` (A), `d` (turn right), `l` (C-stick right, camera).

The M5.1a behaviour is still available on an M5.1b build as `MPPM_TICK_ORDER=draw
MPPM_LATE_SAMPLE=0` (the "legacy" figures). Expected software figures under Xvfb (design): about
16.7 ms from press to pump on average, then about 1 ms, the update, the draw and the present:
about 32 ms in the 640x480 cave with Smooth Motion off (92.4 before), about 40 ms with it on (67.8
before), and about 50 ms in 2P Battle at 1280x960 (110.3 before; the Xvfb present alone takes
about 27 ms there).

### Recorded figures (M5.1b)

`tools/net/latency_probe.sh`, 30 held presses each, median of the last 30 samples (min to max), all
with estimated press times, Xvfb, SDL dummy audio. "Field" is `-m cave` with the reference save
(Day 3 field, 640x480, 60 fps cap, `MPPM_VSYNC=0`). M5.1a column: the M5.1a build `193d13937`;
M5.1b columns: the M5.1b verification (build `3f91f1a3b`) and the fix round (final build of the
round, after the late-sampling fix below).

| Scene | Smooth Motion | Key | M5.1a | M5.1b (verification) | M5.1b (fix round) |
|---|---|---|---|---|---|
| 2P Battle stage 0, 1280x960 | off (forced) | `space` (A) | 110.3 | 45.1 | 45.3 (44.9 to 64.8) |
| 2P Battle stage 0, 1280x960 | off (forced) | `d` (turn) | 110.4 | 45.3 | - |
| Field | off | `space` (A) | 92.4 | 26.7 | 26.8 (26.3 to 43.3) |
| Field | off | `d` (turn) | 92.3 | 26.6 | 26.5 (26.2 to 27.0) |
| Field | off | `l` (camera, C-stick) | no C-stick probe | 26.7 | 26.7 (26.3 to 43.3) |
| Field | on | `space` (A) | 84.1 | 50.6 | 50.5 (35.0 to 60.5) |
| Field | on | `d` (turn) | 84.0 | 50.5 | 50.6 (34.8 to 60.4) |
| Field | on | `l` (camera) | - | 50.7 | 50.6 (34.5 to 60.7) |

With Smooth Motion off the gain is about 65 ms, two ticks: one from update-first, one from late
sampling. With it on the gain is about 33 ms (one tick, late sampling; the pass stays draw-first,
D16); 43 of 50 samples end on an in-between picture. Turning, cursor (A) and camera give the same
figure. `--vs-stage` forces Smooth Motion off, so Smooth Motion on is measured in the field only.

**Picture rate against `main`** (Smooth Motion on, normal field session with no harness flags,
`MPPM_FPS=1`, mean of the last 20 fps lines; pictures shown per second, in-between pictures per
second in brackets):

| Window, cap | `main` (`9fcbaf860`) | M5.1b (verification) | M5.1b (fix round) |
|---|---|---|---|
| 1280x960, 60 | 30.1 (0) | 30.1 (0) | 30.1 (0) |
| 1280x960, 120 | 32.8, 33.0 (16 to 18; about 14 tick pictures a second skipped) | 30.0 (0) | 33.4 (8.8; 5.6 tick pictures skipped) |
| 640x480, 60 | 60.0 | 59.9 | - |
| 640x480, 120 | 98.7 (68.8) | 89.7, 89.5 (59.6) | 97.8 (67.8) |

The verification build lost the last in-between slot of each tick (see [Late sampling](#late-sampling));
the fix round matches `main` within the run-to-run spread. At 1280x960 under Xvfb `main` shows more
in-between pictures because it drops more of the ticks' own pictures (a present still in flight).

Still open for this row of the plan:

- **Press-to-photon rig.** Not built (no hardware here). The patch and its 3-frame hold are in
  place for it. The plan's 55 ms target includes about 22 ms of display that Xvfb does not have.
- **A real-display run** (60 Hz fullscreen, turning) to confirm the ~107 ms offline baseline. The
  harness rules allow only Xvfb runs in this environment. The probe measures turning and the
  camera, so the run needs only a display and a person: `--latency-probe`, then hold a direction
  30 times.
- **Matrix A.** The row asks for the same log to match on two machines with different CPUs. Only
  one machine was available; the `no-fma` cell (glibc's non-FMA `sinf`, `cosf`, `sin`) is a
  stand-in that exercises a second libm path on the same CPU, not a second CPU.
- **A desync CI job.** The matrix needs a disc, so it cannot run in public CI. It has only been
  run by hand (`desync_ci.sh`, full and `--quick`).
- **The MSan or Valgrind pass** of section 4 item 13 (one VS match). Not done: neither tool is
  installed in the build container, and the uninitialised heap read found by the zero-init A/B
  (tick 8310, field `piki`) is still unlocated. Zero on alloc keeps it deterministic. Details in
  [M5_1B_BUILD.md](M5_1B_BUILD.md#memory-checker-pass-section-4-item-13).

## Deviations from the plan

Design decisions where the plan says something else or leaves it open (D1 to D11 from M5.1a; D4
to D6 and D11 updated for M5.1b):

| # | Topic | Decision | Why |
|---|---|---|---|
| D1 | Session start | At the VS start barrier with `--vs-stage`, otherwise at the first pad read after boot. | Boot is not deterministic (D2). Plan 2.3 seeds the RNG and resets the tick at the start barrier, which is what M5.3b needs. |
| D2 | Reaching 2P Battle | A port hook, `--vs-stage N`, leaves the title screen by itself and fills the VS config as the stage select does on confirm. | Replaying menus from boot needs a deterministic boot. The DVD thread, the card check and the sound system run beside the main loop, so the number of pad reads before the title differs per run. |
| D3 | First section | The run still goes through `Title::Section`. | It mounts resident archives and loads the game options that gameplay needs. |
| D4 | Load barrier | In a session, a DVD command from the main thread returns only when the DVD thread has finished it (pulled forward in M5.1a). M5.1b adds the `nonsim` wrappers around the real loading loops (plan 2.3 step 5) and runs the DVD thread's `rand()` on the simulation stream during that wait. There is no separate "loaded" bit barrier: with the wait, a load completes inside one tick on every peer. | Cave generation runs on the DVD thread and is part of the simulation; the main thread is blocked for the whole command, so its calls are serialised with the simulation. |
| D5 | How | Wait for the DVD thread rather than run the command inline (plan 2.3 step 5). | The callbacks expect the DVD thread's context. One same-line edit at `dvdThread.cpp:191`. |
| D6 | Tick | M5.1b: one `JUTGamePad::read()` inside the session and outside a nonsim region (plan 2.1, section 4 item 7). Loading passes are not ticks, and one released read runs at the end of each nonsim region. `Section::fadeIn` (draw only) counts as nonsim too. | The number of loading passes may now differ per machine. M5.1a counted every read, so its version 1 logs do not line up and are refused. |
| D7 | Done-test input | A synthetic two-player log (`netlog.py synth`), not a recorded one. `--record` is checked by recording while replaying. | No human and no second pad. The live record path was checked by hand (a paced keyboard recording replayed headless to identical checksums). |
| D8 | Reset combo | Same-line guards; `sResetPattern` is not cleared (section 4 item 8). | See [What a harness run changes](#what-a-harness-run-changes). |
| D9 | Hold and Smooth Motion | Both off in a harness run. | Extra sim ticks; `rand()` in draw. |
| D10 | Build id | FNV-1a 64 of the running executable (found through `/proc/self/exe`, `_NSGetExecutablePath` or `GetModuleFileName`); 0 means any build; a mismatch is a warning. | Real binary identity with no CMake change. The full `build_hash` is M5.3b. |
| D11 | Audio | Audio stays on in the reference runs; `MPPM_AUDIO=0` is a matrix cell (`audio-off`, also through a match end). | The section 4 item 10 audit ([M5_1B_AUDIT.md](M5_1B_AUDIT.md) (b)) found no simulation branch on audio state, and no `rand()` in sound code. |

Decisions added by M5.1b:

| # | Topic | Decision | Why |
|---|---|---|---|
| D12 | RNG streams | Chosen inside `p2_msl_rand` by thread and by two scope counters; no `rand()` call site is rewritten. A draw-time call outside a presentation scope goes to PRES and is counted, not asserted. | All call sites already reach one function; routing draws to PRES makes the simulation stream immune to the number of draws (Smooth Motion, tick order). |
| D13 | UI randomness | Tested, not proven: `MPPM_RNG_PERTURB` must leave the checksums unchanged. | 2D screens run inside the tick and their results can reach the simulation; a static proof over 147 files is not practical. |
| D14 | Sim k | 3.6 by default while the harness or an online session runs, stored in the log header; offline the window's k. | The simulation frustum must contain every render frustum, or objects at the screen edge are culled while visible. |
| D15 | Settings lock | Harness runs lock with defaults; M5.3b will lock with the host's values. `video.aspectRatio` is Local now. | One overlay with priority over every other layer; the aspect no longer reaches the simulation. |
| D16 | Tick order with Smooth Motion | Draw-first when Smooth Motion is in effect. | An interpolated picture is one tick behind by construction. |
| D17 | Golden logs | Tied to the toolchain; a golden difference is reported apart from a matrix difference. | A different compiler may give other checksums without any bug. |
| D19 | Late sampling and the HostFrames loop | The late-sampling wait also runs the HostFrames and Smooth Motion loop body (in-between pictures, UI frames); the tick wait keeps about 1 ms. Plan 2.10 item 1 says to skip the late sampling in HostFrames; only its Smooth Motion note asks for the merge. | The pad read comes before `waitForTick`, so the pictures must be drawn before the read or the read moves back by a whole wait. Frames may still start up to the tick's retrace (fix round), so the picture count matches `main`. |

**Visual checks** (design 4.4 and 10, D11 in the design: the direct-draw markers read
`mCurViewMatrix`; update-first runs before the GX set-up). A paced `Ls` replay held with
`MPPM_HOLD_AT_TICK` was screenshotted in update-first at tick 1500 and in draw-first at ticks 1500
and 1501 (M5.1b verification, build `3f91f1a3b`). All three show the world, the HUD and the life
gauges and counters on their objects in both viewports; none is black, the camera does not jump,
and nothing an update draws ends up under the scene. The update-first picture of tick 1500 differs
from the draw-first picture of tick 1501 in 254 pixels (7226 against draw-first 1500): update-first
is one tick ahead, as intended.

Plan items closed by M5.1b that M5.1a left open:

- **RNG reseed per floor.** Each further match of a session reseeds the simulation stream from
  `hash(sessionSeed, match index)` (`mppm: net: floor N reseed`).
- **Coverage of the done test.** The `Le` log of the matrix ends a match with a test event, so the
  result screen, a second load, the second VS inject, the nonsim regions and the DVD wait on a
  reload are exercised.
- **Audio coupling.** Audited ([M5_1B_AUDIT.md](M5_1B_AUDIT.md)); `audio-off` is a matrix cell.
- **Window size and settings.** The two cells that differed at the base commit are fixed by the
  simulation frustum and the settings lock.

## Reading a cmp-cksum report

```
differ at line 101, tick 3000
  A: C 3000 ... navi=00002328 piki=00000000 ...
  B: C 3000 ... navi=00002328 piki=0000f000 ...
  fields: piki
```

The first line gives the first tick on which the runs disagree, and `fields` names the
subsystems that differ there. Earlier ticks matched, so the cause lies between the previous
line's tick and this one.

- If only `rng` differs, something drew random numbers a different number of times without yet
  changing objects. A difference in `rng` on an `L` line points at a load screen.
- `counts` means an object was spawned or removed in one run but not in the other.
- `cam` alone means the cameras differ (the camera step, a view input) before any object does.
- `kind (C vs L)` means one run did a gameplay update where the other was loading.
- If one file is a prefix of the other, the report says which log ends first. Usually that run
  crashed or timed out; its `.log` file has the reason.

## Known limits

- Only sessions started with `--vs-stage` are deterministic. A boot session (a replay without a
  stage, which goes through the title screen and menus) is not expected to reproduce; online play
  bypasses the stock menus (plan 2.3).
- Story-only paths are not covered: the day-end result still steps the camera in its draw
  (`MainResultState::draw`), and THP movies and cutscenes need the M6 barriers.
- A golden checksum log belongs to one toolchain: another compiler may produce other checksums
  without any bug. Refresh the golden logs only after checking the change.
- The `--latency-probe` figure measured under Xvfb is not a display latency, because nothing is
  scanned out. See [Latency probe](#latency-probe) for what is still open.
- The checksum's mode hash covers the VS state id, both players' lose causes and the red, blue
  and draw counts. Version 1 checksum logs (M5.1a) do not verify against version 2 ones.
- The input log carries no Nintendo data. Keep disc images, extracted files and saves out of the
  repo; logs and outputs belong under `/var/tmp`.
