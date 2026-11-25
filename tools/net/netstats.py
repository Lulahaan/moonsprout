#!/usr/bin/env python3
"""netstats.py - statistics of mppm online sessions (M5.3b) from the peers' console logs.

Reads the "mppm: net: " lines an online peer prints (docs/online/M5_3B_NETCODE.md, "Log lines"):
the per-900-tick stall windows, the session summary, barriers, input delay changes, time sync,
peer connections, drops, desyncs and the session end. Python 3 standard library only.

Subcommands:
  summary LOG... [--format table|tsv]
      one row per peer log: role, link, ticks, stalled ticks, stall rate, longest stall, barriers,
      barrier wait, phase error p50/p99/max (us), input delay per seat and its maximum, RTT,
      kbps out/in, end reason
  check LOG... [--max-stall-rate 2.0] [--max-phase-p99-us 2000] [--require-link direct|ice|relay]
               [--max-stall-ms MS] [--require-end REASON]
      exit 1 with one line per violation; 0 when every log passes; a negative limit is not checked
  --selftest
      the built-in tests (embedded sample lines, malformed ones included)

Exit codes: 0 ok, 1 a check failed (or the selftest), 2 bad arguments or an unreadable log.
"""

import argparse
import io
import os
import re
import sys
import tempfile

PREFIX = "mppm: net: "
LINKS = ("direct", "ice", "relay")

# summary: every key the lockstep package prints (design 8.12), with its parser
SUMMARY_KEYS = {
    "ticks": int, "stalled": int, "rate": float, "max_ms": float, "barriers": int,
    "barrier_ms": float, "phase_p50_us": int, "phase_p99_us": int, "phase_max_us": int,
    "d": str, "d_max": int, "rtt_us": int, "kbps_out": float, "kbps_in": float, "link": str,
}
STALL_KEYS = {"window_ticks": int, "stalled": int, "rate": float, "max_ms": float, "barrier_ms": float}

RE_ONLINE = re.compile(r"^online \((.*)\)\s*$")
RE_PEER = re.compile(r"^peer (\d+) (connected|disconnected)(.*)$")
RE_DROP = re.compile(r"^drop slot=(\d+) tick=(\d+)(?: reason=(\S+))?")
RE_DESYNC = re.compile(r"^desync at tick (\d+)\b(.*)$")
RE_END = re.compile(r"^session end reason=(\S+)")
RE_LOBBY = re.compile(r"^lobby (created|joined|failed)\b(.*)$")
RE_ICE = re.compile(r"^ice connected peer=(\S+) local=(\S+) remote=(\S+)")
RE_IMPAIR = re.compile(r"^impairment (.*)$")


def parse_kv(text, keys, required=True):
    """'a=1 b=2 ...' -> dict of the known keys parsed with their types; None when a required key is
    missing or a value does not parse. Unknown keys are kept as strings under '_extra'."""
    out = {}
    extra = {}
    for tok in text.split():
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        if k in keys:
            try:
                out[k] = keys[k](v)
            except ValueError:
                return None
        else:
            extra[k] = v
    if required and any(k not in out for k in keys):
        return None
    out["_extra"] = extra
    return out


def parse_d(text):
    """'0:2,1:3' -> {0: 2, 1: 3}; None when malformed. '-' or '' -> {}."""
    if text in ("", "-"):
        return {}
    out = {}
    for part in text.split(","):
        try:
            s, d = part.split(":", 1)
            out[int(s)] = int(d)
        except ValueError:
            return None
    return out


class PeerLog(object):
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        self.role = "-"
        self.online = {}
        self.summary = None
        self.windows = []
        self.barriers = []      # (tick, waited_ms, jump_us)
        self.delays = []        # (slot, d, path_us, reason, capped)
        self.syncs = []         # dicts of the sync line
        self.peers = []         # (member, event, rest)
        self.links = []         # link= of the 'peer N connected' lines
        self.drops = []         # (slot, tick, reason)
        self.desyncs = []       # (tick, rest)
        self.ends = []          # reasons
        self.lobby = []         # (what, rest)
        self.ice = []           # (peer, local, remote)
        self.impair = None
        self.malformed = []     # (line number, text)
        self.lines = 0

    # ---- derived values
    def link(self):
        if self.summary is not None:
            return self.summary["link"]
        return self.links[-1] if self.links else "-"

    def end(self):
        return self.ends[-1] if self.ends else "-"

    def bad(self, n, text):
        self.malformed.append((n, text.strip()))


