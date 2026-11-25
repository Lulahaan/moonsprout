//! Sessions, members and every state change (create, join, attach, signal, kick, leave, expiry).
//!
//! One mutex guards all sessions (`App::inner`); it is never held across an await. Lock order:
//! `inner`, then `limits`, then the relay's own state (`relay::Relay`); never the other way round.

use std::collections::{BTreeMap, HashMap, HashSet};
use std::net::{IpAddr, SocketAddr};
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde_json::{json, Value};
use tokio::sync::{mpsc, watch, Notify};

use crate::codes::{self, display_code, generate_code, normalize_code, random_hex, sanitize_name};
use crate::config::{Args, RelayMode};
use crate::ice::IceConfig;
use crate::ratelimit::{IpLimiter, Limits, What};
use crate::relay::{CloseReason, Relay};

/// A REST-created session or REST join must be attached by a WebSocket within this time.
pub const ATTACH_WINDOW: Duration = Duration::from_secs(60);
/// Tokens of ended sessions answer MPRL ERR "session closed" for this long.
const DEAD_TOKEN_MEMORY: Duration = Duration::from_secs(600);
/// Outbound messages queued per socket before it counts as stuck and is closed.
pub const OUTBOUND_QUEUE: usize = 512;

pub fn unix_now() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

/// Why a request was refused: an error code of PROTOCOL.md and one English sentence.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[error("{code}: {msg}")]
pub struct Refusal {
    pub code: &'static str,
    pub msg: String,
}

impl Refusal {
    pub fn new(code: &'static str, msg: impl Into<String>) -> Refusal {
        Refusal { code, msg: msg.into() }
    }

    pub fn http_status(&self) -> u16 {
        match self.code {
            "forbidden" | "bad_password" => 403,
            "not_found" => 404,
            "full" => 409,
            "too_large" => 413,
            "rate_limited" => 429,
            _ => 400,
        }
    }
}

/// Something queued for one WebSocket.
#[derive(Debug)]
pub enum Out {
    Text(String),
    /// Send a close frame with this code and end the socket.
    Close(u16),
}

/// The sending half of a member's live WebSocket.
#[derive(Debug, Clone)]
pub struct Conn {
    pub id: u64,
    pub tx: mpsc::Sender<Out>,
    pub kill: Arc<Notify>,
}

impl Conn {
    pub fn send(&self, v: &Value) {
        self.push(Out::Text(v.to_string()));
    }

    pub fn push(&self, o: Out) {
        if self.tx.try_send(o).is_err() {
            // Queue full (the client stopped reading) or the task is gone: close it.
            self.kill.notify_one();
        }
    }
}

#[derive(Debug)]
pub struct Member {
    pub id: u32,
    pub name: String,
    pub spectator: bool,
    pub secret: String,
    pub relay_token: [u8; 16],
    pub ip: IpAddr,
    /// None while a REST create/join waits for its WebSocket attach.
    pub conn: Option<Conn>,
    pub attach: Option<(String, Instant)>,
}

#[derive(Debug)]
pub struct Session {
    pub id: String,
    pub code: String,
    pub password: String,
    pub max_members: u8,
    pub expires_at: Instant,
    pub expires_unix: u64,
    pub host_ip: IpAddr,
    pub members: BTreeMap<u32, Member>,
    pub next_member: u32,
    pub banned: HashSet<IpAddr>,
}

impl Session {
    fn member_list(&self, include: u32) -> Value {
        Value::Array(
            self.members
                .values()
                // The host (member 0) is always listed: members signal only the host, and one that joins
                // a REST-created session before the host attached its WebSocket still needs its id.
                .filter(|m| m.conn.is_some() || m.id == include || m.id == 0)
                .map(|m| json!({ "member": m.id, "name": m.name, "spectator": m.spectator }))
                .collect(),
        )
    }

    fn broadcast(&self, except: u32, v: &Value) {
        for m in self.members.values() {
            if m.id != except {
                if let Some(c) = &m.conn {
                    c.send(v);
                }
            }
        }
    }
}

