#!/usr/bin/env python3
"""modhash.py - reference for the mppm mods folder rules and content hashes (M5.2).

Scans a mods folder the way the game does before it mounts anything (docs/online/M5_2_MODS.md,
docs/mods/README.md): load_order.txt, every mod.json, the path rules, the size limits and the
validators that need no disc. Prints the result in the form of the game's log lines and the
two hashes:

  mppm: mods: hash <mods hash, 64 hex> content <content hash, 64 hex>

Usage:
  modhash.py MODS_DIR [--build-id HEX | --exe PATH] [--mppm-version X.Y.Z] [--json FILE]
  modhash.py --selftest

  --build-id HEX   the build id of the game (the "# build=" line of a checksum log)
  --exe PATH       compute the build id as the game does: FNV-1a 64 of the executable (slow:
                   about half a minute for a 165 MB file)
  --mppm-version   the version to check the "mppm" key against (default 0.1.0)
  --json FILE      also write the scan as JSON (mods, states, issues, conflicts, hashes)

Without a build id the content hash is printed as "-". Checks that need the disc (does the
archive hold that inner file, is a path a folder on the disc) are not made here: the game makes
them while it mounts and drops a mod that fails one, so for a folder with such a mod the hashes
printed here differ from the game's. The error sentences follow the game's where they are
quoted in the docs, but are not all worded the same; the hash line is the part to compare.

Python 3 standard library only. The folder is untrusted input: run with `python3 -I`.
"""

import argparse
import hashlib
import json
import os
import re
import stat
import struct
import sys

# ---- limits (design 4.1-4.5) -------------------------------------------------------------------
MAX_MOD_FOLDERS = 256
MAX_MANIFEST = 64 << 10
MAX_DEPTH = 16
MAX_FILES = 4096
MAX_LOAD_ORDER = 64 << 10
MAX_LOAD_ORDER_LINES = 1024
MAX_FILE = 64 << 20
MAX_INNER = 16 << 20
MAX_MOD = 256 << 20
MAX_TOTAL = 512 << 20
MAX_PATH = 240
MAX_COMPONENTS = 16
MAX_COMPONENT = 64

ID_RE = re.compile(r"^[a-z0-9-]{1,64}$")
VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$")
MPPM_RE = re.compile(r"^>=([0-9]+)(?:\.([0-9]+))?(?:\.([0-9]+))?$")
COMPONENT_RE = re.compile(r"^[A-Za-z0-9._-]+$")
DEVICE_NAMES = {"con", "prn", "aux", "nul"} | {"com%d" % i for i in range(1, 10)} | \
    {"lpt%d" % i for i in range(1, 10)}
KNOWN_KEYS = ("id", "version", "name", "mppm", "files", "author", "description", "affects")

VS_STAGES = "/user/abe/vs/stages.txt"
CHALLENGE_STAGES = ("/user/matoba/challenge/stages.txt", "/user/matoba/challenge/kfes-stages.txt")

LOAD_ORDER_HEADER = ('# Moonsprout mod load order: top = lowest priority, bottom = highest. '
                     '"-" turns a mod off.')


# ---- hashes (design 4.7) -----------------------------------------------------------------------

def fnv1a64_file(path):
    h = 0xCBF29CE484222325
    prime = 0x100000001B3
    mask = 0xFFFFFFFFFFFFFFFF
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            for b in chunk:
                h = ((h ^ b) * prime) & mask
    return h


def serialize(mods):
    """S of design 4.7. mods: [(id, version, [(key, size, sha256 bytes)])] in load order."""
    out = bytearray()
    for mid, ver, files in mods:
        idb, verb = mid.encode("utf-8"), ver.encode("utf-8")
        out += struct.pack("<I", len(idb)) + idb + struct.pack("<I", len(verb)) + verb
        out += struct.pack("<I", len(files))
        for key, size, digest in sorted(files, key=lambda f: f[0].encode("utf-8")):
            kb = key.encode("utf-8")
            out += struct.pack("<I", len(kb)) + kb + struct.pack("<Q", size) + digest
    return bytes(out)


