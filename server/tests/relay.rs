//! Builtin relay (MPRL v1) with real UDP sockets: one host and two clients. Member ports come from
//! 47000-47039 (one sub-range per test).

mod common;

use std::net::SocketAddr;
use std::time::Duration;

use common::*;
use serde_json::{json, Value};
use tokio::net::UdpSocket;

fn token(v: &Value) -> [u8; 16] {
    let s = v["relay"]["token"].as_str().unwrap();
    assert_eq!(s.len(), 32);
    core::array::from_fn(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap())
}

fn reg(tok: &[u8; 16]) -> Vec<u8> {
    let mut v = b"MPRL\x01\x01\x00\x00".to_vec();
    v.extend_from_slice(tok);
    v
}

async fn udp() -> UdpSocket {
    UdpSocket::bind("127.0.0.1:0").await.unwrap()
}

/// Next datagram within `ms`, or None.
async fn recv(s: &UdpSocket, ms: u64) -> Option<(Vec<u8>, SocketAddr)> {
    let mut buf = vec![0u8; 4096];
    match tokio::time::timeout(Duration::from_millis(ms), s.recv_from(&mut buf)).await {
        Ok(Ok((n, from))) => Some((buf[..n].to_vec(), from)),
        _ => None,
    }
}

/// REG and parse the OK: (role, relay port, seen address).
async fn register(s: &UdpSocket, control: SocketAddr, tok: &[u8; 16]) -> (u8, u16, SocketAddr) {
    s.send_to(&reg(tok), control).await.unwrap();
    let (d, from) = recv(s, 2000).await.expect("an answer to REG");
    assert_eq!(from, control, "OK comes from the control port");
    assert_eq!(d.len(), 18, "OK is 18 bytes: {d:?}");
    assert_eq!(&d[..8], b"MPRL\x01\x02\x00\x00");
    assert_eq!(d[11], 4);
    let seen = SocketAddr::from(([d[12], d[13], d[14], d[15]], u16::from_be_bytes([d[16], d[17]])));
    (d[8], u16::from_be_bytes([d[9], d[10]]), seen)
}

async fn session_with_two_clients(srv: &Server) -> (Ws, Value, Ws, Value, Ws, Value) {
    let url = srv.url();
    let (mut host, c) = create(&url, "H").await;
    let code = c["code"].as_str().unwrap().to_string();
    let (a, ja) = join(&url, &code, "A", false).await;
    let (b, jb) = join(&url, &code, "B", true).await;
    host.recv_t("member_joined").await;
    host.recv_t("member_joined").await;
    (host, c, a, ja, b, jb)
}

#[tokio::test]
async fn relay_reg_forward_drop_and_logs() {
    let mut srv = Server::start(&["--relay", "--relay-port", "0", "--relay-ports", "47000-47009"]);
    let rp = srv.relay.expect("relay port in the ready line");
    assert!(
        srv.ready_line.ends_with(&format!("relay=127.0.0.1:{rp} ports=47000-47009 mode=builtin")),
        "{}",
        srv.ready_line
    );
    let control: SocketAddr = format!("127.0.0.1:{rp}").parse().unwrap();
    let (_host_ws, c, _a_ws, ja, b_ws, jb) = session_with_two_clients(&srv).await;
    assert_eq!(c["relay"]["addr"], format!("127.0.0.1:{rp}"));
    assert_eq!(ja["relay"]["addr"], format!("127.0.0.1:{rp}"));
    assert_eq!(c["ice"], json!([]));
    let (th, ta, tb) = (token(&c), token(&ja), token(&jb));
    assert!(th != ta && ta != tb);

    let (h, a, b, x) = (udp().await, udp().await, udp().await, udp().await);
    let (role, port, seen) = register(&h, control, &th).await;
    assert_eq!((role, port, seen), (0, 0, h.local_addr().unwrap()));
    let (role, ra, seen) = register(&a, control, &ta).await;
    assert_eq!((role, seen), (1, a.local_addr().unwrap()));
    assert!((47000..=47009).contains(&ra));
    let (_, rb, _) = register(&b, control, &tb).await;
    assert!((47000..=47009).contains(&rb) && rb != ra);
    // A repeated REG keeps the port.
    assert_eq!(register(&a, control, &ta).await.1, ra);
    let ra_addr: SocketAddr = format!("127.0.0.1:{ra}").parse().unwrap();
    let rb_addr: SocketAddr = format!("127.0.0.1:{rb}").parse().unwrap();

    // Client -> host and back, both through R.
    a.send_to(b"hello from A", ra_addr).await.unwrap();
    assert_eq!(recv(&h, 2000).await, Some((b"hello from A".to_vec(), ra_addr)));
    h.send_to(b"to A", ra_addr).await.unwrap();
    assert_eq!(recv(&a, 2000).await, Some((b"to A".to_vec(), ra_addr)));
    h.send_to(b"to B", rb_addr).await.unwrap();
    assert_eq!(recv(&b, 2000).await, Some((b"to B".to_vec(), rb_addr)));
    b.send_to(b"from B", rb_addr).await.unwrap();
    assert_eq!(recv(&h, 2000).await, Some((b"from B".to_vec(), rb_addr)));

    // Foreign datagrams and MPRL datagrams on R are dropped.
    x.send_to(b"intruder", ra_addr).await.unwrap();
    b.send_to(b"wrong port", ra_addr).await.unwrap();
    a.send_to(b"MPRL\x01\x04\x00\x00\x01\x02\x03\x04\x05\x06\x07\x08", ra_addr).await.unwrap();
    assert_eq!(recv(&h, 300).await, None);
    assert_eq!(recv(&a, 100).await, None);

    // Unknown token: ERR 1. Garbage: no answer.
    x.send_to(&reg(&[0xEE; 16]), control).await.unwrap();
    assert_eq!(recv(&x, 2000).await.unwrap().0, b"MPRL\x01\x03\x00\x00\x01".to_vec());
    x.send_to(b"MPRL\x01\x01\x00\x00short", control).await.unwrap();
    assert_eq!(recv(&x, 300).await, None);

    // B leaves the lobby: its mapping is freed (logged), its token answers ERR 2.
    b_ws.close().await;
    let line = srv.wait_log(&format!("member=2 port={rb} up_bytes"), Duration::from_secs(3));
    assert!(line.contains("up_bytes=6 down_bytes=4 closed=session"), "{line}");
    assert!(
        line.starts_with(&format!("mppm-server: relay mapping session={} member=2", c["session"].as_str().unwrap()))
    );
    b.send_to(&reg(&tb), control).await.unwrap();
    assert_eq!(recv(&b, 2000).await.unwrap().0, b"MPRL\x01\x03\x00\x00\x02".to_vec());

    // Shutdown frees the rest with closed=shutdown.
    assert_eq!(srv.terminate(), 0);
    let line = srv.wait_log(&format!("member=1 port={ra} up_bytes"), Duration::from_secs(1));
    assert!(line.ends_with("up_bytes=12 down_bytes=4 closed=shutdown"), "{line}");
}

