#!/usr/bin/env python3
"""Pixel QA for the isometric home animations: JS prototype vs firmware, on the real device.

  host/.venv/bin/python tools/iso_qa.py 03-waiting [--frames 0,5,10] [--helpers N] [--grey] [--out DIR]

For each frame: renders iso-clawd/anims/NN-name.html headless (chromium, zoom 1 = logical 240x121), forces the
same animation/frame on the device ({"t":"cmd","anim":...}), grabs the framebuffer, cuts the scene (y 54..416),
downsamples it 3x back to logical pixels (columns 40..199) and compares. Prints the mismatch count per frame and
writes <out>/<anim>_f<N>.png = [JS | device | diff] side by side. Exit code 1 if any frame differs.
Needs pillow and chromium. Takes the device lock (the daemon steps aside)."""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "host"))
sys.path.insert(0, str(ROOT / "tools"))

from PIL import Image, ImageChops  # noqa: E402

import dev  # noqa: E402  (device_lock, open_link, write_png)

VIEW_X0, VIEW_W, ZOOM, SCENE_Y = 40, 160, 3, 54


def render_js(anim: str, frame: int, out: Path, helpers: int = 0) -> Image.Image:
    page = ROOT / "iso-clawd" / "anims" / f"{anim}.html"
    url = f"file://{page}?embed=1&frame={frame}&zoom=1&play=0&n={helpers}"   # n: helper count (17-delegating)
    subprocess.run(["chromium", "--headless=new", "--no-sandbox", "--disable-gpu", "--hide-scrollbars",
                    "--force-device-scale-factor=1", "--window-size=240,121", f"--screenshot={out}", url],
                   check=True, capture_output=True, timeout=60)
    im = Image.open(out).convert("RGB")
    return im.crop((VIEW_X0, 0, VIEW_X0 + VIEW_W, 121))


def grab_device(link, anim: str, frame: int, helpers: int, grey: bool, out: Path) -> Image.Image:
    link.send({"t": "cmd", "nav": "home"})
    link.send({"t": "cmd", "anim": anim, "frame": frame, "helpers": helpers, "grey": 1 if grey else 0})
    time.sleep(0.6)
    w, h, data, rot = link.screenshot()
    tmp = out / "_device_shot.png"
    dev.write_png(tmp, w, h, data, rot)
    full = Image.open(tmp).convert("RGB")
    scene = full.crop((0, SCENE_Y, VIEW_W * ZOOM, SCENE_Y + 121 * ZOOM))
    # nearest 3x downsample: sample the centre of each 3x3 block
    small = Image.new("RGB", (VIEW_W, 121))
    px = scene.load()
    sp = small.load()
    for y in range(121):
        for x in range(VIEW_W):
            sp[x, y] = px[x * ZOOM + 1, y * ZOOM + 1]
    return small


def q565(im: Image.Image) -> Image.Image:
    """Quantise an RGB image the way the device does (RGB565 -> expanded back), so colours compare equal."""
    out = Image.new("RGB", im.size)
    src, dst = im.load(), out.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b = src[x, y]
            r, g, b = r >> 3, g >> 2, b >> 3
            dst[x, y] = ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("anim", help="e.g. 03-waiting")
    ap.add_argument("--frames", default="0,8,16", help="list and/or ranges: 0,8,16 or 0-39")
    ap.add_argument("--helpers", type=int, default=0)
    ap.add_argument("--grey", action="store_true")
    ap.add_argument("--out", default=str(ROOT / "iso-clawd" / "shots" / "qa"))
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    frames = []
    for part in a.frames.split(","):          # "0,5,10" or "0-39"
        lo, _, hi = part.partition("-")
        frames += list(range(int(lo), int(hi) + 1)) if hi else [int(lo)]
    bad = 0
    with dev.device_lock("iso_qa"):
        link = dev.open_link()
        # hold the ambient dimming / night screensaver off while we drive the panel (the daemon is stepped aside,
        # so the device would otherwise go "host offline" -> saver mid-sweep); 0 restores the settings
        link.send({"t": "event", "kind": "dev_ambient", "saver": 0, "saver_s": 65535, "dim_s": 65535})
        try:
            run(a, frames, out, link)
        finally:
            link.send({"t": "cmd", "anim": "", "mirror": -1})   # always release the device's QA override
            link.send({"t": "event", "kind": "dev_ambient", "saver_s": 0, "dim_s": 0})
            link.close()
    return 1 if BAD[0] else 0


BAD = [0]


def run(a, frames, out: Path, link) -> None:
    bad = 0
    if True:
        for f in frames:
            js = q565(render_js(a.anim, f, out / f"_js_{a.anim}_f{f}.png", a.helpers))
            de = grab_device(link, a.anim, f, a.helpers, a.grey, out)
            diff = ImageChops.difference(js, de)
            # the counters row starts at logical row ~118: ignore the last rows
            diff.paste((0, 0, 0), (0, 117, VIEW_W, 121))
            n = sum(1 for p in list(diff.getdata()) if p != (0, 0, 0))
            bad += n
            mark = Image.new("RGB", js.size, (26, 25, 24))
            mp, dp = mark.load(), diff.load()
            for y in range(js.height):
                for x in range(js.width):
                    if dp[x, y] != (0, 0, 0): mp[x, y] = (229, 83, 75)
            sheet = Image.new("RGB", (VIEW_W * 3 + 8, 121), (60, 58, 54))
            sheet.paste(js, (0, 0)); sheet.paste(de, (VIEW_W + 4, 0)); sheet.paste(mark, (2 * VIEW_W + 8, 0))
            sheet = sheet.resize((sheet.width * 3, sheet.height * 3), Image.NEAREST)
            sheet.save(out / f"{a.anim}_f{f}.png")
            bb = f" in x {diff.getbbox()[0] + VIEW_X0}..{diff.getbbox()[2] - 1 + VIEW_X0} y {diff.getbbox()[1]}..{diff.getbbox()[3] - 1}" if n else ""
            print(f"{a.anim} frame {f}: {n} differing pixels{bb}")
    BAD[0] = bad


if __name__ == "__main__":
    sys.exit(main())
