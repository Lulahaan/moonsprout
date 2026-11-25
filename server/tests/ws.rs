//! WebSocket protocol flows against the real binary.

mod common;

use std::time::Duration;

use common::*;
use serde_json::json;
use tokio_tungstenite::tungstenite::Message;

#[tokio::test]
async fn create_join_auth_signal_leave() {
    let srv = Server::start(&[]);
    assert!(srv.ready_line.ends_with("relay=- ports=- mode=none"), "{}", srv.ready_line);
    let url = srv.url();

    let (mut host, created) = create(&url, "  Olimar\u{7} ").await;
    let code = created["code"].as_str().unwrap().to_string();
    assert_eq!(code.len(), 9);
    assert_eq!(&code[4..5], "-");
    assert_eq!(created["member"], 0);
    assert!(created["session"].as_str().unwrap().starts_with("s_"));
    assert_eq!(created["session"].as_str().unwrap().len(), 18);
    assert!(is_hex(&created["secret"], 16));
    assert_eq!(created["ice"], json!([]));
    assert_eq!(created["relay"], json!(null));
    assert!(created["expires"].as_u64().unwrap() > 1_700_000_000);

    // Join with a lower-case, hyphen-less code.
    let typed = code.replace('-', "").to_lowercase();
    let (mut a, joined) = join(&url, &typed, "Louie", false).await;
    assert_eq!(joined["t"], "joined", "{joined}");
    assert_eq!(joined["member"], 1);
    assert_eq!(joined["host"], 0);
    assert_eq!(joined["code"], code.as_str());
    assert!(is_hex(&joined["secret"], 16));
    assert_eq!(
        joined["members"],
        json!([{"member":0,"name":"Olimar","spectator":false},{"member":1,"name":"Louie","spectator":false}])
    );
    let mj = host.recv().await;
    assert_eq!(mj["t"], "member_joined");
    assert_eq!(mj["member"], 1);
    assert_eq!(mj["name"], "Louie");
    assert_eq!(mj["auth"], joined["secret"], "the host gets the member's secret as auth");

    let (mut b, joined_b) = join(&url, &code, "", true).await;
    assert_eq!(joined_b["member"], 2);
    assert_eq!(joined_b["members"][2], json!({"member":2,"name":"Player","spectator":true}));
    assert!(joined_b["members"].as_array().unwrap().iter().all(|m| m.get("auth").is_none()));
    let mj_host = host.recv().await;
    assert_eq!(mj_host["auth"], joined_b["secret"]);
    let mj_a = a.recv().await;
    assert_eq!(mj_a["t"], "member_joined");
    assert_eq!(mj_a["member"], 2);
    assert!(mj_a.get("auth").is_none(), "auth goes to the host only: {mj_a}");

    // Signals: host -> member, member -> host (4096 bytes), member -> member refused.
    host.send(json!({"t":"signal","to":1,"kind":"desc","data":"hello"})).await;
    assert_eq!(a.recv().await, json!({"t":"signal","from":0,"kind":"desc","data":"hello"}));
    let big = "x".repeat(4096);
    a.send(json!({"t":"signal","to":0,"kind":"cand","data":big})).await;
    let got = host.recv().await;
    assert_eq!(got["from"], 1);
    assert_eq!(got["data"].as_str().unwrap().len(), 4096);
    a.send(json!({"t":"signal","to":2,"kind":"addr","data":"sneaky"})).await;
    let err = a.recv().await;
    assert_eq!(err["t"], "error");
    assert_eq!(err["code"], "forbidden");
    assert_eq!(err["fatal"], false);
    b.silent(300).await;
    // Too large, unknown target: non-fatal.
    let too_big = "y".repeat(4097);
    host.send(json!({"t":"signal","to":1,"kind":"desc","data":too_big})).await;
    assert_eq!(host.recv().await["code"], "too_large");
    host.send(json!({"t":"signal","to":9,"kind":"desc","data":"?"})).await;
    assert_eq!(host.recv().await["code"], "not_found");
    // Members cannot kick.
    a.send(json!({"t":"kick","member":2})).await;
    assert_eq!(a.recv().await["code"], "forbidden");
    // Ping.
    b.send(json!({"t":"ping"})).await;
    assert_eq!(b.recv().await, json!({"t":"pong"}));

    // Member leaves: others get member_left.
    b.send(json!({"t":"leave"})).await;
    let (_, c) = b.until_closed().await;
    assert_eq!(c, Some(1000));
    assert_eq!(host.recv().await, json!({"t":"member_left","member":2,"reason":"left"}));
    assert_eq!(a.recv().await, json!({"t":"member_left","member":2,"reason":"left"}));

    // Socket drop of a member: member_left too.
    let (c3, j3) = join(&url, &code, "Third", false).await;
    assert_eq!(j3["member"], 3, "ids are never reused");
    host.recv_t("member_joined").await;
    a.recv_t("member_joined").await;
    drop(c3);
    assert_eq!(host.recv().await, json!({"t":"member_left","member":3,"reason":"left"}));
    a.recv_t("member_left").await;

    // Host leaves: members get closed host_left.
    host.send(json!({"t":"leave"})).await;
    let (msgs, _) = a.until_closed().await;
    assert_eq!(msgs, vec![json!({"t":"closed","reason":"host_left"})]);
    // The code is dead now.
    let (mut late, v) = join(&url, &code, "Late", false).await;
    assert_eq!(v["t"], "error");
    assert_eq!(v["code"], "not_found");
    assert_eq!(late.until_closed().await.1, Some(4003));
}

