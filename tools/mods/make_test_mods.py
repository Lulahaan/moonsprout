#!/usr/bin/env python3
"""make_test_mods.py - build the M5.2 test mods from your own Pikmin 2 disc (USA, GPVE01).

Usage:
  make_test_mods.py --disc DISC --out DIR [--use-dtk] [--pellet-evidence auto|checksum|screenshot]

Reads a few text files from the disc (a plain ISO/GCM directly, anything else through
`dtk vfs cp`), changes numbers in them and writes one mods folder per test scenario under DIR,
each with its own load_order.txt, plus DIR/scenarios.json with what tools/mods/mods_test.sh
expects. The edits are text-level: the Shift-JIS bytes and CRLF line ends stay as they are; only
the numbers change.

Everything written comes from your disc or is a tiny hand-written file with no game data. DIR
must be outside the repository (the script refuses otherwise), so no game file can be committed
by accident. An existing DIR is replaced only when an earlier run of this script made it.

Scenarios (folders under DIR):
  stock-empty   the folder exists, no load_order.txt
  arena         mppm-test-vs-arena
  conflict      mppm-test-vs-arena, mppm-test-vs-newfiles (newfiles wins stages.txt)
  conflict-rev  mppm-test-vs-newfiles, mppm-test-vs-arena (arena wins)
  archive       mppm-test-enemyparms, mppm-test-pelletlist
  enemy         mppm-test-enemyparms
  pellet        mppm-test-pelletlist
  pellet-bmd    mppm-test-pelletlist-bmd (the screenshot fallback for the pellet check)
  identity      mppm-test-identity (unchanged bytes)
  bad           every mppm-test-bad-* mod, then mppm-test-vs-arena last; also a folder named
                "Bad Folder Name" and a "-mppm-test-gone" line with no folder (two warnings)
  archive-whole     mppm-test-enemyparms, mppm-test-enemyparms-whole (the whole enemyParms.szs,
                    unchanged, replaces the archive: the inner files of the mod before it drop)
  archive-whole-rev mppm-test-enemyparms-whole, mppm-test-enemyparms (the inner files go on top)

Python 3 standard library only; run with `python3 -I`.
"""

import argparse
import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import disc as disclib  # noqa: E402  (tools/mods/disc.py)

GAME_ID = "GPVE01"
MARKER = ".mppm-test-mods"
VERSION = "1.0.0"

# disc paths below files/ (the reader finds the disc's own spelling)
P_VS_STAGES = "user/Abe/vs/stages.txt"
P_VS1_CAVE = "user/Mukki/mapunits/caveinfo/vs_1_otegaru.txt"
P_VS1_EDIT = "user/Abe/vs/vs_1_otegaru_edit.txt"
P_ENEMYPARMS = "enemy/parm/enemyParms.szs"
P_PELLETLIST = "user/Abe/Pellet/us/pelletlist_us.szs"
# Read at boot by JKRFileCache::findFile, which compares names case-sensitively: the identity mod
# replaces it to prove an override keeps the disc's spelling (fix round 2 of M5.2).
P_PSOUND = "AudioRes/PSound.aaf"
ENEMY_FOLDERS = ("kochappy", "egg", "pelplant")
NEW_CAVE = "mppm_test_arena.txt"
NEW_EDIT = "mppm_test_arena_edit.txt"


class EditError(Exception):
    pass


# --------------------------------------------------------------------------------------------
# text-level edits (bytes in, bytes out; Shift-JIS comments and CRLF untouched)
# --------------------------------------------------------------------------------------------

def sub_exact(data, pattern, repl, count, what):
    """re.sub on bytes that must match exactly `count` times."""
    rx = re.compile(pattern)
    found = len(rx.findall(data))
    if found != count:
        raise EditError("%s: expected %d match(es) of %r, found %d (is this the USA disc?)"
                        % (what, count, pattern, found))
    return rx.sub(repl, data)


def first_block(data, start=0):
    """(begin, end) of the first top-level { ... } block at or after start."""
    b = data.index(b"{", start)
    depth = 0
    for i in range(b, len(data)):
        c = data[i:i + 1]
        if c == b"{":
            depth += 1
        elif c == b"}":
            depth -= 1
            if depth == 0:
                return b, i + 1
    raise EditError("unbalanced braces")


def edit_stage0(data, fn, what):
    """Apply fn to the bytes of stage 0 (the first { } block of stages.txt)."""
    b, e = first_block(data)
    return data[:b] + fn(data[b:e], what) + data[e:]


