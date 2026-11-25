"""Shared helpers for the launcher art generators.

- Glyph outlines from the bundled variable font (fontTools), so SVG sources contain
  paths only and render the same everywhere.
- Rasterising SVG through ImageMagick's librsvg delegate.
- Re-saving PNGs through Pillow so the files carry no timestamps (byte-reproducible).
"""

from __future__ import annotations

import io
import math
import os
import subprocess
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ART_DIR = REPO / "port" / "launcher" / "art"
IMG_DIR = REPO / "port" / "launcher" / "res" / "img"
FONT_DIR = REPO / "port" / "launcher" / "res" / "fonts"

# Palette (DESIGN.md 3.2).
NAVY_900 = "#050B1A"
NAVY_800 = "#0B1730"
NAVY_700 = "#1B2E6B"
DUSK = "#6B3A7A"
RIM_CYAN = "#6FB6DE"
GLOW_CYAN = "#5AB8FF"
ICE = "#DDF1FF"
SPROUT = "#8CE04A"
LEAF = "#5FBF3A"
LEAF_DEEP = "#2E7D32"
LEAF_HI = "#B9F27C"
BRAND = "#F0496E"
BRAND_FILL = "#C8284F"
SUN = "#FFD23F"
PIK_RED = "#E8392B"
PIK_BLUE = "#2F7BEA"
PETAL = "#F7B6D2"


# ---------------------------------------------------------------------------
# Fonts


class Font:
    """A static instance of a variable TTF with SVG path extraction."""

    def __init__(self, path: Path, **axes: float):
        from fontTools.ttLib import TTFont
        from fontTools.varLib import instancer

        vf = TTFont(str(path))
        self.font = instancer.instantiateVariableFont(vf, axes) if "fvar" in vf else vf
        self.upem = self.font["head"].unitsPerEm
        self.cmap = self.font.getBestCmap()
        self.glyphset = self.font.getGlyphSet()
        self.hmtx = self.font["hmtx"]
        os2 = self.font["OS/2"]
        self.cap_height = os2.sCapHeight
        self.x_height = os2.sxHeight

    def glyph_name(self, ch: str) -> str:
        return self.cmap[ord(ch)]

    def advance(self, ch: str) -> int:
        return self.hmtx[self.glyph_name(ch)][0]

    def kern(self, a: str, b: str) -> int:
        """Pair kerning from a simple GPOS PairPos lookup (format 1 and 2)."""
        if "GPOS" not in self.font:
            return 0
        ga, gb = self.glyph_name(a), self.glyph_name(b)
        gpos = self.font["GPOS"].table
        total = 0
        for lookup in gpos.LookupList.Lookup:
            for st in lookup.SubTable:
                if lookup.LookupType == 9:
                    st = st.ExtSubTable
                if getattr(st, "LookupType", lookup.LookupType) != 2:
                    continue
                cov = st.Coverage.glyphs
                if ga not in cov:
                    continue
                if st.Format == 1:
                    ps = st.PairSet[cov.index(ga)]
                    for rec in ps.PairValueRecord:
                        if rec.SecondGlyph == gb and rec.Value1 is not None:
                            total += getattr(rec.Value1, "XAdvance", 0) or 0
                elif st.Format == 2:
                    c1 = st.ClassDef1.classDefs.get(ga, 0)
                    c2 = st.ClassDef2.classDefs.get(gb, 0)
                    v = st.Class1Record[c1].Class2Record[c2].Value1
                    if v is not None:
                        total += getattr(v, "XAdvance", 0) or 0
        return total

    def path(self, ch: str, x: float, baseline: float, size: float) -> str:
        """SVG path data for one glyph, y down, origin at (x, baseline)."""
        from fontTools.pens.svgPathPen import SVGPathPen
        from fontTools.pens.transformPen import TransformPen

        s = size / self.upem
        pen = SVGPathPen(self.glyphset, ntos=lambda v: f"{v:.2f}".rstrip("0").rstrip("."))
        tpen = TransformPen(pen, (s, 0, 0, -s, x, baseline))
        self.glyphset[self.glyph_name(ch)].draw(tpen)
        return pen.getCommands()

    def bounds(self, ch: str, x: float, baseline: float, size: float):
        from fontTools.pens.boundsPen import BoundsPen

        bp = BoundsPen(self.glyphset)
        self.glyphset[self.glyph_name(ch)].draw(bp)
        if bp.bounds is None:
            return None
        s = size / self.upem
        x0, y0, x1, y1 = bp.bounds
        return (x + x0 * s, baseline - y1 * s, x + x1 * s, baseline - y0 * s)

    def points(self, ch: str, x: float, baseline: float, size: float):
        """Flattened outline points (for finding stems and terminals)."""
        from fontTools.pens.recordingPen import DecomposingRecordingPen

        rp = DecomposingRecordingPen(self.glyphset)
        self.glyphset[self.glyph_name(ch)].draw(rp)
        s = size / self.upem
        pts = []
        for _op, args in rp.value:
            for px, py in args:
                pts.append((x + px * s, baseline - py * s))
        return pts

    def layout(self, text: str, x: float, baseline: float, size: float, tracking: float = 0.0):
        """Return [(char, x)] pen positions with kerning and tracking (fraction of em)."""
        out = []
        pen = x
        for i, ch in enumerate(text):
            out.append((ch, pen))
            pen += self.advance(ch) * size / self.upem + tracking * size
            if i + 1 < len(text):
                pen += self.kern(ch, text[i + 1]) * size / self.upem
        return out, pen


# ---------------------------------------------------------------------------
# Raster output


def rasterize(svg_text: str, out_png: Path, width: int | None = None, height: int | None = None) -> None:
    """Render SVG text to a PNG via librsvg (ImageMagick's RSVG delegate).

    The SVG's own width/height decide the pixel size unless width/height are given.
    """
    out_png.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "in.svg"
        src.write_text(svg_text, encoding="utf-8")
        tmp_png = Path(td) / "out.png"
        cmd = ["magick", "-background", "none"]
        if width and height:
            cmd += ["-size", f"{width}x{height}"]
        cmd += [f"rsvg:{src}", f"PNG32:{tmp_png}"]
        subprocess.run(cmd, check=True)
        save_png(load_png(tmp_png), out_png)


def load_png(path: Path):
    from PIL import Image

    im = Image.open(path)
    im.load()
    return im.convert("RGBA")


def save_png(im, path: Path) -> None:
    """Save without timestamps or text chunks so output bytes depend only on pixels."""
    path.parent.mkdir(parents=True, exist_ok=True)
    buf = io.BytesIO()
    im.save(buf, format="PNG", optimize=True)
    data = buf.getvalue()
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def resize(im, size: int):
    from PIL import Image

    return im.resize((size, size), Image.LANCZOS)


def fmt(v: float) -> str:
    s = f"{v:.2f}".rstrip("0").rstrip(".")
    return "0" if s == "-0" else s


def polar(cx: float, cy: float, r: float, deg: float):
    a = math.radians(deg)
    return cx + r * math.cos(a), cy + r * math.sin(a)


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")
