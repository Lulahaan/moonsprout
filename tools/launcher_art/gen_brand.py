#!/usr/bin/env python3
"""Moonsprout brand art (DESIGN.md 1.3 and 1.4, adapted to the name chosen in README 6).

Outputs
  port/launcher/art/wordmark.svg         full wordmark source (letters, moons, sprout)
  port/launcher/art/wordmark-leaf.svg    the sprout layer alone, same canvas
  port/launcher/art/icon.svg             app icon source, 1024 viewBox
  port/launcher/art/icon-small.svg       simplified icon for 32 px and below
  port/launcher/res/img/wordmark.png       1680x420, letters and moons, no sprout
  port/launcher/res/img/wordmark-leaf.png  1680x420, sprout only; stacks on wordmark.png
  port/launcher/res/img/icon.png           256x256
  port/launcher/res/img/icon-<n>.png       n = 16, 32, 48, 64, 128, 256, 512, 1024

The letters are Gluten (OFL) at wght 700, converted to outlines with fontTools, so the
SVGs contain paths only. Everything random (icon stars) uses SEED.
"""

from __future__ import annotations

import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (ART_DIR, DUSK, FONT_DIR, GLOW_CYAN, IMG_DIR, LEAF, LEAF_DEEP,  # noqa: E402
                    LEAF_HI, NAVY_700, NAVY_800, NAVY_900, PETAL, Font, fmt, load_png, rasterize,
                    save_png, write_text)

SEED = 20261121
NAME = "Moonsprout"

W, H = 1680, 420
SIZE = 214          # font size in px
TRACK = -0.015      # tracking, fraction of the em
BASELINE = 318
OUTLINE = 13        # visible navy outline outside the letters, px
DEPTH = 9           # offset of the dark "extrusion" under the outline, px


# ---------------------------------------------------------------------------
# Shared shapes


def leaf_path(length: float, width: float) -> str:
    """Leaf with its base at the origin and its tip at (length, 0)."""
    L, Wd = length, width / 2
    return (f"M 0,0 C {fmt(L * 0.10)},{fmt(-Wd * 1.05)} {fmt(L * 0.58)},{fmt(-Wd * 1.22)} {fmt(L)},0 "
            f"C {fmt(L * 0.58)},{fmt(Wd * 1.22)} {fmt(L * 0.10)},{fmt(Wd * 1.05)} 0,0 Z")


def leaf_shine(length: float, width: float) -> str:
    L, Wd = length, width / 2
    return (f"M {fmt(L * 0.16)},{fmt(-Wd * 0.30)} C {fmt(L * 0.22)},{fmt(-Wd * 0.85)} {fmt(L * 0.55)},{fmt(-Wd * 0.95)} "
            f"{fmt(L * 0.80)},{fmt(-Wd * 0.38)} C {fmt(L * 0.55)},{fmt(-Wd * 0.55)} {fmt(L * 0.30)},{fmt(-Wd * 0.50)} "
            f"{fmt(L * 0.16)},{fmt(-Wd * 0.30)} Z")


