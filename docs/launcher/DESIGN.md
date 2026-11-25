# Launcher design system

Status: spec, not yet implemented. Nothing in `port/`, `src/` or `include/` was changed to write this. The only files added are the fonts and licence texts listed in section 3.6.

Path shorthand used below:

- `M:` = `<this repository>/`
- `D:` = `<a Dusklight checkout>/`
- `R:` = `M:build-pc/_deps/rmlui-src/` (the RmlUi fork Aurora pins)
- `rml/` = `M:port/launcher/res/rml/`

All sizes are in `dp`. The reference frame is 1920x1080 dp.

## Review notes

Changed in the cross-spec review (see `README.md` in this folder):

- 6.2: composition numbers aligned with the camera in SCENE.md 8.2 (pilot's head higher, ship centre
  a range).
- 6.6: with the Original aspect the UI cannot draw in the pillars; the start menu forces the window
  shape instead (ASPECT.md 5). Open question 5 is answered.
- 7.1 to 7.3: cue names and times re-timed to the real take-off (SCENE.md 11). After the take-off
  rework the full launch is about 17.6 s to the title (15.25 s of scene), On or Off, skippable with
  any button. Open question 6 is answered.
- 7.3, 10: the setting is `interface.launchAnimation` (the `prelaunch.` prefix does not exist);
  all four new keys are declared in SETTINGS.md 9.1.
- 8.1: the bar's entries are the registry of SETTINGS.md 13.

## 0. Constraints that shape the design

These were checked in the tree and decide what the spec can ask for.

| Fact | Where | Consequence |
|---|---|---|
| No `::before` / `::after` pseudo-elements | no pseudo-element support in `R:Source/Core` | Every decorative part (toggle knob, sprout marker, slider fill) is a real child element created in C++. |
| No `calc()`, no `env()` | `R:Source/Core/StyleSheetSpecification.cpp` | Layout uses `vw`/`vh`, `%`, `dp` and media queries only. |
| No `prefers-reduced-motion` media feature. Features are width, height, aspect-ratio, resolution, orientation and their min/max forms | `R:Source/Core/StyleSheetParser.cpp:254-262` | Reduced motion is a setting that puts a class on `body`. |
| SVG plugin is off (`RMLUI_SVG_PLUGIN:BOOL=OFF`) | `M:build-pc/CMakeCache.txt:1028` | Art is authored as SVG but shipped as PNG, or built from RCSS gradients. |
| Tweens available: back, bounce, circular, cubic, elastic, exponential, linear, quadratic, quartic, quintic, sine, each `-in`, `-out`, `-in-out`. No `cubic-bezier` | `R:Source/Core/PropertyParserAnimation.cpp:33-75` | Motion tokens are tween names. |
| `box-shadow` accepts `inset` | `R:Source/Core/PropertyParserBoxShadow.cpp:66-68` | Glass bevels can be drawn with inset shadows. |
| `glass()` filter: `glass(<bezel> <refraction> <specular> <tint> <saturation> <profile> <dome> <edges>)`, usable as `backdrop-filter` | `M:extern/aurora/lib/rmlui/GlassFilter.hpp:10`, `GlassFilter.cpp:46-57`, registered `M:extern/aurora/lib/rmlui.cpp:371-372` | Real refracting glass orbs are available. Nothing in Dusklight uses it, so treat it as untested (open question 3). |
| Image decorator fit modes include `repeat`, `repeat-x`, `repeat-y` | `R:Source/Core/DecoratorTiled.cpp:232` | Star fields can tile a small PNG. |
| `letter-spacing`, `mask-image`, radial and conic gradients exist | `R:Source/Core/StyleSheetSpecification.cpp:358,405`, `R:Source/Core/Factory.cpp:208-212` | Used below. |
| Variable fonts load one face per named weight instance; width is fixed to the instance nearest 100 | `R:Source/Core/FontEngineDefault/FontProvider.cpp:124-200` | The variable TTFs in section 3.6 work with numeric `font-weight`. |
| `var()` is a textual substitution | `R:Source/Core/ElementStyle.cpp:165` | Tokens can hold whole values. Use inside a `transition` shorthand is unverified (open question 4). |
| UI scale is `interface.uiScale`, percent of display scale, 0 = automatic, max 200 | `M:port/launcher/settings.cpp:13`, applied at `M:port/launcher/ui/ui.cpp:601-610` | Section 8.3 changes the automatic rule. |
| Sheets are linked per document: prelaunch links `theme` + `prelaunch`; windows link `theme`, `controls`, `tabbing`, `window`; popovers link `theme`, `controls`, `popover`; `global.rcss` is merged into every document | `M:port/launcher/prelaunch.cpp:19-20`, `M:port/launcher/ui/window.cpp:36-39,52-54`, `M:port/launcher/ui/popover.cpp:11-13`, `M:port/launcher/ui/document.cpp:21` | Tokens go in `theme.rcss`; shared component rules go in `global.rcss` and `controls.rcss`. |

## 1. Name and logo

### 1.1 Name ideas

None of these contains a Nintendo trademark or an in-game proper noun. Each was checked with one web search for an app, game or software product of that name. That is a collision check, not a trademark search (open question 1).

| # | Name | Rationale | Check |
|---|---|---|---|
| 1 | **Sproutlight** | A sprout glowing at dusk. It also nods to Dusklight, whose UI framework this launcher ports. | No product found. Nearest are "Sproutle" and "Modern Sprout". |
| 2 | **Hundredfold** | A hundred followers on the field, and a harvest multiplied. | No software or game found. |
| 3 | **Leafwhistle** | The two things you do all day: call them, and watch the leaves follow. | No product found. |
| 4 | **Pluckwork** | Plucking sprouts, plus "handiwork". Sounds like a tool. | No product found. Nearest is the game "Pluck: A Berry Sweet Adventure". |
| 5 | **Bloomtide** | Leaf, bud, flower: the tide of a growing squad. | No product found. |
| 6 | **Second Sprout** | The "2" without naming the game. | No product found. Nearest is an upcoming puzzle game, "The First Sprout". |
| 7 | **Whistlebloom** | Whistle plus flower stage; the most toy-like of the set. | No product found. |
| 8 | **Stemward** | "Homeward", by stem: get everyone back before sundown. | No product found. |

Rejected:

- **Sproutbound**: a Steam game of that name released in 2025.
- **Budburst**: I recall a plant-science project of this name; not re-checked.
- **Dandori**: a term Nintendo uses to market the series.
- Anything built on Onion, Hocotate, Poko or Pellet Posy: in-game proper nouns.

### 1.2 Recommendation: Sproutlight

It is short, pronounceable, visually suggestive (a leaf that glows), and states the lineage honestly. The repo and binary stay `mppm`; Sproutlight is the product name shown in the window title, the wordmark and the release notes.

Usage rules:

- Window title: `Sproutlight`.
- Version string: `Sproutlight v0.4.0`.
- The game's title appears only in descriptive body text, for example "Disc ready: Pikmin 2 (USA)". It never appears in the wordmark, the icon or the eyebrow.
- `M:port/launcher/prelaunch.cpp:27-28` currently renders `<eyebrow>A native PC port of</eyebrow><wordmark>Pikmin 2</wordmark>`. Replace both: the eyebrow becomes `A native port for your own disc`, the wordmark becomes the art below.
- About text carries: "Not affiliated with or endorsed by Nintendo. Requires your own copy of the game."

### 1.3 Wordmark

Authored as SVG, exported to PNG. Source file `M:port/launcher/art/wordmark.svg`; exports `M:port/launcher/res/img/wordmark.png` (1680x420 px) and `wordmark-leaf.png` (the leaf as a separate layer so it can animate).

- **Canvas:** viewBox `0 0 1680 420`, transparent.
- **Text:** `Sproutlight`, capital S then lowercase. Gluten, weight 700, slant 0, size 250 px, tracking -2%. Baseline at y = 300. Convert to outlines. Gluten is OFL; outlines used in artwork are not font software, so the logo carries no font licence obligation.
- **Letter fill:** vertical gradient `#FFFFFF` at the cap height to `#D6ECFF` at the baseline.
- **Glass highlight:** on each letter, a white shape at 35% opacity covering the top 38% of the letter, clipped to the letter and inset 10 px, with its lower edge an arc. This is the game's glass-bubble shine, redrawn.
- **Outline:** 16 px stroke in `#0B1730`, round joins, painted behind the fill.
- **Outer glow:** Gaussian blur 18 px of the outline shape in `#5AB8FF` at 55% opacity, behind everything.
- **The "i":** remove the tittle. In its place draw a sprout:
  - Stem: a 12 px round-capped stroke in `#5FBF3A`, starting at the top centre of the i's stem, rising 70 px and curving 14 px to the right.
  - Leaf: a teardrop 120x66 px attached at the stem tip, rotated -28 degrees (tip pointing up and to the right). Fill is a linear gradient along its length from `#B9F27C` (tip) to `#5FBF3A` (base). A 4 px midrib in `#2E7D32` at 60% opacity runs along 70% of its length. Outline 10 px `#0B1730`.
  - Glow: blur 22 px of the leaf in `#B9F27C` at 70% opacity, behind the leaf.
  - `wordmark-leaf.png` contains only stem, leaf and glow, on the same canvas, so the two PNGs register when stacked.
- **Pellets:** three circles of 26 px diameter under the "ght", centres 44 px apart, on the line y = 360: `#E8392B`, `#FFD23F`, `#2F7BEA`. Each has a 6 px `#0B1730` outline and an 8 px white highlight dot at its top left at 60% opacity. They are plain dots, not characters.

Until the PNG exists, the wordmark is live text and needs no art:

```css
wordmark {
    display: block;
    font-family: Gluten;
    font-weight: 700;
    font-size: 72dp;
    line-height: 1;
    color: #F7FBFF;
    font-effect: outline(3dp #0B1730), glow(0dp 10dp 0dp 0dp #5AB8FF8C);
}
```

### 1.4 App icon

`M:port/launcher/art/icon.svg`, exported at 1024, 256, 128, 64, 48, 32 and 16 px.

- Rounded square, corner radius 22.5% of the side.
- Background: vertical gradient `#050B1A` (top) to `#1B2E6B` (70%) to `#6B3A7A` (bottom).
- A planet horizon: a circle 1.9x the icon width, centred below the icon so its top arc crosses at 72% of the height. Fill `#2E9A5B` to `#14605A`. A 3%-wide rim glow in `#7FD8FF` at 50%.
- Centred on the horizon: one sprout. Stem 6% of the icon width, 30% tall, `#5FBF3A`. One leaf, 34% of the width, rotated -28 degrees, gradient `#B9F27C` to `#5FBF3A`, with a soft `#B9F27C` glow.
- Three pellets (red, yellow, blue) at the base of the stem, each 7% of the width.
- At 32 px and below, drop the pellets and the rim glow.

## 2. Design principles

1. **Glass over a living world.** Panels are smoked navy glass with a cyan rim and a soft top shine, like the game's HUD bubbles. The scene behind stays visible.
2. **Round and soft.** Pills, orbs and large radii. No sharp corners anywhere.
3. **Things grow.** State changes sprout, bloom or get plucked: short overshoot tweens, never long ones.
4. **Colour means something.** Sprout green is focus and "on". Ship pink-red is the one primary action. Sun yellow is "changed" and "new". Red, yellow and blue as a trio appear only as small accents.
5. **Nothing from the disc ships.** Every shape here is drawn from scratch or built from gradients.

## 3. Tokens (`rml/theme.rcss`)

Same structure and variable names as Dusklight's `D:res/rml/theme.rcss:1-74` (mirrored today at `rml/theme.rcss:1-74`), so `controls.rcss`, `window.rcss`, `popover.rcss`, `overlay.rcss` and `tabbing.rcss` keep resolving. New names are marked `new`.

### 3.1 The file

