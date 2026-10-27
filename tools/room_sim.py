#!/usr/bin/env python3
"""Host simulator for the home screen room (firmware/src/screens/room): renders Clawd's Studio without a device.

  python3 tools/room_sim.py states [--tod 10] [--out DIR]          one 4x frame per task, plus a contact sheet
  python3 tools/room_sim.py task code [--seconds 40] [--gif] [--tod 21] [--helpers 2] [--react happy 10]
  python3 tools/room_sim.py day [--seconds 1800] [--todspeed 24]  scripted session: activity log + variety stats + sheet
  python3 tools/room_sim.py clock [--seconds 86400]               24 h of lighting at one frame per 20 min

Builds tools/room_sim/sim.cpp with g++ (same sources as the firmware). Needs pillow."""
from __future__ import annotations

import argparse
import collections
import os
import subprocess
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "firmware" / "src" / "screens" / "room"
W, H = 120, 106
TASKS = ["code", "web", "review", "read", "run", "search", "think", "delegate", "plan", "idle", "wait", "sleep", "offline"]


def build(build_dir: Path) -> Path:
    build_dir.mkdir(parents=True, exist_ok=True)
    exe = build_dir / "room_sim"
    srcs = [ROOT / "tools" / "room_sim" / "sim.cpp", SRC / "room.cpp", SRC / "director.cpp"]
    newest = max(p.stat().st_mtime for p in srcs + [SRC / "room.h", SRC / "director.h"])
    if exe.exists() and exe.stat().st_mtime > newest:
        return exe
    cmd = ["g++", "-O2", "-std=c++17", "-Wall", "-Wno-misleading-indentation", f"-I{SRC}", *map(str, srcs), "-o", str(exe)]
    subprocess.run(cmd, check=True)
    return exe


def run(exe: Path, out: Path, **kw) -> tuple[list[Image.Image], str]:
    out.mkdir(parents=True, exist_ok=True)
    args = [str(exe), "--out", str(out)]
    for k, v in kw.items():
        if v is None or v is False:
            continue
        if v is True:
            args.append(f"--{k}")
        elif isinstance(v, (list, tuple)):
            args += [f"--{k}", *map(str, v)]
        else:
            args += [f"--{k}", str(v)]
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)
    data = (out / "frames.bin").read_bytes()
    n = len(data) // (W * H * 3)
    frames = [Image.frombytes("RGB", (W, H), data[i * W * H * 3:(i + 1) * W * H * 3]) for i in range(n)]
    return frames, (out / "log.txt").read_text()


def up(im: Image.Image, z: int) -> Image.Image:
    return im.resize((W * z, H * z), Image.NEAREST)


def sheet(frames: list[Image.Image], cols: int, z: int, path: Path) -> None:
    rows = (len(frames) + cols - 1) // cols
    s = Image.new("RGB", (cols * W * z, rows * H * z), (20, 20, 20))
    for i, f in enumerate(frames):
        s.paste(up(f, z), ((i % cols) * W * z, (i // cols) * H * z))
    s.save(path)


def gif(frames: list[Image.Image], path: Path, z: int, fps: int) -> None:
    ims = [up(f, z).quantize(colors=128, dither=Image.Dither.NONE) for f in frames]
    ims[0].save(path, save_all=True, append_images=ims[1:], duration=int(1000 / fps), loop=0, optimize=False)


def cmd_states(a, exe: Path) -> None:
    shots = []
    for t in TASKS:
        frames, _ = run(exe, a.out / "_run", task=t, seconds=9, every=12 * 9, tod=a.tod, helpers=a.helpers if t == "delegate" else 0, todo=(5, 2))
        shots.append(frames[-1])
        up(frames[-1], 4).save(a.out / f"state_{t}.png")
    sheet(shots, 4, 2, a.out / "states_sheet.png")
    print(a.out / "states_sheet.png")


def cmd_task(a, exe: Path) -> None:
    kw = dict(task=a.task, seconds=a.seconds, every=1, tod=a.tod, helpers=a.helpers, seed=a.seed, todo=(5, 2))
    if a.react:
        kw["react"] = a.react
    frames, log = run(exe, a.out / "_run", **kw)
    (a.out / f"task_{a.task}_log.txt").write_text(log)
    step = max(1, int(a.seconds * 12 / 24))
    sheet(frames[::step][:24], 6, 2, a.out / f"task_{a.task}_sheet.png")
    if a.gif:
        gif(frames, a.out / f"task_{a.task}.gif", 2, 12)
        print(a.out / f"task_{a.task}.gif")
    print(a.out / f"task_{a.task}_sheet.png")
    print(log[-1500:])


def stats(log: str) -> str:
    acts = [l.split()[1] for l in log.splitlines() if l and not l.startswith("#") and "INPUT" not in l]
    fid = [x for x in acts if x.startswith(("fidget:", "errand:"))]
    cnt = collections.Counter(fid)
    rep = sum(1 for i in range(1, len(fid)) if fid[i] == fid[i - 1])
    out = [f"activity changes: {len(acts)}, fidgets+errands: {len(fid)}, distinct: {len(cnt)}, back-to-back repeats: {rep}"]
    out += [f"  {k:20s} {v}" for k, v in cnt.most_common()]
    return "\n".join(out)


def cmd_day(a, exe: Path) -> None:
    frames, log = run(exe, a.out / "_run", day=True, seconds=a.seconds, every=12 * 20, tod=a.tod, todspeed=a.todspeed, seed=a.seed)
    (a.out / "day_log.txt").write_text(log)
    sheet(frames[:48], 8, 1, a.out / "day_sheet.png")
    print(a.out / "day_sheet.png")
    print(stats(log))


def cmd_clock(a, exe: Path) -> None:
    frames, log = run(exe, a.out / "_run", task="idle", seconds=a.seconds / a.todspeed, every=max(1, int(12 * 1200 / a.todspeed)), tod=0, todspeed=a.todspeed, seed=a.seed)
    sheet(frames, 8, 1, a.out / "clock_sheet.png")
    print(a.out / "clock_sheet.png", log.splitlines()[-1])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=Path(os.environ.get("ROOM_SIM_OUT", ROOT / "mocks" / "room_sim")))
    ap.add_argument("--build", type=Path, default=Path(os.environ.get("ROOM_SIM_BUILD", ROOT / "mocks" / "room_sim" / "build")))
    ap.add_argument("--tod", type=float, default=10.0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--helpers", type=int, default=2)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("states").set_defaults(f=cmd_states)
    t = sub.add_parser("task")
    t.add_argument("task", choices=TASKS)
    t.add_argument("--seconds", type=float, default=40)
    t.add_argument("--gif", action="store_true")
    t.add_argument("--react", nargs=2, metavar=("REACT", "PERIOD_S"))
    t.set_defaults(f=cmd_task)
    d = sub.add_parser("day")
    d.add_argument("--seconds", type=float, default=1800)
    d.add_argument("--todspeed", type=float, default=1)
    d.set_defaults(f=cmd_day)
    c = sub.add_parser("clock")
    c.add_argument("--seconds", type=float, default=86400)
    c.add_argument("--todspeed", type=float, default=240)
    c.set_defaults(f=cmd_clock)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    exe = build(a.build)
    a.f(a, exe)
    return 0


if __name__ == "__main__":
    sys.exit(main())
