//! `GET /v1/ws`: one task per WebSocket (PROTOCOL.md, "WebSocket protocol").

use std::net::IpAddr;
use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::time::{Duration, Instant};

use axum::extract::ws::{CloseFrame, Message, WebSocket};
use serde_json::json;
use tokio::sync::{mpsc, Notify};

use crate::proto::{self, close, ClientMsg};
use crate::ratelimit::Bucket;
use crate::session::{Admitted, App, Out, Refusal, Via, OUTBOUND_QUEUE};

const FIRST_MESSAGE_TIMEOUT: Duration = Duration::from_secs(10);
const SILENCE_TIMEOUT: Duration = Duration::from_secs(60);
const MSG_RATE: f64 = 40.0;
const MSG_BURST: f64 = 80.0;
const EXCESS_FATAL_AFTER: Duration = Duration::from_secs(5);

/// One received frame, classified.
enum Frame {
    Text(String),
    /// Binary, or text over 16 KiB.
    Bad(&'static str, &'static str),
    /// Ping/pong frames: activity only.
    Control,
    /// Close frame, error or end of stream.
    Gone,
}

fn classify(m: Option<Result<Message, axum::Error>>) -> Frame {
    match m {
        Some(Ok(Message::Text(t))) if t.len() <= proto::MAX_FRAME_BYTES => Frame::Text(t.as_str().to_owned()),
        Some(Ok(Message::Text(_))) => Frame::Bad("too_large", "Messages are limited to 16 KiB."),
        Some(Ok(Message::Binary(_))) => Frame::Bad("bad_request", "Only JSON text messages are accepted."),
        Some(Ok(Message::Ping(_) | Message::Pong(_))) => Frame::Control,
        Some(Ok(Message::Close(_))) | Some(Err(_)) | None => Frame::Gone,
    }
}

async fn send_json(sock: &mut WebSocket, v: &serde_json::Value) -> bool {
    sock.send(Message::Text(v.to_string().into())).await.is_ok()
}

async fn send_close(sock: &mut WebSocket, code: u16) {
    let _ = sock.send(Message::Close(Some(CloseFrame { code, reason: "".into() }))).await;
}

async fn fail(sock: &mut WebSocket, code: &str, msg: &str, close_code: u16) {
    send_json(sock, &json!({ "t": "error", "code": code, "msg": msg, "fatal": true })).await;
    send_close(sock, close_code).await;
}

fn error_json(r: &Refusal, fatal: bool) -> serde_json::Value {
    json!({ "t": "error", "code": r.code, "msg": r.msg, "fatal": fatal })
}

/// Runs one socket; the live-WebSocket slot of `ip` was taken by the upgrade handler.
pub async fn serve(app: Arc<App>, mut sock: WebSocket, ip: IpAddr) {
    app.live_ws.fetch_add(1, Ordering::SeqCst);
    run(&app, &mut sock, ip).await;
    app.limits().ws_close(ip);
    app.live_ws.fetch_sub(1, Ordering::SeqCst);
}

async fn run(app: &Arc<App>, sock: &mut WebSocket, ip: IpAddr) {
    let mut shutdown = app.shutdown.subscribe();
    if *shutdown.borrow_and_update() {
        return fail(sock, "rate_limited", "The server is shutting down.", close::REFUSED).await;
    }

    // ---- first message: create, join or attach within 10 s ----
    let deadline = tokio::time::Instant::now() + FIRST_MESSAGE_TIMEOUT;
    let text = loop {
        let frame = tokio::select! {
            _ = shutdown.changed() => return send_close(sock, close::NORMAL).await,
            r = tokio::time::timeout_at(deadline, sock.recv()) => match r {
                Ok(m) => classify(m),
                Err(_) => return fail(sock, "bad_request", "The first message must arrive within 10 seconds.", close::FIRST_MESSAGE).await,
            },
        };
        match frame {
            Frame::Text(t) => break t,
            Frame::Control => continue,
            Frame::Bad(code, msg) => return fail(sock, code, msg, close::FRAME).await,
            Frame::Gone => return,
        }
    };
    let first = match proto::parse_client(&text) {
        Ok(m @ (ClientMsg::Create { .. } | ClientMsg::Join { .. } | ClientMsg::Attach { .. })) => m,
        Ok(_) => {
            return fail(sock, "bad_request", "The first message must be create, join or attach.", close::FIRST_MESSAGE)
                .await
        }
        Err(e) => return fail(sock, "bad_request", &e, close::FIRST_MESSAGE).await,
    };
    if first.version() != Some(proto::PROTOCOL_VERSION) {
        return fail(sock, "version", "This server speaks protocol version 1; update the game.", close::VERSION).await;
    }

    let (tx, mut rx) = mpsc::channel::<Out>(OUTBOUND_QUEUE);
    let kill = Arc::new(Notify::new());
    let conn = app.new_conn(tx, kill.clone());
    let conn_id = conn.id;
    let admitted = match first {
        ClientMsg::Create { name, password, max_members, .. } => {
            app.create(ip, &name, &password, max_members, Via::Ws(conn))
        }
        ClientMsg::Join { code, name, password, spectator, .. } => {
            app.join(ip, &code, &name, &password, spectator, Via::Ws(conn))
        }
        ClientMsg::Attach { token, .. } => app.attach(&token, conn),
        _ => unreachable!("checked above"),
    };
    let Admitted { session, member, reply } = match admitted {
        Ok(a) => a,
        Err(r) => return fail(sock, r.code, &r.msg, close::REFUSED).await,
    };
    // The reply goes out first; messages queued for us meanwhile (member_joined, ...) follow it.
    if !send_json(sock, &reply).await {
        app.member_gone(&session, member, conn_id, "left");
        return;
    }

    // ---- session loop ----
    let reason = session_loop(app, sock, &session, member, &mut rx, &kill, &mut shutdown).await;
    app.member_gone(&session, member, conn_id, reason);
}

