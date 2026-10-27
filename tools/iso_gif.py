#!/usr/bin/env python3
"""Review GIFs for the room animations: renders a page's ?grid= canvas headless and slices it into frames.

  host/.venv/bin/python tools/iso_gif.py 20-room-idle [--light day] [--room office] [--chrome] [--n 3] [--scale 2] [--out DIR]

Writes <out>/<anim>_<light>.gif (and the grid PNG next to it). Needs chromium and pillow."""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
W = H = 160


def chrome(im: Image.Image, clock: str) -> Image.Image:
    """The LVGL layer over the scene at device resolution: real font, real alpha (not pixel art)."""
    from PIL import ImageDraw, ImageFont
    F = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"; FB = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
    f = lambda s, b=False: ImageFont.truetype(FB if b else F, s)
    C = dict(bg=(0x1A, 0x19, 0x18), text=(0xEC, 0xE9, 0xE4), muted=(0x9C, 0x97, 0x8E), dim=(0x5E, 0x5A, 0x54), orange=(0xD9, 0x77, 0x57),
             violet=(0xB3, 0x92, 0xF0), green=(0x7F, 0xB0, 0x69), surface2=(0x33, 0x31, 0x2E))
    im = im.convert("RGBA")
    ov = Image.new("RGBA", im.size, (0, 0, 0, 0)); od = ImageDraw.Draw(ov)
    od.rectangle((0, 400, im.width, im.height), fill=C["bg"] + (200,))
    im = Image.alpha_composite(im, ov).convert("RGB"); d = ImageDraw.Draw(im)
    d.text((16, 8), clock, font=f(26, True), fill=C["text"]); d.text((17, 40), "Sun 18 Oct", font=f(13), fill=C["muted"])
    for y, lab, frac, col in ((14, "5H", 0.42, C["green"]), (30, "7D", 0.18, C["violet"])):
        x1 = 464; x0 = x1 - 96; d.text((x0 - 8, y - 2), lab, font=f(12), fill=C["muted"], anchor="ra")
        d.rounded_rectangle((x0, y, x1, y + 7), 3, fill=C["surface2"]); d.rounded_rectangle((x0, y, x0 + int(96 * frac), y + 7), 3, fill=col)
    for i, (v, l, col) in enumerate([("4", "working", C["orange"]), ("0", "need you", C["dim"]), ("0", "idle", C["dim"]), ("2", "helpers", C["violet"])]):
        x = 72 + i * 112; d.text((x, 410), v, font=f(26, True), fill=col, anchor="ma"); d.text((x, 441), l, font=f(12), fill=C["dim"] if v == "0" else C["muted"], anchor="ma")
    for i in range(7): d.ellipse((216 + i * 8 - 2, 470, 216 + i * 8 + 2, 474), fill=C["orange"] if i == 0 else C["dim"])
    return im


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("anim")
    ap.add_argument("--light", default="day")
    ap.add_argument("--room", default="office")
    ap.add_argument("--chrome", action="store_true")
    ap.add_argument("--n", type=int, help="helper count (delegating)")
    ap.add_argument("--cols", type=int, default=12)
    ap.add_argument("--scale", type=int, default=3, help="GIF scale (nearest)")
    ap.add_argument("--out", type=Path, default=ROOT / "mocks" / "gif")
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    page = ROOT / "iso-clawd" / "anims" / f"{a.anim}.html"
    src = page.read_text()
    m = re.search(r"fps:\s*(\d+)", src)
    fps = int(m.group(1)) if m else 10
    url = f"file://{page}?grid={a.cols}&light={a.light}&room={a.room}" + (f"&n={a.n}" if a.n else "")
    grid_png = a.out / f"{a.anim}_{a.light}_grid.png"
    # the grid is at most cols*240 wide; rows unknown -> tall window, cropped afterwards
    subprocess.run(["chromium", "--headless=new", "--no-sandbox", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                    f"--window-size={a.cols * W},{H * 20}", f"--screenshot={grid_png}", url], check=True, capture_output=True, timeout=120)
    im = Image.open(grid_png).convert("RGB")
    # frame count: cells that are not entirely black (unused grid cells and the page background are #000)
    frames = []
    for r in range(im.height // H):
        for c in range(a.cols):
            cell = im.crop((c * W, r * H, (c + 1) * W, (r + 1) * H))
            if cell.getbbox() is None:
                break
            frames.append(cell)
    if not frames:
        sys.exit("no frames found (page error?)")
    if a.scale != 1:
        frames = [f.resize((W * a.scale, H * a.scale), Image.NEAREST) for f in frames]
    if a.chrome:
        clock = {"morning": "07:40", "day": "12:15", "sunset": "19:05", "dusk": "20:40", "night": "00:12"}.get(a.light, "12:15")
        frames = [chrome(f, clock) for f in frames]
    pal = [f.quantize(colors=128, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE) for f in frames]
    gif = a.out / f"{a.anim}_{a.light}.gif"
    pal[0].save(gif, save_all=True, append_images=pal[1:], duration=round(1000 / fps), loop=0, optimize=False)
    print(f"{gif}  {len(frames)} frames @ {fps} fps")
    return 0


if __name__ == "__main__":
    sys.exit(main())