def scale_number(m, factor, fmt):
    return m.group(1) + (fmt % (float(m.group(2)) * factor)).encode("ascii") + m.group(3)


def arena_stage0(block, what):
    block = sub_exact(block, rb"(\t)3( \t# col0 happa0\r\n)", rb"\g<1>10\g<2>", 1, what + " col0 happa0")
    block = sub_exact(block, rb"(\t)3( \t# col1 happa0\r\n)", rb"\g<1>10\g<2>", 1, what + " col1 happa0")
    block = sub_exact(block, rb"(\t)200\.000000( \t# time\r\n)", rb"\g<1>120.000000\g<2>", 1, what + " time")
    return block


def newfiles_stage0(block, what):
    block = sub_exact(block, rb"(\t)vs_1_otegaru\.txt( \r\n)", b"\\g<1>" + NEW_CAVE.encode() + b"\\g<2>", 1,
                      what + " caveinfo name")
    block = sub_exact(block, rb"(\t)vs_1_otegaru_edit\.txt( \r\n)", b"\\g<1>" + NEW_EDIT.encode() + b"\\g<2>", 1,
                      what + " edit layout name")
    return block


def cave_max_enemies(data, value, what):
    return sub_exact(data, rb"(\t\{f002\} 4 )30( \t)", b"\\g<1>" + str(value).encode() + b"\\g<2>", 1, what)


def cave_fewer_enemies(data, what):
    # {f002} alone changes nothing in the 3600-tick Ls replay (the stage never reaches the cap),
    # so the new caveinfo also lowers two spawn weights; the enemy count then differs from arena's.
    data = cave_max_enemies(data, 12, what + " f002")
    data = sub_exact(data, rb"(\tPelplant )100( \t# weight\r\n)", rb"\g<1>10\g<2>", 1, what + " Pelplant weight")
    return sub_exact(data, rb"(\tKochappy )40( \t# weight\r\n)", rb"\g<1>4\g<2>", 1, what + " Kochappy weight")


def enemy_life_x4(data, what):
    return sub_exact(data, rb"(\t\{fp00\} 4 )([0-9]+\.[0-9]+)( \t)",
                     lambda m: scale_number(m, 4.0, "%.6f"), 1, what)


def pellet_block(data, name, what):
    """(begin, end) of the { } block whose name line is `name`."""
    m = re.search(rb"\tname\t+" + re.escape(name) + rb"\r\n", data)
    if not m:
        raise EditError("%s: no pellet named %s" % (what, name.decode()))
    b = data.rindex(b"{", 0, m.start())
    return first_block(data, b)


def pellet_numbers(data, what, swap_bmd=False):
    for name in (b"number1", b"number5"):
        b, e = pellet_block(data, name, what)
        block = data[b:e]
        w = "%s %s" % (what, name.decode())
        for key in (b"radius", b"p_radius", b"height"):
            block = sub_exact(block, rb"(\t" + key + rb"\t+)([0-9]+\.[0-9]+)(\r\n)",
                              lambda m: scale_number(m, 3.0, "%.1f"), 1, w + " " + key.decode())
        block = sub_exact(block, rb"(\tmax\t+)([0-9]+)(\r\n)",
                          lambda m: m.group(1) + str(int(m.group(2)) + 2).encode() + m.group(3), 1, w + " max")
        if swap_bmd and name == b"number1":
            block = sub_exact(block, rb"(\tbmd\t+)white1\.bmd(\r\n)", rb"\g<1>white2.bmd\g<2>", 1, w + " bmd")
        data = data[:b] + block + data[e:]
    return data


# --------------------------------------------------------------------------------------------
# output
# --------------------------------------------------------------------------------------------

def inside_repo(path):
    real = os.path.realpath(path)
    repo = os.path.realpath(REPO)
    return real == repo or real.startswith(repo + os.sep)


def prepare_out(out):
    if inside_repo(out):
        raise SystemExit("make_test_mods: --out %s is inside the repository (%s); use a folder "
                         "outside it, such as /var/tmp/<you>/mods-test" % (out, REPO))
    if os.path.lexists(out):
        if not os.path.isdir(out) or os.path.islink(out):
            raise SystemExit("make_test_mods: %s exists and is not a folder" % out)
        if os.listdir(out) and not os.path.isfile(os.path.join(out, MARKER)):
            raise SystemExit("make_test_mods: %s is not empty and was not made by this script; "
                             "pick another --out" % out)
        shutil.rmtree(out)
    os.makedirs(out)
    with open(os.path.join(out, MARKER), "w") as f:
        f.write("made by tools/mods/make_test_mods.py; holds data from your own disc; do not share\n")


