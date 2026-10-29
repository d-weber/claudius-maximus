# Sprite sheets of Clawd's Studio

Every piece of pixel art in the home-screen room comes from the sheets in `firmware/assets/sprites/`. They are the
masters: edit them, and the firmware build turns them into C arrays. [FORMAT.md](FORMAT.md) is the file format,
[CONVERTER.md](CONVERTER.md) the converter (`tools/sprites.py`).

```
firmware/assets/sprites/
  <sheet>.png      indexed PNG (mode P), pixel value = room::Idx
  atlas.json       every sprite's rectangle on its sheet, anchor, group and frame
  palette.gpl      room::BASE as a GIMP/Aseprite palette, in index order
  palette.png      the same, N_COL x 1 (what the converter reads)
```

## Build-time generation

`firmware/platformio.ini` runs [`tools/sprites_prebuild.py`](../sprites_prebuild.py) before every build. It calls
`tools/sprites.py build`, which writes `firmware/src/screens/room/sprites_gen.h/.cpp`. These two files are
gitignored and never committed. `build` rewrites them only when their content changes, so a build with no sheet
edits recompiles nothing (the hook takes ~0.2 s). An invalid sheet fails the build with the converter's `error:`
line. The hook needs Pillow. It uses PlatformIO's Python if Pillow is there, then `host/.venv`, and otherwise
installs Pillow into PlatformIO's Python. `tools/room_sim.py` runs the same `build` before it compiles the room on
the host. CI runs `tools/sprites.py check` on the committed sheets.

## Editing a sheet

1. Open the sheet in **indexed mode** with `palette.gpl` loaded. In Aseprite, load the palette and use Sprite >
   Color Mode > Indexed. In GIMP, use Image > Mode > Indexed with that palette. The pixel value is the palette
   index, and lighting recolours by index, so a pixel must keep its index, not just its colour. BASE repeats
   `#ffffff` (WHT, STAR, QW) and `#ffd23f` (LEDY, QY). A sheet saved as RGB loses which one was meant, and the
   converter rejects those pixels. Index 0 is transparent (magenta in the palette).
2. Paint inside the sprite's rectangle (`x, y, w, h` in `atlas.json`). Pixels outside every rectangle are
   ignored. Keep the grid: one row per group, frames left to right in play order, 1 px gutters.
3. Run `host/.venv/bin/python tools/sprites.py check`, then preview with `tools/room_sim.py` or
   `tools/sprites.py preview --out DIR` (4x views with the rectangles outlined, written outside the repo's
   assets).