def sprout_group(prefix: str, x: float, y: float, s: float, outline: float, glow: float,
                 glow_opacity: float = 0.7, clip_y: float | None = None,
                 base_color: str = LEAF) -> tuple[str, str]:
    """A two-leaf seedling whose stem starts at (x, y), scaled by s. Returns (defs, body).

    clip_y: nothing is drawn below this line (so the stem can grow out of a letter);
    base_color: stem colour at its base, blending into whatever it grows from.
    """
    stem = (f"M {fmt(x)},{fmt(y)} C {fmt(x - 2 * s)},{fmt(y - 34 * s)} {fmt(x + 4 * s)},{fmt(y - 58 * s)} "
            f"{fmt(x + 14 * s)},{fmt(y - 74 * s)}")
    tip = (x + 14 * s, y - 74 * s)
    big_L, big_W = 128 * s, 70 * s
    small_L, small_W = 78 * s, 44 * s
    big_t = f"translate({fmt(tip[0])} {fmt(tip[1])}) rotate(-34)"
    small_t = f"translate({fmt(tip[0] - 3 * s)} {fmt(tip[1] + 6 * s)}) rotate(-152)"
    big = leaf_path(big_L, big_W)
    small = leaf_path(small_L, small_W)
    stem_w = 12 * s
    defs = f"""
    <linearGradient id="{prefix}LeafBig" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="{fmt(big_L)}" y2="0">
      <stop offset="0" stop-color="{LEAF}"/><stop offset="0.55" stop-color="#8CE04A"/><stop offset="1" stop-color="{LEAF_HI}"/>
    </linearGradient>
    <linearGradient id="{prefix}LeafSmall" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="{fmt(small_L)}" y2="0">
      <stop offset="0" stop-color="{LEAF}"/><stop offset="1" stop-color="#9BE35C"/>
    </linearGradient>
    <filter id="{prefix}Glow" filterUnits="userSpaceOnUse" x="-2000" y="-2000" width="6000" height="6000">
      <feGaussianBlur stdDeviation="{fmt(glow)}"/>
    </filter>
    <linearGradient id="{prefix}Stem" gradientUnits="userSpaceOnUse" x1="0" y1="{fmt(y)}" x2="0" y2="{fmt(y - 50 * s)}">
      <stop offset="0" stop-color="{base_color}"/><stop offset="1" stop-color="{LEAF}"/>
    </linearGradient>
    <clipPath id="{prefix}Clip"><rect x="-4000" y="-4000" width="8000" height="{fmt((clip_y if clip_y is not None else 4000) + 4000)}"/></clipPath>"""
    sil = (f'<path d="{stem}" fill="none" stroke-width="{fmt(stem_w)}" stroke-linecap="round"/>'
           f'<path d="{big}" transform="{big_t}"/><path d="{small}" transform="{small_t}"/>')
    body = f"""
    <g filter="url(#{prefix}Glow)" opacity="{glow_opacity}" fill="{LEAF_HI}" stroke="{LEAF_HI}"
       stroke-width="{fmt(outline * 2)}" stroke-linejoin="round" stroke-linecap="round">{sil}</g>
    <g clip-path="url(#{prefix}Clip)">
    <g fill="{NAVY_800}" stroke="{NAVY_800}" stroke-width="{fmt(outline * 2)}" stroke-linejoin="round"
       stroke-linecap="round">
      <path d="{stem}" fill="none" stroke-width="{fmt(stem_w + outline * 2)}"/>
      <path d="{big}" transform="{big_t}"/><path d="{small}" transform="{small_t}"/>
    </g>
    <path d="{stem}" fill="none" stroke="url(#{prefix}Stem)" stroke-width="{fmt(stem_w)}" stroke-linecap="round"/>
    <path d="{stem}" fill="none" stroke="{LEAF_HI}" stroke-width="{fmt(stem_w * 0.3)}" stroke-linecap="round"
          opacity="0.55" transform="translate({fmt(-stem_w * 0.18)} 0)"/>
    <g transform="{small_t}">
      <path d="{small}" fill="url(#{prefix}LeafSmall)"/>
      <path d="M {fmt(small_L * 0.12)},0 L {fmt(small_L * 0.72)},0" stroke="{LEAF_DEEP}" stroke-width="{fmt(3.5 * s)}"
            stroke-linecap="round" opacity="0.6"/>
    </g>
    <g transform="{big_t}">
      <path d="{big}" fill="url(#{prefix}LeafBig)"/>
      <path d="{leaf_shine(big_L, big_W)}" fill="#FFFFFF" opacity="0.38"/>
      <path d="M {fmt(big_L * 0.1)},0 C {fmt(big_L * 0.35)},{fmt(1.5 * s)} {fmt(big_L * 0.6)},{fmt(1 * s)} {fmt(big_L * 0.8)},0"
            fill="none" stroke="{LEAF_DEEP}" stroke-width="{fmt(4 * s)}" stroke-linecap="round" opacity="0.6"/>
    </g>
    </g>"""
    return defs, body