def parse_line(log, n, raw):
    i = raw.find(PREFIX)
    if i < 0:
        return
    log.lines += 1
    body = raw[i + len(PREFIX):].rstrip("\r\n")
    if body.startswith("summary "):
        kv = parse_kv(body[len("summary "):], SUMMARY_KEYS)
        if kv is None or parse_d(kv["d"]) is None or kv["ticks"] < 0 or kv["stalled"] < 0:
            log.bad(n, body)
            return
        kv["d_map"] = parse_d(kv["d"])
        log.summary = kv
    elif body.startswith("stall "):
        kv = parse_kv(body[len("stall "):], STALL_KEYS)
        if kv is None:
            log.bad(n, body)
            return
        log.windows.append(kv)
    elif body.startswith("barrier "):
        kv = parse_kv(body[len("barrier "):], {"tick": int, "waited_ms": float, "jump_us": int})
        if kv is None:
            log.bad(n, body)
            return
        log.barriers.append((kv["tick"], kv["waited_ms"], kv["jump_us"]))
    elif body.startswith("delay "):
        kv = parse_kv(body[len("delay "):], {"slot": int, "d": int, "path_us": int, "reason": str})
        if kv is None:
            log.bad(n, body)
            return
        log.delays.append((kv["slot"], kv["d"], kv["path_us"], kv["reason"], body.rstrip().endswith("capped")))
    elif body.startswith("sync "):
        kv = parse_kv(body[len("sync "):], {"tick": int, "offset_us": int, "rtt_us": int,
                                            "phase_us": int, "slew_us": int})
        if kv is None:
            log.bad(n, body)
            return
        log.syncs.append(kv)
    else:
        m = RE_ONLINE.match(body)
        if m:
            for part in m.group(1).split(","):
                if "=" in part:
                    k, v = part.strip().split("=", 1)
                    log.online[k] = v
            log.role = log.online.get("role", "-")
            return
        m = RE_PEER.match(body)
        if m:
            rest = m.group(3)
            log.peers.append((int(m.group(1)), m.group(2), rest.strip()))
            if m.group(2) == "connected":
                lk = re.search(r"\blink=(\S+)", rest)
                if lk:
                    log.links.append(lk.group(1))
            return
        m = RE_DROP.match(body)
        if m:
            log.drops.append((int(m.group(1)), int(m.group(2)), m.group(3) or "-"))
            return
        m = RE_DESYNC.match(body)
        if m:
            log.desyncs.append((int(m.group(1)), m.group(2).strip()))
            return
        m = RE_END.match(body)
        if m:
            log.ends.append(m.group(1))
            return
        m = RE_LOBBY.match(body)
        if m:
            log.lobby.append((m.group(1), m.group(2).strip()))
            return
        m = RE_ICE.match(body)
        if m:
            log.ice.append((m.group(1), m.group(2), m.group(3)))
            return
        m = RE_IMPAIR.match(body)
        if m:
            log.impair = m.group(1).strip()
            return
        # other mppm: net: lines (harness, rng, floor, ...) are not ours


def read_log(path, text=None):
    log = PeerLog(path)
    if text is None:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    for n, raw in enumerate(text.splitlines(), 1):
        parse_line(log, n, raw)
    return log


# --------------------------------------------------------------------------
# summary
# --------------------------------------------------------------------------

COLUMNS = ["log", "role", "link", "ticks", "stalled", "rate%", "max_ms", "barriers", "barrier_ms",
           "ph_p50", "ph_p99", "ph_max", "d", "d_max", "rtt_us", "kbps_out", "kbps_in", "end"]


def row_of(log):
    s = log.summary
    if s is None:
        # no summary (killed peer, no session): what the windows tell
        stalled = sum(w["stalled"] for w in log.windows)
        ticks = sum(w["window_ticks"] for w in log.windows)
        mx = max([w["max_ms"] for w in log.windows] or [0.0])
        d = ",".join("%d:%d" % (sl, dd) for sl, dd in sorted({x[0]: x[1] for x in log.delays}.items())) or "-"
        return [log.name, log.role, log.link(), "%d*" % ticks if log.windows else "-",
                str(stalled) if log.windows else "-",
                ("%.2f*" % (100.0 * stalled / ticks)) if ticks else "-",
                ("%.1f*" % mx) if log.windows else "-", str(len(log.barriers)),
                "%.1f*" % sum(b[1] for b in log.barriers) if log.barriers else "-",
                "-", "-", "-", d, "-", "-", "-", "-", log.end()]
    return [log.name, log.role, s["link"], str(s["ticks"]), str(s["stalled"]), "%.2f" % s["rate"],
            "%.1f" % s["max_ms"], str(s["barriers"]), "%.1f" % s["barrier_ms"],
            str(s["phase_p50_us"]), str(s["phase_p99_us"]), str(s["phase_max_us"]), s["d"] or "-",
            str(s["d_max"]), str(s["rtt_us"]), "%.1f" % s["kbps_out"], "%.1f" % s["kbps_in"], log.end()]


