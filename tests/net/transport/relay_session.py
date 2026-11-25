#!/usr/bin/env python3
"""M5.3b transport tests: open a session on a real mppm-server to get builtin-relay tokens.

Python standard library only. Creates a session and joins it over REST (design 5.4), attaches both
tokens over the WebSocket (GET /v1/ws, {"t":"attach"}) so the server sees live members, prints one JSON
line on stdout, then keeps both sockets alive until stdin closes; it then sends {"t":"leave"} and
closes them (the session ends with the host's socket, which frees the relay mappings).

    relay_session.py --server http://127.0.0.1:47200
    -> {"relay": "127.0.0.1:47201", "host_token": "<32 hex>", "client_token": "<32 hex>", "code": "...", "attached": true}

Plaintext only: the server must be on loopback (design 2.11, section 10).
"""
import argparse
import base64
import json
import os
import select
import socket
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


def log(msg):
    print("relay_session: " + msg, file=sys.stderr, flush=True)


def post(base, path, body):
    req = urllib.request.Request(base + path, data=json.dumps(body).encode(), method="POST",
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read().decode())


class WebSocket:
    """A minimal RFC 6455 client: text frames, masked, no extensions."""

    def __init__(self, host, port, path):
        self.sock = socket.create_connection((host, port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
        self.sock.sendall(req.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise OSError("connection closed during the WebSocket handshake")
            head += chunk
        status, _, self.buf = head.partition(b"\r\n\r\n")
        if b" 101 " not in status.split(b"\r\n", 1)[0]:
            raise OSError("WebSocket upgrade refused: " + status.split(b"\r\n", 1)[0].decode(errors="replace"))

    def send(self, obj, opcode=1):
        payload = json.dumps(obj).encode() if opcode == 1 else obj
        head = bytes([0x80 | opcode])
        n = len(payload)
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", n)
        mask = os.urandom(4)
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise OSError("WebSocket closed")
            self.buf += chunk

    def recv(self):
        """One message as a dict, or None for a control frame (pings are answered)."""
        self._need(2)
        b0, b1 = self.buf[0], self.buf[1]
        n, off = b1 & 0x7F, 2
        if n == 126:
            self._need(4)
            n, off = struct.unpack(">H", self.buf[2:4])[0], 4
        elif n == 127:
            self._need(10)
            n, off = struct.unpack(">Q", self.buf[2:10])[0], 10
        masked = b1 & 0x80
        if masked:
            off += 4
        self._need(off + n)
        payload = self.buf[off:off + n]
        if masked:
            mask = self.buf[off - 4:off]
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.buf = self.buf[off + n:]
        opcode = b0 & 0x0F
        if opcode == 9:
            self.send(payload, opcode=10)
            return None
        if opcode == 8:
            raise OSError("WebSocket closed by the server")
        if opcode != 1:
            return None
        return json.loads(payload.decode())

    def close(self):
        try:
            self.send(b"\x03\xe8", opcode=8)
        except OSError:
            pass
        self.sock.close()


def attach(host, port, token, want):
    ws = WebSocket(host, port, "/v1/ws")
    ws.send({"t": "attach", "v": 1, "token": token})
    deadline = time.time() + 5
    while time.time() < deadline:
        m = ws.recv()
        if m is None:
            continue
        if m.get("t") == want:
            return ws, m
        if m.get("t") == "error":
            raise OSError("attach refused: " + json.dumps(m))
    raise OSError("no '" + want + "' after attach")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", required=True, help="http://127.0.0.1:PORT")
    args = ap.parse_args()
    url = urllib.parse.urlparse(args.server)
    host, port = url.hostname, url.port or 80
    if url.scheme != "http" or host not in ("127.0.0.1", "localhost", "::1"):
        log("plaintext only on loopback: give http://127.0.0.1:PORT")
        return 2
    base = f"http://{host}:{port}"
    try:
        created = post(base, "/v1/sessions", {"name": "relay-test-host", "password": "", "maxMembers": 2})
        joined = post(base, f"/v1/sessions/{urllib.parse.quote(created['code'])}/join",
                      {"name": "relay-test-client", "password": "", "spectator": False})
    except (urllib.error.URLError, OSError, KeyError, ValueError) as e:
        log(f"REST failed: {e}")
        return 1
    relay_h, relay_c = created.get("relay"), joined.get("relay")
    if not relay_h or not relay_c:
        log("the server offers no builtin relay (start it with --relay)")
        return 1
    sockets = []
    attached = True
    try:
        ws_h, _ = attach(host, port, created["token"], "created")
        sockets.append(ws_h)
        ws_c, _ = attach(host, port, joined["token"], "joined")
        sockets.append(ws_c)
    except (OSError, KeyError, ValueError) as e:
        attached = False
        log(f"WebSocket attach failed ({e}); continuing with the REST tokens")
    print(json.dumps({"relay": relay_h["addr"], "host_token": relay_h["token"], "client_token": relay_c["token"],
                      "code": created.get("code"), "attached": attached}), flush=True)
    last_ping = time.time()
    while True:
        socks = [s.sock for s in sockets]
        r, _, _ = select.select([sys.stdin] + socks, [], [], 1.0)
        if sys.stdin in r and not sys.stdin.readline():
            break
        for ws in sockets:
            if ws.sock in r:
                try:
                    ws.recv()
                except (OSError, ValueError):
                    pass
        if time.time() - last_ping > 20:
            last_ping = time.time()
            for ws in sockets:
                try:
                    ws.send({"t": "ping"})
                except OSError:
                    pass
    for ws in reversed(sockets):  # the client leaves first, then the host (the session ends)
        try:
            ws.send({"t": "leave"})
        except OSError:
            pass
        ws.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
