# Sprite sheets for Clawd's Studio: file format

The home-screen room (`firmware/src/screens/room/`) draws into a 120 x 106 canvas of **palette indices**
(`room::Idx`, one byte per pixel, `room::BASE[N_COL]` entries, index 0 = `T` = transparent). `N_COL` is
whatever room.h says (61 at the time of writing; tools read it from the source, never hard-code it). Lighting
is a palette transform, so art must use palette indices, never free colours. Sprite sheets keep that model: a
sheet is a PNG whose pixels are palette indices, and the build converts it into C arrays the firmware blits.

## Files (committed under `firmware/assets/sprites/`)

- `palette.png`: N_COL x 1 image, pixel i = `BASE[i]` RGB (index 0 is magenta `#ff00ff` as the transparent
  key). `palette.gpl` is the same as a GIMP/Aseprite palette, so an editor shows the right colours in index
  order. Readers accept a wider image and use the first N_COL pixels.
- `<sheet>.png`: one sheet per group (`furniture.png`, `clawd.png`, `minis.png`, `props.png`, `fx.png`,
  `window.png`, ...). **Indexed-colour PNG (mode P)** whose palette is exactly the `BASE` colours in index
  order (entries beyond N_COL unused), with index 0 transparent (tRNS). Pixel value == `room::Idx`. This is
  the authoring format: edit sheets in an editor's indexed mode with `palette.gpl` loaded, so indices survive.
  A sheet saved as RGB/RGBA is accepted only as far as it is unambiguous: BASE repeats some colours (`#ffffff`
  is WHT, STAR and QW; `#ffd23f` is LEDY and QY) and the entries differ in their lighting weight, so an RGB
  pixel with several candidate indices is an error naming them, as is any colour not in the palette. Alpha
  < 128 or `#ff00ff` means transparent; opaque `#000000` is SHD (T is only ever reached through
  transparency).
- `atlas.json`: where every sprite is on which sheet, and its anchor.
- No previews live here: `make_sheets.py --preview DIR` and `sprites.py preview` write labelled 4x views
  elsewhere on request. The indexed sheets are the only art.

Sheets are laid out like a DOS-era sprite sheet (Tyrian, Jazz): a uniform grid of cells per sheet, one row per
animation or state group, frames left to right in play order, 1 px gutter between cells. Cell size is the
largest sprite on that sheet. Unused cells are transparent.

## atlas.json

```json
{
  "version": 1,
  "palette": "palette.png",
  "canvas": {"w": 120, "h": 106},
  "sheets": {
    "clawd": {
      "file": "clawd.png",
      "cell": {"w": 24, "h": 20},
      "sprites": [
        {"name": "clawd_front_walk_0", "x": 1, "y": 1, "w": 18, "h": 12, "ax": 0, "ay": 0,
         "group": "clawd_front_walk", "frame": 0,
         "tags": {"view": "front", "legPhase": 0, "arms": "down", "eyes": "open", "mouth": "none"}}
      ]
    }
  }
}
```

- `x, y, w, h`: the sprite's pixel rectangle on the sheet (the cropped bounding box of its opaque pixels; it
  may be smaller than the cell). Transparent pixels inside the rectangle stay index 0.
- `ax, ay`: the **anchor**, in sprite pixels from the rectangle's top-left, of the position the firmware
  passes to the draw call today (Clawd: pose `x, y`, the 18 x 12 body's top-left; furniture: the canvas
  position it is drawn at, so `ax, ay` = minus the canvas x, y of the crop, i.e. blitting at anchor (0, 0)
  puts it back where `drawDesk` & co. draw it). `blit(x - ax, y - ay, sprite)` reproduces the original.
- `group` + `frame`: frames of one animation/state set, in order; the converter emits one array per group.
- `tags`: free-form, what the frame represents (view, phase, state, icon...). Human/tool documentation only.
- Names are C identifiers: `[a-z0-9_]+`, unique across all sheets.

## Generated code (`tools/sprites.py build`)

`firmware/src/screens/room/sprites_gen.h` + `sprites_gen.cpp` (portable C++, no Arduino/LVGL includes, so
`tools/room_sim.py` builds them too):

```cpp
namespace room { namespace spr {
struct Sprite { uint8_t w, h; int8_t ax, ay; uint8_t ox, oy, ow, oh; uint16_t n; const Sprite *base; const uint8_t *rle; };
extern const Sprite clawd_front_walk_0;
extern const Sprite *const clawd_front_walk[4];                      // one table per group, frames in order
constexpr int clawd_front_walk_n = 4;
}}  // namespace room::spr
```

The pixel data is compact, because blank space and repeated frames are what a sheet mostly is:

- **RLE**: `rle` holds `n` bytes; a control byte `c < 0x80` repeats the next byte `c` times (1..127);
  `0xC0 | n` is followed by `n` (1..63) literal bytes that are all opaque, which the decoder copies as one
  block; `0x80 | n` by `n` literal bytes that may contain index 0, checked pixel by pixel. Rows follow each
  other. Transparent and flat runs cost two bytes.
- **Deltas**: frame 0 of a group is stored whole. A later frame of the same size is stored as an overlay over
  frame 0 (`base`): `rle` then covers only the rect `(ox, oy, ow, oh)` inside the sprite, index 0 meaning "keep
  the base pixel". A frame that would need to erase a base pixel, or whose delta is not smaller, is stored whole
  (`base = nullptr`, rect = the sprite). `base` is never itself a delta.
- **Sharing**: sprites with identical pixels point at one array.

Drawing a sprite: top-left = (x - ax, y - ay); if `base`, draw it there first (its own anchor is ignored); then
decode `rle` over the rect, skipping index 0, clipped to the canvas; `flip` mirrors within `w` (the rect moves
to `w - ox - ow`). `const` arrays stay in flash (`.rodata`). On the current art this is ~49 KB instead of
218 KB plain (`tools/sprites.py build` prints the numbers).

## Round trip

`tools/sprites.py check` converts every sheet back through the atlas and compares each sprite with the
original canvas pixels, so a sheet edited by hand is validated the same way as a generated one.
