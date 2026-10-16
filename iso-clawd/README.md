# Isometric Clawd — animation prototypes

Review-only prototypes of the home-screen Clawd redrawn as 2:1 isometric pixel art. This is not firmware. See `BRIEF.md` for the design brief.

## Open it

Open `index.html` in a browser (file:// works, no server, no build). It shows every animation as a device-size card. Click a title to open the full player. `anims/00-model-sheet.html` is the visual reference for every pose, prop and FX.

```
iso-clawd/
  BRIEF.md              design brief
  index.html            gallery (hard-coded ANIMS list; missing files show "not built yet")
  lib/iso.js            the shared library (window.Iso), plain script
  anims/_template.html  copy this to start an animation
  anims/00-model-sheet.html  reference: expressions, arms, legs, tints, mini, props, FX, palette, spots
  anims/NN-name.html    one animation per file (01..17)
  shots/                screenshots (*.png is gitignored)
```

## Add an animation

1. Copy `anims/_template.html` to `anims/NN-name.html` (use the file name listed in `index.html`).
2. Keep `<script src="../lib/iso.js"></script>`. Put all your code inline in the page. Do not `fetch` or `import` local files, because that breaks on file://.
3. Call `Iso.player({...})` once. It builds the whole page UI.
4. **Do not edit `lib/iso.js`.** Other animations share it and are built in parallel. If you need a helper, write it in your own page. If you think the lib has a bug, report it.
5. Check the result at `?zoom=4` and in the gallery. Use `?frame=N` for still screenshots.

## Stage and coordinates

- The logical canvas is **240 × 121 px**, shown at **2x = 480 × 242 device px** (the home face area). The background is `C.bg` `#1A1918`, which the player fills before every frame.
- Projection is classic 2:1 iso. World `+x` → screen `(+2, +1)` (down-right). World `+y` → screen `(−2, +1)` (down-left). `+z` → screen `(0, −1)` (up). One x/y unit is 2 px across and 1 px down. One z unit is 1 px.
- Lighting: the **top** face is lightest, the **left** face is mid, the **right** face is darkest.
- **Anchor convention:** every Clawd and prop takes `(x, y)` = the screen point of the **centre of its footprint on the surface it stands on**. `z` lifts the object while that point stays on the ground. FX sprites are centred on `(x, y)`. `fx.puff` is the one exception: it is anchored at its top-left corner.
- **Default desk:** `Iso.drawDesk(ctx)` draws a dark walnut slab of 44 × 28 units, 7 px thick. Its top-surface centre is at (120, 74). The top diamond's corners are back (104, 38), left (48, 66), right (192, 82) and front (136, 110). The slab bottom is at y ≈ 117. The desk top is z = 0 for everything standing on it.
- **Default Clawd:** feet at `SPOT.clawd` = (120, 76). He spans x ≈ 90–150 (with arms) and y ≈ 32–86. The top of his head is at y = 32, which leaves about 30 px of headroom for FX.
- **Named spots** (`Iso.SPOT`, screen coords on the desk top):

| spot | x, y | draw order vs Clawd |
|---|---|---|
| `clawd` | 120, 76 | |
| `center` | 120, 74 | |
| `backLeft` | 84, 56 | before |
| `backRight` | 148, 66 | before |
| `left` | 74, 70 | before (beside) |
| `right` | 166, 78 | before (beside) |
| `frontLeft` | 100, 88 | after |
| `frontRight` | 148, 92 | after |
| `front` | 128, 100 | after |

- `Iso.deskPoint(u, v)` returns the screen point at fractions `u` (along x, back → right corner) and `v` (along y, back → left corner) of the default desk.
- **Draw order is yours (painter's algorithm).** Draw the desk first, then props behind Clawd, then Clawd, then props in front of him, then FX. Nothing sorts across separate calls.

## API (`window.Iso`)

### Palette

- `Iso.C`: bg, surface, surface2, border, text, muted, dim, orange, orangeLight, orangeDark, blush, eye, amber, amberDark, green, greenDark, red, redDark, violet, violetLight, violetDark, blue, blueLight, blueDark, grey, greyLight, greyDark, woodTop, woodLeft, woodRight, woodEdge, woodShadow, grain, paper, paperShade, ink, screen, glow, shadow.
- `Iso.SHADE.<name>` = `{top, left, right[, edge]}` for orange, grey, violet, wood, paper, metal, dark, blue, green, amber and red.
- Stay on palette.

### Pixels

All coordinates are rounded to integers. Never use `ctx.arc`, transforms, alpha or smoothing.

- `Iso.rect(ctx, x, y, w, h, color)`, `Iso.px(ctx, x, y, color)`, `Iso.clear(ctx, color?)`
- `Iso.line(ctx, x0, y0, x1, y1, color)`: Bresenham.
- `Iso.isoLine(ctx, x, y, n, dir, color)`: clean 2:1 line, `n` steps. `dir` is one of 'dr', 'dl', 'ur', 'ul'.
- `Iso.dither(ctx, x, y, w, h, color, density)`: `density` is 0.25, 0.5 or 0.75.
- `Iso.sprite(ctx, x, y, rows, map, flipX?)`: draws from strings like `['.#.', '###']` with `{'#': C.amber}`. `.` and space are transparent.
- `Iso.text(ctx, x, y, str, color)`: 3×5 uppercase font with a 4 px advance. Returns the width. `Iso.textWidth(str)` gives the width without drawing.

### Iso geometry

- `Iso.box(ctx, x, y, z, a, b, h, pal, opts?)`: centre-anchored voxel box. `a` is units along x, `b` is units along y, `h` is px. `pal` = `{top, left, right, edge?}`; any face can be null. `opts.round` softens the corners.
  - Returns `{left, right, top, x, y, w, h}`.
  - `left` and `right` are **face painters**: `.rect(u, v, w, h, c)` (sheared, follows the face), `.px(u, v, c)`, `.sprite(u, v, rows, map)` (upright, unsheared), `.spriteSheared(...)`, `.at(u, v) → {x, y}`, `.cols`, `.rows`. `u` is the column from the face's screen-left edge (0 .. 2·len−1). `v` is the row from the face's top edge.
  - `top` has `.cell(i, j, c)`, which paints the 2×1 px for unit cell (i, j), and `.at(i, j)`.
- `Iso.boxAt(ctx, sx, sy, z, a, b, h, pal, opts?)`: same, but `(sx, sy)` is the box's back (min) corner.
- `Iso.tile(ctx, x, y, a, b, color)`: flat diamond, for example a shadow.
- `Iso.plane(ctx, x, y, z, len, h, 'L'|'R', fill)`: zero-thickness vertical panel. Returns its face painter. `'L'` runs along x and faces down-left; `'R'` runs along y and faces down-right. `(x, y)` is the bottom-centre.
- `Iso.w2s(ox, oy, wx, wy, wz)`: world → screen, given the screen position of the world origin.
- `Iso.obj(ctx, x, y, A, B, facing, z)`: builder for rotatable multi-box props. Call `.box(lx, ly, lz, la, lb, lh, pal, tag)`, then `.draw()`, which returns the face painters per tag. See `prop.laptop` for an example.

### Clawd: `Iso.drawClawd(ctx, opts)` → anchors

| opt | values (default first) |
|---|---|
| `x, y` | feet point (default `SPOT.clawd`) |
| `z` | lift in px. The shadow stays on the ground and shrinks. |
| `facing` | `'left'` (face on the left face, looking down-left) \| `'right'` (mirrored; face on the darker right face) |
| `size` | `'full'` \| `'mini'` (subagent helper, ~30×26 px) |
| `tint` | `'orange'` \| `'grey'` (offline) \| `'violet'` (subagent) \| any `Iso.SHADE` name \| `{top, left, right}` |
| `eyes` | `'open'` or `{shape, gazeX, gazeY, l?, r?}`. Shapes: `open blink closed happy wince wide wink`. `gazeX` −1..1 (2 px steps along the face), `gazeY` −2..2 px. `l`/`r` override one eye's shape (screen left/right). `wink` = left open, right `^`. |
| `mouth` | `'none'` \| `'o'` \| `'laugh'` (red tongue) \| `'smile'` \| `'wobbly'` \| `'flat'` |
| `blush` | `false` \| `true` |
| `shine` | catch-light pixel on `open` eyes (default off). `wide` has one unless `shine:false`. |
| `arms` | `'out'` or `{l, r}` (**screen** left/right). Poses: `out high up waveA waveB down forward`(=`hold`) `typeA typeB none` |
| `legs` | `'stand'` \| `'stepA'` \| `'stepB'` (march: alternate) \| `'tap'` (screen-right foot up; alternate with `stand`) \| `'sit'` (body drops 5 px, legs 1 px) \| `'dangle'` / `'dangleB'` (sitting on a ledge; see below) |
| `squash` | px. `+` is shorter and wider (2 → +1 unit each side, 5 → +2), `−` is taller and narrower (≤ −3 → −1 unit). Use 2..6 for landings and −3..−4 for take-offs. |
| `shake` | x offset in px (the shadow stays) |
| `shadow` | color (default `C.woodShadow`, right for the desk) \| `false`. Off the desk, use `C.shadow` or `false`. |
| `eyeColor`, `blushColor` | overrides |

Returned anchors (screen px) for placing held props and FX:

- `hand.l`, `hand.r`: arm tips. `null` for `'none'`.
- `head`: top-centre of the sprite (badges go about 6–10 px above it).
- `top`: centre of the top face.
- `face`: centre of the face.
- `eyes.l`, `eyes.r`, `mouth`.
- `feet`.
- `bbox` `{x, y, w, h}`.
- `facePainter`: paint extra things on the face, such as glasses, a glow or tears.

The full face is 32 cols × 28 rows. Eyes are at u 4..9 and 22..27, v 5..16. Mouth and blush sit on row v 18. Anything you paint via `facePainter` lands on top of the arms.

Size reference (full, facing left, `z` 0): the body is 32 px face width × 28 px tall, 6 units deep. Legs are 6 px. The overall sprite is ~44 px wide (60 with arms) and ~55 px tall. The mini is FW 8, D 3, H 12, legs 3.

Dangle: put Clawd on a ledge with `z` = ledge height, so that his face side is flush with the ledge's front-left edge and his centre is 3 units (+6, −3 px) behind that edge's midpoint. The legs then hang over the edge (see the model sheet).

### Desk and props

`Iso.drawDesk(ctx, {x, y, a, b, thick, pal, grain, mat})` → `{pt(u, v), top, desk}`. All options are optional.

Props are `Iso.prop.NAME(ctx, x, y, opts)`. `(x, y)` is the footprint centre on the desk top. For props that take a facing, it is the direction the **screen/board faces**:

- `'left'` (down-left, toward the viewer)
- `'right'` (down-right)
- `'upLeft'` / `'upRight'` (away from the viewer: you see the lid back with a glowing edge, and Clawd sits behind it)

Clawd facing left matches a laptop facing `'upRight'` placed in front-left of him, and vice versa.

| prop | opts | notes |
|---|---|---|
| `laptop` | `facing, screen:'code'\|'search'\|'plan'\|'blank'\|'off'\|fn(p, area, frame), frame, glow, z` | 11×7 units, lid 10 px. Returns `{screen, area, lid, base}`. The screen is `null` when facing away. |
| `monitor` | same as laptop | 12×5 units on a stand, panel 14 px tall. Returns `{screen, area, panel}`. |
| `paper` / `doc` | `lines, a, b, color, z` | flat sheet, 6×8 |
| `book` | `color` (SHADE name or obj), `open, z` | closed 6×8×3; open 10×7 |
| `notepad` | `lines, z` | 5×7×2, amber header |
| `pencil` | `dir:'x'\|'y', len` | lying flat, centre-anchored. Usable as a held item. |
| `mug` | `frame, steam, color, z` | 4×4×8. Steam has a 4-frame cycle. |
| `magnifier` | `handle:'dr'\|'dl'` | 2D sprite, `(x, y)` = lens centre. Meant to be held at `hand.*`. |
| `sticky` | `color, plane:'top'\|'L'\|'R', z, mark` | 3×3 note. On a plane it is upright. |
| `terminal` | `plane:'L'\|'R', len:14, h:18, z:10, lines:[[indent, len, color]...], reveal, cursor, frame` | floating window. `(x, y)` = point under its bottom-centre. `reveal` = columns typed so far. Returns `{painter, area}`. |
| `browser` | as terminal + `reveal` (rows loaded), `frame` (spinner) | floating window with an address bar |
| `globe` | `frame` | 16-frame spin |
| `planBoard` | `facing:'left'\|'right', items:4, checked, z` | easel board with checklist. Returns `{painter}`. |
| `clipboard` | `checked, z` | flat on the desk |
| `lamp` | `facing:'left'\|'right', on` | light pool is dithered |
| `shadow` | `(ctx, x, y, a, b, color)` | ground diamond |

`Iso.screenContent(painter, area, kind, frame)` lets you reuse the screen fillers on your own panels.

### FX (screen space, centred)

- `Iso.fx.sparkle(ctx, x, y, size 0..3, color)`
- `Iso.fx.bang(ctx, x, y, {color, badge:true})`: amber "!" badge, or a plain "!" with `badge:false`.
- `Iso.fx.question(ctx, x, y, {color, badge})`
- `Iso.fx.zee(ctx, x, y, size 1..3, color)`
- `Iso.fx.sweat(ctx, x, y)`
- `Iso.fx.heart(ctx, x, y, size 1|2, color)`
- `Iso.fx.check(ctx, x, y, color)`, `Iso.fx.cross(ctx, x, y, color)`
- `Iso.fx.dots(ctx, x, y, n 0..3, color)`
- `Iso.fx.poof(ctx, x, y, frame 0..3, color)`: spawn/vanish puff.
- `Iso.fx.ticks(ctx, x, y, 'up'|'left'|'right', color)`
- `Iso.fx.puff(ctx, x, y, w, h, {kind:'speech'|'thought', tail:'left'|'right'|'none', fill, border})`: anchored top-left. Returns the inner area `{x, y, w, h}` for text or icons.

### Timing helpers

- `Iso.seq([[value, holdFrames], ...], i)` returns the value at frame `i` (wraps). `Iso.seq.len(list)` gives the total frame count.
- `Iso.cycle(arr, i, hold?)`, `Iso.pingpong(n, i)`, `Iso.lerp(a, b, t)` (rounded).

### Player: `Iso.player({name, fps, frames, draw(ctx, i, t), loop, notes, width?, height?, bg?})`

The player builds the page:

- stage, with play/pause, step ◀ ▶, a frame counter and speed 0.5/1/2x
- zoom: 1x logical / device 480×242 / 2x device
- badges for frames, fps, duration, loop/one-shot and resolution
- **Export spritesheet PNG**: all frames in one horizontal row at logical resolution, named `name_Nf_FPSfps_240x121.png`
- an optional notes paragraph (HTML)

Keys: space toggles play, ←/→ step, 1/2/3 set zoom. `t = i / fps`.

`loop:false` (one-shot) holds the last frame for about 1 s in the player, then replays. The export contains only the real frames. The function returns `{canvas, ctx, render(i, ctx?), exportSheet(), frame, play(), pause()}`.

URL params:

- `?embed=1`: stage only, used by the gallery.
- `?frame=N`: paused on frame N, for screenshots.
- `?zoom=1|2|4`
- `?play=0`

## Animation conventions

- **Frame-based only:** 8–12 fps (10 is the default). Build discrete poses and hold them with `Iso.seq`. Don't use physics or easing on floats.
- `draw` must be a **pure function of the frame index**, with no state carried between calls. The export and the step buttons render frames in any order.
- Loops last 1–4 s. One-shot reactions last 0.7–2.5 s and **end on the neutral pose**, which is `eyes:'open'`, `arms:'out'`, `legs:'stand'` at `SPOT.clawd`, so they chain back into idle.
- Use anticipation → action → squash/settle. Make blinks 1 frame, and hop apexes 2–3 frames. Add secondary motion: steam, a foot tap, a paper flutter. Keep a gag or two per animation and stay readable at a glance.
- Use the palette only, with integer pixels. Dithering is the only "gradient".

## Gotchas

- `facing:'right'` mirrors the geometry and keeps the light from the left, so the face sits on the **darker** side. `arms.l/r` and `eyes.l/r` always mean **screen** left/right, whichever way Clawd faces.
- The screen-left arm (facing left) is drawn *behind* the body, so the body hides part of it. `down` arms are mostly hidden. To show an arm clearly, use `out`/`high`/`up`/`forward`.
- `gazeX` moves the eyes in 2 px steps along the slanted face, so it also moves them 1 px vertically. `gazeY` ±2 is the useful range.
- `squash` changes width in whole units, so the eyes shift with the face centre. Keep squash values small and brief.
- `legs:'sit'` makes the body 5 px lower. Badges anchored on `head` follow automatically.
- Clawd and props don't depth-sort against each other. If a prop must pass behind and then in front of Clawd, switch the call order per frame.
- The desk shadow colour (`C.woodShadow`) is wrong off the desk. Pass `shadow: C.shadow` or `false`.
- On file:// the gallery can't detect missing files directly. A card stays "not built yet" until the embedded page calls `Iso.player`, which posts a ready message.