#[tokio::test]
async fn host_socket_close_ends_session() {
    let srv = Server::start(&[]);
    let url = srv.url();
    let (host, created) = create(&url, "H").await;
    let code = created["code"].as_str().unwrap();
    let (mut a, _) = join(&url, code, "A", false).await;
    drop(host);
    let (msgs, _) = a.until_closed().await;
    assert_eq!(msgs, vec![json!({"t":"closed","reason":"host_left"})]);
}

#[tokio::test]
async fn kick_and_ban() {
    let srv = Server::start(&[]);
    let url = srv.url();
    let (mut host, created) = create(&url, "H").await;
    let code = created["code"].as_str().unwrap().to_string();
    let (mut a, _) = join(&url, &code, "A", false).await;
    let (mut b, _) = join(&url, &code, "B", false).await;
    host.recv_t("member_joined").await;
    host.recv_t("member_joined").await;
    a.recv_t("member_joined").await;

    host.send(json!({"t":"kick","member":2,"ban":false})).await;
    let (msgs, code_b) = b.until_closed().await;
    assert_eq!(msgs, vec![json!({"t":"closed","reason":"kicked"})]);
    assert_eq!(code_b, Some(1000));
    assert_eq!(a.recv().await, json!({"t":"member_left","member":2,"reason":"kicked"}));
    assert_eq!(host.recv().await, json!({"t":"member_left","member":2,"reason":"kicked"}));
    // Kicked (not banned): may join again.
    let (mut b2, j) = join(&url, &code, "B", false).await;
    assert_eq!(j["t"], "joined");
    host.recv_t("member_joined").await;
    a.recv_t("member_joined").await;

    // Ban: closed banned, then the IP (127.0.0.1, everyone here) is refused for this session.
    host.send(json!({"t":"kick","member":j["member"],"ban":true})).await;
    let (msgs, _) = b2.until_closed().await;
    assert_eq!(msgs, vec![json!({"t":"closed","reason":"banned"})]);
    assert_eq!(a.recv().await["reason"], "banned");
    let (mut b3, v) = join(&url, &code, "B", false).await;
    assert_eq!(v["t"], "error");
    assert_eq!(v["code"], "forbidden");
    assert_eq!(v["fatal"], true);
    assert_eq!(b3.until_closed().await.1, Some(4003));
    // Kick of a missing member / the host itself: non-fatal errors.
    assert_eq!(host.recv().await["reason"], "banned");
    host.send(json!({"t":"kick","member":42})).await;
    assert_eq!(host.recv().await["code"], "not_found");
    host.send(json!({"t":"kick","member":0})).await;
    assert_eq!(host.recv().await["code"], "bad_request");
}