#[derive(Debug, Default)]
pub struct Inner {
    pub sessions: HashMap<String, Session>,
    pub by_code: HashMap<String, String>,
    pub attach_tokens: HashMap<String, (String, u32)>,
    pub relay_tokens: HashMap<[u8; 16], (String, u32)>,
    pub dead_relay_tokens: HashMap<[u8; 16], Instant>,
}

/// How a member reaches the server: a live WebSocket, or a REST call that gets an attach token.
pub enum Via {
    Ws(Conn),
    Rest,
}

/// A successful create or join.
pub struct Admitted {
    pub session: String,
    pub member: u32,
    /// The `created`/`joined` message (WebSocket) or the REST answer body.
    pub reply: Value,
}

/// What a REG on the relay control port refers to.
pub enum RelayLookup {
    Member { session: String, member: u32 },
    Closed,
    Unknown,
}

pub struct App {
    pub args: Args,
    pub ice: IceConfig,
    pub limits: Mutex<IpLimiter>,
    pub inner: Mutex<Inner>,
    pub relay: Option<Arc<Relay>>,
    /// "ip:port" announced to clients for the builtin relay.
    pub relay_announce: Option<String>,
    pub shutdown: watch::Sender<bool>,
    pub live_ws: AtomicUsize,
    next_conn: AtomicU64,
}

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

impl App {
    pub fn new(args: Args, relay: Option<Arc<Relay>>, relay_announce: Option<String>) -> Arc<App> {
        let (shutdown, _) = watch::channel(false);
        Arc::new(App {
            ice: IceConfig::from_args(&args),
            limits: Mutex::new(IpLimiter::new(Limits::scaled(args.limits_scale))),
            inner: Mutex::new(Inner::default()),
            relay,
            relay_announce,
            shutdown,
            live_ws: AtomicUsize::new(0),
            next_conn: AtomicU64::new(1),
            args,
        })
    }

