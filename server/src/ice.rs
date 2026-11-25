//! ICE server lists per relay mode (PROTOCOL.md, "ICE servers and relay modes").

use base64::Engine as _;
use hmac::{Hmac, Mac};
use serde_json::{json, Value};
use sha1::Sha1;

use crate::config::{Args, RelayMode};

/// coturn `use-auth-secret` (TURN REST API) credential: base64(HMAC-SHA1(secret, username)).
pub fn turn_credential(secret: &str, username: &str) -> String {
    let mut mac = Hmac::<Sha1>::new_from_slice(secret.as_bytes()).expect("HMAC takes any key length");
    mac.update(username.as_bytes());
    base64::engine::general_purpose::STANDARD.encode(mac.finalize().into_bytes())
}

/// What the server hands out; built once from the command line.
#[derive(Debug, Clone)]
pub struct IceConfig {
    mode: RelayMode,
    stun: Vec<String>,
    turn: Option<String>,
    turn_user: String,
    turn_pass: String,
    turn_secret: String,
    turn_ttl: u64,
}

impl IceConfig {
    pub fn from_args(a: &Args) -> IceConfig {
        IceConfig {
            mode: a.mode(),
            stun: a.stun.clone(),
            turn: a.turn.clone(),
            turn_user: a.turn_user.clone().unwrap_or_default(),
            turn_pass: a.turn_pass.clone().unwrap_or_default(),
            turn_secret: a.turn_secret.clone().unwrap_or_default(),
            turn_ttl: a.turn_ttl,
        }
    }

    /// The `ice` array for one member at unix time `now`.
    pub fn list(&self, member: u32, now: u64) -> Value {
        let mut out: Vec<Value> = self.stun.iter().map(|u| json!({ "urls": [u] })).collect();
        match (self.mode, &self.turn) {
            (RelayMode::Static, Some(turn)) => out.push(json!({
                "urls": [turn],
                "username": self.turn_user,
                "credential": self.turn_pass,
            })),
            (RelayMode::SharedSecret, Some(turn)) => {
                let username = format!("{}:{}", now + self.turn_ttl, member);
                let credential = turn_credential(&self.turn_secret, &username);
                out.push(json!({ "urls": [turn], "username": username, "credential": credential }));
            }
            _ => {}
        }
        Value::Array(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use clap::Parser;

    #[test]
    fn hmac_vector() {
        // python3: base64.b64encode(hmac.new(b"north", b"1764000000:3", hashlib.sha1).digest())
        assert_eq!(turn_credential("north", "1764000000:3"), "tz73U4Kkw6KkWeFfmzeQRzoBQe0=");
    }

    #[test]
    fn lists() {
        let a = Args::parse_from(["mppm-server", "--stun", "stun:127.0.0.1:3478"]);
        assert_eq!(IceConfig::from_args(&a).list(1, 0), json!([{ "urls": ["stun:127.0.0.1:3478"] }]));

        let a = Args::parse_from(["mppm-server", "--relay"]);
        assert_eq!(IceConfig::from_args(&a).list(1, 0), json!([]));

        let a = Args::parse_from([
            "mppm-server",
            "--relay-mode",
            "static",
            "--turn",
            "turn:127.0.0.1:3479",
            "--turn-user",
            "u",
            "--turn-pass",
            "p",
        ]);
        assert_eq!(
            IceConfig::from_args(&a).list(2, 0),
            json!([{ "urls": ["turn:127.0.0.1:3479"], "username": "u", "credential": "p" }])
        );

        let a = Args::parse_from([
            "mppm-server",
            "--relay-mode",
            "shared-secret",
            "--turn",
            "turn:t:3478",
            "--turn-secret",
            "north",
            "--turn-ttl",
            "100",
            "--stun",
            "stun:s:3478",
        ]);
        assert_eq!(
            IceConfig::from_args(&a).list(3, 1_763_999_900),
            json!([
                { "urls": ["stun:s:3478"] },
                { "urls": ["turn:t:3478"], "username": "1764000000:3", "credential": "tz73U4Kkw6KkWeFfmzeQRzoBQe0=" } // gitleaks:allow (test vector for the secret "north")
            ])
        );
    }
}
