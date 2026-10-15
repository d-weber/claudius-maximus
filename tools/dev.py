#!/usr/bin/env python3
"""Developer tool for Claudius Maximus. Run with host/.venv/bin/python tools/dev.py <cmd>.

There is ONE physical device shared by several developers/agents, so every command that touches
it takes an exclusive lock (.devlock). Builds use a per-developer build dir so they don't collide.

  build                         compile firmware (own build dir, set DEV_NAME=<you>)
  flash                         build + upload to the device (locked)
  shot OUT.png [options]        stream demo data, navigate, screenshot the device (locked)
      --screen ID               tile to show (home, agents, feed, usage, orbit, settings)
      --detail                  open the detail overlay for the first demo session
      --alert KIND              trigger alerts_demo(KIND) before the shot
      --seconds N               seconds of demo data to stream before the shot (default 4)
      --nodemo                  don't stream demo data
  perf [--screen ID] [--seconds N] [--nodemo] [--detail]
                                measure fps / frame time / cpu% / heap on a screen (locked)
  demo [--seconds N]            stream demo data to the device (locked)
  send JSON                     send one raw JSON line (locked)
  monitor [--seconds N]         print device output (locked)

Exit code != 0 on failure. Don't hold the device more than ~60 s at a time.
"""
from __future__ import annotations

import argparse
import fcntl
import json
import os
import struct
import subprocess
import sys
import time
import zlib
from contextlib import contextmanager
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "host"))

from companion.link import DeviceLink  # noqa: E402
from companion.providers.demo import DemoProvider  # noqa: E402
from companion.daemon import snapshot  # noqa: E402


def extra_providers() -> list:
    """Real (non-session) providers that are safe to mix into demo streams, e.g. host system metrics."""
    out = []
    try:
        from companion.providers.system import SystemProvider
        out.append(SystemProvider())
    except Exception:
        pass
    return out

FW = ROOT / "firmware"
LOCK = ROOT / ".devlock"


def dev_name() -> str:
    return os.environ.get("DEV_NAME", "default")


def build_dir() -> Path:
    return FW / ".pio" / f"build-{dev_name()}"


@contextmanager
def device_lock(what: str):
    want = ROOT / f".devwant.{os.getpid()}"   # tells the companion daemon to release the port now
    want.touch()
    fd = os.open(LOCK, os.O_CREAT | os.O_RDWR)
    t0 = time.time()
    while True:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            break
        except BlockingIOError:
            if time.time() - t0 > 600:
                sys.exit("timed out waiting for device lock")
            time.sleep(0.5)
    waited = time.time() - t0
    if waited > 1:
        print(f"[dev] waited {waited:.0f}s for device lock", file=sys.stderr)
    os.ftruncate(fd, 0)
    os.write(fd, f"{dev_name()} {what} pid={os.getpid()}\n".encode())
    try:
        yield
    finally:
        fcntl.flock(fd, fcntl.LOCK_UN)
        os.close(fd)
        want.unlink(missing_ok=True)


LASTFLASH = FW / ".pio" / "lastflash.json"   # what is (probably) on the device: written by every `flash`


def fw_tree_hash(fw: Path = FW) -> str:
    """Short hash of the firmware sources (src/, include/, lib/, platformio.ini) = identity of a build."""
    import hashlib
    h = hashlib.sha1()
    files = [fw / "platformio.ini"]
    for d in ("src", "include", "lib"):
        files += sorted(p for p in (fw / d).rglob("*") if p.is_file())
    for p in files:
        try:
            h.update(str(p.relative_to(fw)).encode() + b"\0" + p.read_bytes() + b"\0")
        except OSError:
            pass
    return h.hexdigest()[:10]


def last_flash() -> dict:
    try:
        return json.loads(LASTFLASH.read_text())
    except (OSError, ValueError):
        return {}


def pio(*args: str) -> int:
    env = dict(os.environ, PLATFORMIO_BUILD_DIR=str(build_dir()))
    return subprocess.call(["pio", "run", *args], cwd=FW, env=env)


def cmd_build(_a) -> int:
    return pio()


def cmd_flash(_a) -> int:
    tree = fw_tree_hash()  # hash before building: what this build is made of
    rc = pio()
    if rc:
        return rc
    with device_lock("flash"):
        rc = pio("-t", "upload")
        if rc == 0:
            try:
                LASTFLASH.write_text(json.dumps({"by": dev_name(), "ts": int(time.time()), "tree": tree}))
            except OSError:
                pass
        time.sleep(2.5)  # let it boot
    return rc


def open_link() -> DeviceLink:
    link = DeviceLink()
    for _ in range(20):
        if link.connect():
            return link
        time.sleep(0.5)
    sys.exit("device not found")


