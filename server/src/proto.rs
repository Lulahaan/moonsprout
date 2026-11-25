//! Wire formats: the WebSocket client messages (JSON) and the MPRL v1 relay datagrams.

use std::net::{Ipv4Addr, SocketAddrV4};

use serde_json::Value;

pub const PROTOCOL_VERSION: u64 = 1;
pub const MAX_FRAME_BYTES: usize = 16 * 1024;
pub const MAX_SIGNAL_DATA: usize = 4096;
pub const MAX_PASSWORD_BYTES: usize = 64;
pub const MAX_NAME_INPUT_BYTES: usize = 256;
pub const SIGNAL_KINDS: [&str; 4] = ["desc", "cand", "done", "addr"];

/// WebSocket close codes.
pub mod close {
    pub const NORMAL: u16 = 1000;
    /// The first message was not create/join/attach, or did not arrive within 10 s.
    pub const FIRST_MESSAGE: u16 = 4000;
    /// Unsupported protocol version.
    pub const VERSION: u16 = 4001;
    /// A frame that is not a JSON text message of at most 16 KiB, or a malformed message.
    pub const FRAME: u16 = 4002;
    /// A fatal error (rate limit, refused join, ...); the error message precedes the close.
    pub const REFUSED: u16 = 4003;
    /// No message for 60 s.
    pub const TIMEOUT: u16 = 4004;
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ClientMsg {
    Create { v: Option<u64>, name: String, password: String, max_members: Option<u64> },
    Join { v: Option<u64>, code: String, name: String, password: String, spectator: bool },
    Attach { v: Option<u64>, token: String },
    Signal { to: u64, kind: String, data: String },
    Kick { member: u64, ban: bool },
    Leave,
    Ping,
}

impl ClientMsg {
    pub fn version(&self) -> Option<u64> {
        match self {
            ClientMsg::Create { v, .. } | ClientMsg::Join { v, .. } | ClientMsg::Attach { v, .. } => *v,
            _ => None,
        }
    }
}

fn opt_str(o: &serde_json::Map<String, Value>, key: &str, max: usize) -> Result<String, String> {
    match o.get(key) {
        None | Some(Value::Null) => Ok(String::new()),
        Some(Value::String(s)) if s.len() <= max => Ok(s.clone()),
        Some(Value::String(_)) => Err(format!("\"{key}\" is too long")),
        Some(_) => Err(format!("\"{key}\" must be a string")),
    }
}

fn req_str(o: &serde_json::Map<String, Value>, key: &str, max: usize) -> Result<String, String> {
    match o.get(key) {
        Some(Value::String(s)) if s.len() <= max => Ok(s.clone()),
        Some(Value::String(_)) => Err(format!("\"{key}\" is too long")),
        _ => Err(format!("\"{key}\" must be a string")),
    }
}

fn opt_u64(o: &serde_json::Map<String, Value>, key: &str) -> Result<Option<u64>, String> {
    match o.get(key) {
        None | Some(Value::Null) => Ok(None),
        Some(v) => v.as_u64().map(Some).ok_or_else(|| format!("\"{key}\" must be a non-negative integer")),
    }
}

fn req_u64(o: &serde_json::Map<String, Value>, key: &str) -> Result<u64, String> {
    opt_u64(o, key)?.ok_or_else(|| format!("\"{key}\" is missing"))
}

fn opt_bool(o: &serde_json::Map<String, Value>, key: &str) -> Result<bool, String> {
    match o.get(key) {
        None | Some(Value::Null) => Ok(false),
        Some(Value::Bool(b)) => Ok(*b),
        Some(_) => Err(format!("\"{key}\" must be a boolean")),
    }
}

/// Parses one client text frame. Err = a human-readable reason (bad_request).
pub fn parse_client(text: &str) -> Result<ClientMsg, String> {
    let v: Value = serde_json::from_str(text).map_err(|_| "not valid JSON".to_string())?;
    let o = v.as_object().ok_or("not a JSON object")?;
    let t = o.get("t").and_then(Value::as_str).ok_or("\"t\" is missing")?;
    Ok(match t {
        "create" => ClientMsg::Create {
            v: opt_u64(o, "v")?,
            name: opt_str(o, "name", MAX_NAME_INPUT_BYTES)?,
            password: opt_str(o, "password", MAX_PASSWORD_BYTES)?,
            max_members: opt_u64(o, "maxMembers")?,
        },
        "join" => ClientMsg::Join {
            v: opt_u64(o, "v")?,
            code: req_str(o, "code", 64)?,
            name: opt_str(o, "name", MAX_NAME_INPUT_BYTES)?,
            password: opt_str(o, "password", MAX_PASSWORD_BYTES)?,
            spectator: opt_bool(o, "spectator")?,
        },
        "attach" => ClientMsg::Attach { v: opt_u64(o, "v")?, token: req_str(o, "token", 64)? },
        "signal" => {
            let kind = req_str(o, "kind", 16)?;
            if !SIGNAL_KINDS.contains(&kind.as_str()) {
                return Err("\"kind\" must be desc, cand, done or addr".into());
            }
            ClientMsg::Signal { to: req_u64(o, "to")?, kind, data: req_str(o, "data", MAX_FRAME_BYTES)? }
        }
        "kick" => ClientMsg::Kick { member: req_u64(o, "member")?, ban: opt_bool(o, "ban")? },
        "leave" => ClientMsg::Leave,
        "ping" => ClientMsg::Ping,
        _ => return Err(format!("unknown message type {t:?}")),
    })
}

// ---- MPRL v1 (big-endian) ----

pub const MPRL_MAGIC: [u8; 4] = *b"MPRL";
pub const MPRL_VERSION: u8 = 1;
pub const T_REG: u8 = 1;
pub const T_OK: u8 = 2;
pub const T_ERR: u8 = 3;
pub const T_PUNCH: u8 = 4;

pub const ERR_UNKNOWN_TOKEN: u8 = 1;
pub const ERR_SESSION_CLOSED: u8 = 2;
pub const ERR_QUOTA: u8 = 3;
pub const ERR_NO_PORT: u8 = 4;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mprl {
    Reg { token: [u8; 16] },
    Ok { role: u8, relay_port: u16, seen: SocketAddrV4 },
    Err { code: u8 },
    Punch { nonce: [u8; 8] },
}

/// True for any datagram that starts with the MPRL magic (such datagrams are never forwarded).
pub fn is_mprl(d: &[u8]) -> bool {
    d.len() >= 4 && d[..4] == MPRL_MAGIC
}

fn header(t: u8) -> Vec<u8> {
    let mut v = Vec::with_capacity(24);
    v.extend_from_slice(&MPRL_MAGIC);
    v.push(MPRL_VERSION);
    v.push(t);
    v.extend_from_slice(&[0, 0]);
    v
}

pub fn encode(m: &Mprl) -> Vec<u8> {
    match *m {
        Mprl::Reg { token } => {
            let mut v = header(T_REG);
            v.extend_from_slice(&token);
            v
        }
        Mprl::Ok { role, relay_port, seen } => {
            let mut v = header(T_OK);
            v.push(role);
            v.extend_from_slice(&relay_port.to_be_bytes());
            v.push(4);
            v.extend_from_slice(&seen.ip().octets());
            v.extend_from_slice(&seen.port().to_be_bytes());
            v
        }
        Mprl::Err { code } => {
            let mut v = header(T_ERR);
            v.push(code);
            v
        }
        Mprl::Punch { nonce } => {
            let mut v = header(T_PUNCH);
            v.extend_from_slice(&nonce);
            v
        }
    }
}

/// Decodes one datagram; None for anything that is not exactly a valid MPRL v1 message.
pub fn decode(d: &[u8]) -> Option<Mprl> {
    if d.len() < 8 || d[..4] != MPRL_MAGIC || d[4] != MPRL_VERSION || d[6] != 0 || d[7] != 0 {
        return None;
    }
    match (d[5], d.len()) {
        (T_REG, 24) => Some(Mprl::Reg { token: d[8..24].try_into().ok()? }),
        (T_OK, 18) if d[11] == 4 => Some(Mprl::Ok {
            role: d[8],
            relay_port: u16::from_be_bytes([d[9], d[10]]),
            seen: SocketAddrV4::new(Ipv4Addr::new(d[12], d[13], d[14], d[15]), u16::from_be_bytes([d[16], d[17]])),
        }),
        (T_ERR, 9) => Some(Mprl::Err { code: d[8] }),
        (T_PUNCH, 16) => Some(Mprl::Punch { nonce: d[8..16].try_into().ok()? }),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mprl_layouts() {
        let token: [u8; 16] = core::array::from_fn(|i| i as u8 + 1);
        let reg = encode(&Mprl::Reg { token });
        assert_eq!(reg.len(), 24);
        assert_eq!(&reg[..8], &[0x4D, 0x50, 0x52, 0x4C, 1, 1, 0, 0]);
        assert_eq!(&reg[8..], &token);

        let seen = SocketAddrV4::new(Ipv4Addr::new(127, 0, 0, 1), 0xABCD);
        let ok = encode(&Mprl::Ok { role: 1, relay_port: 41000, seen });
        assert_eq!(ok, vec![0x4D, 0x50, 0x52, 0x4C, 1, 2, 0, 0, 1, 0xA0, 0x28, 4, 127, 0, 0, 1, 0xAB, 0xCD]);

        let err = encode(&Mprl::Err { code: ERR_QUOTA });
        assert_eq!(err, vec![0x4D, 0x50, 0x52, 0x4C, 1, 3, 0, 0, 3]);

        let punch = encode(&Mprl::Punch { nonce: [9, 8, 7, 6, 5, 4, 3, 2] });
        assert_eq!(punch, vec![0x4D, 0x50, 0x52, 0x4C, 1, 4, 0, 0, 9, 8, 7, 6, 5, 4, 3, 2]);

        for m in [
            Mprl::Reg { token },
            Mprl::Ok { role: 0, relay_port: 0, seen },
            Mprl::Err { code: 4 },
            Mprl::Punch { nonce: [1; 8] },
        ] {
            assert_eq!(decode(&encode(&m)), Some(m));
        }
    }

    #[test]
    fn mprl_rejects() {
        let reg = encode(&Mprl::Reg { token: [0; 16] });
        assert!(decode(&reg[..23]).is_none()); // short
        let mut long = reg.clone();
        long.push(0);
        assert!(decode(&long).is_none());
        let mut bad = reg.clone();
        bad[4] = 2; // version
        assert!(decode(&bad).is_none());
        let mut bad = reg.clone();
        bad[6] = 1; // reserved
        assert!(decode(&bad).is_none());
        let mut bad = reg.clone();
        bad[5] = 9; // type
        assert!(decode(&bad).is_none());
        let mut ok = encode(&Mprl::Ok { role: 0, relay_port: 0, seen: SocketAddrV4::new(Ipv4Addr::LOCALHOST, 1) });
        ok[11] = 6; // only IPv4
        assert!(decode(&ok).is_none());
        assert!(is_mprl(b"MPRL"));
        assert!(!is_mprl(b"MPR"));
        assert!(!is_mprl(b"\x00MPRL"));
    }

    #[test]
    fn client_messages() {
        assert_eq!(
            parse_client(r#"{"t":"create","v":1,"name":"Olimar","password":"","maxMembers":8}"#),
            Ok(ClientMsg::Create { v: Some(1), name: "Olimar".into(), password: String::new(), max_members: Some(8) })
        );
        assert_eq!(
            parse_client(r#"{"t":"join","v":1,"code":"ABCD-EFGH","name":"Louie","spectator":true}"#),
            Ok(ClientMsg::Join {
                v: Some(1),
                code: "ABCD-EFGH".into(),
                name: "Louie".into(),
                password: String::new(),
                spectator: true
            })
        );
        assert_eq!(
            parse_client(r#"{"t":"signal","to":0,"kind":"addr","data":"x"}"#),
            Ok(ClientMsg::Signal { to: 0, kind: "addr".into(), data: "x".into() })
        );
        assert_eq!(parse_client(r#"{"t":"kick","member":3}"#), Ok(ClientMsg::Kick { member: 3, ban: false }));
        assert_eq!(parse_client(r#"{"t":"ping"}"#), Ok(ClientMsg::Ping));
        assert!(parse_client(r#"{"t":"signal","to":0,"kind":"evil","data":"x"}"#).is_err());
        assert!(parse_client(r#"{"t":"signal","to":-1,"kind":"addr","data":"x"}"#).is_err());
        assert!(parse_client(r#"{"t":"join","v":1}"#).is_err());
        assert!(parse_client(r#"{"t":"nope"}"#).is_err());
        assert!(parse_client(r#"[1,2]"#).is_err());
        assert!(parse_client("{").is_err());
        assert!(parse_client(r#"{"t":"create","name":5}"#).is_err());
        let long = "x".repeat(65);
        assert!(parse_client(&format!(r#"{{"t":"create","password":"{long}"}}"#)).is_err());
    }
}
