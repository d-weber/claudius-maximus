#!/usr/bin/env python3
"""Per-item review for the square office: crops ONE scene item from the reference mock and from the live render,
side by side at 2x, so a reviewer can iterate on lib/room/<item>.js until the two match.

  host/.venv/bin/python tools/room_item.py shelf [--light day] [--page 00-room-sheet] [--out mocks/items]

Writes <out>/<item>_<light>.png = [reference | render] of the item's region (device px, 480x480 frame), and prints it.
Items and regions are in REGIONS (x0, y0, x1, y1 in device px); 'full' is the whole frame. Needs chromium + pillow."""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
REGIONS = {
    "shelf":   (0, 60, 230, 300),
    "lamp":    (0, 200, 130, 350),
    "monitor": (50, 220, 200, 390),
    "clawd":   (130, 180, 330, 400),
    "desk":    (0, 270, 480, 440),
    "window":  (240, 30, 480, 330),
    "walls":   (0, 0, 480, 430),
    "light":   (0, 0, 480, 480),
    "helper":  (290, 270, 470, 400),
    "chair":   (240, 170, 380, 340),
    "full":    (0, 0, 480, 480),
}


def render(page: str, light: str, out: Path) -> Image.Image:
    url = f"file://{ROOT}/iso-clawd/anims/{page}.html?embed=1&frame=3&zoom=3&play=0&light={light}"
    subprocess.run(["chromium", "--headless=new", "--no-sandbox", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                    "--window-size=480,480", f"--screenshot={out}", url], check=True, capture_output=True, timeout=90)
    return Image.open(out).convert("RGB")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("item", choices=REGIONS)
    ap.add_argument("--light", default="day")
    ap.add_argument("--page", default="00-room-sheet")
    ap.add_argument("--out", type=Path, default=ROOT / "mocks" / "items")
    ap.add_argument("--zoom", type=int, default=2)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    ref = Image.open(ROOT / "mocks" / "r_final" / f"scene_{a.light}.png").convert("RGB")
    live = render(a.page, a.light, a.out / f"_render_{a.item}_{a.light}.png")
    x0, y0, x1, y1 = REGIONS[a.item]
    w, h = (x1 - x0) * a.zoom, (y1 - y0) * a.zoom
    sheet = Image.new("RGB", (w * 2 + 8, h + 20), (0, 0, 0))
    d = ImageDraw.Draw(sheet)
    for i, (im, label) in enumerate(((ref, "REFERENCE"), (live, "RENDER"))):
        crop = im.crop((x0, y0, x1, y1)).resize((w, h), Image.NEAREST)
        sheet.paste(crop, (i * (w + 8), 20))
        d.text((i * (w + 8) + 4, 4), f"{label}  {a.item} {a.light}  region {x0},{y0}-{x1},{y1}", fill=(255, 255, 0))
    out = a.out / f"{a.item}_{a.light}.png"
    sheet.save(out)
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