def mods_hash(s, any_mod):
    if not any_mod:
        return bytes(32)
    return hashlib.sha256(b"mppm-mods-v1" + s).digest()


def content_hash(build_id, s):
    return hashlib.sha256(b"mppm-content-v1" + struct.pack("<Q", build_id) + s).digest()


# ---- scan ------------------------------------------------------------------------------------

class Issue(object):
    def __init__(self, severity, path, message):
        self.severity = severity  # "error" or "warning"
        self.path = path or "-"
        self.message = message


class Mod(object):
    def __init__(self, folder):
        self.folder = folder  # folder name (the id when valid)
        self.id = folder
        self.version = ""
        self.name = folder
        self.author = ""
        self.description = ""
        self.affects = "gameplay"
        self.files = []  # [(manifest path, key, size, sha256 or None)]
        self.issues = []
        self.state = "new"  # on, off, new, problem, missing
        self.listed = False
        self.enabled = False
        self.size = 0

    def error(self, path, msg):
        self.issues.append(Issue("error", path, msg))

    def warning(self, path, msg):
        self.issues.append(Issue("warning", path, msg))

    @property
    def valid(self):
        return not any(i.severity == "error" for i in self.issues)


def control_free(s):
    return "".join(ch for ch in s if ord(ch) >= 0x20 and ord(ch) != 0x7F)


def json_depth_ok(text):
    """Prescan of [ and { outside strings, before the parser sees the text."""
    depth, in_str, esc = 0, False, False
    for ch in text:
        if in_str:
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
        elif ch in "[{":
            depth += 1
            if depth > MAX_DEPTH:
                return False
        elif ch in "]}":
            depth -= 1
    return True


def check_path(p):
    """(key, kind, message). kind is "file" or "archive"; message is None when the path is fine."""
    if not isinstance(p, str):
        return None, None, "A file entry is not a string."
    try:
        raw = p.encode("utf-8")
    except UnicodeEncodeError:
        return None, None, "The path is not valid text."
    if len(raw) > MAX_PATH:
        return None, None, "The path is longer than %d bytes." % MAX_PATH
    if p.startswith("/"):
        return None, None, "The path must be relative to the mod folder (no leading /)."
    if "\\" in p:
        return None, None, "The path uses a backslash; use / between folders."
    if ":" in p:
        return None, None, "The path contains a colon."
    parts = p.split("/")
    if len(parts) > MAX_COMPONENTS:
        return None, None, "The path has more than %d parts." % MAX_COMPONENTS
    for c in parts:
        if c == "":
            return None, None, "The path has an empty part (// or a / at the end)."
        if c in (".", "..") or c.startswith("."):
            return None, None, "A path part may not start with a dot (no . or ..)."
        if len(c.encode("utf-8")) > MAX_COMPONENT:
            return None, None, "A path part is longer than %d bytes." % MAX_COMPONENT
        if not COMPONENT_RE.match(c):
            return None, None, "A path part may use only letters, digits, '.', '_' and '-'."
        if c.endswith("."):
            return None, None, "A path part may not end with a dot."
        if c.lower().split(".")[0] in DEVICE_NAMES:
            return None, None, "\"%s\" is a reserved device name on Windows." % c
    if parts[0] == "files":
        if len(parts) < 2:
            return None, None, "The path names no file under files/."
        return "/" + "/".join(parts[1:]).lower(), "file", None
    if parts[0] == "archives":
        for i in range(1, len(parts)):
            low = parts[i].lower()
            if low.endswith(".szs") or low.endswith(".arc"):
                inner = parts[i + 1:]
                if not inner:
                    return None, None, "The path names an archive but no file inside it."
                key = "/" + "/".join(parts[1:i + 1]).lower() + ":" + "/".join(inner).lower()
                return key, "archive", None
        return None, None, "An archives/ path needs a .szs or .arc archive in it."
    return None, None, "A path must start with files/ or archives/."


def strip_comments(text):
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def count_blocks(text):
    """(first integer token or None, number of top-level {} blocks, braces balanced)."""
    body = strip_comments(text)
    toks = body.split()
    first = None
    if toks:
        try:
            first = int(toks[0])
        except ValueError:
            first = None
    depth, blocks, balanced = 0, 0, True
    for ch in body:
        if ch == "{":
            if depth == 0:
                blocks += 1
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth < 0:
                balanced = False
                depth = 0
    if depth != 0:
        balanced = False
    return first, blocks, balanced


