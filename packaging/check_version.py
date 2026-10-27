#!/usr/bin/env python3
"""Release guard: the git tag must be `v` + companion.version.__version__ (the app's single version source).

  python packaging/check_version.py              print the app version (used for artifact names)
  python packaging/check_version.py v0.3.0       exit 1 with a message unless the tag matches
  python packaging/check_version.py --fw         print the firmware version (firmware/src/core/version.h)

Reads the files with a regex instead of importing companion, so it runs before `pip install`.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP_VERSION_FILE = ROOT / "host" / "companion" / "version.py"
FW_VERSION_FILE = ROOT / "firmware" / "src" / "core" / "version.h"


def app_version(path: Path = APP_VERSION_FILE) -> str:
    m = re.search(r'^__version__\s*=\s*["\']([^"\']+)["\']', path.read_text(), re.M)
    if not m:
        raise ValueError(f"no __version__ in {path}")
    return m.group(1)


def fw_version(path: Path = FW_VERSION_FILE) -> str:
    m = re.search(r'^\s*#define\s+FW_VERSION\s+"([^"]+)"', path.read_text(), re.M)
    if not m:
        raise ValueError(f"no FW_VERSION in {path}")
    return m.group(1)


def check_tag(tag: str, version: str) -> str | None:
    """None when the tag matches, else a human sentence. Accepts `refs/tags/v1.2.3` as well as `v1.2.3`."""
    tag = tag.strip()
    if tag.startswith("refs/tags/"):
        tag = tag[len("refs/tags/"):]
    if tag == f"v{version}":
        return None
    return (f"tag {tag!r} does not match the app version {version!r} (host/companion/version.py): "
            f"tag the release v{version}, or bump __version__ first")


def main(argv: list[str]) -> int:
    if argv and argv[0] == "--fw":
        print(fw_version())
        return 0
    version = app_version()
    if not argv:
        print(version)
        return 0
    err = check_tag(argv[0], version)
    if err:
        print(f"error: {err}", file=sys.stderr)
        return 1
    print(f"ok: {argv[0]} == v{version}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