def moon(prefix: str, cx: float, cy: float, r: float, palette: str, outline: float,
         glow: float, craters=True) -> tuple[str, str]:
    """A round alien moon: shaded sphere, terminator shadow, craters, rim light."""
    if palette == "ice":
        hi, mid, lo, shade, glow_c, crater = "#FFFFFF", "#D9F0FF", "#8EC4EC", "#5C83C8", GLOW_CYAN, "#A9D2F2"
    else:  # petal
        hi, mid, lo, shade, glow_c, crater = "#FFF0F6", PETAL, "#D879AE", "#8A4F9E", "#F58BC0", "#E9A0C8"
    defs = f"""
    <radialGradient id="{prefix}Fill" cx="0.36" cy="0.32" r="0.78">
      <stop offset="0" stop-color="{hi}"/><stop offset="0.45" stop-color="{mid}"/><stop offset="1" stop-color="{lo}"/>
    </radialGradient>
    <clipPath id="{prefix}Clip"><circle cx="{fmt(cx)}" cy="{fmt(cy)}" r="{fmt(r)}"/></clipPath>
    <filter id="{prefix}Glow" filterUnits="userSpaceOnUse" x="-2000" y="-2000" width="6000" height="6000">
      <feGaussianBlur stdDeviation="{fmt(glow)}"/>
    </filter>
    <filter id="{prefix}Soft" filterUnits="userSpaceOnUse" x="-2000" y="-2000" width="6000" height="6000">
      <feGaussianBlur stdDeviation="{fmt(r * 0.06)}"/>
    </filter>"""
    cr = ""
    if craters:
        for (dx, dy, rr) in ((-0.30, 0.18, 0.20), (0.22, -0.30, 0.13), (0.34, 0.30, 0.10), (-0.05, -0.05, 0.08)):
            x, y, rad = cx + dx * r, cy + dy * r, rr * r
            cr += (f'<circle cx="{fmt(x)}" cy="{fmt(y)}" r="{fmt(rad)}" fill="{crater}"/>'
                   f'<path d="M {fmt(x - rad * 0.8)},{fmt(y + rad * 0.35)} A {fmt(rad)},{fmt(rad)} 0 0 0 {fmt(x + rad * 0.8)},{fmt(y + rad * 0.35)}" '
                   f'fill="none" stroke="#FFFFFF" stroke-width="{fmt(max(1.0, rad * 0.22))}" stroke-linecap="round" opacity="0.55"/>')
    body = f"""
    <circle cx="{fmt(cx)}" cy="{fmt(cy)}" r="{fmt(r + outline)}" fill="{glow_c}" opacity="0.6" filter="url(#{prefix}Glow)"/>
    <circle cx="{fmt(cx)}" cy="{fmt(cy)}" r="{fmt(r + outline)}" fill="{NAVY_800}"/>
    <circle cx="{fmt(cx)}" cy="{fmt(cy)}" r="{fmt(r)}" fill="url(#{prefix}Fill)"/>
    <g clip-path="url(#{prefix}Clip)">
      {cr}
      <circle cx="{fmt(cx + r * 0.62)}" cy="{fmt(cy + r * 0.55)}" r="{fmt(r * 1.02)}" fill="none" stroke="{shade}"
              stroke-width="{fmt(r * 0.5)}" opacity="0.55" filter="url(#{prefix}Soft)"/>
      <path d="M {fmt(cx - r * 0.72)},{fmt(cy - r * 0.08)} A {fmt(r * 0.74)},{fmt(r * 0.74)} 0 0 1 {fmt(cx - r * 0.1)},{fmt(cy - r * 0.70)}"
            fill="none" stroke="#FFFFFF" stroke-width="{fmt(r * 0.13)}" stroke-linecap="round" opacity="0.85"/>
    </g>"""
    return defs, body


