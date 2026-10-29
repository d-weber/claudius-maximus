#!/usr/bin/env python3
"""Live preview of Clawd's Studio in a browser, for editing the sprite sheets.

  host/.venv/bin/python tools/room_view.py [--port 8765] [--no-open]

Serves tools/room_view/index.html on http://localhost:8765. The page picks a state (task, time of day, helpers,
plan progress, a reaction) and plays what the firmware would draw, 4x like the device. "Refresh sprites" (or the
auto-watch, polling the sheets' mtimes) reruns the pipeline the firmware build runs: tools/sprites.py build from
firmware/assets/sprites, recompile the host simulator (tools/room_sim.py, the same room C++ as the firmware), and
re-render the current state. An invalid sheet shows the converter's error on the page instead of a frame.
Needs pillow and g++. Nothing is written outside mocks/room_view/."""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import subprocess
import sys
import threading
import time
import webbrowser
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parent.parent
try:
    import PIL  # noqa: F401
except ImportError:   # run with the system python: switch to the repo venv, which has pillow
    venv_py = ROOT / "host" / ".venv" / "bin" / "python"
    # sys.prefix, not the executable's path: the venv's python is a symlink to the same binary as the system one
    if venv_py.is_file() and Path(sys.prefix).resolve() != (ROOT / "host" / ".venv").resolve():
        os.execv(str(venv_py), [str(venv_py), *sys.argv])
    sys.exit("error: pillow is not installed: run host/.venv/bin/pip install -e host, "
             "then host/.venv/bin/python tools/room_view.py")
ASSETS = ROOT / "firmware" / "assets" / "sprites"
PAGE = ROOT / "tools" / "room_view" / "index.html"
WORK = ROOT / "mocks" / "room_view"

_spec = importlib.util.spec_from_file_location("room_sim", ROOT / "tools" / "room_sim.py")
room_sim = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(room_sim)

TASKS = room_sim.TASKS
REACTS = ["glance", "hop", "surprise", "wink", "giggle", "happy", "wince"]


def sheets_stamp() -> float:
    """The newest mtime under the assets: what "did the sheets change" means to the watcher."""
    return max((p.stat().st_mtime for p in ASSETS.iterdir() if p.is_file()), default=0.0)


class Renderer:
    """One simulator binary, rebuilt when the sheets or the room sources change (room_sim.build caches on mtimes).
    `error` holds the converter's or compiler's message while the sheets are broken, so the page can show it."""

    def __init__(self):
        self.lock = threading.Lock()
        self.exe: Path | None = None
        self.error = ""
        self.stamp = 0.0
        self.version = 0

    def refresh(self) -> None:
        with self.lock:
            self.stamp = sheets_stamp()
            self.version += 1
            # the converter first, with its output captured: a bad sheet is the error the page must show
            r = subprocess.run([sys.executable, str(ROOT / "tools" / "sprites.py"), "build", "--out", str(room_sim.SRC)],
                               capture_output=True, text=True)
            if r.returncode:
                self.exe, self.error = None, (r.stderr or r.stdout).strip()
                return
            try:
                self.exe = room_sim.build(WORK / "build")   # a no-op converter run, then g++ if anything changed
                self.error = ""
            except subprocess.CalledProcessError as e:
                self.exe = None
                self.error = f"compile failed (see the terminal running room_view.py): {e}"

    def render(self, task: str, tod: float, helpers: int, todo: int, done: int, react: str | None,
               seconds: float, seed: int) -> tuple[bytes, int, str]:
        """A filmstrip PNG (frames stacked vertically), its frame count and the activity log."""
        with self.lock:
            if self.exe is None:
                raise RuntimeError(self.error or "not built")
            out = WORK / "run"
            kw = dict(task=task if task != "free" else None, seconds=seconds, tod=tod, helpers=helpers,
                      todo=(todo, done) if todo else None, seed=seed, every=1)
            if react:
                kw["react"] = (react, 3)
            frames, log = room_sim.run(self.exe, out, **kw)
        from PIL import Image
        strip = Image.new("RGB", (room_sim.W, room_sim.H * len(frames)))
        for i, f in enumerate(frames):
            strip.paste(f, (0, i * room_sim.H))
        buf = BytesIO()
        strip.save(buf, "PNG", optimize=False)
        return buf.getvalue(), len(frames), log


R = Renderer()



class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):   # quiet: the terminal is for the sprites.py errors
        pass

    def send(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, obj, code: int = HTTPStatus.OK) -> None:
        self.send(code, json.dumps(obj).encode(), "application/json")

    def do_GET(self):   # noqa: N802
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/":
            return self.send(HTTPStatus.OK, PAGE.read_bytes(), "text/html; charset=utf-8")
        if u.path == "/status":
            return self.send_json({"version": R.version, "error": R.error, "tasks": TASKS, "reacts": REACTS,
                                   "sheets": sorted(p.name for p in ASSETS.glob("*.png")),
                                   "stamp": R.stamp})
        if u.path == "/frames":
            try:
                png, n, log = R.render(q.get("task", "code"), float(q.get("tod", 10)), int(q.get("helpers", 0)),
                                       int(q.get("todo", 0)), int(q.get("done", 0)), q.get("react") or None,
                                       min(60.0, float(q.get("seconds", 8))), int(q.get("seed", 1)))
            except Exception as e:   # noqa: BLE001
                return self.send_json({"error": str(e)}, HTTPStatus.CONFLICT)
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "image/png")
            self.send_header("Content-Length", str(len(png)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Frames", str(n))
            self.send_header("X-Version", str(R.version))
            self.end_headers()
            self.wfile.write(png)
            return
        if u.path.startswith("/sheet/"):
            p = ASSETS / Path(u.path[len("/sheet/"):]).name
            if p.suffix == ".png" and p.is_file():
                return self.send(HTTPStatus.OK, p.read_bytes(), "image/png")
        self.send(HTTPStatus.NOT_FOUND, b"not found", "text/plain")

    def do_POST(self):   # noqa: N802
        if urlparse(self.path).path == "/refresh":
            R.refresh()
            return self.send_json({"version": R.version, "error": R.error})
        self.send(HTTPStatus.NOT_FOUND, b"not found", "text/plain")


def watcher(interval: float = 1.0) -> None:
    """Rebuild when a sheet file changes on disk (Photoshop saves): the page sees the version bump on /status."""
    while True:
        time.sleep(interval)
        try:
            if sheets_stamp() != R.stamp:
                time.sleep(0.3)   # let the editor finish writing
                R.refresh()
        except Exception:   # noqa: BLE001
            pass


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--no-open", action="store_true", help="do not open the browser")
    a = ap.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    R.refresh()
    if R.error:
        print(R.error, file=sys.stderr)
    threading.Thread(target=watcher, daemon=True).start()
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    url = f"http://localhost:{a.port}/"
    print(f"room view: {url}  (sheets: {ASSETS})")
    if not a.no_open:
        webbrowser.open(url)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
