#!/usr/bin/env python3
"""M5.3b transport tests: a stand-in for mppm-server's builtin relay (design 5.6, MPRL v1).

Python standard library only. Used by tests/net/transport/transport_test.sh before the server package
merges; the same test can point at the real server's relay instead (--relay-server).

    relay_stub.py --bind 127.0.0.1 --port 47053 --ports 47060-47069 \
                  --token 0:<32 hex> --token 1:<32 hex> [--ready-file PATH]

Member 0 is the host. A REG from the host records the host address H; a REG from a client records its
address C and binds a relay port R for that member (the same R on a repeated REG); OK goes back from
the control port. On R a datagram from C goes to H and one from H goes to C, both sent from R;
anything else and every MPRL datagram on R is dropped. An unknown token gets ERR 1.
On SIGTERM/SIGINT every mapping is logged like the server does, then the stub exits 0:
    relay_stub: relay mapping session=stub member=<m> port=<R> up_bytes=<n> down_bytes=<n> closed=shutdown
"""
import argparse
import selectors
import signal
import socket
import struct
import sys

MAGIC = b"MPRL"
REG, OK, ERR, PUNCH = 1, 2, 3, 4


def log(msg):
    print("relay_stub: " + msg, file=sys.stderr, flush=True)


def ok_datagram(role, relay_port, addr):
    ip = socket.inet_aton(addr[0])
    return MAGIC + bytes([1, OK, 0, 0, role]) + struct.pack(">H", relay_port) + bytes([4]) + ip + struct.pack(">H", addr[1])


def err_datagram(code):
    return MAGIC + bytes([1, ERR, 0, 0, code])


class Mapping:
    def __init__(self, member, sock, port):
        self.member, self.sock, self.port = member, sock, port
        self.client = None
        self.up = 0      # client -> host bytes
        self.down = 0    # host -> client bytes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--ports", required=True, help="relay port range A-B")
    ap.add_argument("--token", action="append", default=[], help="MEMBER:HEX32 (member 0 = host)")
    ap.add_argument("--ready-file", help="written once listening")
    args = ap.parse_args()

    tokens = {}
    for t in args.token:
        member, hexs = t.split(":", 1)
        raw = bytes.fromhex(hexs)
        if len(raw) != 16:
            ap.error("tokens are 16 bytes (32 hex digits)")
        tokens[raw] = int(member)
    lo, hi = (int(x) for x in args.ports.split("-"))
    free_ports = list(range(lo, hi + 1))

    sel = selectors.DefaultSelector()
    ctrl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ctrl.bind((args.bind, args.port))
    ctrl.setblocking(False)
    sel.register(ctrl, selectors.EVENT_READ, None)

    host_addr = None
    mappings = {}  # member -> Mapping
    stop = []

    def on_signal(signum, frame):
        stop.append(signum)

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    log(f"listening control={args.bind}:{ctrl.getsockname()[1]} ports={lo}-{hi} members={len(tokens)}")
    if args.ready_file:
        with open(args.ready_file, "w") as f:
            f.write(f"{ctrl.getsockname()[1]}\n")

    while not stop:
        try:
            events = sel.select(timeout=0.2)
        except InterruptedError:
            continue
        for key, _ in events:
            sock = key.fileobj
            try:
                data, src = sock.recvfrom(65535)
            except (BlockingIOError, InterruptedError):
                continue
            except OSError:
                continue
            m = key.data
            if m is None:  # control port
                if len(data) != 24 or data[:4] != MAGIC or data[4] != 1 or data[5] != REG:
                    continue
                member = tokens.get(data[8:24])
                if member is None:
                    ctrl.sendto(err_datagram(1), src)
                    log(f"REG from {src[0]}:{src[1]}: unknown token")
                    continue
                if member == 0:
                    host_addr = src
                    ctrl.sendto(ok_datagram(0, 0, src), src)
                    log(f"REG host from {src[0]}:{src[1]}")
                    continue
                mp = mappings.get(member)
                if mp is None:
                    if not free_ports:
                        ctrl.sendto(err_datagram(4), src)
                        continue
                    port = free_ports.pop(0)
                    rs = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    rs.bind((args.bind, port))
                    rs.setblocking(False)
                    mp = Mapping(member, rs, port)
                    mappings[member] = mp
                    sel.register(rs, selectors.EVENT_READ, mp)
                mp.client = src
                ctrl.sendto(ok_datagram(1, mp.port, src), src)
                log(f"REG member {member} from {src[0]}:{src[1]} -> relay port {mp.port}")
                continue
            # A relay port.
            if data[:4] == MAGIC:
                continue  # never forwarded
            if mp_is(src, m.client):
                if host_addr is not None:
                    m.sock.sendto(data, host_addr)
                    m.up += len(data)
            elif mp_is(src, host_addr):
                if m.client is not None:
                    m.sock.sendto(data, m.client)
                    m.down += len(data)

    for member, mp in sorted(mappings.items()):
        log(f"relay mapping session=stub member={member} port={mp.port} up_bytes={mp.up} down_bytes={mp.down} closed=shutdown")
        mp.sock.close()
    ctrl.close()
    return 0


def mp_is(a, b):
    return b is not None and a[0] == b[0] and a[1] == b[1]


if __name__ == "__main__":
    sys.exit(main())