```css
body {
    /* ---- type ---- */
    --font-family-body: Nunito;
    --font-family-heading: Fredoka;
    --font-family-display: Gluten;
    --font-family-monospace: Noto Mono;
    --font-family-icons: Material Symbols Rounded;

    --font-size-3xs: 12dp;   /* was 11: nothing renders below 12dp */
    --font-size-2xs: 13dp;
    --font-size-xs: 14dp;
    --font-size-sm: 15dp;
    --font-size-base: 16dp;
    --font-size-md: 17dp;
    --font-size-lg: 18dp;
    --font-size-xl: 19dp;
    --font-size-2xl: 20dp;
    --font-size-3xl: 22dp;
    --font-size-4xl: 24dp;
    --font-size-5xl: 28dp;
    --font-size-6xl: 34dp;
    --font-size-hero: 72dp;          /* new */

    --font-weight-body: 600;         /* new: Nunito SemiBold is the body weight */
    --font-weight-strong: 800;       /* new */
    --font-weight-heading: 600;      /* new: Fredoka SemiBold */
    --font-weight-label: 500;        /* new: Fredoka Medium */

    /* ---- spacing ---- */
    --space-2xs: 2dp;
    --space-xs: 4dp;
    --space-sm: 8dp;
    --space-md: 12dp;
    --space-lg: 16dp;
    --space-xl: 24dp;
    --space-2xl: 32dp;
    --space-3xl: 48dp;               /* new */
    --space-4xl: 64dp;               /* new */
    --space-edge: 96dp;              /* new: launcher margin at 1080p */

    /* ---- radii ---- */
    --radius-small: 8dp;
    --radius-control: 12dp;
    --radius-panel: 18dp;
    --radius-window: 28dp;
    --radius-row: 16dp;              /* new: settings rows */
    --radius-pill: 999dp;            /* new */

    --toolbar-height: 64dp;
    --toolbar-hidden-offset: -88dp;  /* bar is floating: 64 + 16 margin + 8 shadow */

    /* ---- colour: core ---- */
    --color-text-rgb: 238, 247, 255;          /* #EEF7FF glass white */
    --color-text: rgb(var(--color-text-rgb));
    --color-accent-rgb: 140, 224, 74;         /* #8CE04A sprout green */
    --color-accent: rgb(var(--color-accent-rgb));
    --color-border-rgb: 111, 182, 222;        /* #6FB6DE glass rim cyan */
    --color-border: rgb(var(--color-border-rgb));
    --color-surface-rgb: 11, 23, 48;          /* #0B1730 deep-space navy */
    --color-interactive-rgb: 191, 230, 255;   /* #BFE6FF glass highlight */
    --color-control-rgb: 6, 14, 34;           /* #060E22 */
    --color-neutral-rgb: 191, 230, 255;       /* tab bar wash: same as interactive */
    --color-neutral: rgb(var(--color-neutral-rgb));
    --color-white-rgb: 255, 255, 255;
    --color-white: rgb(var(--color-white-rgb));
    --color-black-rgb: 5, 11, 26;             /* #050B1A: "black" is space navy */
    --color-black: rgb(var(--color-black-rgb));

    /* ---- colour: brand and world (new) ---- */
    --color-brand-rgb: 240, 73, 110;          /* #F0496E ship pink-red: glows, large type */
    --color-brand: rgb(var(--color-brand-rgb));
    --color-brand-fill-rgb: 200, 40, 79;      /* #C8284F: fill under white text */
    --color-brand-fill-hi: #E63C66;
    --color-sun-rgb: 255, 210, 63;            /* #FFD23F */
    --color-sun: rgb(var(--color-sun-rgb));
    --color-raised-rgb: 19, 38, 74;           /* #13264A: popovers, rows on hover */
    --color-ink: #0A1A0C;                     /* text on green or yellow fills */
    --color-muted: #9FB4CC;

    --color-pik-red: #E8392B;
    --color-pik-yellow: #FFD23F;
    --color-pik-blue: #2F7BEA;
    --color-pik-blue-hi: #6FA8FF;             /* blue as text */
    --color-pik-purple: #8A4FC9;
    --color-pik-purple-hi: #B78BEA;           /* purple as text */
    --color-pik-white: #F3EEF7;
    --color-leaf: #5FBF3A;
    --color-leaf-deep: #2E7D32;
    --color-leaf-hi: #B9F27C;
    --color-petal: #F7B6D2;
    --color-onion-red: #B8283A;
    --color-band-blue: #0A1FB8;
    --color-space-900: #050B1A;
    --color-space-800: #0B1730;
    --color-space-700: #1B2E6B;
    --color-sunset-1: #FFC56B;
    --color-sunset-2: #FF8A3D;
    --color-sunset-3: #F25C3B;
    --color-dusk: #6B3A7A;

    /* ---- colour: status ---- */
    --color-success-rgb: 111, 214, 73;        /* #6FD649 */
    --color-success: rgb(var(--color-success-rgb));
    --color-info-rgb: 90, 184, 255;           /* #5AB8FF */
    --color-info: rgb(var(--color-info-rgb));
    --color-warning-rgb: 255, 177, 59;        /* #FFB13B */
    --color-warning: rgb(var(--color-warning-rgb));
    --color-error-rgb: 255, 92, 108;          /* #FF5C6C: icons, rims */
    --color-error: rgb(var(--color-error-rgb));
    --color-error-text: #FF8A94;              /* new: error as small text */

    --color-danger-border: #9E1F3D;
    --color-danger-heading: #FF8A94;

    /* ---- buttons ---- */
    --button-color: var(--color-text);
    --button-background: rgba(var(--color-control-rgb), 45%);
    --button-background-hover: rgba(var(--color-interactive-rgb), 16%);
    --button-background-selected: rgba(var(--color-interactive-rgb), 28%);
    --button-background-active: rgba(var(--color-accent-rgb), 30%);

    --toast-border-color: var(--color-border);
    --toast-heading-color: var(--color-text);

    /* ---- surfaces (new) ---- */
    --surface-window: rgba(var(--color-surface-rgb), 86%);
    --surface-popover: rgba(var(--color-raised-rgb), 96%);
    --surface-bar: rgba(var(--color-surface-rgb), 80%);
    --surface-toast: rgba(var(--color-surface-rgb), 88%);
    --surface-shine: linear-gradient(180deg, rgba(255, 255, 255, 14%) 0%, rgba(255, 255, 255, 3%) 38%, rgba(255, 255, 255, 0%) 60%);
    --blur-panel: blur(14dp);
    --blur-bar: blur(10dp);

    /* ---- shadows and glows (new) ---- */
    --rim: 2dp;
    --shadow-window: 0 18dp 48dp 0 rgba(5, 11, 26, 60%), 0 0 28dp 2dp rgba(var(--color-info-rgb), 18%), inset 0 1dp 0 0 rgba(255, 255, 255, 30%), inset 0 -10dp 24dp 0 rgba(5, 11, 26, 35%);
    --shadow-popover: 0 8dp 28dp 2dp rgba(5, 11, 26, 60%), inset 0 1dp 0 0 rgba(255, 255, 255, 24%);
    --shadow-control: 0 0 0 1dp rgba(var(--color-border-rgb), 28%), inset 0 1dp 0 0 rgba(255, 255, 255, 10%);
    --shadow-focus: 0 0 0 2dp rgb(var(--color-accent-rgb)), 0 0 0 4dp rgba(5, 11, 26, 85%), 0 0 16dp 3dp rgba(var(--color-accent-rgb), 45%);
    --shadow-orb: 0 6dp 18dp 0 rgba(5, 11, 26, 60%), inset 5dp 7dp 12dp 0 rgba(255, 255, 255, 18%), inset -6dp -10dp 18dp 0 rgba(5, 11, 26, 50%);
    --glow-text: glow(0dp 4dp 0dp 2dp rgba(5, 11, 26, 85%));
    --glow-text-cyan: glow(0dp 8dp 0dp 0dp rgba(90, 184, 255, 55%));

    /* ---- motion (new) ---- */
    --dur-press: 0.09s;
    --dur-fast: 0.12s;
    --dur-base: 0.2s;
    --dur-panel: 0.26s;
    --dur-sprout: 0.32s;
    --dur-bloom: 0.5s;
    --dur-scene: 0.8s;
    --stagger: 0.07s;
    --ease-standard: cubic-out;
    --ease-exit: cubic-in;
    --ease-move: cubic-in-out;
    --ease-pop: back-out;
    --ease-pluck: back-in;
    --ease-bloom: elastic-out;
    --ease-drop: bounce-out;
    --ease-drift: sine-in-out;
}
```

### 3.2 Where each colour comes from

Sampled values are from the earlier screenshot study of the running port. "Brightened" means the sampled pixel was lit or shaded in-game and was raised to work as a flat UI colour.

| Token | Hex | Source in the game's look |
|---|---|---|
| surface / space-800 | `#0B1730` | File-select deep space (`#020A10`, `#071018`), lifted toward the mode-screen navy `#1E2759` so panels separate from a black backdrop |
| space-900 / black | `#050B1A` | File-select background |
| space-700 | `#1B2E6B` | Mode-screen blue `#1E2759`, `#294673` |
| band-blue | `#0A1FB8` | File-select glowing band, core `#020C9C`, brightened |
| border (rim cyan) | `#6FB6DE` | File-select panel rim `#275C90`, highlight `#5699B3`, brightened |
| interactive | `#BFE6FF` | The white-cyan shine on glass HUD bubbles |
| text | `#EEF7FF` | HUD text white with the cyan cast of the glow |
| accent (sprout) | `#8CE04A` | Pause-menu ring light `#78D400`, `#B3E60E`, pulled toward leaf green |
| leaf / leaf-deep / leaf-hi | `#5FBF3A` / `#2E7D32` / `#B9F27C` | Leaf and dialogue-text greens `#81BE71`, `#59C776` |
| brand | `#F0496E` | Ship magenta `#9D3664` and the freight-sign coral `#D86060`, brightened |
| brand-fill | `#C8284F` | Same hue, darkened until white text passes 4.5:1 |
| onion-red | `#B8283A` | Lit Onion `#B42424` and the red glass `#95272B`, `#A03341` |
| sun | `#FFD23F` | Sun meter `#B49018`, coin `#FAFA6B`; also the yellow of the trio |
| pik-red / blue / purple / white | `#E8392B` / `#2F7BEA` / `#8A4FC9` / `#F3EEF7` | Design picks: saturated, flat versions of the character colours |
| petal | `#F7B6D2` | Flower stage |
| sunset-1/2/3, dusk | `#FFC56B` / `#FF8A3D` / `#F25C3B` / `#6B3A7A` | Design picks for the end-of-day sky. No screenshot of a sunset was available |

### 3.3 Contrast

WCAG 2.x ratios, computed. "Worst case" composites the translucent surface over a bright sky (`#CFE8FF`) with no blur, which is harsher than anything the scene should produce.

| Foreground | Background | Ratio | Use |
|---|---|---|---|
| text `#EEF7FF` | surface `#0B1730` | 16.4 | body |
| text | control `#060E22` | 17.7 | fields |
| text | raised `#13264A` | 13.8 | popovers |
| text | window surface at 86% over bright sky (about `#283650`) | about 11 | worst case, windows |
| text | bar surface at 80% over bright sky `#324159` | 9.5 | worst case, in-game bar |
| text at 72% (secondary) | surface | 8.9 | help text, captions |
| text at 72% | bar worst case | 5.8 | |
| text at 55% (tertiary) | surface | 5.7 | section headings, placeholders |
| text at 38% (disabled) | surface | 3.4 | disabled only; exempt |
| muted `#9FB4CC` | surface / bar worst case | 8.4 / 4.9 | version, disc id |
| accent `#8CE04A` | surface / bar worst case | 10.9 / 6.3 | "On", selected tab |
| sun `#FFD23F` | surface / bar worst case | 12.3 / 7.1 | modified values, update pill |
| border `#6FB6DE` | surface | 8.0 | rims, icons |
| info `#5AB8FF` | surface | 8.3 | |
| success `#6FD649` | surface | 9.7 | |
| warning `#FFB13B` | surface | 9.8 | |
| error-text `#FF8A94` | surface / bar worst case | 7.9 / 4.6 | error text |
| error `#FF5C6C` | surface | 5.9 | icons and rims; text only at 18dp and up |
| brand `#F0496E` | surface | 5.0 | 24dp and larger, or glows; never small text |
| white | brand-fill `#C8284F` | 5.4 | Play button label |
| white | brand-fill-hi `#E63C66` | about 4.0 | top of the Play gradient; label is 34dp bold, needs 3.0 |
| ink `#0A1A0C` | accent `#8CE04A` | 11.0 | toggle knob, green buttons |
| ink | sun `#FFD23F` | 12.5 | update pill, slider knob |
| text | hover fill (interactive 16% on surface, `#283851`) | 10.9 | hovered row |
| text | selected fill (interactive 28%, `#3D516A`) | 7.5 | selected row |
| text | launcher scrim at 82% over white `#323743` | 11.0 | menu column, worst case |
| pik-red / pik-blue / pik-purple | surface | 4.3 / 4.4 / 3.4 | decoration only. For text use `--color-error-text`, `--color-pik-blue-hi` (7.4), `--color-pik-purple-hi` (6.7) |

Rules that follow:

- Body text is always `--color-text` at 100% or 72%. Nothing readable sits below 55%.
- The three character colours are never text.
- The focus ring is two rings: 2dp sprout green, then 2dp navy outside it. Sprout green on a bright sky is only 1.3:1; the navy ring is 14:1 against the same sky.

### 3.4 Typography

