#!/usr/bin/env python3
"""Pre-disc backdrop art (DESIGN.md 6.4): seamless star tiles and the toy rocket.

Outputs
  port/launcher/res/img/stars-far.png   512x512, 140 faint dots, tiles seamlessly
  port/launcher/res/img/stars-near.png  512x512, 36 brighter dots, a few tinted, four flares
  port/launcher/art/ship.svg            vector source of the rocket
  port/launcher/res/img/ship.png        512x512 render of ship.svg

Everything is derived from SEED; running the script twice gives identical files.
"""

from __future__ import annotations

import math
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (ART_DIR, BRAND_FILL, ICE, IMG_DIR, NAVY_800, RIM_CYAN, SUN, fmt, rasterize,  # noqa: E402
                    save_png, write_text)

SEED = 20261121
TILE = 512


# ---------------------------------------------------------------------------
# Stars


def _torus_dist(ax, ay, bx, by, n=TILE):
    dx = abs(ax - bx)
    dy = abs(ay - by)
    return math.hypot(min(dx, n - dx), min(dy, n - dy))


def _place(rng: random.Random, count: int, min_gap: float, taken=()):
    pts = []
    tries = 0
    while len(pts) < count:
        tries += 1
        x, y = rng.uniform(0, TILE), rng.uniform(0, TILE)
        gap = min_gap if tries < 20000 else min_gap * 0.5
        if all(_torus_dist(x, y, px, py) >= gap for px, py in list(taken) + pts):
            pts.append((x, y))
    return pts


class Canvas:
    """Float RGBA accumulation buffer (premultiplied) on a torus."""

    def __init__(self, n=TILE):
        self.n = n
        self.r = [0.0] * (n * n)
        self.g = [0.0] * (n * n)
        self.b = [0.0] * (n * n)
        self.a = [0.0] * (n * n)

    def splat(self, px: int, py: int, col, alpha: float):
        if alpha <= 0.002:
            return
        i = (py % self.n) * self.n + (px % self.n)
        # "over" composite of premultiplied colour
        k = 1.0 - alpha
        self.r[i] = col[0] * alpha + self.r[i] * k
        self.g[i] = col[1] * alpha + self.g[i] * k
        self.b[i] = col[2] * alpha + self.b[i] * k
        self.a[i] = alpha + self.a[i] * k

    def star(self, cx, cy, radius, alpha, col, halo=0.0, flare=0.0):
        """Anti-aliased disc (box-filtered by 4x4 supersampling) with optional soft halo and cross flare."""
        reach = int(math.ceil(radius + 1 + max(halo * 3, flare)))
        x0, y0 = int(math.floor(cx)) - reach, int(math.floor(cy)) - reach
        for py in range(y0, y0 + 2 * reach + 2):
            for px in range(x0, x0 + 2 * reach + 2):
                cov = 0.0
                for sy in range(4):
                    for sx in range(4):
                        d = math.hypot(px + (sx + 0.5) / 4 - cx, py + (sy + 0.5) / 4 - cy)
                        if d <= radius:
                            cov += 1
                cov /= 16.0
                dx, dy = px + 0.5 - cx, py + 0.5 - cy
                d = math.hypot(dx, dy)
                a = cov
                if halo > 0:
                    a = max(a, 0.35 * math.exp(-(d * d) / (2 * halo * halo)))
                if flare > 0:
                    for u, v in ((dx, dy), (dy, dx)):
                        if abs(v) < 1.2 and abs(u) < flare:
                            t = 1 - abs(u) / flare
                            a = max(a, 0.75 * t * t * max(0.0, 1 - abs(v) / 1.2))
                self.splat(px, py, col, min(1.0, a) * alpha)

    def image(self):
        from PIL import Image

        n = self.n
        im = Image.new("RGBA", (n, n))
        px = []
        for i in range(n * n):
            a = self.a[i]
            if a <= 0:
                px.append((0, 0, 0, 0))
                continue
            px.append((round(min(1, self.r[i] / a) * 255), round(min(1, self.g[i] / a) * 255),
                       round(min(1, self.b[i] / a) * 255), round(min(1, a) * 255)))
        im.putdata(px)
        return im


def _rgb(hexs: str):
    return tuple(int(hexs[i:i + 2], 16) / 255 for i in (1, 3, 5))


def stars_far(rng: random.Random):
    c = Canvas()
    for x, y in _place(rng, 140, 18):
        r = rng.uniform(0.5, 1.0)
        a = rng.uniform(0.40, 0.80)
        tint = rng.random()
        col = (1, 1, 1) if tint < 0.8 else _rgb("#D6ECFF") if tint < 0.93 else _rgb("#FFF1D0")
        c.star(x, y, r, a, col, halo=r * 1.4)
    return c.image()