def validate_file(mod, mpath, key, data):
    low = key
    if low == VS_STAGES or low in CHALLENGE_STAGES:
        text = data.decode("latin-1")
        n, blocks, _ = count_blocks(text)
        if n is None or n != blocks:
            mod.error(mpath, "The stage list says %s stages but holds %d." % (n if n is not None else "no number of", blocks))
        elif low == VS_STAGES and not (1 <= n <= 10):
            mod.error(mpath, "The VS stage list has %d stages; the stage select shows at most 10." % n)
        elif low != VS_STAGES and n < 30:
            mod.error(mpath, "The Challenge stage list has %d stages; it needs at least 30." % n)
    if ":" not in low and (low.endswith(".szs") or low.endswith(".arc")):
        if data[:4] not in (b"Yaz0", b"RARC"):
            mod.error(mpath, "This archive does not start with Yaz0 or RARC.")
    if low.endswith(".txt") or low.endswith(".ini"):
        if b"\0" in data:
            mod.warning(mpath, "This text file holds a NUL byte.")
        else:
            _, _, balanced = count_blocks(data.decode("latin-1"))
            if not balanced:
                mod.warning(mpath, "The { and } in this text file do not pair up.")


def version_tuple(text):
    m = re.match(r"^([0-9]+)(?:\.([0-9]+))?(?:\.([0-9]+))?", text)
    if not m:
        return (0, 0, 0)
    return tuple(int(x or 0) for x in m.groups())


def read_manifest(mod, mod_dir, mppm_version):
    mpath = "mod.json"
    full = os.path.join(mod_dir, mpath)
    try:
        st = os.lstat(full)
    except OSError:
        mod.error("-", "The mod has no mod.json.")
        return None
    if stat.S_ISLNK(st.st_mode) or not stat.S_ISREG(st.st_mode):
        mod.error(mpath, "mod.json must be a regular file, not a link or a folder.")
        return None
    if st.st_size > MAX_MANIFEST:
        mod.error(mpath, "mod.json is larger than 64 KiB.")
        return None
    with open(full, "rb") as f:
        raw = f.read(MAX_MANIFEST + 1)
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        mod.error(mpath, "mod.json is not UTF-8 text.")
        return None
    if not json_depth_ok(text):
        mod.error(mpath, "mod.json is nested too deeply (more than %d levels)." % MAX_DEPTH)
        return None
    try:
        doc = json.loads(text)
    except (ValueError, RecursionError) as e:
        mod.error(mpath, "mod.json is not valid JSON (%s)." % str(e).split(":")[0])
        return None
    if not isinstance(doc, dict):
        mod.error(mpath, "mod.json must hold a JSON object.")
        return None

    def string(key, lo, hi):
        v = doc.get(key)
        if not isinstance(v, str):
            mod.error(mpath, "\"%s\" must be a string." % key)
            return None
        if not (lo <= len(v) <= hi):
            mod.error(mpath, "\"%s\" must be %d to %d characters long." % (key, lo, hi))
            return None
        return v

    if "id" not in doc:
        mod.error(mpath, "\"id\" is missing.")
    else:
        v = string("id", 1, 64)
        if v is not None:
            if not ID_RE.match(v):
                mod.error(mpath, "\"id\" may use only a-z, 0-9 and -.")
            elif v != mod.folder:
                mod.error(mpath, "The folder name \"%s\" is not the id \"%s\"." % (mod.folder, v))
            else:
                mod.id = v
    if "version" not in doc:
        mod.error(mpath, "\"version\" is missing.")
    else:
        v = string("version", 1, 32)
        if v is not None:
            if VERSION_RE.match(v):
                mod.version = v
            else:
                mod.error(mpath, "\"version\" must look like 1.2.3.")
    if "name" in doc:
        v = string("name", 1, 64)
        if v is not None:
            mod.name = control_free(v) or mod.id
    else:
        mod.name = mod.id
    if "mppm" in doc:
        v = string("mppm", 1, 32)
        if v is not None:
            m = MPPM_RE.match(v)
            if not m:
                mod.error(mpath, "\"mppm\" must look like >=0.1.")
            else:
                need = tuple(int(x or 0) for x in m.groups())
                if need > version_tuple(mppm_version):
                    shown = ".".join(x for x in m.groups() if x is not None)
                    if "." not in shown:
                        shown += ".0"
                    mod.error(mpath, "Needs Moonsprout %s or newer." % shown)
    if "author" in doc:
        v = string("author", 0, 64)
        if v is not None:
            mod.author = control_free(v)
    if "description" in doc:
        v = string("description", 0, 1024)
        if v is not None:
            mod.description = control_free(v)
    if "affects" in doc:
        v = doc.get("affects")
        if v not in ("gameplay", "cosmetic"):
            mod.error(mpath, "\"affects\" must be \"gameplay\" or \"cosmetic\".")
        else:
            mod.affects = v
    for k in doc:
        if k not in KNOWN_KEYS:
            mod.warning(mpath, "Unknown key \"%s\" ignored." % control_free(str(k))[:64])
    files = doc.get("files")
    if "files" not in doc:
        mod.error(mpath, "\"files\" is missing.")
        return None
    if not isinstance(files, list) or not (1 <= len(files) <= MAX_FILES):
        mod.error(mpath, "\"files\" must be a list of 1 to %d paths." % MAX_FILES)
        return None
    return files


