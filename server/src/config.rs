//! Command line (PROTOCOL.md and README.md list every flag).

use std::net::{IpAddr, SocketAddr};
use std::path::PathBuf;

use clap::{Parser, ValueEnum};

#[derive(Copy, Clone, Debug, PartialEq, Eq, ValueEnum)]
pub enum RelayMode {
    /// STUN URLs only (from --stun), no relay.
    None,
    /// --turn with fixed --turn-user/--turn-pass.
    Static,
    /// --turn with coturn use-auth-secret credentials minted from --turn-secret.
    SharedSecret,
    /// The builtin UDP relay (MPRL v1).
    Builtin,
    /// Cloudflare Realtime TURN (not available yet).
    Cloudflare,
}

impl RelayMode {
    pub fn as_str(self) -> &'static str {
        match self {
            RelayMode::None => "none",
            RelayMode::Static => "static",
            RelayMode::SharedSecret => "shared-secret",
            RelayMode::Builtin => "builtin",
            RelayMode::Cloudflare => "cloudflare",
        }
    }
}

#[derive(Parser, Debug, Clone)]
#[command(name = "mppm-server", version, about = "Lobby, signalling and relay server for mppm online play")]
pub struct Args {
    /// HTTP/WebSocket listen address (loopback by default).
    #[arg(long, default_value = "127.0.0.1:8080")]
    pub bind: SocketAddr,
    /// Write the bound HTTP port (and "relay=<port>" on a second line) to this file once listening.
    #[arg(long)]
    pub port_file: Option<PathBuf>,
    /// Append log lines to this file as well as stderr.
    #[arg(long)]
    pub log_file: Option<PathBuf>,
    /// Which ICE servers / relay the clients get.
    #[arg(long, value_enum, env = "MPPM_RELAY_MODE", default_value = "none")]
    pub relay_mode: RelayMode,
    /// Same as --relay-mode builtin.
    #[arg(long)]
    pub relay: bool,
    /// STUN server URL given to clients (repeatable), e.g. stun:stun.example.org:3478.
    #[arg(long)]
    pub stun: Vec<String>,
    /// TURN server URL (static and shared-secret modes), e.g. turn:turn.example.org:3478.
    #[arg(long)]
    pub turn: Option<String>,
    /// TURN username (static mode).
    #[arg(long)]
    pub turn_user: Option<String>,
    /// TURN password (static mode).
    #[arg(long)]
    pub turn_pass: Option<String>,
    /// coturn static-auth-secret (shared-secret mode).
    #[arg(long, env = "MPPM_TURN_SECRET", hide_env_values = true)]
    pub turn_secret: Option<String>,
    /// Lifetime of minted TURN credentials, seconds.
    #[arg(long, default_value_t = 21600)]
    pub turn_ttl: u64,
    /// Builtin relay: address the control port and the member ports bind to.
    #[arg(long, default_value = "127.0.0.1")]
    pub relay_bind: IpAddr,
    /// Builtin relay: UDP control port (0 = any free port).
    #[arg(long, default_value_t = 3479)]
    pub relay_port: u16,
    /// Builtin relay: range of member ports, "LO-HI".
    #[arg(long, default_value = "41000-41999", value_parser = parse_port_range)]
    pub relay_ports: (u16, u16),
    /// Builtin relay: address announced to clients instead of --relay-bind.
    #[arg(long)]
    pub relay_public: Option<IpAddr>,
    /// Builtin relay: bandwidth cap per member port and direction, kbit/s.
    #[arg(long, default_value_t = 2048)]
    pub relay_kbps: u32,
    /// Builtin relay: a member port is freed this long after its last datagram (tests only).
    #[arg(long, default_value_t = 60, hide = true)]
    pub relay_idle_secs: u64,
    /// Hard lifetime of a session, seconds.
    #[arg(long, default_value_t = 21600)]
    pub session_ttl: u64,
    /// Maximum number of live sessions.
    #[arg(long, default_value_t = 2000)]
    pub max_sessions: usize,
    /// Maximum members per session (host included), 2..8.
    #[arg(long, default_value_t = 8, value_parser = clap::value_parser!(u8).range(2..=8))]
    pub max_members: u8,
    /// Multiplies every per-IP rate limit.
    #[arg(long, default_value_t = 1.0)]
    pub limits_scale: f64,
    /// Take the client IP from Fly-Client-IP / X-Forwarded-For (behind a proxy only).
    #[arg(long)]
    pub trust_proxy: bool,
}

fn parse_port_range(s: &str) -> Result<(u16, u16), String> {
    let (lo, hi) = s.split_once('-').ok_or_else(|| format!("expected LO-HI, got {s:?}"))?;
    let lo: u16 = lo.trim().parse().map_err(|_| format!("bad port {lo:?}"))?;
    let hi: u16 = hi.trim().parse().map_err(|_| format!("bad port {hi:?}"))?;
    if lo == 0 || hi < lo {
        return Err(format!("bad port range {s:?}"));
    }
    Ok((lo, hi))
}

impl Args {
    /// The effective relay mode (--relay wins).
    pub fn mode(&self) -> RelayMode {
        if self.relay {
            RelayMode::Builtin
        } else {
            self.relay_mode
        }
    }

    /// Checks flag combinations. Err = message for stderr, exit status 2.
    pub fn validate(&self) -> Result<(), String> {
        match self.mode() {
            RelayMode::Cloudflare => {
                return Err("relay mode cloudflare is not available yet (deferred TURN spike)".into())
            }
            RelayMode::Static => {
                if self.turn.is_none() || self.turn_user.is_none() || self.turn_pass.is_none() {
                    return Err("--relay-mode static needs --turn, --turn-user and --turn-pass".into());
                }
            }
            RelayMode::SharedSecret => {
                if self.turn.is_none() {
                    return Err("--relay-mode shared-secret needs --turn".into());
                }
                if self.turn_secret.as_deref().unwrap_or("").is_empty() {
                    return Err("--relay-mode shared-secret needs --turn-secret or MPPM_TURN_SECRET".into());
                }
            }
            RelayMode::None | RelayMode::Builtin => {}
        }
        for url in self.stun.iter().chain(self.turn.iter()) {
            if url.is_empty() || url.len() > 256 || url.chars().any(|c| c.is_control() || c == ' ') {
                return Err(format!("bad ICE server URL {url:?}"));
            }
        }
        if !(self.limits_scale.is_finite() && self.limits_scale > 0.0) {
            return Err("--limits-scale must be a positive number".into());
        }
        if self.session_ttl == 0 || self.turn_ttl == 0 || self.relay_idle_secs == 0 {
            return Err("--session-ttl, --turn-ttl and --relay-idle-secs must be at least 1".into());
        }
        if self.relay_kbps == 0 {
            return Err("--relay-kbps must be at least 1".into());
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn port_range() {
        assert_eq!(parse_port_range("41000-41999"), Ok((41000, 41999)));
        assert_eq!(parse_port_range("5-5"), Ok((5, 5)));
        assert!(parse_port_range("10-5").is_err());
        assert!(parse_port_range("0-5").is_err());
        assert!(parse_port_range("abc").is_err());
    }

    #[test]
    fn cloudflare_refused() {
        let a = Args::parse_from(["mppm-server", "--relay-mode", "cloudflare"]);
        assert_eq!(a.validate().unwrap_err(), "relay mode cloudflare is not available yet (deferred TURN spike)");
    }

    #[test]
    fn relay_flag_is_builtin() {
        let a = Args::parse_from(["mppm-server", "--relay"]);
        assert_eq!(a.mode(), RelayMode::Builtin);
        assert!(a.validate().is_ok());
    }
}