def stars_near(rng: random.Random):
    c = Canvas()
    pts = _place(rng, 36, 48)
    flares = set(rng.sample(range(36), 4))
    for i, (x, y) in enumerate(pts):
        r = rng.uniform(1.0, 1.8)
        a = rng.uniform(0.70, 1.00)
        col = (1, 1, 1)
        if i % 6 == 0:
            col = _rgb("#BFE6FF") if (i // 6) % 2 == 0 else _rgb("#FFE27A")
        if i in flares:
            c.star(x, y, 1.8, 1.0, col, halo=2.6, flare=6.0)
        else:
            c.star(x, y, r, a, col, halo=r * 1.3)
    return c.image()


# ---------------------------------------------------------------------------
# Ship


def _fin(attach_x, attach_y, angle_deg, length, root_w, tip_w):
    """A stubby rounded paddle fin hanging from (attach_x, attach_y), rotated by angle_deg."""
    hw, tw = root_w / 2, tip_w / 2
    d = (f"M {fmt(-hw)},-20 L {fmt(hw)},-20 "
         f"C {fmt(hw + 4)},{fmt(length * 0.45)} {fmt(tw + 10)},{fmt(length - tw)} {fmt(tw)},{fmt(length - tw * 0.3)} "
         f"A {fmt(tw)},{fmt(tw)} 0 0 1 {fmt(-tw)},{fmt(length - tw * 0.3)} "
         f"C {fmt(-tw - 10)},{fmt(length - tw)} {fmt(-hw - 4)},{fmt(length * 0.45)} {fmt(-hw)},-20 Z")
    return f'<path d="{d}" transform="translate({fmt(attach_x)} {fmt(attach_y)}) rotate({fmt(angle_deg)})"/>'


def ship_svg() -> str:
    cx = 256
    # Fat teardrop: rounded base, narrowing to a nose that the glass bulb caps.
    body = ("M 256,112 C 300,138 351,236 351,318 C 351,386 309,424 256,424 "
            "C 203,424 161,386 161,318 C 161,236 212,138 256,112 Z")
    fins_side = (_fin(186, 368, 25, 92, 58, 40) + _fin(326, 368, -25, 92, 58, 40))
    fin_mid = ("M 249,356 C 249,348 263,348 263,356 L 276,440 "
               "C 278,452 272,458 262,458 L 250,458 C 240,458 234,452 236,440 Z")
    nozzle = "M 222,410 L 290,410 L 298,446 C 298,456 214,456 214,446 Z"
    light_band = "M 120,90 L 236,90 C 206,190 200,330 232,440 L 120,440 Z"
    dark_band = "M 392,90 L 322,90 C 338,200 340,330 318,440 L 392,440 Z"
    ol = NAVY_800
    silhouette = f"""
    <path d="M 256,76 L 262,56 L 268,36"/>
    <circle cx="270" cy="30" r="11"/>
    {fins_side}
    <path d="{nozzle}"/>
    <path d="{body}"/>
    <path d="{fin_mid}"/>
    <circle cx="{cx}" cy="118" r="45"/>"""
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" viewBox="0 0 512 512">
  <!-- Moonsprout pre-disc rocket. Original artwork, generated by tools/launcher_art/gen_backdrop.py. -->
  <defs>
    <clipPath id="bodyClip"><path d="{body}"/></clipPath>
    <radialGradient id="bulbFill" cx="0.38" cy="0.32" r="0.75">
      <stop offset="0" stop-color="#FFFFFF"/>
      <stop offset="0.45" stop-color="{ICE}"/>
      <stop offset="1" stop-color="#9FCBEB"/>
    </radialGradient>
    <radialGradient id="portFill" cx="0.4" cy="0.35" r="0.8">
      <stop offset="0" stop-color="#2A4C8F"/>
      <stop offset="0.6" stop-color="#14275A"/>
      <stop offset="1" stop-color="{ol}"/>
    </radialGradient>
    <radialGradient id="ballFill" cx="0.35" cy="0.35" r="0.8">
      <stop offset="0" stop-color="#FFF4B8"/>
      <stop offset="0.5" stop-color="{SUN}"/>
      <stop offset="1" stop-color="#E9A91C"/>
    </radialGradient>
    <linearGradient id="nozzleFill" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#6E7FB8"/>
      <stop offset="0.5" stop-color="#4A5A92"/>
      <stop offset="1" stop-color="#2C3A6E"/>
    </linearGradient>
  </defs>

  <!-- faint cyan rim so the navy outline separates from a navy sky -->
  <g fill="{RIM_CYAN}" stroke="{RIM_CYAN}" stroke-width="27" stroke-linejoin="round" stroke-linecap="round"
     opacity="0.4">{silhouette}</g>
  <!-- silhouette outline: 10 px of navy outside every part -->
  <g fill="{ol}" stroke="{ol}" stroke-width="20" stroke-linejoin="round" stroke-linecap="round">{silhouette}</g>

  <!-- antenna -->
  <path d="M 256,80 L 262,56 L 268,38" fill="none" stroke="#C9D8F2" stroke-width="6" stroke-linecap="round"/>
  <circle cx="270" cy="30" r="11" fill="url(#ballFill)"/>
  <circle cx="266.5" cy="26.5" r="3.2" fill="#FFFFFF" opacity="0.85"/>

  <!-- side fins and nozzle sit behind the body -->
  <g fill="{BRAND_FILL}" stroke="{ol}" stroke-width="6" stroke-linejoin="round">{fins_side}</g>
  <path d="M 172,380 C 164,404 154,426 146,440" fill="none" stroke="#F06A8C" stroke-width="7" stroke-linecap="round"/>
  <path d="{nozzle}" fill="url(#nozzleFill)" stroke="{ol}" stroke-width="6" stroke-linejoin="round"/>

  <!-- body with its two shading bands -->
  <path d="{body}" fill="#E63C66"/>
  <g clip-path="url(#bodyClip)">
    <path d="{light_band}" fill="#FF7A98"/>
    <path d="{dark_band}" fill="#9E1F3D"/>
    <!-- belly shadow where the fins attach -->
    <ellipse cx="256" cy="432" rx="120" ry="34" fill="#9E1F3D" opacity="0.45"/>
    <!-- shadow of the bulb on the nose -->
    <ellipse cx="262" cy="166" rx="48" ry="16" fill="#9E1F3D" opacity="0.55"/>
    <!-- specular streak -->
    <path d="M 196,228 C 186,262 184,300 190,336" fill="none" stroke="#FFFFFF" stroke-width="13"
          stroke-linecap="round" opacity="0.55"/>
    <circle cx="198" cy="206" r="6.5" fill="#FFFFFF" opacity="0.55"/>
  </g>
  <path d="{body}" fill="none" stroke="{ol}" stroke-width="6" stroke-linejoin="round"/>

  <!-- porthole -->
  <circle cx="{cx}" cy="282" r="35" fill="{ICE}" stroke="{ol}" stroke-width="6"/>
  <circle cx="{cx}" cy="282" r="25" fill="url(#portFill)" stroke="{ol}" stroke-width="4"/>
  <path d="M 239,276 A 18,18 0 0 1 256,264" fill="none" stroke="#FFFFFF" stroke-width="5.5" stroke-linecap="round" opacity="0.9"/>
  <circle cx="267" cy="294" r="3" fill="#BFE6FF" opacity="0.7"/>

  <!-- front fin -->
  <path d="{fin_mid}" fill="{BRAND_FILL}" stroke="{ol}" stroke-width="6" stroke-linejoin="round"/>
  <path d="M 254,366 L 248,446" fill="none" stroke="#F06A8C" stroke-width="5" stroke-linecap="round"/>

  <!-- glass nose bulb -->
  <circle cx="{cx}" cy="118" r="45" fill="url(#bulbFill)" stroke="{ol}" stroke-width="6"/>
  <path d="M 228,108 A 30,30 0 0 1 250,86" fill="none" stroke="#FFFFFF" stroke-width="8" stroke-linecap="round"/>
  <circle cx="236" cy="124" r="4.5" fill="#FFFFFF"/>
  <path d="M 278,140 A 30,30 0 0 0 290,118" fill="none" stroke="#8DB8DA" stroke-width="4" stroke-linecap="round" opacity="0.8"/>
</svg>
"""


def main() -> None:
    rng = random.Random(SEED)
    save_png(stars_far(random.Random(rng.random())), IMG_DIR / "stars-far.png")
    save_png(stars_near(random.Random(rng.random())), IMG_DIR / "stars-near.png")

    svg = ship_svg()
    write_text(ART_DIR / "ship.svg", svg)
    rasterize(svg, IMG_DIR / "ship.png")
    print("backdrop: stars-far.png stars-near.png ship.png")


if __name__ == "__main__":
    main()
