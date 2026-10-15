#!/usr/bin/env python3
"""Repeatable firmware performance benchmark for Claudius Maximus (owner: perf-bench).

Uses the on-device perf probe ({"t":"cmd","perf":1} -> {"t":"perf",...}, firmware/src/core/perf.cpp) and the demo
provider with a FIXED seed, so two runs on two builds are comparable. Each scenario takes the shared device lock
on its own (others need the device too) and is short (<= ~10 s) except the soak.

  run [--screens a,b] [--scenarios static,wait,stream,burst,nav,alerts,soak] [--soak-seconds 120]
      [--label NAME] [--out DIR] [--flash]
        run the suite, print a table, write <out>/<stamp>-<label>.json (+ .txt)
  show RESULT.json              print the table of a saved run
  compare A.json B.json [--pct 10]
        per-scenario deltas B vs A; green = better, red = worse (beyond --pct % and a small absolute floor)

Scenarios (per screen: home agents feed usage system orbit settings detail):
  static   one fixed demo state with NO waiting session, nothing sent during the window  -> expect ~0 fps
  wait     same fixed state but one session waiting (alerts edge glow / pill)          -> cost of the glow
  stream   steady demo stream (speed 3, like `dev.py perf`), 8 s
  burst    30 events within 1 s (+ a state every 250 ms), then 1.5 s quiet; window covers both
Global:
  nav      demo stream while jumping to each tile in turn; one perf window per transition (1 s each)
  alerts   on home, fixed no-wait state: demo_alert stop (confetti), error, waiting (card)  4 s each
  soak     demo stream for --soak-seconds, cycling all screens twice; heap_int/psram per visit and
           start->end -> leaks show up as a per-screen negative delta between cycle 1 and cycle 2

The device runs whatever was flashed last: `meta.device` records tools/dev.py's last-flash stamp (who/when/tree
hash) and `meta.tree` the hash of the firmware sources in the working tree; `--flash` builds+flashes first.
Run: DEV_NAME=<you> host/.venv/bin/python tools/bench.py run --label mychange
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dev  # noqa: E402  (tools/dev.py: device_lock, open_link, read_perf, DemoProvider, snapshot)

SEED = 7
SPEED = 3.0
SCREENS = ["home", "agents", "feed", "usage", "system", "orbit", "settings", "detail"]
TILES = SCREENS[:-1]
ALL_SCEN = ["static", "wait", "stream", "burst", "nav", "alerts", "soak"]
DEFAULT_OUT = dev.FW / ".pio" / "bench"

# metric -> (label, better: "low" | "high" | None, abs floor for coloring)
METRICS = {
    "fps": ("fps", None, 1.0),
    "frame_ms_avg": ("frm avg", "low", 3.0),
    "frame_ms_max": ("frm max", "low", 10.0),
    "px_per_frame": ("px/frm", "low", 5000),
    "cpu_pct": ("cpu%", "low", 3.0),
    "loop_max_ms": ("loop max", "low", 10.0),
    "lvgl_avg": ("lvgl avg", "low", 1.0),
    "link_avg": ("link avg", "low", 0.5),
    "parse_max": ("parse max", "low", 2.0),
    "heap_int": ("heap KB", "high", 2.0),
}


# ---------------------------------------------------------------------------------------------- helpers
def new_demo() -> dev.DemoProvider:
    d = dev.DemoProvider(seed=SEED, speed=SPEED)
    d._extras = dev.extra_providers()
    return d


def no_wait(d: dev.DemoProvider) -> None:
    for s in d.sessions:
        if s["status"] == "waiting":
            s["status"] = "busy"
            s.pop("waiting", None)


def send_state(link, d) -> None:
    now = time.time()
    for p in d._extras:
        p.poll(now)
    link.send(dev.snapshot([d, *d._extras]))


def drain(link) -> None:
    for _ in link.read_messages():
        pass


def nav(link, d, screen: str) -> None:
    if screen == "detail":
        link.send({"t": "cmd", "nav": "home"})
        link.send({"t": "cmd", "open": "detail", "sid": d.sessions[0]["id"]})
    else:
        link.send({"t": "cmd", "nav": screen})


def quiet(link, seconds: float) -> None:
    t_end = time.time() + seconds
    while time.time() < t_end:
        drain(link)
        time.sleep(0.1)


def settle(link, max_s: float = 10.0, cpu_ok: float = 45.0) -> float:
    """Wait until leftovers of the previous scenario (toasts, confetti, cards, glow fade) are gone:
    1 s perf windows until cpu is low twice in a row or stable (+-5%), at most max_s. Returns seconds waited."""
    t0 = time.time()
    dev.read_perf(link)
    prev = None
    ok = 0
    while time.time() - t0 < max_s:
        quiet(link, 1.0)
        p = dev.read_perf(link) or {}
        cpu = p.get("cpu_pct", 100)
        ok = ok + 1 if (cpu < cpu_ok or (prev is not None and abs(cpu - prev) < 5)) else 0
        prev = cpu
        if ok >= 2:
            break
    return round(time.time() - t0, 1)


def slim(p: dict | None) -> dict:
    """Keep the comparable numbers of a perf reply."""
    if not p:
        return {"error": "no perf reply"}
    out = {k: p.get(k) for k in ("screen", "win_s", "fps", "frame_ms_avg", "frame_ms_max", "px_per_frame", "cpu_pct",
                                 "loop_max_ms", "heap_int", "heap_int_min", "heap_int_largest", "psram_free")}
    for st in ("link", "tick", "lvgl", "flush", "parse"):
        v = p.get(st) or {}
        out[f"{st}_avg"] = v.get("avg_ms")
        out[f"{st}_max"] = v.get("max_ms")
    return out


def stamp_device() -> dict:
    lf = dev.last_flash()
    return {"by": lf.get("by"), "ts": lf.get("ts"), "tree": lf.get("tree")}


class Pinned:
    """--pin FW_DIR: snapshot FW_DIR, build it and flash it ONCE before the run. We never re-flash over a teammate's
    build: if someone flashes mid-run (lastflash.json changed, or the since-boot heap low-water mark went UP = reboot)
    the affected scenarios are marked `foreign_fw` and listed at the end (re-run them with --screens/--scenarios)."""
    src: Path | None = None
    env: dict = {}
    stamp: dict = {}
    last_min: int | None = None

    @classmethod
    def setup(cls, src: Path, label: str) -> int:
        # snapshot the sources: the working tree keeps changing under us
        import shutil
        snap = dev.FW / ".pio" / f"bench-src-{label}"
        shutil.rmtree(snap, ignore_errors=True)
        snap.mkdir(parents=True)
        shutil.copy2(src / "platformio.ini", snap / "platformio.ini")
        for d in ("src", "include", "lib"):
            if (src / d).exists():
                shutil.copytree(src / d, snap / d)
        cls.src = snap
        bdir = dev.FW / ".pio" / f"build-{dev.dev_name()}-bench-{label}"
        cls.env = dict(os.environ, PLATFORMIO_BUILD_DIR=str(bdir),
                       PLATFORMIO_LIBDEPS_DIR=str(dev.FW / ".pio" / "libdeps"))
        cls.stamp = {"by": f"{dev.dev_name()}:bench:{label}", "tree": dev.fw_tree_hash(snap)}
        print(f"[bench] building {src} (tree {cls.stamp['tree']})", file=sys.stderr)
        rc = subprocess.call(["pio", "run", "-s"], cwd=snap, env=cls.env, stdout=subprocess.DEVNULL)
        if rc:
            return rc
        with dev.device_lock("bench:flash"):
            rc = subprocess.call(["pio", "run", "-s", "-t", "upload"], cwd=snap, env=cls.env,
                                 stdout=subprocess.DEVNULL)
            if rc == 0:
                cls.stamp["ts"] = int(time.time())
                try:
                    dev.LASTFLASH.write_text(json.dumps(cls.stamp))
                except OSError:
                    pass
                time.sleep(3.0)
        return rc

    @classmethod
    def foreign(cls) -> bool:
        return bool(cls.src) and dev.last_flash() != cls.stamp


class Session:
    """One scenario = one lock hold."""

    def __init__(self, what: str):
        self.what = what

    def __enter__(self):
        self._cm = dev.device_lock(f"bench:{self.what}")
        self._cm.__enter__()
        self.link = dev.open_link()
        self.dev_stamp = stamp_device()
        drain(self.link)
        return self

    def __exit__(self, *exc):
        try:
            self.link.close()
        finally:
            self._cm.__exit__(*exc)
        return False


# ---------------------------------------------------------------------------------------------- scenarios
def sc_static(screen: str, waiting: bool, seconds: float = 6.0) -> dict:
    with Session(f"{'wait' if waiting else 'static'}:{screen}") as S:
        d = new_demo()
        if not waiting:
            no_wait(d)
        send_state(S.link, d)
        nav(S.link, d, screen)
        st = settle(S.link)         # let transitions / alert fades of the previous scenario die out
        dev.read_perf(S.link)       # reset window
        quiet(S.link, seconds)
        r = slim(dev.read_perf(S.link))
        r["settle_s"] = st
        r["dev"] = S.dev_stamp
        return r


def sc_stream(screen: str, seconds: float = 8.0) -> dict:
    with Session(f"stream:{screen}") as S:
        d = new_demo()
        dev.stream_demo(S.link, 0.5, d)
        nav(S.link, d, screen)
        time.sleep(0.5)
        dev.read_perf(S.link)
        dev.stream_demo(S.link, seconds, d)
        r = slim(dev.read_perf(S.link))
        r["dev"] = S.dev_stamp
        return r


def sc_burst(screen: str, n_events: int = 30, tail: float = 1.5) -> dict:
    with Session(f"burst:{screen}") as S:
        d = new_demo()
        no_wait(d)
        send_state(S.link, d)
        nav(S.link, d, screen)
        st = settle(S.link)
        dev.read_perf(S.link)
        t0 = time.time()
        sent, next_state = 0, t0
        while sent < n_events:
            d.next_change = 0          # force a demo step
            d.poll(time.time())
            for e in d.drain_events():
                if sent >= n_events:
                    break
                S.link.send(e)
                sent += 1
                # pace to n_events per second
                target = t0 + sent / n_events
                if time.time() >= next_state:
                    send_state(S.link, d)
                    next_state += 0.25
                drain(S.link)
                time.sleep(max(0.0, target - time.time()))
        send_state(S.link, d)
        quiet(S.link, tail)
        r = slim(dev.read_perf(S.link))
        r["burst_s"] = round(time.time() - t0, 2)
        r["settle_s"] = st
        r["dev"] = S.dev_stamp
        return r


def sc_nav(per: float = 1.0) -> dict:
    out = {}
    with Session("nav") as S:
        d = new_demo()
        dev.stream_demo(S.link, 0.5, d)
        nav(S.link, d, "settings")
        dev.stream_demo(S.link, 0.5, d)
        for scr in SCREENS:
            dev.read_perf(S.link)
            nav(S.link, d, scr)
            dev.stream_demo(S.link, per, d)
            r = slim(dev.read_perf(S.link))
            r["dev"] = S.dev_stamp
            out[f"nav>{scr}"] = r
    return out


def sc_alerts(seconds: float = 4.0) -> dict:
    out = {}
    for kind in ("stop", "error", "waiting"):  # 'waiting' last: demo card stays up to 30 s
        with Session(f"alert:{kind}") as S:
            d = new_demo()
            no_wait(d)
            send_state(S.link, d)
            nav(S.link, d, "home")
            settle(S.link)
            dev.read_perf(S.link)
            S.link.send({"t": "cmd", "demo_alert": kind})
            quiet(S.link, seconds)
            r = slim(dev.read_perf(S.link))
            r["dev"] = S.dev_stamp
            out[f"alert/{kind}"] = r
    return out


def sc_soak(seconds: float) -> dict:
    visits = []
    with Session("soak") as S:
        d = new_demo()
        dev.stream_demo(S.link, 0.5, d)
        nav(S.link, d, "home")
        dev.stream_demo(S.link, 3.0, d)  # warm-up
        first = slim(dev.read_perf(S.link))
        order = SCREENS * 2
        per = max(2.0, seconds / len(order))
        t0 = time.time()
        for i, scr in enumerate(order):
            nav(S.link, d, scr)
            dev.stream_demo(S.link, per, d)
            p = slim(dev.read_perf(S.link))
            visits.append({"cycle": i // len(SCREENS) + 1, "screen": scr, "t": round(time.time() - t0, 1),
                           "heap_int": p.get("heap_int"), "heap_int_min": p.get("heap_int_min"),
                           "heap_int_largest": p.get("heap_int_largest"), "psram_free": p.get("psram_free"),
                           "cpu_pct": p.get("cpu_pct"), "frame_ms_max": p.get("frame_ms_max"),
                           "loop_max_ms": p.get("loop_max_ms")})
        nav(S.link, d, "home")
        dev.stream_demo(S.link, 3.0, d)
        last = slim(dev.read_perf(S.link))
        dev_stamp = S.dev_stamp
    per_screen = {}
    for scr in SCREENS:
        v = [x for x in visits if x["screen"] == scr]
        if len(v) == 2 and v[0]["heap_int"] is not None and v[1]["heap_int"] is not None:
            per_screen[scr] = {"heap_int_d": v[1]["heap_int"] - v[0]["heap_int"],
                               "psram_d": (v[1]["psram_free"] or 0) - (v[0]["psram_free"] or 0)}
    return {
        "seconds": round(time.time() - t0, 1),
        "heap_int_start": first.get("heap_int"), "heap_int_end": last.get("heap_int"),
        "heap_int_min": min([x["heap_int_min"] for x in visits if x["heap_int_min"]] or [0]),
        "psram_start": first.get("psram_free"), "psram_end": last.get("psram_free"),
        "heap_int_d": (last.get("heap_int") or 0) - (first.get("heap_int") or 0),
        "psram_d": (last.get("psram_free") or 0) - (first.get("psram_free") or 0),
        "per_screen_cycle2_minus_cycle1": per_screen, "visits": visits, "dev": dev_stamp,
    }


# ---------------------------------------------------------------------------------------------- meta
def git(*args: str) -> str:
    try:
        return subprocess.run(["git", *args], cwd=dev.ROOT, capture_output=True, text=True, timeout=20).stdout.strip()
    except Exception:
        return ""


def fw_hello() -> str:
    with Session("hello") as S:
        S.link.send({"t": "cmd", "hello": 1})
        t_end = time.time() + 2
        while time.time() < t_end:
            for m in S.link.read_messages():
                if m.get("t") == "hello":
                    return m.get("fw", "?")
            time.sleep(0.05)
    return "?"


def meta(label: str) -> dict:
    import hashlib
    diff = git("diff", "HEAD", "--", "firmware")
    return {
        "label": label, "time": time.strftime("%Y-%m-%d %H:%M:%S"), "seed": SEED, "speed": SPEED,
        "git_head": git("rev-parse", "--short", "HEAD"),
        "git_diff_stat": git("diff", "HEAD", "--shortstat", "--", "firmware"),
        "git_fw_diff_sha": hashlib.sha1(diff.encode()).hexdigest()[:10],
        "tree": dev.fw_tree_hash(), "device": stamp_device(), "fw": fw_hello(), "by": dev.dev_name(),
    }


# ---------------------------------------------------------------------------------------------- output
def fmt(v, k=None) -> str:
    if v is None:
        return "-"
    if k == "heap_int":
        return f"{v / 1024:.0f}"
    if k == "px_per_frame":
        return f"{v / 1000:.0f}k"
    if isinstance(v, float):
        return f"{v:.1f}"
    return str(v)


def table(res: dict) -> str:
    cols = list(METRICS)
    lines = [f"# bench {res['meta'].get('label')}  {res['meta'].get('time')}  head={res['meta'].get('git_head')} "
             f"tree={res['meta'].get('tree')} device={res['meta'].get('device')}"]
    hdr = f"{'scenario':<18}" + "".join(f"{METRICS[c][0]:>10}" for c in cols)
    lines += [hdr, "-" * len(hdr)]
    for key, r in res["results"].items():
        if "error" in r:
            lines.append(f"{key:<18}  {r['error']}")
            continue
        lines.append(f"{key:<18}" + "".join(f"{fmt(r.get(c), c):>10}" for c in cols)
                     + ("  [rebooted before]" if r.get("rebooted_before") else ""))
    s = res.get("soak")
    if s:
        lines.append("")
        lines.append(f"soak {s['seconds']}s: heap_int {s['heap_int_start']} -> {s['heap_int_end']} "
                     f"({s['heap_int_d']:+d}), min {s['heap_int_min']}; psram {s['psram_start']} -> {s['psram_end']} "
                     f"({s['psram_d']:+d})")
        lines.append("  per-screen heap_int cycle2-cycle1: " + ", ".join(
            f"{k} {v['heap_int_d']:+d}" for k, v in s["per_screen_cycle2_minus_cycle1"].items()))
    return "\n".join(lines)


def save(res: dict, out: Path) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    base = out / f"{time.strftime('%Y%m%d-%H%M%S')}-{res['meta']['label']}"
    base.with_suffix(".json").write_text(json.dumps(res, indent=1))
    base.with_suffix(".txt").write_text(table(res) + "\n")
    return base.with_suffix(".json")


# ---------------------------------------------------------------------------------------------- commands
def cmd_run(a) -> int:
    if a.pin:
        rc = Pinned.setup(Path(a.pin).resolve(), a.label)
        if rc:
            return rc
    elif a.flash:
        rc = dev.cmd_flash(a)
        if rc:
            return rc
    scen = [s.strip() for s in a.scenarios.split(",") if s.strip()]
    screens = [s.strip() for s in a.screens.split(",") if s.strip()]
    res = {"meta": meta(a.label), "results": {}}
    path = None

    last_min = [None]

    def put(k, v):
        nonlocal path
        # heap_int_min is a since-boot low-water mark: if it goes UP, the device rebooted (someone flashed)
        m = v.get("heap_int_min")
        if m is not None and last_min[0] is not None and m > last_min[0] + 512:
            v["rebooted_before"] = True
            res["meta"]["reboots"] = res["meta"].get("reboots", 0) + 1
            print(f"[bench] device rebooted before {k} (someone flashed?)", file=sys.stderr)
        if m is not None:
            last_min[0] = m
        if Pinned.foreign() or (Pinned.src and v.get("rebooted_before")):
            v["foreign_fw"] = True
        res["results"][k] = v
        r = v
        print(f"{k:<18}" + ("".join(f"{fmt(r.get(c), c):>10}" for c in METRICS) if "error" not in r else r["error"]),
              flush=True)
        path = save_partial(res, a, path)

    print(f"{'scenario':<18}" + "".join(f"{METRICS[c][0]:>10}" for c in METRICS), flush=True)
    # scenario-major order: quiet scenarios first so a burst's leftovers can't pollute a static measurement
    for name, fn in (("static", lambda x: sc_static(x, False)), ("wait", lambda x: sc_static(x, True)),
                     ("stream", sc_stream), ("burst", sc_burst)):
        if name in scen:
            for scr in screens:
                put(f"{scr}/{name}", fn(scr))
    if "nav" in scen:
        for k, v in sc_nav().items():
            put(k, v)
    if "alerts" in scen:
        for k, v in sc_alerts().items():
            put(k, v)
    if "soak" in scen:
        res["soak"] = sc_soak(a.soak_seconds)
    stamps = {json.dumps(r.get("dev"), sort_keys=True) for r in res["results"].values() if isinstance(r, dict)}
    if res.get("soak"):
        stamps.add(json.dumps(res["soak"].get("dev"), sort_keys=True))
    res["meta"]["device_changed_mid_run"] = len(stamps) > 1 or bool(res["meta"].get("reboots"))
    if Pinned.src:
        bad = [k for k, v in res["results"].items() if v.get("foreign_fw")]
        res["meta"]["pinned"] = {"src": str(Pinned.src), **Pinned.stamp, "foreign_fw": bad}
        if bad:
            print(f"WARNING: someone else's firmware was on the device for: {','.join(bad)}", file=sys.stderr)
    path = save_partial(res, a, path)
    print()
    print(table(res))
    if res["meta"]["device_changed_mid_run"]:
        print("WARNING: someone flashed the device during this run; results mix builds", file=sys.stderr)
    print(path)
    return 0


def save_partial(res, a, path):
    if path is None:
        return save(res, Path(a.out))
    path.write_text(json.dumps(res, indent=1))
    path.with_suffix(".txt").write_text(table(res) + "\n")
    return path


def cmd_show(a) -> int:
    print(table(json.loads(Path(a.file).read_text())))
    return 0


RED, GREEN, DIM, RST = "\033[31m", "\033[32m", "\033[2m", "\033[0m"


def judge(key: str, metric: str, va, vb, pct: float) -> str:
    """'+' better, '-' worse, '' neutral."""
    if va is None or vb is None:
        return ""
    label, better, floor = METRICS[metric]
    if metric == "heap_int":
        floor *= 1024
    if metric == "fps":
        if not key.endswith(("/static", "/wait")):
            return ""
        better = "low"
    d = vb - va
    if abs(d) < floor or (va and abs(d) / abs(va) * 100 < pct):
        return ""
    good = d < 0 if better == "low" else d > 0
    return "+" if good else "-"


def cmd_compare(a) -> int:
    A = json.loads(Path(a.a).read_text())
    B = json.loads(Path(a.b).read_text())
    color = sys.stdout.isatty() and not a.no_color
    print(f"A: {A['meta'].get('label')} {A['meta'].get('time')} tree={A['meta'].get('tree')} device={A['meta'].get('device')}")
    print(f"B: {B['meta'].get('label')} {B['meta'].get('time')} tree={B['meta'].get('tree')} device={B['meta'].get('device')}")
    cols = list(METRICS)
    w = 14
    hdr = f"{'scenario':<18}" + "".join(f"{METRICS[c][0]:>{w}}" for c in cols)
    print(hdr)
    print("-" * len(hdr))
    nb = nw = 0
    for key in [k for k in A["results"] if k in B["results"]] + [k for k in B["results"] if k not in A["results"]]:
        ra, rb = A["results"].get(key, {}), B["results"].get(key, {})
        row = f"{key:<18}"
        for c in cols:
            va, vb = ra.get(c), rb.get(c)
            cell = f"{fmt(va, c)}>{fmt(vb, c)}"
            j = judge(key, c, va, vb, a.pct)
            mark = {"+": "+", "-": "!", "": " "}[j]
            nb += j == "+"
            nw += j == "-"
            txt = f"{cell}{mark}".rjust(w)
            if color and j:
                txt = (GREEN if j == "+" else RED) + txt + RST
            row += txt
        print(row)
    sa, sb = A.get("soak"), B.get("soak")
    if sa and sb:
        print(f"\nsoak heap_int delta: {sa['heap_int_d']:+d} > {sb['heap_int_d']:+d}   "
              f"heap_int_min: {sa['heap_int_min']} > {sb['heap_int_min']}   psram delta: {sa['psram_d']:+d} > {sb['psram_d']:+d}")
    print(f"\n{nb} better (+, green), {nw} worse (!, red); threshold {a.pct}% and per-metric floor")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--screens", default=",".join(SCREENS))
    r.add_argument("--scenarios", default=",".join(ALL_SCEN))
    r.add_argument("--soak-seconds", type=float, default=120)
    r.add_argument("--label", default=os.environ.get("DEV_NAME", "run"))
    r.add_argument("--out", default=os.environ.get("BENCH_OUT", str(DEFAULT_OUT)))
    r.add_argument("--flash", action="store_true", help="build + flash the working tree first")
    r.add_argument("--pin", metavar="FW_DIR", help="snapshot+build FW_DIR (e.g. firmware, or an extracted old tree), "
                   "flash it once, and mark scenarios measured after someone else flashed")
    r.set_defaults(f=cmd_run)
    s = sub.add_parser("show")
    s.add_argument("file")
    s.set_defaults(f=cmd_show)
    c = sub.add_parser("compare")
    c.add_argument("a")
    c.add_argument("b")
    c.add_argument("--pct", type=float, default=10.0)
    c.add_argument("--no-color", action="store_true")
    c.set_defaults(f=cmd_compare)
    a = ap.parse_args()
    return a.f(a)


if __name__ == "__main__":
    sys.exit(main())