#[tokio::test]
async fn password_and_full_room() {
    let srv = Server::start(&[]);
    let url = srv.url();
    let mut host = Ws::connect(&url).await;
    host.send(json!({"t":"create","v":1,"name":"H","password":"pikmin","maxMembers":2})).await;
    let created = host.recv().await;
    let code = created["code"].as_str().unwrap().to_string();

    let mut a = Ws::connect(&url).await;
    a.send(json!({"t":"join","v":1,"code":code,"name":"A","password":"wrong"})).await;
    let v = a.recv().await;
    assert_eq!(
        (v["t"].as_str(), v["code"].as_str(), v["fatal"].as_bool()),
        (Some("error"), Some("bad_password"), Some(true))
    );
    assert_eq!(a.until_closed().await.1, Some(4003));

    let mut a = Ws::connect(&url).await;
    a.send(json!({"t":"join","v":1,"code":code,"name":"A","password":"pikmin"})).await;
    assert_eq!(a.recv().await["t"], "joined");

    let mut b = Ws::connect(&url).await;
    b.send(json!({"t":"join","v":1,"code":code,"name":"B","password":"pikmin"})).await;
    assert_eq!(b.recv().await["code"], "full");

    // maxMembers below 2 is refused; above --max-members is clamped.
    let mut c = Ws::connect(&url).await;
    c.send(json!({"t":"create","v":1,"name":"C","maxMembers":1})).await;
    assert_eq!(c.recv().await["code"], "bad_request");
}

#[tokio::test]
async fn first_message_rules() {
    let srv = Server::start(&[]);
    let url = srv.url();

    // Wrong first message.
    let mut ws = Ws::connect(&url).await;
    ws.send(json!({"t":"signal","to":0,"kind":"desc","data":"x"})).await;
    let (msgs, code) = ws.until_closed().await;
    assert_eq!(msgs[0]["code"], "bad_request");
    assert_eq!(code, Some(4000));

    // Garbage.
    let mut ws = Ws::connect(&url).await;
    ws.send_text("not json".into()).await;
    assert_eq!(ws.until_closed().await.1, Some(4000));

    // Bad version (and missing version).
    for msg in [json!({"t":"create","v":2,"name":"x"}), json!({"t":"join","code":"ABCD-EFGH"})] {
        let mut ws = Ws::connect(&url).await;
        ws.send(msg).await;
        let (msgs, code) = ws.until_closed().await;
        assert_eq!(msgs[0]["code"], "version");
        assert_eq!(msgs[0]["fatal"], true);
        assert_eq!(code, Some(4001));
    }

    // Oversize frame (20 KiB) and binary frame.
    let mut ws = Ws::connect(&url).await;
    ws.send_text(format!("{{\"t\":\"create\",\"v\":1,\"name\":\"{}\"}}", "z".repeat(20 * 1024))).await;
    let (msgs, code) = ws.until_closed().await;
    assert_eq!(msgs[0]["code"], "too_large");
    assert_eq!(code, Some(4002));
    let mut ws = Ws::connect(&url).await;
    ws.send_msg(Message::Binary(vec![1, 2, 3].into())).await;
    assert_eq!(ws.until_closed().await.1, Some(4002));

    // Oversize after joining: closed too.
    let (mut host, _) = create(&url, "H").await;
    host.send_text("x".repeat(17 * 1024)).await;
    let (msgs, code) = host.until_closed().await;
    assert_eq!(msgs[0]["code"], "too_large");
    assert_eq!(code, Some(4002));

    // Bad code: not_found.
    let (mut ws, v) = join(&url, "ABCD-EFGU", "x", false).await;
    assert_eq!(v["code"], "not_found");
    ws.until_closed().await;
}