def inside(path, root):
    return path == root or path.startswith(root.rstrip(os.sep) + os.sep)


def scan_mod(mods_dir, folder, mppm_version):
    mod = Mod(folder)
    mod_dir = os.path.join(mods_dir, folder)
    if not ID_RE.match(folder):
        mod.issues.append(Issue("warning", "-", "The folder name is not a mod id (1 to 64 characters of a-z, 0-9 "
                                               "and -), so this mod can't load. Rename the folder to the mod's id."))
    entries = read_manifest(mod, mod_dir, mppm_version)
    if entries is None:
        return mod
    try:
        canon_root = os.path.realpath(mod_dir)
    except OSError:
        mod.error("-", "The mod folder cannot be read.")
        return mod
    keys = {}
    total = 0
    for p in entries:
        key, kind, msg = check_path(p)
        shown = p if isinstance(p, str) else "-"
        if msg:
            mod.error(control_free(shown)[:MAX_PATH] or "-", msg)
            continue
        if key in keys:
            mod.error(p, "Two entries name the same file (%s)." % key[1:])
            continue
        # every part must exist and none may be a link
        full = mod_dir
        bad = None
        parts = p.split("/")
        for i, c in enumerate(parts):
            full = os.path.join(full, c)
            try:
                st = os.lstat(full)
            except OSError:
                bad = "The file is listed in mod.json but missing."
                break
            if stat.S_ISLNK(st.st_mode):
                bad = "The file is a symbolic link; mods may hold only regular files."
                break
            if i < len(parts) - 1 and not stat.S_ISDIR(st.st_mode):
                bad = "The file is listed in mod.json but missing."
                break
        if bad is None:
            if not stat.S_ISREG(st.st_mode):
                bad = "This is not a regular file."
            elif not inside(os.path.realpath(full), canon_root):
                bad = "The file is outside the mod folder."
        if bad:
            mod.error(p, bad)
            continue
        size = st.st_size
        limit = MAX_INNER if kind == "archive" else MAX_FILE
        if size > limit:
            mod.error(p, "The file is too large: %d MiB, the limit is %d MiB." % (size >> 20, limit >> 20))
            continue
        if total + size > MAX_MOD:
            mod.error(p, "The mod is too large: the limit is 256 MiB for one mod.")
            continue
        total += size
        with open(full, "rb") as f:
            data = f.read(limit + 1)
        validate_file(mod, p, key, data)
        keys[key] = True
        mod.files.append((p, key, len(data), hashlib.sha256(data).digest()))
    mod.size = total
    return mod


