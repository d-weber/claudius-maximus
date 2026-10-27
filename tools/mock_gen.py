#!/usr/bin/env python3
"""Generate home-screen design mock-ups with Replicate image models and preview them on the device.

The output is for direction, not final pixels: models ignore the 2:1 iso grid, so every result is
down-scaled to the logical stage, snapped to the iso palette and up-scaled nearest-neighbour so it
reads like the real thing on the panel.

Examples (run with host/.venv/bin/python):
  tools/mock_gen.py "warmer desk lamp light, a plant and a second monitor" --ref docs/img/home.png -n 4 --show
  tools/mock_gen.py "isometric pixel art desk companion, orange blob mascot" --model recraft -n 2
  tools/mock_gen.py --from-images mocks/2026-10-18_1612/raw_*.png --mode full --show   # re-process, no API

Run via `zsh -ic '...'` if the token only lives in ~/.zshrc.
Token: REPLICATE_API_TOKEN, or REPLICATE_TOKEN_FILE, or ~/.config/replicate/{token,api_token,config.json}.
It is only ever sent to api.replicate.com.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
from pathlib import Path

import requests
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
API = "https://api.replicate.com/v1"
BG = (0x1A, 0x19, 0x18)

# iso-clawd/lib/iso.js  Iso.C + the extra SHADE tones. Keep in sync by hand.
PALETTE_HEX = """
1A1918 262523 33312E 3D3A36 ECE9E4 9C978E 5E5A54
D97757 F0A07E B4603F F4A592
F2C14E C99A2E 7FB069 5E8C4C E5534B B23A33
B392F0 CDB5F6 8E6CC9 6A9BCC A9C8E6 46719C 77716B 948E86
6B5140 523D30 3F2F25 80624E 4C392D 62493A
ECE9E4 C9C2B6 1E2A36 121110 A3CB8F F7D885 EE8680
""".split()
PALETTE = [tuple(int(h[i:i + 2], 16) for i in (0, 2, 4)) for h in PALETTE_HEX]

# logical size, scale, y offset of the scene on the 480x480 screen
MODES = {
    "stage": (160, 121, 3, 54),     # firmware home_face (screen_home.cpp: home_face_create(p, 54, 363))
    "stage240": (240, 121, 2, 54),  # iso-clawd browser pages
    "full": (240, 240, 2, 0),       # whole screen at 2x pixels
    "full1x": (480, 480, 1, 0),     # whole screen, no pixel look
}

MODELS = {
    # name: (owner/model, needs reference image, how to build the input)
    "nano-banana": ("google/nano-banana", False,
                    lambda p, ref: {"prompt": p, "output_format": "png", **({"image_input": ref} if ref else {})}),
    "kontext": ("black-forest-labs/flux-kontext-pro", True,
                lambda p, ref: {"prompt": p, "input_image": ref[0], "aspect_ratio": "match_input_image",
                                "output_format": "png", "safety_tolerance": 2}),
    "recraft": ("recraft-ai/recraft-v3", False,
                lambda p, ref: {"prompt": p, "size": "1024x1024", "style": "digital_illustration/pixel_art"}),
    "flux": ("black-forest-labs/flux-dev", False,
             lambda p, ref: {"prompt": p, "aspect_ratio": "1:1", "output_format": "png", "num_outputs": 1,
                             "go_fast": True}),
    "schnell": ("black-forest-labs/flux-schnell", False,
                lambda p, ref: {"prompt": p, "aspect_ratio": "1:1", "output_format": "png", "num_outputs": 1}),
}

STYLE_HINT = (" Style: 2:1 isometric pixel art, crisp pixels, no anti-aliasing, dark charcoal background #1A1918,"
              " limited palette with orange #D97757 mascot, dark walnut desk, soft amber and violet accents.")


# ------------------------------------------------------------------ token
def token() -> str:
    t = os.environ.get("REPLICATE_API_TOKEN")
    if t:
        return t.strip()
    candidates = [os.environ.get("REPLICATE_TOKEN_FILE")] + [
        Path.home() / ".config/replicate" / n for n in ("token", "api_token", "api-token", "config.json")]
    for c in candidates:
        if not c:
            continue
        c = Path(c).expanduser()
        if c.is_file():
            raw = c.read_text().strip()
            if c.suffix == ".json":
                d = json.loads(raw)
                raw = d.get("token") or d.get("api_token") or d.get("REPLICATE_API_TOKEN") or ""
            if raw:
                return raw
    sys.exit("no Replicate token: set REPLICATE_API_TOKEN or REPLICATE_TOKEN_FILE")


# ------------------------------------------------------------------ replicate
def data_uri(path: Path) -> str:
    im = Image.open(path).convert("RGB")
    if max(im.size) < 1024:           # give the model something to work with
        im = im.resize((im.width * 2, im.height * 2), Image.NEAREST)
    import io
    buf = io.BytesIO()
    im.save(buf, "PNG", optimize=True)
    b = buf.getvalue()
    if len(b) > 250_000:
        buf = io.BytesIO(); im.save(buf, "JPEG", quality=90); b = buf.getvalue()
        return "data:image/jpeg;base64," + base64.b64encode(b).decode()
    return "data:image/png;base64," + base64.b64encode(b).decode()


def predict(tok: str, model: str, inp: dict, timeout: float = 240) -> list[str]:
    h = {"Authorization": f"Bearer {tok}", "Content-Type": "application/json", "Prefer": "wait=60"}
    for attempt in range(8):
        r = requests.post(f"{API}/models/{model}/predictions", headers=h, json={"input": inp}, timeout=90)
        if r.status_code == 429:          # low-credit accounts: 6/min, burst 1
            wait = float(r.headers.get("retry-after") or 12)
            try:
                wait = float(r.json().get("retry_after", wait))
            except Exception:  # noqa: BLE001
                pass
            time.sleep(wait + 1)
            continue
        break
    if r.status_code >= 400:
        raise RuntimeError(f"{model}: HTTP {r.status_code} {r.text[:300]}")
    p = r.json()
    t0 = time.time()
    while p["status"] not in ("succeeded", "failed", "canceled"):
        if time.time() - t0 > timeout:
            raise RuntimeError(f"{model}: timed out ({p.get('id')})")
        time.sleep(1.5)
        p = requests.get(p["urls"]["get"], headers=h, timeout=30).json()
    if p["status"] != "succeeded":
        raise RuntimeError(f"{model}: {p['status']}: {p.get('error')}")
    out = p["output"]
    return out if isinstance(out, list) else [out]


def download(url: str, dst: Path) -> Path:
    r = requests.get(url, timeout=120)
    r.raise_for_status()
    dst.write_bytes(r.content)
    return dst


# ------------------------------------------------------------------ pixel post-processing
def quantize(im: Image.Image) -> Image.Image:
    pal = Image.new("P", (1, 1))
    flat = [c for rgb in PALETTE for c in rgb]
    flat += [0] * (768 - len(flat))
    pal.putpalette(flat)
    return im.convert("RGB").quantize(palette=pal, dither=Image.Dither.NONE).convert("RGB")


def pixelate(src: Path, mode: str, no_quant: bool) -> Image.Image:
    lw, lh, scale, y0 = MODES[mode]
    im = Image.open(src).convert("RGB")
    # centre crop to the target aspect, then box-filter down to logical pixels
    want = lw / lh
    have = im.width / im.height
    if have > want:
        w = int(im.height * want); im = im.crop(((im.width - w) // 2, 0, (im.width - w) // 2 + w, im.height))
    elif have < want:
        h = int(im.width / want); im = im.crop((0, (im.height - h) // 2, im.width, (im.height - h) // 2 + h))
    small = im.resize((lw, lh), Image.BOX)
    if not no_quant:
        small = quantize(small)
    big = small.resize((lw * scale, lh * scale), Image.NEAREST)
    screen = Image.new("RGB", (480, 480), BG)
    screen.paste(big, ((480 - big.width) // 2, y0))
    return screen


def contact_sheet(paths: list[Path], dst: Path, cell: int = 240) -> None:
    n = len(paths)
    cols = min(4, n)
    rows = (n + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cell, rows * cell), (0, 0, 0))
    for i, p in enumerate(paths):
        im = Image.open(p).convert("RGB")
        im.thumbnail((cell, cell))
        sheet.paste(im, ((i % cols) * cell, (i // cols) * cell))
    sheet.save(dst)


# ------------------------------------------------------------------ main
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prompt", nargs="?", help="what to change / draw")
    ap.add_argument("--ref", type=Path, nargs="+", help="reference PNG(s), e.g. docs/img/home.png; switches to an editing model."
                    " nano-banana takes several (first = identity, then layout sketches), the others use the first")
    ap.add_argument("--model", choices=MODELS, help="default: nano-banana with --ref, recraft without")
    ap.add_argument("-n", type=int, default=2, help="variants")
    ap.add_argument("--parallel", type=int, default=1, help="concurrent predictions (accounts under $5 credit allow 1)")
    ap.add_argument("--mode", choices=MODES, help="default: full (240x240@2x) with --ref, else stage (160x121@3x)")
    ap.add_argument("--no-quantize", action="store_true", help="keep model colours (still pixelated)")
    ap.add_argument("--no-style", action="store_true", help="don't append the pixel-art style hint to the prompt")
    ap.add_argument("--from-images", nargs="*", type=Path, help="skip the API, just post-process these files")
    ap.add_argument("--out", type=Path, help="batch folder (default mocks/<timestamp>)")
    ap.add_argument("--show", action="store_true", help="slideshow the results on the device (tools/dev.py show)")
    ap.add_argument("--seconds", type=float, default=30)
    a = ap.parse_args()

    a.mode = a.mode or ("full" if a.ref else "stage")
    a.mode = a.mode or ("full" if a.ref else "stage")
    out = a.out or ROOT / "mocks" / datetime.now().strftime("%Y-%m-%d_%H%M%S")
    out.mkdir(parents=True, exist_ok=True)

    raws: list[Path] = []
    if a.from_images:
        raws = list(a.from_images)
    else:
        if not a.prompt:
            ap.error("a prompt is required unless --from-images is given")
        model = a.model or ("nano-banana" if a.ref else "recraft")
        name, needs_ref, build = MODELS[model]
        if needs_ref and not a.ref:
            ap.error(f"--model {model} needs --ref")
        prompt = a.prompt if a.no_style else a.prompt + STYLE_HINT
        ref = [data_uri(r) for r in a.ref] if a.ref else None
        tok = token()
        (out / "prompt.json").write_text(json.dumps(
            {"model": name, "prompt": prompt, "ref": [str(r) for r in a.ref or []], "n": a.n}, indent=1))
        print(f"{name}: {a.n} x {prompt[:80]}…")

        def one(i: int) -> list[Path]:
            urls = predict(tok, name, build(prompt, ref))
            return [download(u, out / f"{out.name}_raw_{i:02d}{'' if k == 0 else chr(97 + k)}.png")
                    for k, u in enumerate(urls)]

        with ThreadPoolExecutor(max_workers=min(a.parallel, a.n)) as ex:
            for fut in [ex.submit(one, i) for i in range(a.n)]:
                try:
                    got = fut.result()
                    raws += got
                    print("  got", ", ".join(p.name for p in got))
                except Exception as e:  # noqa: BLE001 - report and keep the rest of the batch
                    print("  FAILED:", e, file=sys.stderr)
        if not raws:
            return 1

    screens: list[Path] = []
    for r in raws:
        stem = r.stem if r.stem.startswith(out.name) else f"{out.name}_{r.stem}"
        dst = out / f"{stem.replace('raw', 'mock')}_{a.mode}.png"
        pixelate(r, a.mode, a.no_quantize).save(dst)
        screens.append(dst)
    contact_sheet(raws, out / f"{out.name}_sheet_raw.png")
    contact_sheet(screens, out / f"{out.name}_sheet_mock.png")
    print(f"{len(screens)} mock(s) in {out}")

    if a.show:
        py = sys.executable
        cmd = [py, str(ROOT / "tools/dev.py"), "show", *map(str, screens), "--seconds", str(a.seconds)]
        if len(screens) > 1:
            cmd += ["--interval", str(max(2.0, a.seconds / len(screens)))]
        return subprocess.call(cmd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
