#!/usr/bin/env python3
"""Fill @VERSION@ / @ARCH@ placeholders in a packaging template (the .desktop file for the AppImage/tar.gz).

  python packaging/subst.py IN OUT [--version X.Y.Z] [--arch x64]

The version defaults to companion.version.__version__ (via check_version.py). The Inno Setup script does not need
this: it takes the version as an ISCC define (/DAppVersion=X.Y.Z).
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_version import app_version  # noqa: E402


def substitute(text: str, version: str, arch: str = "x64") -> str:
    return text.replace("@VERSION@", version).replace("@ARCH@", arch)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("src", type=Path)
    ap.add_argument("dst", type=Path)
    ap.add_argument("--version", default=None)
    ap.add_argument("--arch", default="x64")
    a = ap.parse_args(argv)
    a.dst.write_text(substitute(a.src.read_text(), a.version or app_version(), a.arch))
    return 0


if __name__ == "__main__":
    sys.exit(main())
