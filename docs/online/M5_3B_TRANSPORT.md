# M5.3b transport: ENet over direct UDP, ICE and the builtin relay

Part of milestone M5.3b of [ONLINE_MULTIPLAYER.md](../ONLINE_MULTIPLAYER.md) (section 8, the second
"Step 3" row; plan 2.2 and 2.11). The transport carries the lockstep session between the members of
an online game: one reliable ordered channel for control messages and one unreliable unsequenced
channel for inputs and bundles. It is a passive object: the session's net thread
(`port/net/session/`, see `M5_3B_LOCKSTEP.md`) creates it, calls `service()` about every millisecond
and reacts to the events. Nothing here runs unless an online session is started.

| File | What |
|---|---|
| `port/net/transport.hpp` | the interface (`mppm::net::Transport`, `TransportConfig`, `Event`, `PeerStats`, `Impair`, `parse_impair`); every member's semantics are in its comments |
| `port/net/transport/transport.cpp` | the implementation over ENet and libjuice; the relay client and punches |
| `port/net/transport/enet_juice.{c,h}` | the ENet socket shim: routing, impairment stage, agent queue, MPRL filter |
| `port/net/transport/mprl.h` | the builtin relay's datagram codec (MPRL v1) |
| `port/net/transport/impair_parse.cpp` | `parse_impair` and `Transport::now_ns` |
| `cmake/mppm_net_transport.cmake` | fetches and builds ENet and libjuice, links them into `mppm`, the test targets |
| `tests/net/transport/` | `transport_test.sh`, `transport_test.cpp` (`p2transport_test`), `turn_server.c` (`p2net_turn`), `relay_stub.py`, `relay_session.py` |

## Dependencies and licences

Both are fetched by CMake `FetchContent` from GitHub release tarballs pinned by SHA-256, and used
unmodified as libraries:

| Library | Version | Licence | Tarball SHA-256 | Built as |
|---|---|---|---|---|
| [ENet](https://github.com/lsalzman/enet) | 1.3.18 | MIT | `28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd` | our static target `mppm_enet` (not ENet's CMakeLists): `callbacks.c compress.c host.c list.c packet.c peer.c protocol.c unix.c` with `HAS_FCNTL HAS_POLL HAS_GETADDRINFO HAS_GETNAMEINFO HAS_INET_PTON HAS_INET_NTOP HAS_MSGHDR_FLAGS HAS_SOCKLEN_T`; `unix.c` alone gets 8 compile definitions `enet_socket_{create,bind,get_address,set_option,send,receive,wait,destroy}=enet_sys_socket_*` |
| [libjuice](https://github.com/paullouisageneau/libjuice) | 1.7.4 | MPL-2.0 | `95c088862aa1b88b73d62aa99577135010eb768fcafb252d13aa85a8515ef798` | its own CMake (`NO_TESTS ON`, `NO_SERVER OFF` for the test TURN server, `EXCLUDE_FROM_ALL`), static target `juice-static` (`LibJuice::LibJuiceStatic`), consumers define `JUICE_STATIC`; the fragment leaves no global `BUILD_SHARED_LIBS` behind |

MPL-2.0 is file-level copyleft: the libjuice files are not changed, so nothing of ours falls under it.
The compile definitions rename symbols in ENet's platform file without editing it. Windows (`win32.c`
with the same renames, Winsock in the shim) and macOS are not done yet.

## The API in short

- `Transport::create(cfg, err)`: binds the ENet UDP socket on `bindAddress:port` (both host and client
  bind, so a client's socket has a stable address for the relay and for punches), creates the ENet host
  (`maxPeers` slots, 2 channels), the wake eventfd and the shim entry. At most 8 transports per process.
- Peer ids: `connect()`, `ice_create()` and incoming connections get ids 1, 2, ...; an id is never reused
  by one transport. An incoming ICE connection keeps the id of its agent (`ice_create`), so the session
  sees one peer from the first candidate to the end.
- Only a transport with `listen` accepts incoming ENet connections; a client resets them at once.
- `Connected.connectData` is the connecting side's value on both sides, except for an incoming ICE
  connection: there it is the value the agent was created with (the member that signalled it), and a
  remote that claims another value is reset (section 10).
- `Disconnected.text`: `local data=<n>` (our `disconnect`), `remote data=<n>` (the other side's),
  `timeout` (nothing heard for 2 s or more when ENet gave up), `connect failed` (the connect never
  completed), `ice failed`, `no free ENet peer`.
- `send()` refuses unknown or not yet connected peers, size 0 and sizes above `kMaxDatagram` (1200).
  Incoming, the ENet host has `maximumPacketSize = kMaxDatagram` (ENet's default is 32 MiB) and
  `maximumWaitingData = 64 KiB`, and a received packet above 1200 bytes is dropped (section 10's
  fixed maximum packet size). `duplicatePeers` stays at `maxPeers`: every relayed peer arrives from the
  relay's address and the local tests share 127.0.0.1, so a per-address cap would refuse them.
- `service(timeoutUs, out)` waits (ppoll, nanosecond timeout) on the UDP socket and the eventfd, for at
  most `timeoutUs` and never past the next held impaired datagram or relay/punch timer; it returns at
  once when work is already pending. Then: libjuice callbacks are turned into events, due impaired
  datagrams are sent, relay and punch timers run, `enet_host_service(host, &e, 0)` runs until it has
  no event, and MPRL answers become relay events. `wake()` (any thread) writes the eventfd.
- `Received.recvNs` is the steady time at which the shim took the newest datagram from that peer off
  the socket (or libjuice handed it over): within microseconds of the true arrival for the packet that
  woke `service()`.
- `stats()`: link and remote (`127.0.0.1:40123`, `ice 127.0.0.1:52011`, `relay 127.0.0.1:47201`),
  ENet's RTT, variance and loss estimate, payload bytes, and the shim's datagram counters per address
  (ENet's own datagrams included) and impairment drops.

ENet settings (design 7.3): every peer gets `enet_peer_timeout(peer, 32, timeoutMinMs, timeoutMaxMs)`
(5 s / 10 s by default) and `enet_peer_ping_interval(peer, 250)`; once connected,
`enet_peer_throttle_configure(peer, 5000, 0, 0)` and `packetThrottle = ENET_PEER_PACKET_THROTTLE_SCALE`,
so ENet never throttles our unreliable packets. Channel 0 packets are `ENET_PACKET_FLAG_RELIABLE`,
channel 1 packets `ENET_PACKET_FLAG_UNSEQUENCED`; `send(..., flush=true)` calls `enet_host_flush`.

## The shim

ENet does all its socket I/O through 8 functions. `unix.c` is compiled with them renamed to
`enet_sys_socket_*`, and `enet_juice.c` defines the public names. A table maps each attached ENet
socket (at most 8) to its state; a socket that is not attached behaves exactly like ENet's own.

- **Addresses.** An `ENetAddress` in 240.0.0.0/4 is synthetic: `240.0.0.<id>` port 1 is libjuice agent
  `<id>` (1 to 254) of that socket. Every other address is a real UDP address.
- **Send.** ENet's buffers are gathered into one datagram, which goes through the impairment stage and
  then to `enet_sys_socket_send` (real address) or `juice_send` (synthetic address; only once the agent
  is connected). The shim always reports the full length to ENet: a failed send is counted and treated
  as loss, because ENet aborts its whole send pass (all peers) on one failed send, and one unreachable
  candidate address must not stall the others.
- **Receive.** First the queue of datagrams the agents delivered (filled by libjuice's `cb_recv` on its
  own thread, under a mutex, at most 2048; the source becomes the agent's synthetic address), then the
  real socket. A real datagram that starts with `MPRL` goes to a small queue for the relay client and
  never reaches ENet; a real datagram claiming a 240.0.0.0/4 source is dropped.
- **Wait** is not used (the net thread calls `enet_host_service` with timeout 0); it delegates.

## Impairment (tests only)

Enabled only through `TransportConfig::impair` (the session fills it from `--net-impair SPEC` or
`MPPM_NET_IMPAIR`, design D7). It acts on every datagram this endpoint sends (both channels, ENet's own
acknowledgements and pings, relay registrations and punches), so impairing both ends impairs both
directions and the receiver needs no setting.

Per datagram, from a seeded xorshift32: drop with probability `loss`; otherwise one copy, plus a second
with probability `dup`; each copy is released at `now + delay + U(0, jitter)` (so jitter may reorder).
Held datagrams wait in a min-heap ordered by release time (ties in send order); `service()` sends the
due ones and wakes for the next. When enabled the transport prints once:

```
mppm: net: impairment delay_us=46000 jitter_us=8000 loss_ppm=0 dup_ppm=0 seed=1 (test only)
```

`parse_impair` grammar: comma-separated `key=value`, keys `delay`, `jitter`, `loss`, `dup`, `seed` in
any order, each at most once; times in `us`, `ms` or `s` (fractions allowed, at most 10 s); rates in `%`
or `ppm` (at most 100%); `seed` a decimal 32-bit number (0 means 1). `""` and `off` mean no impairment.
Example: `delay=46ms,jitter=8ms,loss=2%,dup=0.1%,seed=7`. Anything else is an error with a message.
"100 ms RTT with jitter" in the done test is `delay=46ms,jitter=8ms` on every peer.

## ICE

- One libjuice agent per remote member. The session host's agents share one UDP port in
  `JUICE_CONCURRENCY_MODE_MUX` (a free port picked at the first `ice_create` and kept); clients use
  `JUICE_CONCURRENCY_MODE_POLL`. Exception: libjuice does no TURN in mux mode ("TURN servers are not
  supported in mux mode", `agent.c`), so a host with TURN servers configured uses poll mode too.
- `bind_address` is `TransportConfig::bindAddress`; with `0.0.0.0` libjuice is given none and lists
  the interfaces itself. With `127.0.0.1` the bound address is the only host candidate, which is how
  the tests run ICE on loopback.
- STUN: the first `stun:` entry of `iceServers`. TURN: every `turn:` entry (UDP; `?transport=udp`
  accepted, TCP/TLS entries ignored) with its username and credential.
- Roles: libjuice makes the agent that produces its description first the controlling one. The client
  calls `ice_create(true, connectData)` and sends its description first; the host calls
  `ice_create(false, 0)`, sets the remote description, answers. Both gather and trickle candidates
  (`IceCandidate` lines are `a=candidate:...`, then `IceGatheringDone`).
- On `JUICE_STATE_CONNECTED` or `COMPLETED` the transport logs the selected pair and reports
  `IceState "connected"`; the controlling side then starts the ENet connection to the agent's synthetic
  address with the stored connect data, and `Connected` follows on both sides.

  ```
  mppm: net: ice connected peer=1 local=host remote=host
  ```

  `JUICE_STATE_FAILED` reports `IceState "failed"`; if an ENet peer exists it is reset and
  `Disconnected` (`ice failed`) follows. libjuice callbacks only queue and `wake()`.
- `iceRelayOnly` (tests): `a=candidate` lines whose type is not `relay` are removed from both
  descriptions, and such candidates are neither reported nor added. TURN permissions are per IP, so on
  loopback a host candidate can still reach a relay address on the same IP; the TURN test therefore
  runs the TURN server on 127.0.0.2, where both sides must select `local=relay remote=relay`.

## Builtin relay client and punches

The builtin relay of mppm-server (design 5.6, `server/PROTOCOL.md` "Relay") gives every client member
its own relay port R; datagrams from the client to R go to the host and back, so the host sees each
relayed client as a distinct address. The datagrams (network byte order, `mprl.h`):

| Type | Size | Direction | Body (from offset 8) |
|---|---|---|---|
| 1 REG | 24 | client socket to the relay control port | the member's 16-byte relay token |
| 2 OK | 18 | control port to the sender | role (0 host, 1 client), R (0 for the host, 2 bytes), family 4, the sender's IPv4 and port as the relay saw them |
| 3 ERR | 9 | control port to the sender | 1 unknown token, 2 session closed, 3 quota, 4 no free port |
| 4 PUNCH | 16 | peer to peer | 8-byte random nonce |

All start with `MPRL` and version 1, then the type and two zero bytes.

- `relay_register(relay, token)` sends REG from the ENet socket (the socket that carries the game
  traffic) at once, then every 250 ms until OK or ERR; without an answer after 5 s it reports
  `RelayFailed` (`no answer from the relay ... within 5 s`); ERR reports `RelayFailed`
  (`relay error 1 (unknown token)`). After OK it repeats REG every 15 s (keepalive and NAT rebinding).
  `RelayRegistered.text` is `host` or `<relay ip>:<R>`, `aux` this socket's public `ip:port`. A
  repeated OK raises the event again only if something changed. Answers are accepted only from the
  relay control address.
- The client then calls `connect("<relay ip>:<R>")`; its peer has `Link::Relay` and remote
  `relay <relay ip>:<R>`. The host sees an ordinary incoming connection from R (it marks it `Relay`
  only when the relay's address is not one of its own; on one machine it shows as `Direct`).
- `punch(addr, count)` sends `count` PUNCH datagrams 20 ms apart from the ENet socket (NAT hole
  punching before a direct attempt). A received PUNCH is dropped by the shim.

## Tests

Build the two test targets (not part of the default build), then run the script from the repo root on
the host. It needs no display and no disc.

```
distrobox enter mppm-build -- bash -lc 'cd <worktree> && ninja -C build-pc p2transport_test p2net_turn'
tests/net/transport/transport_test.sh                 # all cases, about 35 s
tests/net/transport/transport_test.sh -c direct,relay # some cases
tests/net/transport/transport_test.sh --relay-server http://127.0.0.1:47200 --server-log OUT/server.log -c relay
```

Options: `-b` build directory, `-o` output directory (default `/var/tmp/mppm-online/m53b-transport/tt`,
one log per run), `-p` first fixed port (default 47050; the script uses 47050-47079 on 127.0.0.1 and
127.0.0.2), `-B` builds the targets first, `-l` lists the cases. Exit 0 only if every case passes.

| Case | Runs | Pass when |
|---|---|---|
| `selftest` | `p2transport_test selftest` | `parse_impair` table (13 valid, 16 invalid specs); synthetic address mapping; MPRL encode/decode with exact bytes and the rejections; impairment statistics over 100 000 draws for 5 settings (loss within 0.2% absolute, every delay within [delay, delay + jitter], duplication rate, mean); the shim's delay heap end to end on two UDP sockets (400 datagrams, 5 ms + U(0, 5 ms): all released, none early, reordered by jitter) |
| `direct` | `serve` and `client` as separate processes on 127.0.0.1, three runs: clean, `delay=50ms` on both, `loss=5%` on both (different seeds) | Connected with the connect data on both sides; 2000 unreliable pings each way (one every 2 ms, echoed) plus 200 reliable messages; reliable all in order; delay run: echo RTT median 100 ± 3 ms; loss run: unreliable loss 5 ± 1.5% in each direction; clean run: no loss; graceful disconnect seen by both; the impairment log line |
| `fanout` | one `serve --clients 4`, four `client` processes at once | all four complete (1000 pings, 100 reliable each), the host saw the four connect data values |
| `ice` | `p2transport_test ice`: two transports in one process, loopback host candidates, host mux, client poll | connected, `Link::Ice`, 1000 pings and 50 reliable both ways, two log lines `ice connected ... local=host remote=host` |
| `turn` | `p2net_turn --bind 127.0.0.2 --port P --user u --pass p`, then `ice --turn ... --relay-only`; again with the server on 127.0.0.1 (the done test's form) | both runs connect and carry the traffic; on 127.0.0.2 both sides log `local=relay remote=relay`; on 127.0.0.1 a selected candidate is `relay` |
| `relay` | `relay_stub.py` (a standard-library reimplementation of 5.6) and `p2transport_test relay` | both register (host `host`, client its relay port; `aux` equals each socket's address), the client punches the host's direct address, connects to its relay port, 2000 pings + 100 reliable flow, client link `Relay`, the host sees the relay port; a third socket with an unknown token gets `RelayFailed` (`relay error 1`); the stub's `relay mapping` line shows more than 50 000 bytes up and down |
| `wake` | `service(100000)` while another thread calls `wake()` after 20 ms, 20 rounds | every return within 1 ms of the `wake()` (measured: about 0.03 ms); an idle `service(1000)` lasts about 1 ms |
| `teardown` | 50 create/connect/traffic/destroy cycles of a host and a client (every 5th over ICE, every other one closing gracefully, the rest destroyed while connected) | all cycles pass; the process's fd count is the same before and after |

**Against the real server.** `--relay-server URL` replaces the stub: `relay_session.py` (standard
library only) creates a session and joins it over REST, attaches both tokens over the WebSocket, prints
the relay address and tokens, keeps the sockets alive while the test runs and then leaves (the session
ends, freeing the mapping). With `--server-log FILE` the case also requires the server's
`relay mapping ... up_bytes=<n> down_bytes=<n>` line above 50 000 bytes each. The done test
(`tools/net/m53b_test.sh`, check `selftest`) runs it this way after integration.

Example results (this machine, all cases in about 35 s): direct delay run RTT median 100.1 ms (ENet's
own RTT 102 ms); loss run 4.1% up and 4.7% down, all 200 reliable in order; wake 0.024 ms median.

## Not done here

- Windows and macOS (Winsock in the shim, `win32.c` renames) and IPv6 peers: ENet 1.3 is IPv4 only;
  libjuice handles IPv6 candidates inside an agent.
- The host cannot tell relayed clients from direct ones on one machine (see above).
- No congestion control beyond ENet's reliable window: the session's traffic is a few kbit/s.
