#!/usr/bin/env python3
"""Renders every sprite of Clawd's Studio from the firmware's draw code into DOS-style sprite sheets.

  host/.venv/bin/python tools/sprites/make_sheets.py [--out firmware/assets/sprites] [--build mocks/sprites_build]
                                                     [--preview DIR]

Builds tools/sprites/sheet.cpp with g++ against firmware/src/screens/room/room.cpp, runs it, and writes
<sheet>.png (indexed, palette = room::BASE, index 0 transparent), atlas.json, palette.png and palette.gpl.
--preview DIR also writes labelled 4x views of each sheet there (for looking at, never next to the assets:
only the indexed sheets are the art). The format is tools/sprites/FORMAT.md. Output is deterministic: running
it twice gives identical files. Needs pillow."""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import OrderedDict
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent.parent
SRC = ROOT / "firmware" / "src" / "screens" / "room"
HERE = Path(__file__).resolve().parent
W, H = 120, 106
KEY = (255, 0, 255)     # index 0 (T) in the PNG palette: the transparent key
SHEETS = ["room", "furniture", "window", "clawd", "minis", "props", "fx"]   # sheet order in atlas.json
MAX_COLS = 16          # frames per sheet row; longer groups wrap, so one 48-frame screen loop does not set the width
ZOOM = 4


def build(build_dir: Path) -> Path:
    """Compile sheet.cpp + room.cpp, reusing the exe while it is newer than every source."""
    build_dir.mkdir(parents=True, exist_ok=True)
    exe = build_dir / "sheet"
    srcs = [HERE / "sheet.cpp", SRC / "room.cpp"]
    newest = max(p.stat().st_mtime for p in srcs + [SRC / "room.h"])
    if exe.exists() and exe.stat().st_mtime > newest:
        return exe
    cmd = ["g++", "-O2", "-std=c++17", "-Wall", "-Wno-misleading-indentation", f"-I{SRC}", *map(str, srcs), "-o", str(exe)]
    subprocess.run(cmd, check=True)
    return exe


def colour_names() -> list[str]:
    """The palette enum of room.h in index order (T, OUT, WAL, ... up to N_COL)."""
    text = (SRC / "room.h").read_text()
    body = re.search(r"enum\s*:\s*Idx\s*\{(.*?)N_COL", text, re.S).group(1)
    body = re.sub(r"//[^\n]*", "", body)
    return [re.sub(r"=.*", "", n).strip() for n in body.split(",") if n.strip()]


def load_cells(cells_dir: Path) -> tuple[list[dict], list[tuple[int, int, int]]]:
    raw = (cells_dir / "cells.bin").read_bytes()
    cells = []
    for line in (cells_dir / "cells.jsonl").read_text().splitlines():
        c = json.loads(line)
        c["px"] = raw[c["off"]:c["off"] + c["w"] * c["h"]]
        cells.append(c)
    pal = (cells_dir / "palette.bin").read_bytes()
    base = [tuple(pal[i:i + 3]) for i in range(0, len(pal), 3)]
    return cells, base


def png_palette(base: list[tuple[int, int, int]]) -> list[int]:
    flat = [*KEY]
    for rgb in base[1:]:
        flat += rgb
    return flat


def compose(cells: list[dict], base) -> tuple[Image.Image, dict, list[str]]:
    """Lay one sheet out: a row per group (first-seen order), frames left to right, 1 px gutters. Each row has
    its own cell size (the group's widest and tallest frame), so a 22 x 34 quest bang does not pad every 13 x 11
    emote; within a row the cells stay uniform so an animation's frames line up for editing."""
    groups: OrderedDict[str, list[dict]] = OrderedDict()
    for c in cells:
        groups.setdefault(c["group"], []).append(c)
    rows = []   # one entry per sheet row; a group longer than MAX_COLS continues on the next row(s)
    y = 1
    for name, frames in groups.items():
        frames.sort(key=lambda c: c["frame"])
        if [c["frame"] for c in frames] != list(range(len(frames))):
            sys.exit(f"error: group {name} frames are not 0..n-1")
        cw, ch = max(c["w"] for c in frames), max(c["h"] for c in frames)
        for first in range(0, len(frames), MAX_COLS):
            n = min(MAX_COLS, len(frames) - first)
            rows.append({"group": name, "first": first, "n": n, "y": y, "cw": cw, "ch": ch})
            y += ch + 1
    width = 1 + max(r["n"] * (r["cw"] + 1) for r in rows)
    im = Image.new("P", (width, y), 0)
    im.putpalette(png_palette(base), rawmode="RGB")
    sprites = []
    for r in rows:
        for col in range(r["n"]):
            c = groups[r["group"]][r["first"] + col]
            x = 1 + col * (r["cw"] + 1)
            im.paste(Image.frombytes("P", (c["w"], c["h"]), c["px"]), (x, r["y"]))
            sprites.append({"name": f"{r['group']}_{c['frame']}", "x": x, "y": r["y"], "w": c["w"], "h": c["h"], "ax": c["ax"],
                            "ay": c["ay"], "group": r["group"], "frame": c["frame"], "tags": c["tags"]})
    return im, {"rows": rows, "sprites": sprites}, list(groups)