    pub fn inner(&self) -> MutexGuard<'_, Inner> {
        lock(&self.inner)
    }

    pub fn limits(&self) -> MutexGuard<'_, IpLimiter> {
        lock(&self.limits)
    }

    pub fn new_conn(&self, tx: mpsc::Sender<Out>, kill: Arc<Notify>) -> Conn {
        Conn { id: self.next_conn.fetch_add(1, Ordering::Relaxed), tx, kill }
    }

    pub fn is_shutting_down(&self) -> bool {
        *self.shutdown.borrow()
    }

    /// The client address for rate limits (with --trust-proxy: Fly-Client-IP or X-Forwarded-For).
    pub fn client_ip(&self, headers: &axum::http::HeaderMap, peer: SocketAddr) -> IpAddr {
        if self.args.trust_proxy {
            let from = |name: &str| {
                headers
                    .get(name)
                    .and_then(|v| v.to_str().ok())
                    .and_then(|s| s.split(',').next())
                    .and_then(|s| s.trim().parse::<IpAddr>().ok())
            };
            if let Some(ip) = from("fly-client-ip").or_else(|| from("x-forwarded-for")) {
                return ip;
            }
        }
        peer.ip()
    }

    fn relay_json(&self, m: &Member) -> Value {
        match (&self.relay_announce, self.args.mode()) {
            (Some(addr), RelayMode::Builtin) => json!({ "addr": addr, "token": codes::hex(&m.relay_token) }),
            _ => Value::Null,
        }
    }

    fn new_member(inner: &mut Inner, sid: &str, id: u32, name: &str, spectator: bool, ip: IpAddr, via: Via) -> Member {
        let mut relay_token = [0u8; 16];
        codes::os_random(&mut relay_token);
        while inner.relay_tokens.contains_key(&relay_token) {
            codes::os_random(&mut relay_token);
        }
        inner.relay_tokens.insert(relay_token, (sid.to_string(), id));
        let (conn, attach) = match via {
            Via::Ws(c) => (Some(c), None),
            Via::Rest => {
                let mut t = random_hex(16);
                while inner.attach_tokens.contains_key(&t) {
                    t = random_hex(16);
                }
                inner.attach_tokens.insert(t.clone(), (sid.to_string(), id));
                (None, Some((t, Instant::now() + ATTACH_WINDOW)))
            }
        };
        Member { id, name: sanitize_name(name), spectator, secret: random_hex(8), relay_token, ip, conn, attach }
    }

    fn created_json(&self, s: &Session, m: &Member, rest: bool) -> Value {
        let mut v = json!({
            "t": "created",
            "code": display_code(&s.code),
            "session": s.id,
            "member": m.id,
            "secret": m.secret,
            "ice": self.ice.list(m.id, unix_now()),
            "relay": self.relay_json(m),
            "expires": s.expires_unix,
        });
        if rest {
            let o = v.as_object_mut().expect("object");
            o.remove("t");
            o.remove("secret");
            o.insert("token".into(), json!(m.attach.as_ref().map(|a| a.0.clone())));
        }
        v
    }

    fn joined_json(&self, s: &Session, m: &Member, rest: bool) -> Value {
        let mut v = json!({
            "t": "joined",
            "code": display_code(&s.code),
            "session": s.id,
            "member": m.id,
            "secret": m.secret,
            "host": 0,
            "members": s.member_list(m.id),
            "ice": self.ice.list(m.id, unix_now()),
            "relay": self.relay_json(m),
            "expires": s.expires_unix,
        });
        if rest {
            let o = v.as_object_mut().expect("object");
            o.remove("t");
            o.remove("secret");
            o.insert("token".into(), json!(m.attach.as_ref().map(|a| a.0.clone())));
        }
        v
    }

    fn member_joined_json(m: &Member, with_auth: bool) -> Value {
        let mut v = json!({ "t": "member_joined", "member": m.id, "name": m.name, "spectator": m.spectator });
        if with_auth {
            v["auth"] = json!(m.secret);
        }
        v
    }

    /// Announces a member that just went live: auth to the host only.
    fn announce(s: &Session, m: &Member) {
        let plain = Self::member_joined_json(m, false);
        let authed = Self::member_joined_json(m, true);
        for other in s.members.values() {
            if other.id == m.id {
                continue;
            }
            if let Some(c) = &other.conn {
                c.send(if other.id == 0 { &authed } else { &plain });
            }
        }
    }

    pub fn create(
        &self,
        ip: IpAddr,
        name: &str,
        password: &str,
        max_members: Option<u64>,
        via: Via,
    ) -> Result<Admitted, Refusal> {
        if self.is_shutting_down() {
            return Err(Refusal::new("rate_limited", "The server is shutting down."));
        }
        let max = match max_members {
            None => self.args.max_members,
            Some(n) if n < 2 => return Err(Refusal::new("bad_request", "maxMembers must be at least 2.")),
            Some(n) => n.min(u64::from(self.args.max_members)) as u8,
        };
        let now = Instant::now();
        let mut inner = self.inner();
        if inner.sessions.len() >= self.args.max_sessions {
            return Err(Refusal::new("rate_limited", "The server has no room for another session."));
        }
        {
            let mut l = self.limits();
            if !l.allow(ip, What::Create, now) {
                return Err(Refusal::new(
                    "rate_limited",
                    "Too many sessions created from this address; wait a minute.",
                ));
            }
            if !l.host_open(ip, now) {
                return Err(Refusal::new("rate_limited", "This address already hosts the maximum number of sessions."));
            }
        }
        let mut code = generate_code();
        while inner.by_code.contains_key(&code) {
            code = generate_code();
        }
        let mut id = format!("s_{}", random_hex(8));
        while inner.sessions.contains_key(&id) {
            id = format!("s_{}", random_hex(8));
        }
        let rest = matches!(via, Via::Rest);
        let host = Self::new_member(&mut inner, &id, 0, name, false, ip, via);
        let ttl = self.args.session_ttl;
        let mut s = Session {
            id: id.clone(),
            code: code.clone(),
            password: password.to_string(),
            max_members: max,
            expires_at: now + Duration::from_secs(ttl),
            expires_unix: unix_now() + ttl,
            host_ip: ip,
            members: BTreeMap::new(),
            next_member: 1,
            banned: HashSet::new(),
        };
        let reply = self.created_json(&s, &host, rest);
        s.members.insert(0, host);
        inner.by_code.insert(code, id.clone());
        inner.sessions.insert(id.clone(), s);
        tracing::info!(session = %id, rest, "session created");
        Ok(Admitted { session: id, member: 0, reply })
    }

    pub fn join(
        &self,
        ip: IpAddr,
        code: &str,
        name: &str,
        password: &str,
        spectator: bool,
        via: Via,
    ) -> Result<Admitted, Refusal> {
        if self.is_shutting_down() {
            return Err(Refusal::new("rate_limited", "The server is shutting down."));
        }
        let now = Instant::now();
        if !self.limits().allow(ip, What::Join, now) {
            return Err(Refusal::new("rate_limited", "Too many join attempts from this address; wait a few minutes."));
        }
        let failed = |r: Refusal| -> Refusal {
            if self.limits().join_failed(ip, now) {
                r
            } else {
                Refusal::new("rate_limited", "Too many failed join attempts from this address; wait five minutes.")
            }
        };
        let Some(code) = normalize_code(code) else {
            return Err(failed(Refusal::new("not_found", "No session has this code.")));
        };
        let mut inner = self.inner();
        let Some(sid) = inner.by_code.get(&code).cloned() else {
            drop(inner);
            return Err(failed(Refusal::new("not_found", "No session has this code.")));
        };
        enum Check {
            Admit(u32),
            Failed(Refusal),
            Refused(Refusal),
        }
        let check = {
            let s = inner.sessions.get(&sid).expect("by_code and sessions agree");
            if s.banned.contains(&ip) {
                Check::Refused(Refusal::new("forbidden", "The host has banned you from this session."))
            } else if s.password != password {
                Check::Failed(Refusal::new("bad_password", "The password is wrong."))
            } else if s.members.len() >= usize::from(s.max_members) {
                Check::Refused(Refusal::new("full", "The session is full."))
            } else {
                Check::Admit(s.next_member)
            }
        };
        let next = match check {
            Check::Admit(n) => n,
            Check::Refused(r) => return Err(r),
            Check::Failed(r) => {
                drop(inner);
                return Err(failed(r));
            }
        };
        let rest = matches!(via, Via::Rest);
        let m = Self::new_member(&mut inner, &sid, next, name, spectator, ip, via);
        let s = inner.sessions.get_mut(&sid).expect("checked above");
        s.next_member += 1;
        s.members.insert(next, m);
        let m = &s.members[&next];
        let reply = self.joined_json(s, m, rest);
        if m.conn.is_some() {
            Self::announce(s, m);
        }
        tracing::info!(session = %sid, member = next, rest, "member joined");
        Ok(Admitted { session: sid, member: next, reply })
    }

    /// Redeems a REST token for a WebSocket.
    pub fn attach(&self, token: &str, conn: Conn) -> Result<Admitted, Refusal> {
        if codes::parse_hex(token, 16).is_none() {
            return Err(Refusal::new("bad_request", "The token must be 32 hex digits."));
        }
        let token = token.to_ascii_lowercase();
        let mut inner = self.inner();
        let Some((sid, mid)) = inner.attach_tokens.remove(&token) else {
            return Err(Refusal::new("not_found", "The token is unknown, used or expired."));
        };
        let Some(s) = inner.sessions.get_mut(&sid) else {
            return Err(Refusal::new("not_found", "The session has ended."));
        };
        let Some(m) = s.members.get_mut(&mid) else {
            return Err(Refusal::new("not_found", "The token is unknown, used or expired."));
        };
        m.attach = None;
        m.conn = Some(conn);
        let s = &*s;
        let m = &s.members[&mid];
        let reply = if mid == 0 {
            // Tell the host about members that attached before it did (with their auth), after `created`.
            let created = self.created_json(s, m, false);
            for other in s.members.values().filter(|o| o.id != 0 && o.conn.is_some()) {
                m.conn.as_ref().expect("just set").send(&Self::member_joined_json(other, true));
            }
            created
        } else {
            let joined = self.joined_json(s, m, false);
            Self::announce(s, m);
            joined
        };
        Ok(Admitted { session: sid, member: mid, reply })
    }

    /// Forwards a signal. Err = a non-fatal error for the sender.
    pub fn signal(&self, sid: &str, from: u32, to: u64, kind: &str, data: &str) -> Result<(), Refusal> {
        if data.len() > crate::proto::MAX_SIGNAL_DATA {
            return Err(Refusal::new("too_large", "Signal data is limited to 4096 bytes."));
        }
        if from != 0 && to != 0 {
            return Err(Refusal::new("forbidden", "Members may only signal the host."));
        }
        if u64::from(from) == to {
            return Err(Refusal::new("bad_request", "Cannot signal yourself."));
        }
        let inner = self.inner();
        let Some(s) = inner.sessions.get(sid) else {
            return Err(Refusal::new("not_found", "The session has ended."));
        };
        let target = u32::try_from(to).ok().and_then(|t| s.members.get(&t)).and_then(|m| m.conn.as_ref());
        let Some(c) = target else {
            return Err(Refusal::new("not_found", format!("Member {to} is not in the session.")));
        };
        c.send(&json!({ "t": "signal", "from": from, "kind": kind, "data": data }));
        Ok(())
    }

    /// Host only: removes a member (and bans its IP from the session).
    pub fn kick(&self, sid: &str, by: u32, member: u64, ban: bool) -> Result<(), Refusal> {
        if by != 0 {
            return Err(Refusal::new("forbidden", "Only the host can kick."));
        }
        let Ok(member) = u32::try_from(member) else {
            return Err(Refusal::new("not_found", "No such member."));
        };
        if member == 0 {
            return Err(Refusal::new("bad_request", "The host cannot kick itself."));
        }
        let removed = {
            let mut inner = self.inner();
            let Some(s) = inner.sessions.get_mut(sid) else {
                return Err(Refusal::new("not_found", "The session has ended."));
            };
            let Some(m) = s.members.remove(&member) else {
                return Err(Refusal::new("not_found", "No such member."));
            };
            if ban {
                s.banned.insert(m.ip);
            }
            let reason = if ban { "banned" } else { "kicked" };
            if let Some(c) = &m.conn {
                c.send(&json!({ "t": "closed", "reason": reason }));
                c.push(Out::Close(crate::proto::close::NORMAL));
            }
            if m.conn.is_some() {
                s.broadcast(member, &json!({ "t": "member_left", "member": member, "reason": reason }));
            }
            Self::forget_member_tokens(&mut inner, &m);
            m
        };
        tracing::info!(session = %sid, member, ban, "member kicked");
        if let Some(r) = &self.relay {
            r.close_member(sid, removed.id, CloseReason::Session);
        }
        Ok(())
    }

    fn forget_member_tokens(inner: &mut Inner, m: &Member) {
        inner.relay_tokens.remove(&m.relay_token);
        inner.dead_relay_tokens.insert(m.relay_token, Instant::now());
        if let Some((t, _)) = &m.attach {
            inner.attach_tokens.remove(t);
        }
    }

    /// A member's WebSocket ended (`reason`: left | timeout). No-op when the member was already
    /// removed (kick, session end) or another socket owns it.
    pub fn member_gone(&self, sid: &str, member: u32, conn_id: u64, reason: &str) {
        if member == 0 {
            let owned = {
                let inner = self.inner();
                inner
                    .sessions
                    .get(sid)
                    .and_then(|s| s.members.get(&0))
                    .and_then(|m| m.conn.as_ref())
                    .is_some_and(|c| c.id == conn_id)
            };
            if owned {
                self.end_session(sid, "host_left");
            }
            return;
        }
        let removed = {
            let mut inner = self.inner();
            let Some(s) = inner.sessions.get_mut(sid) else { return };
            let owned = s.members.get(&member).and_then(|m| m.conn.as_ref()).is_some_and(|c| c.id == conn_id);
            if !owned {
                return;
            }
            let m = s.members.remove(&member).expect("checked");
            s.broadcast(member, &json!({ "t": "member_left", "member": member, "reason": reason }));
            Self::forget_member_tokens(&mut inner, &m);
            m
        };
        tracing::info!(session = %sid, member, reason, "member left");
        if let Some(r) = &self.relay {
            r.close_member(sid, removed.id, CloseReason::Session);
        }
    }

    /// Ends a session: every live member but the host gets `closed {reason}` (the host too for
    /// expired and server_shutdown), sockets close, tokens and relay mappings are freed.
    pub fn end_session(&self, sid: &str, reason: &str) {
        let s = {
            let mut inner = self.inner();
            let Some(s) = inner.sessions.remove(sid) else { return };
            inner.by_code.remove(&s.code);
            for m in s.members.values() {
                Self::forget_member_tokens(&mut inner, m);
            }
            s
        };
        self.limits().host_close(s.host_ip);
        let msg = json!({ "t": "closed", "reason": reason });
        for m in s.members.values() {
            if let Some(c) = &m.conn {
                if m.id != 0 || reason != "host_left" {
                    c.send(&msg);
                }
                c.push(Out::Close(crate::proto::close::NORMAL));
            }
        }
        tracing::info!(session = %sid, reason, "session ended");
        if let Some(r) = &self.relay {
            let why = if reason == "server_shutdown" { CloseReason::Shutdown } else { CloseReason::Session };
            r.close_session(sid, why);
        }
    }

    /// Ends every session (SIGINT/SIGTERM).
    pub fn end_all(&self) {
        let ids: Vec<String> = self.inner().sessions.keys().cloned().collect();
        for id in ids {
            self.end_session(&id, "server_shutdown");
        }
    }

    /// Periodic work: session expiry, unattached REST creates and joins, old state.
    pub fn reap(&self, now: Instant) {
        let mut expired = Vec::new();
        let mut unattached_hosts = Vec::new();
        let mut dropped = Vec::new();
        {
            let mut inner = self.inner();
            let inner = &mut *inner;
            for s in inner.sessions.values_mut() {
                if now >= s.expires_at {
                    expired.push(s.id.clone());
                    continue;
                }
                let late: Vec<u32> = s
                    .members
                    .values()
                    .filter(|m| m.attach.as_ref().is_some_and(|(_, until)| now >= *until))
                    .map(|m| m.id)
                    .collect();
                for id in late {
                    if id == 0 {
                        unattached_hosts.push(s.id.clone());
                    } else if let Some(m) = s.members.remove(&id) {
                        inner.relay_tokens.remove(&m.relay_token);
                        if let Some((t, _)) = &m.attach {
                            inner.attach_tokens.remove(t);
                        }
                        dropped.push((s.id.clone(), id));
                    }
                }
            }
            inner.dead_relay_tokens.retain(|_, t| now.saturating_duration_since(*t) < DEAD_TOKEN_MEMORY);
        }
        for id in expired {
            self.end_session(&id, "expired");
        }
        for id in unattached_hosts {
            self.end_session(&id, "expired");
        }
        if let Some(r) = &self.relay {
            for (sid, m) in dropped {
                r.close_member(&sid, m, CloseReason::Session);
            }
        }
    }

    pub fn relay_lookup(&self, token: &[u8; 16]) -> RelayLookup {
        let inner = self.inner();
        if let Some((s, m)) = inner.relay_tokens.get(token) {
            RelayLookup::Member { session: s.clone(), member: *m }
        } else if inner.dead_relay_tokens.contains_key(token) {
            RelayLookup::Closed
        } else {
            RelayLookup::Unknown
        }
    }

    /// (sessions, live members) for /v1/health.
    pub fn counts(&self) -> (usize, usize) {
        let inner = self.inner();
        let members = inner.sessions.values().map(|s| s.members.values().filter(|m| m.conn.is_some()).count()).sum();
        (inner.sessions.len(), members)
    }
}
