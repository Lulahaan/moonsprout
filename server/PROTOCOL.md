# mppm-server protocol, version 1

This file is the authority for clients of `mppm-server` (the game's lobby client is
`port/net/lobby_client.cpp`; the relay client side is in `port/net/transport/`). It covers the
WebSocket protocol, the REST endpoints, the ICE server lists and the builtin UDP relay (MPRL v1).

The server knows only **membership**: who is in a session and whether they spectate. Seats, teams,
ready state, mode, stage and the content check are decided by the host and travel between the
peers, not through the server. The server relays signalling messages between the host and each
member, enforces kicks and bans, and hands out ICE servers or builtin relay tokens.

## Sessions, members, codes

- A session has a **code**, an opaque **id** (`s_` and 16 hex digits), a **host** (member 0) and
  members 1, 2, ... in join order. Member ids are never reused within a session.
- `maxMembers` counts the host: 2..8, default 8 (`--max-members` caps it; a larger value is lowered,
  a value below 2 is `bad_request`). Members that joined over REST and have not attached yet count.
- **Code**: 8 symbols of Crockford base32 (`0123456789ABCDEFGHJKMNPQRSTVWXYZ`), 40 bits from the
  operating system's random number generator, written `ABCD-EFGH`. On input: any case, spaces and
  hyphens are ignored, `I` and `L` read as `1`, `O` as `0`, `U` (and anything else outside the
  alphabet) is invalid, exactly 8 symbols remain. A code dies with its session.
- Each member gets a **secret** (64 bits, 16 hex digits). The host receives every member's secret as
  `auth` in `member_joined` and checks it when the member's game connects to it directly.
- With the builtin relay each member also gets a **relay token** (16 bytes, 32 hex digits).
- **Names** are trimmed, control characters are removed, at most 24 characters (Unicode scalar
  values) are kept, an empty name becomes `Player`. Name input is limited to 256 bytes, passwords to
  64 bytes.
- A session ends when the host's WebSocket closes or the host sends `leave` (members get
  `closed {reason:"host_left"}`), when it reaches its hard expiry (`--session-ttl`, default 6 h;
  everyone gets `closed {reason:"expired"}`), or when the server shuts down (`server_shutdown`).
- Join checks, in this order: protocol version (`version`), rate limits (`rate_limited`), the code
  exists (`not_found`), the address is not banned from this session (`forbidden`), the password
  (`bad_password`), room (`full`).

## WebSocket: `GET /v1/ws`

Text frames, each one JSON object of at most 16 KiB. The first client message must be `create`,
`join` or `attach` and must arrive within 10 s. Unknown fields are ignored. Integers are JSON
integers (no fractions).

### Client to server

```json
{"t":"create","v":1,"name":"Olimar","password":"","maxMembers":8}
{"t":"join","v":1,"code":"ABCD-EFGH","name":"Louie","password":"","spectator":false}
{"t":"attach","v":1,"token":"<32 hex from REST>"}
{"t":"signal","to":3,"kind":"desc","data":"..."}
{"t":"kick","member":3,"ban":false}
{"t":"leave"}
{"t":"ping"}
```

`v` (required in the first message) must be 1. `password`, `maxMembers`, `spectator` and `ban` may
be omitted (empty, 8, false, false).

### Server to client

```json
{"t":"created","code":"ABCD-EFGH","session":"s_0123456789abcdef","member":0,"secret":"<16 hex>",
 "ice":[{"urls":["stun:127.0.0.1:3478"]},{"urls":["turn:127.0.0.1:3479"],"username":"u","credential":"p"}],
 "relay":{"addr":"127.0.0.1:3479","token":"<32 hex>"},"expires":1764000000}
{"t":"joined","code":"ABCD-EFGH","session":"s_...","member":3,"secret":"<16 hex>","host":0,
 "members":[{"member":0,"name":"Olimar","spectator":false},{"member":3,"name":"Louie","spectator":false}],
 "ice":[...],"relay":{...},"expires":1764000000}
{"t":"member_joined","member":3,"name":"Louie","spectator":false,"auth":"<16 hex, host only>"}
{"t":"member_left","member":3,"reason":"left|kicked|banned|timeout"}
{"t":"signal","from":0,"kind":"addr","data":"..."}
{"t":"error","code":"<code>","msg":"<one English sentence>","fatal":true}
{"t":"closed","reason":"host_left|expired|server_shutdown|kicked|banned"}
{"t":"pong"}
```