| Role | Family | Weight | Size | Case and tracking |
|---|---|---|---|---|
| Wordmark fallback, hero numerals | Gluten | 700 | 72dp | as written |
| Eyebrow above the wordmark | Fredoka | 500 | 20dp | sentence case, `letter-spacing: 0.02em` |
| Launcher menu items | Fredoka | 600 | 28dp; Play 34dp | Title Case |
| Tabs | Fredoka | 600 | 19dp | Title Case, `0.02em` |
| Setting names (`select-button key`) | Fredoka | 500 | 19dp | sentence case |
| Setting values | Nunito | 700; 800 when modified | 19dp | |
| Section headings in panes | Fredoka | 600 | 14dp | UPPERCASE, `0.08em`, text at 55% |
| Body, help pane, modal body | Nunito | 600 | 18dp in panes, 17dp in modals | line-height 1.4 |
| Buttons | Fredoka | 600 | 19dp | Title Case |
| Captions, disc id, version | Nunito | 600 | 15dp | |
| Tooltip | Nunito | 600 | 15dp | |
| Paths, hashes, hex colours | Noto Mono | 400 | 15dp | |

Decisions:

- Dusklight sets headings and setting names in uppercase condensed type (`rml/controls.rcss:58-63`, `rml/tabbing.rcss:7`, `rml/prelaunch.rcss:135`). Fredoka is wide, and the game's own menus are Title Case, so `text-transform: uppercase` is removed from tabs, setting names and menu buttons. It stays only on the 14dp section headings.
- Tab width check: the eight tab labels total 52 characters, about 11.5dp each at 19dp, plus 8 x 40dp padding, comes to about 920dp. The window is 1120dp wide, so they fit without scrolling at 100%.
- Nunito 600 is the body weight because 400 looks thin over a moving scene.
- Numbers in values use Nunito's default figures. Slider value boxes get `min-width: 56dp; text-align: right` so they do not jitter.

### 3.5 Font loading

Replace `M:port/launcher/ui/ui.cpp:92-97` with:

```cpp
load_font("Nunito-VF.ttf", true);   // fallback face
load_font("Fredoka-VF.ttf");
load_font("Gluten-VF.ttf");
load_font("MaterialSymbolsRounded-Regular.ttf");
load_font("NotoMono-Regular.ttf");
```

With the default weight argument RmlUi registers one face per named instance: Fredoka 300-700, Nunito 200-900, Gluten 100-900. Glyph atlases are built per face and size on first use, so unused weights cost little. The four Fira Sans files are then unused. Remove them together with their two licence files, which saves 1.8 MB.

### 3.6 Fonts and licences on disk

Downloaded from `raw.githubusercontent.com/google/fonts/main/ofl/<family>/`. The upstream file names contain brackets (`Fredoka[wdth,wght].ttf`), so the files were renamed; the font data is unmodified. Renaming a file is not a modification under the OFL, and none of the three licences declares a Reserved Font Name.

| File in `M:port/launcher/res/fonts/` | Upstream name | Size | SHA-256 (first 16) | Licence file in `fonts/licenses/` |
|---|---|---|---|---|
| `Fredoka-VF.ttf` | `ofl/fredoka/Fredoka[wdth,wght].ttf` | 159,184 | `2ba02e68b152868a` | `Fredoka-OFL.txt`: "Copyright 2016 The Fredoka Project Authors", OFL 1.1 |
| `Nunito-VF.ttf` | `ofl/nunito/Nunito[wght].ttf` | 276,932 | `bb55a5ca5c204233` | `Nunito-OFL.txt`: "Copyright 2014 The Nunito Project Authors", OFL 1.1 |
| `Gluten-VF.ttf` | `ofl/gluten/Gluten[slnt,wght].ttf` | 349,672 | `cb4b01e4754e3661` | `Gluten-OFL.txt`: "Copyright 2020 The Gluten Project Authors", OFL 1.1 |
| `MaterialSymbolsRounded-Regular.ttf` (already present) | | 1,196,044 | | `MaterialSymbolsRounded-Apache-2.0.txt`: the `LICENSE` file of `google/material-design-icons`, Apache License 2.0 |
| `FiraSans-*.ttf`, `FiraSansCondensed-*.ttf` | | | | Removed by W1 with their two licence files |
| `NotoMono-Regular.ttf` (already present) | | 107,848 | | `NotoMono-OFL.txt`: written from the font's name table ("Copyright 2007 Google Inc.", OFL 1.1) plus the standard OFL 1.1 text |

Verification done: each OFL file contains the line "SIL OPEN FONT LICENSE Version 1.1"; each family's `METADATA.pb` says `license: "OFL"`; `fc-scan` reports the expected family names and named instances for the three variable fonts.

`M:CMakeLists.txt:179` copies `port/launcher/res` next to the binary, so `fonts/licenses/` ships automatically.

### 3.7 Icons

Icons are Material Symbols Rounded glyphs drawn with the `text("&#x....;" center center)` decorator, as in `rml/global.rcss:59-106`.

Codepoints already used in the tree and known to render correctly: check `e5ca`, close `e5cd`, chevron left `e5cb`, chevron right `e5cc`, arrow forward `e5c8`, schedule `e8b5`, warning `e002`, error `e000`, help `e887`, autorenew `e863`, download `e2c4`, controller `f135`, celebration `ea65`, extension `e87b`.

New icons needed. Each codepoint is from the upstream codepoints list and is present in the shipped font's character map, but the shipped font is a different build, so check each one on screen before relying on it:

| Use | Glyph name | Codepoint |
|---|---|---|
| Play | `rocket_launch` | `eb9b` |
| Choose disc | `album` | `e019` |
| Settings | `settings` | `e8b8` |
| Quit | `power_settings_new` | `f8c7` |
| Mods | `extension` | `e87b` |
| Multiplayer | `group` | `ea21` |
| Sprout (tab marker fallback, update pill) | `psychiatry` | `e123` |
| Leaf | `eco` | `ea35` |
| Flower | `local_florist` | `e545` |
| Dropdown arrow | `expand_more` | `e5cf` |
| Open in browser | `open_in_new` | `e89e` |
| Reset to default | `restart_alt` | `f053` |
| Info | `info` | `e88e` |
| Folder | `folder_open` | `e2c8` |
| Tab icons, optional: Video, Input, Audio, Gameplay, Cheats, Interface, Tools | `monitor`, `stadia_controller`, `volume_up`, `sports_esports`, `science`, `palette`, `build` | `ef5b`, `f135`, `e050`, `ea28`, `ea4b`, `e40a`, `f8cd` |

The upstream list gives `e668` for `check`, which is not in the shipped font, while the tree's `e5ca` works. Treat that as proof the two builds differ.

## 4. Motion

### 4.1 Tokens and when to use them

| Token | Value | Use |
|---|---|---|
| `--dur-press` | 0.09s | press squash |
| `--dur-fast` | 0.12s | hover, focus, colour changes |
| `--dur-base` | 0.2s | fades, exits |
| `--dur-panel` | 0.26s | windows, popovers, the in-game bar |
| `--dur-sprout` | 0.32s | things that grow in: tab marker, toggle leaf, menu items |
| `--dur-bloom` | 0.5s | one-off celebration: update pill, first-run |
| `--dur-scene` | 0.8s | scrim and backdrop cross-fades |
| `--stagger` | 0.07s | delay between list items |
| `--ease-standard` | `cubic-out` | default for entering |
| `--ease-exit` | `cubic-in` | leaving |
| `--ease-pop` | `back-out` | anything that grows or lands |
| `--ease-pluck` | `back-in` | anything pulled away |
| `--ease-bloom` | `elastic-out` | at most one element at a time, and never on text |
| `--ease-drop` | `bounce-out` | toasts landing; nowhere else |
| `--ease-drift` | `sine-in-out` | idle loops |

Rules:

- Overshoot tweens (`back`, `elastic`, `bounce`) apply only to `transform`. Opacity and colour always use `cubic` or `linear`. `back-out` on opacity or `filter` overshoots past 1 and can flash.
- Exits are faster than entrances and never overshoot.
- Idle loops move at most 6dp or 4 degrees, with a period of 3s or longer.
- Hover transitions on launcher buttons are enabled only after the entrance finishes, using the existing `anim-done` class pattern (`rml/prelaunch.rcss:150-152`). Otherwise the entrance and hover transitions fight.

### 4.2 Recipes

| Name | Definition |
|---|---|
| Sprout in | `transform: scale(0)` to `scale(1)`, `transform-origin: bottom center`, 0.32s `back-out` |
| Pluck out | `transform: translateY(-24dp) scale(0.9)` plus opacity to 0, 0.26s; transform uses `back-in`, opacity `cubic-in` |
| Press | `scale(0.96)` in 0.09s `cubic-out` on `:active`, back to `scale(1)` in 0.2s `back-out` |
| Panel in | opacity 0 to 1 in 0.2s `cubic-out`; `scale(0.92)` to `scale(1)` in 0.26s `back-out` |
| Panel out | opacity to 0 and `scale(0.96)`, 0.16s `cubic-in` |
| Leaf sway | `rotate(-4deg)` to `rotate(4deg)`, 3.2s `sine-in-out`, `alternate infinite`, origin at the leaf base |
| Bob | `translateY(-5dp)` to `translateY(5dp)`, 5s `sine-in-out`, `alternate infinite` |
| Twinkle | opacity 0.35 to 1, 3.2s `sine-in-out`, `alternate infinite` |

## 5. Components

Each entry names the element as the framework emits it, the states, and any DOM the C++ must add.

### 5.1 Focus ring

One definition for every focusable control:

```css
box-shadow: var(--shadow-focus);
```

- Shown on `:focus-visible` (keyboard and gamepad) and on `:hover`. On `:hover` alone, use the 2dp green ring without the outer glow.
- Appears instantly. It fades out over 0.12s.
- The focused element also gets `z-index: 1` so the ring is not clipped by its neighbours.
- Scroll containers keep `padding: 6dp` so a 4dp ring on a row at the edge is not cut off.

### 5.2 Button (`button`)

Base rule replaces `rml/controls.rcss:1-42`.

