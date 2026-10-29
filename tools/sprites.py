#!/usr/bin/env python3
"""Sprite sheets -> C++ arrays for the room (the contract is tools/sprites/FORMAT.md; usage in CONVERTER.md).

  host/.venv/bin/python tools/sprites.py palette [--room-cpp PATH] [--out DIR]
  host/.venv/bin/python tools/sprites.py build   [--atlas firmware/assets/sprites/atlas.json] [--out DIR]
  host/.venv/bin/python tools/sprites.py check   [--atlas ...] [--room-cpp PATH] [--gen DIR]
  host/.venv/bin/python tools/sprites.py preview [--atlas ...] --out DIR

palette  extracts room::BASE from firmware/src/screens/room/room.cpp (the single source of truth for the
         colours) and writes palette.png (N_COL x 1, index 0 = #ff00ff, the transparent key) + palette.gpl
         (GIMP/Aseprite) into --out (default firmware/assets/sprites). N_COL is counted from room.h's enum
         (61 today; FORMAT.md's "64" is the upper bound, a 64-wide palette.png is read too).
build    reads atlas.json and every sheet it names, cuts each sprite by its rectangle and writes sprites_gen.h +
         sprites_gen.cpp into --out (default firmware/src/screens/room). Files are only rewritten when their
         content changes, and the output is deterministic (sorted, no timestamps).
check    validates the atlas and sheets without writing anything: names, rectangles, anchors, groups,
         indices < N_COL, palette.png against room.cpp, and that every indexed sheet survives an RGB export
         unchanged (what an editor that saves RGB would produce). --gen DIR also fails if DIR/sprites_gen.*
         are stale.
preview  writes <sheet>@4x.png (nearest, transparent as a checkerboard, sprite rectangles outlined) into --out.

Sheets: an indexed PNG (mode P) is read as raw indices (pixel == room::Idx); its palette must equal BASE for
every index a sprite uses (other mismatches only warn). An RGB/RGBA PNG must use BASE colours exactly; alpha <
128 or #ff00ff is transparent (index 0). BASE has duplicate colours (#ffffff = WHT/STAR/QW, #ffd23f = LEDY/QY),
which an RGB pixel cannot tell apart: such a pixel fails with its candidates, keep those sheets indexed.
Exit status 1 with an "error:" line on any failure.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
ROOM_DIR = ROOT / "firmware" / "src" / "screens" / "room"
ROOM_CPP = ROOM_DIR / "room.cpp"
ROOM_H = ROOM_DIR / "room.h"
ASSETS = ROOT / "firmware" / "assets" / "sprites"
ATLAS = ASSETS / "atlas.json"

KEY = (255, 0, 255)              # transparent key colour for index 0 in RGB sheets / palette.png
NAME_RE = re.compile(r"[a-z][a-z0-9_]*\Z")   # FORMAT.md says [a-z0-9_]+; a C identifier can't start with a digit
CPP_KEYWORDS = {
    "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch", "char", "class", "const",
    "constexpr", "continue", "default", "delete", "do", "double", "else", "enum", "explicit", "export", "extern",
    "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new",
    "noexcept", "not", "nullptr", "operator", "or", "private", "protected", "public", "register", "return",
    "short", "signed", "sizeof", "static", "struct", "switch", "template", "this", "throw", "true", "try",
    "typedef", "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "while", "xor",
}
GEN_H, GEN_CPP = "sprites_gen.h", "sprites_gen.cpp"


class SpriteError(Exception):
    """A user-facing failure: printed as `error: ...`, exit status 1."""


def warn(msg: str) -> None:
    print(f"warning: {msg}", file=sys.stderr)


# ---------------------------------------------------------------- palette

def room_n_col(path: Path = ROOM_H) -> int:
    """room::N_COL: the number of names before N_COL in room.h's `enum : Idx { ... }`."""
    m = re.search(r"enum\s*:\s*Idx\s*\{(.*?)\bN_COL\b", path.read_text(), re.S)
    if not m:
        raise SpriteError(f"no `enum : Idx {{ ... N_COL }}` in {path}")
    body = re.sub(r"//[^\n]*", "", m.group(1))
    return len([n for n in body.split(",") if n.strip()])


