#!/usr/bin/env python3
"""netlog.py - offline tools for the mppm determinism harness (M5.1a, M5.1b, M5.2, M5.3a, M5.3b).

Writes synthetic two- or four-player input logs (.mpnl), prints and compares them, and
compares and summarises the checksum logs (.cksum) the game writes with
--checksum-log. Python 3 standard library only.

Subcommands:
  synth --seed S --synth-seed K --ticks N [--vs-stage V | --ch-stage C] [--handicap R,B]
        [--tick-order update|draw] [--sim-k X] [--event T:CODE ...] [--format-version 1..5]
        [--players 2|4] [--online] [--mods-hash HEX] [--mods-list TEXT] -o FILE
  mutate FILE --tick T [--port P] [--ticks N] -o FILE
  dump FILE [--from T] [--count N] [--events]
  cmp-input A B
  cmp-cksum A B [--limit N]
  stats FILE
  selftest

See docs/online/M5_1A_HARNESS.md for the file formats.
"""

import argparse
import os
import struct
import sys
import tempfile

# --------------------------------------------------------------------------
# Input log format
# --------------------------------------------------------------------------

MAGIC = b"MPNL"
VERSION = 3             # what synth writes by default with 2 players (the engine writes 3, or 4 with 4 captains)
VERSIONS = (1, 2, 3, 4, 5) # what the tools read
HEADER_SIZE = 64     # versions 1 and 2; version 3: V3_FIXED_SIZE + the mod list area
SLOTS = 4
SLOT_SIZE = 12
RECORD_SIZE = SLOTS * SLOT_SIZE  # 48
OPEN_TICKS = 0xFFFFFFFF
STAGE_NONE = 0xFF
FLAG_SYNTHETIC = 0x1
FLAG_ONLINE = 0x2       # recorded in an online session (M5.3b; informational, any version >= 3)
START_BOOT = 0
START_VS = 1
START_CH = 2            # version 5: 2P Challenge start barrier, byte 27 = the course
START_MODE_NAMES = {START_BOOT: "boot", START_VS: "VS start barrier", START_CH: "Challenge start barrier"}

# offset 0 magic, 4 version, 6 header size, 8 seed, 12 ticks, 16 build id,
# 24 slots, 25 record size, 26 start mode, 27 stage, 28 flags, 32..63 zero (version 1)
HEADER_FMT = "<4sHHIIQBBBBI32x"
# version 2 uses the former zero area: 32 tick order (0 draw-first, 1 update-first), 33..35 zero,
# 36 sim k (IEEE-754 float bits, 0 = engine default 3.6), 40 Synced-settings hash (0 = not
# recorded), 44..63 zero
HEADER_V2_FMT = "<4sHHIIQBBBBIB3xII20x"
TICK_ORDER_DRAW = 0
TICK_ORDER_UPDATE = 1
TICK_ORDER_NAMES = {TICK_ORDER_DRAW: "draw-first", TICK_ORDER_UPDATE: "update-first"}
SIM_K_DEFAULT = 3.6
SIM_K_MIN = 1.0
SIM_K_MAX = 3.6
# version 3: v2's 64 bytes, then 64 mods hash (32 bytes, zero = stock content), 96 content hash
# (32 bytes, informational), 128 mod list "id@version,..." NUL-terminated and zero-padded to a
# multiple of 16 (16 zero bytes for stock). The header size at 6 is 128 + the list area.
V3_FIXED_SIZE = 128
V3_LIST_MIN = 16
V3_LIST_MAX = 1024
HASH_SIZE = 32
# version 4 (M5.3a): version 3 plus byte 33 = captains (4; 0 and 2 read as 2). The engine writes it only
# with more than 2 captains, so 1-2 captain logs stay version 3. Only 2 or 4 exist in M5.3a part 1.
PLAYERS_BYTE = 33
STOCK_PLAYERS = 2
PLAYER_COUNTS = (2, 4)
# version 5 (M5.3b): the version 4 layout plus start mode 2 (Challenge, course in byte 27) and the VS
# handicaps of team 0 (Red) and team 1 (Blue) at bytes 34 and 35 (0 = stock). The engine writes it only for a
# Challenge session or non-stock handicaps, so every other log stays version 3 or 4.
HANDICAP_BYTES = (34, 35)
HANDICAP_MAX = 10
CH_COURSES = 30         # --ch-stage 0..29
# byte 11 of port 0's slot (version 2): test event of the tick (0 = none); code c needs c <= players
EVENT_NAMES = {1: "captain 0 lethal damage", 2: "captain 1 lethal damage",
               3: "captain 2 lethal damage", 4: "captain 3 lethal damage"}
SLOT_FMT = "<Hbbbb BBBB bB".replace(" ", "")
assert struct.calcsize(HEADER_FMT) == HEADER_SIZE
assert struct.calcsize(HEADER_V2_FMT) == HEADER_SIZE
assert struct.calcsize(SLOT_FMT) == SLOT_SIZE

BTN_LEFT = 0x0001
BTN_RIGHT = 0x0002
BTN_DOWN = 0x0004
BTN_UP = 0x0008
BTN_Z = 0x0010
BTN_R = 0x0020
BTN_L = 0x0040
BTN_A = 0x0100
BTN_B = 0x0200
BTN_X = 0x0400
BTN_Y = 0x0800
BTN_START = 0x1000

# Letters used by dump, in print order.
BUTTON_LETTERS = [
    (BTN_A, "A"), (BTN_B, "B"), (BTN_X, "X"), (BTN_Y, "Y"), (BTN_Z, "Z"),
    (BTN_R, "R"), (BTN_L, "L"), (BTN_START, "S"),
    (BTN_UP, "^"), (BTN_DOWN, "v"), (BTN_LEFT, "<"), (BTN_RIGHT, ">"),
]


class LogError(Exception):
    pass


class Slot(object):
    __slots__ = ("button", "stickX", "stickY", "substickX", "substickY",
                 "triggerLeft", "triggerRight", "analogA", "analogB", "err", "event")

    def __init__(self, button=0, stickX=0, stickY=0, substickX=0, substickY=0,
                 triggerLeft=0, triggerRight=0, analogA=0, analogB=0, err=0, event=0):
        self.button = button
        self.stickX = stickX
        self.stickY = stickY
        self.substickX = substickX
        self.substickY = substickY
        self.triggerLeft = triggerLeft
        self.triggerRight = triggerRight
        self.analogA = analogA
        self.analogB = analogB
        self.err = err
        self.event = event  # byte 11; version 2: test event code on port 0, else 0

    @staticmethod
    def absent():
        return Slot(err=-1)

    def pack(self):
        return struct.pack(SLOT_FMT, self.button, self.stickX, self.stickY,
                           self.substickX, self.substickY, self.triggerLeft,
                           self.triggerRight, self.analogA, self.analogB,
                           self.err, self.event)

    @staticmethod
    def unpack(buf, off):
        v = struct.unpack_from(SLOT_FMT, buf, off)
        return Slot(*v)

    def as_tuple(self):
        return (self.button, self.stickX, self.stickY, self.substickX,
                self.substickY, self.triggerLeft, self.triggerRight,
                self.analogA, self.analogB, self.err, self.event)


class InputLog(object):
    def __init__(self):
        self.version = VERSION
        self.seed = 0
        self.header_ticks = 0
        self.build_id = 0
        self.start_mode = 0
        self.stage = STAGE_NONE
        self.flags = 0
        self.tick_order = TICK_ORDER_UPDATE
        self.sim_k_bits = 0
        self.settings_hash = 0
        self.mods_hash = bytes(HASH_SIZE)     # version 3; zero (stock) for versions 1 and 2
        self.content_hash = bytes(HASH_SIZE)  # version 3; zero = not recorded
        self.mods_list = ""                   # version 3; "" = stock
        self.players = STOCK_PLAYERS          # version 4: byte 33; else 2
        self.handicap = (0, 0)                # version 5: bytes 34/35 (Red, Blue); else stock
        self.data = b""
        self.rec_off = HEADER_SIZE
        self.ticks = 0
        self.open = False

    def record(self, tick):
        """Return the 4 slots of one tick."""
        base = self.rec_off + tick * RECORD_SIZE
        return [Slot.unpack(self.data, base + p * SLOT_SIZE) for p in range(SLOTS)]

    def record_bytes(self, tick):
        base = self.rec_off + tick * RECORD_SIZE
        return self.data[base:base + RECORD_SIZE]

    def event(self, tick):
        """Test event code of record `tick` (byte 11 of port 0), 0 when none."""
        return self.data[self.rec_off + tick * RECORD_SIZE + SLOT_SIZE - 1]

    def events(self):
        """[(checksum tick, code)] of every record with an event (record T-1 is tick T)."""
        out = []
        for t in range(self.ticks):
            c = self.event(t)
            if c:
                out.append((t + 1, c))
        return out

    def stock(self):
        """True when the log was made with stock content (no mods)."""
        return self.mods_hash == bytes(HASH_SIZE)

    def challenge(self):
        """True for a 2P Challenge log (start mode 2; byte 27 is then the course)."""
        return self.start_mode == START_CH

    def sim_k(self):
        """The header's sim k as a float, None for 0 (the engine default)."""
        if self.sim_k_bits == 0:
            return None
        return struct.unpack("<f", struct.pack("<I", self.sim_k_bits))[0]


def fmt_sim_k(log):
    k = log.sim_k()
    return "default (%.1f)" % SIM_K_DEFAULT if k is None else "%g" % k


def fmt_content(log):
    """'stock' or 'mods <hash8> (<list>)'."""
    if log.stock():
        return "stock"
    return "mods %s (%s)" % (log.mods_hash.hex()[:8], log.mods_list or "none")


def fmt_flags(flags):
    names = []
    if flags & FLAG_SYNTHETIC:
        names.append("synthetic")
    if flags & FLAG_ONLINE:
        names.append("online")
    return "0x%08x%s" % (flags, " (%s)" % ", ".join(names) if names else "")


def fmt_handicap(h):
    return "red %s, blue %s" % tuple("stock" if v == 0 else str(v) for v in h)


def fmt_tick_order(v):
    return TICK_ORDER_NAMES.get(v, "invalid (%d)" % v)