def cmd_summary(a, out=None):
    out = out or sys.stdout
    logs = [read_log(p) for p in a.logs]
    rows = [row_of(l) for l in logs]
    if a.format == "tsv":
        out.write("\t".join(COLUMNS) + "\n")
        for r in rows:
            out.write("\t".join(r) + "\n")
        return 0
    widths = [max(len(COLUMNS[i]), *(len(r[i]) for r in rows)) for i in range(len(COLUMNS))]
    out.write("  ".join(COLUMNS[i].ljust(widths[i]) for i in range(len(COLUMNS))).rstrip() + "\n")
    for r in rows:
        out.write("  ".join(r[i].ljust(widths[i]) for i in range(len(COLUMNS))).rstrip() + "\n")
    notes = []
    for l in logs:
        if l.summary is None:
            notes.append("%s: no summary line%s" % (l.name, " (values marked * come from %d stall windows)"
                                                   % len(l.windows) if l.windows else ""))
        if l.malformed:
            notes.append("%s: %d malformed line(s), first at line %d: %s"
                         % (l.name, len(l.malformed), l.malformed[0][0], l.malformed[0][1][:100]))
        for sl, t, why in l.drops:
            notes.append("%s: drop slot=%d tick=%d reason=%s" % (l.name, sl, t, why))
        for t, rest in l.desyncs:
            notes.append("%s: desync at tick %d %s" % (l.name, t, rest))
        capped = [x for x in l.delays if x[4]]
        if capped:
            notes.append("%s: input delay capped %d time(s)" % (l.name, len(capped)))
        if l.impair:
            notes.append("%s: impairment %s" % (l.name, l.impair))
        for peer, loc, rem in l.ice:
            notes.append("%s: ice peer=%s local=%s remote=%s" % (l.name, peer, loc, rem))
    for n in notes:
        out.write("  %s\n" % n)
    return 0


# --------------------------------------------------------------------------
# check
# --------------------------------------------------------------------------

def check_logs(logs, max_rate=2.0, max_p99=2000, require_link=None, max_stall_ms=None, require_end=None):
    """[violation text] for the parsed logs."""
    v = []
    clients = 0
    for l in logs:
        s = l.summary
        if s is None:
            v.append("%s: no summary line%s" % (l.name, " (%d malformed)" % len(l.malformed) if l.malformed else ""))
            continue
        if max_rate is not None and max_rate >= 0 and s["rate"] > max_rate:
            v.append("%s: stall rate %.2f%% above %.2f%% (%d of %d ticks)" % (l.name, s["rate"], max_rate,
                                                                            s["stalled"], s["ticks"]))
        if max_p99 is not None and max_p99 >= 0 and s["phase_p99_us"] >= 0 and s["phase_p99_us"] > max_p99:
            v.append("%s: phase error p99 %d us above %d us" % (l.name, s["phase_p99_us"], max_p99))
        if max_stall_ms is not None and max_stall_ms >= 0 and s["max_ms"] > max_stall_ms:
            v.append("%s: longest stall %.1f ms above %.1f ms" % (l.name, s["max_ms"], max_stall_ms))
        if require_end is not None and l.end() != require_end:
            v.append("%s: session end reason=%s, expected %s" % (l.name, l.end(), require_end))
        if require_link is not None and s["link"] != "host":
            clients += 1
            if s["link"] != require_link:
                v.append("%s: link=%s, expected %s" % (l.name, s["link"], require_link))
    if require_link is not None and clients == 0 and not any(x.endswith("no summary line") for x in v):
        v.append("no client log (link other than host) to check --require-link %s against" % require_link)
    return v


