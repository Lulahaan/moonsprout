# M5.3b part 1: lockstep netcode (session protocol and game integration)

Milestone M5.3b of [ONLINE_MULTIPLAYER.md](../ONLINE_MULTIPLAYER.md) (section 8, the second "Step 3"
row; plan sections 2.1 to 2.11, 7b and 10) turns the deterministic harness of M5.1a/b, M5.2 and M5.3a
into online play: a host and up to three players (plus spectators) run the same simulation from the
same input bundles. This document covers the **lockstep package**: the session protocol on a net
thread, the pure host and client state machines, the connector, the game integration (input seam,
stall loop merged with late sampling, phase slew and grid jump, network barriers, VS and Challenge
inject), the launcher API and the in-process test bots. The transport (ENet over a socket shim with
ICE and the builtin relay) is [M5_3B_TRANSPORT.md](M5_3B_TRANSPORT.md), the lobby server and its
protocol `server/PROTOCOL.md`, the whole milestone and its done test [M5_3B_NETCODE.md](M5_3B_NETCODE.md).

With no online flag and no online UI action, nothing of this runs: the seam keeps its harness
behaviour, the retrace grid is exactly as before (no slew, no jump), `tools/net/desync_ci.sh` golden
logs still match, and 1-2 captain Battle recordings stay version 3 byte for byte.

## Files

| File | Content |
|---|---|
| `port/net/session/types.hpp` | seats, ticks (`P` = 2 x 1001/60000 s), `Input` (8 bytes), `SessionConfig`, end reasons |
| `port/net/session/wire.{hpp,cpp}` | message structs, encoders, bounds-checked decoders |
| `port/net/session/clock_sync.{hpp,cpp}` | offset estimator, one-way p99 trackers, phase controller |
| `port/net/session/delay.{hpp,cpp}` | the input delay formula and its change rules |
| `port/net/session/host_core.{hpp,cpp}` | the host state machine (pure: time passed in, outputs through `CoreEnv`) |
| `port/net/session/client_core.{hpp,cpp}` | the client and spectator state machine (pure) |
| `port/net/session/link.hpp`, `spsc.hpp` | game thread <-> net thread messages (`G2N`, `N2G`), lock-free rings |
| `port/net/session/endpoint.{hpp,cpp}` | one core wired to a `Transport` and a game link (net thread, bots, netsim) |
| `port/net/session/game_side.{hpp,cpp}` | the game side of a link: bundle table, readiness, barrier releases, stall statistics |
| `port/net/session/connector.{hpp,cpp}` | direct, ICE, relay and auto connection of a member to the host |
| `port/net/session/mem_transport.{hpp,cpp}` | an in-process `Transport` with the same impairment model (bots, self-test) |
| `port/net/session/bots.{hpp,cpp}` | `--net-test-bots N` |
| `port/net/session/build_hash.cpp` | the build hash of the handshake (D19) |
| `port/net/session/session.{hpp,cpp}` | the net thread, the command line, `mppm::online` (launcher API), the game-thread side for the seam |
| `port/net/net.cpp` | the seam: online exchange, records, checksums, `--ch-stage`, `.mpnl` version 5 |
| `port/main.cpp` | hold loop, readiness in the late-sampling wait, tick alignment, due times |
| `port/sdk/os_thread.cpp` | phase slew and grid jump of the retrace grid |
| `port/game/net/vs_boot.cpp` | Challenge inject, title exit into Challenge, session handicaps |
| `port/game/net/probe_patch.cpp`, `port/net/probe.cpp` | the latency probe reads the local seat; 16-pass limit online |
| `src/plugProjectKandoU/vsGS_Title.cpp:192` | the only `src/` edit: `PC_ONLY(if (p2_net_ch_inject(section, this)) { return; })` |
| `tests/net/session/` | `p2net_selftest`: wire, delay, sync and the netsim cases |
| `cmake/mppm_net_session.cmake` | the build-flags header, the default server URL, the self-test target |

## Threads

```
 game thread                                            net thread (session.cpp, 1 ms loop)
 JUTGamePad::read -> p2_net_exchange  --G2N-->  Spsc  --> Endpoint -> HostCore / ClientCore
   (sample port 0, push TickDue+input,                    wire encode/decode, bundles, barriers,
    wait ready, copy bundle, record)                      sync, delay, desync, drops, lobby room
 p2_net_nonsim_leave (depth 0) -> Loaded                     |                  |
 p2_host_present -> pre_read_wait     <--N2G--  Spsc  <--  Transport (ENet)  LobbyClient (ws)
   (wait due-1 ms and ready, hold loop)                   Connector (direct / ICE / relay)
 p2_net_game_poll (START, END)                            Bots (MemTransport) with --net-test-bots
 launcher -> mppm::online::* --commands (mutex)-->        snapshot (mutex) for the UI
 os_thread timerMain: nextRetrace += P/2 + clamp(slew, +-1 ms) <- p2_vi_phase_slew_set / grid_jump
```

The cores never read a clock, sleep or touch a socket: every input carries the time and every output
goes through `CoreEnv` (send, disconnect, to-game message, note, slew). The self-test runs the same
cores under a virtual clock over `MemTransport`.

## Session flow

