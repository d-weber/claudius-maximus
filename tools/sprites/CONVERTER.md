# Sprite converter: `tools/sprites.py`

Turns the sheets described in [FORMAT.md](FORMAT.md) into `sprites_gen.h` + `sprites_gen.cpp` for the room.
Everything runs with the host venv (Pillow): `host/.venv/bin/python tools/sprites.py <command>`. The firmware
build runs `build` through the PlatformIO pre-build hook `tools/sprites_prebuild.py`, so the generated files are
never committed (they are gitignored).

## Commands

| Command | What it does |
|---|---|
| `palette [--room-cpp PATH] [--out DIR]` | Reads `room::BASE` from `firmware/src/screens/room/room.cpp` and writes `palette.png` (N_COL x 1, index 0 = `#ff00ff`) + `palette.gpl` into `firmware/assets/sprites/`. Run it after any change to BASE. |
| `build [--atlas PATH] [--out DIR]` | Reads `atlas.json` and every sheet in it, writes `sprites_gen.h/.cpp` (default `firmware/src/screens/room/`). Prints sprites, groups and byte totals. Files are only rewritten when their content changes. |
| `check [--atlas PATH] [--room-cpp PATH] [--gen DIR]` | Validates without writing (see below). With `--gen DIR` it also fails if `DIR/sprites_gen.*` are stale. CI runs it on the committed sheets. |
| `preview [--atlas PATH] --out DIR` | `<sheet>@4x.png`: nearest 4x, transparency as a checkerboard, sprite rectangles outlined. |

Every command exits 1 with an `error:` line that names the sheet, the sprite and the pixel.

`N_COL` comes from the `enum : Idx` in `room.h` (61 today). The tool counts it and never hard-codes it.
`--room-h` overrides where it is read from. A `palette.png` that is wider than N_COL is accepted, and only
its first N_COL pixels are used.

## What gets validated (build and check)

- `atlas.json`: version 1. Names and groups match `[a-z][a-z0-9_]*` and are not C++ keywords. All
  generated identifiers are unique within `room::spr`: sprite names, group names, `<group>_n` and
  `<name>_px`. `w`/`h` are 1..255 (uint8). `ax`/`ay` are -128..127 (int8, so they can sit outside the
  rectangle). Rectangles lie inside their sheet. Each group's frames are exactly 0..n-1.
- Indexed (mode P) sheets: a pixel's value is its index and must be < N_COL. For every index a sprite
  uses, the PNG palette entry must equal BASE. Entries nobody uses only warn.
- RGB/RGBA sheets: these are hand edits from an editor that saved as RGB. Alpha < 128 or `#ff00ff` means
  index 0. Any other pixel must match a BASE colour exactly. An unknown colour fails, naming the sheet
  pixel and the sprite pixel. Opaque black is SHD, since T is never reached by colour. BASE repeats
  `#ffffff` (WHT, STAR, QW) and `#ffd23f` (LEDY, QY). Those entries carry different light weights, so a
  pixel of one of these colours fails and lists its candidate indices. Save such a sheet as indexed:
  Aseprite with `palette.gpl` loaded in Indexed mode, or GIMP Image > Mode > Indexed with that palette.
- `check` only: `palette.png` must equal BASE in `room.cpp`. Each indexed sheet is also exported to RGBA
  in memory and read back the RGB way. That read must give the same indices, modulo the duplicate
  colours above. This catches an index 0 that is opaque and not magenta, or another entry made
  transparent. A sheet that uses duplicate-colour indices gets a note saying it must stay indexed.

## Generated code

The output matches FORMAT.md: `struct Sprite { uint8_t w, h; int8_t ax, ay; uint8_t ox, oy, ow, oh;
uint16_t n; const Sprite *base; const uint8_t *rle; }`, one `extern const Sprite <name>` per sprite. Each
group gets `extern const Sprite *const <group>[n]` and `constexpr int <group>_n`, with frames in frame
order. Pixel data is `static const uint8_t <name>_rle[]` in the .cpp: run-length encoded, and for a later
frame of a group only the rect that differs from frame 0 (`base`), when that is smaller and needs no erase
(FORMAT.md, "Generated code"). Sprites with identical pixels share one array. `build` prints raw vs encoded
bytes. Output is sorted by name and carries a content hash of the atlas and sheets, with no timestamp, so a
rebuild with no changes leaves the files byte-identical. The code is portable C++17 with no Arduino/LVGL
includes.

## Using it in the firmware

`Canvas::blit(int x, int y, const spr::Sprite &s, bool flip = false)` in room.cpp
is the reference decoder: it draws `base` first if there is one, then walks the RLE stream over the overlay
rect, writing non-zero indices into the canvas with clipping. `blitRect` draws only the part of a sprite
inside a canvas rect (the floor under the plant). Usage: `c.blit(pose.x, pose.y, *spr::clawd_front_walk[pose.legPhase]);`

Lighting stays a palette transform, because sprites hold indices. The `const` arrays stay in flash
(`.rodata`) on the ESP32-S3.