def stream_demo(link: DeviceLink, seconds: float, demo: DemoProvider | None = None) -> DemoProvider:
    demo = demo or DemoProvider(speed=3.0)
    if not hasattr(demo, "_extras"):
        demo._extras = extra_providers()
    t_end = time.time() + seconds
    while time.time() < t_end:
        now = time.time()
        for p in [demo, *demo._extras]:
            p.poll(now)
        for e in demo.drain_events():
            link.send(e)
        link.send(snapshot([demo, *demo._extras]))
        for m in link.read_messages():
            if m.get("t") == "log":
                print("[device]", m.get("msg"))
        time.sleep(0.25)
    return demo


def write_png(path: Path, w: int, h: int, rgb565: bytes, rot: int) -> None:
    px = struct.unpack(f"<{w * h}H", rgb565)
    if rot == 180:
        px = px[::-1]
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        for v in px[y * w:(y + 1) * w]:
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            rows += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))

    def chunk(t: bytes, d: bytes) -> bytes:
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def cmd_shot(a) -> int:
    out = Path(a.out).resolve()
    with device_lock("shot"):
        link = open_link()
        demo = None
        if not a.nodemo:
            demo = stream_demo(link, 0.5)
        if a.screen:
            link.send({"t": "cmd", "nav": a.screen})
        if a.detail and demo:
            link.send({"t": "cmd", "open": "detail", "sid": demo.sessions[0]["id"]})
        if not a.nodemo:
            stream_demo(link, a.seconds, demo)
        if a.alert:
            link.send({"t": "cmd", "demo_alert": a.alert})
            time.sleep(a.alert_wait)
        time.sleep(0.3)
        w, h, data, rot = link.screenshot()
        link.close()
    write_png(out, w, h, data, rot)
    print(out)
    return 0


def read_perf(link: DeviceLink, timeout: float = 3.0) -> dict | None:
    link.send({"t": "cmd", "perf": 1})
    t_end = time.time() + timeout
    while time.time() < t_end:
        for m in link.read_messages():
            if m.get("t") == "perf":
                return m
        time.sleep(0.05)
    return None


def cmd_perf(a) -> int:
    """Measure firmware performance on a screen: resets the window, runs N s (demo stream unless --nodemo)."""
    with device_lock("perf"):
        link = open_link()
        demo = None if a.nodemo else stream_demo(link, 0.5)
        if a.screen:
            link.send({"t": "cmd", "nav": a.screen})
        if a.detail and demo:
            link.send({"t": "cmd", "open": "detail", "sid": demo.sessions[0]["id"]})
        time.sleep(0.5)
        read_perf(link)  # discard warm-up window, start fresh
        if a.nodemo:
            time.sleep(a.seconds)
        else:
            stream_demo(link, a.seconds, demo)
        res = read_perf(link)
        link.close()
    if not res:
        print("no perf reply", file=sys.stderr)
        return 1
    print(json.dumps(res, indent=None if a.compact else 2))
    return 0


def cmd_demo(a) -> int:
    with device_lock("demo"):
        link = open_link()
        stream_demo(link, a.seconds)
    return 0


def cmd_send(a) -> int:
    with device_lock("send"):
        link = open_link()
        link.send(json.loads(a.json))
        time.sleep(0.5)
        for m in link.read_messages():
            print(json.dumps(m))
    return 0


def cmd_monitor(a) -> int:
    with device_lock("monitor"):
        link = open_link()
        link.send({"t": "cmd", "hello": 1})
        t_end = time.time() + a.seconds
        while time.time() < t_end:
            for m in link.read_messages():
                print(json.dumps(m))
            time.sleep(0.1)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("build").set_defaults(f=cmd_build)
    sub.add_parser("flash").set_defaults(f=cmd_flash)
    s = sub.add_parser("shot")
    s.add_argument("out")
    s.add_argument("--screen")
    s.add_argument("--detail", action="store_true")
    s.add_argument("--alert")
    s.add_argument("--alert-wait", type=float, default=0.8)
    s.add_argument("--seconds", type=float, default=4)
    s.add_argument("--nodemo", action="store_true")
    s.set_defaults(f=cmd_shot)
    pf = sub.add_parser("perf")
    pf.add_argument("--screen")
    pf.add_argument("--detail", action="store_true")
    pf.add_argument("--seconds", type=float, default=10)
    pf.add_argument("--nodemo", action="store_true")
    pf.add_argument("--compact", action="store_true")
    pf.set_defaults(f=cmd_perf)
    d = sub.add_parser("demo")
    d.add_argument("--seconds", type=float, default=20)
    d.set_defaults(f=cmd_demo)
    se = sub.add_parser("send")
    se.add_argument("json")
    se.set_defaults(f=cmd_send)
    m = sub.add_parser("monitor")
    m.add_argument("--seconds", type=float, default=5)
    m.set_defaults(f=cmd_monitor)
    a = ap.parse_args()
    return a.f(a)


if __name__ == "__main__":
    sys.exit(main())