#[tokio::test]
async fn first_message_timeout() {
    let srv = Server::start(&[]);
    let mut ws = Ws::connect(&srv.url()).await;
    let start = std::time::Instant::now();
    match ws.next(Duration::from_secs(13)).await {
        Some(Got::Json(v)) => {
            assert_eq!(v["code"], "bad_request");
            assert_eq!(ws.until_closed().await.1, Some(4000));
        }
        other => panic!("expected an error then close 4000, got {other:?}"),
    }
    let waited = start.elapsed();
    assert!(waited >= Duration::from_millis(9500) && waited < Duration::from_secs(12), "{waited:?}");
}

#[tokio::test]
async fn session_ttl_expiry() {
    let srv = Server::start(&["--session-ttl", "2"]);
    let url = srv.url();
    let (mut host, created) = create(&url, "H").await;
    let (mut a, _) = join(&url, created["code"].as_str().unwrap(), "A", false).await;
    host.recv_t("member_joined").await;
    let start = std::time::Instant::now();
    let (msgs, _) = a.until_closed().await;
    assert_eq!(msgs, vec![json!({"t":"closed","reason":"expired"})]);
    let (msgs, _) = host.until_closed().await;
    assert_eq!(msgs, vec![json!({"t":"closed","reason":"expired"})]);
    assert!(start.elapsed() < Duration::from_secs(4));
    let (status, health) = http(srv.http, "GET", "/v1/health", None);
    assert_eq!(status, 200);
    assert_eq!(health["sessions"], 0);
}

#[tokio::test]
async fn message_rate_limit() {
    let srv = Server::start(&[]);
    let (mut host, _) = create(&srv.url(), "H").await;
    for _ in 0..120 {
        host.send(json!({"t":"ping"})).await;
    }
    let mut pongs = 0;
    let mut limited = 0;
    while let Some(Got::Json(v)) = host.next(Duration::from_millis(500)).await {
        match v["t"].as_str() {
            Some("pong") => pongs += 1,
            Some("error") => {
                assert_eq!(v["code"], "rate_limited");
                assert_eq!(v["fatal"], false);
                limited += 1;
            }
            _ => {}
        }
    }
    assert!((80..=90).contains(&pongs), "pongs {pongs}");
    assert_eq!(limited, 1, "one rate_limited notice per second");
}

#[tokio::test]
async fn shutdown_closes_everyone() {
    let mut srv = Server::start(&[]);
    let url = srv.url();
    let (mut host, created) = create(&url, "H").await;
    let (mut a, _) = join(&url, created["code"].as_str().unwrap(), "A", false).await;
    host.recv_t("member_joined").await;
    let code = srv.terminate();
    assert_eq!(code, 0);
    let (msgs, _) = a.until_closed().await;
    assert_eq!(msgs, vec![json!({"t":"closed","reason":"server_shutdown"})]);
    let (msgs, _) = host.until_closed().await;
    assert_eq!(msgs, vec![json!({"t":"closed","reason":"server_shutdown"})]);
}

#[tokio::test]
async fn bad_flags() {
    let (code, err) = run_to_exit(&["--relay-mode", "cloudflare"]);
    assert_eq!(code, 2);
    assert!(err.contains("relay mode cloudflare is not available yet (deferred TURN spike)"), "{err}");
    let (code, _) = run_to_exit(&["--relay-mode", "static", "--turn", "turn:x:1"]);
    assert_eq!(code, 2);
    let (code, _) = run_to_exit(&["--relay-mode", "shared-secret", "--turn", "turn:x:1"]);
    assert_eq!(code, 2);
    let (code, _) = run_to_exit(&["--max-members", "9"]);
    assert_eq!(code, 2);
    let (code, _) = run_to_exit(&["--relay-ports", "9-3"]);
    assert_eq!(code, 2);
}