#[tokio::test]
async fn relay_rate_cap() {
    let srv = Server::start(&["--relay", "--relay-port", "0", "--relay-ports", "47010-47019", "--relay-kbps", "64"]);
    let control: SocketAddr = format!("127.0.0.1:{}", srv.relay.unwrap()).parse().unwrap();
    let (_hw, c, _aw, ja, _bw, _jb) = session_with_two_clients(&srv).await;
    let (h, a) = (udp().await, udp().await);
    register(&h, control, &token(&c)).await;
    let (_, ra, _) = register(&a, control, &token(&ja)).await;
    let ra_addr: SocketAddr = format!("127.0.0.1:{ra}").parse().unwrap();
    // 64 kbit/s = 8000 bytes/s with a one-second burst: 100 x 1000 bytes sent at once, about 8 pass.
    let payload = vec![7u8; 1000];
    for _ in 0..100 {
        a.send_to(&payload, ra_addr).await.unwrap();
    }
    let mut got = 0;
    while recv(&h, 300).await.is_some() {
        got += 1;
    }
    assert!((6..=12).contains(&got), "{got} datagrams passed the 64 kbit/s cap");
    // The other direction has its own bucket.
    for _ in 0..3 {
        h.send_to(&payload, ra_addr).await.unwrap();
    }
    let mut back = 0;
    while recv(&a, 300).await.is_some() {
        back += 1;
    }
    assert_eq!(back, 3);
}

#[tokio::test]
async fn relay_idle_expiry_and_reg_limit() {
    let srv =
        Server::start(&["--relay", "--relay-port", "0", "--relay-ports", "47020-47029", "--relay-idle-secs", "1"]);
    let control: SocketAddr = format!("127.0.0.1:{}", srv.relay.unwrap()).parse().unwrap();
    let (_hw, c, _aw, ja, _bw, _jb) = session_with_two_clients(&srv).await;
    let (h, a) = (udp().await, udp().await);
    register(&h, control, &token(&c)).await;
    let (_, ra, _) = register(&a, control, &token(&ja)).await;
    let ra_addr: SocketAddr = format!("127.0.0.1:{ra}").parse().unwrap();
    a.send_to(b"ping", ra_addr).await.unwrap();
    assert!(recv(&h, 1000).await.is_some());

    let line = srv.wait_log(&format!("member=1 port={ra} up_bytes"), Duration::from_secs(4));
    assert!(line.ends_with("up_bytes=4 down_bytes=0 closed=idle"), "{line}");
    a.send_to(b"late", ra_addr).await.unwrap();
    assert_eq!(recv(&h, 300).await, None, "a freed port forwards nothing");
    // A new REG maps the member again.
    let (_, ra2, _) = register(&a, control, &token(&ja)).await;
    assert!((47020..=47029).contains(&ra2));

    // 16 REG per minute per IP: 3 were used above, so 13 more are answered and the 17th is not.
    for i in 0..13 {
        h.send_to(&reg(&token(&c)), control).await.unwrap();
        assert!(recv(&h, 1000).await.is_some(), "REG {i} unanswered");
    }
    h.send_to(&reg(&token(&c)), control).await.unwrap();
    assert_eq!(recv(&h, 500).await, None, "the 17th REG within a minute is dropped");
}

#[tokio::test]
async fn relay_off_without_builtin_mode() {
    let srv = Server::start(&["--relay-mode", "none"]);
    assert!(srv.relay.is_none());
    let (_h, c) = create(&srv.url(), "H").await;
    assert_eq!(c["relay"], json!(null));
}