def read_load_order(mods_dir, folder_issues):
    """[(id, on)] in file order (first occurrence counts), or None when there is no file."""
    path = os.path.join(mods_dir, "load_order.txt")
    try:
        st = os.lstat(path)
    except OSError:
        return None
    if not stat.S_ISREG(st.st_mode):
        folder_issues.append(Issue("warning", "load_order.txt", "load_order.txt is not a regular file."))
        return None
    if st.st_size > MAX_LOAD_ORDER:
        folder_issues.append(Issue("warning", "load_order.txt", "load_order.txt is larger than 64 KiB."))
        return None
    with open(path, "rb") as f:
        text = f.read(MAX_LOAD_ORDER + 1).decode("utf-8", "replace")
    lines = text.splitlines()
    if len(lines) > MAX_LOAD_ORDER_LINES:
        folder_issues.append(Issue("warning", "load_order.txt", "load_order.txt has more than 1024 lines; the rest is ignored."))
        lines = lines[:MAX_LOAD_ORDER_LINES]
    order, seen = [], set()
    for n, raw in enumerate(lines, 1):
        s = raw.strip()
        if not s or s.startswith("#"):
            continue
        on = not s.startswith("-")
        mid = s if on else s[1:].strip()
        if not ID_RE.match(mid):
            folder_issues.append(Issue("warning", "load_order.txt", "Line %d is not a mod id." % n))
            continue
        if mid in seen:
            folder_issues.append(Issue("warning", "load_order.txt", "Line %d repeats \"%s\"; the first one counts." % (n, mid)))
            continue
        seen.add(mid)
        order.append((mid, on))
    return order


def scan(mods_dir, mppm_version="0.1.0"):
    res = {"dir": mods_dir, "mods": [], "folder_issues": [], "order": None, "conflicts": [],
           "mounted": []}
    if not os.path.isdir(mods_dir):
        return res
    folders = []
    for name in sorted(os.listdir(mods_dir)):
        if name.startswith("."):
            continue
        full = os.path.join(mods_dir, name)
        if os.path.isdir(full):  # follows a symlinked mod folder (the player put it there)
            folders.append(name)
    if len(folders) > MAX_MOD_FOLDERS:
        res["folder_issues"].append(Issue("warning", "-", "More than %d mod folders; the rest are ignored." % MAX_MOD_FOLDERS))
        folders = folders[:MAX_MOD_FOLDERS]
    order = read_load_order(mods_dir, res["folder_issues"])
    res["order"] = order
    by_folder = {}
    for name in folders:
        by_folder[name] = scan_mod(mods_dir, name, mppm_version)
    listed = []
    for mid, on in (order or []):
        mod = by_folder.get(mid)
        if mod is None:
            m = Mod(mid)
            m.state = "missing"
            m.listed = True
            res["folder_issues"].append(Issue("warning", "load_order.txt", "\"%s\" is listed but has no folder." % mid))
            listed.append(m)
            continue
        mod.listed = True
        mod.state = ("on" if on else "off") if mod.valid else "problem"
        mod.enabled = on
        listed.append(mod)
    for name in folders:
        mod = by_folder[name]
        if not mod.listed:
            mod.state = "new" if mod.valid else "problem"
            mod.enabled = False
            listed.append(mod)
    res["mods"] = listed
    # the mounted set: enabled valid mods in load order, within the total limit
    total = 0
    for mod in listed:
        if mod.state != "on":
            continue
        if total + mod.size > MAX_TOTAL:
            mod.error("-", "Too much mod data: the limit is 512 MiB for all mods together.")
            mod.state = "problem"
            continue
        total += mod.size
        res["mounted"].append(mod)
    # conflicts: last wins; losers nearest first. For an inner key "<archive>:<inner>", the last mod
    # that replaces the whole archive (loose key "<archive>") takes part at its place in the order
    # (README 5): a mod before it loses its inner file, a mod after it patches the replaced archive.
    providers = {}
    for n, mod in enumerate(res["mounted"]):
        for _, key, _, _ in mod.files:
            providers.setdefault(key, []).append(n)
    names = [m.id for m in res["mounted"]]
    for key in sorted(providers, key=lambda k: k.encode("utf-8")):
        idx = list(providers[key])
        whole = None
        if ":" in key and key.split(":", 1)[0] in providers:
            r = providers[key.split(":", 1)[0]][-1]
            if r not in idx:
                idx = sorted(idx + [r])
                whole = names[r]
        if len(idx) > 1:
            res["conflicts"].append((key, names[idx[-1]], [names[i] for i in reversed(idx[:-1])], whole))
    return res


