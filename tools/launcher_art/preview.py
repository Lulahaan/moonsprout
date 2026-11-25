#!/usr/bin/env python3
"""Render preview composites of the pre-disc start screen (DESIGN.md 6.4 and 6.5).

    python3 tools/launcher_art/preview.py OUT_DIR

Writes OUT_DIR/preview-1080.png (1920x1080) and OUT_DIR/preview-2160.png (3840x2160, 200%):
the backdrop layers approximated in SVG (sky, stars, sun, planet, ship, flame, sparks), the left
scrim and the wordmark with its leaf layer. Previews are for checking the art; they are not
shipped and the RCSS in prelaunch.rcss stays the real implementation.
"""

from __future__ import annotations

import base64
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import IMG_DIR, fmt, rasterize  # noqa: E402


def embed_images(svg: str) -> str:
    """librsvg (through ImageMagick) will not load external files, so inline them."""
    def repl(m):
        data = base64.b64encode((IMG_DIR / m.group(1)).read_bytes()).decode()
        return f'href="data:image/png;base64,{data}"'
    return re.sub(r'href="([\w.-]+\.png)"', repl, svg)


def preview_svg(scale: float) -> str:
    Wd, Hd = 1920, 1080                 # dp
    W, H = int(Wd * scale), int(Hd * scale)
    dp = scale
    stars = ""
    for name, op, off in (("stars-far.png", 0.7, (0, 0)), ("stars-near.png", 0.9, (0, 0)),
                          ("stars-near.png", 0.5, (137, 61))):
        stars += (f'<rect x="0" y="0" width="{W}" height="{H}" fill="url(#pat-{name[:-4]}-{off[0]})" '
                  f'opacity="{op}"/>')
    pats = ""
    for name, off in (("stars-far.png", (0, 0)), ("stars-near.png", (0, 0)), ("stars-near.png", (137, 61))):
        t = 512 * dp
        pats += (f'<pattern id="pat-{name[:-4]}-{off[0]}" patternUnits="userSpaceOnUse" x="{fmt(off[0] * dp)}" '
                 f'y="{fmt(off[1] * dp)}" width="{fmt(t)}" height="{fmt(t)}">'
                 f'<image href="{name}" width="{fmt(t)}" height="{fmt(t)}"/></pattern>')
    # planet: 240vw circle, left -70vw, top 72vh
    pr = 1.2 * W
    pcx = -0.7 * W + pr
    pcy = 0.72 * H + pr
    # ship: 220dp, centre (72%, 34%), rotated -18
    sc = (0.72 * W, 0.34 * H)
    ss = 220 * dp
    wm_w = 420 * dp
    wm_h = wm_w / 4
    wm_x = 96 * dp
    wm_y = H * 0.5 - 300 * dp
    # sparks trail behind the tail, along the ship's axis (tail points down-right after -18 deg)
    import math
    ang = math.radians(90 - 18)
    sparks = ""
    for d, col in ((60, "#E8392B"), (92, "#FFD23F"), (124, "#2F7BEA")):
        x = sc[0] + math.cos(ang) * (d + 70) * dp
        y = sc[1] + math.sin(ang) * (d + 70) * dp
        sparks += (f'<circle cx="{fmt(x)}" cy="{fmt(y)}" r="{fmt(10 * dp)}" fill="{col}" opacity="0.6" filter="url(#b10)"/>'
                   f'<circle cx="{fmt(x)}" cy="{fmt(y)}" r="{fmt(5 * dp)}" fill="{col}"/>')
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">
  <defs>
    <linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#050B1A"/><stop offset="0.42" stop-color="#0B1730"/><stop offset="0.74" stop-color="#1B2E6B"/>
      <stop offset="0.90" stop-color="#3A2A6E"/><stop offset="1" stop-color="#6B3A7A"/>
    </linearGradient>
    <radialGradient id="sun" gradientUnits="userSpaceOnUse" cx="{fmt(0.70 * W)}" cy="{fmt(0.78 * H + 260 * dp)}" r="{fmt(450 * dp)}"
                    gradientTransform="translate(0 {fmt(0.78 * H + 260 * dp)}) scale(1 {fmt(520 / 450)}) translate(0 {fmt(-(0.78 * H + 260 * dp))})">
      <stop offset="0" stop-color="#FFC56B" stop-opacity="0.7"/><stop offset="0.3" stop-color="#FF8A3D" stop-opacity="0.4"/>
      <stop offset="0.55" stop-color="#F25C3B" stop-opacity="0.16"/><stop offset="0.8" stop-color="#6B3A7A" stop-opacity="0"/>
    </radialGradient>
    <radialGradient id="planet" gradientUnits="userSpaceOnUse" cx="{fmt(pcx - pr + 1.2 * pr)}" cy="{fmt(pcy - pr)}" r="{fmt(2 * pr)}">
      <stop offset="0" stop-color="#7BD66A"/><stop offset="0.14" stop-color="#2E9A5B"/><stop offset="0.34" stop-color="#14605A"/>
      <stop offset="0.62" stop-color="#0B2F4A"/><stop offset="1" stop-color="#071A30"/>
    </radialGradient>
    <linearGradient id="scrim" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#050B1A" stop-opacity="0.82"/><stop offset="0.28" stop-color="#050B1A" stop-opacity="0.74"/>
      <stop offset="0.52" stop-color="#050B1A" stop-opacity="0.46"/><stop offset="0.78" stop-color="#050B1A" stop-opacity="0.16"/>
      <stop offset="1" stop-color="#050B1A" stop-opacity="0"/>
    </linearGradient>
    <radialGradient id="flame" cx="0.5" cy="0" r="0.75">
      <stop offset="0" stop-color="#FFF3B0"/><stop offset="0.35" stop-color="#FFB13B"/><stop offset="0.75" stop-color="#F25C3B" stop-opacity="0"/>
    </radialGradient>
    <filter id="b60" filterUnits="userSpaceOnUse" x="-{W}" y="-{H}" width="{3 * W}" height="{3 * H}"><feGaussianBlur stdDeviation="{fmt(30 * dp)}"/></filter>
    <filter id="b10" filterUnits="userSpaceOnUse" x="-{W}" y="-{H}" width="{3 * W}" height="{3 * H}"><feGaussianBlur stdDeviation="{fmt(5 * dp)}"/></filter>
    {pats}
  </defs>
  <rect width="{W}" height="{H}" fill="url(#sky)"/>
  {stars}
  <rect width="{W}" height="{H}" fill="url(#sun)"/>
  <circle cx="{fmt(pcx)}" cy="{fmt(pcy)}" r="{fmt(pr + 10 * dp)}" fill="#7FD8FF" opacity="0.32" filter="url(#b60)"/>
  <circle cx="{fmt(pcx)}" cy="{fmt(pcy)}" r="{fmt(pr)}" fill="url(#planet)"/>
  <circle cx="{fmt(pcx)}" cy="{fmt(pcy)}" r="{fmt(pr - 8 * dp)}" fill="none" stroke="#B9F27C" stroke-width="{fmt(10 * dp)}" opacity="0.3" filter="url(#b10)"/>
  {sparks}
  <g transform="translate({fmt(sc[0])} {fmt(sc[1])}) rotate(-18)">
    <ellipse cx="0" cy="{fmt(ss * 0.43 + 35 * dp)}" rx="{fmt(18 * dp)}" ry="{fmt(35 * dp)}" fill="url(#flame)"/>
    <image href="ship.png" x="{fmt(-ss / 2)}" y="{fmt(-ss / 2)}" width="{fmt(ss)}" height="{fmt(ss)}"/>
  </g>
  <rect width="{fmt(0.62 * W)}" height="{H}" fill="url(#scrim)"/>
  <image href="wordmark.png" x="{fmt(wm_x)}" y="{fmt(wm_y)}" width="{fmt(wm_w)}" height="{fmt(wm_h)}"/>
  <image href="wordmark-leaf.png" x="{fmt(wm_x)}" y="{fmt(wm_y)}" width="{fmt(wm_w)}" height="{fmt(wm_h)}"/>
  <g fill="#EEF7FF" opacity="0.9">
    <rect x="{fmt(wm_x)}" y="{fmt(wm_y + wm_h + 40 * dp)}" width="{fmt(400 * dp)}" height="{fmt(68 * dp)}" rx="{fmt(34 * dp)}" fill="#C8284F"/>
    <rect x="{fmt(wm_x)}" y="{fmt(wm_y + wm_h + 118 * dp)}" width="{fmt(300 * dp)}" height="{fmt(56 * dp)}" rx="{fmt(28 * dp)}" fill="#0B1730" opacity="0.7" stroke="#6FB6DE" stroke-width="{fmt(dp)}"/>
    <rect x="{fmt(wm_x)}" y="{fmt(wm_y + wm_h + 184 * dp)}" width="{fmt(300 * dp)}" height="{fmt(56 * dp)}" rx="{fmt(28 * dp)}" fill="#0B1730" opacity="0.7" stroke="#6FB6DE" stroke-width="{fmt(dp)}"/>
  </g>
</svg>
"""


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1]).resolve()
    out.mkdir(parents=True, exist_ok=True)
    for scale, name in ((1.0, "preview-1080.png"), (2.0, "preview-2160.png")):
        rasterize(embed_images(preview_svg(scale)), out / name)
    print(f"previews in {out}")


if __name__ == "__main__":
    main()
