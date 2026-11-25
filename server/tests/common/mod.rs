//! Test helpers: start the real mppm-server binary on 127.0.0.1, a WebSocket client, a tiny HTTP
//! client. Relay member ports come from 47000-47049 (each test file uses its own sub-range).
#![allow(dead_code)]

use std::io::{BufRead, BufReader, Read, Write};
use std::net::TcpStream;
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use tokio::net::TcpStream as TokioTcp;
use tokio_tungstenite::tungstenite::protocol::frame::coding::CloseCode;
use tokio_tungstenite::tungstenite::Message;
use tokio_tungstenite::{MaybeTlsStream, WebSocketStream};

pub const T: Duration = Duration::from_secs(5);

pub struct Server {
    pub child: Child,
    pub http: u16,
    pub relay: Option<u16>,
    pub log: Arc<Mutex<Vec<String>>>,
    pub ready_line: String,
}

impl Server {
    pub fn start(extra: &[&str]) -> Server {
        let mut cmd = Command::new(env!("CARGO_BIN_EXE_mppm-server"));
        cmd.args(["--bind", "127.0.0.1:0"]).args(extra);
        cmd.env_remove("MPPM_TURN_SECRET").env_remove("RUST_LOG");
        cmd.stdout(Stdio::null()).stderr(Stdio::piped()).stdin(Stdio::null());
        let mut child = cmd.spawn().expect("spawn mppm-server");
        let stderr = child.stderr.take().unwrap();
        let log = Arc::new(Mutex::new(Vec::new()));
        let (tx, rx) = std::sync::mpsc::channel::<String>();
        {
            let log = log.clone();
            std::thread::spawn(move || {
                let mut sent = false;
                for line in BufReader::new(stderr).lines() {
                    let Ok(line) = line else { break };
                    if !sent {
                        let _ = tx.send(line.clone());
                        sent = true;
                    }
                    log.lock().unwrap().push(line);
                }
            });
        }
        let ready_line = rx.recv_timeout(Duration::from_secs(10)).expect("ready line");
        assert!(ready_line.starts_with("mppm-server: listening http=127.0.0.1:"), "first line: {ready_line}");
        let field =
            |k: &str| -> Option<String> { ready_line.split(' ').find_map(|w| w.strip_prefix(k).map(str::to_string)) };
        let http: u16 = field("http=").unwrap().rsplit(':').next().unwrap().parse().unwrap();
        let relay = field("relay=").filter(|r| r != "-").map(|r| r.rsplit(':').next().unwrap().parse().unwrap());
        Server { child, http, relay, log, ready_line }
    }

    pub fn url(&self) -> String {
        format!("ws://127.0.0.1:{}/v1/ws", self.http)
    }

    pub fn log_lines(&self) -> Vec<String> {
        self.log.lock().unwrap().clone()
    }

    /// Waits until a stderr line contains `needle`.
    pub fn wait_log(&self, needle: &str, timeout: Duration) -> String {
        let end = Instant::now() + timeout;
        loop {
            if let Some(l) = self.log_lines().into_iter().find(|l| l.contains(needle)) {
                return l;
            }
            assert!(Instant::now() < end, "no log line with {needle:?}; log: {:?}", self.log_lines());
            std::thread::sleep(Duration::from_millis(20));
        }
    }

