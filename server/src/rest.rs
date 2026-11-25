//! HTTP routes: the REST endpoints, health and the WebSocket upgrade (PROTOCOL.md, "REST").

use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Instant;

use axum::body::Bytes;
use axum::extract::rejection::BytesRejection;
use axum::extract::ws::WebSocketUpgrade;
use axum::extract::{ConnectInfo, DefaultBodyLimit, Path, State};
use axum::http::{HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post};
use axum::{Json, Router};
use serde_json::{json, Value};

use crate::proto;
use crate::ratelimit::What;
use crate::session::{App, Refusal, Via};

pub const MAX_BODY: usize = 4096;

pub fn router(app: Arc<App>) -> Router {
    Router::new()
        .route("/v1/health", get(health))
        .route("/v1/sessions", post(create))
        .route("/v1/sessions/{code}/join", post(join))
        .route("/v1/ws", get(ws_upgrade))
        .fallback(|| async { refusal(&Refusal::new("not_found", "No such endpoint.")) })
        .layer(DefaultBodyLimit::max(MAX_BODY))
        .with_state(app)
}

fn refusal(r: &Refusal) -> Response {
    let status = StatusCode::from_u16(r.http_status()).unwrap_or(StatusCode::BAD_REQUEST);
    (status, Json(json!({ "error": r.code, "msg": r.msg }))).into_response()
}

fn bad(msg: &str) -> Response {
    refusal(&Refusal::new("bad_request", msg))
}

/// The JSON object body of a REST call.
const BAD_BODY: &str = "The body must be a JSON object of at most 4 KiB.";

fn body_object(body: Result<Bytes, BytesRejection>) -> Result<serde_json::Map<String, Value>, Refusal> {
    let body = body.map_err(|_| Refusal::new("bad_request", BAD_BODY))?;
    if body.len() > MAX_BODY {
        return Err(Refusal::new("bad_request", BAD_BODY));
    }
    match serde_json::from_slice::<Value>(&body) {
        Ok(Value::Object(o)) => Ok(o),
        _ => Err(Refusal::new("bad_request", BAD_BODY)),
    }
}

fn str_field(o: &serde_json::Map<String, Value>, key: &str, max: usize) -> Result<String, Refusal> {
    match o.get(key) {
        None | Some(Value::Null) => Ok(String::new()),
        Some(Value::String(s)) if s.len() <= max => Ok(s.clone()),
        _ => Err(Refusal::new("bad_request", format!("\"{key}\" must be a string of at most {max} bytes."))),
    }
}

async fn health(State(app): State<Arc<App>>) -> Response {
    let (sessions, members) = app.counts();
    Json(json!({ "ok": true, "version": env!("CARGO_PKG_VERSION"), "sessions": sessions, "members": members }))
        .into_response()
}

async fn create(
    State(app): State<Arc<App>>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    body: Result<Bytes, BytesRejection>,
) -> Response {
    let ip = app.client_ip(&headers, peer);
    let o = match body_object(body) {
        Ok(o) => o,
        Err(r) => return refusal(&r),
    };
    let name = match str_field(&o, "name", proto::MAX_NAME_INPUT_BYTES) {
        Ok(s) => s,
        Err(r) => return refusal(&r),
    };
    let password = match str_field(&o, "password", proto::MAX_PASSWORD_BYTES) {
        Ok(s) => s,
        Err(r) => return refusal(&r),
    };
    let max_members = match o.get("maxMembers") {
        None | Some(Value::Null) => None,
        Some(v) => match v.as_u64() {
            Some(n) => Some(n),
            None => return bad("\"maxMembers\" must be a positive integer."),
        },
    };
    match app.create(ip, &name, &password, max_members, Via::Rest) {
        Ok(a) => (StatusCode::CREATED, Json(a.reply)).into_response(),
        Err(r) => refusal(&r),
    }
}

async fn join(
    State(app): State<Arc<App>>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Path(code): Path<String>,
    body: Result<Bytes, BytesRejection>,
) -> Response {
    let ip = app.client_ip(&headers, peer);
    let o = match body_object(body) {
        Ok(o) => o,
        Err(r) => return refusal(&r),
    };
    let name = match str_field(&o, "name", proto::MAX_NAME_INPUT_BYTES) {
        Ok(s) => s,
        Err(r) => return refusal(&r),
    };
    let password = match str_field(&o, "password", proto::MAX_PASSWORD_BYTES) {
        Ok(s) => s,
        Err(r) => return refusal(&r),
    };
    let spectator = match o.get("spectator") {
        None | Some(Value::Null) => false,
        Some(Value::Bool(b)) => *b,
        Some(_) => return bad("\"spectator\" must be a boolean."),
    };
    match app.join(ip, &code, &name, &password, spectator, Via::Rest) {
        Ok(a) => (StatusCode::OK, Json(a.reply)).into_response(),
        Err(r) => refusal(&r),
    }
}

async fn ws_upgrade(
    State(app): State<Arc<App>>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    upgrade: WebSocketUpgrade,
) -> Response {
    let ip = app.client_ip(&headers, peer);
    {
        let now = Instant::now();
        let mut l = app.limits();
        if !l.allow(ip, What::Upgrade, now) {
            return refusal(&Refusal::new("rate_limited", "Too many connections from this address; wait a minute."));
        }
        if !l.ws_open(ip, now) {
            return refusal(&Refusal::new("rate_limited", "Too many open connections from this address."));
        }
    }
    // Frames up to 64 KiB reach the handler, which refuses anything over 16 KiB with close 4002.
    let failed_app = app.clone();
    upgrade
        .max_message_size(64 * 1024)
        .max_frame_size(64 * 1024)
        .on_failed_upgrade(move |_| {
            // The socket task releases the slot taken above; a failed upgrade has no task.
            failed_app.limits().ws_close(ip);
        })
        .on_upgrade(move |sock| crate::ws::serve(app, sock, ip))
}