/// Returns the member_left reason ("left" or "timeout").
async fn session_loop(
    app: &Arc<App>,
    sock: &mut WebSocket,
    session: &str,
    member: u32,
    rx: &mut mpsc::Receiver<Out>,
    kill: &Notify,
    shutdown: &mut tokio::sync::watch::Receiver<bool>,
) -> &'static str {
    let mut last_rx = tokio::time::Instant::now();
    let mut bucket = Bucket::new(MSG_RATE, MSG_BURST, Instant::now());
    let mut excess_start: Option<Instant> = None;
    let mut last_excess = Instant::now();
    let mut last_rate_error: Option<Instant> = None;
    loop {
        tokio::select! {
            biased;
            out = rx.recv() => match out {
                Some(Out::Text(t)) => {
                    if sock.send(Message::Text(t.into())).await.is_err() {
                        return "left";
                    }
                }
                Some(Out::Close(code)) => {
                    send_close(sock, code).await;
                    return "left";
                }
                None => return "left",
            },
            _ = kill.notified() => {
                send_close(sock, close::REFUSED).await;
                return "left";
            }
            _ = shutdown.changed() => {
                // end_all() normally queued `closed` already; this covers a socket it missed.
                send_json(sock, &json!({ "t": "closed", "reason": "server_shutdown" })).await;
                send_close(sock, close::NORMAL).await;
                return "left";
            }
            r = tokio::time::timeout_at(last_rx + SILENCE_TIMEOUT, sock.recv()) => {
                let frame = match r {
                    Ok(m) => classify(m),
                    Err(_) => {
                        send_close(sock, close::TIMEOUT).await;
                        return "timeout";
                    }
                };
                last_rx = tokio::time::Instant::now();
                let text = match frame {
                    Frame::Text(t) => t,
                    Frame::Control => continue,
                    Frame::Gone => return "left",
                    Frame::Bad(code, msg) => {
                        fail(sock, code, msg, close::FRAME).await;
                        return "left";
                    }
                };
                let now = Instant::now();
                if !bucket.take(1.0, now) {
                    if excess_start.is_none() || now.duration_since(last_excess) > Duration::from_secs(1) {
                        excess_start = Some(now);
                    }
                    last_excess = now;
                    if excess_start.is_some_and(|s| now.duration_since(s) >= EXCESS_FATAL_AFTER) {
                        fail(sock, "rate_limited", "Too many messages.", close::REFUSED).await;
                        return "left";
                    }
                    if last_rate_error.is_none_or(|t| now.duration_since(t) >= Duration::from_secs(1)) {
                        last_rate_error = Some(now);
                        let r = Refusal::new("rate_limited", "Too many messages; some were dropped.");
                        send_json(sock, &error_json(&r, false)).await;
                    }
                    continue;
                }
                let msg = match proto::parse_client(&text) {
                    Ok(m) => m,
                    Err(e) => {
                        fail(sock, "bad_request", &e, close::FRAME).await;
                        return "left";
                    }
                };
                let result = match msg {
                    ClientMsg::Ping => {
                        if !send_json(sock, &json!({ "t": "pong" })).await {
                            return "left";
                        }
                        Ok(())
                    }
                    ClientMsg::Signal { to, kind, data } => app.signal(session, member, to, &kind, &data),
                    ClientMsg::Kick { member: target, ban } => app.kick(session, member, target, ban),
                    ClientMsg::Leave => {
                        send_close(sock, close::NORMAL).await;
                        return "left";
                    }
                    ClientMsg::Create { .. } | ClientMsg::Join { .. } | ClientMsg::Attach { .. } => {
                        Err(Refusal::new("bad_request", "This socket is already in a session."))
                    }
                };
                if let Err(r) = result {
                    if !send_json(sock, &error_json(&r, false)).await {
                        return "left";
                    }
                }
            }
        }
    }
}