def read_input_log(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < HEADER_SIZE:
        raise LogError("%s: too short for a header (%d bytes)" % (path, len(data)))
    (magic, version, hsize, seed, ticks, build, slots, rsize, mode, stage,
     flags) = struct.unpack_from(HEADER_FMT, data, 0)
    if magic != MAGIC:
        raise LogError("%s: bad magic %r" % (path, magic))
    if version not in VERSIONS:
        raise LogError("%s: unsupported version %d" % (path, version))
    if hsize < HEADER_SIZE:
        raise LogError("%s: bad header size %d" % (path, hsize))
    if slots != SLOTS or rsize != RECORD_SIZE:
        raise LogError("%s: unsupported layout slots=%d record=%d" % (path, slots, rsize))
    log = InputLog()
    log.version = version
    log.seed = seed
    log.header_ticks = ticks
    log.build_id = build
    log.start_mode = mode
    log.stage = stage
    log.flags = flags
    if version >= 2:
        order, kbits, shash = struct.unpack_from("<B3xII", data, 32)
        log.tick_order = order
        log.sim_k_bits = kbits
        log.settings_hash = shash
    else:
        log.tick_order = TICK_ORDER_DRAW  # version 1 builds ran stock draw-first only
    if version >= 3:
        if not V3_FIXED_SIZE + V3_LIST_MIN <= hsize <= V3_FIXED_SIZE + V3_LIST_MAX:
            raise LogError("%s: bad header size %d (version 3: %d to %d)"
                           % (path, hsize, V3_FIXED_SIZE + V3_LIST_MIN, V3_FIXED_SIZE + V3_LIST_MAX))
        if len(data) < hsize:
            raise LogError("%s: too short for its header (%d of %d bytes)" % (path, len(data), hsize))
        area = data[V3_FIXED_SIZE:hsize]
        nul = area.find(b"\0")
        if nul < 0:
            raise LogError("%s: bad header: the mod list has no NUL" % path)
        log.mods_hash = bytes(data[64:96])
        log.content_hash = bytes(data[96:128])
        log.mods_list = area[:nul].decode("ascii", errors="replace")
        if version >= 4:
            n = data[PLAYERS_BYTE]
            if n in (0, STOCK_PLAYERS):
                log.players = STOCK_PLAYERS
            elif n == 4:
                log.players = 4
            else:
                raise LogError("%s: bad header: captains byte %d (version %d: 2 or 4)" % (path, n, version))
        if version >= 5:
            h = (data[HANDICAP_BYTES[0]], data[HANDICAP_BYTES[1]])
            if max(h) > HANDICAP_MAX:
                raise LogError("%s: bad header: handicaps %d,%d (version 5: 0 to %d)" % (path, h[0], h[1], HANDICAP_MAX))
            log.handicap = h
    if mode == START_CH:
        if version < 5:
            raise LogError("%s: bad header: start mode 2 (Challenge) needs version 5, the file is version %d"
                           % (path, version))
        if stage >= CH_COURSES:
            raise LogError("%s: bad header: Challenge course %d (0 to %d)" % (path, stage, CH_COURSES - 1))
        if log.players != STOCK_PLAYERS:
            raise LogError("%s: bad header: a Challenge log has 2 captains, not %d" % (path, log.players))
    elif mode not in (START_BOOT, START_VS):
        raise LogError("%s: bad header: start mode %d (0 boot, 1 VS, 2 Challenge)" % (path, mode))
    elif hsize > len(data):
        raise LogError("%s: too short for its header (%d of %d bytes)" % (path, len(data), hsize))
    log.data = data
    log.rec_off = hsize
    avail = (len(data) - hsize) // RECORD_SIZE
    if ticks == OPEN_TICKS:
        log.open = True
        log.ticks = avail
    else:
        if ticks > avail:
            raise LogError("%s: header says %d ticks but the file holds %d"
                           % (path, ticks, avail))
        log.ticks = ticks
    return log


# --------------------------------------------------------------------------
# Synthetic input generator
# --------------------------------------------------------------------------

M32 = 0xFFFFFFFF


def _mix32(x):
    """Murmur3 finaliser: a 32-bit bijection used to derive stream seeds."""
    x &= M32
    x ^= x >> 16
    x = (x * 0x85EBCA6B) & M32
    x ^= x >> 13
    x = (x * 0xC2B2AE35) & M32
    x ^= x >> 16
    return x


class XorShift32(object):
    """Marsaglia xorshift32. Pure integer arithmetic, so the stream does not
    depend on the Python version or platform."""

    def __init__(self, synth_seed, port):
        s = _mix32((synth_seed & M32) ^ _mix32(0x9E3779B9 * (port + 1)))
        s = _mix32(s + 0x6D2B79F5 * (port + 1))
        self.state = s if s != 0 else 0x2545F491

    def next(self):
        x = self.state
        x ^= (x << 13) & M32
        x ^= x >> 17
        x ^= (x << 5) & M32
        self.state = x
        return x

    def below(self, n):
        """Uniform-ish integer in [0, n) (the tiny modulo bias is irrelevant)."""
        return self.next() % n

    def range(self, lo, hi):
        """Integer in [lo, hi], inclusive."""
        return lo + self.below(hi - lo + 1)


# 8 directions, counter-clockwise from right: (x, y) unit signs.
DIRS = [(1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1)]

# Full deflection after the game's PADClamp (JUTGamePad sClampMode 1; Aurora's ClampStick with
# max 72 / xy 40 for the main stick, 59 / 31 for the C-stick): (on one axis, each axis on a diagonal).
MAIN_FULL = (72, 40)
MAIN_HALF = (36, 20)
CSTICK = (59, 31)
TRIGGER_FULL = 150

# (button, 1-in-N start chance per tick, min hold, max hold, trigger byte)
BUTTON_PLAN = [
    (BTN_A, 20, 1, 6, None),
    (BTN_B, 45, 5, 40, None),
    (BTN_X, 300, 2, 2, None),
    (BTN_Y, 500, 2, 2, None),
    (BTN_Z, 500, 2, 2, None),
    (BTN_R, 300, 3, 3, "R"),
    (BTN_L, 300, 3, 3, "L"),
    (BTN_UP, 900, 2, 2, None),
    (BTN_DOWN, 900, 2, 2, None),
]


def _dir_vec(d, size):
    dx, dy = DIRS[d]
    m = size[1] if (dx and dy) else size[0]
    return dx * m, dy * m


class PortGen(object):
    """One synthetic controller. Every tick draws from the PRNG in a fixed
    order, so the output is a pure function of (synth seed, port, tick)."""

    def __init__(self, synth_seed, port):
        self.rng = XorShift32(synth_seed, port)
        self.main_left = 0
        self.main = (0, 0)
        self.c_left = 0
        self.c = (0, 0)
        # per button: remaining held ticks, and a one-tick release gap so two
        # presses never merge into one long hold
        self.held = [0] * len(BUTTON_PLAN)
        self.gap = [0] * len(BUTTON_PLAN)

    def step(self):
        r = self.rng
        if self.main_left == 0:
            self.main_left = r.range(8, 60)
            if r.below(5) == 0:          # 20% neutral
                self.main = (0, 0)
            else:
                d = r.below(8)
                size = MAIN_FULL if r.below(2) == 0 else MAIN_HALF
                self.main = _dir_vec(d, size)
        self.main_left -= 1

        if self.c_left == 0:
            self.c_left = r.range(8, 60)
            if r.below(100) < 85:        # 85% neutral
                self.c = (0, 0)
            else:
                self.c = _dir_vec(r.below(8), CSTICK)
        self.c_left -= 1

        slot = Slot(stickX=self.main[0], stickY=self.main[1],
                    substickX=self.c[0], substickY=self.c[1])
        for i, (btn, chance, lo, hi, trig) in enumerate(BUTTON_PLAN):
            if self.held[i] == 0:
                if self.gap[i]:
                    self.gap[i] -= 1
                elif r.below(chance) == 0:
                    self.held[i] = r.range(lo, hi)
            if self.held[i] > 0:
                slot.button |= btn
                if trig == "R":
                    slot.triggerRight = TRIGGER_FULL
                elif trig == "L":
                    slot.triggerLeft = TRIGGER_FULL
                self.held[i] -= 1
                if self.held[i] == 0:
                    self.gap[i] = 1
        return slot


def sim_k_bits(k):
    """Header bits of --sim-k: 0 for the engine default, else the float32 bits of k."""
    if k is None or k == 0:
        return 0
    if not SIM_K_MIN <= k <= SIM_K_MAX:
        raise LogError("--sim-k %g outside %.1f to %.1f (or 0 for the default)"
                       % (k, SIM_K_MIN, SIM_K_MAX))
    return struct.unpack("<I", struct.pack("<f", k))[0]


def parse_mods_hash(text):
    """--mods-hash: 64 hex digits -> 32 bytes ("" or None = stock, 32 zero bytes)."""
    if not text:
        return bytes(HASH_SIZE)
    try:
        b = bytes.fromhex(text)
    except ValueError:
        b = b""
    if len(b) != HASH_SIZE:
        raise LogError("bad --mods-hash %r (want 64 hex digits)" % text)
    return b


def v3_list_area(mods_list):
    """The version 3 mod list area: the text, a NUL, zero padding to a multiple of 16."""
    raw = (mods_list or "").encode("ascii")
    if b"\0" in raw:
        raise LogError("the mod list holds a NUL")
    size = (len(raw) + 1 + 15) // 16 * 16
    if size > V3_LIST_MAX:
        raise LogError("the mod list is too long (%d bytes, at most %d)" % (len(raw), V3_LIST_MAX - 1))
    return raw + bytes(size - len(raw))


def parse_handicap(text):
    """--handicap 'R,B' -> (R, B), each 0 (stock) or 1..10."""
    try:
        parts = [int(x, 0) for x in text.split(",")]
    except ValueError:
        parts = []
    if len(parts) != 2 or not all(0 <= v <= HANDICAP_MAX for v in parts):
        raise LogError("bad --handicap %r (want R,B with each 0 = stock or 1 to %d, e.g. 3,7)"
                       % (text, HANDICAP_MAX))
    return tuple(parts)


def needs_v5(ch_stage, handicap):
    """True when the log needs version 5: a Challenge course or non-stock handicaps."""
    return ch_stage is not None or (handicap is not None and tuple(handicap) != (0, 0))


def synth_bytes(seed, synth_seed, ticks, vs_stage=None, version=None,
                tick_order=TICK_ORDER_UPDATE, sim_k=None, events=None,
                mods_hash=None, mods_list="", content_hash=None, players=STOCK_PLAYERS,
                ch_stage=None, handicap=None, online=False):
    """A synthetic log. events: {checksum tick T: code}; the code goes into byte 11 of port 0's
    slot in record T-1, so it applies in the pass whose checksum tick is T. players: 2 or 4; port p
    (p < players) comes from PortGen(synth_seed, p), so ports 0 and 1 do not depend on the count.
    ch_stage: 2P Challenge course 0..29 (start mode 2, 2 players, version 5). handicap: (Red, Blue),
    0 = stock (non-stock needs version 5). online: set flags bit 1.
    version None: 5 with a course or non-stock handicaps, else 3, or 4 with more than 2 players."""
    if players not in PLAYER_COUNTS:
        raise LogError("bad player count %r (2 or 4; 3 teams are not supported yet)" % (players,))
    handicap = (0, 0) if handicap is None else tuple(handicap)
    if len(handicap) != 2 or not all(isinstance(v, int) and 0 <= v <= HANDICAP_MAX for v in handicap):
        raise LogError("bad handicaps %r (0 = stock or 1 to %d each)" % (handicap, HANDICAP_MAX))
    if ch_stage is not None:
        if vs_stage is not None:
            raise LogError("--ch-stage and --vs-stage exclude each other")
        if not 0 <= ch_stage < CH_COURSES:
            raise LogError("bad Challenge course %d (0 to %d)" % (ch_stage, CH_COURSES - 1))
        if players != STOCK_PLAYERS:
            raise LogError("--ch-stage is 2P Challenge: --players %d is not allowed" % players)
        if handicap != (0, 0):
            raise LogError("--handicap is for Battle, not with --ch-stage")
    if version is None:
        if needs_v5(ch_stage, handicap):
            version = 5
        else:
            version = 4 if players > STOCK_PLAYERS else VERSION
    if ticks < 0 or ticks >= OPEN_TICKS:
        raise LogError("bad tick count %d" % ticks)
    if version not in VERSIONS:
        raise LogError("bad format version %d (1 to 5)" % version)
    if players > STOCK_PLAYERS and version < 4:
        raise LogError("%d players need format version 4" % players)
    if needs_v5(ch_stage, handicap) and version < 5:
        raise LogError("--ch-stage and --handicap need format version 5")
    if online and version < 3:
        raise LogError("the online flag needs format version 3")
    mods_hash = bytes(HASH_SIZE) if mods_hash is None else bytes(mods_hash)
    content_hash = bytes(HASH_SIZE) if content_hash is None else bytes(content_hash)
    if len(mods_hash) != HASH_SIZE or len(content_hash) != HASH_SIZE:
        raise LogError("the mods and content hashes are 32 bytes")
    if version < 3 and (mods_hash != bytes(HASH_SIZE) or mods_list
                        or content_hash != bytes(HASH_SIZE)):
        raise LogError("--mods-hash and --mods-list need format version 3")
    events = dict(events or {})
    if version == 1:
        if events:
            raise LogError("--event needs format version 2")
        if tick_order != TICK_ORDER_UPDATE or sim_k:
            raise LogError("--tick-order and --sim-k need format version 2")
    if tick_order not in TICK_ORDER_NAMES:
        raise LogError("bad tick order %r" % tick_order)
    kbits = sim_k_bits(sim_k)
    for t, c in events.items():
        if not 1 <= t <= ticks:
            raise LogError("event tick %d outside the log (1 to %d)" % (t, ticks))
        if not 1 <= c <= players:
            raise LogError("bad event code %d (1 to %d with %d players)" % (c, players, players))
    if ch_stage is not None:
        mode, stage = START_CH, ch_stage
    elif vs_stage is None:
        mode, stage = START_BOOT, STAGE_NONE
    else:
        if not 0 <= vs_stage < STAGE_NONE:
            raise LogError("bad VS stage %d" % vs_stage)
        mode, stage = START_VS, vs_stage
    flags = FLAG_SYNTHETIC | (FLAG_ONLINE if online else 0)
    if version == 1:
        header = struct.pack(HEADER_FMT, MAGIC, 1, HEADER_SIZE, seed & M32,
                             ticks, 0, SLOTS, RECORD_SIZE, mode, stage,
                             FLAG_SYNTHETIC)
    elif version == 2:
        header = struct.pack(HEADER_V2_FMT, MAGIC, version, HEADER_SIZE, seed & M32,
                             ticks, 0, SLOTS, RECORD_SIZE, mode, stage,
                             FLAG_SYNTHETIC, tick_order, kbits, 0)
    else:
        area = v3_list_area(mods_list)
        header = bytearray(struct.pack(HEADER_V2_FMT, MAGIC, version, V3_FIXED_SIZE + len(area), seed & M32,
                                       ticks, 0, SLOTS, RECORD_SIZE, mode, stage,
                                       flags, tick_order, kbits, 0) + mods_hash + content_hash + area)
        if version >= 4:
            header[PLAYERS_BYTE] = players
        if version >= 5:
            header[HANDICAP_BYTES[0]], header[HANDICAP_BYTES[1]] = handicap
        header = bytes(header)
    gens = [PortGen(synth_seed, p) for p in range(players)]
    absent = Slot.absent().pack()
    out = [header]
    for t in range(ticks):
        s0 = gens[0].step()
        s0.event = events.get(t + 1, 0)
        out.append(s0.pack())
        for p in range(1, SLOTS):
            out.append(gens[p].step().pack() if p < players else absent)
    return b"".join(out)


# --------------------------------------------------------------------------
# Checksum log format
# --------------------------------------------------------------------------

# The columns of version 1 (version 2 adds cam= after mode=). Only the order of print in stats
# uses this list; every other key=value of a line is handled generically.
HASH_FIELDS = ["rng", "navi", "piki", "enemy", "pellet", "time", "mode", "cam"]
NON_HASH_KEYS = ("n", "lines")
COUNT_NAMES = ["navi", "piki", "enemy", "pellet"]


def parse_cksum_line(line):
    """Parse one non-comment line. Returns a dict, or None if malformed."""
    toks = line.split()
    if len(toks) < 3 or toks[0] not in ("C", "L", "E"):
        return None
    try:
        tick = int(toks[1])
    except ValueError:
        return None
    d = {"kind": toks[0], "tick": tick, "total": toks[2], "fields": {}}
    for t in toks[3:]:
        if "=" in t:
            k, v = t.split("=", 1)
            d["fields"][k] = v
    return d


def read_cksum(path):
    """Returns (comment lines, data lines) with line endings stripped."""
    comments, lines = [], []
    with open(path, "r", encoding="ascii", errors="replace") as f:
        for raw in f:
            s = raw.rstrip("\r\n")
            if s.startswith("#"):
                comments.append(s)
            elif s.strip() == "":
                continue
            else:
                lines.append(s)
    return comments, lines


def hash_keys(*field_dicts):
    """Hash column names of parsed lines, in file order: every key=value except the counts
    (n) and the E line's lines=, so a new column (version 2's cam=) needs no code change."""
    keys = []
    for fd in field_dicts:
        for k in fd:
            if k not in NON_HASH_KEYS and k not in keys:
                keys.append(k)
    return keys


def cksum_version(comments):
    """The N of the '# mppm-cksum vN' header line, or None."""
    for c in comments:
        t = c.lstrip("#").split()
        if len(t) >= 2 and t[0] == "mppm-cksum" and t[1][:1] == "v":
            try:
                return int(t[1][1:])
            except ValueError:
                return None
    return None


def describe_diff(la, lb):
    """Names of the parts that differ between two checksum lines."""
    pa, pb = parse_cksum_line(la), parse_cksum_line(lb)
    if pa is None or pb is None:
        return ["unparsable line"]
    out = []
    if pa["kind"] != pb["kind"]:
        out.append("kind (%s vs %s)" % (pa["kind"], pb["kind"]))
    if pa["tick"] != pb["tick"]:
        out.append("tick (%d vs %d)" % (pa["tick"], pb["tick"]))
    for k in hash_keys(pa["fields"], pb["fields"]):
        if pa["fields"].get(k) != pb["fields"].get(k):
            out.append(k)
    if pa["fields"].get("n") != pb["fields"].get("n"):
        out.append("counts (%s vs %s)" % (pa["fields"].get("n", "-"),
                                          pb["fields"].get("n", "-")))
    if pa["kind"] == "E" and pa["fields"].get("lines") != pb["fields"].get("lines"):
        out.append("lines")
    if not out and pa["total"] != pb["total"]:
        out.append("total")
    if not out:
        out.append("text only (whitespace or unknown fields)")
    return out


def tick_of(line):
    p = parse_cksum_line(line)
    return "?" if p is None else str(p["tick"])


def cmp_cksum(a_path, b_path, limit=None, out=sys.stdout):
    ca, la = read_cksum(a_path)
    cb, lb = read_cksum(b_path)
    va, vb = cksum_version(ca), cksum_version(cb)
    if va != vb:
        out.write("note: format versions differ (A v%s, B v%s)\n" % (va, vb))
    if limit is not None:
        la, lb = la[:limit], lb[:limit]
    n = min(len(la), len(lb))
    for i in range(n):
        if la[i] != lb[i]:
            out.write("differ at line %d, tick %s\n" % (i + 1, tick_of(la[i])))
            out.write("  A: %s\n" % la[i])
            out.write("  B: %s\n" % lb[i])
            out.write("  fields: %s\n" % ", ".join(describe_diff(la[i], lb[i])))
            return 1
    if len(la) != len(lb):
        longer, name, short = (la, "A", "B") if len(la) > len(lb) else (lb, "B", "A")
        out.write("differ at line %d, tick %s: %s ends after %d lines, %s goes on\n"
                  % (n + 1, tick_of(longer[n]), short, n, name))
        out.write("  %s: %s\n" % (name, longer[n]))
        return 1
    out.write("identical (%d lines)\n" % n)
    return 0


def cksum_stats(path, out=sys.stdout):
    comments, lines = read_cksum(path)
    nC = nL = bad = 0
    last_tick = None
    e_line = None
    distinct = {"total": set()}
    order = ["total"]
    maxc = [0, 0, 0, 0]
    for s in lines:
        p = parse_cksum_line(s)
        if p is None:
            bad += 1
            continue
        if p["kind"] == "E":
            e_line = p
            continue
        last_tick = p["tick"]
        if p["kind"] == "L":
            nL += 1
            continue
        nC += 1
        distinct["total"].add(p["total"])
        for k in hash_keys(p["fields"]):
            if k not in distinct:
                distinct[k] = set()
                order.append(k)
            distinct[k].add(p["fields"][k])
        n = p["fields"].get("n")
        if n:
            parts = n.split("/")
            for j in range(min(4, len(parts))):
                try:
                    maxc[j] = max(maxc[j], int(parts[j]))
                except ValueError:
                    pass
    out.write("file: %s\n" % path)
    for c in comments:
        out.write("header: %s\n" % c.lstrip("# ").rstrip())
    ver = cksum_version(comments)
    out.write("format: %s\n" % ("v%d" % ver if ver else "unknown"))
    out.write("C lines: %d\n" % nC)
    out.write("L lines: %d\n" % nL)
    if bad:
        out.write("malformed lines: %d\n" % bad)
    out.write("last tick: %s\n" % ("-" if last_tick is None else last_tick))
    if e_line is None:
        out.write("E tick: none (log not closed)\n")
    else:
        out.write("E tick: %d (rolling=%s lines=%s)\n"
                  % (e_line["tick"], e_line["total"], e_line["fields"].get("lines", "?")))
    known = [k for k in ["total"] + HASH_FIELDS if k in distinct]
    order = known + [k for k in order if k not in known]
    out.write("distinct: %s\n" % " ".join("%s=%d" % (k, len(distinct[k])) for k in order))
    out.write("max counts: %s\n" % " ".join("%s=%d" % (COUNT_NAMES[j], maxc[j])
                                            for j in range(4)))
    return 0


# --------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------

def fmt_buttons(b):
    s = "".join(ch for bit, ch in BUTTON_LETTERS if b & bit)
    unknown = b & ~sum(bit for bit, _ in BUTTON_LETTERS) & 0xFFFF
    if unknown:
        s += "?%04x" % unknown
    return s or "-"


def fmt_slot(p, s):
    if s.err != 0:
        return "P%d err=%d" % (p, s.err)
    txt = "P%d %-6s st(%+3d,%+3d) c(%+3d,%+3d) tr(%3d,%3d)" % (
        p, fmt_buttons(s.button), s.stickX, s.stickY, s.substickX,
        s.substickY, s.triggerLeft, s.triggerRight)
    if s.analogA or s.analogB:
        txt += " an(%d,%d)" % (s.analogA, s.analogB)
    return txt


def parse_event(text):
    """'T:CODE' -> (T, CODE)."""
    try:
        t, c = text.split(":", 1)
        return int(t, 0), int(c, 0)
    except ValueError:
        raise LogError("bad --event %r (want TICK:CODE, e.g. 2400:2)" % text)


def cmd_synth(a):
    events = {}
    for e in a.event or []:
        t, c = parse_event(e)
        if t in events:
            raise LogError("two events at tick %d" % t)
        events[t] = c
    order = TICK_ORDER_UPDATE if a.tick_order == "update" else TICK_ORDER_DRAW
    if a.format_version == 1 and a.tick_order_given:
        raise LogError("--tick-order needs format version 2")
    if a.mods_list and not a.mods_hash:
        raise LogError("--mods-list needs --mods-hash (a zero hash is stock content)")
    if a.players not in PLAYER_COUNTS:
        raise LogError("bad --players %d (2 or 4; 3 teams are not supported yet)" % a.players)
    handicap = parse_handicap(a.handicap) if a.handicap is not None else (0, 0)
    if a.ch_stage is not None:
        if a.vs_stage is not None:
            raise LogError("--ch-stage (2P Challenge) and --vs-stage (Battle) exclude each other")
        if a.players != STOCK_PLAYERS:
            raise LogError("--ch-stage is 2P Challenge: --players %d is not allowed" % a.players)
        if a.handicap is not None:
            raise LogError("--handicap is for Battle, not with --ch-stage")
    if a.format_version is None:
        if needs_v5(a.ch_stage, handicap):
            a.format_version = 5
        else:
            a.format_version = 4 if a.players > STOCK_PLAYERS else VERSION
    if a.players > STOCK_PLAYERS and a.format_version < 4:
        raise LogError("--players %d needs format version 4 (got --format-version %d)"
                       % (a.players, a.format_version))
    if needs_v5(a.ch_stage, handicap) and a.format_version < 5:
        raise LogError("--ch-stage and --handicap need format version 5 (got --format-version %d)"
                       % a.format_version)
    if a.online and a.format_version < 3:
        raise LogError("--online needs format version 3 or later")
    data = synth_bytes(a.seed, a.synth_seed, a.ticks, a.vs_stage, version=a.format_version,
                       tick_order=order, sim_k=a.sim_k, events=events,
                       mods_hash=parse_mods_hash(a.mods_hash), mods_list=a.mods_list or "",
                       players=a.players, ch_stage=a.ch_stage, handicap=handicap, online=a.online)
    tmp = a.output + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, a.output)
    extra = ""
    if a.format_version >= 2:
        extra = " order=%s sim-k=%s" % (TICK_ORDER_NAMES[order],
                                        "default" if not a.sim_k else "%g" % a.sim_k)
        if events:
            extra += " events=%s" % ",".join("%d:%d" % (t, events[t]) for t in sorted(events))
    if a.format_version >= 3:
        extra += " content=%s" % ("stock" if not a.mods_hash else
                                  "mods %s (%s)" % (a.mods_hash[:8].lower(), a.mods_list or "none"))
    if a.format_version >= 4:
        extra += " players=%d" % a.players
    if a.format_version >= 5 and a.ch_stage is None:
        extra += " handicaps=%d,%d" % handicap
    if a.online:
        extra += " online"
    where = ("course=%d (Challenge)" % a.ch_stage if a.ch_stage is not None
             else "stage=%s" % ("none" if a.vs_stage is None else a.vs_stage))
    print("wrote %s: version %d, %d ticks, %d bytes, seed=%d synth-seed=%d %s%s"
          % (a.output, a.format_version, a.ticks, len(data), a.seed, a.synth_seed, where, extra))
    return 0


