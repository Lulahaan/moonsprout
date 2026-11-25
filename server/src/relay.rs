//! Builtin UDP relay, MPRL v1 (PROTOCOL.md, "Builtin relay").
//!
//! The control port answers REG with OK/ERR. Each client member gets its own relay port R: a
//! datagram from the client's registered address C goes to the host's registered address H and
//! back, both sent from R. Nothing else is forwarded.

use std::collections::{HashMap, HashSet};
use std::net::{IpAddr, SocketAddr, SocketAddrV4};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant};

use tokio::net::UdpSocket;
use tokio::sync::Notify;

use crate::proto::{self, Mprl};
use crate::ratelimit::{Bucket, What};
use crate::session::{App, RelayLookup};

/// At most this many member ports per session.
pub const MAX_PORTS_PER_SESSION: usize = 8;
const MAX_DATAGRAM: usize = 2048;

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum CloseReason {
    Idle,
    Session,
    Shutdown,
}

impl CloseReason {
    fn as_str(self) -> &'static str {
        match self {
            CloseReason::Idle => "idle",
            CloseReason::Session => "session",
            CloseReason::Shutdown => "shutdown",
        }
    }
}

type HostAddr = Arc<Mutex<Option<SocketAddr>>>;

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

struct Mapping {
    session: String,
    member: u32,
    port: u16,
    client: Mutex<SocketAddr>,
    host: HostAddr,
    up: AtomicU64,
    down: AtomicU64,
    last: Mutex<Instant>,
    stop: Notify,
    closed: AtomicBool,
}

#[derive(Default)]
struct RelaySession {
    host: HostAddr,
    mappings: HashMap<u32, Arc<Mapping>>,
}

#[derive(Default)]
struct State {
    sessions: HashMap<String, RelaySession>,
    used_ports: HashSet<u16>,
    cursor: u32,
}

pub struct Relay {
    pub control: Arc<UdpSocket>,
    bind_ip: IpAddr,
    ports: (u16, u16),
    bytes_per_sec: f64,
    idle: Duration,
    state: Mutex<State>,
}

/// The two halves of a mapping freed by `free`, for the log line.
fn log_closed(m: &Mapping, why: CloseReason) {
    crate::emit(&format!(
        "mppm-server: relay mapping session={} member={} port={} up_bytes={} down_bytes={} closed={}",
        m.session,
        m.member,
        m.port,
        m.up.load(Ordering::Relaxed),
        m.down.load(Ordering::Relaxed),
        why.as_str()
    ));
}

impl Relay {
    /// Binds the control port. `kbps` caps each member port per direction.
    pub async fn bind(
        bind_ip: IpAddr,
        port: u16,
        ports: (u16, u16),
        kbps: u32,
        idle: Duration,
    ) -> anyhow::Result<Relay> {
        if !bind_ip.is_ipv4() {
            anyhow::bail!("--relay-bind must be an IPv4 address (relay addresses are IPv4 only)");
        }
        let control = UdpSocket::bind(SocketAddr::new(bind_ip, port))
            .await
            .map_err(|e| anyhow::anyhow!("cannot bind the relay control port {bind_ip}:{port}: {e}"))?;
        Ok(Relay {
            control: Arc::new(control),
            bind_ip,
            ports,
            bytes_per_sec: f64::from(kbps) * 1000.0 / 8.0,
            idle,
            state: Mutex::new(State::default()),
        })
    }

    pub fn local_port(&self) -> u16 {
        self.control.local_addr().map(|a| a.port()).unwrap_or(0)
    }