def preview(im: Image.Image, info: dict, groups: list[str], base) -> Image.Image:
    """4x RGB view: checkerboard under transparency, dark gutters, group names on the left, frame numbers."""
    font = ImageFont.load_default()
    rows = info["rows"]
    margin = 8 + max(int(font.getlength(g)) for g in groups)
    big = im.convert("RGB").resize((im.width * ZOOM, im.height * ZOOM), Image.NEAREST)
    mask = im.point(lambda v: 255 if v == 0 else 0, "L").resize(big.size, Image.NEAREST)
    checker = Image.new("RGB", big.size, (205, 205, 205))
    d = ImageDraw.Draw(checker)
    for y in range(0, big.height, 8):
        for x in range((y // 8) % 2 * 8, big.width, 16):
            d.rectangle([x, y, x + 7, y + 7], fill=(235, 235, 235))
    big.paste(checker, (0, 0), mask)
    out = Image.new("RGB", (big.width + margin, big.height), (40, 40, 48))
    out.paste(big, (margin, 0))
    d = ImageDraw.Draw(out)
    # gutters
    for r in rows:
        y0, y1 = (r["y"] - 1) * ZOOM, (r["y"] + r["ch"]) * ZOOM
        d.rectangle([margin, y0, out.width, y0 + ZOOM - 1], fill=(40, 40, 48))
        d.rectangle([margin, y1, out.width, y1 + ZOOM - 1], fill=(40, 40, 48))
        for col in range(r["n"] + 1):
            gx = margin + col * (r["cw"] + 1) * ZOOM
            d.rectangle([gx, y0, gx + ZOOM - 1, y1 + ZOOM - 1], fill=(40, 40, 48))
        d.rectangle([margin + (1 + r["n"] * (r["cw"] + 1)) * ZOOM, y0, out.width, y1 + ZOOM - 1], fill=(40, 40, 48))
        d.text((4, r["y"] * ZOOM + 2), r["group"] if r["first"] == 0 else f"  … {r['first']}", fill=(240, 240, 240), font=font)
    cells = {r["group"]: (r["cw"], r["ch"]) for r in rows}
    for s in info["sprites"]:
        x, y = margin + s["x"] * ZOOM, s["y"] * ZOOM
        d.rectangle([x, y, x + s["w"] * ZOOM - 1, y + s["h"] * ZOOM - 1], outline=(0, 160, 255))
        # frame number in the cell's bottom-right corner, where sprites (cropped top-left) rarely reach
        label = str(s["frame"])
        cw, ch = cells[s["group"]]
        lx, ly = x + cw * ZOOM - int(font.getlength(label)) - 3, y + ch * ZOOM - 12
        d.rectangle([lx - 1, ly, x + cw * ZOOM - 1, y + ch * ZOOM - 1], fill=(40, 40, 48))
        d.text((lx, ly), label, fill=(255, 220, 90), font=font)
    return out


def write_atlas(path: Path, sheets: dict) -> None:
    """atlas.json with one sprite per line, so diffs stay readable."""
    lines = ["{", '  "version": 1,', '  "palette": "palette.png",', f'  "canvas": {{"w": {W}, "h": {H}}},', '  "sheets": {']
    for i, (name, s) in enumerate(sheets.items()):
        lines.append(f'    "{name}": {{')
        lines.append(f'      "file": "{name}.png",')
        lines.append('      "rows": [' + ", ".join(json.dumps(r) for r in s["rows"]) + "],")
        lines.append('      "sprites": [')
        sp = [f"        {json.dumps(x)}" for x in s["sprites"]]
        lines.append(",\n".join(sp))
        lines.append("      ]")
        lines.append("    }" + ("," if i < len(sheets) - 1 else ""))
    lines += ["  }", "}"]
    path.write_text("\n".join(lines) + "\n")


def write_palette(out: Path, base, names: list[str]) -> None:
    n = len(base)
    im = Image.new("RGB", (n, 1))
    im.putdata([KEY] + base[1:])
    im.save(out / "palette.png")
    rows = ["GIMP Palette", "Name: Clawd's Studio (room::BASE)", "Columns: 16", "#"]
    for i, rgb in enumerate([KEY] + base[1:]):
        rows.append(f"{rgb[0]:3d} {rgb[1]:3d} {rgb[2]:3d}\t{i} {names[i]}")
    (out / "palette.gpl").write_text("\n".join(rows) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=ROOT / "firmware" / "assets" / "sprites")
    ap.add_argument("--build", type=Path, default=ROOT / "mocks" / "sprites_build")
    ap.add_argument("--preview", type=Path, default=None, help="also write <sheet>@4x.png views into this directory")
    a = ap.parse_args()
    exe = build(a.build)
    cells_dir = a.build / "cells"
    cells_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(exe), "--out", str(cells_dir)], check=True)
    cells, base = load_cells(cells_dir)
    names = colour_names()
    if len(names) != len(base):
        sys.exit(f"error: room.h names {len(names)} colours, BASE has {len(base)}")
    seen = set()
    for c in cells:
        n = f"{c['group']}_{c['frame']}"
        if not re.fullmatch(r"[a-z][a-z0-9_]*", n) or n in seen:
            sys.exit(f"error: bad or duplicate sprite name {n}")
        seen.add(n)
    a.out.mkdir(parents=True, exist_ok=True)
    write_palette(a.out, base, names)
    atlas = {}
    total = 0
    for sh in SHEETS:
        sc = [c for c in cells if c["sheet"] == sh]
        im, info, groups = compose(sc, base)
        im.save(a.out / f"{sh}.png", transparency=0, optimize=False)
        if a.preview:
            a.preview.mkdir(parents=True, exist_ok=True)
            preview(im, info, groups, base).save(a.preview / f"{sh}@4x.png")
        atlas[sh] = info
        total += len(sc)
        opaque = sum(1 for v in im.getdata() if v)
        print(f"{sh:10s} {len(sc):4d} sprites  {len(groups):3d} groups  sheet {im.width}x{im.height}  "
              f"{100 * opaque // (im.width * im.height)}% opaque")
    if total != len(cells):
        sys.exit("error: cells on a sheet not listed in SHEETS")
    write_atlas(a.out / "atlas.json", atlas)
    print(f"{total} sprites -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