def write_file(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def manifest(mid, name, files, description, extra=None):
    doc = {
        "id": mid,
        "version": VERSION,
        "name": name,
        "mppm": ">=0.1",
        "author": "mppm tests",
        "description": description,
        "affects": "gameplay",
        "files": files,
    }
    if extra:
        doc.update(extra)
    return doc


def write_mod(mods_dir, spec):
    """spec: {"id", "folder"?, "manifest" (dict or raw bytes), "files": {path: bytes},
    "links": {path: target}, "sparse": {path: size}}"""
    root = os.path.join(mods_dir, spec.get("folder", spec["id"]))
    os.makedirs(root, exist_ok=True)
    m = spec["manifest"]
    raw = m if isinstance(m, bytes) else (json.dumps(m, indent=2) + "\n").encode("utf-8")
    write_file(os.path.join(root, "mod.json"), raw)
    for p, data in spec.get("files", {}).items():
        write_file(os.path.join(root, p), data)
    for p, target in spec.get("links", {}).items():
        full = os.path.join(root, p)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        os.symlink(target, full)
    for p, size in spec.get("sparse", {}).items():
        full = os.path.join(root, p)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.truncate(size)


def write_load_order(mods_dir, ids):
    lines = ['# Moonsprout mod load order: top = lowest priority, bottom = highest. "-" turns a mod off.']
    lines += ids
    with open(os.path.join(mods_dir, "load_order.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def synthetic_stage_list(n):
    """A VS stage list of n blocks made of placeholder text (no game data)."""
    out = ["%d\t# synthetic test list" % n]
    for i in range(n):
        out += ["{", "\t4", "\tmppm_test_%d.txt" % i, "}"]
    return ("\r\n".join(out) + "\r\n").encode("ascii")


# --------------------------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------------------------

def build(a):
    try:
        d = disclib.Disc(a.disc, REPO, use_dtk=a.use_dtk)
    except (OSError, disclib.DiscError) as e:
        raise SystemExit("make_test_mods: cannot read the disc: %s" % e)
    if d.game_id != GAME_ID:
        raise SystemExit("make_test_mods: the disc is %s; these tests need the USA disc (%s)"
                         % (d.game_id or "unknown", GAME_ID))
    src = "dtk %s" % d.dtk if d.dtk else "built-in ISO reader"

    try:
        vs_stages = d.read(P_VS_STAGES)
        vs1_cave = d.read(P_VS1_CAVE)
        vs1_edit = d.read(P_VS1_EDIT)
        enemy = {}
        for folder in ENEMY_FOLDERS:
            inner, data = d.read_inner(P_ENEMYPARMS, folder + "/enemyparm.txt")
            enemy[folder] = (inner, data)
        pel_inner, pel_cfg = d.read_inner(P_PELLETLIST, "numberpellet_config.txt")
        psound = d.read(P_PSOUND)
        enemyparms_szs = d.read(P_ENEMYPARMS)
        rp = d.real_path
        vs_path, cave_path, edit_path = rp(P_VS_STAGES), rp(P_VS1_CAVE), rp(P_VS1_EDIT)
        ep_path, pl_path = rp(P_ENEMYPARMS), rp(P_PELLETLIST)
        ps_path = rp(P_PSOUND)
    except disclib.DiscError as e:
        raise SystemExit("make_test_mods: %s" % e)
    cave_dir = os.path.dirname(cave_path)
    edit_dir = os.path.dirname(edit_path)

    try:
        arena_stages = edit_stage0(vs_stages, arena_stage0, "arena stages.txt")
        arena_cave = cave_max_enemies(vs1_cave, 6, "arena caveinfo f002")
        new_stages = edit_stage0(vs_stages, newfiles_stage0, "newfiles stages.txt")
        new_cave = cave_fewer_enemies(vs1_cave, "newfiles caveinfo")
        enemy_mod = {f: (inner, enemy_life_x4(data, "%s enemyparm fp00" % f)) for f, (inner, data) in enemy.items()}
        pel_mod = pellet_numbers(pel_cfg, "numberpellet_config")
        pel_bmd = pellet_numbers(pel_cfg, "numberpellet_config", swap_bmd=True)
    except (EditError, ValueError) as e:
        raise SystemExit("make_test_mods: %s" % e)

    def arc(archive, inner):
        return "archives/%s/%s" % (archive, inner)

    mods = {}
    files = {"files/" + vs_path: arena_stages, "files/" + cave_path: arena_cave}
    mods["mppm-test-vs-arena"] = {
        "id": "mppm-test-vs-arena", "files": files,
        "manifest": manifest("mppm-test-vs-arena", "Test: Battle arena tuning", sorted(files),
                             "Stage 0: more Pikmin, a shorter timer, fewer enemies."),
    }
    files = {"files/" + vs_path: new_stages,
             "files/%s/%s" % (cave_dir, NEW_CAVE): new_cave,
             "files/%s/%s" % (edit_dir, NEW_EDIT): vs1_edit}
    mods["mppm-test-vs-newfiles"] = {
        "id": "mppm-test-vs-newfiles", "files": files,
        "manifest": manifest("mppm-test-vs-newfiles", "Test: Battle arena from new files", sorted(files),
                             "Stage 0 reads two files that are not on the disc."),
    }
    files = {arc(ep_path, inner): data for inner, data in enemy_mod.values()}
    mods["mppm-test-enemyparms"] = {
        "id": "mppm-test-enemyparms", "files": files,
        "manifest": manifest("mppm-test-enemyparms", "Test: tougher enemies", sorted(files),
                             "Four times the health for three enemies, inside enemyParms.szs."),
    }
    files = {arc(pl_path, pel_inner): pel_mod}
    mods["mppm-test-pelletlist"] = {
        "id": "mppm-test-pelletlist", "files": files,
        "manifest": manifest("mppm-test-pelletlist", "Test: big number pellets", sorted(files),
                             "Larger 1 and 5 pellets that take more carriers."),
    }
    files = {arc(pl_path, pel_inner): pel_bmd}
    mods["mppm-test-pelletlist-bmd"] = {
        "id": "mppm-test-pelletlist-bmd", "files": files,
        "manifest": manifest("mppm-test-pelletlist-bmd", "Test: big number pellets, other model", sorted(files),
                             "As mppm-test-pelletlist, and the 1 pellet uses the 5 pellet's model."),
    }
    files = {"files/" + ep_path: enemyparms_szs}
    mods["mppm-test-enemyparms-whole"] = {
        "id": "mppm-test-enemyparms-whole", "files": files,
        "manifest": manifest("mppm-test-enemyparms-whole", "Test: whole enemyParms.szs", sorted(files),
                             "The disc's own enemyParms.szs as a loose file: replaces the whole archive."),
    }
    k_inner, k_data = enemy["kochappy"]
    files = {"files/" + vs_path: vs_stages, "files/" + ps_path: psound,
             arc(ep_path, k_inner): k_data, arc(pl_path, pel_inner): pel_cfg}
    mods["mppm-test-identity"] = {
        "id": "mppm-test-identity", "files": files,
        "manifest": manifest("mppm-test-identity", "Test: unchanged files", sorted(files),
                             "The disc's own bytes: every override applies and nothing changes."),
    }

    # ---- hand-written bad mods (no game data) ----
    def bad(mid, name, files_list, desc, **kw):
        spec = {"id": mid, "manifest": manifest(mid, name, files_list, desc)}
        spec.update(kw)
        return spec

    note = b"mppm test file\n"
    bads = []

    def add_bad(spec, path, fragment, what, ids=None):
        mods[spec.get("folder", spec["id"])] = spec
        bads.append({"folder": spec.get("folder", spec["id"]), "ids": ids or [spec.get("folder", spec["id"])],
                     "path": path, "fragment": fragment, "what": what})

    add_bad({"id": "mppm-test-bad-json", "manifest": b'{"id": "mppm-test-bad-json", "version": "1.0.0",\n'
                                                     b'  "files": ["files/user/test/a.txt",]\n',
             "files": {"files/user/test/a.txt": note}},
            None, r"json", "malformed JSON")
    add_bad(bad("mppm-test-bad-dotdot", "Bad: dot-dot path", ["files/../outside.txt"], "Path escapes the mod."),
            "files/../outside.txt", r"dot|\.\.|path", "../ path")
    add_bad(bad("mppm-test-bad-abs", "Bad: absolute path", ["/user/abe/vs/stages.txt"], "Leading slash."),
            "/user/abe/vs/stages.txt", r"relative|leading|absolute|/|path", "absolute path")
    add_bad(bad("mppm-test-bad-symlink", "Bad: symlink", ["files/user/test/hostname.txt"], "A link.",
                links={"files/user/test/hostname.txt": "/etc/hostname"}),
            "files/user/test/hostname.txt", r"link", "symlink to /etc/hostname")
    spec = bad("mppm-test-bad-other", "Bad: folder name", ["files/user/test/a.txt"], "Id is not the folder.",
               files={"files/user/test/a.txt": note})
    spec["folder"] = "mppm-test-bad-folder"
    add_bad(spec, None, r"folder|\bid\b", "folder name differs from the id",
            ids=["mppm-test-bad-folder", "mppm-test-bad-other"])
    add_bad(bad("mppm-test-bad-missing", "Bad: missing file", ["files/user/test/missing.txt"], "Not there."),
            "files/user/test/missing.txt", r"missing|not found|no such|does not exist|exist", "listed file missing")
    spec = bad("mppm-test-bad-version", "Bad: needs a newer build", ["files/user/test/a.txt"], "From the future.",
               files={"files/user/test/a.txt": note})
    spec["manifest"]["mppm"] = ">=99.0"
    add_bad(spec, None, r"Needs Moonsprout 99\.0 or newer", 'mppm ">=99.0"')
    add_bad(bad("mppm-test-bad-stages", "Bad: 11 VS stages", ["files/user/abe/vs/stages.txt"], "Too many.",
                files={"files/user/abe/vs/stages.txt": synthetic_stage_list(11)}),
            "files/user/abe/vs/stages.txt", r"11 stages", "11-stage VS list (synthetic)")
    add_bad(bad("mppm-test-bad-huge", "Bad: 70 MiB file", ["files/user/test/huge.bin"], "Too large.",
                sparse={"files/user/test/huge.bin": 70 << 20}),
            "files/user/test/huge.bin", r"MiB|large|limit|size", "70 MiB sparse file")
    add_bad({"id": "mppm-test-bad-depth", "manifest": b"[" * 10000 + b"\n"},
            None, r"deep|depth|nest|level", "depth bomb (10000 [)")

    scen = {
        "stock-empty": [],
        "arena": ["mppm-test-vs-arena"],
        "conflict": ["mppm-test-vs-arena", "mppm-test-vs-newfiles"],
        "conflict-rev": ["mppm-test-vs-newfiles", "mppm-test-vs-arena"],
        "archive": ["mppm-test-enemyparms", "mppm-test-pelletlist"],
        "enemy": ["mppm-test-enemyparms"],
        "pellet": ["mppm-test-pelletlist"],
        "pellet-bmd": ["mppm-test-pelletlist-bmd"],
        "identity": ["mppm-test-identity"],
        "bad": [b["folder"] for b in bads] + ["mppm-test-vs-arena"],
        "archive-whole": ["mppm-test-enemyparms", "mppm-test-enemyparms-whole"],
        "archive-whole-rev": ["mppm-test-enemyparms-whole", "mppm-test-enemyparms"],
    }
    # Not mods of their own: a folder whose name is not an id, and an off line with no folder.
    bad_folder = "Bad Folder Name"
    gone_id = "mppm-test-gone"
    mods[bad_folder] = {"id": "bad-folder-name", "folder": bad_folder,
                        "manifest": manifest("bad-folder-name", "Bad: folder name is not an id",
                                             ["files/user/test/a.txt"], "Spaces and capitals."),
                        "files": {"files/user/test/a.txt": note}}

    out = a.out
    prepare_out(out)
    for name, ids in scen.items():
        mods_dir = os.path.join(out, name)
        os.makedirs(mods_dir)
        for mid in ids:
            write_mod(mods_dir, mods[mid])
        if name == "bad":
            write_mod(mods_dir, mods[bad_folder])
            write_load_order(mods_dir, ["-" + gone_id] + ids)
        elif name != "stock-empty":
            write_load_order(mods_dir, ids)

    vs_key = "/" + vs_path.lower()
    doc = {
        "format": 1,
        "game_id": d.game_id,
        "source": src,
        "version": VERSION,
        "scenarios": {name: {"dir": os.path.join(out, name), "load_order": ids,
                             "load_order_file": name != "stock-empty"} for name, ids in scen.items()},
        "mods": {mid: {"folder": s.get("folder", s["id"]),
                       "files": sorted(list(s.get("files", {})) + list(s.get("links", {})) + list(s.get("sparse", {})))}
                 for mid, s in mods.items()},
        "archives": {"enemyparms": "/" + ep_path.lower(), "pelletlist": "/" + pl_path.lower()},
        "expect": {
            "conflict": ["mppm: mods: conflict %s: mppm-test-vs-newfiles overrides mppm-test-vs-arena" % vs_key],
            "conflict-rev": ["mppm: mods: conflict %s: mppm-test-vs-arena overrides mppm-test-vs-newfiles" % vs_key],
            "identity_applied": ["/" + ep_path.lower(), "/" + pl_path.lower()],
            "bad": bads,
            "bad_good_mod": "mppm-test-vs-arena",
            "bad_warnings": ["mppm: mods: warning %s: -: The folder name is not a mod id" % bad_folder,
                             "mppm: mods: warning %s: -: load_order.txt lists this mod, but its folder is missing." % gone_id],
            "whole": ["mppm: mods: conflict /%s:%s: mppm-test-enemyparms-whole overrides mppm-test-enemyparms "
                      "(mppm-test-enemyparms-whole replaces the whole archive)" % (ep_path.lower(), inner.lower())
                      for inner, _ in sorted(enemy_mod.values())],
            "whole-rev": ["mppm: mods: conflict /%s:%s: mppm-test-enemyparms overrides mppm-test-enemyparms-whole "
                          "(mppm-test-enemyparms-whole replaces the whole archive)" % (ep_path.lower(), inner.lower())
                          for inner, _ in sorted(enemy_mod.values())],
        },
        "pellet": {
            "evidence": a.pellet_evidence,
            "checksum_scenario": "pellet",
            "screenshot_scenario": "pellet-bmd",
            "note": ("auto: mods_test.sh uses the checksum when the pellet run differs from the golden "
                     "log within 3600 ticks, else the tick-60 screenshot pair with pellet-bmd "
                     "(number1 drawn with white2.bmd); it prints which one it used"),
        },
        "edits": {
            "mppm-test-vs-arena": "stages.txt stage 0: col0 happa0 and col1 happa0 3 -> 10, time 200 -> 120; "
                                  "vs_1_otegaru.txt {f002} 30 -> 6",
            "mppm-test-vs-newfiles": "stages.txt stage 0 names %s and %s (new files; caveinfo copy with "
                                     "{f002} 30 -> 12, Pelplant weight 100 -> 10, Kochappy weight 40 -> 4; edit layout copy unchanged)" % (NEW_CAVE, NEW_EDIT),
            "mppm-test-enemyparms": "enemyParms.szs kochappy, egg, pelplant enemyparm.txt {fp00} x 4",
            "mppm-test-pelletlist": "numberpellet_config.txt number1, number5: radius, p_radius, height x 3, max + 2",
            "mppm-test-pelletlist-bmd": "as mppm-test-pelletlist, and number1 bmd white1.bmd -> white2.bmd",
            "mppm-test-identity": "unchanged stages.txt, kochappy/enemyparm.txt, numberpellet_config.txt",
            "mppm-test-enemyparms-whole": "unchanged enemyParms.szs as a loose file (whole-archive replacement)",
        },
    }
    with open(os.path.join(out, "scenarios.json"), "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    print("make_test_mods: %s (%s, %s): %d scenarios, %d mods in %s"
          % (d.game_id, os.path.basename(a.disc), src, len(scen), len(mods), out))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="build the M5.2 test mods from your own disc")
    ap.add_argument("--disc", required=True, help="your Pikmin 2 (USA) disc image")
    ap.add_argument("--out", required=True, help="output folder, outside the repository")
    ap.add_argument("--use-dtk", action="store_true", help="read the disc through dtk even if it is a plain ISO")
    ap.add_argument("--pellet-evidence", choices=("auto", "checksum", "screenshot"), default="checksum",
                    help="which evidence the pellet check uses (default checksum: measured at integration, the "
                         "pellet run first differs from the golden Ls at tick 210; auto lets mods_test.sh decide)")
    a = ap.parse_args(argv)
    a.out = os.path.abspath(a.out)
    return build(a)


if __name__ == "__main__":
    sys.exit(main())