def room_palette(path: Path = ROOM_CPP) -> tuple[list[tuple[int, int, int]], list[str]]:
    """BASE[] from room.cpp: (colours, names). Names come from each entry's trailing comment (`// OUT ...`).
    The entry count is checked against N_COL in the room.h next to it, when there is one."""
    text = path.read_text()
    m = re.search(r"const\s+Rgb\s+BASE\s*\[\s*N_COL\s*\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise SpriteError(f"no `const Rgb BASE[N_COL] = {{...}};` in {path}")
    cols, names = [], []
    for line in m.group(1).splitlines():
        e = re.match(r"\s*RGB\(\s*0x([0-9a-fA-F]{6})\s*,\s*\d+\s*\)\s*,?\s*(?://\s*(\S+))?", line)
        if e:
            h = int(e.group(1), 16)
            cols.append((h >> 16, (h >> 8) & 0xFF, h & 0xFF))
            names.append(e.group(2) or f"idx{len(names)}")
    hdr = path.with_name("room.h")
    if hdr.is_file() and len(cols) != room_n_col(hdr):
        raise SpriteError(f"{path}: BASE has {len(cols)} RGB(...) entries, room.h N_COL is {room_n_col(hdr)}")
    if not cols:
        raise SpriteError(f"{path}: BASE has no RGB(...) entries")
    cols[0] = KEY   # T is never drawn; the files use magenta so an editor shows it as "the key"
    return cols, names


def write_palette(cols, names, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    im = Image.new("RGB", (len(cols), 1))
    im.putdata(cols)
    im.save(out / "palette.png")
    lines = ["GIMP Palette", "Name: Clawd's Studio (room::BASE)", "Columns: 8", "#"]
    lines += [f"{r:3d} {g:3d} {b:3d}\t{i:02d} {n}" for i, ((r, g, b), n) in enumerate(zip(cols, names))]
    (out / "palette.gpl").write_text("\n".join(lines) + "\n")


def load_palette_png(path: Path, n_col: int) -> list[tuple[int, int, int]]:
    """The first n_col pixels of palette.png's single row. A wider file is accepted (FORMAT.md says 64 x 1
    while room::N_COL is smaller); entries past n_col are ignored and sprite indices must stay < n_col."""
    if not path.is_file():
        raise SpriteError(f"palette {path} not found (tools/sprites.py palette writes it)")
    with Image.open(path) as f:
        im = f.convert("RGB")
    if im.size[1] != 1 or im.size[0] < n_col:
        raise SpriteError(f"{path}: palette must be at least {n_col} x 1, got {im.size[0]} x {im.size[1]}")
    cols = [im.getpixel((i, 0)) for i in range(n_col)]
    cols[0] = KEY
    return cols


def colour_map(pal) -> dict[tuple[int, int, int], list[int]]:
    """RGB -> candidate indices (1..63; index 0 is only ever reached through transparency)."""
    m: dict[tuple[int, int, int], list[int]] = {}
    for i in range(1, len(pal)):
        m.setdefault(tuple(pal[i]), []).append(i)
    if KEY in m:
        raise SpriteError(f"palette index {m[KEY][0]} is #ff00ff, the transparent key")
    return m


def canonical(pal) -> list[int]:
    """Index -> lowest index with the same colour (what an RGB round trip can preserve)."""
    first: dict[tuple, int] = {}
    return [0] + [first.setdefault(tuple(pal[i]), i) for i in range(1, len(pal))]


# ---------------------------------------------------------------- atlas

def _int(sp, key, lo, hi, where):
    v = sp.get(key)
    if not isinstance(v, int) or isinstance(v, bool) or not lo <= v <= hi:
        raise SpriteError(f"{where}: `{key}` must be an integer in {lo}..{hi}, got {v!r}")
    return v


def load_atlas(path: Path) -> dict:
    """Parse and validate atlas.json. Returns {"dir", "palette", "sheets": {name: {"file", "sprites"}},
    "sprites": [sprite dicts with "sheet"], "groups": {group: [sprites by frame]}}."""
    if not path.is_file():
        raise SpriteError(f"atlas {path} not found")
    try:
        doc = json.loads(path.read_text())
    except json.JSONDecodeError as e:
        raise SpriteError(f"{path}: invalid JSON: {e}") from None
    if doc.get("version") != 1:
        raise SpriteError(f"{path}: unsupported atlas version {doc.get('version')!r} (expected 1)")
    base = path.parent
    sheets, sprites = {}, []
    for sname, sh in sorted((doc.get("sheets") or {}).items()):
        if "file" not in sh:
            raise SpriteError(f"sheet {sname}: no `file`")
        sheets[sname] = {"file": base / sh["file"], "sprites": []}
        for i, sp in enumerate(sh.get("sprites") or []):
            name = sp.get("name")
            where = f"sheet {sname}, sprite {name or '#' + str(i)}"
            if not isinstance(name, str) or not NAME_RE.match(name) or name in CPP_KEYWORDS:
                raise SpriteError(f"{where}: name {name!r} is not a C identifier of the form [a-z][a-z0-9_]*")
            s = {"name": name, "sheet": sname}
            s["x"] = _int(sp, "x", 0, 1 << 16, where)
            s["y"] = _int(sp, "y", 0, 1 << 16, where)
            s["w"] = _int(sp, "w", 1, 255, where)     # Sprite::w/h are uint8_t
            s["h"] = _int(sp, "h", 1, 255, where)
            s["ax"] = _int(sp, "ax", -128, 127, where)  # Sprite::ax/ay are int8_t: may lie outside the rect
            s["ay"] = _int(sp, "ay", -128, 127, where)
            g = sp.get("group")
            if g is not None:
                if not isinstance(g, str) or not NAME_RE.match(g) or g in CPP_KEYWORDS:
                    raise SpriteError(f"{where}: group {g!r} is not a C identifier of the form [a-z][a-z0-9_]*")
                s["group"] = g
                s["frame"] = _int(sp, "frame", 0, 255, where)
            sheets[sname]["sprites"].append(s)
            sprites.append(s)
    if not sprites:
        raise SpriteError(f"{path}: no sprites")

    # Every generated identifier lives in room::spr, so sprites, groups, <group>_n and <name>_px must not clash.
    seen: dict[str, str] = {"Sprite": "the struct"}
    def claim(ident, what):
        if ident in seen:
            raise SpriteError(f"name clash: `{ident}` is both {seen[ident]} and {what}")
        seen[ident] = what
    for s in sprites:
        claim(s["name"], f"sprite {s['name']} (sheet {s['sheet']})")
    groups: dict[str, list] = {}
    for s in sprites:
        if "group" in s:
            groups.setdefault(s["group"], []).append(s)
    for g in sorted(groups):
        claim(g, f"group {g}")
        claim(g + "_n", f"the frame count of group {g}")
    for s in sprites:
        claim(s["name"] + "_rle", f"the pixel data of sprite {s['name']}")
    for g, fr in groups.items():
        fr.sort(key=lambda s: s["frame"])
        got = [s["frame"] for s in fr]
        if got != list(range(len(fr))):
            raise SpriteError(f"group {g}: frames must be 0..{len(fr) - 1} with no gaps or repeats, got {got}")
    return {"path": path, "dir": base, "palette": base / doc.get("palette", "palette.png"), "sheets": sheets,
            "sprites": sorted(sprites, key=lambda s: s["name"]), "groups": dict(sorted(groups.items())),
            "by_name": {s["name"]: s for s in sprites}}


# ---------------------------------------------------------------- sheets

def open_sheet(path: Path) -> Image.Image:
    if not path.is_file():
        raise SpriteError(f"sheet {path} not found")
    with Image.open(path) as im:
        return im.copy()   # copy keeps mode, palette and info (tRNS) without holding the file open


def sheet_sprites(im: Image.Image, sheet: str, sprites, pal, ambiguous: str = "fail") -> dict[str, bytes]:
    """Cut every sprite of one sheet into w*h indices. ambiguous: what an RGB pixel matching several palette
    entries becomes: "fail", or "canon" (the lowest index; only used by check's round trip)."""
    W, H = im.size
    for s in sprites:
        if s["x"] + s["w"] > W or s["y"] + s["h"] > H:
            raise SpriteError(f"sheet {sheet}, sprite {s['name']}: rectangle {s['x']},{s['y']} {s['w']}x{s['h']} "
                              f"is outside the {W}x{H} sheet")
    out = {}
    if im.mode == "P":
        data = im.tobytes()
        flat = im.getpalette() or []
        ppal = [tuple(flat[i:i + 3]) for i in range(0, len(flat), 3)]
        used = set()
        for s in sprites:
            px = bytearray()
            for y in range(s["y"], s["y"] + s["h"]):
                row = data[y * W + s["x"]: y * W + s["x"] + s["w"]]
                for dx, v in enumerate(row):
                    if v >= len(pal):
                        raise SpriteError(f"sheet {sheet}, sprite {s['name']}: index {v} >= N_COL {len(pal)} at "
                                          f"sheet pixel "
                                          f"({s['x'] + dx},{y}) (sprite pixel ({dx},{y - s['y']}))")
                px += row
            used.update(px)
            out[s["name"]] = bytes(px)
        for i in sorted(used - {0}):
            if i >= len(ppal) or ppal[i] != tuple(pal[i]):
                have = "missing" if i >= len(ppal) else "#%02x%02x%02x" % ppal[i]
                raise SpriteError(f"sheet {sheet}: PNG palette entry {i} is {have}, BASE[{i}] is "
                                  f"#%02x%02x%02x (the sheet's palette is not room::BASE in index order)" % tuple(pal[i]))
        bad = [i for i in range(1, min(len(pal), len(ppal))) if i not in used and ppal[i] != tuple(pal[i])]
        if bad:
            warn(f"sheet {sheet}: unused PNG palette entries differ from BASE: {bad}")
        return out

    cmap = colour_map(pal)
    data = im.convert("RGBA").tobytes()
    for s in sprites:
        px = bytearray()
        for y in range(s["y"], s["y"] + s["h"]):
            for x in range(s["x"], s["x"] + s["w"]):
                o = (y * W + x) * 4
                r, g, b, a = data[o:o + 4]
                if a < 128 or (r, g, b) == KEY:
                    px.append(0)
                    continue
                cand = cmap.get((r, g, b))
                at = f"sheet {sheet}, sprite {s['name']}: colour #{r:02x}{g:02x}{b:02x} at sheet pixel ({x},{y}) " \
                     f"(sprite pixel ({x - s['x']},{y - s['y']}))"
                if not cand:
                    raise SpriteError(f"{at} is not in the palette")
                if len(cand) > 1 and ambiguous == "fail":
                    raise SpriteError(f"{at} matches several palette entries {cand}; save this sheet as indexed "
                                      f"(mode P) so the index is explicit")
                px.append(cand[0])
        out[s["name"]] = bytes(px)
    return out


def read_all(atlas: dict, pal) -> tuple[dict[str, bytes], dict[str, Image.Image]]:
    pixels, images = {}, {}
    for sname, sh in atlas["sheets"].items():
        im = open_sheet(sh["file"])
        images[sname] = im
        pixels.update(sheet_sprites(im, sname, sh["sprites"], pal))
    return pixels, images


# ---------------------------------------------------------------- code generation

def source_label(atlas: dict) -> str:
    p = atlas["path"].resolve()
    try:
        return str(p.relative_to(ROOT))
    except ValueError:
        return p.name


def content_hash(atlas: dict) -> str:
    """Hash of the atlas and every sheet file, so a generated file names exactly what it came from."""
    h = hashlib.sha256(atlas["path"].read_bytes())
    for sname in sorted(atlas["sheets"]):
        h.update(atlas["sheets"][sname]["file"].read_bytes())
    return h.hexdigest()[:16]


STRUCT = ("struct Sprite { uint8_t w, h; int8_t ax, ay; uint8_t ox, oy, ow, oh; uint16_t n; "
          "const Sprite *base; const uint8_t *rle; };")


def rle_encode(px: bytes) -> bytes:
    """Runs for pixel art. Control byte c: c < 0x80 repeats the next byte c times (1..127); 0xC0 | n is followed
    by n (1..63) literal bytes that are all opaque (the decoder copies them as one block); 0x80 | n by n literal
    bytes that may contain transparent pixels (the decoder checks each). Transparent and flat areas collapse to
    two bytes each; textured opaque areas (floor planks) cost one byte per pixel plus one per 63."""
    # A run only pays when it is long: every control byte is a loop iteration for the decoder (the ESP32-S3
    # spends ~50 cycles per segment, flash reads included), so short repeats stay inside literal blocks.
    MIN_RUN = 8
    out = bytearray()
    i, n = 0, len(px)

    def run_at(k: int) -> int:
        j = k
        while j < n and j - k < 127 and px[j] == px[k]:
            j += 1
        return j - k

    while i < n:
        r = run_at(i)
        if r >= MIN_RUN:
            out += bytes((r, px[i]))
            i += r
            continue
        j = i
        while j < n and j - i < 63 and run_at(j) < MIN_RUN:
            j += 1
        # split the literal where opacity changes, so most of it goes out as opaque blocks
        k = i
        while k < j:
            m = k
            opaque = px[k] != 0
            while m < j and (px[m] != 0) == opaque:
                m += 1
            out += bytes(((0xC0 if opaque else 0x80) | (m - k),)) + px[k:m]
            k = m
        i = j
    return bytes(out)


def rle_decode(data: bytes, count: int) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        c = data[i]
        i += 1
        if c & 0x80:
            k = c & 0x3F
            out += data[i:i + k]
            i += k
        else:
            out += data[i:i + 1] * c
            i += 1
    if len(out) != count:
        raise ValueError(f"rle decodes to {len(out)} bytes, expected {count}")
    return bytes(out)


def delta(w: int, h: int, px: bytes, base: bytes):
    """The overlay (ox, oy, ow, oh, pixels) that turns `base` into `px`: the bounding rect of the differing
    pixels, with 0 where the base pixel stays. None when a pixel would have to be erased (base opaque, frame
    transparent): index 0 means "keep", so such a frame is stored whole."""
    xs, ys = [], []
    for k in range(w * h):
        if px[k] != base[k]:
            if px[k] == 0:
                return None
            xs.append(k % w)
            ys.append(k // w)
    if not xs:
        return (0, 0, 1, 1, bytes(1))   # identical: an overlay of one transparent pixel
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    sub = bytes(px[y * w + x] if px[y * w + x] != base[y * w + x] else 0
                for y in range(y0, y1 + 1) for x in range(x0, x1 + 1))
    return (x0, y0, x1 - x0 + 1, y1 - y0 + 1, sub)


def encode_all(atlas: dict, pixels: dict[str, bytes]) -> dict[str, dict]:
    """Per sprite: base (name of its group's frame 0, or None), the overlay rect and its RLE bytes. Frame 0 of a
    group and sprites without a group are stored whole (rect = the sprite). A later frame of the same size is
    stored as a delta over frame 0 when that is smaller than storing it whole, and never needs an erase."""
    first = {}
    for g, fr in atlas["groups"].items():
        first[g] = fr[0]["name"]
    enc = {}
    for s in atlas["sprites"]:
        n, w, h = s["name"], s["w"], s["h"]
        px = pixels[n]
        whole = rle_encode(px)
        best = {"base": None, "rect": (0, 0, w, h), "rle": whole}
        b = first.get(s.get("group"))
        if b and b != n and (atlas["by_name"][b]["w"], atlas["by_name"][b]["h"]) == (w, h):
            d = delta(w, h, px, pixels[b])
            if d is not None:
                data = rle_encode(d[4])
                if len(data) < len(whole):
                    best = {"base": b, "rect": d[:4], "rle": data}
        enc[n] = best
    return enc


def generate(atlas: dict, pixels: dict[str, bytes]) -> tuple[str, str]:
    files = ", ".join(sorted(sh["file"].name for sh in atlas["sheets"].values()))
    head = (f"// GENERATED by tools/sprites.py build from {source_label(atlas)} ({files}),\n"
            f"// content hash {content_hash(atlas)}. Do not edit: change the sheets and rebuild "
            f"(see tools/sprites/CONVERTER.md).\n")
    h = [head + "#pragma once\n#include <stdint.h>\n", "namespace room { namespace spr {\n",
         "// A sprite is w x h palette indices (room::Idx, 0 = T), drawn with its anchor (ax, ay) on the given point.",
         "// `base` (its group's frame 0, or nullptr) is drawn first; then `rle` (n bytes) covers only the rect",
         "// (ox, oy, ow, oh) inside the sprite, 0 keeping what is there. RLE: a control byte c < 0x80 repeats the",
         "// next byte c times; 0xC0|n is followed by n opaque literal bytes, 0x80|n by n literal bytes that may",
         "// contain 0 (tools/sprites/FORMAT.md). Rows are consecutive.",
         STRUCT + "\n"]
    h += [f"extern const Sprite {s['name']};" for s in atlas["sprites"]]
    if atlas["groups"]:
        h.append("")
        for g, fr in atlas["groups"].items():
            h.append(f"extern const Sprite *const {g}[{len(fr)}];")
            h.append(f"constexpr int {g}_n = {len(fr)};")
    h.append("\n}}  // namespace room::spr\n")

    c = [head + f'#include "{GEN_H}"\n', "namespace room { namespace spr {\n"]
    enc = encode_all(atlas, pixels)
    # Identical frames share one array (ARMS_HOLD draws like ARMS_DOWN, a blink frame repeats a walk frame, window
    # variants repeat...): the first sprite with a given (w, h, pixels) owns the encoding, the others reuse it.
    owners: dict[tuple, str] = {}
    shared = deltas = data_bytes = 0
    for s in atlas["sprites"]:
        n, w, hh = s["name"], s["w"], s["h"]
        key = (w, hh, pixels[n])
        owner = owners.get(key)
        e = enc[owner or n]
        ox, oy, ow, oh = e["rect"]
        base = f"&{e['base']}" if e["base"] else "nullptr"
        if owner is None:
            owners[key] = n
            data = e["rle"]
            data_bytes += len(data)
            deltas += e["base"] is not None
            c.append(f"static const uint8_t {n}_rle[{len(data)}] = {{")
            for k in range(0, len(data), 32):
                c.append("    " + ",".join(str(v) for v in data[k:k + 32]) + ",")
            c.append("};")
            note = f"   // {ow}x{oh} over {e['base']}" if e["base"] else ""
            c.append(f"const Sprite {n} = {{{w}, {hh}, {s['ax']}, {s['ay']}, {ox}, {oy}, {ow}, {oh}, {len(data)}, {base}, {n}_rle}};{note}\n")
        else:
            shared += 1
            c.append(f"const Sprite {n} = {{{w}, {hh}, {s['ax']}, {s['ay']}, {ox}, {oy}, {ow}, {oh}, {len(e['rle'])}, {base}, "
                     f"{owner}_rle}};   // same pixels as {owner}\n")
    atlas["stats"] = {"shared": shared, "deltas": deltas, "data_bytes": data_bytes,
                      "raw_bytes": sum(len(p) for p in pixels.values())}
    for g, fr in atlas["groups"].items():
        c.append(f"const Sprite *const {g}[{len(fr)}] = {{" + ", ".join("&" + s["name"] for s in fr) + "};")
    c.append("\n}}  // namespace room::spr\n")
    return "\n".join(h), "\n".join(c)


def write_if_changed(path: Path, text: str) -> bool:
    if path.is_file() and path.read_text() == text:
        return False
    path.write_text(text)
    return True


# ---------------------------------------------------------------- commands

def cmd_palette(a) -> int:
    cols, names = room_palette(Path(a.room_cpp))
    write_palette(cols, names, Path(a.out))
    print(f"palette: {len(cols)} colours from {a.room_cpp} -> {Path(a.out) / 'palette.png'}, palette.gpl")
    return 0


def cmd_build(a) -> int:
    atlas = load_atlas(Path(a.atlas))
    pal = load_palette_png(atlas["palette"], room_n_col(Path(a.room_h)))
    pixels, _ = read_all(atlas, pal)
    hdr, cpp = generate(atlas, pixels)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    changed = [n for n, t in ((GEN_H, hdr), (GEN_CPP, cpp)) if write_if_changed(out / n, t)]
    st = atlas["stats"]
    print(f"sprites: {len(atlas['sprites'])} sprites, {len(atlas['groups'])} groups, {st['raw_bytes']} raw pixel bytes "
          f"-> {st['data_bytes']} B encoded ({st['deltas']} frames as deltas over frame 0, {st['shared']} sharing "
          f"another's data; + {16 * len(atlas['sprites'])} B of Sprite structs) -> {out}/{GEN_H}, {GEN_CPP}"
          f"{'' if changed else ' (unchanged)'}")
    return 0


def cmd_check(a) -> int:
    atlas = load_atlas(Path(a.atlas))
    pal = load_palette_png(atlas["palette"], room_n_col(Path(a.room_h)))
    notes = []
    rc = Path(a.room_cpp)
    if rc.is_file():
        base, names = room_palette(rc)
        diff = [f"{i} {names[i]}" for i in range(1, len(pal)) if tuple(base[i]) != tuple(pal[i])]
        if diff:
            raise SpriteError(f"{atlas['palette']} differs from BASE in {rc} at: {', '.join(diff)} "
                              f"(rerun tools/sprites.py palette)")
    else:
        notes.append(f"{rc} not found, palette.png not compared with BASE")

    pixels, images = read_all(atlas, pal)   # rectangles, indices < 64, PNG palette / exact colours
    canon = canonical(pal)
    for sname, im in images.items():
        if im.mode != "P":
            notes.append(f"sheet {sname} is {im.mode}, not indexed: no RGB round trip to do")
            continue
        # What an editor saving this sheet as RGBA would produce, read back the RGB way.
        rgba = im.convert("RGBA")
        back = sheet_sprites(rgba, sname, atlas["sheets"][sname]["sprites"], pal, ambiguous="canon")
        for s in atlas["sheets"][sname]["sprites"]:
            orig, got = pixels[s["name"]], back[s["name"]]
            for k, (o, g) in enumerate(zip(orig, got)):
                if canon[o] != g:
                    x, y = k % s["w"], k // s["w"]
                    raise SpriteError(f"sheet {sname}, sprite {s['name']}: RGB round trip changes sprite pixel "
                                      f"({x},{y}) from index {o} to {g} (is index 0's PNG entry opaque and not "
                                      f"#ff00ff, or another entry transparent?)")
        amb = sorted({o for s in atlas["sheets"][sname]["sprites"] for o in pixels[s["name"]] if canon[o] != o})
        if amb:
            notes.append(f"sheet {sname} uses duplicate-colour indices {amb}: keep it indexed (an RGB save "
                         f"would be rejected)")

    if a.gen:
        hdr, cpp = generate(atlas, pixels)
        for n, t in ((GEN_H, hdr), (GEN_CPP, cpp)):
            p = Path(a.gen) / n
            if not p.is_file() or p.read_text() != t:
                raise SpriteError(f"{p} is stale or missing: run tools/sprites.py build")

    for n in notes:
        print(f"note: {n}")
    print(f"check: OK, {len(atlas['sprites'])} sprites on {len(atlas['sheets'])} sheets, "
          f"{len(atlas['groups'])} groups")
    return 0


def cmd_preview(a) -> int:
    atlas = load_atlas(Path(a.atlas))
    pal = load_palette_png(atlas["palette"], room_n_col(Path(a.room_h)))
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    S = 4
    for sname, sh in atlas["sheets"].items():
        im = open_sheet(sh["file"])
        px = sheet_sprites(im, sname, sh["sprites"], pal)
        W, H = im.size
        big = Image.new("RGB", (W * S, H * S))
        d = ImageDraw.Draw(big)
        for y in range(H):            # checkerboard = transparent, also outside every sprite
            for x in range(W):
                d.rectangle([x * S, y * S, x * S + S - 1, y * S + S - 1],
                            fill=(60, 60, 68) if (x + y) & 1 else (44, 44, 50))
        for s in sh["sprites"]:
            data = px[s["name"]]
            for k, v in enumerate(data):
                if v:
                    x, y = s["x"] + k % s["w"], s["y"] + k // s["w"]
                    d.rectangle([x * S, y * S, x * S + S - 1, y * S + S - 1], fill=tuple(pal[v]))
            d.rectangle([s["x"] * S - 1, s["y"] * S - 1, (s["x"] + s["w"]) * S, (s["y"] + s["h"]) * S],
                        outline=(0, 230, 255))
        p = out / f"{sname}@4x.png"
        big.save(p)
        print(f"preview: {p}")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("palette", help="room.cpp BASE -> palette.png + palette.gpl")
    p.add_argument("--room-cpp", default=str(ROOM_CPP))
    p.add_argument("--out", default=str(ASSETS))
    p.set_defaults(f=cmd_palette)
    p = sub.add_parser("build", help="sheets + atlas.json -> sprites_gen.h/.cpp")
    p.add_argument("--atlas", default=str(ATLAS))
    p.add_argument("--room-h", default=str(ROOM_H), help="where N_COL is read from")
    p.add_argument("--out", default=str(ROOM_DIR))
    p.set_defaults(f=cmd_build)
    p = sub.add_parser("check", help="validate the atlas and sheets")
    p.add_argument("--atlas", default=str(ATLAS))
    p.add_argument("--room-h", default=str(ROOM_H), help="where N_COL is read from")
    p.add_argument("--room-cpp", default=str(ROOM_CPP))
    p.add_argument("--gen", help="also fail if DIR/sprites_gen.h/.cpp differ from a fresh build")
    p.set_defaults(f=cmd_check)
    p = sub.add_parser("preview", help="4x previews of the sheets")
    p.add_argument("--atlas", default=str(ATLAS))
    p.add_argument("--room-h", default=str(ROOM_H), help="where N_COL is read from")
    p.add_argument("--out", required=True)
    p.set_defaults(f=cmd_preview)
    a = ap.parse_args(argv)
    try:
        return a.f(a)
    except SpriteError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