def sparkle(cx: float, cy: float, r: float, color="#FFFFFF", opacity=1.0) -> str:
    k = r * 0.22
    d = (f"M {fmt(cx)},{fmt(cy - r)} Q {fmt(cx + k)},{fmt(cy - k)} {fmt(cx + r)},{fmt(cy)} "
         f"Q {fmt(cx + k)},{fmt(cy + k)} {fmt(cx)},{fmt(cy + r)} Q {fmt(cx - k)},{fmt(cy + k)} {fmt(cx - r)},{fmt(cy)} "
         f"Q {fmt(cx - k)},{fmt(cy - k)} {fmt(cx)},{fmt(cy - r)} Z")
    return f'<path d="{d}" fill="{color}" opacity="{fmt(opacity)}"/>'


# ---------------------------------------------------------------------------
# Wordmark


def wordmark_parts():
    font = Font(FONT_DIR / "Gluten-VF.ttf", wght=700, slnt=0)
    _, end = font.layout(NAME, 0, BASELINE, SIZE, TRACK)
    x0 = (W - end) / 2 - 40   # leave room on the right for the leaves
    lay, _ = font.layout(NAME, x0, BASELINE, SIZE, TRACK)
    glyphs = []
    for i, (ch, x) in enumerate(lay):
        glyphs.append(dict(ch=ch, d=font.path(ch, x, BASELINE, SIZE), box=font.bounds(ch, x, BASELINE, SIZE),
                           moon=i < 4))
    # top of the t's stem: the highest outline points of the last glyph
    tx = lay[-1][1]
    pts = font.points("t", tx, BASELINE, SIZE)
    top_y = min(p[1] for p in pts)
    near = [p for p in pts if p[1] < top_y + 6]
    t_top = (sum(p[0] for p in near) / len(near), top_y)
    return glyphs, t_top


def wordmark_svg(include_letters=True, include_sprout=True) -> str:
    glyphs, t_top = wordmark_parts()
    defs = []
    body = []

    defs.append(f"""
    <linearGradient id="fillMoon" gradientUnits="userSpaceOnUse" x1="0" y1="{BASELINE - 150}" x2="0" y2="{BASELINE}">
      <stop offset="0" stop-color="#FFFFFF"/><stop offset="1" stop-color="#CFE7FF"/>
    </linearGradient>
    <linearGradient id="fillSprout" gradientUnits="userSpaceOnUse" x1="0" y1="{BASELINE - 150}" x2="0" y2="{BASELINE + 50}">
      <stop offset="0" stop-color="#DDFBA8"/><stop offset="0.45" stop-color="{LEAF_HI}"/><stop offset="1" stop-color="#7CCB45"/>
    </linearGradient>
    <filter id="outerGlow" filterUnits="userSpaceOnUse" x="-200" y="-200" width="{W + 400}" height="{H + 400}">
      <feGaussianBlur stdDeviation="18"/>
    </filter>""")

    if include_letters:
        all_d = " ".join(g["d"] for g in glyphs)
        moon_d = " ".join(g["d"] for g in glyphs if g["moon"])
        sprout_d = " ".join(g["d"] for g in glyphs if not g["moon"])

        # moons above "Moon"
        m1d, m1b = moon("m1", 330, 112, 50, "ice", 9, 16)
        m2d, m2b = moon("m2", 448, 58, 25, "petal", 7, 10)
        defs += [m1d, m2d]

        # per-letter glass shine: top 38% of the letter body, inset 10 px, lower edge an arc
        shine = []
        for i, g in enumerate(glyphs):
            bx0, by0, bx1, by1 = g["box"]
            body_bottom = min(by1, BASELINE)
            hy = by0 + 0.40 * (body_bottom - by0)
            midx = (bx0 + bx1) / 2
            defs.append(f"""
    <mask id="inset{i}" maskUnits="userSpaceOnUse" x="0" y="0" width="{W}" height="{H}">
      <path d="{g['d']}" fill="#FFFFFF" stroke="#000000" stroke-width="20" stroke-linejoin="round"/>
    </mask>""")
            shine.append(
                f'<path mask="url(#inset{i})" fill="#FFFFFF" opacity="0.38" d="M {fmt(bx0 - 5)},{fmt(by0 - 5)} '
                f'L {fmt(bx1 + 5)},{fmt(by0 - 5)} L {fmt(bx1 + 5)},{fmt(hy - 12)} '
                f'Q {fmt(midx)},{fmt(hy + 14)} {fmt(bx0 - 5)},{fmt(hy - 12)} Z"/>')

        sw = OUTLINE * 2
        body.append(f"""
  <!-- glow behind everything -->
  <path d="{moon_d}" fill="{GLOW_CYAN}" stroke="{GLOW_CYAN}" stroke-width="{sw + 10}" stroke-linejoin="round"
        opacity="0.55" filter="url(#outerGlow)"/>
  <path d="{sprout_d}" fill="#7FE07A" stroke="#7FE07A" stroke-width="{sw + 10}" stroke-linejoin="round"
        opacity="0.45" filter="url(#outerGlow)"/>
  <!-- moons in the sky over the word -->
  {m1b}
  {m2b}
  {sparkle(236, 74, 13, "#FFFFFF", 0.95)}
  {sparkle(536, 104, 9, "#FFE27A", 0.95)}
  {sparkle(1262, 96, 8, "#BFE6FF", 0.9)}
  <!-- extrusion and outline -->
  <path d="{all_d}" transform="translate(0 {DEPTH})" fill="{NAVY_900}" stroke="{NAVY_900}" stroke-width="{sw}"
        stroke-linejoin="round"/>
  <path d="{all_d}" fill="{NAVY_800}" stroke="{NAVY_800}" stroke-width="{sw}" stroke-linejoin="round"/>
  <!-- letter fills -->
  <path d="{moon_d}" fill="url(#fillMoon)"/>
  <path d="{sprout_d}" fill="url(#fillSprout)"/>
  {"".join(shine)}""")

    if include_sprout:
        sx, sy = t_top
        sd, sb = sprout_group("sp", sx + 1, sy + 24, 1.0, 10, 20, clip_y=sy - OUTLINE * 0.45,
                              base_color="#C6F08E")
        defs.append(sd)
        body.append(f"\n  <!-- sprout on the t (also exported alone as wordmark-leaf.png) -->{sb}")

    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">\n'
            f"  <!-- Moonsprout wordmark. Letters: Gluten (SIL OFL 1.1) converted to outlines. "
            f"Generated by tools/launcher_art/gen_brand.py. -->\n"
            f"  <defs>{''.join(defs)}\n  </defs>{''.join(body)}\n</svg>\n")