def cmd_check(a, out=None):
    out = out or sys.stdout
    logs = [read_log(p) for p in a.logs]
    v = check_logs(logs, a.max_stall_rate, a.max_phase_p99_us, a.require_link, a.max_stall_ms, a.require_end)
    for x in v:
        out.write("violation: %s\n" % x)
    if not v:
        parts = []
        for l in logs:
            s = l.summary
            parts.append("%s rate=%.2f%% p99=%s link=%s" % (l.name, s["rate"],
                                                          "-" if s["phase_p99_us"] < 0 else "%dus" % s["phase_p99_us"],
                                                          s["link"]))
        out.write("ok: %s\n" % "; ".join(parts))
    return 1 if v else 0


# --------------------------------------------------------------------------
# selftest
# --------------------------------------------------------------------------

SAMPLE_HOST = """\
mppm: net: online (server=http://127.0.0.1:47200, role=host, mode=battle, net-mode=auto, bind=127.0.0.1)
mppm: net: impairment delay_us=46000 jitter_us=8000 loss_ppm=0 dup_ppm=0 seed=1 (test only)
mppm: net: lobby created code=K7QD-M2XA server=http://127.0.0.1:47200
mppm: net: peer 1 connected link=direct remote=127.0.0.1:40123
mppm: net: delay slot=0 d=2 path_us=52000 reason=init
mppm: net: barrier tick=0 waited_ms=412.5 jump_us=-3120
mppm: net: stall window_ticks=900 stalled=3 rate=0.33 max_ms=21.4 barrier_ms=0.0
mppm: net: stall window_ticks=900 stalled=0 rate=0.00 max_ms=0.0 barrier_ms=0.0
some other output line
mppm: net: rng session end draws: sim=123 pres=4 draw-unflagged=0 other-in-session=0
mppm: net: session end reason=done
mppm: net: summary ticks=1800 stalled=3 rate=0.17 max_ms=21.4 barriers=1 barrier_ms=412.5 phase_p50_us=-1 phase_p99_us=-1 phase_max_us=-1 d=0:2,1:2 d_max=2 rtt_us=101200 kbps_out=21.3 kbps_in=9.8 link=host
"""

SAMPLE_CLIENT = """\
[t=1.0] mppm: net: online (server=http://127.0.0.1:47200, role=join, mode=-, net-mode=relay, bind=127.0.0.1)
mppm: net: peer 0 connected link=relay remote=127.0.0.1:47210
mppm: net: delay slot=1 d=2 path_us=55000 reason=init
mppm: net: delay slot=1 d=3 path_us=70000 reason=path
mppm: net: sync tick=300 offset_us=-1520 rtt_us=100400 phase_us=310 slew_us=-120
mppm: net: sync tick=330 offset_us=-1500 rtt_us=100900 phase_us=-80 slew_us=0
mppm: net: barrier tick=0 waited_ms=10.0 jump_us=880
mppm: net: stall window_ticks=900 stalled=27 rate=3.00 max_ms=48.0 barrier_ms=0.0
mppm: net: drop slot=3 tick=1612 reason=timeout
mppm: net: session end reason=done
mppm: net: summary ticks=1800 stalled=30 rate=1.67 max_ms=48.0 barriers=1 barrier_ms=10.0 phase_p50_us=410 phase_p99_us=2400 phase_max_us=3900 d=0:2,1:3 d_max=3 rtt_us=100800 kbps_out=9.9 kbps_in=21.0 link=relay
"""

SAMPLE_MALFORMED = """\
mppm: net: online (server=x, role=join, mode=-, net-mode=ice, bind=127.0.0.1)
mppm: net: stall window_ticks=900 stalled=x rate=0.00 max_ms=0.0 barrier_ms=0.0
mppm: net: stall window_ticks=900 stalled=9 rate=1.00 max_ms=30.5 barrier_ms=0.0
mppm: net: barrier tick=5000 waited_ms=12
mppm: net: delay slot=1 d=9 path_us=400000 reason=path capped
mppm: net: sync tick=1 offset_us=1
mppm: net: ice connected peer=1 local=relay remote=relay
mppm: net: desync at tick 900 (slot 1: host 1234abcd peer 1234abce); report /tmp/r.txt
mppm: net: session end reason=desync
mppm: net: summary ticks=900 stalled=1 rate=0.11 max_ms=1.0 barriers=0 barrier_ms=0.0 phase_p50_us=1 phase_p99_us=2 phase_max_us=3 d=0:x d_max=2 rtt_us=1 kbps_out=1.0 kbps_in=1.0 link=ice
mppm: net: summary ticks=900 stalled=1 rate=zero
mppm: net:
"""


