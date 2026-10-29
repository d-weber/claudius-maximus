"""Tests for tools/sprites.py (sheet -> C++ converter) on small synthetic sheets; nothing is compiled.

Run: host/.venv/bin/python -m unittest discover -s host/tests -v
"""
from __future__ import annotations

import collections
import contextlib
import importlib.util
import io
import json
import re
import tempfile
import unittest
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("sprites_tool", ROOT / "tools" / "sprites.py")
sprites = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sprites)

PAL, NAMES = sprites.room_palette()
IDX = {n: i for i, n in enumerate(NAMES)}
N = len(PAL)
OUT, WAL, C, CS, RED, GRN, SHD = (IDX[k] for k in ("OUT", "WAL", "C", "CS", "RED", "GRN", "SHD"))

# Sheet "clawd" (mode P): a 2-frame group + a lone sprite. Frames listed out of order on purpose.
WALK0 = [[OUT, C, OUT], [0, CS, 0]]
WALK1 = [[OUT, C, 0], [CS, 0, CS]]
BOX = [[WAL, WAL], [WAL, SHD]]
# Sheet "fx" (RGBA): magenta and alpha 0 are both transparent.
DOT = [[RED, "M", RED], ["A", GRN, "A"], [RED, "M", RED]]


def p_sheet(path: Path, w: int, h: int, sprites_px) -> None:
    im = Image.new("P", (w, h), 0)
    flat = []
    for c in PAL:
        flat += c
    im.putpalette(flat + [0] * (768 - len(flat)))
    for (x0, y0), rows in sprites_px:
        for y, row in enumerate(rows):
            for x, v in enumerate(row):
                im.putpixel((x0 + x, y0 + y), v)
    im.save(path, transparency=0)


def rgba_sheet(path: Path, w: int, h: int, sprites_px) -> None:
    im = Image.new("RGBA", (w, h), (255, 0, 255, 255))
    for (x0, y0), rows in sprites_px:
        for y, row in enumerate(rows):
            for x, v in enumerate(row):
                c = (255, 0, 255, 255) if v == "M" else (10, 20, 30, 0) if v == "A" else (*v, 255) \
                    if isinstance(v, tuple) else (*PAL[v], 255)
                im.putpixel((x0 + x, y0 + y), c)
    im.save(path)


def sp(name, x, y, w, h, ax=0, ay=0, group=None, frame=None):
    d = {"name": name, "x": x, "y": y, "w": w, "h": h, "ax": ax, "ay": ay, "tags": {}}
    if group:
        d["group"], d["frame"] = group, frame
    return d


def make_atlas(d: Path, fx_dot=DOT, extra=None) -> Path:
    sprites.write_palette(PAL, NAMES, d)
    p_sheet(d / "clawd.png", 16, 8, [((1, 1), WALK0), ((5, 1), WALK1), ((9, 1), BOX)])
    rgba_sheet(d / "fx.png", 8, 8, [((1, 1), fx_dot)])
    atlas = {"version": 1, "palette": "palette.png", "canvas": {"w": 120, "h": 106}, "sheets": {
        "clawd": {"file": "clawd.png", "cell": {"w": 4, "h": 4}, "sprites": [
            sp("clawd_walk_1", 5, 1, 3, 2, 0, -1, "clawd_walk", 1),
            sp("clawd_walk_0", 1, 1, 3, 2, 0, -1, "clawd_walk", 0),
            sp("desk_box", 9, 1, 2, 2, -40, 100)]},
        "fx": {"file": "fx.png", "cell": {"w": 4, "h": 4}, "sprites": [sp("fx_dot", 1, 1, 3, 3, 1, 1)]}}}
    if extra:
        extra(atlas)
    path = d / "atlas.json"
    path.write_text(json.dumps(atlas))
    return path


def load(path: Path) -> Image.Image:
    with Image.open(path) as im:
        return im.copy()


def run(*argv) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = sprites.main(list(argv))
    return rc, out.getvalue(), err.getvalue()


def array_bytes(cpp: str, name: str) -> bytes:
    """The RLE bytes of a sprite's own array."""
    m = re.search(rf"static const uint8_t {name}_rle\[(\d+)\] = \{{(.*?)\}};", cpp, re.S)
    vals = bytes(int(v) for v in m.group(2).replace("\n", "").split(",") if v.strip())
    assert len(vals) == int(m.group(1))
    return vals