| Property | Value |
|---|---|
| Height | 44dp minimum; `.compact` 32dp |
| Padding | `10dp 22dp`; `.compact` `6dp 14dp` |
| Radius | `--radius-pill` |
| Font | Fredoka 600, 19dp; `.compact` 15dp |
| Rest | background `--button-background`, shadow `--shadow-control`, opacity 1 (Dusklight's resting 0.9 is dropped: it greys text over the scene) |
| Hover | background `--button-background-hover`, 2dp green ring |
| Focus-visible | same fill, full `--shadow-focus` |
| Active | background `--button-background-active`, `transform: scale(0.96)` |
| Selected | background `--button-background-selected` |
| Disabled | opacity 0.4, `cursor: default`, no hover |
| Transition | `background-color 0.12s cubic-out, box-shadow 0.12s cubic-out, transform 0.2s back-out` |

Variants:

- `.primary`: fill `linear-gradient(180deg, #E63C66 0%, #C8284F 100%)`, text `#FFFFFF`, shadow `0 0 0 1dp rgba(255,255,255,22%), inset 0 1dp 0 0 rgba(255,255,255,35%), 0 4dp 14dp 0 rgba(200,40,79,45%)`. Hover: gradient `#F0496E` to `#D43460`. One per screen.
- `.confirm` (new): fill `linear-gradient(180deg, #A6EA6A, #6FD649)`, text `--color-ink`. For "Apply" and "Restart now".
- `.overlay`: fill `rgba(var(--color-surface-rgb), 80%)` plus `backdrop-filter: var(--blur-bar)`. For buttons that float directly on the scene.
- `.danger`: text `--color-error-text`, rim `rgba(var(--color-error-rgb), 55%)`; hover fill `rgba(var(--color-error-rgb), 18%)`.
- `.icon-button`: a 44dp circle (`.compact` 32dp), icon 24dp, shadow `--shadow-orb` when used on the scene.
- `.with-icon`: gap 8dp, icon 1.2em.

### 5.3 Launcher menu button (`#menu-list button`)

Replaces `rml/prelaunch.rcss:129-152,222-226`.

| Property | Value |
|---|---|
| Size | 360dp x 56dp; Play 360dp x 68dp |
| Radius | pill |
| Font | Fredoka 600, 28dp; Play 34dp. Title Case. `font-effect: var(--glow-text)` at rest |
| Padding | `0 24dp` at rest; `0 24dp 0 44dp` when focused. The text slides 20dp right |
| Rest | transparent background, text `--color-text` |
| Hover / focus-visible | fill `linear-gradient(180deg, rgba(255,255,255,26%), rgba(255,255,255,10%))`, `backdrop-filter: blur(8dp)`, `--shadow-focus`, and the cursor sprout appears |
| Active | `scale(0.97)`, fill 34% to 14% |
| Disabled | opacity 0.5, no fill |
| Transition (only with `.anim-done`) | `padding 0.2s cubic-out, decorator 0.12s linear-in-out, box-shadow 0.12s cubic-out, transform 0.2s back-out` |

Cursor sprout. DOM to add in `Prelaunch::Prelaunch`, `M:port/launcher/prelaunch.cpp:58-65`: each menu button gets a first child `<cursor-sprout><leaf class="l"/><leaf class="r"/></cursor-sprout>`.

```css
cursor-sprout {
    position: absolute; left: 16dp; top: 50%; margin-top: -11dp;
    width: 22dp; height: 22dp;
    transform: scale(0); transform-origin: bottom center;
    transition: transform 0.32s back-out;
}
#menu-list button:hover cursor-sprout,
#menu-list button:focus-visible cursor-sprout { transform: scale(1); }
leaf {
    position: absolute; bottom: 4dp; width: 12dp; height: 12dp;
    decorator: linear-gradient(135deg, #B9F27C, #5FBF3A);
}
leaf.l { left: 0;  border-radius: 12dp 1dp 12dp 1dp; }
leaf.r { right: 0; border-radius: 1dp 12dp 1dp 12dp; }
```

The leaf is a square with two opposite corners fully rounded: an original shape that needs no image. The same `<leaf>` rule is reused by the tab marker and the toggle.

The Play button:

- Always uses the `.primary` gradient fill. Text is white with no glow. A `rocket_launch` icon (28dp) sits at the right edge.
- With no disc it reads `Choose Disc Image` and the icon is `album`. Wiring exists at `M:port/launcher/prelaunch.cpp:100-102`.
- At rest it has a slow halo: `box-shadow` outer glow `0 0 18dp 2dp rgba(240,73,110,35%)` animating to `0 0 28dp 6dp rgba(240,73,110,55%)`, 2.4s `sine-in-out`, `alternate infinite`. The halo stops while another item has focus.

Menu order: Play, Multiplayer, Mods, Settings, Quit. Mods exists since M5.2, Multiplayer since M5.3b (section 8.4). Six items of 56dp plus a 68dp Play and 10dp gaps come to 398dp, which fits the 1080dp and 720dp frames. Future items are not shown as disabled placeholders; they appear when they exist.

### 5.4 Settings row (`select-button`)

The row every setting control is built on (`M:port/launcher/ui/select_button.cpp:12-26`: `key`, `icon`, `value` children).

| Property | Value |
|---|---|
| Min height | 52dp; `.compact` 36dp |
| Padding | `10dp 18dp` |
| Radius | `--radius-row` (16dp) |
| Gap | 12dp |
| Rest | background `rgba(var(--color-control-rgb), 35%)`, shadow `0 0 0 1dp rgba(var(--color-border-rgb), 16%)` |
| Hover | background `--button-background-hover` |
| Focus-visible | background `--button-background-hover`, `--shadow-focus` |
| Active | background `--button-background-active` |
| Disabled | opacity 0.4 |
| `key` | Fredoka 500, 19dp, sentence case, `flex: 0 1 auto`, ellipsis |
| `value` | Nunito 700, 19dp, right-aligned, colour text at 85% |
| `value.modified` | colour `--color-sun`, weight 800, preceded by a 6dp sun-yellow dot. The class is already set at `select_button.cpp:37`; the dot is `padding-left: 14dp` plus a `radial-gradient` decorator positioned at the left |
| List gap | 6dp between rows (`rml/window.rcss:104` uses 8dp; change to 6dp) |

Group rows (`.group-button`) keep the chevron (`rml/controls.rcss:91-99`). On hover the chevron moves 4dp right over 0.12s.

### 5.5 Toggle (`BoolButton`)

Today it is a row whose value text is "On" or "Off" (`M:port/launcher/ui/bool_button.cpp:32-40`). Keep the text and add a switch after it. The state is then carried by text, position, colour and shape.

DOM to add in the `BoolButton` constructor: class `bool-button` on the root, and a child after `value`:

```html
<toggle><track-fill/><knob><leaf class="l"/><leaf class="r"/></knob></toggle>
```

`BoolButton::update` sets or removes the attribute `on` on the root.

| Part | Off | On |
|---|---|---|
| `toggle` | 54dp x 30dp pill; background `rgba(var(--color-control-rgb), 70%)`; shadow `inset 0 2dp 4dp 0 rgba(5,11,26,60%), 0 0 0 1dp rgba(var(--color-border-rgb), 35%)` | same |
| `track-fill` | absolute, fills the track, radius pill, `linear-gradient(90deg, #5FBF3A, #8CE04A)`, opacity 0 | opacity 1 |
| `knob` | 24dp circle at `left: 3dp; top: 3dp`; fill `#C9D6E6`; `--shadow-orb` | `transform: translateX(24dp)`; fill `#FFFFFF` |
| leaves in the knob | each 11dp, anchored at the knob's top centre, `transform: scale(0)`, origin bottom centre | `scale(1)`; the right leaf starts 60ms after the left |
| `value` text | "Off", text at 55% | "On", `--color-accent`, weight 800 |

Transitions: knob `transform 0.2s back-out`; `track-fill` `opacity 0.16s cubic-out`; leaves `transform 0.32s back-out`. Turning off: leaves `0.12s cubic-in`, knob `0.16s cubic-out`.

Sounds stay as they are (`kSoundItemEnable` / `kSoundItemDisable`, `bool_button.cpp:46`).

### 5.6 Number and slider (`NumberButton`)

Today: a row whose value is formatted text, stepped with Left and Right (`M:port/launcher/ui/number_button.cpp:32-33`). Add a meter between `key` and `value` when the control has a finite range.

DOM to add when `props.max > props.min` (the range is read at `number_button.cpp:15`): class `number-button`, and before `value`:

```html
<meter><fill/><knob/></meter>
```

`NumberButton::update` sets the inline styles `fill { width: P% }` and `knob { left: P% }`, with `P = (value - min) / (max - min) * 100`.

| Part | Spec |
|---|---|
| `meter` | `flex: 0 0 180dp` (120dp in compact layouts); height 10dp; radius pill; background `rgba(255,255,255,10%)`; shadow `inset 0 1dp 3dp 0 rgba(5,11,26,60%)`; `margin: 0 12dp` |
| `fill` | height 100%; radius pill; `linear-gradient(90deg, #FFE27A, #FFB13B)` |
| `knob` (the pellet) | 22dp circle, `margin-left: -11dp; top: -6dp`; fill `radial-gradient(circle at 35% 30%, #FFF3B0 0%, #FFD23F 45%, #E0A51E 100%)`; shadow `0 2dp 6dp 0 rgba(5,11,26,55%), inset 0 0 0 2dp rgba(255,255,255,35%), inset 0 0 0 5dp rgba(224,165,30,60%)`. The two inset rings read as a pellet disc |
| Focused row | knob `scale(1.15)`; a `chevron_left` and a `chevron_right` hint (16dp, text at 55%) appear on either side of the meter |
| Stepping | knob does `scale(1.25)` then back to `1.15`, 0.2s `back-out`; `fill` width transitions 0.12s `cubic-out` |
| `value` | `min-width: 64dp; text-align: right` |

Unbounded numbers have no meter; they look like a plain row.

Mouse: clicking the meter sets the value from the pointer's x position; dragging uses `drag: drag`, as the colour picker does (`rml/popover.rcss:47`). Keyboard and gamepad behaviour is unchanged.

### 5.7 Select and dropdown

- **Select row:** the settings row plus an `expand_more` indicator (16dp, text at 70%), rotating 180 degrees over 0.16s `cubic-out` when expanded (`rml/controls.rcss:109-123`).
- **List:** a popover (section 5.12), `min-width` equal to the row's value column and at least 220dp.

| Item property | Value |
|---|---|
| Height | 40dp |
| Padding | `0 14dp 0 38dp` |
| Radius | 10dp |
| Font | Nunito 700, 17dp |
| Hover / focus | background `--button-background-hover`; a 2dp green ring inset 1dp |
| Selected | text `--color-accent`; a `check` icon (18dp) at `left: 12dp` |
| Disabled | text at 38% |

Separators are 1dp lines in `rgba(var(--color-border-rgb), 22%)` with `margin: 4dp 10dp`.

### 5.8 String input (`StringButton`, `input`)

A row whose value turns into a field while editing (`M:port/launcher/ui/string_button.cpp:33-42`).

| State | Spec |
|---|---|
| Rest | looks like a row; value text at 85% |
| Editing | the `input` gets background `rgba(var(--color-control-rgb), 80%)`, radius 10dp, padding `6dp 12dp`, shadow `inset 0 2dp 4dp 0 rgba(5,11,26,60%), 0 0 0 2dp rgb(var(--color-accent-rgb))`; text 100%, left-aligned |
| Caret | 2dp wide, `--color-accent` |
| Selection | background `rgba(var(--color-accent-rgb), 35%)`, text unchanged |
| Placeholder | text at 55% |
| Invalid | ring `--color-error`; an `error` icon at the right; caption below in `--color-error-text`, 15dp |

### 5.9 Tabs (`tab-bar`, `tab`)

Replaces the visual rules in `rml/tabbing.rcss:19-45` and `rml/window.rcss:74-87`.

DOM to add in `TabBar::add_tab` (`M:port/launcher/ui/tab_bar.cpp:111-117`): after the title text, append `<sprout><stem/><leaf class="l"/><leaf class="r"/></sprout>`.

| Part | Spec |
|---|---|
| `tab-bar` | height 64dp; background `rgba(var(--color-neutral-rgb), 7%)`; bottom border 1dp `rgba(var(--color-border-rgb), 35%)`; padding `0 8dp`; Title Case |
| `tab` | padding `0 20dp`; line-height 64dp; Fredoka 600, 19dp; colour text at 60% (opacity is no longer used; Dusklight's 0.25 fails contrast); `position: relative` |
| Hover | colour text 100%; `decorator: radial-gradient(ellipse at 50% 100%, rgba(140,224,74,22%) 0%, rgba(140,224,74,0%) 70%)` |
| Focus-visible | as hover, plus an inset ring: `box-shadow: inset 0 0 0 2dp rgb(var(--color-accent-rgb))`, radius 14dp, inset `8dp 2dp` |
| Selected | colour `--color-text`; `font-effect: var(--glow-text)`; sprout shown. No underline |
| Active | decorator alpha 36% |
| `sprout` | absolute, `bottom: 0; left: 50%; margin-left: -12dp`; 24dp x 16dp; `transform: scale(0)`, origin bottom centre; transition `transform 0.32s back-out` |
| `sprout stem` | 3dp x 8dp, centred at the bottom, `--color-leaf`, radius 2dp |
| `sprout leaf` | 11dp, `bottom: 6dp`; the `.l` and `.r` shapes from section 5.3 |
| `tab:selected sprout` | `transform: scale(1)` |

So the selected tab has a small two-leaf sprout growing out of the bar's bottom edge. Changing tab shrinks the old sprout (0.12s `cubic-in`) while the new one grows.

- Content change: the outgoing pane fades out in 0.08s; the incoming pane fades in over 0.16s while moving from `translateY(8dp)` to 0 with `cubic-out`. No horizontal slide.
- Close button (`rml/tabbing.rcss:58-98`): a 40dp circle at top 12dp, right 12dp. Hover fill `rgba(var(--color-interactive-rgb), 18%)`, and the icon rotates 90 degrees over 0.2s `back-out`.
- Shoulder-button hints: when the last input was a gamepad, `LB` and `RB` pills sit at the two ends of the bar (26dp tall, 2dp cyan rim, Fredoka 600 13dp). This echoes the game's L/R tab arrows without copying their art.

### 5.10 Window and pane

Replaces `rml/window.rcss:13-33`.

```css
window {
    max-width: 1120dp;
    max-height: 760dp;
    border-radius: var(--radius-window);
    border-width: var(--rim);
    border-color: rgba(var(--color-border-rgb), 70%);
    background-color: var(--surface-window);
    decorator: var(--surface-shine);
    backdrop-filter: var(--blur-panel);
    box-shadow: var(--shadow-window);
    filter: opacity(0);
    transform: scale(0.92);
    transition: filter 0.2s cubic-out, transform 0.26s back-out;
}
window[open] { filter: opacity(1); transform: scale(1); }
```

- Closing: the C++ adds a `closing` class that switches the transition to `filter transform 0.16s cubic-in` and the target to `scale(0.96)`. If no class hook is practical, use `0.2s cubic-in-out` both ways and accept losing the pop on close.
- Body padding around the window: 48dp; 16dp below 640dp height or 768dp width (`rml/window.rcss:55-72` keeps its structure).
- **Left pane (controls):** padding `20dp 20dp 20dp 24dp`, gap 6dp.
- **Right pane (help):** `flex: 0 0 34%`; background `rgba(var(--color-control-rgb), 30%)`; left border 1dp `rgba(var(--color-border-rgb), 25%)`; padding 24dp. Title Fredoka 600 22dp; body Nunito 600 18dp at 85%; an optional "Default: ..." caption at 55%; a "Needs restart" badge in sun yellow with the `restart_alt` icon.
- **Section heading:** Fredoka 600, 14dp, uppercase, `0.08em` tracking, text at 55%, `margin: 14dp 6dp 4dp`. A 1dp line in `rgba(var(--color-border-rgb), 18%)` extends from the text to the right edge.
- **Restart banner:** when a restart-only setting has changed, a 44dp strip pinned to the bottom of the window. Background `rgba(var(--color-sun-rgb), 14%)`, top border 1dp `rgba(var(--color-sun-rgb), 45%)`, text "Some changes apply after a restart", and a `.confirm.compact` button "Restart now".

Do not use `glass()` on windows: refraction behind text hurts legibility. It is reserved for small orbs (section 5.16).

### 5.11 Modal

`window.modal` (`rml/window.rcss:40-42,317-403,604-645`).

| Part | Spec |
|---|---|
| Scrim | the document body gets `background-color: rgba(5,11,26,55%)`, fading in over 0.2s. It carries no blur; only the dialog blurs what is behind it |
| Dialog | 560dp wide (100% below 768dp); radius 28dp; the window surface, at 92% opacity |
| Header | a 44dp orb (`--shadow-orb`, fill `rgba(var(--color-info-rgb), 22%)`) holding a 24dp icon, then the title in Fredoka 600 24dp, `--color-text` (not the border colour, as in Dusklight) |
| Body | Nunito 600 17dp at 85%, line-height 1.45, padding `4dp 28dp 20dp` |
| Actions | right-aligned, gap 10dp, padding `0 24dp 24dp`. The affirmative button is last and is `.primary` or `.confirm`. `.vertical` stacks them full width |
| Danger variant | rim `--color-danger-border`; orb fill `rgba(var(--color-error-rgb), 22%)`; title `--color-danger-heading`; the affirmative button is `.danger`, and focus starts on Cancel |
| Enter | panel in (section 4.2) |
| Exit | panel out |

### 5.12 Popover and context menu

Replaces `rml/popover.rcss:9-32`.

| Property | Value |
|---|---|
| Radius | 18dp |
| Border | 2dp `rgba(var(--color-border-rgb), 60%)` |
| Background | `--surface-popover`, `decorator: var(--surface-shine)`, `backdrop-filter: var(--blur-bar)` |
| Shadow | `--shadow-popover` |
| Padding | 6dp |
| Font | Nunito 700, 17dp |
| Enter | opacity 0.14s `cubic-out`; `scale(0.94)` to 1 in 0.2s `back-out`; `transform-origin` on the edge nearest the anchor |
| Exit | 0.1s `cubic-in`, opacity only |

The colour picker keeps its layout (`rml/popover.rcss:34-155`). Its swatches and sliders get 10dp radii, and its handles become 16dp white circles with a 2dp navy ring.

### 5.13 Tooltip (`ui-tooltip`)

Replaces `rml/global.rcss:224-244`.

- Max width 300dp, padding `8dp 12dp`, radius 12dp.
- Background `rgba(var(--color-raised-rgb), 97%)`, border 1dp `rgba(var(--color-border-rgb), 55%)`, shadow `0 4dp 14dp 0 rgba(5,11,26,55%)`.
- Nunito 600, 15dp, line-height 1.35.
- Shown after 0.5s of hover, or immediately on a focus that has rested 0.8s. Fades in over 0.12s.
- Never the only place a piece of information lives: the help pane shows the same text.

### 5.14 Toasts

`rml/overlay.rcss:32-70` and the `Toast` struct at `M:port/launcher/ui/ui.hpp:39-45`.

| Property | Value |
|---|---|
| Position | top 32dp, right 32dp; stack downward, gap 8dp; at most 3 visible |
| Size | min width 280dp, max width 400dp |
| Shape | radius 20dp; border 1dp `rgba(var(--color-border-rgb), 50%)`; `--surface-toast`; `backdrop-filter: var(--blur-bar)`; shadow `0 8dp 24dp 0 rgba(5,11,26,55%)` |
| Padding | `14dp 18dp 14dp 44dp` |
| Type marker | a 14dp pellet at `left: 16dp; top: 18dp`: a circle in the type colour with `inset 0 0 0 2dp rgba(255,255,255,35%)` |
| Heading | Fredoka 600, 18dp, `--color-text`, not uppercase |
| Body | Nunito 600, 16dp, text at 80% |
| Enter | `translateX(32dp)` to 0 in 0.32s `back-out`; opacity 0.16s `cubic-out` |
| Exit | opacity and `translateX(16dp)`, 0.18s `cubic-in` |
| Duration | 4s default; 6s for warnings and errors; hovering pauses the timer |

Type colours: info `--color-pik-blue`, success `--color-leaf`, warning `--color-sun`, error `--color-pik-red`, achievement `--color-petal`. Every type also shows its own icon in the heading row, so the type is never colour-only.

The FPS counter (`rml/overlay.rcss:227-300`) becomes a pill: 26dp tall, Noto Mono 14dp, `--surface-bar`, 1dp rim, 12dp from its corner.

### 5.15 Scrollbar and progress

- **Scrollbar** (`rml/global.rcss:182-208`): 6dp wide, margin `6dp 6dp 6dp 0`; thumb radius 3dp, min height 32dp, text at 30%. Hover or drag: 10dp wide, `rgba(var(--color-accent-rgb), 85%)`, 0.16s `cubic-out`. The track is invisible. Arrows stay zero-sized.
- **Progress** (`rml/global.rcss:147-180`): 10dp tall, radius pill, track `rgba(255,255,255,10%)` with an inset shadow. Fill `linear-gradient(90deg, #FFE27A, #FFB13B)` with a glow at its leading edge, `box-shadow: 4dp 0 12dp 0 rgba(255,177,59,60%)`. This reads as the sun crossing the day meter. State colours: success `--color-success`, failed `--color-error`, paused text at 35%.
- **Indeterminate:** the track shows three 8dp beads that pulse in sequence (opacity 0.3 to 1, 0.9s, each offset 0.15s). This replaces Dusklight's spinner where a width is available.
- **Spinner**, where only a square is available: a 24dp `eco` leaf icon rotating 360 degrees per 1.1s, linear.

### 5.16 Glass orb (new utility: `orb` element or `.orb` class)

For small round things on the scene: icon buttons, gamepad glyphs, the status dot.

```css
.orb {
    border-radius: 50%;
    background-color: rgba(var(--color-surface-rgb), 55%);
    box-shadow: var(--shadow-orb), 0 0 0 2dp rgba(var(--color-border-rgb), 55%);
    backdrop-filter: blur(8dp);
}
.orb.refract {
    /* opt-in, quality setting "Glass effects: Full" */
    backdrop-filter: glass(14dp 18dp 0.35 rgba(11, 23, 48, 40%) 1.1 convex 0 auto);
}
```

`set_glass_light_dir` (`M:extern/aurora/include/aurora/rmlui.hpp:35`) is called once with `(-0.5, -0.8)`, so highlights fall from the upper left like the leaf shine in the wordmark.

Every element with `backdrop-filter` forces the compositor to copy the game frame (`M:extern/aurora/lib/rmlui.cpp:466-471`). Budget:

- Launcher: the focused menu button only.
- In game: the bar and one window.
- Toasts are exempt because they are rare.

### 5.17 Button prompts

A row of glyph and label pairs, shown at the bottom of the launcher and of windows when the last input was a gamepad or keyboard.

- Glyph: a 26dp orb (`.orb` without blur), Fredoka 600 14dp, the letter centred. Keyboard keys use a rounded rectangle, radius 7dp, min width 26dp, padding `0 7dp`.
- Label: Nunito 700, 15dp, text at 80%.
- Gap: 6dp within a pair, 20dp between pairs.
- Labels: `A Select`, `B Back`, `LB/RB Tab`, `Y Reset to default`. Keyboard: `Enter`, `Esc`, `Q/E`, `R`.
- Glyph faces are plain letters. No platform-holder button art.

## 6. Prelaunch screen

### 6.1 Layers

Back to front, all inside the prelaunch document (`M:port/launcher/prelaunch.cpp:16-45`):

| # | Element | Content |
|---|---|---|
| 0 | the game frame | the live scene, drawn by the game beneath the UI |
| 1 | `prelaunch-background` | the pre-disc backdrop (section 6.4). Opaque. Fades out when the scene is ready |
| 2 | `prelaunch-gradient` | the left scrim |
| 3 | `prelaunch-vignette` (new) | the bottom scrim for the status row |
| 4 | `content#root` | menu, wordmark, status, prompts |
| 5 | `letterbox-top`, `letterbox-bottom` (new) | launch bars, height 0 at rest |
| 6 | `launch-bloom`, `launch-flash` (new) | launch overlays, opacity 0 at rest |

### 6.2 Composition contract with the scene

The UI and the scene must agree on where things are. In fractions of the frame, for 16:9:

- **UI column:** x from 0 to 0.30. The camera must keep this region free of characters; ground, sky and foliage are fine.
- **Subject zone:** x from 0.40 to 0.94, y from 0.28 to 0.90. The pilot and the three small characters stand here, with the pilot's head near (0.59, 0.36). The camera is low, so his head is above the horizon.
- **Ship:** background, centre between x = 0.60 and 0.74 (SCENE.md 8.2 starts at 0.61), y near 0.36. Its exhaust path on take-off is straight up, so the top right stays clear of UI. The update pill and version text sit at the bottom right for that reason.
- **Horizon:** between y = 0.55 and 0.65, so the status row at the bottom sits over ground, which is darker and calmer than sky.

At 4:3 the same fractions hold; the column is narrower in absolute terms and the menu width clamps (section 6.6).

### 6.3 Scrims

```css
prelaunch-gradient {
    position: absolute; left: 0; top: 0; width: 62%; height: 100%;
    decorator: linear-gradient(90deg,
        rgba(5, 11, 26, 82%) 0%,
        rgba(5, 11, 26, 74%) 28%,
        rgba(5, 11, 26, 46%) 52%,
        rgba(5, 11, 26, 16%) 78%,
        rgba(5, 11, 26, 0%) 100%);
    transition: opacity 0.8s sine-in-out;
}
prelaunch-vignette {
    position: absolute; left: 0; bottom: 0; width: 100%; height: 26%;
    decorator: linear-gradient(180deg, rgba(5, 11, 26, 0%) 0%, rgba(5, 11, 26, 58%) 100%);
}
```

- At x = 0.30, the right edge of the menu, the scrim is still about 70%, which keeps text above 8:1 over a white sky. It reaches zero at x = 0.62; at the pilot's position (0.60) it is under 2%.
- Dusklight's note says a coloured gradient banded badly and a black one banded less (`rml/prelaunch.rcss:29`). This scrim is near-black navy with five stops, which shortens each ramp. If banding shows, the fallback is a pre-dithered 512x4 PNG stretched with `image(scrim.png fill)`.
- Menu text also carries `--glow-text`, so it survives a failed or disabled scrim.

### 6.4 Pre-disc backdrop

Shown when no disc is set, and during boot until the scene is ready. Built from RCSS gradients plus two small generated PNGs. Nothing here depicts game characters.

DOM inside `prelaunch-background`:

```html
<bd-sky/>
<bd-stars class="far"/>
<bd-stars class="near"/>
<bd-stars class="twinkle"/>
<bd-sun/>
<bd-planet/>
<bd-ship><bd-flame/></bd-ship>
<bd-spark class="r"/><bd-spark class="y"/><bd-spark class="b"/>
```

| Layer | Spec |
|---|---|
| `bd-sky` | Full frame. `linear-gradient(180deg, #050B1A 0%, #0B1730 42%, #1B2E6B 74%, #3A2A6E 90%, #6B3A7A 100%)` |
| `bd-stars.far` | Full frame, `image(../img/stars-far.png repeat)`, opacity 0.7. Drifts `translateX(0)` to `translateX(-512dp)` over 240s, linear, infinite. The element is 100% + 512dp wide so the loop is seamless |
| `bd-stars.near` | As above with `stars-near.png`, opacity 0.9, 120s |
| `bd-stars.twinkle` | `stars-near.png` offset by `(137dp, 61dp)`, with the twinkle recipe |
| `bd-sun` | 900dp x 520dp ellipse centred at (70%, 78%). `radial-gradient(ellipse at 50% 100%, rgba(255,197,107,70%) 0%, rgba(255,138,61,40%) 30%, rgba(242,92,59,16%) 55%, rgba(107,58,122,0%) 80%)`. Opacity 0.85 to 1 over 6s, `sine-in-out`, alternate |
| `bd-planet` | A circle 240vw wide, `left: -70vw; top: 72vh`, `border-radius: 50%`. Fill `radial-gradient(circle at 60% 0%, #7BD66A 0%, #2E9A5B 14%, #14605A 34%, #0B2F4A 62%, #071A30 100%)`. Atmosphere: `box-shadow: 0 0 60dp 10dp rgba(127,216,255,32%), inset 0 6dp 30dp 0 rgba(185,242,124,35%)`. Only the top arc is on screen: a curved green horizon lit from behind at the right |
| `bd-ship` | `ship.png`, 220dp x 220dp, centred at (72%, 34%), rotated -18 degrees (nose up and to the left, as if arriving). Bob recipe, plus `rotate(-20deg)` to `rotate(-16deg)` over 7s |
| `bd-flame` | Under the ship's tail: 36dp x 70dp, `radial-gradient(ellipse at 50% 0%, #FFF3B0 0%, #FFB13B 35%, rgba(242,92,59,0%) 75%)`. `scaleY(0.8)` to `scaleY(1.15)` over 0.18s, linear, alternate infinite |
| `bd-spark` r, y, b | Three 10dp circles (`#E8392B`, `#FFD23F`, `#2F7BEA`), each with a glow `0 0 10dp 2dp` of its own colour at 60%. They trail behind the ship at 60dp, 92dp and 124dp along its tail direction. Each has the bob recipe with a phase offset of 0.4s, 0.8s and 1.2s. They read as three little lights following the ship |

Generated images. Add a script `M:tools/launcher_art/gen_backdrop.py`; commit it and its outputs:

- `stars-far.png`, 512x512, transparent: 140 dots of radius 0.5 to 1.0 px, white at 40 to 80% alpha, placed with a fixed seed. Wrap dots at the edges so the tile is seamless.
- `stars-near.png`, 512x512: 36 dots of radius 1.0 to 1.8 px, 70 to 100% alpha. One in six is tinted `#BFE6FF` or `#FFE27A`. Four have a 6 px soft cross flare.
- `ship.png`, 512x512, an original toy rocket drawn as flat vector shapes:
  - A fat teardrop body, 300 px tall and 190 px wide, `#E63C66`, with a lighter band `#FF7A98` on the left third and a darker band `#9E1F3D` on the right fifth.
  - A round white-cyan nose bulb, 90 px across, `#DDF1FF`, with a highlight dot.
  - One round porthole, 70 px, navy `#0B1730` with a 10 px `#DDF1FF` ring and a small highlight arc.
  - Three stubby rounded fins at the base, `#C8284F`, splayed 25 degrees.
  - A short antenna with a 16 px yellow ball, `#FFD23F`.
  - A 10 px `#0B1730` outline on everything.
  - It must not be traced from, or proportioned like, the game's ships. No engine pods, no side lamp, no lettering.

Reduced motion: star drift, twinkle, bob and flame all stop. The picture stays as a still.

### 6.5 Layout at 1920x1080 dp

```
+----------------------------------------------------------------------------------+
|                                                                                  |
|   eyebrow (20dp)                                              [ship, background] |
|   WORDMARK (420 x 105dp)                                                         |
|                                                                                  |
|   [ Play                  > ]  68dp, primary                                     |
|   [ Settings ]                 56dp                   [pilot and three friends]  |
|   [ Quit ]                     56dp                                              |
|                                                                                  |
|                                                                                  |
|   (o) Disc ready                                      [Update available v0.4.0 >]|
|   GameCube . USA . GPVE01   Change              (A) Select    Sproutlight v0.3.1 |
+----------------------------------------------------------------------------------+
```

| Element | Position and size |
|---|---|
| `menu` | `left: 96dp`; vertically centred with a -24dp offset (`top: 50%; transform: translateY(-54%)`); width 420dp; column; gap 36dp between hero and list. Replaces `rml/prelaunch.rcss:70-84`, which made the menu up to 50vw wide |
| `eyebrow` | Fredoka 500, 20dp, text at 80%, `--glow-text`. Text: "A native port for your own disc" |
| `hero img` (wordmark) | width 420dp, height auto. `wordmark-leaf.png` is stacked on top with the same box |
| `menu-list` | column, gap 10dp, `align-items: flex-start` |
| `disc-info` | `left: 96dp; bottom: 56dp`; column, gap 6dp |
| `#disc-status` | a pill: 36dp tall, padding `0 14dp 0 6dp`, `--surface-bar`, 1dp rim; a 24dp orb holding the icon, then the label in Fredoka 600 18dp |
| `#disc-version` | Nunito 600, 15dp, `--color-muted`, `--glow-text`: "GameCube . USA . GPVE01" (with real bullet characters), then a text button "Change" (underlined on focus, text at 85%) that reopens the picker (`Prelaunch::pick_disc`, `M:port/launcher/prelaunch.cpp:105-118`) |
| `version-info` | `right: 96dp; bottom: 56dp`; column, `align-items: flex-end`, gap 8dp |
| `version-label` | Nunito 600, 15dp, `--color-muted`, `--glow-text`: "Sproutlight v0.3.1" |
| `update-status` | above the version label; see section 6.7 |
| `prompts` | `bottom: 56dp`, right-aligned to end 24dp left of the version label; hidden for mouse users |

Disc status states. The attribute values are already produced at `M:port/launcher/prelaunch.cpp:78-96`:

| `status` | Label | Icon | Colour (label and orb tint) |
|---|---|---|---|
| `good` | Disc ready | check | `--color-leaf-hi` `#B9F27C` (13.6:1) |
| `none` | No disc image chosen | album | text at 72% |
| `verifying` | Checking disc... | leaf spinner | `--color-text` |
| `mismatch` | Not a supported disc | warning | `#FFD6A7` (13.1:1) |
| `bad` | Can't read this file | error | `#FFC2C8` (11.7:1) |
| `pending` | Restart to switch disc | autorenew | `--color-sun` |

`rml/prelaunch.rcss:305-307` styles `status=unknown`, but the C++ emits `none`. Rename the rule.

When the status is `mismatch` or `bad`, a second line under the pill says what to do, in the same colour at 15dp: "Sproutlight needs the USA GameCube release (GPVE01)." or "Pick a .iso, .gcm, .ciso, .gcz or .rvz file."

### 6.6 Other resolutions and aspect ratios

All breakpoints are in dp, so they respond to both window size and UI scale.

| Condition | Change |
|---|---|
| Default (16:9, 16:10; at least 1100dp wide and 641dp tall) | as section 6.5 |
| `@media (max-width: 1400dp)` | `--space-edge` 64dp; menu width 380dp; wordmark 380dp |
| `@media (max-width: 1100dp)` (4:3 windows, small windows) | `--space-edge` 40dp; menu width 340dp; buttons 320dp wide; menu font 26dp, Play 30dp; `prelaunch-gradient` width 72%; prompts hidden |
| `@media (max-height: 640dp)` (compact; Steam Deck at 125%, 720p at 125%) | Dusklight's row layout (`rml/prelaunch.rcss:445-583`): wordmark left at 300dp, menu list right, 44dp buttons at 22dp, status rows at 32dp from the edges in 14dp type. The scrim becomes the bottom vignette at 40% height plus a 30% navy wash over the whole frame |
| `@media (max-aspect-ratio: 1/1)` (portrait or very narrow) | bottom sheet: the menu is centred, `bottom: 120dp`, buttons full width minus 2 x 24dp; the wordmark is centred at the top, 70vw wide; the scrim is vertical from the bottom, 70% to 0% over 60% of the height |
| `@media (min-aspect-ratio: 7/3)` (21:9 and wider) | `content#root` becomes a centred stage: `width: 233.33vh; left: 50%; margin-left: -116.67vh`. The UI stays within a 21:9 box so the menu does not drift away from the characters on 32:9. The scrim's left edge and width follow the stage |

Additional rules:

- The start menu always fills the window. RmlUi can only draw inside the game's content rectangle, so while the start menu is open the aspect setting is overridden to follow the window (ASPECT.md section 5) and the live scene is rendered at the window's shape. There are no pillars to fill.
- The UI is drawn at window resolution (`M:extern/aurora/lib/rmlui.cpp:57-61`). The internal resolution setting does not blur it.
- No text is positioned with `vw` font sizes. Dusklight's `eyebrow` rule (`rml/prelaunch.rcss:111-116`) is dropped. Scaling is the UI scale's job.

### 6.7 Update status

Notify only. The action opens `https://github.com/owallzz/moonsprout/releases/latest` in the browser.

| `state` | Display |
|---|---|
| (none), or up to date | nothing |
| `checking` | "Checking for updates..." in Nunito 600 15dp, `--color-muted`, with the three-bead loader |
| `available` | A pill button, 36dp tall, padding `0 16dp 0 8dp`. Fill `linear-gradient(180deg, #FFE27A, #FFD23F)`, text `--color-ink` in Fredoka 600 16dp: "Update available: v0.4.0". A 24dp orb at the left holds a `psychiatry` sprout icon in `--color-leaf-deep`; an `open_in_new` icon (16dp) sits at the right |
| `failed` | "Couldn't check for updates" in `--color-muted`, 15dp. It fades out after 6s. No toast and no modal |

On first appearing, the pill uses the sprout-in recipe with `--ease-bloom` (0.5s), once. Its orb icon then has the leaf-sway loop. Focus order: reachable with Right from the menu or from "Change", and with Down from Quit when no "Change" link is shown (with one, Down from Quit goes to "Change", which sits directly under the column; Left from the pill goes back to it). On 16:9 and wider windows the pill sits in the version row, level with the disc line, so the bottom row reads "Change ... pill" in focus order.

### 6.8 Entrance

t = 0 is the first frame after `open` is set (`M:port/launcher/prelaunch.cpp:146-151`).

| t (ms) | Element | Animation |
|---|---|---|
| 0 to 600 | `body` | `filter: opacity(0)` to `opacity(1)`, 0.6s `cubic-out` (Dusklight: 1s after 0.2s, `rml/prelaunch.rcss:16-17`) |
| 0 to 800 | `prelaunch-gradient`, `prelaunch-vignette` | opacity 0 to 1, `sine-in-out` |
| 150 to 570 | eyebrow and wordmark | `translateY(16dp)` to 0, 0.42s `back-out`; opacity 0.3s `cubic-out` |
| 450 to 870 | wordmark leaf layer | sprout in (scale 0 to 1, origin at the stem base), 0.42s `back-out`; then the leaf-sway loop |
| 300 + 70n, lasting 320 | menu button n (0 = Play) | `translateX(-20dp)` to 0, 0.32s `back-out`; opacity 0.24s `cubic-out` |
| 650 to 950 | `disc-info` | `translateY(10dp)` to 0, 0.3s `cubic-out`, with opacity |
| 720 to 1020 | `version-info`, prompts | same |
| 1020 | all menu buttons | add `anim-done`; Play takes focus |

Implementation:

- Keep the `.intro-item` and `.delay-N` classes (`rml/prelaunch.rcss:405-442`) with the delays above.
- Because the `back-out` rule from section 4.1 applies, opacity and transform need separate entries in the `transition` list: `transition: opacity 0.24s 0.3s cubic-out, transform 0.32s 0.3s back-out;`.
- Input is accepted from t = 0. Any confirm press before 1020 ms snaps the entrance to its end state by adding the class `intro-skip`, which sets all transitions to 0s.
- When the document is recreated after a disc is chosen (`show_prelaunch`, `M:port/launcher/launcher.cpp:99-106`), it starts with `intro-skip` and with the backdrop opaque, so the screen does not replay the entrance or flash.

### 6.9 From backdrop to live scene

After a disc is chosen from the pre-disc screen, the game boots under the backdrop.

| t (ms) | What |
|---|---|
| 0 | Disc accepted. Status pill becomes `good`. Play is disabled and reads "Landing..." with the three-bead loader in place of the rocket icon |
| 0 to 900 | `bd-ship` and its sparks fly to the horizon: `translate` to (58%, 66%) of the frame and `scale(0.15)`, `rotate(-60deg)`, 0.9s `cubic-in`. `bd-flame` stretches to `scaleY(1.6)` |
| 900 to 1300 | A 120dp warm glow blooms at the landing point (opacity 0 to 0.8 to 0, 0.4s) |
| until ready | The backdrop holds, without the ship. The scene module sets the body class `scene-ready` once the live scene has drawn its first complete frame |
| ready + 0 to 1000 | `prelaunch-background` opacity 1 to 0, 1s `sine-in-out` (Dusklight's `disc-ready` fade, `rml/prelaunch.rcss:54-56`). Play becomes "Play" again and takes focus |

If the scene is ready before 1300 ms, the cross-fade waits until 1300 ms. If boot fails, the backdrop stays and an error modal appears.

## 7. Start-game choreography

T = 0 is the confirm press on Play with the live scene running (`Prelaunch::on_play`, `M:port/launcher/prelaunch.cpp:120-132`). Today the document only fades out (`begin_close`, `prelaunch.cpp:134-143`).

### 7.1 Scene cues

The scene module owns the animation and reports cues. The UI reacts to cues, with timer fallbacks so it never hangs. Cues arrive through `p2_launcher_scene_cue` and `handoff` is the state `P2_SCENE_DONE` (SCENE.md 4.2). The nominal times below are the full take-off of SCENE.md section 11.

| Cue | Nominal T (ms) | Scene |
|---|---|---|
| `launch_begin` | 0 | The pilot nods to the camera and whistles; the Pikmin hop and run to their Onions |
| `hop` | 3300 | The pilot runs to the pick-up point in front of the ship; the Pikmin climb into the Onions |
| `board` | 7640 | The ship's beam lifts him aboard (the disc has no hatch animation) |
| `ignite` | 12590 | After the Onions rise, the authored lift-off clip starts: pistons, rumble, jets, camera shake |
| `liftoff` | 12990 | The ship leaves the ground; the cameras cut to the sky as the ship passes the Onions |
| `exit_frame` | 16890 | The convoy is high above; the scene starts its own 0.75 s fade in the flash colour |
| `handoff` | 17660 | The scene section ends; the game builds its title section under the opaque flash |

Without a live scene at Play (Live Start Menu off over a booted game, or the scene failed to load),
the still backdrop's ship takes off on its own nominal times instead: `ignite` 2800, `liftoff` 3070,
`exit_frame` 5600, `handoff` 6350 (`kBackdropTimes`).

### 7.2 UI timeline, full mode

| T (ms) | Element | Animation |
|---|---|---|
| 0 | Input | Navigation locked: body class `launching` sets `pointer-events: none` on `content`. `kSoundPlay` plays (`prelaunch.cpp:122`) |
| 0 to 90 | Play button | `scale(0.94)`, 0.09s `cubic-out` |
| 90 to 310 | Play button | `scale(1.04)` then 1, 0.22s `back-out`; its halo jumps to full strength |
| 120 + 50k, lasting 220 (k = 0 for Quit, counting upward) | other menu buttons | `translateX(-32dp)`, opacity to 0, `cubic-in`. They leave bottom to top |
| 120 to 320 | `disc-info`, `version-info`, `update-status`, prompts | `translateY(16dp)`, opacity to 0, 0.2s `cubic-in` |
| 300 to 800 | `letterbox-top`, `letterbox-bottom` | height 0 to 6vh, 0.5s `cubic-out`, colour `#03060F`. A film-frame cue that the menu has become a scene |
| 300 to 1100 | `prelaunch-gradient`, `prelaunch-vignette` | opacity 1 to 0, 0.8s `sine-in-out`. The whole scene is revealed while the pilot crosses to the ship |
| 400 to 660 | Play button | pluck out: `translateY(-24dp) scale(0.9)`, 0.26s `back-in`; opacity `cubic-in` |
| 900 to 1300 | eyebrow and wordmark | `translateY(-40dp)`, opacity to 0, 0.4s `cubic-in`. The wordmark is last to leave. Its leaf does one sway to +10 degrees as it goes |
| `ignite` (12590) to `liftoff` | `launch-bloom` | Full-frame overlay, `radial-gradient(circle at var(--ship-x) var(--ship-y), rgba(255,243,176,55%) 0%, rgba(255,138,61,28%) 22%, rgba(242,92,59,0%) 55%)`, opacity 0 to 0.6, `cubic-out`. The host sets `--ship-x` and `--ship-y` from `p2_launcher_scene_ship_screen` (SCENE.md 4.2); if that is unavailable, fixed at `62% 40%` |
| 1200 | `skip-hint` | "Press any button to skip": a pill at the bottom right, above the letterbox, fades in (0.5s). It dims to 72% at 4000 (darker fill and a text shadow keep it readable) and fades out at `exit_frame` or on a skip. Not shown for Off |
| `liftoff` (12990) to `exit_frame` | `launch-bloom` | opacity 0.6 to 0.85 over 0.5s, tracking the ship. While the ship is off screen (`ship_screen` false) the class `launch-bloom-off` fades it out in 0.12s, so the glow never hangs where the ship left the frame |
| `exit_frame` (16890) | `launch-flash` | Full frame, `#FFF6E0`. With the live scene it is first a 30% veil over the scene's own fade to the same colour; at +300 it closes to opaque in 0.4s, so it is opaque at +700, before the scene ends at +750 |
| `handoff` (17660) | host | The game leaves the launcher scene; the frames after its last one are black until the title draws, so the flash turns opaque at once if it is not already (`launch-handoff`, no transition). The letterbox, bloom and all launcher elements are removed while hidden under the flash, and the user's aspect setting replaces the start-menu override (ASPECT.md 5) |
| `handoff` to +150 | `launch-flash` | hold |
| `handoff` +150 to +1750 | `launch-flash` | opacity 1 to 0, 1.6s `quadratic-in` (`kRevealMs`); the letterbox bars stay above it and slide out, over the title section's own fade-in from black, so the two read as one move |
| `handoff` (about 17660) | input | Game input is unblocked as the menu hands over (`sync_input_block`), so the title answers during the dissolve |
| `handoff` +1750 (about 19400) | document | `hide(true)` |

Fallbacks:

- While the live scene runs, its cues alone drive the timeline, at whatever speed the game runs. The scene polls `p2_launcher_scene_command` once per frame, and the bridge timestamps that poll as a heartbeat. With no poll for 3 s, or 60 s after Play, the scene counts as hung and the UI proceeds on the nominal times.
- If `handoff` has not arrived 2s after the flash became opaque, the flash cross-fades to navy `#050B1A` with the three-bead loader and waits there.

### 7.3 Skipping and shorter modes

- **Skip:** any key, mouse button or gamepad button after T = 600 sends `P2_SCENE_CMD_SKIP` and jumps straight to the flash (0.25s, no veil); the scene ends 0.3s later (`handoff`) and the flash dissolves into the title exactly as after a watched take-off (1.6s).
  - `scene::consume_any_press()` polls went-down edges. Modifier keys, F1 to F24, Print Screen, volume and media keys, and keys pressed with Alt or Super held do not count. Gamepad buttons count while the window has input focus or `input.backgroundInput` is on.
  - `scene::arm_skip()` at the Play press: nothing skips until every input held at that moment has been released. With the 600 ms floor, a held Enter, a double Enter or a double click on Play cannot skip.
  - Confirm, any other navigation command and a click on the document also skip, under the same two rules.
- **Setting `interface.launchAnimation`** (declared in SETTINGS.md 9.1): the "Take-off" row, `full` (On, default) or `off`. Stored `auto` and `short` read as `full`.
  - `Off`: Dusklight's behaviour. The document fades in 0.2s; the scene fades to navy in 0.4s and the game starts.
- **Reduced motion:** the same timeline and cues (the scene holds its cameras still). The UI uses the plain-fade rules (body class `launch-short`: all UI fades together in 0.2s, the scrim in 0.3s, no letterbox), and the warm flash becomes a fade to navy `#050B1A` (0.3s in, 0.4s out). No bloom.

### 7.4 Implementation shape

- `Prelaunch` gains a small state machine: `Idle`, `Launching`, `Flash`, `Handoff`, `Reveal`, `Done`. It is stepped in `Prelaunch::update` (`prelaunch.cpp:145-168`) from a `steady_clock` start time.
- Per-element delays are RCSS transition delays keyed on `body.launching`, exactly like the intro's delay ladder. C++ sets only classes: `launching`, `launch-ignite`, `launch-flash`, `launch-reveal`.
- The current `hide(true)` on the first closing frame (`prelaunch.cpp:164-167`) moves to the `Done` state.
- `launch_requested()` on the standalone path (`M:port/launcher/launcher.cpp:120-124`) keeps its plain fade when Live Start Menu is off: with no scene there is no choreography. With it on, picking a disc on the pre-disc screen boots the game under the backdrop and the menu stays up (section 6.9); Play then runs this section.

## 8. In-game menu (F1 / Back)

Opened with F1 or the gamepad Back button. Dusklight's version is a full-width 64dp bar with a bottom border (`rml/popup.rcss:18-33`). This one floats.

### 8.1 Bar

```css
popup {
    position: absolute; top: 16dp; left: 50%;
    /* width set by content; centred by the C++ or by a flex wrapper */
    height: var(--toolbar-height);
    padding: 0 10dp;
    border-radius: var(--radius-pill);
    border-width: var(--rim);
    border-color: rgba(var(--color-border-rgb), 70%);
    background-color: var(--surface-bar);
    decorator: var(--surface-shine);
    backdrop-filter: var(--blur-bar);
    box-shadow: 0 10dp 28dp 0 rgba(5, 11, 26, 55%), inset 0 1dp 0 0 rgba(255, 255, 255, 30%);
    transform: translateY(var(--toolbar-hidden-offset));
    transition: transform 0.26s back-out;
}
popup[open] { transform: translateY(0); }
```

Centring without `calc()`: wrap the bar in a full-width flex row with `justify-content: center`, and animate the wrapper's child.

Contents, left to right:

| Item | Spec |
|---|---|
| Mark | a 40dp orb with the sprout (the wordmark's leaf, 22dp). Not focusable |
| Tabs | Built from the entry registry of SETTINGS.md section 13: `Resume`, `Settings`, `Controllers`, (`Mods`), (`Online`), `Reset Game`, `Start Menu`, `Quit` (`Mods` is shown since M5.2). Styled as section 5.9 with 18dp padding. Unselected tabs are text at 70%, not Dusklight's 0.35 opacity (`rml/popup.rcss:39-42`) |
| Divider | 1dp x 28dp, `rgba(var(--color-border-rgb), 30%)` |
| Status | FPS (when enabled) and the clock, Noto Mono 14dp, `--color-muted`; then the prompt `B Close` |

- `Reset Game`, `Start Menu` and `Quit` each open a danger modal (copy in SETTINGS.md 13). There is no Quit popover.
- `Mods` is shown since M5.2 and opens the Mods window read-only (SETTINGS.md 17). `Online` is shown while a lobby or an online match exists (M5.3b, section 8.4).
- Close: `transform` back to the hidden offset, 0.18s `cubic-in`. The bar's hidden offset is -88dp because it floats 16dp below the top edge.

### 8.2 Scrim, windows and the game underneath

- When the bar opens, a full-frame scrim `rgba(5, 11, 26, 30%)` fades in over 0.2s. It has no blur. The game stays readable, and darkening is the "paused" signal.
- When a window (Settings) is open, the scrim deepens to 50%. The window is the same component as in the launcher (section 5.10), opened below the bar: body padding `96dp 48dp 48dp`.
- In game, the PRELAUNCH tab is hidden. The remaining seven tabs are Video, Input, Audio, Gameplay, Cheats, Interface, Tools.
- Restart-only settings show the "Needs restart" badge and stay editable, with the restart banner (section 5.10).
- The backdrop-filter budget in game is the bar plus one window (section 5.16). With "Glass effects: Off", both use an opaque surface at 96% and no `backdrop-filter`.
- A small "Paused" tag (Fredoka 600 16dp in a glass pill) sits under the bar when the game is actually paused. Whether it pauses is the integration spec's decision.

### 8.3 First-open hint

The first time a game session starts, a toast says "Press F1 (or Back) for the menu" for 5s. The plumbing exists: `show_menu_notification`, `M:port/launcher/ui/ui.cpp:591-599`.

### 8.4 Online screens

M5.3b (SETTINGS.md 18). Ordinary windows built from the components of section 5; nothing new in
`theme.rcss`. `res/rml/online.rcss` holds their layout.

- **Multiplayer window** (start menu): the Mods window's shell (list left, selected item right, the
  right pane taking the rest of the window, prompts at the bottom). Forms use the settings rows:
  steppers (5.6) for mode, stage and handicaps, text rows (5.8) for the code and the password, a
  toggle (5.5) for spectating, one `.primary` action per pane, kept at a button's width. Text rows
  show a placeholder at text 55% and the invalid ring of 5.8 (`--color-error`, with an error note
  under the row). Connecting uses the bead loader (5.15).
- **Room:** the code in a monospace pill (`--color-control` at 60%, cyan rim, inset shadow) with a
  Copy icon button; seat rows are 56dp settings rows whose line holds the **team marker** (16dp:
  team 0 a plain square in Okabe-Ito vermillion `rgb(213, 94, 0)`, team 1 an Okabe-Ito blue
  `rgb(0, 114, 178)` square in a 3dp white frame; the same shapes and colours as the M5.3a captain
  markers, so colour is never alone), "P<n>" in Fredoka 600 at text 72%, the name in Fredoka 500
  19dp, a sun-yellow star for the host, a "You" tag (cyan fill, ink text), a monospace ping badge, a
  link glyph (cyan direct, sun yellow relay) and the ready pill (26dp: the `.confirm` green with ink
  text when ready, a hairline outline when not, sun yellow while connecting). A content mismatch
  outlines the row in `--color-warning` and adds the note on a second line with the warning glyph.
  The Ready toggle is an ordinary settings row (56dp) for the host and for members alike.
- **Wait pill** (in game): the in-game bar's glass (`--surface-bar`, 1dp rim, shine, blur), 52dp,
  centred 24dp below the top (below the bar while it is open), the spinner leaf (5.15), the text in
  Fredoka 600 19dp, the seconds in Noto Mono at text 72%, and at most one `.danger.compact` button.
  It enters with the toast motion (translate and fade, `back-out`) and leaves in 0.18s `cubic-in`.
- **End messages** are modals (5.11) with one affirmative button; confirmations that drop, remove
  or leave are the danger variant with the focus on Cancel.
- The in-game bar's "Paused" tag reads "The game is still running" during an online match.

## 9. Accessibility

### 9.1 Contrast

- The numbers are in section 3.3. All text is 4.5:1 or better on its real surface. Body text is above 8:1 even with the translucent surface over a bright sky.
- Focus is the double ring (section 5.1), readable on dark and bright backgrounds.
- State is never colour-only. Toggles change text, position and shape. Disc status has an icon and words. Toast types have icons. Modified values get a dot and a heavier weight as well as yellow.
- New setting `interface.highContrast` (bool, default off) puts the class `high-contrast` on every document's `body`:
  - surfaces at 100% opacity, no `backdrop-filter`, no shine decorator;
  - rims become `--color-text` at 2dp;
  - secondary text rises from 72% to 90%, tertiary from 55% to 80%;
  - the launcher scrim becomes a solid `rgba(5,11,26,88%)` panel behind the menu column, from x = 0 to 34%.

### 9.2 Reduced motion

New setting `interface.reduceMotion` (bool, default off) puts the class `reduce-motion` on every document's `body`. RmlUi cannot read the OS preference (section 0).

```css
body.reduce-motion * {
    transition: opacity 0.08s linear-in-out, filter 0.08s linear-in-out,
                background-color 0.08s linear-in-out, color 0.08s linear-in-out;
    animation: none;
}
```

- No transforms animate: windows, popovers, toasts and the bar fade in place. Sprouts and leaves appear without growing.
- Idle loops stop: leaf sway, bob, twinkle, halo pulse, star drift.
- Loaders stay, because they carry information, but the three beads pulse opacity only.
- The take-off keeps its full timeline with still cameras; the UI uses its plain fades and a navy flash, with no letterbox and no bloom (section 7.3).
- Confirm that a universal-selector rule can override per-element `transition` declarations at the specificity used. If not, add the `reduce-motion` override next to each animated rule (open question 4).

### 9.3 UI scale

`interface.uiScale` exists: percent, 0 = automatic, max 200 (`M:port/launcher/settings.cpp:13`).

- Present it as a slider from 75% to 130% in steps of 5% (the layout holds at most 133%; older stored values above that load and show "(using 133%)"), plus an "Automatic" value at the far left. It applies live (`apply_scale`, `M:port/launcher/ui/ui.cpp:601-610`).
- Automatic today equals the OS display scale. On a 4K television at 100% OS scaling, that makes a 20dp label 20 pixels tall, which is too small from a sofa. Proposed automatic rule, in window pixels `w`, `h` and OS display scale `ds`:

  ```
  fit  = min(w / 1920, h / 1080)
  auto = max(fit, min(ds, min(w / 960, h / 540)))
  auto = max(auto, 0.6667)
  ```

  | Window | ds | auto | Viewport in dp |
  |---|---|---|---|
  | 1920x1080 | 1 | 1.0 | 1920x1080 |
  | 3840x2160 | 1 | 2.0 | 1920x1080 |
  | 3840x2160 | 2 | 2.0 | 1920x1080 |
  | 2880x1800 | 2 | 2.0 | 1440x900 |
  | 1280x720 | 1 | 1.0 | 1280x720 |
  | 1280x800 (handheld) | 1 | 1.0 | 1280x800 |
  | 640x480 | 1 | 0.667 | 960x720 |

  A manual percentage multiplies `ds`, as it does now.
- The layout must hold from 960x540 dp upward. That is the smallest viewport the automatic rule produces for a 16:9 window; a manual 200% can go lower, which lands in the compact layout.
- Because breakpoints are in dp, raising the scale moves the layout to the compact variants on its own.
- Minimum sizes at any scale: text 12dp, interactive targets 44dp x 44dp (compact buttons 32dp tall are mouse-only extras and always have a 44dp alternative), 6dp between adjacent targets.

### 9.4 Input and reading

- Everything is reachable with keyboard, gamepad and mouse. Focus order follows visual order. Up and Down wrap in the launcher menu (`M:port/launcher/prelaunch.cpp:178-191`).
- Focus is never lost. Closing a popover or modal returns focus to the control that opened it.
- No timed interaction, except toasts, which pause on hover and never carry the only copy of a message.
- No flashing. The launch flash is a single 0.4s ramp, and is removed under reduced motion.
- Text is sentence or Title Case, except 14dp section headings. Labels say what a control does ("Choose Disc Image", not "Browse").
- Interface language is English only for now. Nunito covers Latin Extended, Cyrillic and Vietnamese. Japanese would need M PLUS Rounded 1c, which is OFL and in `google/fonts`; it was not downloaded.

## 10. File plan

| File | Action |
|---|---|
| `rml/theme.rcss` | Replace with section 3.1 |
| `rml/global.rcss` | Section 5: focus ring, scrollbar, progress, tooltip, `.orb`, `leaf`, the reduced-motion and high-contrast blocks |
| `rml/controls.rcss` | Sections 5.2, 5.4 to 5.8 |
| `rml/tabbing.rcss` | Section 5.9 |
| `rml/window.rcss` | Sections 5.10, 5.11 |
| `rml/popover.rcss` | Sections 5.7, 5.12 |
| `rml/overlay.rcss` | Section 5.14 |
| `rml/popup.rcss` | Section 8 |
| `rml/prelaunch.rcss` | Sections 5.3, 6, 7 |
| `M:port/launcher/res/img/` (new) | `wordmark.png`, `wordmark-leaf.png`, `stars-far.png`, `stars-near.png`, `ship.png`, the icon PNGs |
| `M:port/launcher/art/` (new) | SVG sources |
| `M:tools/launcher_art/gen_backdrop.py` (new) | Generates the star tiles and the ship |
| `M:port/launcher/ui/ui.cpp:92-97` | Font list (section 3.5) |
| `M:port/launcher/ui/ui.cpp:601-610` | Automatic scale rule (section 9.3) |
| `M:port/launcher/ui/bool_button.cpp`, `number_button.cpp`, `tab_bar.cpp:111-117` | Extra child elements (sections 5.5, 5.6, 5.9) |
| `M:port/launcher/prelaunch.cpp` | New DOM (sections 5.3, 6.1, 6.4), status naming (6.5), launch state machine (7.4) |
| `M:port/launcher/settings.{hpp,cpp}` | `interface.reduceMotion`, `interface.highContrast`, `interface.glassEffects` (`off` / `blur` / `full`), `interface.launchAnimation`; declared in SETTINGS.md 9.1 |

Room for later features:

- **Mods and multiplayer** get menu slots (section 5.3), bar slots (section 8.1) and icons (section 3.7); both exist now (sections 8.1, 8.4). Their screens are ordinary windows built from the components here. The Mods window exists since M5.2 (`mods_window.cpp`, `rml/mods.rcss`, SETTINGS.md 17): it is built on the Save Manager's shell, not on Dusklight's `mods.rcss`, `mod_browser.rcss` and `mod_common.rcss`, which were never copied. A mod browser is not built.
- **Online status** (connected, in lobby, ping) fits the bar's status area and the launcher's bottom-right column above the version.
- **Player colours** for PvP should use `--color-pik-red` and `--color-pik-blue` as fills with white text on red (4.2:1, so large sizes only) or `--color-pik-blue-hi` and `--color-error-text` when used as text.

## 11. Open questions

1. **Name.** Is Sproutlight acceptable, or is a name that echoes Dusklight too derivative? Hundredfold is the runner-up. The checks were single web searches; no trademark database was searched.
2. **Who draws the art?** The wordmark, icon and ship are specified precisely enough to build as SVG, but nobody has drawn them. Until then the live-text wordmark (section 1.3) and a shipless backdrop work.
3. **`glass()` filter.** It is registered but unused by Dusklight. Its look and cost on the target GPUs are untested, so it is opt-in (`.orb.refract`).
4. **RCSS details.** Answered by W1 in a running build (see "RCSS findings" below). Still open:
   banding of the five-stop scrim and the frame-time cost of `backdrop-filter` on the focused menu
   button (W3).
5. **Scene aspect.** Answered in review: the start menu and the live scene always use the window's shape (ASPECT.md 5), so section 6.6 holds. Still open there: the in-game menu is confined to the content rectangle when the player chooses Original on a wide window (ASPECT.md open question 3).
6. **Launch timings.** Answered: the full take-off on every start, skippable with any button, or Off (section 7.3).
7. **Material Symbols codepoints.** The shipped font is a different build from the upstream codepoints list (`check` differs). The new icons in section 3.7 need an on-screen check.
8. **Noto Mono licence file.** Answered: `fonts/licenses/NotoMono-OFL.txt` carries the copyright
   line from the font's own name table ("Copyright 2007 Google Inc. All Rights Reserved.") and the
   standard OFL 1.1 text.
9. **Pre-disc sounds.** The game's audio does not exist before a disc is loaded. The launcher is silent there unless CC0 sounds are added; that is outside this spec.

## 12. RCSS findings (W1)

Checked in the running build with the component gallery (`MPPM_UI_GALLERY=1`, `ui/gallery.cpp`),
at 1920x1080 with 100% and 200% Menu Size, automatic scale at 3840x2160, and over the live game.

| Question | Finding | What the sheets do |
|---|---|---|
| Several comma-separated transitions with different tweens on one element | Works (`PropertyParserAnimation::ParseTransition`) | Used on the toggle knob, popovers, toasts, the bar |
| `var()` inside `transition` | Not needed: transitions spell their durations and tweens out; the motion tokens in `theme.rcss` document the scale and serve C++ | |
| Does `body.reduce-motion *` override per-element transitions? | No. RmlUi has no `!important`, and any component rule with a class, attribute or pseudo-class outranks `body.reduce-motion *` | The accessibility selectors repeat their class (`body.reduce-motion.reduce-motion.reduce-motion.reduce-motion *`); RmlUi counts every repeat. Keep the count above the most specific animated selector (three today) |
| Transitions on `box-shadow` | Not supported: Aurora logs "Box shadows do not support animations or transitions" | Focus rings appear and leave instantly; `box-shadow` is in no transition |
| `transitionend` under reduce motion | Windows, popovers and toasts close on the `transitionend` of their filter, the bar on its transform | Reduce motion keeps opacity, filter, colour and background transitions (0.08s); the bar fades with opacity instead of sliding |
| `border-radius: 50%`, `order`, `transform-origin: bottom center` | Percent radii and `order` are rejected; `transform-origin` wants the horizontal keyword first | `999dp` radii; the modal header runs `row-reverse` so the icon orb sits before the title; `center bottom` |
| Bare text inside a flex container | Not laid out (the text vanishes) | Flex rows put labels in an element (`<span>`); badges are `inline-block` |
| Gradient decorators and `border-radius` | Gradients are clipped to the rounded box | Leaves, track fill and pellet use gradients directly |
| A stopped `mousedown` | Cancels RmlUi's drag start | The slider handles `mousedown` without stopping it and stops only `click` |
| Modal scrim | A body background needs a hook; `WindowSmall` documents now carry `body.dialog` | `rgba(5,11,26,55%)`, faded in by a keyframe animation |

Small departures from sections 5 and 9, made while checking on screen:

- 5.3 `leaf`: the radii of `.l` and `.r` were swapped (they drew a roof). `.l` rounds its top-right
  and bottom-left corners, so it leans up and left from its bottom-right; `.r` mirrors it.
- 5.4: the value hugs the right edge at its own width so the modified dot sits right before it.
  Slider values keep `min-width: 64dp` against jitter, so their dot can sit a few dp further out.
  Rows do not squash on press (only buttons do): rows hold sliders that are dragged.
- 5.5: the knob's sprout grows on the knob's face (8dp leaves) instead of above it; above, it read
  as ears and clipped against the row.
- 5.9: unselected tabs are text at 66%, not 60%: 60% measured 4.7:1 under the bar's shine.
- 5.10: the section heading's rule runs the full width under the text (no pseudo-element to start
  it after the text).
- 9.3: the automatic rule uses the presented framebuffer size (`native_fb_width/height`) and is
  re-evaluated when the window size changes (`ui::update`).