def cmd_mutate(a):
    """Copy an input log with port P changed at one tick: A flipped and the main stick mirrored
    (pushed full right when it is neutral), so the leader moves differently whatever it was doing.
    A negative control: its replay must match the original up to that tick and differ after it.
    The whole header (version, tick order, sim k, settings hash; version 3: the hashes and the mod
    list) and every test event are kept."""
    data = bytearray(open(a.file, "rb").read())
    log = read_input_log(a.file)
    if not 0 <= a.tick < log.ticks:
        raise LogError("tick %d outside the log (0 to %d)" % (a.tick, log.ticks - 1))
    if not 0 <= a.port < log.players:
        raise LogError("bad port %d (the log has %d players: 0 to %d)" % (a.port, log.players, log.players - 1))
    hdr = struct.unpack_from("<H", data, 6)[0]
    off = hdr + a.tick * RECORD_SIZE + a.port * SLOT_SIZE
    btn, sx, sy = struct.unpack_from("<Hbb", data, off)
    sx, sy = (-sx, -sy) if (sx or sy) else (MAIN_FULL[0], 0)
    struct.pack_into("<Hbb", data, off, btn ^ BTN_A, sx, sy)
    ticks = log.ticks
    if a.ticks is not None:
        if not a.tick < a.ticks <= log.ticks:
            raise LogError("--ticks must be above the mutated tick and at most %d" % log.ticks)
        ticks = a.ticks
        del data[hdr + ticks * RECORD_SIZE:]
        struct.pack_into("<I", data, 12, ticks)
    tmp = a.output + ".tmp"
    with open(tmp, "wb") as f:
        f.write(bytes(data))
    os.replace(tmp, a.output)
    print("wrote %s: %d ticks, port %d changed at tick %d" % (a.output, ticks, a.port, a.tick))
    return 0