    fn state(&self) -> MutexGuard<'_, State> {
        lock(&self.state)
    }

    /// Frees a mapping (logs it once) and stops its task.
    fn free(&self, st: &mut State, session: &str, member: u32, why: CloseReason) {
        let Some(rs) = st.sessions.get_mut(session) else { return };
        let Some(m) = rs.mappings.remove(&member) else { return };
        st.used_ports.remove(&m.port);
        if !m.closed.swap(true, Ordering::SeqCst) {
            log_closed(&m, why);
        }
        m.stop.notify_one();
    }

    pub fn close_member(&self, session: &str, member: u32, why: CloseReason) {
        let mut st = self.state();
        self.free(&mut st, session, member, why);
    }

    pub fn close_session(&self, session: &str, why: CloseReason) {
        let mut st = self.state();
        let members: Vec<u32> =
            st.sessions.get(session).map(|s| s.mappings.keys().copied().collect()).unwrap_or_default();
        for m in members {
            self.free(&mut st, session, m, why);
        }
        st.sessions.remove(session);
    }

    pub fn close_all(&self, why: CloseReason) {
        let ids: Vec<String> = self.state().sessions.keys().cloned().collect();
        for id in ids {
            self.close_session(&id, why);
        }
    }

    fn reply(&self, to: SocketAddr, m: &Mprl) {
        // Best effort; the client repeats REG.
        let _ = self.control.try_send_to(&proto::encode(m), to);
    }

    /// The control port loop.
    pub async fn run_control(self: Arc<Self>, app: Arc<App>) {
        let mut buf = vec![0u8; MAX_DATAGRAM];
        loop {
            let (n, from) = match self.control.recv_from(&mut buf).await {
                Ok(x) => x,
                Err(e) => {
                    // ICMP port unreachable from an earlier reply shows up here on some systems.
                    tracing::debug!("relay control recv: {e}");
                    continue;
                }
            };
            let Some(Mprl::Reg { token }) = proto::decode(&buf[..n]) else { continue };
            if !app.limits().allow(from.ip(), What::Reg, Instant::now()) {
                continue; // over 16 REG per minute: silently dropped
            }
            self.handle_reg(&app, token, from).await;
        }
    }

    async fn handle_reg(self: &Arc<Self>, app: &Arc<App>, token: [u8; 16], from: SocketAddr) {
        let seen = match from {
            SocketAddr::V4(a) => a,
            SocketAddr::V6(a) => match a.ip().to_ipv4_mapped() {
                Some(v4) => SocketAddrV4::new(v4, a.port()),
                None => return,
            },
        };
        let (session, member) = match app.relay_lookup(&token) {
            RelayLookup::Member { session, member } => (session, member),
            RelayLookup::Closed => return self.reply(from, &Mprl::Err { code: proto::ERR_SESSION_CLOSED }),
            RelayLookup::Unknown => return self.reply(from, &Mprl::Err { code: proto::ERR_UNKNOWN_TOKEN }),
        };
        // Every `entry(..).or_default()` below runs under the relay lock after the token was looked up
        // again under it: the session store forgets a session's tokens before it calls close_session or
        // close_member (never holding its own lock then), so an entry made after a live lookup is
        // removed by that later close, and none is made for a session that already ended (no leak).
        let live = |app: &Arc<App>| matches!(app.relay_lookup(&token), RelayLookup::Member { .. });
        if member == 0 {
            let mut st = self.state();
            if !live(app) {
                drop(st);
                return self.reply(from, &Mprl::Err { code: proto::ERR_SESSION_CLOSED });
            }
            let rs = st.sessions.entry(session).or_default();
            *lock(&rs.host) = Some(from);
            drop(st);
            return self.reply(from, &Mprl::Ok { role: 0, relay_port: 0, seen });
        }
        // Existing mapping: refresh (and follow a NAT rebinding).
        let host = {
            let mut st = self.state();
            if !live(app) {
                drop(st);
                return self.reply(from, &Mprl::Err { code: proto::ERR_SESSION_CLOSED });
            }
            let rs = st.sessions.entry(session.clone()).or_default();
            if let Some(m) = rs.mappings.get(&member) {
                *lock(&m.client) = from;
                *lock(&m.last) = Instant::now();
                let port = m.port;
                drop(st);
                return self.reply(from, &Mprl::Ok { role: 1, relay_port: port, seen });
            }
            if rs.mappings.len() >= MAX_PORTS_PER_SESSION {
                drop(st);
                return self.reply(from, &Mprl::Err { code: proto::ERR_QUOTA });
            }
            rs.host.clone()
        };
        let Some((port, sock)) = self.bind_free_port().await else {
            return self.reply(from, &Mprl::Err { code: proto::ERR_NO_PORT });
        };
        let mapping = Arc::new(Mapping {
            session: session.clone(),
            member,
            port,
            client: Mutex::new(from),
            host,
            up: AtomicU64::new(0),
            down: AtomicU64::new(0),
            last: Mutex::new(Instant::now()),
            stop: Notify::new(),
            closed: AtomicBool::new(false),
        });
        {
            // The member may have left, or another REG raced us, while the port was bound.
            let mut st = self.state();
            if !live(app) {
                st.used_ports.remove(&port);
                drop(st);
                return self.reply(from, &Mprl::Err { code: proto::ERR_SESSION_CLOSED });
            }
            let rs = st.sessions.entry(session.clone()).or_default();
            if rs.mappings.contains_key(&member) || rs.mappings.len() >= MAX_PORTS_PER_SESSION {
                let existing = rs.mappings.get(&member).map(|m| m.port);
                st.used_ports.remove(&port);
                drop(st);
                return match existing {
                    Some(p) => self.reply(from, &Mprl::Ok { role: 1, relay_port: p, seen }),
                    None => self.reply(from, &Mprl::Err { code: proto::ERR_QUOTA }),
                };
            }
            rs.mappings.insert(member, mapping.clone());
        }
        tracing::info!(session = %session, member, port, "relay mapping opened");
        tokio::spawn(self.clone().run_mapping(mapping, sock));
        self.reply(from, &Mprl::Ok { role: 1, relay_port: port, seen });
    }

    /// Reserves and binds the next free port of --relay-ports.
    async fn bind_free_port(&self) -> Option<(u16, UdpSocket)> {
        let (lo, hi) = self.ports;
        let span = u32::from(hi) - u32::from(lo) + 1;
        for _ in 0..span {
            let port = {
                let mut st = self.state();
                let mut found = None;
                for _ in 0..span {
                    let p = (u32::from(lo) + st.cursor % span) as u16;
                    st.cursor = st.cursor.wrapping_add(1);
                    if !st.used_ports.contains(&p) {
                        found = Some(p);
                        break;
                    }
                }
                let p = found?;
                st.used_ports.insert(p);
                p
            };
            match UdpSocket::bind(SocketAddr::new(self.bind_ip, port)).await {
                Ok(s) => return Some((port, s)),
                Err(_) => {
                    // In use by another program: skip it.
                    self.state().used_ports.remove(&port);
                }
            }
        }
        None
    }

    async fn run_mapping(self: Arc<Self>, m: Arc<Mapping>, sock: UdpSocket) {
        let mut buf = vec![0u8; MAX_DATAGRAM];
        let burst = self.bytes_per_sec.max(MAX_DATAGRAM as f64);
        let now = Instant::now();
        let mut up_bucket = Bucket::new(self.bytes_per_sec, burst, now);
        let mut down_bucket = Bucket::new(self.bytes_per_sec, burst, now);
        let mut tick = tokio::time::interval(Duration::from_millis(500));
        loop {
            tokio::select! {
                _ = m.stop.notified() => break,
                _ = tick.tick() => {
                    let idle_for = lock(&m.last).elapsed();
                    if idle_for >= self.idle {
                        let mut st = self.state();
                        self.free(&mut st, &m.session.clone(), m.member, CloseReason::Idle);
                        break;
                    }
                }
                r = sock.recv_from(&mut buf) => {
                    let Ok((n, from)) = r else { continue };
                    let d = &buf[..n];
                    if proto::is_mprl(d) {
                        continue;
                    }
                    let client = *lock(&m.client);
                    let host = *lock(&m.host);
                    let now = Instant::now();
                    if from == client {
                        let Some(h) = host else { continue };
                        if up_bucket.take(n as f64, now) && sock.try_send_to(d, h).is_ok() {
                            m.up.fetch_add(n as u64, Ordering::Relaxed);
                            *lock(&m.last) = now;
                        }
                    } else if Some(from) == host
                        && down_bucket.take(n as f64, now)
                        && sock.try_send_to(d, client).is_ok()
                    {
                        m.down.fetch_add(n as u64, Ordering::Relaxed);
                        *lock(&m.last) = now;
                    }
                    // Anything else: dropped.
                }
            }
        }
    }
}