def hashes(res, build_id):
    mounted = res["mounted"]
    s = serialize([(m.id, m.version, [(k, size, d) for _, k, size, d in m.files]) for m in mounted])
    mh = mods_hash(s, bool(mounted))
    ch = content_hash(build_id, s) if build_id is not None else None
    return s, mh, ch


def report(res, build_id, out):
    w = out.write
    for issue in res["folder_issues"]:
        w("mppm: mods: %s %s: %s: %s\n" % (issue.severity, "-", issue.path, issue.message))
    for mod in res["mods"]:
        if mod.state == "missing":
            continue
        for issue in mod.issues:
            w("mppm: mods: %s %s: %s: %s\n" % (issue.severity, mod.folder, issue.path, issue.message))
        if not mod.valid and mod.listed and mod.enabled:
            w("mppm: mods: skipped %s (errors above)\n" % mod.folder)
    mounted = res["mounted"]
    if mounted:
        nf = sum(1 for m in mounted for f in m.files if ":" not in f[1])
        na = sum(1 for m in mounted for f in m.files if ":" in f[1])
        w("mppm: mods: folder %s: %d on (%s), %d files, %d archive files, %d conflicts\n" % (
            res["dir"], len(mounted), ", ".join("%s@%s" % (m.id, m.version) for m in mounted),
            nf, na, len(res["conflicts"])))
    else:
        w("mppm: mods: folder %s: none on\n" % res["dir"])
    for key, winner, losers, whole in res["conflicts"]:
        w("mppm: mods: conflict %s: %s overrides %s%s\n" % (key, winner, ", ".join(losers),
                                                          " (%s replaces the whole archive)" % whole if whole else ""))
    s, mh, ch = hashes(res, build_id)
    w("mppm: mods: hash %s content %s\n" % (mh.hex(), ch.hex() if ch is not None else "-"))
    return s, mh, ch


def to_json(res, mh, ch, build_id):
    def issue(i):
        return {"severity": i.severity, "path": i.path, "message": i.message}
    return {
        "dir": res["dir"],
        "load_order": None if res["order"] is None else [{"id": i, "on": on} for i, on in res["order"]],
        "folder_issues": [issue(i) for i in res["folder_issues"]],
        "mods": [{
            "folder": m.folder, "id": m.id, "version": m.version, "name": m.name,
            "state": m.state, "affects": m.affects,
            "files": [{"path": p, "key": k, "size": s, "sha256": d.hex()} for p, k, s, d in m.files],
            "issues": [issue(i) for i in m.issues],
        } for m in res["mods"]],
        "mounted": ["%s@%s" % (m.id, m.version) for m in res["mounted"]],
        "conflicts": [{"key": k, "winner": w, "losers": l, "archive_mod": a} for k, w, l, a in res["conflicts"]],
        "build_id": None if build_id is None else "%016x" % build_id,
        "mods_hash": mh.hex(),
        "content_hash": None if ch is None else ch.hex(),
    }


# ---- self-test (design 4.7 vector) ---------------------------------------------------------------

VECTOR = {
    "content": "699dce5678b94e4d309afa06d1e0de3c8c213ae8b1788d05cfc8ff3a07fd6ac3",
    "mods": "d54c604f3bb5fb1d3f40e5b0cd870aa43c26f9c1a0ecaee2b490e16eaf7c531c",
    "stock": "553c9181fbcecab7b9b7d1ce992639878b12c35427e39fd5b92ddc2ee8d1c001",
    "content_rev": "fe7fbbbee89acbfb88b3f51ec383c06bf898775289949e123583ccdb5ac48e53",
    "mods_rev": "9fd19ce6bdc8c5be6150849c3323c21900ef97535807c99aa92916ac003748ff",
}