def cmd_dump(a):
    log = read_input_log(a.file)
    print("file:        %s" % a.file)
    print("version:     %d" % log.version)
    print("seed:        %d (0x%08x)" % (log.seed, log.seed))
    print("ticks:       %d%s" % (log.ticks, " (open recording, from file size)"
                                 if log.open else ""))
    print("build id:    %016x%s" % (log.build_id, " (any)" if log.build_id == 0 else ""))
    print("start mode:  %d (%s)" % (log.start_mode, START_MODE_NAMES.get(log.start_mode, "unknown")))
    if log.challenge():
        print("course:      %d" % log.stage)
    else:
        print("vs stage:    %s" % ("none" if log.stage == STAGE_NONE else log.stage))
    print("players:     %d%s" % (log.players, "" if log.version >= 4 else " (version %d: always 2)" % log.version))
    if log.version >= 5:
        print("handicaps:   %s" % fmt_handicap(log.handicap))
    print("flags:       %s" % fmt_flags(log.flags))
    evs = log.events() if log.version >= 2 else []
    if log.version >= 2:
        print("tick order:  %s" % fmt_tick_order(log.tick_order))
        print("sim k:       %s" % fmt_sim_k(log))
        print("settings:    %s" % ("not recorded" if log.settings_hash == 0
                                   else "hash %08x" % log.settings_hash))
        print("events:      %s" % (", ".join("tick %d code %d (%s)" % (t, c, EVENT_NAMES.get(c, "unknown"))
                                             for t, c in evs) or "none"))
    if log.version >= 3:
        print("header size: %d" % log.rec_off)
        if log.stock():
            print("content:     stock")
        else:
            print("mods hash:   %s" % log.mods_hash.hex())
            print("mods:        %s" % (log.mods_list or "none"))
        print("content hash: %s" % (log.content_hash.hex() if log.content_hash != bytes(HASH_SIZE)
                                    else "not recorded"))
    elif log.version == 2:
        print("content:     stock (version 2)")
    if a.events:
        return 0
    start = max(0, a.from_tick)
    end = log.ticks if a.count is None else min(log.ticks, start + a.count)
    for t in range(start, end):
        slots = log.record(t)
        parts = [fmt_slot(p, s) for p, s in enumerate(slots) if s.err == 0]
        line = "%7d  %s" % (t, "  |  ".join(parts) if parts else "(no controller)")
        ev = slots[0].event if log.version >= 2 else 0
        if ev:
            line += "  << event %d at tick %d (%s)" % (ev, t + 1, EVENT_NAMES.get(ev, "unknown"))
        print(line)
    return 0


def cmd_cmp_input(a):
    la, lb = read_input_log(a.a), read_input_log(a.b)
    for name, va, vb in (("seed", la.seed, lb.seed),
                         ("start mode", la.start_mode, lb.start_mode),
                         ("course" if la.challenge() else "vs stage", la.stage, lb.stage),
                         ("captain count", la.players, lb.players),
                         ("handicaps", "%d,%d" % la.handicap, "%d,%d" % lb.handicap)):
        if va != vb:
            print("differ: %s (%s vs %s)" % (name, va, vb))
            return 1
    # Session constants that a replay may override (the A/B cells): noted, not a difference.
    if la.version != lb.version:
        print("note: format versions differ (A %d, B %d)" % (la.version, lb.version))
    if la.tick_order != lb.tick_order:
        print("note: tick order differs (A %s, B %s)" % (fmt_tick_order(la.tick_order),
                                                         fmt_tick_order(lb.tick_order)))
    if la.sim_k_bits != lb.sim_k_bits:
        print("note: sim k differs (A %s, B %s)" % (fmt_sim_k(la), fmt_sim_k(lb)))
    if la.settings_hash != lb.settings_hash:
        print("note: settings hash differs (A %08x, B %08x)" % (la.settings_hash, lb.settings_hash))
    if la.mods_hash != lb.mods_hash:
        print("note: mods hash differs (A %s, B %s)"
              % (fmt_content(la), fmt_content(lb)))
    n = min(la.ticks, lb.ticks)
    for t in range(n):
        if la.record_bytes(t) != lb.record_bytes(t):
            ra, rb = la.record(t), lb.record(t)
            for p in range(SLOTS):
                if ra[p].as_tuple() != rb[p].as_tuple():
                    if ra[p].as_tuple()[:10] == rb[p].as_tuple()[:10]:
                        print("differ at tick %d, port %d: event byte (A %d, B %d)"
                              % (t, p, ra[p].event, rb[p].event))
                        return 1
                    print("differ at tick %d, port %d" % (t, p))
                    print("  A: %s" % fmt_slot(p, ra[p]))
                    print("  B: %s" % fmt_slot(p, rb[p]))
                    return 1
            print("differ at tick %d (unexpected: equal fields, different bytes)" % t)
            return 1
    if la.ticks != lb.ticks:
        print("differ: length (A has %d ticks, B has %d); first %d ticks equal"
              % (la.ticks, lb.ticks, n))
        return 1
    print("identical")
    return 0


def cmd_cmp_cksum(a):
    return cmp_cksum(a.a, a.b, a.limit)


def cmd_stats(a):
    return cksum_stats(a.file)