def decoded(cpp: str, name: str, w: int, h: int) -> list[int]:
    """The sprite's pixels as the firmware would draw it: whole, or its group's frame 0 plus the overlay."""
    m = re.search(rf"const Sprite {name} = \{{(.*?)\}};", cpp)
    f = [x.strip() for x in m.group(1).split(",")]
    ox, oy, ow, oh, n, base, arr = int(f[4]), int(f[5]), int(f[6]), int(f[7]), int(f[8]), f[9], f[10][:-4]
    px = list(decoded(cpp, base[1:], w, h)) if base != "nullptr" else [0] * (w * h)
    over = sprites.rle_decode(array_bytes(cpp, arr), ow * oh)
    assert n == len(array_bytes(cpp, arr))
    for j in range(oh):
        for i in range(ow):
            v = over[j * ow + i]
            if v:
                px[(oy + j) * w + ox + i] = v
    return px


class Build(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        self.gen = self.d / "gen"

    def tearDown(self):
        self.tmp.cleanup()

    def build(self, **kw):
        return run("build", "--atlas", str(make_atlas(self.d, **kw)), "--out", str(self.gen))

    def test_palette_matches_room_cpp(self):
        self.assertEqual(N, sprites.room_n_col())
        self.assertEqual(PAL[0], (255, 0, 255))
        im = load(make_atlas(self.d).parent / "palette.png")
        self.assertEqual(im.size, (N, 1))
        gpl = (self.d / "palette.gpl").read_text().splitlines()
        self.assertEqual(gpl[0], "GIMP Palette")
        self.assertEqual(len(gpl), 4 + N)

    def test_generated_code(self):
        rc, out, err = self.build()
        self.assertEqual(rc, 0, err)
        self.assertIn("4 sprites, 1 groups, 25 raw pixel bytes", out)
        h = (self.gen / "sprites_gen.h").read_text()
        cpp = (self.gen / "sprites_gen.cpp").read_text()
        self.assertTrue(h.startswith("// GENERATED by tools/sprites.py build from atlas.json"))
        self.assertIn("#pragma once\n#include <stdint.h>", h)
        self.assertIn("namespace room { namespace spr {", h)
        self.assertIn(sprites.STRUCT, h)
        self.assertNotRegex(h + cpp, r"Arduino|lvgl")
        externs = re.findall(r"^extern const Sprite (\w+);$", h, re.M)
        self.assertEqual(externs, ["clawd_walk_0", "clawd_walk_1", "desk_box", "fx_dot"])   # sorted
        self.assertIn("extern const Sprite *const clawd_walk[2];\nconstexpr int clawd_walk_n = 2;", h)
        self.assertIn("const Sprite *const clawd_walk[2] = {&clawd_walk_0, &clawd_walk_1};", cpp)
        self.assertEqual(decoded(cpp, "clawd_walk_0", 3, 2), sum(WALK0, []))
        self.assertEqual(decoded(cpp, "clawd_walk_1", 3, 2), sum(WALK1, []))   # needs erases: stored whole
        self.assertIn(", nullptr, clawd_walk_1_rle};", cpp)
        self.assertEqual(decoded(cpp, "fx_dot", 3, 3), [RED, 0, RED, 0, GRN, 0, RED, 0, RED])
        self.assertEqual(decoded(cpp, "desk_box", 2, 2), [WAL, WAL, WAL, SHD])
        box = sprites.rle_encode(bytes([WAL, WAL, WAL, SHD]))
        self.assertIn(f"const Sprite desk_box = {{2, 2, -40, 100, 0, 0, 2, 2, {len(box)}, nullptr, desk_box_rle}};", cpp)
        self.assertEqual(array_bytes(cpp, "desk_box"), box)

    def test_rle_and_deltas(self):
        enc, dec = sprites.rle_encode, sprites.rle_decode
        for px in (b"", b"\x05", b"\x05\x05\x05\x05", bytes(range(1, 200)), bytes(300), b"\x01\x02\x02\x02\x02\x03"):
            self.assertEqual(dec(enc(px), len(px)), px)
        self.assertEqual(enc(bytes(300)), bytes([127, 0, 127, 0, 46, 0]))          # a blank 300-pixel run: 6 bytes
        self.assertEqual(enc(b"\x01\x02\x00\x03"), bytes([0xC2, 1, 2, 0x81, 0, 0xC1, 3]))   # opaque / mixed literals
        self.assertEqual(enc(bytes(range(1, 100))), bytes([0xC0 | 63]) + bytes(range(1, 64)) + bytes([0xC0 | 36]) + bytes(range(64, 100)))
        self.assertEqual(sprites.delta(2, 2, b"\x01\x02\x03\x04", b"\x01\x02\x03\x04"), (0, 0, 1, 1, b"\x00"))
        self.assertEqual(sprites.delta(2, 2, b"\x01\x09\x03\x04", b"\x01\x02\x03\x04"), (1, 0, 1, 1, b"\x09"))
        self.assertIsNone(sprites.delta(2, 2, b"\x01\x00\x03\x04", b"\x01\x02\x03\x04"))  # an erase
        # a 3-frame group: frame 1 differs by one pixel (delta over frame 0), frame 2 repeats frame 0 (shares it)
        big = [[WAL] * 6 for _ in range(6)]
        f1 = [row[:] for row in big]
        f1[2][3] = RED
        p_sheet(self.d / "a.png", 24, 8, [((1, 1), big), ((8, 1), f1), ((15, 1), big)])
        sprites.write_palette(PAL, NAMES, self.d)
        atlas = {"version": 1, "palette": "palette.png", "canvas": {"w": 120, "h": 106}, "sheets": {"a": {
            "file": "a.png", "sprites": [sp(f"sq_{i}", 1 + 7 * i, 1, 6, 6, 0, 0, "sq", i) for i in range(3)]}}}
        (self.d / "atlas.json").write_text(json.dumps(atlas))
        rc, out, err = run("build", "--atlas", str(self.d / "atlas.json"), "--out", str(self.gen))
        self.assertEqual(rc, 0, err)
        self.assertIn("1 frames as deltas over frame 0, 1 sharing another's data", out)
        cpp = (self.gen / "sprites_gen.cpp").read_text()
        self.assertIn("const Sprite sq_1 = {6, 6, 0, 0, 3, 2, 1, 1, 2, &sq_0, sq_1_rle};   // 1x1 over sq_0", cpp)
        self.assertIn("const Sprite sq_2 = {6, 6, 0, 0, 0, 0, 6, 6, 2, nullptr, sq_0_rle};   // same pixels as sq_0", cpp)
        self.assertEqual(decoded(cpp, "sq_1", 6, 6), sum(f1, []))
        self.assertEqual(decoded(cpp, "sq_2", 6, 6), sum(big, []))

    def test_deterministic_and_unchanged(self):
        self.build()
        first = (self.gen / "sprites_gen.cpp").read_text()
        rc, out, _ = self.build()
        self.assertEqual(rc, 0)
        self.assertIn("(unchanged)", out)
        self.assertEqual(first, (self.gen / "sprites_gen.cpp").read_text())

    def test_unknown_colour_names_sprite_and_pixel(self):
        bad = [row[:] for row in DOT]
        bad[2][1] = (1, 2, 3)
        rc, _, err = self.build(fx_dot=bad)
        self.assertEqual(rc, 1)
        self.assertIn("sprite fx_dot: colour #010203 at sheet pixel (2,3) (sprite pixel (1,2))", err)
        self.assertFalse((self.gen / "sprites_gen.h").exists())

    def test_ambiguous_rgb_colour_fails(self):
        bad = [row[:] for row in DOT]
        bad[0][0] = IDX["WHT"]
        rc, _, err = self.build(fx_dot=bad)
        self.assertEqual(rc, 1)
        self.assertRegex(err, r"#ffffff at sheet pixel \(1,1\).*several palette entries \[\d+, \d+, \d+\]")

    def test_bad_names(self):
        def dup(a):
            a["sheets"]["fx"]["sprites"][0]["name"] = "desk_box"
        def invalid(a):
            a["sheets"]["fx"]["sprites"][0]["name"] = "Fx-Dot"
        def digit(a):
            a["sheets"]["fx"]["sprites"][0]["name"] = "1dot"
        def clash_group(a):
            a["sheets"]["fx"]["sprites"][0]["name"] = "clawd_walk_n"
        for fn, msg in ((dup, "`desk_box` is both"), (invalid, "'Fx-Dot' is not a C identifier"),
                        (digit, "'1dot' is not a C identifier"), (clash_group, "`clawd_walk_n` is both")):
            with self.subTest(fn.__name__):
                rc, _, err = self.build(extra=fn)
                self.assertEqual(rc, 1)
                self.assertIn(msg, err)

    def test_rect_outside_sheet_and_frame_gap(self):
        def outside(a):
            a["sheets"]["fx"]["sprites"][0]["x"] = 6
        def gap(a):
            a["sheets"]["clawd"]["sprites"][0]["frame"] = 2
        def anchor(a):
            a["sheets"]["fx"]["sprites"][0]["ax"] = 200
        for fn, msg in ((outside, "outside the 8x8 sheet"), (gap, "frames must be 0..1"),
                        (anchor, "`ax` must be an integer in -128..127")):
            with self.subTest(fn.__name__):
                rc, _, err = self.build(extra=fn)
                self.assertEqual(rc, 1)
                self.assertIn(msg, err)


class Check(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        self.atlas = make_atlas(self.d)

    def tearDown(self):
        self.tmp.cleanup()

    def test_good_atlas_passes(self):
        rc, out, err = run("check", "--atlas", str(self.atlas))
        self.assertEqual(rc, 0, err)
        self.assertIn("check: OK, 4 sprites on 2 sheets, 1 groups", out)

    def test_index_out_of_range(self):
        im = load(self.d / "clawd.png")
        im.putpixel((2, 2), N)                      # inside clawd_walk_0
        im.save(self.d / "clawd.png", transparency=0)
        rc, _, err = run("check", "--atlas", str(self.atlas))
        self.assertEqual(rc, 1)
        self.assertIn(f"sprite clawd_walk_0: index {N} >= N_COL {N} at sheet pixel (2,2)", err)

    def test_wrong_png_palette_entry(self):
        im = load(self.d / "clawd.png")
        pal = im.getpalette()
        pal[C * 3:C * 3 + 3] = [1, 2, 3]
        im.putpalette(pal)
        im.save(self.d / "clawd.png", transparency=0)
        rc, _, err = run("check", "--atlas", str(self.atlas))
        self.assertEqual(rc, 1)
        self.assertIn(f"PNG palette entry {C} is #010203", err)

    def test_rgb_round_trip_catches_opaque_index0(self):
        im = load(self.d / "clawd.png")
        pal = im.getpalette()
        pal[0:3] = [0, 0, 0]                        # index 0 opaque black reads back as SHD in RGB
        im.putpalette(pal)
        im.info.pop("transparency", None)            # Pillow would otherwise carry the tRNS over
        im.save(self.d / "clawd.png")
        rc, _, err = run("check", "--atlas", str(self.atlas))
        self.assertEqual(rc, 1)
        self.assertIn("RGB round trip changes sprite pixel", err)

    def test_palette_png_differs_from_room_cpp(self):
        im = load(self.d / "palette.png")
        im.putpixel((OUT, 0), (9, 9, 9))
        im.save(self.d / "palette.png")
        rc, _, err = run("check", "--atlas", str(self.atlas))
        self.assertEqual(rc, 1)
        self.assertIn(f"differs from BASE", err)

    def test_stale_generated_files(self):
        gen = self.d / "gen"
        self.assertEqual(run("build", "--atlas", str(self.atlas), "--out", str(gen))[0], 0)
        self.assertEqual(run("check", "--atlas", str(self.atlas), "--gen", str(gen))[0], 0)
        a = json.loads(self.atlas.read_text())
        a["sheets"]["fx"]["sprites"][0]["ay"] = 2
        self.atlas.write_text(json.dumps(a))
        rc, _, err = run("check", "--atlas", str(self.atlas), "--gen", str(gen))
        self.assertEqual(rc, 1)
        self.assertIn("stale", err)

    def test_preview(self):
        rc, out, err = run("preview", "--atlas", str(self.atlas), "--out", str(self.d / "pv"))
        self.assertEqual(rc, 0, err)
        self.assertEqual(load(self.d / "pv" / "fx@4x.png").size, (32, 32))


class RealAtlas(unittest.TestCase):
    """The committed sheets are hand-edited masters that the firmware build converts: they must stay valid."""
    ATLAS = ROOT / "firmware" / "assets" / "sprites" / "atlas.json"

    def test_check_passes(self):
        rc, out, err = run("check", "--atlas", str(self.ATLAS))
        self.assertEqual(rc, 0, err)
        self.assertIn("check: OK", out)

    def test_build_declares_every_group(self):
        a = json.loads(self.ATLAS.read_text())
        groups = collections.Counter(sp["group"] for sh in a["sheets"].values() for sp in sh["sprites"]
                                     if "group" in sp)
        with tempfile.TemporaryDirectory() as d:
            rc, _, err = run("build", "--atlas", str(self.ATLAS), "--out", d)
            self.assertEqual(rc, 0, err)
            h = (Path(d) / "sprites_gen.h").read_text()
        declared = {g: int(n) for g, n in re.findall(r"extern const Sprite \*const (\w+)\[(\d+)\];", h)}
        self.assertEqual(declared, dict(groups))
        for g, n in groups.items():
            self.assertIn(f"constexpr int {g}_n = {n};", h)


if __name__ == "__main__":
    unittest.main()
