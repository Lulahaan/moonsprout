//! REST endpoints, attach, rate limits and ICE lists against the real binary.

mod common;

use common::*;
use serde_json::json;

#[tokio::test]
async fn rest_create_join_attach() {
    let srv = Server::start(&[]);
    let (st, h) = http(srv.http, "GET", "/v1/health", None);
    assert_eq!(st, 200);
    assert_eq!(h, json!({"ok":true,"version":"0.1.0","sessions":0,"members":0}));

    let (st, c) = http(srv.http, "POST", "/v1/sessions", Some(r#"{"name":"Olimar","password":"","maxMembers":4}"#));
    assert_eq!(st, 201, "{c}");
    assert_eq!(c["member"], 0);
    assert!(is_hex(&c["token"], 32));
    assert!(c.get("secret").is_none(), "the secret comes with the WebSocket attach");
    assert_eq!(c["relay"], json!(null));
    assert_eq!(c["ice"], json!([]));
    let code = c["code"].as_str().unwrap().to_string();

    // A REST join before the host attached.
    let (st, j) =
        http(srv.http, "POST", &format!("/v1/sessions/{}/join", code.to_lowercase()), Some(r#"{"name":"Louie"}"#));
    assert_eq!(st, 200, "{j}");
    assert_eq!(j["member"], 1);
    assert_eq!(j["host"], 0);
    assert!(is_hex(&j["token"], 32));
    // The host is listed although it has not attached yet (the game requires it in the list).
    assert_eq!(
        j["members"],
        json!([{"member":0,"name":"Olimar","spectator":false},{"member":1,"name":"Louie","spectator":false}])
    );

    // The member attaches first, then the host: the host learns the member (with auth) after `created`.
    let mut m = Ws::connect(&srv.url()).await;
    m.send(json!({"t":"attach","v":1,"token":j["token"]})).await;
    let joined = m.recv().await;
    assert_eq!(joined["t"], "joined");
    assert_eq!(joined["member"], 1);
    assert!(is_hex(&joined["secret"], 16));

    let mut host = Ws::connect(&srv.url()).await;
    host.send(json!({"t":"attach","v":1,"token":c["token"]})).await;
    let created = host.recv().await;
    assert_eq!(created["t"], "created");
    assert_eq!(created["code"], code.as_str());
    assert_eq!(created["session"], c["session"]);
    let mj = host.recv().await;
    assert_eq!(mj["t"], "member_joined");
    assert_eq!(mj["auth"], joined["secret"]);

    // A token is redeemed once.
    let mut again = Ws::connect(&srv.url()).await;
    again.send(json!({"t":"attach","v":1,"token":c["token"]})).await;
    assert_eq!(again.recv().await["code"], "not_found");
    let mut bad = Ws::connect(&srv.url()).await;
    bad.send(json!({"t":"attach","v":1,"token":"xyz"})).await;
    assert_eq!(bad.recv().await["code"], "bad_request");

    let (_, h) = http(srv.http, "GET", "/v1/health", None);
    assert_eq!((h["sessions"].as_u64(), h["members"].as_u64()), (Some(1), Some(2)));

    // A WebSocket member joining now: the REST member sees member_joined without auth.
    let (_ws, j2) = join(&srv.url(), &code, "Third", false).await;
    assert_eq!(j2["member"], 2);
    let mj = m.recv().await;
    assert_eq!(mj["member"], 2);
    assert!(mj.get("auth").is_none());
    assert!(host.recv().await.get("auth").is_some());
}

#[tokio::test]
async fn rest_errors() {
    let srv = Server::start(&[]);
    let p = srv.http;
    let (st, c) = http(p, "POST", "/v1/sessions", Some(r#"{"name":"H","password":"pw","maxMembers":2}"#));
    assert_eq!(st, 201);
    let code = c["code"].as_str().unwrap().to_string();
    let join_path = format!("/v1/sessions/{code}/join");

    assert_eq!(
        http(p, "POST", "/v1/sessions", Some("nope")),
        (400, json!({"error":"bad_request","msg":"The body must be a JSON object of at most 4 KiB."}))
    );
    let (st, e) = http(p, "POST", "/v1/sessions", Some(&format!(r#"{{"name":"{}"}}"#, "a".repeat(5000))));
    assert_eq!((st, e["error"].as_str()), (400, Some("bad_request")));
    let (st, e) = http(p, "POST", "/v1/sessions/ZZZZ-ZZZZ/join", Some("{}"));
    assert_eq!((st, e["error"].as_str()), (404, Some("not_found")));
    let (st, e) = http(p, "POST", &join_path, Some(r#"{"password":"no"}"#));
    assert_eq!((st, e["error"].as_str()), (403, Some("bad_password")));
    let (st, _) = http(p, "POST", &join_path, Some(r#"{"password":"pw"}"#));
    assert_eq!(st, 200);
    let (st, e) = http(p, "POST", &join_path, Some(r#"{"password":"pw"}"#));
    assert_eq!((st, e["error"].as_str()), (409, Some("full")));
    let (st, e) = http(p, "GET", "/v2/whatever", None);
    assert_eq!((st, e["error"].as_str()), (404, Some("not_found")));
}

#[tokio::test]
async fn join_flood_gets_429() {
    let srv = Server::start(&[]);
    let p = srv.http;
    let (_, c) = http(p, "POST", "/v1/sessions", Some(r#"{"name":"H"}"#));
    let join_path = format!("/v1/sessions/{}/join", c["code"].as_str().unwrap());
    // Valid joins: 10 burst, then 429.
    let statuses: Vec<u16> = (0..12).map(|_| http(p, "POST", &join_path, Some("{}")).0).collect();
    assert!(statuses[..7].iter().all(|&s| s == 200), "{statuses:?}"); // room for 7 members besides the host
    assert!(statuses[7..10].iter().all(|&s| s == 409), "{statuses:?}");
    assert_eq!(&statuses[10..], &[429, 429], "{statuses:?}");
}

#[tokio::test]
async fn failed_joins_lock_out() {
    let srv = Server::start(&[]);
    let p = srv.http;
    let (_, c) = http(p, "POST", "/v1/sessions", Some(r#"{"name":"H"}"#));
    let good = format!("/v1/sessions/{}/join", c["code"].as_str().unwrap());
    let statuses: Vec<u16> = (0..7).map(|_| http(p, "POST", "/v1/sessions/0000-0000/join", Some("{}")).0).collect();
    assert_eq!(statuses, vec![404, 404, 404, 404, 404, 429, 429]);
    // Locked out: even a valid code is refused now.
    assert_eq!(http(p, "POST", &good, Some("{}")).0, 429);
    // And over the WebSocket.
    let (_ws, v) = join(&srv.url(), c["code"].as_str().unwrap(), "x", false).await;
    assert_eq!(v["code"], "rate_limited");
}

#[tokio::test]
async fn limits_scale_and_hosted_cap() {
    let srv = Server::start(&[]);
    // 3 creates in a burst, then 429.
    let statuses: Vec<u16> = (0..4).map(|_| http(srv.http, "POST", "/v1/sessions", Some("{}")).0).collect();
    assert_eq!(statuses, vec![201, 201, 201, 429]);

    let srv = Server::start(&["--limits-scale", "20"]);
    let statuses: Vec<u16> = (0..61).map(|_| http(srv.http, "POST", "/v1/sessions", Some("{}")).0).collect();
    // A burst of 60 creates (3 x 20), then 429 (the hosted-sessions cap of 4 x 20 is a unit test).
    assert!(statuses[..60].iter().all(|&s| s == 201), "{statuses:?}");
    assert_eq!(statuses[60], 429);

    let srv = Server::start(&["--max-sessions", "2", "--limits-scale", "20"]);
    let statuses: Vec<u16> = (0..3).map(|_| http(srv.http, "POST", "/v1/sessions", Some("{}")).0).collect();
    assert_eq!(statuses, vec![201, 201, 429]);
}

#[tokio::test]
async fn ice_static_and_shared_secret() {
    let srv = Server::start(&[
        "--relay-mode",
        "static",
        "--turn",
        "turn:127.0.0.1:3479",
        "--turn-user",
        "u",
        "--turn-pass",
        "p",
        "--stun",
        "stun:127.0.0.1:3478",
    ]);
    assert!(srv.ready_line.ends_with("relay=- ports=- mode=static"), "{}", srv.ready_line);
    let (_h, c) = create(&srv.url(), "H").await;
    let expect = json!([
        {"urls":["stun:127.0.0.1:3478"]},
        {"urls":["turn:127.0.0.1:3479"],"username":"u","credential":"p"}
    ]);
    assert_eq!(c["ice"], expect);
    assert_eq!(c["relay"], json!(null));
    let (_m, j) = join(&srv.url(), c["code"].as_str().unwrap(), "M", false).await;
    assert_eq!(j["ice"], expect);

    let srv = Server::start(&[
        "--relay-mode",
        "shared-secret",
        "--turn",
        "turn:127.0.0.1:3478",
        "--turn-secret",
        "north",
        "--turn-ttl",
        "600",
    ]);
    assert!(srv.ready_line.ends_with("mode=shared-secret"));
    let (_h, c) = create(&srv.url(), "H").await;
    let (_m, j) = join(&srv.url(), c["code"].as_str().unwrap(), "M", false).await;
    for (v, member) in [(&c, 0u32), (&j, 1)] {
        let ice = v["ice"].as_array().unwrap();
        assert_eq!(ice.len(), 1);
        assert_eq!(ice[0]["urls"], json!(["turn:127.0.0.1:3478"]));
        let user = ice[0]["username"].as_str().unwrap();
        let (exp, id) = user.split_once(':').unwrap();
        assert_eq!(id, member.to_string());
        let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_secs();
        let exp: u64 = exp.parse().unwrap();
        assert!(exp >= now + 590 && exp <= now + 610, "expiry {exp} now {now}");
        let cred = ice[0]["credential"].as_str().unwrap();
        assert_eq!(cred.len(), 28, "base64 of a 20-byte HMAC-SHA1");
        assert!(!v.to_string().contains("north"), "the secret never reaches a client");
    }

    // MPPM_TURN_SECRET works in place of --turn-secret.
    let mut child = std::process::Command::new(env!("CARGO_BIN_EXE_mppm-server"))
        .args(["--relay-mode", "shared-secret", "--turn", "turn:x:1", "--bind", "127.0.0.1:0"])
        .env("MPPM_TURN_SECRET", "north")
        .stderr(std::process::Stdio::piped())
        .spawn()
        .unwrap();
    let mut line = String::new();
    std::io::BufRead::read_line(&mut std::io::BufReader::new(child.stderr.take().unwrap()), &mut line).unwrap();
    let _ = child.kill();
    let _ = child.wait();
    assert!(line.contains("mode=shared-secret"), "{line}");
}