def cmd_selftest(_a):
    import io

    fails = []

    def check(cond, what):
        print("%s  %s" % ("ok  " if cond else "FAIL", what))
        if not cond:
            fails.append(what)

    base = "/var/tmp" if os.path.isdir("/var/tmp") else None
    with tempfile.TemporaryDirectory(prefix="netlog-selftest-", dir=base) as d:
        p1, p2, p3 = (os.path.join(d, n) for n in ("a.mpnl", "b.mpnl", "c.mpnl"))
        N = 3000
        for p, k in ((p1, 7), (p2, 7), (p3, 8)):
            with open(p, "wb") as f:
                f.write(synth_bytes(20261122, k, N, 0))
        b1, b2, b3 = (open(p, "rb").read() for p in (p1, p2, p3))
        check(b1 == b2, "same arguments give identical bytes")
        check(b1 != b3, "another synth seed gives different bytes")
        check(len(b1) == V3_FIXED_SIZE + V3_LIST_MIN + N * RECORD_SIZE,
              "file size is 144 + 48*ticks (version 3, stock)")
        check(synth_bytes(1, 7, 10)[:HEADER_SIZE] != b1[:HEADER_SIZE],
              "header follows the arguments")

        log = read_input_log(p1)
        check(log.ticks == N and log.seed == 20261122, "parse: tick count and seed")
        check(log.start_mode == 1 and log.stage == 0, "parse: VS start mode and stage")
        check(log.build_id == 0 and log.flags & FLAG_SYNTHETIC, "parse: build 0, synthetic")
        start_seen = False
        active = [set(), set()]
        clamp_ok = True
        absent_ok = True
        for t in range(N):
            r = log.record(t)
            for p in (0, 1):
                s = r[p]
                if s.button & BTN_START:
                    start_seen = True
                if s.err != 0 or s.analogA or s.analogB:
                    clamp_ok = False
                for v, (ax, dg) in (((s.stickX, s.stickY), MAIN_FULL),
                                    ((s.substickX, s.substickY), CSTICK)):
                    if v[0] and v[1]:
                        clamp_ok &= abs(v[0]) <= dg and abs(v[1]) <= dg
                    else:
                        clamp_ok &= abs(v[0]) <= ax and abs(v[1]) <= ax
                clamp_ok &= s.triggerLeft <= 150 and s.triggerRight <= 150
                active[p].update(bit for bit, _ in BUTTON_LETTERS if s.button & bit)
                if s.stickX or s.stickY:
                    active[p].add("stick")
                if s.substickX or s.substickY:
                    active[p].add("cstick")
            for p in (2, 3):
                absent_ok &= r[p].as_tuple() == Slot.absent().as_tuple()
        check(not start_seen, "START never pressed")
        check(clamp_ok, "values within the post-clamp ranges, ports 0/1 present")
        check(absent_ok, "ports 2 and 3 absent")
        for p in (0, 1):
            check({BTN_A, BTN_B, "stick", "cstick"} <= active[p],
                  "port %d uses A, B, stick and C-stick" % p)

        # cmp-input on the logs, and on an open recording of the same records
        sink = io.StringIO()
        old = sys.stdout
        try:
            sys.stdout = sink
            r_same = cmd_cmp_input(argparse.Namespace(a=p1, b=p2))
            r_diff = cmd_cmp_input(argparse.Namespace(a=p1, b=p3))
            op = os.path.join(d, "open.mpnl")
            hdr = bytearray(b1[:HEADER_SIZE])
            struct.pack_into("<IQ", hdr, 12, OPEN_TICKS, 0x1234567890ABCDEF)
            struct.pack_into("<I", hdr, 28, 0)
            with open(op, "wb") as f:
                f.write(bytes(hdr) + b1[HEADER_SIZE:])
            r_open = cmd_cmp_input(argparse.Namespace(a=p1, b=op))
        finally:
            sys.stdout = old
        check(r_same == 0, "cmp-input: identical logs")
        check(r_diff == 1 and "differ at tick" in sink.getvalue(),
              "cmp-input: different logs name a tick")
        check(r_open == 0, "cmp-input: ignores build id, flags, open tick count")

        mp = os.path.join(d, "mut.mpnl")
        sink = io.StringIO()
        old = sys.stdout
        try:
            sys.stdout = sink
            cmd_mutate(argparse.Namespace(file=p1, tick=1500, port=1, ticks=2000, output=mp))
            r_mut = cmd_cmp_input(argparse.Namespace(a=p1, b=mp))
        finally:
            sys.stdout = old
        ml = read_input_log(mp)
        check(ml.ticks == 2000 and r_mut == 1 and "differ at tick 1500, port 1" in sink.getvalue(),
              "mutate: one tick of one port changed, cut to --ticks")

        # checksum logs
        def cline(t, piki="33333333", n="2/40/10/5"):
            return ("C %d 0000%04x rng=11111111 navi=%08x piki=%s enemy=44444444 "
                    "pellet=55555555 time=%08x mode=77777777 n=%s\n"
                    % (t, t, 0x22220000 + t, piki, t, n))

        hdr_lines = "# mppm-cksum v1 seed=20261122 interval=30\n# build=0000000000000000\n"
        body = [cline(t) for t in range(0, 300, 30)]
        body.insert(0, "L 0 deadbeef rng=11111111\n")
        ca, cb, cc, cd = (os.path.join(d, n + ".cksum") for n in "abcd")
        with open(ca, "w") as f:
            f.write(hdr_lines + "".join(body) + "E 300 cafebabe lines=12\n")
        with open(cb, "w") as f:
            f.write("# mppm-cksum v1 seed=20261122 interval=30\n# build=00000000000000ff\n"
                    + "".join(body) + "E 300 cafebabe lines=12\n")
        mod = list(body)
        mod[5] = cline(120, piki="3333ffff")
        with open(cc, "w") as f:
            f.write(hdr_lines + "".join(mod) + "E 300 cafebabe lines=12\n")
        with open(cd, "w") as f:
            f.write(hdr_lines + "".join(body[:6]))

        def run(fn, *args, **kw):
            s = io.StringIO()
            rc = fn(*args, out=s, **kw)
            return rc, s.getvalue()

        rc, txt = run(cmp_cksum, ca, cb)
        check(rc == 0 and "identical (12 lines)" in txt, "cmp-cksum: ignores # lines")
        rc, txt = run(cmp_cksum, ca, cc)
        check(rc == 1 and "tick 120" in txt and "fields: piki" in txt,
              "cmp-cksum: names tick 120 and piki")
        rc, txt = run(cmp_cksum, ca, cd)
        check(rc == 1 and "ends after 6 lines" in txt, "cmp-cksum: prefix is a difference")
        rc, txt = run(cmp_cksum, ca, cd, limit=6)
        check(rc == 0 and "identical (6 lines)" in txt, "cmp-cksum: --limit on common part")
        rc, txt = run(cmp_cksum, ca, cc, limit=5)
        check(rc == 0, "cmp-cksum: --limit stops before the difference")
        rc, txt = run(cksum_stats, ca)
        check(rc == 0 and "C lines: 10" in txt and "L lines: 1" in txt
              and "E tick: 300" in txt and "last tick: 270" in txt
              and "navi=10" in txt and "piki=1 " in txt and "piki=40" in txt,
              "stats: counts, ticks, distinct values, max counts")
        check("format: v1" in txt, "stats: names the v1 format")

        # ---- version 2 checksum lines (cam= between mode= and n=)
        def cline2(t, cam=None):
            return ("C %d 0000%04x rng=11111111 navi=%08x piki=33333333 enemy=44444444 "
                    "pellet=55555555 time=%08x mode=77777777 cam=%08x n=2/40/10/5\n"
                    % (t, t, 0x22220000 + t, t, 0x88880000 + (t if cam is None else cam)))
        hdr2 = "# mppm-cksum v2 seed=20261122 interval=30\n# build=0000000000000000\n"
        body2 = [cline2(t) for t in range(30, 300, 30)]
        v2a, v2b = os.path.join(d, "v2a.cksum"), os.path.join(d, "v2b.cksum")
        with open(v2a, "w") as f:
            f.write(hdr2 + "".join(body2) + "E 300 cafebabe lines=10\n")
        mod2 = list(body2)
        mod2[3] = cline2(120, cam=0xffff)
        with open(v2b, "w") as f:
            f.write(hdr2 + "".join(mod2) + "E 300 cafebabe lines=10\n")
        rc, txt = run(cmp_cksum, v2a, v2b)
        check(rc == 1 and "tick 120" in txt and "fields: cam" in txt,
              "cmp-cksum v2: names the cam field")
        rc, txt = run(cmp_cksum, v2a, v2a)
        check(rc == 0 and "identical (10 lines)" in txt and "note:" not in txt,
              "cmp-cksum v2: identical files")
        rc, txt = run(cmp_cksum, ca, v2a)
        check(rc == 1 and "format versions differ" in txt, "cmp-cksum: notes a v1/v2 pair")
        rc, txt = run(cksum_stats, v2b)
        check(rc == 0 and "format: v2" in txt and " cam=9" in txt and "C lines: 9" in txt
              and "navi=9" in txt, "stats v2: format and cam column")

        # ---- input log version 2
        v1 = synth_bytes(20261122, 7, 400, 0, version=1)
        v2 = synth_bytes(20261122, 7, 400, 0, version=2)
        check(struct.unpack_from("<H", v1, 4)[0] == 1 and v1[32:64] == bytes(32),
              "synth v1: version 1, bytes 32..63 zero")
        check(struct.unpack_from("<H", v2, 4)[0] == 2 and v2[32] == TICK_ORDER_UPDATE
              and v2[33:64] == bytes(31), "synth v2: version 2, update-first, sim k 0, hash 0")
        check(v1[HEADER_SIZE:] == v2[HEADER_SIZE:], "synth v1 and v2: same records")
        vd = synth_bytes(20261122, 7, 400, 0, version=2, tick_order=TICK_ORDER_DRAW, sim_k=1.5,
                         events={100: 2, 400: 1})
        pv = os.path.join(d, "v2.mpnl")
        with open(pv, "wb") as f:
            f.write(vd)
        lv = read_input_log(pv)
        check(lv.version == 2 and lv.tick_order == TICK_ORDER_DRAW and lv.sim_k() == 1.5
              and lv.settings_hash == 0, "parse v2: tick order, sim k, settings hash")
        check(lv.events() == [(100, 2), (400, 1)] and lv.event(99) == 2 and lv.event(100) == 0,
              "events: code in record T-1, port 0 byte 11")
        check(all(lv.record(t)[1].event == 0 for t in range(400)),
              "events: port 1 byte 11 stays zero")
        evonly = bytearray(vd)
        for t in (99, 399):
            evonly[HEADER_SIZE + t * RECORD_SIZE + SLOT_SIZE - 1] = 0
        check(bytes(evonly[HEADER_SIZE:]) == v2[HEADER_SIZE:], "events change nothing else")
        for bad in (dict(events={0: 1}), dict(events={401: 1}), dict(events={5: 0}),
                    dict(events={5: 256}), dict(version=1, events={5: 1}),
                    dict(version=1, tick_order=TICK_ORDER_DRAW), dict(sim_k=0.5),
                    dict(sim_k=4.0), dict(version=6), dict(version=2, mods_hash=b"\1" * 32),
                    dict(version=2, mods_list="a@1"), dict(mods_list="x" * 1024),
                    dict(mods_list="a\0b"), dict(mods_hash=b"\1" * 31)):
            try:
                synth_bytes(1, 7, 400, 0, **bad)
                ok = False
            except LogError:
                ok = True
            check(ok, "synth refuses %s" % bad)

        sink = io.StringIO()
        old = sys.stdout
        try:
            sys.stdout = sink
            cmd_dump(argparse.Namespace(file=pv, from_tick=98, count=3, events=False))
        finally:
            sys.stdout = old
        dt = sink.getvalue()
        check("tick order:  draw-first" in dt and "sim k:       1.5" in dt
              and "tick 100 code 2" in dt and "<< event 2 at tick 100" in dt
              and dt.count("<< event") == 1, "dump: header fields and the event mark")

        # mutate keeps the header and the events; cmp-input on events and session constants
        pm = os.path.join(d, "v2mut.mpnl")
        pe = os.path.join(d, "v2ev.mpnl")
        po = os.path.join(d, "v2order.mpnl")
        p1v = os.path.join(d, "v1.mpnl")
        with open(pe, "wb") as f:
            f.write(synth_bytes(20261122, 7, 400, 0, version=2, tick_order=TICK_ORDER_DRAW,
                                sim_k=1.5, events={100: 1, 400: 1}))
        with open(po, "wb") as f:
            f.write(synth_bytes(20261122, 7, 400, 0, version=2, events={100: 2, 400: 1}))
        with open(p1v, "wb") as f:
            f.write(v1)
        p2v = os.path.join(d, "v2plain.mpnl")
        with open(p2v, "wb") as f:
            f.write(v2)
        sink = io.StringIO()
        old = sys.stdout
        try:
            sys.stdout = sink
            cmd_mutate(argparse.Namespace(file=pv, tick=99, port=0, ticks=300, output=pm))
            sink.truncate(0)
            sink.seek(0)
            r_mut = cmd_cmp_input(argparse.Namespace(a=pv, b=pm))
            t_mut = sink.getvalue()
            sink.truncate(0)
            sink.seek(0)
            r_ev = cmd_cmp_input(argparse.Namespace(a=pv, b=pe))
            t_ev = sink.getvalue()
            sink.truncate(0)
            sink.seek(0)
            r_ord = cmd_cmp_input(argparse.Namespace(a=pv, b=po))
            t_ord = sink.getvalue()
            sink.truncate(0)
            sink.seek(0)
            r_ver = cmd_cmp_input(argparse.Namespace(a=p1v, b=p2v))
            t_ver = sink.getvalue()
        finally:
            sys.stdout = old
        lm = read_input_log(pm)
        mb = open(pm, "rb").read()
        check(lm.ticks == 300 and mb[32:64] == vd[32:64] and lm.events() == [(100, 2)],
              "mutate v2: header fields and the event kept")
        check(r_mut == 1 and "differ at tick 99, port 0" in t_mut, "cmp-input v2: mutated tick")
        check(r_ev == 1 and "event byte (A 2, B 1)" in t_ev and "differ at tick 99" in t_ev,
              "cmp-input: an event difference is a difference")
        check(r_ord == 0 and "note: tick order differs" in t_ord and "note: sim k differs" in t_ord
              and "identical" in t_ord, "cmp-input: tick order and sim k are only noted")
        check(r_ver == 0 and "format versions differ" in t_ver, "cmp-input: v1 and v2 records compare")

        # ---- input log version 3 (M5.2): mods hash, content hash, mod list
        mh = bytes(range(1, 33))
        ch = bytes(range(100, 132))
        s3 = synth_bytes(20261122, 7, 400, 0, version=3)
        m3 = synth_bytes(20261122, 7, 400, 0, version=3, mods_hash=mh, mods_list="a@1.0.0,b@2",
                         content_hash=ch)
        check(struct.unpack_from("<HH", s3, 4) == (3, 144) and s3[64:144] == bytes(80),
              "synth v3 stock: version 3, header size 144, hashes and list zero")
        check(s3[32:64] == v2[32:64] and s3[144:] == v2[HEADER_SIZE:],
              "synth v3 and v2: same v2 fields and records")
        check(struct.unpack_from("<H", m3, 6)[0] == 144 and m3[64:96] == mh and m3[96:128] == ch
              and m3[128:144] == b"a@1.0.0,b@2" + bytes(5), "synth v3 mods: hashes and list")
        long_list = ",".join("mod%03d@1.0.0" % i for i in range(70))  # 909 bytes -> 912
        ml3 = synth_bytes(1, 7, 10, 0, mods_hash=mh, mods_list=long_list)
        check(struct.unpack_from("<H", ml3, 6)[0] == 128 + 912, "synth v3: list area padded to 16")
        p3s, p3m, p3l = (os.path.join(d, n) for n in ("v3s.mpnl", "v3m.mpnl", "v3l.mpnl"))
        for p, b in ((p3s, s3), (p3m, m3), (p3l, ml3)):
            with open(p, "wb") as f:
                f.write(b)
        l3s, l3m, l3l = read_input_log(p3s), read_input_log(p3m), read_input_log(p3l)
        check(l3s.version == 3 and l3s.stock() and l3s.mods_list == "" and l3s.rec_off == 144
              and l3s.ticks == 400, "parse v3 stock: round trip")
        check(not l3m.stock() and l3m.mods_hash == mh and l3m.content_hash == ch
              and l3m.mods_list == "a@1.0.0,b@2" and l3m.record_bytes(399) == l3s.record_bytes(399),
              "parse v3 mods: round trip, records at the header size")
        check(l3l.mods_list == long_list and l3l.ticks == 10, "parse v3: long list round trip")
        check(read_input_log(p2v).stock(), "parse v2: stock content")
        # bad headers: size out of range, a list without a NUL
        for what, patch in (("header size 128", lambda b: struct.pack_into("<H", b, 6, 128)),
                            ("header size 1168", lambda b: struct.pack_into("<H", b, 6, 1168)),
                            ("header size 64", lambda b: struct.pack_into("<H", b, 6, 64))):
            bb = bytearray(m3)
            patch(bb)
            pb = os.path.join(d, "bad.mpnl")
            with open(pb, "wb") as f:
                f.write(bytes(bb))
            try:
                read_input_log(pb)
                ok = False
            except LogError as e:
                ok = "bad header size" in str(e)
            check(ok, "parse v3 refuses %s" % what)
        bb = bytearray(m3)
        bb[128:144] = b"a@1.0.0,b@2.0.0x"
        pb = os.path.join(d, "nonul.mpnl")
        with open(pb, "wb") as f:
            f.write(bytes(bb))
        try:
            read_input_log(pb)
            ok = False
        except LogError as e:
            ok = "no NUL" in str(e)
        check(ok, "parse v3 refuses a mod list without a NUL")
        sink = io.StringIO()
        old = sys.stdout
        try:
            sys.stdout = sink
            cmd_dump(argparse.Namespace(file=p3m, from_tick=0, count=1, events=False))
            d3m = sink.getvalue()
            sink.truncate(0)
            sink.seek(0)
            cmd_dump(argparse.Namespace(file=p3s, from_tick=0, count=1, events=False))
            d3s = sink.getvalue()
            sink.truncate(0)
            sink.seek(0)
            r_mh = cmd_cmp_input(argparse.Namespace(a=p3s, b=p3m))
            t_mh = sink.getvalue()
            sink.truncate(0)
            sink.seek(0)
            r_23 = cmd_cmp_input(argparse.Namespace(a=p2v, b=p3s))
            t_23 = sink.getvalue()
            sink.truncate(0)
            sink.seek(0)
            pm3 = os.path.join(d, "v3mut.mpnl")
            cmd_mutate(argparse.Namespace(file=p3m, tick=50, port=0, ticks=200, output=pm3))
            sink.truncate(0)
            sink.seek(0)
            r_m3 = cmd_cmp_input(argparse.Namespace(a=p3m, b=pm3))
            t_m3 = sink.getvalue()
        finally:
            sys.stdout = old
        check("mods hash:   " + mh.hex() in d3m and "mods:        a@1.0.0,b@2" in d3m
              and "content hash: " + ch.hex() in d3m, "dump v3: mods hash, mods, content hash")
        check("content:     stock" in d3s and "content hash: not recorded" in d3s,
              "dump v3 stock: content stock")
        check(r_mh == 0 and "note: mods hash differs (A stock, B mods 01020304 (a@1.0.0,b@2))" in t_mh,
              "cmp-input: a mods hash difference is noted")
        check(r_23 == 0 and "format versions differ" in t_23 and "mods hash" not in t_23,
              "cmp-input: v2 and v3 stock records compare")
        mb3 = open(pm3, "rb").read()
        check(mb3[:12] == m3[:12] and mb3[16:144] == m3[16:144]
              and struct.unpack_from("<I", mb3, 12)[0] == 200,
              "mutate v3: whole header kept (hashes, list), tick count cut")
        check(r_m3 == 1 and "differ at tick 50, port 0" in t_m3, "cmp-input v3: mutated tick")

        # ---- input log version 4 (M5.3a): byte 33 = captains, ports 0..N-1
        s4 = synth_bytes(20261122, 7, 400, 0, players=4)
        s3p = synth_bytes(20261122, 7, 400, 0)
        check(struct.unpack_from("<HH", s4, 4) == (4, 144) and s4[PLAYERS_BYTE] == 4,
              "synth --players 4: version 4, header size 144, byte 33 = 4")
        check(s4[:4] + s4[6:PLAYERS_BYTE] + s4[PLAYERS_BYTE + 1:144]
              == s3p[:4] + s3p[6:PLAYERS_BYTE] + s3p[PLAYERS_BYTE + 1:144] and s3p[PLAYERS_BYTE] == 0,
              "synth v4 header: the v3 header but version and byte 33")
        same01 = all(s4[144 + t * RECORD_SIZE:144 + t * RECORD_SIZE + 2 * SLOT_SIZE]
                     == s3p[144 + t * RECORD_SIZE:144 + t * RECORD_SIZE + 2 * SLOT_SIZE] for t in range(400))
        check(same01, "synth --players 4: ports 0 and 1 equal the 2-player synth")
        p4 = os.path.join(d, "v4.mpnl")
        with open(p4, "wb") as f:
            f.write(s4)
        l4 = read_input_log(p4)
        check(l4.version == 4 and l4.players == 4 and l4.ticks == 400 and l4.stock() and l4.rec_off == 144,
              "parse v4: round trip, players 4")
        check(read_input_log(p3s).players == 2 and read_input_log(p2v).players == 2, "parse v2/v3: players 2")
        act = [set(), set()]
        pres4 = True
        for t in range(400):
            r = l4.record(t)
            for i, pp in enumerate((2, 3)):
                pres4 &= r[pp].err == 0
                if r[pp].stickX or r[pp].stickY:
                    act[i].add("stick")
                if r[pp].button & BTN_A:
                    act[i].add("A")
        check(pres4 and act[0] == {"stick", "A"} and act[1] == {"stick", "A"},
              "synth --players 4: ports 2 and 3 present and active")
        check(l4.record(10)[2].as_tuple() != l4.record(10)[3].as_tuple()
              or l4.record(11)[2].as_tuple() != l4.record(11)[3].as_tuple(),
              "synth --players 4: ports 2 and 3 differ")
        e4 = synth_bytes(20261122, 7, 400, 0, players=4, events={100: 3, 200: 4, 300: 1})
        pe4 = os.path.join(d, "v4ev.mpnl")
        with open(pe4, "wb") as f:
            f.write(e4)
        le4 = read_input_log(pe4)
        check(le4.events() == [(100, 3), (200, 4), (300, 1)] and le4.players == 4, "v4: events 3 and 4")
        for bp in (2, 0):
            bb = bytearray(s4)
            bb[PLAYERS_BYTE] = bp
            pb = os.path.join(d, "v4p%d.mpnl" % bp)
            with open(pb, "wb") as f:
                f.write(bytes(bb))
            check(read_input_log(pb).players == 2, "parse v4: captains byte %d reads as 2" % bp)
        for bp in (3, 5, 255):
            bb = bytearray(s4)
            bb[PLAYERS_BYTE] = bp
            pb = os.path.join(d, "v4bad.mpnl")
            with open(pb, "wb") as f:
                f.write(bytes(bb))
            try:
                read_input_log(pb)
                ok = False
            except LogError as e:
                ok = "captains byte" in str(e)
            check(ok, "parse v4 refuses captains byte %d" % bp)
        for bad in (dict(players=3), dict(players=1), dict(players=5), dict(players=4, version=3),
                    dict(players=4, version=2), dict(events={5: 3}), dict(events={5: 4}),
                    dict(players=4, events={5: 5})):
            try:
                synth_bytes(1, 7, 400, 0, **bad)
                ok = False
            except LogError:
                ok = True
            check(ok, "synth refuses %s" % bad)
        v4two = synth_bytes(20261122, 7, 400, 0, version=4)
        check(v4two[PLAYERS_BYTE] == 2 and v4two[144:] == s3p[144:], "synth --format-version 4 with 2 players: byte 33 = 2")
        rc_cli = []
        for argv in (["synth", "--seed", "1", "--synth-seed", "7", "--ticks", "10", "--players", "3", "-o", os.path.join(d, "x.mpnl")],
                     ["synth", "--seed", "1", "--synth-seed", "7", "--ticks", "10", "--players", "4",
                      "--format-version", "3", "-o", os.path.join(d, "x.mpnl")]):
            olde = sys.stderr
            try:
                sys.stderr = io.StringIO()
                rc_cli.append(main(argv))
            finally:
                sys.stderr = olde
        check(rc_cli == [2, 2], "synth CLI: --players 3 and --players 4 --format-version 3 exit 2")
        sink = io.StringIO()
        old = sys.stdout
        try:
            sys.stdout = sink
            rc_c4 = main(["synth", "--seed", "20261122", "--synth-seed", "7", "--ticks", "400",
                          "--vs-stage", "0", "--players", "4", "-o", os.path.join(d, "cli4.mpnl")])
            sink.truncate(0)
            sink.seek(0)
            cmd_dump(argparse.Namespace(file=pe4, from_tick=99, count=1, events=False))
            d4 = sink.getvalue()
            sink.truncate(0)
            sink.seek(0)
            r_24 = cmd_cmp_input(argparse.Namespace(a=p3s, b=p4))
            t_24 = sink.getvalue()
            sink.truncate(0)
            sink.seek(0)
            pm4 = os.path.join(d, "v4mut.mpnl")
            cmd_mutate(argparse.Namespace(file=p4, tick=50, port=3, ticks=200, output=pm4))
            sink.truncate(0)
            sink.seek(0)
            r_m4 = cmd_cmp_input(argparse.Namespace(a=p4, b=pm4))
            t_m4 = sink.getvalue()
            sink.truncate(0)
            sink.seek(0)
            pm42 = os.path.join(d, "v4mut2.mpnl")
            cmd_mutate(argparse.Namespace(file=p4, tick=60, port=2, ticks=None, output=pm42))
            sink.truncate(0)
            sink.seek(0)
            r_m42 = cmd_cmp_input(argparse.Namespace(a=p4, b=pm42))
            t_m42 = sink.getvalue()
        finally:
            sys.stdout = old
        check(rc_c4 == 0 and open(os.path.join(d, "cli4.mpnl"), "rb").read() == s4,
              "synth CLI --players 4 equals synth_bytes(players=4)")
        check("version:     4" in d4 and "players:     4" in d4 and "tick 100 code 3 (captain 2 lethal damage)" in d4
              and "P3 " in d4, "dump v4: version, players, event 3, port 3")
        check(r_24 == 1 and "differ: captain count (2 vs 4)" in t_24, "cmp-input: a different captain count fails")
        mb4 = open(pm4, "rb").read()
        check(r_m4 == 1 and "differ at tick 50, port 3" in t_m4 and mb4[PLAYERS_BYTE] == 4
              and struct.unpack_from("<H", mb4, 4)[0] == 4, "mutate --port 3 on v4: header kept, port 3 changed")
        check(r_m42 == 1 and "differ at tick 60, port 2" in t_m42, "mutate --port 2 on v4")
        try:
            cmd_mutate(argparse.Namespace(file=p3s, tick=5, port=2, ticks=None, output=os.path.join(d, "x.mpnl")))
            ok = False
        except LogError:
            ok = True
        check(ok, "mutate refuses port 2 of a 2-player log")

        # ---- input log version 5 (M5.3b): Challenge start mode, handicaps, online flag
        def quiet(fn, *args):
            s5 = io.StringIO()
            old5 = sys.stdout
            olde5 = sys.stderr
            try:
                sys.stdout = s5
                sys.stderr = s5
                rc5 = fn(*args)
            finally:
                sys.stdout = old5
                sys.stderr = olde5
            return rc5, s5.getvalue()

        c5 = synth_bytes(20261122, 7, 400, ch_stage=0)
        check(struct.unpack_from("<HH", c5, 4) == (5, 144) and c5[26] == START_CH and c5[27] == 0
              and c5[PLAYERS_BYTE] == 2 and c5[34:36] == b"\0\0" and c5[28] == FLAG_SYNTHETIC,
              "synth --ch-stage 0: version 5, start mode 2, course 0, 2 captains, stock handicaps")
        check(c5[:26] == s3p[:4] + b"\5\0" + s3p[6:26] and c5[144:] == s3p[144:],
              "synth --ch-stage: the 2-player records and the v3 fields")
        c17 = synth_bytes(20261122, 7, 400, ch_stage=17)
        check(c17[27] == 17 and c17[144:] == c5[144:], "synth --ch-stage 17: course byte only")
        h5 = synth_bytes(20261122, 7, 400, 0, handicap=(3, 10))
        check(struct.unpack_from("<H", h5, 4)[0] == 5 and h5[26] == START_VS and h5[27] == 0
              and h5[34:36] == bytes([3, 10]) and h5[144:] == s3p[144:],
              "synth --handicap 3,10: version 5, VS stage 0, bytes 34/35")
        h4p = synth_bytes(20261122, 7, 400, 0, handicap=(1, 2), players=4)
        check(struct.unpack_from("<H", h4p, 4)[0] == 5 and h4p[PLAYERS_BYTE] == 4 and h4p[144:] == s4[144:],
              "synth --handicap with --players 4: version 5, 4 captains")
        hs = synth_bytes(20261122, 7, 400, 0, handicap=(0, 0))
        check(hs == s3p, "synth --handicap 0,0 (stock) stays version 3 byte for byte")
        v5plain = synth_bytes(20261122, 7, 400, 0, version=5)
        check(struct.unpack_from("<H", v5plain, 4)[0] == 5 and v5plain[PLAYERS_BYTE] == 2
              and v5plain[34:36] == b"\0\0", "synth --format-version 5 with nothing else: stock fields")
        on3 = synth_bytes(20261122, 7, 400, 0, online=True)
        check(struct.unpack_from("<H", on3, 4)[0] == 3 and struct.unpack_from("<I", on3, 28)[0]
              == FLAG_SYNTHETIC | FLAG_ONLINE and on3[144:] == s3p[144:],
              "synth --online: version 3 with flags bit 1")
        pc5, ph5, ph4, po5, pon = (os.path.join(d, n) for n in ("c5.mpnl", "h5.mpnl", "h4.mpnl", "o5.mpnl", "on3.mpnl"))
        o5 = synth_bytes(20261122, 7, 400, ch_stage=29, online=True)
        for pth, b in ((pc5, c5), (ph5, h5), (ph4, h4p), (po5, o5), (pon, on3)):
            with open(pth, "wb") as f:
                f.write(b)
        lc5, lh5, lh4, lo5, lon = (read_input_log(x) for x in (pc5, ph5, ph4, po5, pon))
        check(lc5.version == 5 and lc5.challenge() and lc5.stage == 0 and lc5.players == 2
              and lc5.handicap == (0, 0) and lc5.ticks == 400 and lc5.rec_off == 144,
              "parse v5 Challenge: round trip")
        check(lh5.version == 5 and not lh5.challenge() and lh5.stage == 0 and lh5.handicap == (3, 10),
              "parse v5 handicaps: round trip")
        check(lh4.players == 4 and lh4.handicap == (1, 2), "parse v5 with 4 captains: round trip")
        check(lo5.stage == 29 and lo5.flags & FLAG_ONLINE and lo5.flags & FLAG_SYNTHETIC,
              "parse v5: course 29 and the online flag")
        check(lon.version == 3 and lon.flags & FLAG_ONLINE and lon.handicap == (0, 0),
              "parse v3 with the online flag; handicaps read as stock below version 5")
        # bad version 5 headers
        for what, patch, msg in (("handicap 11", lambda b: b.__setitem__(34, 11), "handicaps"),
                                 ("course 30", lambda b: b.__setitem__(27, 30), "Challenge course"),
                                 ("Challenge with 4 captains", lambda b: b.__setitem__(PLAYERS_BYTE, 4), "2 captains"),
                                 ("start mode 3", lambda b: b.__setitem__(26, 3), "start mode 3"),
                                 ("start mode 2 in version 4", lambda b: struct.pack_into("<H", b, 4, 4), "needs version 5")):
            bb = bytearray(c5)
            patch(bb)
            pb = os.path.join(d, "bad5.mpnl")
            with open(pb, "wb") as f:
                f.write(bytes(bb))
            try:
                read_input_log(pb)
                ok = False
            except LogError as e:
                ok = msg in str(e)
            check(ok, "parse v5 refuses %s" % what)
        for bad in (dict(ch_stage=0, players=4), dict(ch_stage=0, vs_stage=0), dict(ch_stage=30),
                    dict(ch_stage=-1), dict(ch_stage=0, handicap=(1, 1)), dict(ch_stage=0, version=4),
                    dict(handicap=(1, 1), version=4), dict(handicap=(11, 1)), dict(handicap=(1,)),
                    dict(handicap=(-1, 0)), dict(online=True, version=2)):
            args5 = dict(bad)
            vs5 = args5.pop("vs_stage", None if "ch_stage" in args5 else 0)
            try:
                synth_bytes(1, 7, 400, vs5, **args5)
                ok = False
            except LogError:
                ok = True
            check(ok, "synth refuses %s" % bad)
        base5 = ["synth", "--seed", "20261122", "--synth-seed", "7", "--ticks", "400"]
        rc_bad = []
        for extra in (["--ch-stage", "0", "--players", "4"], ["--ch-stage", "0", "--vs-stage", "0"],
                      ["--ch-stage", "0", "--handicap", "1,1"], ["--ch-stage", "0", "--format-version", "4"],
                      ["--vs-stage", "0", "--handicap", "1,2", "--format-version", "3"],
                      ["--vs-stage", "0", "--handicap", "1"], ["--vs-stage", "0", "--handicap", "0,11"],
                      ["--ch-stage", "30"]):
            rc_bad.append(quiet(main, base5 + extra + ["-o", os.path.join(d, "x5.mpnl")])[0])
        check(rc_bad == [2] * 8, "synth CLI refuses --ch-stage with --players 4, --vs-stage, --handicap, "
              "version 4; --handicap with version 3, malformed or out of range; course 30 (got %s)" % rc_bad)
        rc_c5, t_c5 = quiet(main, base5 + ["--ch-stage", "0", "-o", os.path.join(d, "cli5.mpnl")])
        rc_h5, t_h5 = quiet(main, base5 + ["--vs-stage", "0", "--handicap", "3,10", "-o", os.path.join(d, "clih5.mpnl")])
        rc_v3, _ = quiet(main, base5 + ["--vs-stage", "0", "-o", os.path.join(d, "cli3.mpnl")])
        check(rc_c5 == 0 and open(os.path.join(d, "cli5.mpnl"), "rb").read() == c5
              and "version 5" in t_c5 and "course=0 (Challenge)" in t_c5,
              "synth CLI --ch-stage 0 equals synth_bytes(ch_stage=0)")
        check(rc_h5 == 0 and open(os.path.join(d, "clih5.mpnl"), "rb").read() == h5 and "handicaps=3,10" in t_h5,
              "synth CLI --handicap 3,10 equals synth_bytes(handicap=(3, 10))")
        check(rc_v3 == 0 and open(os.path.join(d, "cli3.mpnl"), "rb").read() == s3p,
              "synth CLI default stays version 3 byte for byte")
        _, d5 = quiet(cmd_dump, argparse.Namespace(file=pc5, from_tick=0, count=1, events=False))
        _, dh5 = quiet(cmd_dump, argparse.Namespace(file=ph5, from_tick=0, count=1, events=False))
        _, do5 = quiet(cmd_dump, argparse.Namespace(file=po5, from_tick=0, count=1, events=True))
        _, d3x = quiet(cmd_dump, argparse.Namespace(file=p3s, from_tick=0, count=1, events=True))
        check("version:     5" in d5 and "start mode:  2 (Challenge start barrier)" in d5
              and "course:      0" in d5 and "vs stage" not in d5 and "handicaps:   red stock, blue stock" in d5,
              "dump v5 Challenge: start mode, course, handicaps")
        check("start mode:  1 (VS start barrier)" in dh5 and "vs stage:    0" in dh5
              and "handicaps:   red 3, blue 10" in dh5, "dump v5 handicaps")
        check("flags:       0x00000003 (synthetic, online)" in do5 and "course:      29" in do5,
              "dump: the online flag")
        check("handicaps" not in d3x and "flags:       0x00000001 (synthetic)" in d3x,
              "dump v3: no handicap line, flags as before")
        r_same5, _ = quiet(cmd_cmp_input, argparse.Namespace(a=pc5, b=os.path.join(d, "cli5.mpnl")))
        pc17 = os.path.join(d, "c17.mpnl")
        with open(pc17, "wb") as f:
            f.write(c17)
        r_course, t_course = quiet(cmd_cmp_input, argparse.Namespace(a=pc5, b=pc17))
        r_mode, t_mode = quiet(cmd_cmp_input, argparse.Namespace(a=pc5, b=p3s))
        r_hc, t_hc = quiet(cmd_cmp_input, argparse.Namespace(a=p3s, b=ph5))
        r_on, t_on = quiet(cmd_cmp_input, argparse.Namespace(a=p3s, b=pon))
        p5plain = os.path.join(d, "v5plain.mpnl")
        with open(p5plain, "wb") as f:
            f.write(v5plain)
        r_v5, t_v5 = quiet(cmd_cmp_input, argparse.Namespace(a=p3s, b=p5plain))
        check(r_same5 == 0 and "identical" in quiet(cmd_cmp_input, argparse.Namespace(a=pc5, b=pc5))[1],
              "cmp-input v5: identical Challenge logs")
        check(r_course == 1 and "differ: course (0 vs 17)" in t_course, "cmp-input: a different course fails")
        check(r_mode == 1 and "differ: start mode (2 vs 1)" in t_mode, "cmp-input: Challenge against Battle fails")
        check(r_hc == 1 and "differ: handicaps (0,0 vs 3,10)" in t_hc, "cmp-input: different handicaps fail")
        check(r_on == 0 and "identical" in t_on, "cmp-input: the online flag is ignored")
        check(r_v5 == 0 and "format versions differ (A 3, B 5)" in t_v5 and "identical" in t_v5,
              "cmp-input: v3 against stock v5 is only a note")
        pm5 = os.path.join(d, "v5mut.mpnl")
        rc_m5, _ = quiet(cmd_mutate, argparse.Namespace(file=po5, tick=50, port=1, ticks=200, output=pm5))
        mb5 = open(pm5, "rb").read()
        r_m5, t_m5 = quiet(cmd_cmp_input, argparse.Namespace(a=po5, b=pm5))
        check(rc_m5 == 0 and mb5[:12] == o5[:12] and mb5[16:144] == o5[16:144]
              and struct.unpack_from("<I", mb5, 12)[0] == 200 and read_input_log(pm5).challenge(),
              "mutate v5: start mode, course, handicaps and flags kept")
        check(r_m5 == 1 and "differ at tick 50, port 1" in t_m5, "cmp-input v5: mutated tick")
        pmh = os.path.join(d, "v5hmut.mpnl")
        quiet(cmd_mutate, argparse.Namespace(file=ph5, tick=10, port=0, ticks=None, output=pmh))
        check(read_input_log(pmh).handicap == (3, 10), "mutate v5: handicaps kept")

    print("selftest %s" % ("passed" if not fails else "FAILED (%d)" % len(fails)))
    return 0 if not fails else 1


