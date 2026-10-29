# Sprite sheets of Clawd's Studio

`make_sheets.py` renders every piece of pixel art of the home-screen room from the firmware's own draw calls
(`firmware/src/screens/room/room.cpp`) into indexed sprite sheets plus `atlas.json`, in the format of
[FORMAT.md](FORMAT.md). [CONVERTER.md](CONVERTER.md) turns the sheets back into C arrays.

```sh
host/.venv/bin/python tools/sprites/make_sheets.py            # -> firmware/assets/sprites/
host/.venv/bin/python tools/sprites/make_sheets.py --out DIR --build DIR
```

It compiles `sheet.cpp` + `room.cpp` with g++ (cached in `mocks/sprites_build/` until a source is newer), runs
it, and composes the sheets. The output is deterministic: a second run writes byte-identical files.

`sheet.cpp` draws each sprite on a transparent canvas (`clear(T)`), crops the opaque bounding box and writes
`cells.jsonl` + `cells.bin` (the exact bytes, used as the reference for round-trip checks) and `palette.bin`.
It fails if a sprite is empty or touches the canvas border (clipped). `make_sheets.py` lays each sheet out as a
grid (one row per group with that group's own cell size, 1 px gutters, frames left to right, at most 16 per
row before a group continues on the next row, sprites at their cell's top-left; `rows` in atlas.json) and
writes:

- `<sheet>.png`: mode P, palette = `room::BASE` in index order (N_COL entries, index 0 = `#ff00ff`, tRNS
  transparent). Pixel value = `room::Idx`.
- With `--preview DIR`: `<sheet>@4x.png` views in that directory (not in the assets): checkerboard =
  transparent, blue box = sprite rectangle, group name on the left, frame number in each cell's bottom-right
  corner.
- `atlas.json`, `palette.png` (N_COL x 1), `palette.gpl` (names from the enum in room.h).

Note: the repo's `.gitignore` ignores `*.png`, so committing the sheets needs a `!firmware/assets/sprites/*.png`
exception.

## Anchors

`blit(x - ax, y - ay, sprite)` reproduces the draw call. Two kinds of groups:

- **Fixed** (drawn at fixed canvas coordinates: room, furniture, window, `clawd_bed*`, all overlays): the anchor
  is minus the crop's canvas position, so the sprite goes back in place with `blit(0, 0, s)`.
- **Positioned** (the call takes x, y: Clawd, minis, props, FX, `paper`): the anchor is the x, y passed to the
  call. Clawd: the pose x, y (18 x 12 body top-left, feet at y + 11; raised arms make `ay` 2). Mini: x, y of
  `drawMini` (feet at y + 8). `quest_bang`: (bx0, by0) before the lift. `thought`: the bubble's (bx0, by0),
  with the trail at (bx0 + 6, by0 + 20) as the director draws it. `zzz`, `sparkle*`, `sweat`, `bird_*`,
  `book`, `can*`, `torch`, `mug_held`, `paper`: their x, y. `dash`: (x0, y0).

**Overlays** (tag `overlay: true`) hold only the pixels a variant changes over a base draw. Blit the base,
then the overlays. `sheet.cpp` checks that they recompose exactly. Base for `desk_*` overlays: `desk_0`, which is
`drawDesk` with screen off, t 0, drawer shut, no steam, mug, unlit. All desk combinations (screen x t x steam x
drawer x mug x lit, 1536 of them) recompose exactly. Base for `rack_progress`: any `rack_idle`/`rack_active`
frame (the bar sits above the rack).

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

Frame selection, where it is not just the parameter:

- `desk_steam`: frame = `((t/3)&1) | ((t/4)&1)<<1 | ((t/5)&1)<<2` for the desk steam counter t (`tags.t` is one
  t giving that frame).
- `desk_scr_code`: frame = t % 48. `desk_scr_web`: frame = (t/8) % 3. `rack_idle`: (t/24) % 5. `zzz`: (t/6) % 3.
  `window_night_m<phase>`: (starT/7) % 5. `window_rain`: rainT % 40. `window_day`: cloudX. `can_pour`: t % 5.
  `book`: page % 6. `dash`: (t/4) % 3.
- `whiteboard_<n>`: a plan over 5 items draws as n = 5, done = (done*5 + total/2) / total.

## Not covered, and why

- **Procedural lighting**, which reads the canvas: `drawSunPatch`, `drawLampPool`, `drawDoorSpot`,
  `drawUnderDeskBeam` and `Canvas::darken`. That includes Clawd's shadow (rendered with `shadow = false`) and the
  mini's shadow, which `drawMini` always draws. On a transparent canvas it is a no-op.
- **Sky colours**: SKYA/SKYB are palette entries replaced per time of day. The night windows look day-blue on the
  sheet, but the indices are correct.
- **Tints**: grey/violet Clawd is a 3-entry swap (C/CS/CH -> GC/GCS/GCH or VC/VCS/VCH). Only `clawd_tint` and the
  `clawd_bed_grey` rows are rendered tinted, and minis are violet except `mini_tint`.
- **Inline director drawing**, which is not a draw function: the open book of a mini at the shelf
  (`drawSprites`) and the happy-reaction confetti (`drawClawdNow`).
- **Combinatorial states**: Clawd's arms x eyes x mouth x legs (only the combinations the director composes
  are in `clawd_expr`). Shooting star over other moon or twinkle states. A bird over other cloud positions.
  The active rack LEDs for seeds other than 1 (a hash of seed and t/2). The screensaver beyond t 23 (its real
  period is thousands of ticks). The dash for other endpoints: it is a line between Clawd and each helper.
- The emote colours of `leaf`, `plug` and `coffee` (`tags.used: false`): the director never shows them, so
  they use GRN, INK and W2.