**Live preview while editing**: `host/.venv/bin/python tools/room_view.py` opens http://localhost:8765 with the
room playing at 12 fps, 4x like the device, for a chosen state (task, time of day, helpers, plan progress, a
reaction). It runs the real room code (the firmware's C++ built for the host, `tools/room_sim.py`) on the sheets
as they are on disk. Saving a sheet from the editor rebuilds and re-renders within a few seconds (the page
watches the sheets' mtimes; "Refresh sprites from disk" does it on demand, <kbd>r</kbd> too). A sheet the
converter rejects shows its error on the page instead of a frame. Keep it on a second monitor next to the
editor.

**Changing a sprite's size**: update its `w, h` in `atlas.json`. If the top-left moved, update `ax, ay` too, so
that `blit(x - ax, y - ay)` still lands where it should (see "Anchors" below).

**Adding a frame to a group**: draw it in a free cell to the right of the group's last frame (widen the PNG if
needed). Add an entry to the sheet's `sprites` in `atlas.json` with a new unique `name`, the same `group`, the
next `frame` number and its rectangle and anchor. The generated `<group>_n` grows by one. Whether the room plays
the new frame depends on how the code picks frames: check the group in `director.cpp`, especially the groups with
a fixed formula (listed below).

**Adding a group**: add a row of cells (on a new or existing sheet), with entries `<group>_0..` (frames 0..n-1),
and draw it from the room code (`spr::<group>[i]`). A new sheet also needs an entry under `sheets` with its `file`.
Each sheet's `rows` list describes the grid for readers. The converter ignores it, but keep it in step.

**Changing the palette**: colours live in `room::BASE` (`room.cpp`). After changing it, run
`tools/sprites.py palette` to rewrite `palette.png` and `palette.gpl`. `check` fails while they differ.

## What is still code

Sprites hold palette indices. Everything that depends on the time of day or reads the canvas is in
`firmware/src/screens/room/`: the lighting (a palette rebuild), the sky colours (SKYA/SKYB are replaced per time
of day, so the night windows look day-blue on the sheet), the light patches and shadows. See
[room/README.md](../../firmware/src/screens/room/README.md) for what the room code draws and how.

## Anchors

`blit(x - ax, y - ay, sprite)` reproduces the draw call. Two kinds of groups:

- **Fixed** (drawn at fixed canvas coordinates: room, furniture, window, `clawd_bed*`, all overlays): the anchor
  is minus the crop's canvas position, so the sprite goes back in place with `blit(0, 0, s)`.
- **Positioned** (the call takes x, y: Clawd, minis, props, FX, `paper`): the anchor is the x, y passed to the
  call. Clawd: the pose x, y (18 x 12 body top-left, feet at y + 11; raised arms make `ay` 2). Mini: x, y of the
  mini (feet at y + 8). `quest_bang`: (bx0, by0) before the lift. `thought`: the bubble's (bx0, by0), with the
  trail at (bx0 + 6, by0 + 20) as the director draws it. `zzz`, `sparkle*`, `sweat`, `bird_*`, `book`, `can*`,
  `torch`, `mug_held`, `paper`: their x, y. `dash`: (x0, y0).

**Overlays** (tag `overlay: true`) hold only the pixels a variant changes over a base sprite. Blit the base, then
the overlays. Base for `desk_*` overlays: `desk_0` (screen off, t 0, drawer shut, no steam, mug, unlit). Base for
`rack_progress`: any `rack_idle`/`rack_active` frame (the bar sits above the rack). When you edit a base, check
that its overlays still fit it.

## Sheets (484 sprites)

| Sheet | Size | Groups (frames) |
|---|---|---|
| room | 243x422 | `walls` 1, `floor` 1, `rug` 1, `background` 1, `alarm_ring` 2 (phase 0/1) |
| furniture | 305x480 | `bookshelf` 10 (gap -1, 0..8), `desk` 6 (one per ScreenMode), overlays `desk_scr_dim` 1, `desk_scr_plug` 1, `desk_scr_code` 48 (t 0..47), `desk_scr_web` 3 (t 0/8/16), `desk_scr_saver` 24 (t 0..23), `desk_steam` 8, `desk_drawer` 1, `desk_nomug` 1, `desk_lit` 1; `rack_idle` 5 (t 0/24/../96), `rack_active` 8 (seed 1, t 0/2/../14), overlay `rack_progress` 17 (0..16); `bed` 1, `bed_sleep` 2 (breath), `nightstand` 2 (lamp off/on), `plant` 3 (sway -1/0/1), `whiteboard_empty` 1, `whiteboard_1`..`_5` (done 0..total), `paper` 1 |
| window | 465x376 | `window_day` 22 (cloudX 0..21), `window_rain` 40 (rainT 0..39), `window_night_m0`..`m7` 5 each (twinkle: starT 0/7/14/21/28), `window_shoot` 7 (shootT 0..6, moon 4, starT 0), `window_bird` 8 (bird 0, 1, 8..13, day cloudX 0) |
| clawd | 305x217 | `clawd_{front,back,left,right}_walk` 4 (legPhase 0..3), `clawd_{front,back,left,right}_arms` 10 (Arms enum order), `clawd_eyes` 10 (Eyes enum order), `clawd_mouth` 4, `clawd_side_blink` 2 (left, right), `clawd_tint` 3 (coral, grey, violet), `clawd_expr` 18 (the director's face combinations: wait, wait_look_l/r, wait_wave_a/b, wait_tap, wink, giggle, happy, wince, stretch, yawn, yawn_up, wave_a/b, sip, doze, doze_swing; see `tags.expr`), `clawd_bed` 6 + `clawd_bed_grey` 6 (shift -1/0/1 x eyes closed/open) |
| minis | 61x61 | `mini_{front,back,left,right}_walk` 4, `mini_front_blink` 2 (legPhase 0/1), `mini_tint` 3 |
| props | 91x33 | `book` 6 (page 0..5: pages vary on page%2 and page%3), `can` 1, `can_pour` 5 (t 0..4), `torch` 1, `mug_held` 1 |
| fx | 225x152 | `emote_l` 16 + `emote_r` 16 (Icon enum order, tail left/right, in the colour the director uses), `quest_bang` 2 (lift 0/1), `thought` 4 (dots 0..3), `zzz` 3 (t 0/6/12), `sparkle` 1 (QW), `sparkle_big` 3 (QY, QW, SCR), `sweat` 1, `dash` 3 (t 0/4/8), `bird_sit` 2 (frame 0/1), `bird_fly` 6 (frame 8..13) |

The `tags` of each sprite in `atlas.json` say what it shows (view, phase, state, icon). Only people and tools read
them.

Frame selection, where it is not just the parameter:

- `desk_steam`: frame = `((t/3)&1) | ((t/4)&1)<<1 | ((t/5)&1)<<2` for the desk steam counter t (`tags.t` is one
  t giving that frame).
- `desk_scr_code`: frame = t % 48. `desk_scr_web`: frame = (t/8) % 3. `rack_idle`: (t/24) % 5. `zzz`: (t/6) % 3.
  `window_night_m<phase>`: (starT/7) % 5. `window_rain`: rainT % 40. `window_day`: cloudX. `can_pour`: t % 5.
  `book`: page % 6. `dash`: (t/4) % 3.
- `whiteboard_<n>`: a plan over 5 items draws as n = 5, done = (done*5 + total/2) / total.
- `clawd_expr`: the order of the director's expression table in `director.cpp`. Adding a frame means adding
  its row there.

The emotes `leaf`, `plug` and `coffee` are marked `tags.used: false`: the director never shows them (they use
GRN, INK and W2).