    pub fn terminate(&mut self) -> i32 {
        let _ = Command::new("kill").args(["-TERM", &self.child.id().to_string()]).status();
        let end = Instant::now() + Duration::from_secs(10);
        loop {
            if let Some(st) = self.child.try_wait().unwrap() {
                return st.code().unwrap_or(-1);
            }
            assert!(Instant::now() < end, "server did not exit after SIGTERM");
            std::thread::sleep(Duration::from_millis(20));
        }
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// Runs a server binary with these flags to completion; (exit code, stderr).
pub fn run_to_exit(extra: &[&str]) -> (i32, String) {
    let out = Command::new(env!("CARGO_BIN_EXE_mppm-server"))
        .args(extra)
        .env_remove("MPPM_TURN_SECRET")
        .stdin(Stdio::null())
        .output()
        .unwrap();
    (out.status.code().unwrap_or(-1), String::from_utf8_lossy(&out.stderr).into_owned())
}

// ---- HTTP ----

pub fn http(port: u16, method: &str, path: &str, body: Option<&str>) -> (u16, Value) {
    let mut s = TcpStream::connect(("127.0.0.1", port)).unwrap();
    s.set_read_timeout(Some(T)).unwrap();
    let body = body.unwrap_or("");
    let req = format!(
        "{method} {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len()
    );
    s.write_all(req.as_bytes()).unwrap();
    let mut resp = Vec::new();
    let _ = s.read_to_end(&mut resp);
    let resp = String::from_utf8_lossy(&resp).into_owned();
    let status: u16 = resp.split(' ').nth(1).unwrap().parse().unwrap();
    let (head, rest) = resp.split_once("\r\n\r\n").unwrap();
    let text =
        if head.to_ascii_lowercase().contains("transfer-encoding: chunked") { dechunk(rest) } else { rest.to_string() };
    (status, serde_json::from_str(&text).unwrap_or(Value::Null))
}

fn dechunk(mut s: &str) -> String {
    let mut out = String::new();
    while let Some((len, rest)) = s.split_once("\r\n") {
        let n = usize::from_str_radix(len.trim(), 16).unwrap_or(0);
        if n == 0 {
            break;
        }
        out.push_str(&rest[..n]);
        s = &rest[n + 2..];
    }
    out
}

// ---- WebSocket ----

pub struct Ws {
    pub s: WebSocketStream<MaybeTlsStream<TokioTcp>>,
}

/// What came out of the socket.
#[derive(Debug)]
pub enum Got {
    Json(Value),
    Closed(Option<u16>),
}

impl Ws {
    pub async fn connect(url: &str) -> Ws {
        let (s, _) = tokio_tungstenite::connect_async(url).await.expect("ws connect");
        Ws { s }
    }

    pub async fn send(&mut self, v: Value) {
        self.s.send(Message::Text(v.to_string().into())).await.unwrap();
    }

    pub async fn send_text(&mut self, t: String) {
        self.s.send(Message::Text(t.into())).await.unwrap();
    }

    pub async fn send_msg(&mut self, m: Message) {
        self.s.send(m).await.unwrap();
    }

    pub async fn next(&mut self, timeout: Duration) -> Option<Got> {
        let end = tokio::time::Instant::now() + timeout;
        loop {
            match tokio::time::timeout_at(end, self.s.next()).await {
                Err(_) => return None,
                Ok(None) | Ok(Some(Err(_))) => return Some(Got::Closed(None)),
                Ok(Some(Ok(Message::Text(t)))) => return Some(Got::Json(serde_json::from_str(t.as_str()).unwrap())),
                Ok(Some(Ok(Message::Close(f)))) => return Some(Got::Closed(f.map(|f| u16::from(f.code)))),
                Ok(Some(Ok(_))) => continue,
            }
        }
    }

    /// The next JSON message (panics on close or timeout).
    pub async fn recv(&mut self) -> Value {
        match self.next(T).await {
            Some(Got::Json(v)) => v,
            other => panic!("expected a message, got {other:?}"),
        }
    }

    /// The next message of type `t`, skipping others.
    pub async fn recv_t(&mut self, t: &str) -> Value {
        loop {
            let v = self.recv().await;
            if v["t"] == t {
                return v;
            }
        }
    }

    /// Reads to the close frame: (messages before it, close code).
    pub async fn until_closed(&mut self) -> (Vec<Value>, Option<u16>) {
        let mut msgs = Vec::new();
        loop {
            match self.next(T).await {
                Some(Got::Json(v)) => msgs.push(v),
                Some(Got::Closed(c)) => return (msgs, c),
                None => panic!("socket not closed; got {msgs:?}"),
            }
        }
    }

    /// Asserts nothing (but control frames) arrives for `ms`.
    pub async fn silent(&mut self, ms: u64) {
        if let Some(g) = self.next(Duration::from_millis(ms)).await {
            panic!("expected silence, got {g:?}");
        }
    }

    pub async fn close(mut self) {
        let _ = self.s.close(None).await;
    }
}

pub fn close_code(c: CloseCode) -> u16 {
    u16::from(c)
}

pub async fn create(url: &str, name: &str) -> (Ws, Value) {
    let mut ws = Ws::connect(url).await;
    ws.send(json!({"t":"create","v":1,"name":name,"password":"","maxMembers":8})).await;
    let v = ws.recv().await;
    assert_eq!(v["t"], "created", "{v}");
    (ws, v)
}

pub async fn join(url: &str, code: &str, name: &str, spectator: bool) -> (Ws, Value) {
    let mut ws = Ws::connect(url).await;
    ws.send(json!({"t":"join","v":1,"code":code,"name":name,"password":"","spectator":spectator})).await;
    let v = ws.recv().await;
    (ws, v)
}

pub fn is_hex(v: &Value, len: usize) -> bool {
    v.as_str().is_some_and(|s| s.len() == len && s.bytes().all(|b| b.is_ascii_hexdigit()))
}