def cmd_selftest(_a):
    fails = []

    def check(cond, what):
        print("%s  %s" % ("ok  " if cond else "FAIL", what))
        if not cond:
            fails.append(what)

    h = read_log("host.log", SAMPLE_HOST)
    c = read_log("client.log", SAMPLE_CLIENT)
    m = read_log("bad.log", SAMPLE_MALFORMED)

    check(h.role == "host" and h.online.get("net-mode") == "auto" and h.online.get("bind") == "127.0.0.1",
          "online line: role and fields")
    check(h.summary is not None and h.summary["ticks"] == 1800 and h.summary["rate"] == 0.17
          and h.summary["phase_p99_us"] == -1 and h.summary["d_map"] == {0: 2, 1: 2}
          and h.summary["link"] == "host" and h.summary["kbps_out"] == 21.3, "summary line parsed")
    check(len(h.windows) == 2 and h.windows[0]["stalled"] == 3 and h.windows[0]["max_ms"] == 21.4,
          "stall windows parsed")
    check(h.barriers == [(0, 412.5, -3120)], "barrier line parsed (negative jump)")
    check(h.impair is not None and "delay_us=46000" in h.impair, "impairment line")
    check(h.lobby == [("created", "code=K7QD-M2XA server=http://127.0.0.1:47200")], "lobby created line")
    check(h.links == ["direct"] and h.peers[0][:2] == (1, "connected"), "peer connected line")
    check(h.end() == "done", "session end reason")
    check(c.role == "join" and c.link() == "relay" and c.links == ["relay"],
          "client: prefix text before 'mppm: net:' allowed, link")
    check(c.delays == [(1, 2, 55000, "init", False), (1, 3, 70000, "path", False)], "delay lines")
    check(len(c.syncs) == 2 and c.syncs[0]["phase_us"] == 310 and c.syncs[1]["slew_us"] == 0, "sync lines")
    check(c.drops == [(3, 1612, "timeout")], "drop line")
    check(c.summary["d_max"] == 3 and c.summary["phase_p99_us"] == 2400, "client summary")
    check(len(m.malformed) == 5 and m.summary is None, "malformed: 5 lines rejected, no valid summary "
          "(got %d: %s)" % (len(m.malformed), [x[0] for x in m.malformed]))
    check(len(m.windows) == 1 and m.windows[0]["stalled"] == 9, "malformed: the good stall line is kept")
    check(m.delays == [(1, 9, 400000, "path", True)], "delay capped flag")
    check(m.ice == [("1", "relay", "relay")], "ice connected line")
    check(m.desyncs and m.desyncs[0][0] == 900 and "report /tmp/r.txt" in m.desyncs[0][1], "desync line")
    check(m.end() == "desync" and m.role == "join", "end and role of the malformed log")
    check(parse_d("0:2,1:3") == {0: 2, 1: 3} and parse_d("-") == {} and parse_d("0:2,x") is None, "parse_d")

    # check rules
    v = check_logs([h, c])
    check(len(v) == 1 and "client.log: phase error p99 2400 us above 2000 us" in v[0], "check: phase p99 of the client, host -1 skipped")
    v = check_logs([h, c], max_rate=1.0, max_p99=3000)
    check(len(v) == 1 and "client.log: stall rate 1.67% above 1.00%" in v[0], "check: stall rate")
    v = check_logs([h, c], max_p99=None, require_link="relay")
    check(v == [], "check: --require-link relay passes, the host's link=host is not checked")
    v = check_logs([h, c], max_p99=None, require_link="direct")
    check(len(v) == 1 and "link=relay, expected direct" in v[0], "check: --require-link direct fails")
    v = check_logs([h], require_link="relay")
    check(len(v) == 1 and "no client log" in v[0], "check: --require-link needs a client log")
    v = check_logs([h, m])
    check(len(v) == 1 and "bad.log: no summary line (5 malformed)" in v[0], "check: a log without a summary fails")
    v = check_logs([h, c], max_p99=None, max_stall_ms=30.0)
    check(len(v) == 1 and "longest stall 48.0 ms above 30.0 ms" in v[0], "check: --max-stall-ms")
    v = check_logs([h, c], max_rate=-1, max_p99=-1)
    check(v == [], "check: negative limits are not checked")
    v = check_logs([h, c], max_p99=None, require_end="done")
    check(v == [], "check: --require-end done")
    check(len(check_logs([m], require_end="done")) == 1, "check: --require-end on a log without summary")

    # CLI on files
    base = "/var/tmp" if os.path.isdir("/var/tmp") else None
    with tempfile.TemporaryDirectory(prefix="netstats-selftest-", dir=base) as d:
        paths = []
        for n, t in (("host.log", SAMPLE_HOST), ("client.log", SAMPLE_CLIENT), ("bad.log", SAMPLE_MALFORMED)):
            p = os.path.join(d, n)
            with open(p, "w") as f:
                f.write(t)
            paths.append(p)

        def run(argv):
            s = io.StringIO()
            olds, olde = sys.stdout, sys.stderr
            try:
                sys.stdout = s
                sys.stderr = s
                rc = main(argv)
            finally:
                sys.stdout, sys.stderr = olds, olde
            return rc, s.getvalue()

        rc, t = run(["summary"] + paths)
        check(rc == 0 and "host.log" in t and "client.log" in t and "1.67" in t and "0:2,1:3" in t
              and "bad.log: no summary line (values marked * come from 1 stall windows)" in t
              and "drop slot=3 tick=1612" in t and "5 malformed" in t, "CLI summary table and notes")
        rc, t = run(["summary", "--format", "tsv"] + paths[:2])
        rows = [r.split("\t") for r in t.strip().split("\n")]
        check(rc == 0 and rows[0] == COLUMNS and len(rows) == 3 and rows[2][2] == "relay" and rows[2][5] == "1.67"
              and rows[1][10] == "-1" and rows[2][13] == "3", "CLI summary --format tsv")
        rc, t = run(["check", "--max-phase-p99-us", "3000"] + paths[:2])
        check(rc == 0 and t.startswith("ok: ") and "client.log rate=1.67% p99=2400us link=relay" in t, "CLI check passes")
        rc, t = run(["check"] + paths)
        check(rc == 1 and t.count("violation: ") == 2, "CLI check: 2 violations, exit 1")
        rc, t = run(["check", "--require-link", "ice"] + paths[:2])
        check(rc == 1 and "expected ice" in t, "CLI check --require-link ice fails on a relay client")
        rc, t = run(["check", os.path.join(d, "missing.log")])
        check(rc == 2 and "netstats.py:" in t, "CLI: a missing log exits 2")
        rc, t = run(["check", "--require-link", "carrier-pigeon"] + paths[:1])
        check(rc == 2, "CLI: a bad --require-link exits 2")
        rc, t = run([])
        check(rc == 2, "CLI: no subcommand exits 2")

    print("selftest %s" % ("passed" if not fails else "FAILED (%d)" % len(fails)))
    return 0 if not fails else 1