- `relay` is `null` unless the server runs with `--relay-mode builtin`. `ice` is an array (possibly
  empty); each entry has `urls` (an array of strings) and, for TURN, `username` and `credential`.
  `expires` is the session's hard expiry in Unix seconds.
- `members` in `joined` lists the members whose WebSocket is live, plus the joiner and always the
  host (member 0, also while a REST-created host has not attached yet), by id; it never carries `auth`.
- `member_joined` goes to every live member except the one that joined (which gets `joined`
  instead); only the host's copy carries `auth`. A host that attaches over REST after members
  already attached gets one `member_joined` (with `auth`) for each of them right after `created`.
- `member_left` goes to every remaining live member, the host included (also after the host's own
  kick). Reasons: `left` (sent `leave`, closed the socket or broke a protocol rule), `kicked`,
  `banned`, `timeout` (60 s without any frame).
- `signal`: `kind` is `desc`, `cand`, `done` or `addr`; `data` is a string of at most 4096 bytes.
  A member may signal only the host (`to` = 0); the host may signal any member. The server forwards
  the message unchanged, with `from` set to the sender.
- `kick` (host only): the member gets `closed {reason:"kicked"}` (or `"banned"`) and its socket
  closes. `ban` also refuses the member's address for the rest of the session.
- `leave`: the host's `leave` ends the session (`host_left`); a member's `leave` removes it. The
  server then closes the socket (code 1000) without a `closed` message.

### Errors

| Code | When | Fatal |
|---|---|---|
| `version` | `v` missing or not 1 | yes (close 4001) |
| `bad_request` | malformed message, wrong first message, bad token, `maxMembers` < 2, kick of the host, a second create/join/attach | first message or malformed: yes; otherwise no |
| `not_found` | unknown code or token; signal or kick to a member that is not there | create/join/attach: yes; otherwise no |
| `forbidden` | join from a banned address; a member signalling another member; a member kicking | join: yes; otherwise no |
| `bad_password`, `full` | join refused | yes |
| `rate_limited` | a per-address limit, the server's session limit, shutdown; too many messages on a socket | yes, except the per-socket message rate (see below) |
| `too_large` | a frame over 16 KiB (fatal, close 4002); signal data over 4096 bytes (not fatal) | see left |

A fatal error is followed by a close frame; a non-fatal one keeps the socket and the session.

### Close codes

| Code | Meaning |
|---|---|
| 1000 | normal: after `closed`, after `leave` |
| 4000 | the first message was not `create`/`join`/`attach`, was malformed, or did not arrive within 10 s |
| 4001 | unsupported protocol version |
| 4002 | a binary frame, a text frame over 16 KiB, or a malformed message after the first |
| 4003 | a refused create/join/attach, or another fatal error |
| 4004 | no frame received for 60 s |

### Keepalive and message rate

- The server closes a socket that sent nothing (no message, no ping frame) for 60 s; the session
  sees `member_left {reason:"timeout"}` (or `host_left` for the host). Clients send `{"t":"ping"}`
  every 20 s (WebSocket ping frames count as well).
- Each socket may send 40 messages per second with a burst of 80. Excess messages are dropped and
  the sender gets a non-fatal `rate_limited` error at most once per second; after 5 s of continuous
  excess it becomes fatal (close 4003).

## REST

| Request | Body | Answer |
|---|---|---|
| `POST /v1/sessions` | `{"name","password","maxMembers"}` | 201 `{"code","session","member":0,"token","ice","relay","expires"}` |
| `POST /v1/sessions/{code}/join` | `{"name","password","spectator"}` | 200 `{"code","session","member","token","host":0,"members","ice","relay","expires"}` |
| `GET /v1/health` | - | 200 `{"ok":true,"version":"0.1.0","sessions":N,"members":M}` |

Bodies are JSON objects of at most 4 KiB; every field may be omitted. `{code}` is normalized like
any typed code. Errors: 400 `bad_request`, 403 `forbidden`/`bad_password`, 404 `not_found`, 409
`full`, 429 `rate_limited`, each with the body `{"error":"<code>","msg":"..."}`.

`token` (32 hex) is redeemed once by a WebSocket `{"t":"attach","v":1,"token":...}` within 60 s;
the answer is `created` or `joined` exactly as for the WebSocket forms (with the `secret`, which the
REST answer does not carry). A REST-created session whose host does not attach in time ends (members
that did attach get `closed {reason:"expired"}`); an unattached REST join is dropped silently.
`member_joined` for a REST join is sent when it attaches. `health.members` counts live WebSockets.

## ICE servers and relay modes

