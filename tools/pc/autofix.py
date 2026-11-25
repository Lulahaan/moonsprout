#!/usr/bin/env python3
"""Mechanical source fixes for the host (Clang) build, driven by Clang diagnostics.

usage: autofix.py [--casts] [--switch] LOG...

  --casts   `(u32)ptr` -> `(uptr)ptr` (and s32/int -> sptr) where Clang reports
            "cast from pointer to smaller type". uptr/sptr are u32/s32 under MWCC,
            so the matching build is unchanged.
  --this    For "use of undeclared identifier" inside a class template with a dependent base
            (or an out-of-line member of a class template), prefix the identifier with `this->`.
            MWCC has no two-phase lookup; `this->` is standard C++ and codegen-neutral.
  --nullptr Replace `nullptr` with `0` where it initializes or is assigned to an integer
            (MWCC's nullptr is a macro for 0).
  --fixits Apply Clang's own fix-its (logs need -fdiagnostics-parseable-fixits), but only for
            an allowlist of codegen-neutral diagnostics (missing `template <>`, `typename`,
            `template` keyword, qualified member-function addresses, ...).
  --fields  Rename decomp-era SDK struct member names to the real SDK names that Aurora uses
            (only where Clang reports "no member named X in T" for a mapped T/X pair).
  --switch  For "cannot jump from switch statement to this case label", wrap the case body
            that declares the bypassed variable in PC_SCOPE_BEGIN / PC_SCOPE_END (blocks on
            host builds only; no-ops under MWCC).
"""
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DIAG = re.compile(r"^(?P<file>[^:\s]+):(?P<line>\d+):(?P<col>\d+): (?P<kind>error|note): (?P<msg>.*)$")

CAST_MAP = {
    "u32": "uptr", "unsigned long": "uptr", "unsigned int": "uptr", "uint": "uptr", "unknown": "uptr",
    "s32": "sptr", "int": "sptr", "long": "sptr", "signed long": "sptr",
}


def parse(logs):
    """Yield (error_diag, [notes]) groups."""
    for log in logs:
        cur = None
        for line in Path(log).read_text(errors="replace").splitlines():
            m = DIAG.match(line)
            if not m:
                continue
            d = m.groupdict()
            d["line"], d["col"] = int(d["line"]), int(d["col"])
            if d["kind"] == "error":
                if cur:
                    yield cur
                cur = (d, [])
            elif cur:
                cur[1].append(d)
        if cur:
            yield cur


def in_repo(path):
    p = (ROOT / path).resolve()
    try:
        p.relative_to(ROOT)
    except ValueError:
        return None
    return p if p.exists() and "extern" not in p.relative_to(ROOT).parts[:1] else None


# ---------------------------------------------------------------- casts
def fix_casts(groups):
    sites = defaultdict(set)
    for err, _ in groups:
        if "cast from pointer to smaller type" in err["msg"]:
            p = in_repo(err["file"])
            if p:
                sites[p].add((err["line"], err["col"]))
    n = 0
    for p, locs in sites.items():
        lines = p.read_text().split("\n")
        for ln, col in sorted(locs, reverse=True):
            s = lines[ln - 1]
            i = col - 1
            # C-style cast: "(T)" starting at col
            m = re.match(r"\(\s*(const\s+)?([A-Za-z_][\w ]*?)\s*\)", s[i:])
            if m and m.group(2) in CAST_MAP:
                s = s[:i] + "(" + (m.group(1) or "") + CAST_MAP[m.group(2)] + ")" + s[i + m.end():]
                n += 1
            else:
                m = re.match(r"(static_cast|reinterpret_cast)<\s*([A-Za-z_][\w ]*?)\s*>", s[i:])
                if m and m.group(2) in CAST_MAP:
                    s = s[:i] + "reinterpret_cast<" + CAST_MAP[m.group(2)] + ">" + s[i + m.end():]
                    n += 1
                else:
                    print(f"  cast: no match {p.relative_to(ROOT)}:{ln}:{col}: {s.strip()[:100]}")
            lines[ln - 1] = s
        p.write_text("\n".join(lines))
    print(f"casts: rewrote {n} site(s) in {len(sites)} file(s)")


