#!/usr/bin/env python3
"""Find #include "..." paths whose letter case differs from the files on disk (harmless on macOS
and Windows, fatal on Linux). With --fix, rewrite them to the on-disk spelling."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SEARCH = [ROOT / "include", ROOT / "include/stl", ROOT / "src", ROOT / "port/include"]


def real_case(base, rel):
    """Return rel with on-disk casing if it exists case-insensitively under base, else None."""
    cur = base
    parts = []
    for part in Path(rel).parts:
        if part in ("..", "."):
            cur = (cur / part)
            parts.append(part)
            continue
        if not cur.is_dir():
            return None
        names = {n.name.lower(): n.name for n in cur.iterdir()}
        hit = names.get(part.lower())
        if hit is None:
            return None
        parts.append(hit)
        cur = cur / hit
    return "/".join(parts)


fix = "--fix" in sys.argv
bad = 0
files = [p for d in ("src", "include", "port") for p in (ROOT / d).rglob("*") if p.suffix in (".c", ".cpp", ".h", ".hpp", ".inc")]
for f in files:
    text = f.read_text(errors="replace")
    new = text
    for m in re.finditer(r'#include\s+"([^"]+)"', text):
        inc = m.group(1)
        for base in [f.parent] + SEARCH:
            rc = real_case(base, inc)
            if rc is not None:
                if rc != inc:
                    bad += 1
                    print(f"{f.relative_to(ROOT)}: \"{inc}\" -> \"{rc}\"")
                    new = new.replace(f'"{inc}"', f'"{rc}"')
                break
    if fix and new != text:
        f.write_text(new)
print(f"{bad} mismatched include(s)" + (" fixed" if fix else ""))
