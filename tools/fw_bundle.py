#!/usr/bin/env python3
"""Build the firmware and bundle the flashable images for the desktop app.

  host/.venv/bin/python tools/fw_bundle.py [--board 4b|all] [--no-build] [--out DIR]

For each board: `pio run -e <env>` in its own build dir (firmware/.pio/build-bundle/<env>, so it never collides
with tools/dev.py builds), then copy bootloader.bin, partitions.bin, the initial otadata image (ESP-IDF's
ota_data_initial.bin, kept under its historical name boot_app0.bin: same content, same 0xe000 slot the Arduino
builds used) and firmware.bin into <out>/<board>/ (default host/companion/firmware/<board>/) and write
manifest.json (docs/DESKTOP.md "Flasher").

Offsets and flash parameters are the ones `pio run -t upload -v` passes to esptool for these envs
(pioarduino 55.03.31, ESP-IDF 5.5.1; the layout is firmware/partitions.csv):
  --flash-mode dio --flash-freq 80m --flash-size detect
  0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
(ESP-IDF writes "dio" into the image header for CONFIG_ESPTOOLPY_FLASHMODE_QIO; the QIO bootloader switches
modes itself.) We write flash_size 16MB instead of "detect": the board carries 16 MB and it skips a probe.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "firmware"

ENVS = {"4b": "companion"}
NAMES = {"4b": "ESP32-S3-Touch-LCD-4B"}   # board.h BOARD_NAME
PARTS = [("bootloader.bin", "0x0"), ("partitions.bin", "0x8000"), ("boot_app0.bin", "0xe000"),
         ("firmware.bin", "0x10000")]
FLASH = {"chip": "esp32s3", "flash_size": "16MB", "flash_mode": "dio", "flash_freq": "80m"}
BUILD_ROOT = FW / ".pio" / "build-bundle"


def fw_version() -> str:
    m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', (FW / "src" / "core" / "version.h").read_text())
    if not m:
        sys.exit("FW_VERSION not found in firmware/src/core/version.h")
    return m.group(1)


def tree_hash(fw: Path = FW) -> str:
    """Same as tools/dev.py fw_tree_hash (kept in sync by hand): dev.py imports fcntl and the whole daemon, which
    a Windows/macOS CI runner bundling firmware should not need."""
    import hashlib
    h = hashlib.sha1()
    files = [fw / n for n in ("platformio.ini", "CMakeLists.txt", "sdkconfig.defaults", "partitions.csv")]
    # assets/ instead of the sprites_gen.* generated from it, which exist only after a first build
    for d in ("src", "include", "lib", "assets"):
        files += sorted(p for p in (fw / d).rglob("*") if p.is_file() and not p.name.startswith("sprites_gen."))
    for p in files:
        try:
            h.update(str(p.relative_to(fw)).encode() + b"\0" + p.read_bytes() + b"\0")
        except OSError:
            pass
    return h.hexdigest()[:10]


def pio_bin() -> str:
    cand = Path(sys.executable).parent / "pio"
    return str(cand) if cand.exists() else (shutil.which("pio") or "pio")


def build(env: str) -> int:
    for p in FW.glob("sdkconfig.*"):   # same as tools/dev.py drop_stale_sdkconfig: defaults only apply to a fresh file
        if p.name != "sdkconfig.defaults" and p.is_file() and p.stat().st_mtime < (FW / "sdkconfig.defaults").stat().st_mtime:
            p.unlink()
    e = dict(os.environ, PLATFORMIO_BUILD_DIR=str(BUILD_ROOT))
    return subprocess.call([pio_bin(), "run", "-e", env], cwd=FW, env=e)


def bundle(board: str, out_root: Path, tree: str, version: str) -> Path:
    src = BUILD_ROOT / ENVS[board]
    files = {"bootloader.bin": src / "bootloader.bin", "partitions.bin": src / "partitions.bin",
             "boot_app0.bin": src / "ota_data_initial.bin", "firmware.bin": src / "firmware.bin"}
    for name, p in files.items():
        if not p.is_file():
            sys.exit(f"{board}: {p} missing (build first, or drop --no-build)")
    out = out_root / board
    out.mkdir(parents=True, exist_ok=True)
    for name, p in files.items():
        shutil.copyfile(p, out / name)
    manifest = {"board": board, "board_name": NAMES[board], "fw_version": version, "tree": tree, **FLASH,
                "parts": [{"file": f, "offset": o} for f, o in PARTS],
                "built": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    size = sum((out / f).stat().st_size for f, _ in PARTS)
    print(f"[bundle] {board}: fw {version} tree {tree} -> {out} ({size // 1024} KB)")
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--board", choices=["4b", "all"], default="all")
    ap.add_argument("--no-build", action="store_true", help="bundle the existing build dir as is")
    ap.add_argument("--out", type=Path, default=ROOT / "host" / "companion" / "firmware")
    a = ap.parse_args(argv)
    boards = list(ENVS) if a.board == "all" else [a.board]
    tree, version = tree_hash(), fw_version()   # hash before building: what this build is made of
    for b in boards:
        if not a.no_build:
            rc = build(ENVS[b])
            if rc:
                print(f"[bundle] {b}: pio run -e {ENVS[b]} failed ({rc})", file=sys.stderr)
                return rc
        bundle(b, a.out.resolve(), tree, version)
    return 0


if __name__ == "__main__":
    sys.exit(main())