| `--relay-mode` | `ice` | `relay` |
|---|---|---|
| `none` (default) | one entry per `--stun` URL | `null` |
| `static` | `--stun` entries, then `{"urls":[--turn],"username":--turn-user,"credential":--turn-pass}` | `null` |
| `shared-secret` | `--stun` entries, then `{"urls":[--turn],"username":"<expiry>:<member id>","credential":base64(HMAC-SHA1(--turn-secret, username))}` with expiry = now + `--turn-ttl` (Unix seconds); this is coturn's `use-auth-secret` scheme | `null` |
| `builtin` (`--relay`) | `--stun` entries | `{"addr":"<--relay-public or --relay-bind>:<control port>","token":"<32 hex>"}` |
| `cloudflare` | the server refuses to start (deferred) | - |

Clients never receive the TURN secret. Check value: secret `north`, username `1764000000:3` give the
credential `tz73U4Kkw6KkWeFfmzeQRzoBQe0=`.

## Builtin relay: MPRL v1 (UDP)

A port-mapped UDP relay. Every client member gets its own relay port R, so the host sees each relayed
client as a distinct address. Byte order is network order (big-endian). Every MPRL datagram starts
with the 4 bytes `MPRL` and the version byte 1, and has an exact length.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `4D 50 52 4C` ("MPRL") |
| 4 | 1 | version = 1 |
| 5 | 1 | type: 1 REG, 2 OK, 3 ERR, 4 PUNCH |
| 6 | 2 | 0 |
| **REG** (24 bytes), member socket to the relay control port | | |
| 8 | 16 | the member's relay token |
| **OK** (18 bytes), relay control port to the sender | | |
| 8 | 1 | role: 0 host, 1 client |
| 9 | 2 | the client's relay port R (0 for the host) |
| 11 | 1 | 4 (IPv4) |
| 12 | 4 | the sender's address as the relay saw it |
| 16 | 2 | the sender's port as the relay saw it |
| **ERR** (9 bytes), relay control port to the sender | | |
| 8 | 1 | 1 unknown token, 2 session closed, 3 quota, 4 no free port |
| **PUNCH** (16 bytes), peer to peer, never sent to the relay | | |
| 8 | 8 | random nonce |

- REG is sent from the socket that carries the game traffic. The host's REG records the host address
  H. A client's REG records its address C and binds a port R from `--relay-ports` for that member;
  a repeated REG answers the same R, and a REG from a new address replaces C (NAT rebinding).
  Clients repeat REG every 15 s. OK and ERR come from the control port.
- ERR 2 answers tokens of members or sessions that ended in the last 10 minutes; older or never
  issued tokens get ERR 1. A malformed datagram on the control port gets no answer.
- Forwarding on R: a datagram from C is sent to H, a datagram from H is sent to C, both from R.
  Anything else is dropped, and so is every datagram on R that starts with `MPRL`. Datagrams from C
  are dropped while the host has not registered.
- Caps: `--relay-kbps` (default 2048 kbit/s) per member port and direction, as a token bucket with a
  one-second burst (excess datagrams are dropped); at most 8 member ports per session; at most 16 REG
  per minute per source address (scaled by `--limits-scale`; excess REGs get no answer); a port is
  freed 60 s after its last forwarded datagram or REG, when its member leaves or is kicked, or when
  the session ends.
- Every freed mapping logs one line:
  `mppm-server: relay mapping session=<id> member=<m> port=<R> up_bytes=<n> down_bytes=<n> closed=<idle|session|shutdown>`
  (`up` = client to host, `down` = host to client, forwarded bytes only).
- Relay addresses are IPv4 only.

## Rate limits

Per source address (the TCP peer; with `--trust-proxy` the first address in `Fly-Client-IP`, else
in `X-Forwarded-For`), as token buckets. Every number is multiplied by `--limits-scale`.

| What | Rate | Burst |
|---|---|---|
| session creates | 6 per minute | 3 |
| joins (REST and WebSocket) | 30 per minute | 10 |
| failed joins (`not_found`, `bad_password`) | 10 per minute; the next failure locks joins from the address out for 5 minutes | 5 |
| WebSocket upgrades | 20 per minute | 10 |
| live WebSockets | 16 at a time | - |
| live sessions hosted | 4 at a time | - |
| relay REG | 16 per minute | 16 |

Global: `--max-sessions` (default 2000) live sessions, `--max-members` (default 8) per session. A
limit answers 429 `rate_limited` (REST, upgrade) or a fatal `error {code:"rate_limited"}`
(WebSocket).
