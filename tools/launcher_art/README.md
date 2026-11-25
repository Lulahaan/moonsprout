# Launcher art generator

Generates every image the Moonsprout launcher uses (docs/launcher/DESIGN.md 1.3, 1.4, 6.4).
All of it is original artwork drawn as code; nothing is traced from or derived from game data.

| Script | Writes |
|---|---|
| `gen_backdrop.py` | `res/img/stars-far.png`, `res/img/stars-near.png`, `art/ship.svg`, `res/img/ship.png` |
| `gen_brand.py` | `art/wordmark.svg`, `art/wordmark-leaf.svg`, `art/icon.svg`, `art/icon-small.svg`, `res/img/wordmark.png`, `res/img/wordmark-leaf.png`, `res/img/icon.png`, `res/img/icon-{16,32,48,64,128,256,512,1024}.png` |
| `preview.py OUT_DIR` | `OUT_DIR/preview-1080.png`, `OUT_DIR/preview-2160.png`: the pre-disc screen approximated in SVG, for checking the art. Not shipped |
| `common.py` | Shared helpers: palette, glyph outlines, rasterising, timestamp-free PNG writing |

`res/` and `art/` are `port/launcher/res/` and `port/launcher/art/`.

## Regenerating

Requirements: Python 3.10+, `fonttools` and `pillow`, and ImageMagick 7 (`magick`) built with
the librsvg delegate (`magick -list format | grep RSVG`).

```sh
python3 -m venv /tmp/artenv && /tmp/artenv/bin/pip install -r tools/launcher_art/requirements.txt
/tmp/artenv/bin/python tools/launcher_art/gen_backdrop.py
/tmp/artenv/bin/python tools/launcher_art/gen_brand.py
/tmp/artenv/bin/python tools/launcher_art/preview.py /tmp/artpreview   # optional
```

The output is deterministic: random placement uses the fixed `SEED` in each script, PNGs are
written by Pillow without timestamps, and a file is only rewritten when its bytes change. Running
both scripts on a clean checkout leaves `git status` clean (with the same librsvg version; another
version may change anti-aliasing by a level or two).

Edit the scripts, not the SVGs: the SVGs in `port/launcher/art/` are outputs, kept as editable
vector sources and for review. The RmlUi build has no SVG plugin, so only the PNGs ship.

## The assets

| File | Size | What it is |
|---|---|---|
| `wordmark.png` | 1680x420 | "Moonsprout" in Gluten 700, outlined. "Moon" is moonlight white, "sprout" leaf green; each letter has a navy outline, a darker extrusion 9 px down, a glass shine on its top 40 % and a soft glow (cyan behind "Moon", green behind "sprout"). Two alien moons float over "Moon" (a large ice-blue cratered one and a small pink one) with three small sparkles. Shown at 420 dp wide, so it holds up at 400 % UI scale |
| `wordmark-leaf.png` | 1680x420 | Only the two-leaf seedling growing from the top of the "t", with its glow, on the same canvas. Stack it on `wordmark.png` in the same box; animate it with `transform-origin` at the stem base (about 87 %, 45 % of the box) |
| `stars-far.png` | 512x512 | 140 faint stars (radius 0.5 to 1 px, 40 to 80 % alpha), a few with a blue or warm tint. Wraps on all edges, so it tiles seamlessly with `image(... repeat)` |
| `stars-near.png` | 512x512 | 36 brighter stars (radius 1 to 1.8 px), one in six tinted `#BFE6FF` or `#FFE27A`, four with a 6 px cross flare. Seamless |
| `ship.png` | 512x512 | The pre-disc rocket: a fat pink-red teardrop body with light and dark bands, a glass nose bulb, one porthole, three stubby fins (two splayed 25 degrees, one in front), a short nozzle, an antenna with a yellow ball, a 10 px navy outline and a faint cyan rim. Upright and centred; the nozzle sits at the bottom centre for `bd-flame`. No engine pods, side lamps or lettering (DESIGN 6.4) |
| `icon.png` | 256x256 | App icon, same as `icon-256.png` |
| `icon-<n>.png` | n x n | App icon: rounded square (22.5 % radius) with a navy-to-dusk sky, stars, the two moons, a seedling growing from a green planet horizon lit from behind, and a faint cyan rim. 16 and 32 use `icon-small.svg`: no stars, one moon, no rim glow, a larger seedling |

Notes for the UI:

- The two wordmark PNGs register pixel for pixel. The seedling's stem base is clipped at the
  top of the "t" outline, so the stem grows out of the letter when stacked.
- The planet horizon is not an image: DESIGN 6.4 builds it from RCSS gradients (`bd-planet`).
- Material Symbols glyphs cover the UI icons (DESIGN 3.7); no icon images are needed for them.

## Fonts and licences

The wordmark's letters are Gluten (SIL Open Font License 1.1, `port/launcher/res/fonts/Gluten-VF.ttf`,
licence in `port/launcher/res/fonts/licenses/Gluten-OFL.txt`), instanced at wght 700 and converted
to outlines with fontTools. Artwork made from a font's outlines is not Font Software under the OFL,
so the images carry no font licence obligation. Everything else is drawn by these scripts and
falls under the repository's licence.