def selftest():
    fails = 0

    def check(cond, what):
        nonlocal fails
        print("%s  %s" % ("PASS" if cond else "FAIL", what))
        if not cond:
            fails += 1

    def f(key, data):
        return (key, len(data), hashlib.sha256(data).digest())

    bid = 0x0123456789ABCDEF
    alpha = ("alpha", "1.0.0", [f("/user/abe/vs/stages.txt", b"10\n")])
    beta = ("beta", "0.2.1", [f("/user/mukki/mapunits/caveinfo/test.txt", b"hello\n"),
                              f("/enemy/parm/enemyparms.szs:chappy/enemyparm.txt", b"x")])
    s = serialize([alpha, beta])
    check(len(s) == 283, "len(S) = %d (expected 283)" % len(s))
    check(content_hash(bid, s).hex() == VECTOR["content"], "content hash")
    check(mods_hash(s, True).hex() == VECTOR["mods"], "mods hash")
    check(content_hash(bid, b"").hex() == VECTOR["stock"], "stock content hash")
    check(mods_hash(b"", False) == bytes(32), "stock mods hash is 32 zero bytes")
    s2 = serialize([beta, alpha])
    check(content_hash(bid, s2).hex() == VECTOR["content_rev"], "content hash, order beta then alpha")
    check(mods_hash(s2, True).hex() == VECTOR["mods_rev"], "mods hash, order beta then alpha")
    # path rules
    good = [("files/user/Abe/vs/stages.txt", "/user/abe/vs/stages.txt"),
            ("archives/enemy/parm/enemyParms.szs/kochappy/enemyparm.txt",
             "/enemy/parm/enemyparms.szs:kochappy/enemyparm.txt"),
            ("archives/a/B.ARC/x/y.bin", "/a/b.arc:x/y.bin")]
    for p, key in good:
        k, _, msg = check_path(p)
        check(k == key and msg is None, "path %s -> %s" % (p, k))
    bad = ["../x", "files/../x", "/files/x", "files//x", "files/x/", "files/.hidden", "files/a.",
           "files/con.txt", "files/LPT1", "files/a\\b", "files/a:b", "other/x", "files",
           "archives/x/y.szs", "archives/x/y.txt", "files/" + "a" * 65, "files/" + "/".join(["a"] * 16),
           "files/sp ace", 5]
    for p in bad:
        _, _, msg = check_path(p)
        check(msg is not None, "path %r refused (%s)" % (p, msg))
    check(json_depth_ok("[" * 16 + "]" * 16) and not json_depth_ok("[" * 17), "depth prescan")
    check(json_depth_ok('{"a": "[[[[[[[[[[[[[[[[[[[["}'), "depth prescan skips strings")
    n, blocks, bal = count_blocks("2 # n\r\n{ a { b } }\r\n# {\r\n{ c }\r\n")
    check((n, blocks, bal) == (2, 2, True), "stage block count")
    print("modhash selftest: %d failed" % fails)
    return 0 if fails == 0 else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description="mppm mods folder reference scan and hashes")
    ap.add_argument("mods_dir", nargs="?")
    ap.add_argument("--build-id", default=None)
    ap.add_argument("--exe", default=None)
    ap.add_argument("--mppm-version", default="0.1.0")
    ap.add_argument("--json", default=None, help="also write the scan as JSON to this file")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.mods_dir:
        ap.error("give a mods folder (or --selftest)")
    if a.build_id and a.exe:
        ap.error("give --build-id or --exe, not both")
    build_id = None
    if a.build_id:
        try:
            build_id = int(a.build_id, 16)
        except ValueError:
            ap.error("--build-id: not a hex number")
        if not (0 <= build_id < (1 << 64)):
            ap.error("--build-id: out of range")
    elif a.exe:
        try:
            build_id = fnv1a64_file(a.exe)
        except OSError as e:
            print("modhash: cannot read %s: %s" % (a.exe, e), file=sys.stderr)
            return 2
    if not version_tuple(a.mppm_version):
        ap.error("--mppm-version: not a version")
    res = scan(a.mods_dir, a.mppm_version)
    s, mh, ch = report(res, build_id, sys.stdout)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(to_json(res, mh, ch, build_id), f, indent=1)
            f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
