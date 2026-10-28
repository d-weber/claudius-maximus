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
      --calm                    demo sessions never wait on you (no "needs you" glow; for docs shots)
      --settle N                freeze the demo for N s before the shot so toasts fade (docs shots: 4)
  perf [--screen ID] [--seconds N] [--nodemo] [--detail] [--cycle]
                                measure fps / frame time / cpu% / heap on a screen (locked)
  demo [--seconds N]            stream demo data to the device (locked)
  send JSON                     send one raw JSON line (locked)
  monitor [--seconds N]         print device output (locked)
  show IMG.png [...] [--seconds N] [--interval S] [--keep]
                                show PNG mock-ups full screen on the device (locked; needs pillow)

Exit code != 0 on failure. Don't hold the device more than ~60 s at a time.
"""
from __future__ import annotations

import argparse
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

from companion import locking  # noqa: E402
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
    fd = locking.open_lock_file(LOCK)
    t0 = time.time()
    while not locking.try_lock(fd):
        if time.time() - t0 > 600:
            sys.exit("timed out waiting for device lock")
        time.sleep(0.5)
    waited = time.time() - t0
    if waited > 1:
        print(f"[dev] waited {waited:.0f}s for device lock", file=sys.stderr)
    locking.write_holder(fd, f"{dev_name()} {what} pid={os.getpid()}\n")
    try:
        yield
    finally:
        locking.unlock(fd)
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


def pio_bin() -> str:
    """`pio` next to the running interpreter (host/.venv/bin/pio when installed there), else from PATH."""
    cand = Path(sys.executable).parent / "pio"
    return str(cand) if cand.exists() else "pio"


def pio(*args: str) -> int:
    env = dict(os.environ, PLATFORMIO_BUILD_DIR=str(build_dir()))
    return subprocess.call([pio_bin(), "run", *args], cwd=FW, env=env)


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


def stream_demo(link: DeviceLink, seconds: float, demo: DemoProvider | None = None, calm: bool = False,
                freeze: bool = False) -> DemoProvider:
    """Stream demo data for `seconds`. With freeze=True the demo stops evolving (no new events: toasts fade out)
    but the snapshot keeps going out so the device still sees the host."""
    demo = demo or DemoProvider(speed=3.0, calm=calm)
    if not hasattr(demo, "_extras"):
        demo._extras = extra_providers()
    t_end = time.time() + seconds
    while time.time() < t_end:
        now = time.time()
        for p in ([] if freeze else [demo]) + list(demo._extras):
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
            demo = stream_demo(link, 0.5, calm=getattr(a, "calm", False))
        if a.screen:
            link.send({"t": "cmd", "nav": a.screen})
        if a.detail and demo:
            link.send({"t": "cmd", "open": "detail", "sid": demo.sessions[0]["id"]})
        if not a.nodemo:
            stream_demo(link, a.seconds, demo)
            if a.settle:
                stream_demo(link, a.settle, demo, freeze=True)
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
        if a.cycle:
            # page changes every 1.2 s (tileview slide animations = heaviest redraws), demo data streaming
            order = ["home", "agents", "feed", "usage", "system", "orbit", "settings"]
            t_end = time.time() + a.seconds
            i = order.index(a.screen) if a.screen in order else 0
            while time.time() < t_end:
                i = (i + 1) % len(order)
                link.send({"t": "cmd", "nav": order[i]})
                if a.nodemo:
                    time.sleep(1.2)
                else:
                    stream_demo(link, 1.2, demo)
        elif a.nodemo:
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


def png_to_rgb565(path: Path) -> bytes:
    """480x480 RGB565 little-endian (LVGL native, LV_COLOR_16_SWAP 0); other sizes are centred on the bg colour."""
    from PIL import Image
    im = Image.open(path).convert("RGB")
    if im.size != (480, 480):
        bg = Image.new("RGB", (480, 480), (0x1A, 0x19, 0x18))
        im.thumbnail((480, 480))
        bg.paste(im, ((480 - im.width) // 2, (480 - im.height) // 2))
        im = bg
    return im.tobytes("raw", "BGR;16")   # Pillow's name for RGB565 little-endian


def cmd_show(a) -> int:
    """Show PNG mock-ups full screen on the device (slideshow when several), then clear."""
    frames = [png_to_rgb565(Path(p)) for p in a.images]
    with device_lock("show"):
        link = open_link()
        try:
            show_loop(a, frames, link)
        finally:
            if not a.keep:
                link.send({"t": "cmd", "img": 0})
            link.close()
    return 0


def show_loop(a, frames, link) -> None:
    if True:
        t_end = time.time() + a.seconds
        i = 0
        while True:
            link.send({"t": "cmd", "img": len(frames[i % len(frames)])})
            link.ser.write(frames[i % len(frames)])
            link.ser.flush()
            i += 1
            if time.time() >= t_end:
                break
            time.sleep(a.interval if len(frames) > 1 else min(1.0, max(0.0, t_end - time.time())))
            if len(frames) == 1 and time.time() < t_end:
                time.sleep(max(0.0, t_end - time.time()))
                break


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
    s.add_argument("--calm", action="store_true")
    s.add_argument("--settle", type=float, default=0, help="seconds of frozen demo before the shot (toasts fade)")
    s.set_defaults(f=cmd_shot)
    pf = sub.add_parser("perf")
    pf.add_argument("--screen")
    pf.add_argument("--detail", action="store_true")
    pf.add_argument("--seconds", type=float, default=10)
    pf.add_argument("--nodemo", action="store_true")
    pf.add_argument("--compact", action="store_true")
    pf.add_argument("--cycle", action="store_true", help="change page every 1.2 s during the window")
    pf.set_defaults(f=cmd_perf)
    sh = sub.add_parser("show")
    sh.add_argument("images", nargs="+", help="PNG files (480x480; others are centred)")
    sh.add_argument("--seconds", type=float, default=20)
    sh.add_argument("--interval", type=float, default=0.5, help="slideshow period when several images")
    sh.add_argument("--keep", action="store_true", help="leave the last picture on screen")
    sh.set_defaults(f=cmd_show)
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