# ---------------------------------------------------------------------------
# Icon


def icon_svg(px: int, small: bool = False) -> str:
    S = 1024
    rad = S * 0.225
    rng = random.Random(SEED)
    hz = S * 0.72                      # horizon crossing
    pr = S * 0.95                      # planet radius (1.9x the width)
    pcy = hz + pr
    stars = ""
    if not small:
        for _ in range(26):
            x, y = rng.uniform(60, S - 60), rng.uniform(50, hz - 120)
            r = rng.uniform(2.5, 6.5)
            stars += f'<circle cx="{fmt(x)}" cy="{fmt(y)}" r="{fmt(r)}" fill="#FFFFFF" opacity="{fmt(rng.uniform(0.35, 0.9))}"/>'
        stars += sparkle(808, 470, 22, "#FFE27A", 0.95) + sparkle(178, 520, 16, "#BFE6FF", 0.9)
    ol = 26 if small else 18
    m1d, m1b = moon("im1", 268 if not small else 250, 262 if not small else 250, 124 if not small else 150,
                    "ice", ol, 34, craters=not small)
    m2d, m2b = ("", "") if small else moon("im2", 790, 196, 62, "petal", 14, 20)
    stem_base = (S * 0.5, hz + 70)
    s = 3.05 if not small else 3.75
    sd, sb = sprout_group("isp", stem_base[0] - 20, stem_base[1], s, ol, 34 if not small else 0.01,
                          glow_opacity=0.65 if not small else 0)
    rim = "" if small else f"""
      <circle cx="{fmt(S / 2)}" cy="{fmt(pcy)}" r="{fmt(pr)}" fill="none" stroke="#7FD8FF" stroke-width="{fmt(S * 0.03)}"
              opacity="0.5" filter="url(#rimBlur)"/>"""
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="{px}" height="{px}" viewBox="0 0 {S} {S}">
  <!-- Moonsprout app icon{' (small sizes)' if small else ''}. Generated by tools/launcher_art/gen_brand.py. -->
  <defs>
    <clipPath id="tile"><rect width="{S}" height="{S}" rx="{fmt(rad)}"/></clipPath>
    <linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="{NAVY_900}"/><stop offset="0.7" stop-color="{NAVY_700}"/><stop offset="1" stop-color="{DUSK}"/>
    </linearGradient>
    <radialGradient id="planet" gradientUnits="userSpaceOnUse" cx="{fmt(S * 0.62)}" cy="{fmt(hz)}" r="{fmt(S * 0.9)}">
      <stop offset="0" stop-color="#7BD66A"/><stop offset="0.18" stop-color="#2E9A5B"/><stop offset="0.6" stop-color="#14605A"/>
      <stop offset="1" stop-color="#0B2F4A"/>
    </radialGradient>
    <radialGradient id="dawn" gradientUnits="userSpaceOnUse" cx="{fmt(S * 0.5)}" cy="{fmt(hz)}" r="{fmt(S * 0.55)}">
      <stop offset="0" stop-color="#FFC56B" stop-opacity="0.55"/><stop offset="0.45" stop-color="#F25C3B" stop-opacity="0.18"/>
      <stop offset="1" stop-color="{DUSK}" stop-opacity="0"/>
    </radialGradient>
    <filter id="rimBlur" filterUnits="userSpaceOnUse" x="-2000" y="-2000" width="6000" height="6000">
      <feGaussianBlur stdDeviation="{fmt(S * 0.012)}"/>
    </filter>
    <linearGradient id="gloss" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#FFFFFF" stop-opacity="0.16"/><stop offset="1" stop-color="#FFFFFF" stop-opacity="0"/>
    </linearGradient>
    {m1d}{m2d}{sd}
  </defs>
  <g clip-path="url(#tile)">
    <rect width="{S}" height="{S}" fill="url(#sky)"/>
    <rect width="{S}" height="{S}" fill="url(#dawn)"/>
    {stars}
    {m1b}
    {m2b}
    {sb}
    <g>{rim}
      <circle cx="{fmt(S / 2)}" cy="{fmt(pcy)}" r="{fmt(pr)}" fill="url(#planet)"/>
      <circle cx="{fmt(S / 2)}" cy="{fmt(pcy)}" r="{fmt(pr - 8)}" fill="none" stroke="#B9F27C" stroke-width="10" opacity="0.45"/>
    </g>
    <rect width="{S}" height="{fmt(S * 0.5)}" fill="url(#gloss)"/>
  </g>
  <rect x="6" y="6" width="{S - 12}" height="{S - 12}" rx="{fmt(rad - 6)}" fill="none" stroke="#BFE6FF"
        stroke-opacity="0.28" stroke-width="{12 if not small else 0}"/>
</svg>
"""


def main() -> None:
    full = wordmark_svg(True, True)
    write_text(ART_DIR / "wordmark.svg", full)
    leaf = wordmark_svg(False, True)
    write_text(ART_DIR / "wordmark-leaf.svg", leaf)
    rasterize(wordmark_svg(True, False), IMG_DIR / "wordmark.png")
    rasterize(leaf, IMG_DIR / "wordmark-leaf.png")

    write_text(ART_DIR / "icon.svg", icon_svg(1024))
    write_text(ART_DIR / "icon-small.svg", icon_svg(32, small=True))
    for n in (1024, 512, 256, 128, 64, 48):
        rasterize(icon_svg(n), IMG_DIR / f"icon-{n}.png")
    for n in (32, 16):
        rasterize(icon_svg(n, small=True), IMG_DIR / f"icon-{n}.png")
    save_png(load_png(IMG_DIR / "icon-256.png"), IMG_DIR / "icon.png")
    print("brand: wordmark.png wordmark-leaf.png icon*.png")


if __name__ == "__main__":
    main()