1. **Lobby.** The host creates a lobby on mppm-server (`mppm: net: lobby created code=ABCD-EFGH
   server=<url>`, written to `--code-file`); members join by code (`mppm: net: lobby joined code=...
   member=<id> server=<url>`). The connector joins each member's transport peer to the host's
   (section "Connector"). On `Connected` the member sends HELLO (member id, its join secret, name,
   spectator flag, build hash, content hash, mod list); the host checks protocol, secret, room and the
   started flag, answers WELCOME or REJECT, seats players in the lowest free seat, and broadcasts LOBBY.
   A connection is bound to the member its ENet `connectData` names only by such a HELLO (section
   10): before it, the host reads nothing but a HELLO from it, and it never replaces the connection of
   a member already in. A second connection with the right secret replaces the first in the lobby and
   is refused after START; a connection with no verified HELLO within 5 s is dropped. For ICE peers the
   member id is the one that signalled the agent, not the remote's `connectData`.
   Unverified connections are capped (section 10, fix-min): one is kept only for a member id the lobby
   has admitted (MemberJoined seen, not kicked or banned, not gone), at most 2 per source address
   (`pending_source_key`: the IP of a direct peer; an ICE or relay path is per member; loopback is not
   capped), at most 2 per member id and at most 4 in all (`kMaxPending*` in `host_core.hpp`, below the
   transport's 8 peer slots, so a real member can always connect). Any other connection is let go at
   once (`connection claiming member <m> refused: <why>`). A kicked or banned id is remembered: its
   pending connections go and a later MemberJoined for it is ignored. Self-test case `pendingcap`.
2. **Room.** REQ messages take a seat, set ready, or leave. A seat request for a seat held by a member
   the host seated automatically swaps the two (several `--slot` requests in any join order would
   otherwise block each other). A member whose build or content hash differs stays with a note
   ("Louie has different mods: x 1.2.0 (host 1.1.0).") and cannot be ready. `canStart` = every seat
   of the mode taken by a connected, ready, matching member. When the host picks a mode with fewer
   seats (2v2 -> Battle or Challenge), every member whose seat is gone moves to the lowest free seat or
   watches (toast "<name> is watching" on the host, "You're watching" on that member); the host
   always plays and, with no seat free, takes the highest seat of a member it seated automatically
   (else the highest seat), whose member then watches. No seat ever holds two members, so START seats
   exactly what the room showed. Self-test case `modeswitch`.
3. **Start.** The host sends START (the SessionConfig). With `--ready` (or the test bots) it starts by
   itself once `canStart` holds, `--expect` members are connected and every seated member has
   reported its downlink and been connected for 1.5 s (the 30 Hz exchange before START warms the
   clock estimate and the one-way delays, so the first delays come from measured paths).
4. **Apply.** `p2_net_game_poll` (game thread, once per pass; in the standalone start menu, before any
   game exists, the menu's frame loop does it, so the take-off can boot the game) applies START: `p2_launcher_net_lock`
   with the host's Synced settings (a refusal leaves the session), `p2_net_set_synced_hash`, the seam
   turns on, `p2_net_set_online(1)`, `p2_launcher_online_started()`. Log
   `mppm: net: online start mode=... stage=... seats=... seat=... code=... seed=...`.
5. **Title exit and inject.** The title leaves by itself into Battle (`Select_Vs`) or Challenge
   (`Select_Challenge`); `p2_net_vs_inject` / `p2_net_ch_inject` fill the section from the session
   (stage or course, seats, handicaps) and call `p2_net_session_begin`: seed = the session seed, tick
   0, logs open (the recording always), and the **start barrier** (Loaded at tick 0, see Barriers).
6. **Ticks.** Tick t = the t-th session read. At read t a player samples physical port 0 (or record
   t-1 of its seat in `--net-input`) and the net thread tags it t + d. Each player sent neutral inputs
   for tags 1..d at START. The host completes bundle X the moment every seat's input for X is in and
   forwards it at once (forward-on-complete) with each peer's missing window.
7. **End.** `--exit-after-ticks N`: each peer ends after tick N; the host lingers until every client
   and spectator acknowledged bundle N (at most 10 s), then sends END. A desync, the host's loss or a
   drop of this seat end the session everywhere it applies (section "Desync, drops, end").

## Wire format

Every datagram starts with an 8-byte header: `u8 0xB5`, `u8 version 1`, `u8 type`, `u8 flags`,
`u32 sessionTag` (0 before START; a datagram with another session's tag is dropped silently).
Little-endian; ENet carries the length; u32 microsecond fields wrap (differences are `int32`).

Control channel (reliable, ordered):

| Type | Name | Direction | Payload |
|---|---|---|---|
| 1 | HELLO | client -> host | `u16 proto`, `u32 member`, `u64 secret`, `u8 spectator`, `u8 len` + name (<= 24 code points, <= 96 bytes), `u8 build[32]`, `u8 content[32]`, `u16 len` + mods (<= 1023) |
| 2 | WELCOME | host -> client | `u32 member`, `u8 role` (1 player, 2 spectator) |
| 3 | REJECT | host -> client | `u8 reason` (1 protocol, 2 auth, 3 full, 4 banned, 5 started), `u16 len` + text (<= 200) |
| 4 | LOBBY | host -> all | `u32 generation`, `u8 phase`, `u8 mode`, `u8 stage`, `u8 seats`, `u8 handicap[2]`, `u8 count`, per member `u32 id`, `u8 slot` (255 none), `u8 flags` (ready, spectator, contentOk, host, connected), `u16 pingMs`, `u8 delay`, `u8 link`, `u8 len` + name (as in HELLO), `u8 len` + note (<= 120 bytes) |
| 5 | REQ | client -> host | `u8 what` (1 seat, 2 ready, 3 leave), `i8 value` |
| 6 | START | host -> all | SessionConfig: `u32 sessionTag`, `u32 seed`, `u8 mode`, `u8 stage`, `u8 seats`, `u8 slotMask`, `u32 slotMember[4]`, `u8 handicap[2]`, `u8 tickOrder`, `f32 simK`, `u16 cksInterval`, `u8 maxDelay`, `u32 syncedHash`, `u16 len` + Synced JSON (<= 2048), `u8 build[32]`, `u8 content[32]`, `u16 len` + mods |
| 7 | RELEASE | host -> all | `u32 barrierTick`, `u16 count`, `u64 releaseHostNs` |
| 8 | DROP | host -> all | `u8 slot`, `u32 effectiveTick`, `u8 reason` (1 left, 2 timeout, 3 kicked, 4 by host) |
| 9 | DESYNC | host -> all | `u32 tick`, `u8 slot` (255 spectator), `u32 hostTotal`, `u32 peerTotal` |
| 10 | CKSLINE | peer <-> host | `u32 tick`, `u16 len` + checksum line (<= 300) |
| 11 | END | host -> all | `u8 reason` (end reason), `u16 len` + text |
| 12 | BYE | any | `u8 reason` (1 left, 2 done, 3 error, 4 desync) |
| 13 | CHUNK | any | `u8 index`, `u8 count` (2..16), up to 1000 bytes: a control message above 1100 bytes in parts (the transport caps a datagram at 1200 bytes; START with a long Synced or mod list and a full LOBBY can be longer). The receiving endpoint reassembles it before the core sees it. |

Input channel (unreliable, unsequenced, flushed at once; the newest packet goes again 16.7 ms later,
and one goes at least every 33 ms):

- **INPUT** (32, client and spectator -> host), 50 bytes + 8 per input: `u32 tSendUs`, `u32 echoUs`,
  `u32 holdUs`, `u32 ackBundle` (every bundle <= it received), `u32 firstTag`, `u8 count` (0..32),
  `u8 slot` (255 spectator), `u8 delay`, `u8 flags`, `u32 loadedTick`, `u16 loadCount`, `u32 cksTick`,
  `u32 cksTotal`, `u16 downP99` (0.1 ms), `u16 stallPermille`, then `count` inputs.
- **BUNDLE** (33, host -> client and spectator), 64 bytes + bundles + events: `u32 tSendUs`,
  `u32 echoUs`, `u32 holdUs`, `u32 hostTick`, `u32 hostDueUs`, `u32 firstTick`, `u8 count`,
  `u8 slotMask`, `u8 nEvents` (0..8), `u8 flags` (bit 0 release valid), `u32 releaseTick`,
  `u32 releaseHostUs`, `u16 releaseCount`, `u8 waitMask`, `u8 pad`, `u32 reserved`, `u8 upP99[4]`,
  `u8 downP99[4]` (ms, 255 unknown), `u8 blame[4]` (permille / 4), then `count` bundles of
  `popcount(slotMask) x 8` bytes (seat order), then `nEvents` x `{u32 tick, u8 id, u8 arg, u16 seq}`
  (id 1 = drop notice, arg = seat). Largest: 64 + 32 x 32 + 64 = 1152 bytes.

Validation (plan section 10): a datagram shorter than its header or than its fields imply, or with
`count > 32` / `nEvents > 8`, is **malformed** and its sender is disconnected (a seated player is then
dropped); an unknown type, a wrong direction, a name or list over its limit, a slot other than the
sender's seat, inputs from a spectator or tags above `completed + 64` are **invalid**: dropped and
counted, and more than 50 in one second disconnect the sender. Tags at or below the newest complete
bundle are normal redundancy and ignored.

## Bundles and readiness

- The host keeps every bundle of the session and, per seat, a 128-entry ring of inputs by tag. Bundle
  X is complete when every seat has its input for X (a dropped seat is neutral from its effective
  tick). On completion it sends each client and spectator its window from `ackBundle + 1` to the
  newest complete bundle (at most 32; normally 1-8), schedules the duplicate send, and hands bundle X
  to its own game thread. `waitMask` = the seats whose input for the oldest incomplete bundle is missing.
- A client stores every received bundle once (any order) and hands it to its game thread; its ack is
  the newest contiguous one. It resends its own unacknowledged tags (from `ack + 1`, at most 32) in
  every INPUT.
- `p2_net_tick_ready(t)` (game thread) = bundle t is in the game side's table, no unreleased barrier
  before t, and at the first tick after a release the release time minus the late-sampling margin
  reached.
- Bundle t is written into all four `PADStatus` slots: seated ports get the 8 bytes (`err` 0, analog
  A/B 0), the other ports are released with `PAD_ERR_NO_CONTROLLER`. Pre-session and nonsim reads are
  as in the harness (released, `err` live, D18).

## Timing: late sampling, the stall loop, the hold loop

- Online, the end of every armed pass (`pre_read_wait` in `p2_host_present`) waits until
  `due(T+1) - 1 ms` **and** `p2_net_tick_ready(T+1)`. Until `due(T+1)` it polls readiness every
  0.5 ms (and keeps the one-slot retrace queue free, as the late-sampling wait always did); past
  `due(T+1)` it becomes the **hold loop** `p2_host_net_hold`. With `MPPM_LATE_SAMPLE=0` it still runs,
  readiness only.
- The seam has the same hold as a fallback when a read finds its tick not ready (the first read after
  a barrier, a missed wait). In a session that ended, a command-line run gives neutral input and
  exits; the UI keeps holding over the frozen picture until "Back to start menu".
- `p2_host_net_hold(ready, arg)`: UI frames over the last picture at the UI rate
  (`aurora_preserve_frame`, `aurora_end_frame`, `beginHostFrame`, `p2_wide_update`,
  `launcher::update`, so the wait overlay animates and takes input), `p2_net_game_poll`, and between
  frames `ready(arg)` every 0.5 ms (sleeping inside `p2_host_block_begin/end`). No UI frame starts in
  the last 20 ms before a known tick time (a barrier release), so the read is not late behind a
  present. It returns 0 when the window asked to quit. The game's own sound frame work does not run
  during a hold.
- A tick is **stalled** when its read waited past its due time for readiness (a read that is late but
  finds its bundle is no stall; the first tick after a release counts as barrier time).

## Barriers (D10)

- A barrier is reported with its tick B and its number n (outermost nonsim leaves since the session
  start; deterministic, so peers agree): `p2_net_nonsim_leave` at depth 0 in an online session pushes
  `Loaded(B)`; INPUT carries `loadedTick` and `loadCount`.
- **Start barrier.** Battle and Challenge load their stage inside sim ticks (LoadState with
  synchronous DVD commands), so no nonsim region follows the inject. `p2_net_session_begin` therefore
  reports the inject itself as barrier 1 at tick 0: tick 1 runs at the host's release time on every
  peer. Any later nonsim region is a further barrier.
- The host releases barrier n when every seated, not dropped player (itself included) reported
  `loadCount >= n`: RELEASE(B, n, G) on the control channel and in every BUNDLE header while it is the
  newest; G = host now + clamp(1.5 x max seated downlink p99 + 10 ms, 30 ms, 300 ms).
- A client converts G to its clock (`G_local = G - offset`). The game thread takes the release inside
  `ready(B+1)`: `C = p2_vi_grid_jump(G_local)` (the next retrace fires at G_local, or now when that is
  past; the slew is cleared), `p2_host_align_tick(C)` (the next armed tick wait accepts no retrace
  below C, and the retrace queue's slot is freed so retrace C can be posted), and readiness of B+1
  waits for `retraceTime(C) - margin`. Every peer reads about 1 ms before G and runs tick B+1 at G.
  Logged: `mppm: net: barrier tick=<B> waited_ms=<ms> jump_us=<signed us>`.
- Spectators never report and are never waited for; they apply releases like players.

## Time sync and phase (D11)

- **Offset.** Every INPUT and BUNDLE carries `tSend`, the echo of the newest `tSend` received from
  that peer and the hold time. Per sample `rtt = (t3 - t0) - hold` and `offset = ((t1 - t0) + (t2 -
  t3)) / 2` with both halves kept modulo 2^32 us (two machines' clocks may be days apart). Of the
  samples of the last 2 s, the offsets of those within 1.5 ms of the minimum RTT are averaged and the
  result is smoothed (about 1 s time constant). Measured in the self-test: worst error 0.44 ms with
  8 ms symmetric jitter (the plain minimum-RTT sample gave 1.5 ms).
- **One-way delays.** The host measures each client's uplink from INPUT, each client its downlink
  from BUNDLE (`recv - (tSend - offset)`), p99 over 5 s; clients report theirs in INPUT, the host
  broadcasts every seat's up/down p99 (ms) in every BUNDLE.
- **Phase.** For each BUNDLE a client computes `e = dueLocal(hostTick) - (hostDue - offset)`, not
  wrapped (`dueLocal` from its own TickDue ring, projected up to 16 ticks ahead; |e| above 16 ticks is
  no sample). The stage load runs inside sim ticks, so after its long ticks the two grids are whole
  retraces or ticks apart; a stall does not move the grid, so a lagging client would make the host
  stall on every tick for the rest of the session. Whole ticks of a correction (from one tick on,
  rounded) move the game's tick grid at once (`p2_host_tick_shift`: a late client catches up one tick
  per retrace, an early one waits once; logged `mppm: net: phase shift ticks=<n> rest_us=<us>`, no
  phase samples for 0.4 s after), and the slew corrects the rest. The
  controller updates at most every 250 ms with at least 3 samples: new remaining correction = current
  remaining - median(e + remaining-at-sample) (samples taken with no correction pending preferred; a
  residual under 0.2 ms is left alone). `os_thread.cpp` applies `step = clamp(remaining, -1 ms,
  +1 ms)` per retrace where the grid advances (`nextRetrace += period + step`, compare-exchange so a
  concurrent set is not lost); the catch-up rule is unchanged. `p2_vi_retrace_time_ns` of a future
  retrace includes the step already in the next retrace and up to 1 ms per further retrace of the
  remaining correction: unslewed, the late-sampling wait slept past an early retrace while the
  one-slot retrace queue still held the previous count, and every tick took three retraces. The host never slews. Clients log once
  per second `mppm: net: sync tick=<T> offset_us=<o> rtt_us=<r> phase_us=<e> slew_us=<remaining>`.
- `p2_vi_grid_jump(localNs)` (under the scheduler lock): next retrace = max(localNs, now), slew
  cleared, the published grid (seqlock) updated so `p2_vi_retrace_time_ns` of that count returns the
  new time at once, the timer woken; returns the count. Offline nothing calls these.

## Input delay (D12, D13)

- `d = clamp(ceil((path_p99 + 2 ms + 1 ms) / P), 1, 8)`, path = own uplink (host: 0) + the largest
  downlink p99 over **all** seated players, this one included (deviation from D12's "other": a
  2-player client's own bundle comes back over its own downlink, and without it its delay ignored its
  round trip and stalled every tick).
- Recomputed every second from the newest table. An increase applies at the next read (the skipped
  tags are sent as copies of the previous input); a decrease waits for the next barrier release (the
  reads whose tag would not be new are dropped, one per step).
- Blame: the host marks a completed bundle late when it cannot reach the slowest seat by the host's
  due time, and blames the seat whose input came last (not right after a release, and not when that
  is the host and its own read was late, i.e. its own long tick); it broadcasts each seat's blame rate
  over 3 s. A sender above 2% adds 1, at most once per 4 s after any change, never in the first 16 s
  of a session (the stage load); the extra goes again at a barrier when the blame is low.
- Every change logs `mppm: net: delay slot=<s> d=<d> path_us=<p> reason=init|path|stall|boundary`; a
  wish above 8 logs `... reason=path capped` and, once per session, shows a warning toast ("Your
  connection is slow", on the host "A connection is slow": "The input delay is held at its limit of 8
  ticks, so the game may pause now and then.").

## Connector (D6)

| Mode | Host | Client |
|---|---|---|
| direct | on MemberJoined: signal `addr "direct a1;a2;..."` (`local_addresses()`) | connects to each address for 1.5 s in order |
| relay | registers with the builtin relay at lobby creation (always, when offered) | `relay_register`; on RelayRegistered connects to `<relay ip>:<port>` |
| ice | on `desc`: `ice_create(false)`, remote description, answer `desc`, gather, trickle `cand`, `done` | `ice_create(true, member)`, `desc`, gather, trickle; the transport connects ENet when ICE connects |
| auto | ICE servers listed: ice; else relay offered: direct (LAN addresses, then public addresses from RelayRegistered exchanged by `addr public` signals, with punches) then the relay; else direct | same |

A client with no connection after 15 s ends ("Couldn't connect to the host.", exit 9). Logged:
`mppm: net: peer <member> connected link=direct|ice|relay remote=<addr>` and
`mppm: net: peer <member> disconnected reason=<text>`.

## Desync, drops, spectators, end

- **Checksums.** Online the seam computes the checksum every 30 ticks whether or not a log is open
  (C lines in game sections, L lines elsewhere), keeps the last 64 lines and hands `(tick, total,
  line)` to the session; INPUT carries the newest. The host compares each peer's value with its own
  (it keeps 128). On a mismatch it sends DESYNC and its own line to all and stops forwarding; every
  peer sends its CKSLINE, waits up to 0.5 s for the host's line, writes its report
  `<pref>/net/reports/desync-<code>-t<tick>.txt` (build and content hashes, mods, seed,
  SessionConfig, seat, the host's line, the peers' lines at the host, this machine's last 64 lines,
  the recording) and ends: `mppm: net: desync at tick <T> (slot <s>: host <8 hex> peer <8 hex>);
  report <path>`, exit 7. A spectator that differs is told alone (slot 255) and stops ("Your game went
  out of sync at tick <T>. The players go on without you."); the players go on. Self-test case
  `specdesync`.
- **Drops.** A seated player's peer disconnects (BYE, ENet timeout, malformed or flooding traffic), or
  sends no packet for 8 s, or its game is stuck, or the host drops it (`drop_slot`, also at a load
  barrier): E = max(highest contiguous tag of the seat, newest complete bundle) + 1; bundles from E on
  carry neutral input for the seat; DROP to all and a drop notice event in bundle E;
  `mppm: net: drop slot=<s> tick=<E> reason=left|timeout|kicked|host`. The dropped machine, if it
  still runs, ends (exit 8).
- **Stuck games** (fix round 2). The net thread keeps sending keepalive INPUTs (count 0) while its game
  thread hangs, sits on the crash screen or never reaches the inject, so packets prove nothing; the host
  judges progress (`HostCore::check_stuck`, every pass): mid-match, a seat whose input the host's own
  game has been waiting for (it read the tick; a host that is loading or in a long tick of its own
  reads nothing, so nobody is timed then) for 10 s without a bundle completing is dropped with
  `peer <m> made no progress for 10 s`, reason `timeout`; at a load barrier, a seat still loading 30 s
  after the host reached it (60 s for the start barrier, where every peer boots its game first) goes
  with `peer <m> did not finish loading within <n> s`. The crash screen (`p2_net_crash_stop`) also
  makes the net thread say BYE (error) at once (`GameLink::leaveReq`, set from any thread), so a
  crashed client is dropped at once and a crashed host ends the session; a headless run waits 300 ms
  for it before it exits 5.
- A peer that says BYE after its last tick (`--exit-after-ticks`) is not reported as a drop. If the
  host still waits for that seat's input (the client's last tick came first), the seat is dropped
  after 1 s of waiting (`seat <s> ended its session before the host's last tick`).
- **Host loss.** A client whose host is gone (transport, END, BYE) keeps its
  received bundles and ends when it needs one that cannot come (`session end reason=host_left`, exit 8;
  `p2_net_game_poll` notices it, so the UI's "host left" message comes up while the picture is held);
  with every bundle up to its own last tick (`--exit-after-ticks`) it ends normally. The lobby's
  `closed host_left` only says the host's lobby WebSocket is gone: before START it ends the session;
  after START the P2P link decides, as on the host for its members (`ClientCore::lobby_host_left`):
  while the host's packets keep coming the match goes on (`lobby says the host left; the match ends
  once its connection is silent`), and once the link has been silent for 1 s the host counts as gone
  (`the host's connection is silent too`). Self-test case `hostleft`.
- **Remove (kick).** The host's Remove sends END (kicked) to the member and drops it: in the lobby its
  seat opens; after START its seat is dropped with reason `kicked` and one E everywhere, the removed
  machine ends (`session end reason=dropped`, exit 8) and the others play on. Self-test case `kick`
  (the done test does not remove anyone).
- **Spectators** join in the lobby only, get START, consume bundles, ack them and send time sync and
  checksums, and are never waited for.
- **Lines.** `mppm: net: session end reason=done|desync|host_left|dropped|left|error` (printed when
  the session ends, so a UI peer that restarts has it too; once), then at shutdown
  `mppm: net: summary ticks=<T> stalled=<k> rate=<pct> max_ms=<ms> barriers=<n> barrier_ms=<ms>
  phase_p50_us=<n> phase_p99_us=<n> phase_max_us=<n> d=<seat>:<d>,... d_max=<n> rtt_us=<n>
  kbps_out=<n> kbps_in=<n> link=<direct|ice|relay|host>` (phase from |e| samples at least 1 s after the
  newest release, -1 on the host; the host lists every seat's d, a client its own). Every 900 ticks:
  `mppm: net: stall window_ticks=900 stalled=<k> rate=<pct> max_ms=<ms> barrier_ms=<ms>`.

## Seam changes (`port/net/net.cpp`)

- `p2_net_init` parses the online flags (`session::cli_init`); an online command-line run turns the
  seam on and starts the net thread at once (the lobby work waits for the booted game: the content
  hash and the Synced settings come from it).
- Online `p2_net_exchange`: push TickDue and the local input, wait for readiness (fallback hold),
  write bundle t into the four slots, record (12-byte slots, event 0), `MPPM_NET_TEST_DESYNC`, `++tick`.
  Before the session (an online command-line run, or after START) every read is released, as in a
  `--vs-stage` run.
- Session-aware getters: `p2_net_vs_stage` (Battle modes), `p2_net_ch_stage`, `p2_net_vs_players`
  (2 or 4), `p2_net_vs_handicap`, `p2_net_tick_order`, `p2_net_sim_k` (the SessionConfig),
  `p2_net_view_captain` (the local seat with 4 seats, captain 0 for spectators, unless
  `--view-captain`), `p2_net_checksum_due` (every 30 ticks online), `p2_net_should_exit` and
  `p2_net_exit_code` (D30).
- Recording (D24): always online, to `--record` or `<pref>/net/sessions/<code>-<yyyymmdd-hhmmss>.mpnl`
  (the 5 newest kept); flags bit 1 = online.
- The synced hash of START is the launcher's `net_synced_hash` over the host's Synced JSON (the same
  key=value lines, sorted, FNV-1a 32), so an offline replay of a session with default settings
  matches it.

### `.mpnl` version 5

Version 5 = the version 4 layout plus start mode 2 (2P Challenge, course at byte 27) and the VS
handicaps at bytes 34 (Red) and 35 (Blue), 0 = stock; byte 33 = captains. Written only for a
Challenge session or non-stock handicaps; everything else stays version 3 (version 4 with 4
captains) byte for byte. The engine reads 2 to 5; a version 5 replay drives `--ch-stage` and the
handicaps from its header (a differing flag prints one warning; the header wins).

### `--ch-stage N`

`--ch-stage N` / `MPPM_CH_STAGE` (0..29): direct boot into 2P Challenge course N, the session starts
at the Challenge inject (`p2_net_ch_inject`, behind the one `src/` edit), start mode 2 in a version 5
log. `--vs-stage` with `--ch-stage` exits 2. Offline determinism of course 0 (5400 ticks): two
headless replays, the `MPPM_RNG_PERTURB` cell and a paced replay give identical checksum logs,
`other-in-session=0`.

## Host changes (`port/main.cpp`, `port/sdk/os_thread.cpp`)

- `p2_host_next_tick_due_ns()`: after `p2_host_align_tick(C)` the time of retrace C; else, when an
  armed tick wait set it since the last unarmed one, the time of the next armed tick's retrace; else 0.
- `p2_host_align_tick(C)`: the next armed tick wait (HostFrames and Game mode) drops retrace messages
  below C (and the held one); cleared once a tick is accepted.
- Fixed tick grid online (integration): offline a tick accepted late moves every later tick (next =
  accepted + 2, the original rule). In an online session the next tick is due at previous + 2, so a
  long tick of up to 4 ticks is caught up (one tick per retrace) and leaves no phase difference; from
  4 ticks behind (the stage load) the grid is rebased as before and the phase controller corrects it.
  `p2_host_tick_shift(n)` moves the grid n ticks earlier (caught up the same way) or later.
- `p2_net_game_poll()` once per pass before `launcher::update()`, and in the hold loop.
- `p2_host_late_sample_margin_ns()` (internal): the late-sampling margin for the barrier readiness.
- Phase slew and grid jump as above; `p2_vi_phase_slew_get`.

## Command line and environment

Flags win over the environment; `--flag value` and `--flag=value`.

| Flag | Environment | Meaning |
|---|---|---|
| `--server URL` | `MPPM_SERVER_URL` | lobby server (overrides `online.serverUrl`) |
| `--host` | - | create a lobby at start-up |
| `--join CODE` | - | join that lobby at start-up |
| `--spectate` | - | with `--join`: as a spectator |
| `--name TEXT` | `MPPM_NET_NAME` | player name (overrides `online.playerName`) |
| `--password TEXT` | - | lobby password |
| `--mode battle\|2v2\|challenge` | - | host: the mode |
| `--stage N` | - | host: VS stage or Challenge course (0..29) |
| `--handicap R,B` | - | host, battle: handicaps 1..10 |
| `--slot N` | - | ask for seat N |
| `--ready` | - | ready once seated; the host also starts as soon as it can |
| `--expect N` | - | host with `--ready`: wait for N connected members (host and spectators included) |
| `--code-file PATH` | - | host: write the code (`ABCD-EFGH\n`) |
| `--net-input FILE` | `MPPM_NET_INPUT` | local input from this seat's records of an `.mpnl` log (record t-1 at read t; neutral after it) |
| `--net-impair SPEC` | `MPPM_NET_IMPAIR` | send-side impairment, test only (`delay=46ms,jitter=8ms,loss=2%,dup=0.1%,seed=7`) |
| `--net-mode auto\|direct\|ice\|relay` | `MPPM_NET_MODE` | connection order; `MPPM_NET_FORCE_RELAY=1` = relay |
| `--net-bind ADDR` | `MPPM_NET_BIND` | UDP bind address (tests: 127.0.0.1) |
| `--net-allow-plaintext` | `MPPM_NET_ALLOW_PLAINTEXT=1` | `ws://` to a non-loopback server |
| `--net-test-bots N` | - | host without a server: N (1..3) in-process bots take seats 1..N |
| `--ch-stage N` | `MPPM_CH_STAGE` | harness: direct boot into 2P Challenge course N |
| | `MPPM_NET_ICE_RELAY_ONLY=1` | ICE with relay candidates only |
| | `MPPM_NET_TEST_DESYNC=T` | XOR this peer's SIM RNG state with 1 at the read of tick T |
| | `MPPM_NET_TEST_LEAVE_AT=T` | leave gracefully after tick T (BYE, `session end reason=left`, exit 0) |
| | `MPPM_NET_TEST_HANG_AT=T[:MS]` | the game thread sleeps MS ms (default 120000, at most 600000) after tick T while the net thread runs on: a stuck game for the host's progress rule |
| | `MPPM_NET_TEST_BOT_LOAD_MS=N` | the bots report each barrier N ms late |
| | `MPPM_NET_TEST_SLEW_NS=N` | `p2_vi_phase_slew_set(N)` once at the first session tick; logs the measured grid shift 12 retraces later (`test slew shift_us=...`) |

An online run prints `mppm: net: online (server=<url>, role=host|join|bots, mode=..., net-mode=...,
bind=...)` at start-up. `--host`/`--join` with `--replay` or `--headless-ticks` exits 2 ("online
sessions run in real time and cannot replay"); `--host` with `--vs-stage` or `--ch-stage` exits 2.

## Exit codes (command-line online runs, D30)

| Code | When | Line |
|---|---|---|
| 0 | `--exit-after-ticks` reached, window closed, a test leave | `session end reason=done` (`left`) |
| 2 | bad or conflicting flags | the reason |
| 7 | desync | `desync at tick ...`, then `session end reason=desync` |
| 8 | host left, this seat dropped or removed, connection lost, settings refused | `session end reason=host_left\|dropped\|error` |
| 9 | lobby failure: server unreachable or refusing, refused at Hello, connect timeout | `mppm: net: lobby failed error=<code> msg=<text>` |

The harness codes (3 checksum mismatch, 4 log write error, 6 `MPPM_RNG_STRICT`) still apply after a
normal end.

## Launcher API (`mppm::online`, `port/include/p2_online.hpp`)

Implemented in `session.cpp`. Commands go to the net thread through a mutex-protected queue and
return at once; `snapshot()` and `take_notices()` copy what the net thread publishes under a mutex;
`stall()` is built on the game thread from its wait state and the host's `waitMask` /
`loadingMask` ("Louie (P2)"; empty = the host itself is late; `canDrop` for the host after 5 s,
`canLeave` for clients after 10 s). Strings follow the launcher's copy style ("Waiting for Louie to
get ready.", "Couldn't reach the lobby server.", "No game uses that code.", "This lobby has a
password."). The server URL resolves `--server`, else `online.serverUrl` (`configure`), else
`MPPM_DEFAULT_SERVER_URL` (empty in part 1: "No lobby server is set. Choose one in Settings ›
Online."). `leave()` sends BYE (and closes the lobby for the host) and lets the net thread finish
within 0.2 s; the object is reaped on the next call.

A session created from the standalone start menu (no game booted yet) offers the content hash and
mod list of the enabled mods in the folder, read on the net thread with `mods::scan` and
`mods::content_hash` without mounting (plan 7b). The mount can still drop a mod over a disc-dependent
check: once the game has booted, `game_poll` compares `p2_mods_content_hash` with the hash it sent,
and on a difference logs `content after the boot differs from the one sent in the handshake; leaving`
and leaves the session ("The mods that loaded differ from the ones this game was joined with.")
instead of playing into a desync.

## Test bots and netsim

- `--net-test-bots N`: the host runs without a lobby server; N bots (ids and seats 1..N, names "Bot
  n") are in-process clients on a `MemTransport` hub, with the `--net-impair` model (seed + bot
  index). Each bot reads tick t once the host's game has read it (so it never passes a barrier the
  host holds), plays its seat of `--net-input`, acks bundles, and reports each barrier as soon as the
  host's game did (`MPPM_NET_TEST_BOT_LOAD_MS` later). The bots start the match by themselves.
- `p2net_selftest` runs the same cores in a virtual-clock simulation: every endpoint has its own
  steady clock (offsets of seconds to days), its own retrace grid (slew per retrace, grid jump, tick
  alignment, the one-slot queue's "late tick slips to the next retrace" rule) and a fake game that
  reads 1 ms before each due time, pushes a synthetic input, consumes bundles, runs loads of given
  lengths at given ticks and checksums its consumed stream.

## Tests (before integration)

| Test | Command | Result |
|---|---|---|
| self-test | `ninja -C build-pc p2net_selftest && ./build-pc/p2net_selftest -v` (ASan + UBSan) | PASS (23 cases since fix-min, which added `modeswitch`, `hostleft`, `utf8`, `pendingcap`, `kick` and `specdesync`, and changed `hijack`: a connection for a member the lobby has not admitted is let go at once; 16 cases since fix round 2, which added `stuck`: a seat whose game hangs while its endpoint keeps sending is dropped 10 s into the host's wait mid-match, 30 s after the host reached a load barrier and 60 s at the start barrier, with the same E on every survivor; and to `leave` a BYE "done" before the host's last tick (dropped after 1 s) and the crash screen's `leaveReq` (dropped at once). Fix round 1 added `hijack`: a connection claiming a welcomed member's id neither displaces it nor sets it ready, a wrong secret is refused, a silent connection goes after 5 s, Hellos before MemberJoined are sorted out then; about 7 s): wire (round trips, sizes, 200 000 fuzzed datagrams), delay (table 0/15/58/116/300 ms -> 1/1/2/4/8, rules), sync (offset 0.44 ms worst with 8 ms jitter; 10 ms phase error under 1 ms in 0.23 s, overshoot 0.3 ms, p99 0.3 ms), lockstep2/lockstep4 (18 000 ticks, 46 ms + 8 ms jitter each way: identical streams, 0 stalls, d 2 host / 4 clients, phase p99 0.28 ms), loss 2% (stall 0.02%), barrier (B+1 within 0.35 ms across endpoints, loads up to 2 s apart, clock offsets of 123 s), drop (seat 3 neutral from the same E on all), desync (all end kEndDesync at 900), spectator (a 150 ms spectator never delays the players), delaychange (d 1 -> 5 in 0.5 s, lowered only after the barrier), lobby (a content mismatch keeps the host from starting with the note "P1 has different game files.", a wrong join secret is refused), leave (graceful BYE at tick 900: E = 901, the host goes on without a stall), chunk (a 2 KB Synced JSON and a 60-mod list reach every peer in parts) |
| bots, 2P Battle | `--host --net-test-bots 1 --mode battle --stage 0 --net-input S2.mpnl --net-impair delay=46ms,jitter=8ms --record h.mpnl --checksum-log h.cksum --exit-after-ticks 3600` | exit 0, 1 barrier, stall 0.03%, d 0:2,1:4; recording version 3 with the online flag; headless replay with `--checksum-verify h.cksum` exit 0, checksum logs identical |
| bots, 2v2 | the same with `--mode 2v2 --net-test-bots 3` and a 4-port log | exit 0; version 4, 4 captains, 120 C lines with `n=4/`; replay verifies |
| bots, Challenge | the same with `--mode challenge` and a version 5 synthetic log | exit 0; version 5, start mode 2, course 0; replay verifies |
| bots, barrier | `MPPM_NET_TEST_BOT_LOAD_MS=2000` | `barrier tick=0 waited_ms=2050` (0 stalls: the wait is barrier time) |
| bots, handicaps | `--mode battle --handicap 3,7` | version 5 recording, start mode 1, handicaps 3/7 at bytes 34/35; its headless replay verifies |
| bots, test leave | host with `MPPM_NET_TEST_LEAVE_AT=300` | `test: leaving after tick 300`, `session end reason=left`, exit 0 |
| flags | `--host` with the stub lobby client; `--vs-stage` with `--host`; a bad `--join` code; `--headless-ticks` online | exit 9 (`lobby failed error=unreachable ...`), 2, 2, 2 |
| slew | paced replay with `MPPM_NET_TEST_SLEW_NS=5000000` | `test slew shift_us=5000 after_retraces=12`; checksum log identical to the headless reference |
| probe | `--latency-probe` in a bots session (host on live keyboard, 15 ms each way) | 29 samples, median 58.8 ms at d = 1 |
| Challenge offline | `--replay C2.mpnl` (v5, course 0, 5400 ticks): headless twice, `MPPM_RNG_PERTURB`, paced | all identical, `other-in-session=0` |
| golden | `MPPM_GOLDEN=strict tools/net/desync_ci.sh --quick -d 63`; `tools/net/m53a_test.sh -q -d 64` | PASS (50 PASS lines, golden L10/Ls/Le identical); PASS (lines, selftest, golden, v4, activity, deaths, view, neg, shots, perf) |

The two-process tests (lobby server, real transport, relay, ICE, TURN, drop by `kill -9`, leave,
desync with a spectator, the UI) are the done test of [M5_3B_NETCODE.md](M5_3B_NETCODE.md) after the
transport, server and launcher packages are merged.

## Deviations from the design, and open items

| Item | What and why |
|---|---|
| D12 path | max downlink over **all** seated players, this one included (see "Input delay"). |
| D10 start barrier | The inject itself is barrier 1 at tick 0: Battle and Challenge have no nonsim region after it. Barriers carry a number (`loadCount` in INPUT, `count` in RELEASE, `releaseCount` in BUNDLE) so two loads at one tick cannot deadlock. INPUT is 50 + 8n bytes (design: 52 + 8n); BUNDLE keeps its 64-byte header (2 bytes for the count, 4 reserved). |
| CHUNK (type 13) | The transport caps a datagram at 1200 bytes; START (design: up to 2.3 KB) and a full LOBBY go in parts. |
| Offset filter | Mean of the samples within 1.5 ms of the minimum RTT over 2 s, smoothed: the plain minimum-RTT sample missed the 0.5 ms requirement with 8 ms jitter (1.5 ms). |
| Blame | Grace periods (16 s after START, 4 s after any change), none right after a release or for the host's own long ticks: the stage load's long ticks raised d for good otherwise (no barrier lowers it again in a Battle). |
| Lobby | A seat request swaps an automatically seated member; auto start waits for warm clocks (1.5 s connected, downlink reported). |
| Stalls | Counted only when the read waited past due for readiness; the first tick after a release counts as barrier time. |
| Exit codes | REJECT at Hello and the 15 s connect timeout exit 9 (lobby failure); a kick exits 8. |
| Bots | They follow the host game's reads instead of running their own retrace grid (they cannot know where a barrier is before the host's game reports it). |
| D11 phase (integration) | The phase error is not wrapped into [-P/2, P/2) and the correction may reach 16 ticks: whole-tick differences after the stage load's long ticks are not absorbed by stalls (a stall leaves the grid where it is), they made the host stall on every tick. The slewed grid is also in `p2_vi_retrace_time_ns` (see "Time sync and phase"). Whole ticks of a correction move the tick grid at once (`p2_host_tick_shift`), and online the grid is fixed (a long tick of under 4 ticks is caught up instead of moving every later tick): with the slew alone, the other players stalled on a lagging client's inputs for seconds after the stage load. The phase statistics of the summary (`phase_p50_us`, `phase_p99_us`) leave out samples taken while more than 2 ms of correction was pending (the catch-up after a long tick). |
| Fix round 2 | A stuck game whose net thread still runs is dropped by progress, not by packets (see "Stuck games"); the host's Drop also works at a load barrier (after 10 s); the crash screen says BYE at once; a BYE "done" before the host's last tick no longer leaves the host waiting; bad or out-of-order CHUNK parts reach the core as invalid traffic, and reassembly state goes with its peer; a Hello name is cleaned like the server's names and may not copy another member's; `code()` is read under the session mutex. |
| Fix round 1 | Section 10: connections are bound to a member only by a HELLO with its secret (see "Session flow"; self-test case `hijack`); ENet refuses packets over 1200 bytes and keeps at most 64 KiB waiting per peer; a member's "addr public" is punched once. A start-menu session sends the folder's content hash and re-checks it after the boot (see "Launcher API"). Host ICE agents that never carried a connection are released when their member leaves or ICE fails. Offline runs ignore bad values of the online flags instead of exiting 2 (they are checked only with `--host`, `--join` or `--net-test-bots`). `--view-captain` is ignored in a 2-seat online session (D16). The capped delay shows a toast. |
| Fix-min | A 2v2 -> 2-seat mode switch keeps every seat to one member (`modeswitch`); after START the lobby's `host_left` no longer ends a client whose link to the host is alive (`hostleft`); every encoder cuts strings at a UTF-8 code point boundary, names are 24 code points (96 bytes) like the server's, the host bounds its own name the same way and a HELLO name cut mid-character is kept without the cut character (`utf8`); unverified connections are capped (`pendingcap`, see "Session flow"); the host's Remove before and after START (`kick`) and a spectator-only desync (`specdesync`) are self-test cases. |
| Open | The two-process paths (lobby client, transport, connector modes, the UI) run in the done test since the integration (`M5_3B_NETCODE.md`, Results). The host's Remove (kick) is not in the done test; it is covered by the self-test case `kick` only. |
