#!/usr/bin/env python3
"""Write the application icons packaging/icons/app.{png,ico,icns} from companion.tray.icon.render_app_icon.

  host/.venv/bin/python packaging/make_icons.py [--out DIR]

app.png is 1024 px (Linux .desktop/AppImage), app.ico holds 16..256 px (Windows exe + installer), app.icns the
macOS sizes (Pillow's ICNS writer works on every OS, so CI can build it on Linux too). The icons are small and
committed; re-run this when the icon design changes. CI re-runs it before PyInstaller so a release always ships
the current render.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "host"))

ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
ICNS_SIZES = [16, 32, 64, 128, 256, 512, 1024]


def _placeholder_icon(size: int) -> Image.Image:
    """Stand-in for render_app_icon: orange rounded square with two eyes (Clawd's face)."""
    s = 4  # supersample for smooth edges at small sizes
    n = size * s
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = round(n * 0.06)
    d.rounded_rectangle([pad, pad, n - pad, n - pad], radius=round(n * 0.22), fill=(217, 119, 87, 255))
    ew, eh = round(n * 0.11), round(n * 0.22)
    for cx in (round(n * 0.34), round(n * 0.66)):
        top = round(n * 0.36)
        d.rounded_rectangle([cx - ew // 2, top, cx + ew // 2, top + eh], radius=ew // 2, fill=(26, 20, 18, 255))
    return img.resize((size, size), Image.LANCZOS)


def renderer():
    """companion.tray.icon.render_app_icon when the tray module provides it, else the inline placeholder."""
    try:
        from companion.tray.icon import render_app_icon
        return render_app_icon, "companion.tray.icon"
    except ImportError:
        # TODO: drop this fallback once companion/tray/icon.py (tray agent) ships render_app_icon.
        return _placeholder_icon, "placeholder"


def render(fn, size: int) -> Image.Image:
    img = fn(size).convert("RGBA")
    if img.size != (size, size):
        img = img.resize((size, size), Image.LANCZOS)
    return img


def make_icons(out: Path) -> list[Path]:
    fn, source = renderer()
    out.mkdir(parents=True, exist_ok=True)
    big = render(fn, 1024)
    png, ico, icns = out / "app.png", out / "app.ico", out / "app.icns"
    big.save(png, optimize=True)
    # Each size rendered natively (the face must stay readable at 16 px), not just downscaled from 1024.
    ico_imgs = [render(fn, s) for s in ICO_SIZES]
    ico_imgs[-1].save(ico, format="ICO", sizes=[(s, s) for s in ICO_SIZES], append_images=ico_imgs[:-1])
    big.save(icns, format="ICNS", append_images=[render(fn, s) for s in ICNS_SIZES[:-1]])
    print(f"icons from {source} -> {out}")
    return [png, ico, icns]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=Path(__file__).resolve().parent / "icons")
    make_icons(ap.parse_args(argv).out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