# ---------------------------------------------------------------- switch scopes
def strip_code(text):
    """Blank out comments and string/char literals, preserving offsets."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            for k in range(i + 1, min(j, n)):
                out[k] = " "
            i = j + 1
        else:
            i += 1
    return "".join(out)


LABEL = re.compile(r"\b(case\b[^;{}]*?[^:]:(?!:)|default\s*:(?!:))")


def line_offsets(text):
    offs = [0]
    for i, c in enumerate(text):
        if c == "\n":
            offs.append(i + 1)
    return offs


def depth_array(code):
    d, arr = 0, []
    for c in code:
        if c == "{":
            d += 1
        arr.append(d)
        if c == "}":
            d -= 1
    return arr


def find_case_region(code, depth, var_off):
    """Return (insert_begin_off, insert_end_off) around the case body containing var_off."""
    d = depth[var_off]
    # backward: nearest case/default label at the same depth
    begin = None
    for m in LABEL.finditer(code, 0, var_off):
        if depth[m.start()] == d:
            begin = m.end()
    if begin is None:
        return None
    # forward: next label at same depth, or the closing brace of this depth
    end = None
    i = var_off
    n = len(code)
    while i < n:
        if code[i] == "}" and depth[i] == d:
            end = i
            break
        if depth[i] == d and (code.startswith("case", i) or code.startswith("default", i)):
            m = LABEL.match(code, i)
            if m and (i == 0 or not (code[i - 1].isalnum() or code[i - 1] == "_")):
                end = i
                break
        i += 1
    if end is None:
        return None
    # don't double-wrap
    if code[begin:end].strip().startswith("PC_SCOPE_BEGIN"):
        return None
    return begin, end


def fix_switch(groups):
    todo = defaultdict(set)  # path -> set of variable (line, col)
    for err, notes in groups:
        if "cannot jump from switch statement to this case label" not in err["msg"]:
            continue
        for nt in notes:
            if "jump bypasses" in nt["msg"]:
                p = in_repo(nt["file"])
                if p:
                    todo[p].add((nt["line"], nt["col"]))
    total = 0
    for p, vars_ in todo.items():
        text = p.read_text()
        code = strip_code(text)
        depth = depth_array(code)
        offs = line_offsets(text)
        regions = set()
        for ln, col in vars_:
            r = find_case_region(code, depth, offs[ln - 1] + col - 1)
            if r:
                regions.add(r)
            else:
                print(f"  switch: no region {p.relative_to(ROOT)}:{ln}:{col}")
        # merge identical/overlapping and apply from the end
        edits = []
        for b, e in sorted(regions):
            edits.append((e, "end"))
            edits.append((b, "begin"))
        for off, kind in sorted(edits, key=lambda x: (x[0], x[1] == "begin"), reverse=True):
            if kind == "begin":
                text = text[:off] + " PC_SCOPE_BEGIN" + text[off:]
            else:
                # place PC_SCOPE_END on its own line before the label / closing brace
                ls = text.rfind("\n", 0, off) + 1
                indent = re.match(r"[ \t]*", text[ls:]).group(0)
                if text[ls:off].strip() == "":
                    text = text[:ls] + indent + "\tPC_SCOPE_END\n" + text[ls:]
                else:
                    text = text[:off] + "PC_SCOPE_END " + text[off:]
        p.write_text(text)
        total += len(regions)
    print(f"switch: wrapped {total} case bod(ies) in {len(todo)} file(s)")


# ---------------------------------------------------------------- this->
TEMPLATE_CLASS = re.compile(r"template\s*<[^{};]*>\s*(struct|class)\s+\w+[^;{]*:[^;{]*$", re.S)
TEMPLATE_MEMBER = re.compile(r"template\s*<[^{};]*>[^;{}]*\b\w+\s*<[^;{}]*>\s*::\s*~?\w+\s*\([^{};]*\)[^;{}]*$", re.S)


def enclosing_is_template(code, depth, off):
    d = depth[off]
    i = off
    while d > 0:
        # find the '{' that opened depth d
        while i >= 0 and not (code[i] == "{" and depth[i] == d):
            i -= 1
        if i < 0:
            return False
        head_start = max(code.rfind(";", 0, i), code.rfind("}", 0, i), code.rfind("{", 0, i)) + 1
        head = code[head_start:i]
        # include a preceding "template <...>" that may sit before an earlier ';'-free span
        t = code.rfind("template", max(0, head_start - 400), i)
        if t != -1:
            head2 = code[t:i]
            if TEMPLATE_CLASS.search(head2) or TEMPLATE_MEMBER.search(head2):
                return True
        if TEMPLATE_CLASS.search(head) or TEMPLATE_MEMBER.search(head):
            return True
        d -= 1
        i -= 1
    return False


def fix_this(groups):
    sites = defaultdict(set)
    for err, _ in groups:
        m = re.match(r"use of undeclared identifier '(\w+)'|explicit qualification required to use member '(\w+)' from dependent base class", err["msg"])
        if m:
            p = in_repo(err["file"])
            if p:
                sites[p].add((err["line"], err["col"], m.group(1) or m.group(2)))
    n = 0
    for p, locs in sites.items():
        text = p.read_text()
        code = strip_code(text)
        depth = depth_array(code)
        offs = line_offsets(text)
        edits = []
        for ln, col, name in locs:
            off = offs[ln - 1] + col - 1
            if text[off:off + len(name)] != name:
                continue
            prev = code[:off].rstrip()
            if prev.endswith((".", "->", "::")):
                continue
            # "Type name(...)" is a declaration whose type needs `typename`, not a member access.
            if re.search(r"(\w|>)$", prev) and not re.search(r"\b(return|case|delete|throw|else|do)$", prev):
                continue
            if enclosing_is_template(code, depth, off):
                edits.append(off)
        for off in sorted(set(edits), reverse=True):
            text = text[:off] + "this->" + text[off:]
            n += 1
        if edits:
            p.write_text(text)
    print(f"this: inserted {n} this-> in {len(sites)} candidate file(s)")


# ---------------------------------------------------------------- nullptr -> 0
def fix_nullptr(groups):
    sites = defaultdict(set)
    for err, _ in groups:
        msg = err["msg"]
        if ("std::nullptr_t" in msg and "incompatible" in msg) or ("nullptr_t" in msg and "cannot initialize" in msg) \
                or re.match(r"invalid operands to binary expression \('(const )?(u|s)(8|16|32|64)' .*'std::nullptr_t'\)", msg):
            p = in_repo(err["file"])
            if p:
                sites[p].add((err["line"], err["col"]))
    n = 0
    for p, locs in sites.items():
        lines = p.read_text().split("\n")
        for ln, col in sorted(locs, reverse=True):
            s = lines[ln - 1]
            i = s.find("nullptr", max(0, col - 1))
            if i == -1:
                i = s.rfind("nullptr", 0, col)
            if i == -1:
                print(f"  nullptr: no match {p.relative_to(ROOT)}:{ln}")
                continue
            lines[ln - 1] = s[:i] + "0" + s[i + 7:]
            n += 1
        p.write_text("\n".join(lines))
    print(f"nullptr: rewrote {n} site(s)")


# ---------------------------------------------------------------- clang fix-its
FIXIT_OK = (
    "template specialization requires 'template<>'",
    "did you forget 'typename'",
    "missing 'typename' prior to dependent type name",
    "use 'template' keyword to treat",
    "must explicitly qualify name of member function when taking its address",
    "call to non-static member function without an object argument",
    "explicit qualification required to use member",
    "extra qualification on member",
)
FIXIT = re.compile(r'^fix-it:"(?P<file>[^"]+)":\{(?P<l1>\d+):(?P<c1>\d+)-(?P<l2>\d+):(?P<c2>\d+)\}:"(?P<text>.*)"$')


def fix_fixits(logs):
    edits = defaultdict(set)
    for log in logs:
        cur_ok = False
        for line in Path(log).read_text(errors="replace").splitlines():
            m = DIAG.match(line)
            if m:
                cur_ok = m.group("kind") == "error" and any(k in m.group("msg") for k in FIXIT_OK)
                continue
            f = FIXIT.match(line)
            if f and cur_ok:
                p = in_repo(f.group("file"))
                if p:
                    text = f.group("text").encode().decode("unicode_escape")
                    edits[p].add((int(f.group("l1")), int(f.group("c1")), int(f.group("l2")), int(f.group("c2")), text))
    n = 0
    for p, es in edits.items():
        src = p.read_text()
        offs = line_offsets(src)
        spans = sorted({(offs[l1 - 1] + c1 - 1, offs[l2 - 1] + c2 - 1, t) for l1, c1, l2, c2, t in es}, reverse=True)
        last = None
        for a, b, t in spans:
            if last is not None and b > last:
                continue  # overlapping edit; leave for next round
            src = src[:a] + t + src[b:]
            last = a
            n += 1
        p.write_text(src)
    print(f"fixits: applied {n} edit(s) in {len(edits)} file(s)")


# ---------------------------------------------------------------- SDK field names
FIELD_MAP = {
    "GXVtxAttrFmtList": {"mAttr": "attr", "mCount": "cnt", "mType": "type", "mFrac": "frac"},
    "GXVtxDescList": {"mAttr": "attr", "mType": "type"},
    "_GXFogAdjTable": {"fogVals": "r"},
    "GXFogAdjTable": {"fogVals": "r"},
}


def fix_fields(groups):
    sites = defaultdict(set)
    for err, _ in groups:
        m = re.match(r"no member named '(\w+)' in '(\w+)'", err["msg"])
        if m and m.group(2) in FIELD_MAP and m.group(1) in FIELD_MAP[m.group(2)]:
            p = in_repo(err["file"])
            if p:
                sites[p].add((err["line"], err["col"], m.group(1), FIELD_MAP[m.group(2)][m.group(1)]))
    n = 0
    for p, locs in sites.items():
        lines = p.read_text().split("\n")
        for ln, col, old, new in sorted(locs, reverse=True):
            s = lines[ln - 1]
            i = col - 1
            if s[i:i + len(old)] == old:
                lines[ln - 1] = s[:i] + new + s[i + len(old):]
                n += 1
        p.write_text("\n".join(lines))
    print(f"fields: renamed {n} member access(es)")


if __name__ == "__main__":
    args = sys.argv[1:]
    modes = {a for a in args if a.startswith("--")}
    logs = [a for a in args if not a.startswith("--")]
    groups = list(parse(logs))
    if "--casts" in modes:
        fix_casts(groups)
    if "--switch" in modes:
        fix_switch(groups)
    if "--this" in modes:
        fix_this(groups)
    if "--nullptr" in modes:
        fix_nullptr(groups)
    if "--fields" in modes:
        fix_fields(groups)
    if "--fixits" in modes:
        fix_fixits(logs)