def main(argv=None):
    ap = argparse.ArgumentParser(prog="netstats.py",
                                 description="mppm online sessions: statistics from the peers' console logs")
    ap.add_argument("--selftest", action="store_true", help="run the built-in tests")
    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("summary", help="one row per peer log")
    p.add_argument("logs", nargs="+")
    p.add_argument("--format", choices=("table", "tsv"), default="table")
    p.set_defaults(fn=cmd_summary)

    p = sub.add_parser("check", help="exit 1 when a log breaks a limit")
    p.add_argument("logs", nargs="+")
    p.add_argument("--max-stall-rate", type=float, default=2.0, metavar="PCT",
                   help="highest stall rate in percent (default 2.0; negative: not checked)")
    p.add_argument("--max-phase-p99-us", type=int, default=2000, metavar="US",
                   help="highest phase error p99 in microseconds (default 2000; negative: not checked; "
                        "the host's -1 is skipped)")
    p.add_argument("--require-link", choices=LINKS, default=None,
                   help="every client log (link other than host) must have this link")
    p.add_argument("--max-stall-ms", type=float, default=None, metavar="MS",
                   help="highest longest stall (summary max_ms)")
    p.add_argument("--require-end", default=None, metavar="REASON",
                   help="the last 'session end reason=' must be this (done, left, ...)")
    p.set_defaults(fn=cmd_check)

    try:
        a = ap.parse_args(argv)
    except SystemExit as e:
        return 2 if e.code else 0
    if a.selftest:
        return cmd_selftest(a)
    if not getattr(a, "fn", None):
        ap.print_help(sys.stderr)
        return 2
    try:
        return a.fn(a)
    except OSError as e:
        sys.stderr.write("netstats.py: %s\n" % e)
        return 2
    except BrokenPipeError:
        return 0


if __name__ == "__main__":
    sys.exit(main())