def main(argv=None):
    ap = argparse.ArgumentParser(prog="netlog.py",
                                 description="mppm determinism harness: input and checksum log tools")
    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("synth", help="write a synthetic two- or four-player input log")
    p.add_argument("--seed", type=int, required=True, help="session seed for the header")
    p.add_argument("--synth-seed", type=int, required=True, help="generator seed")
    p.add_argument("--ticks", type=int, required=True)
    p.add_argument("--vs-stage", type=int, default=None)
    p.add_argument("--ch-stage", type=int, default=None, metavar="C",
                   help="2P Challenge course C, 0 to 29: start mode 2, version 5 (M5.3b; not with "
                        "--vs-stage, --players 4 or --handicap)")
    p.add_argument("--handicap", default=None, metavar="R,B",
                   help="VS handicaps of Red and Blue, 0 = stock or 1 to 10; non-stock writes version 5 (M5.3b)")
    p.add_argument("--online", action="store_true",
                   help="set flags bit 1 (recorded in an online session; version 3 or later)")
    p.add_argument("--tick-order", choices=("update", "draw"), default=None,
                   help="header tick order (version 2; default update)")
    p.add_argument("--sim-k", type=float, default=0.0,
                   help="header sim k, 1.0 to 3.6 (version 2; default 0 = engine default 3.6)")
    p.add_argument("--event", action="append", metavar="T:CODE",
                   help="test event CODE in the pass of checksum tick T (version 2; repeatable; "
                        "1 = captain 0 lethal damage, 2 = captain 1, 3 = captain 2, 4 = captain 3; "
                        "CODE at most --players)")
    p.add_argument("--players", type=int, default=STOCK_PLAYERS,
                   help="captains: 2 (default) or 4; 4 writes version 4 and fills ports 0 to 3")
    p.add_argument("--format-version", type=int, choices=VERSIONS, default=None,
                   help="file format version (default %d, 4 with --players 4, 5 with --ch-stage or "
                        "--handicap; 2 for builds before M5.2, 1 for builds before M5.1b)" % VERSION)
    p.add_argument("--mods-hash", default="", metavar="HEX",
                   help="header mods hash, 64 hex digits (version 3; default stock = zero)")
    p.add_argument("--mods-list", default="", metavar="TEXT",
                   help="header mod list 'id@version,...' (version 3; default stock = empty)")
    p.add_argument("-o", "--output", required=True)
    p.set_defaults(fn=cmd_synth)

    p = sub.add_parser("mutate", help="copy an input log with one tick of one port changed")
    p.add_argument("file")
    p.add_argument("--tick", type=int, required=True)
    p.add_argument("--port", type=int, default=0)
    p.add_argument("--ticks", type=int, default=None, help="cut the copy to this many ticks")
    p.add_argument("-o", "--output", required=True)
    p.set_defaults(fn=cmd_mutate)

    p = sub.add_parser("dump", help="print an input log")
    p.add_argument("file")
    p.add_argument("--from", dest="from_tick", type=int, default=0)
    p.add_argument("--count", type=int, default=None)
    p.add_argument("--events", action="store_true", help="print the header and events only")
    p.set_defaults(fn=cmd_dump)

    p = sub.add_parser("cmp-input", help="compare two input logs")
    p.add_argument("a")
    p.add_argument("b")
    p.set_defaults(fn=cmd_cmp_input)

    p = sub.add_parser("cmp-cksum", help="compare two checksum logs")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--limit", type=int, default=None)
    p.set_defaults(fn=cmd_cmp_cksum)

    p = sub.add_parser("stats", help="summarise a checksum log")
    p.add_argument("file")
    p.set_defaults(fn=cmd_stats)

    p = sub.add_parser("selftest", help="run the built-in tests")
    p.set_defaults(fn=cmd_selftest)

    a = ap.parse_args(argv)
    if getattr(a, "cmd", None) == "synth":
        a.tick_order_given = a.tick_order is not None
        if a.tick_order is None:
            a.tick_order = "update"
    if not getattr(a, "fn", None):
        ap.print_help()
        return 2
    try:
        return a.fn(a)
    except (LogError, OSError) as e:
        sys.stderr.write("netlog.py: %s\n" % e)
        return 2
    except BrokenPipeError:
        return 0


if __name__ == "__main__":
    sys.exit(main())
