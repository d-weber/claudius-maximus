"""Host machine metrics (btop-style) for the "System" screen.

Contributes ``state["system"]``. Sampled at ~1 Hz (process table every 4 s). Cheap: ~0.5% of one core.
All numbers are rounded ints unless noted; strings are ASCII. Any key may be missing (firmware must
default). Typical payload ~1.2 KB.

    "system": {
      "host": "Damiens MacBook Air",  # computer name (ASCII)
      "ncpu": 8,
      "cpu": 23,                      # total CPU %, 0-100
      "cores": [12, 40, 3, ...],      # per-core %, in kernel CPU order
      "kinds": "EEEEPPPP",            # per-core type: E=efficiency, P=performance ("" if unknown)
      "hist": [10, 12, ...],          # total CPU % history, 1 Hz, oldest first, up to 60 values
      "load": [2.1, 2.4, 2.6],        # load average 1/5/15 min (1 decimal)
      "mem": {
        "total": 16384,               # MB
        "used": 11200,                # MB = app + wired + compressed (Activity Monitor "Memory Used")
        "app": 6500, "wired": 2600, "comp": 2100,  # MB breakdown of "used"
        "cache": 3900,                # MB cached files (reclaimable)
        "swap": 0, "swap_total": 0,   # MB
        "pressure": 50,               # % memory pressure (100 - kern.memorystatus_level)
        "level": "normal"             # normal | warn | critical
      },
      "mem_hist": [68, 68, ...],      # used/total %, 1 Hz, up to 60 values
      "procs": [                      # top 6 by CPU (ps %cpu, decaying avg), refreshed every 4 s
        {"n": "claude", "c": 12.8, "m": 540, "cl": 1}   # name<=20ch, cpu % (1 dec, can exceed 100), RSS MB,
      ],                                                  # cl=1 if Claude Code or a descendant of it
      "claude": {                     # Claude Code footprint
        "n": 2,                       # claude processes (sessions)
        "cpu": 14.0, "mem": 850,      # claude processes + all their descendants (tools, builds, MCP servers)
        "kids": 7                     # number of descendant processes
      },
      "net": {"rx": 1200, "tx": 40,   # KB/s (physical interfaces, excl. loopback/VPN tunnels)
              "rx_hist": [...], "tx_hist": [...]},  # KB/s, 1 Hz, up to 40 values
      "disk": {"used": 266, "total": 494, "pct": 54},  # GB, data volume (total - free)
      "uptime": 123456,               # seconds since boot
      "batt": {"pct": 100, "plug": 1, "mins": -1},     # omitted on desktops; mins=-1 unknown/charging
      "thermal": "nominal"            # nominal | fair | serious | critical (NSProcessInfo; omitted if unknown)
    }
"""
from __future__ import annotations

import ctypes
import ctypes.util
import logging
import os
import socket
import subprocess
import time
import unicodedata
from collections import deque

from .base import Provider

log = logging.getLogger("companion.system")

try:
    import psutil
except Exception:  # pragma: no cover
    psutil = None

HIST_N = 60
NET_HIST_N = 40
PROC_EVERY_S = 4.0
TOP_N = 6
_SKIP_NIC = ("lo", "utun", "awdl", "llw", "bridge", "gif", "stf", "anpi", "ap", "ipsec", "ppp", "vmenet")

_libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)


def _ascii(s: str) -> str:
    s = s.replace("’", "").replace("'", "")
    return unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode().strip()


def _sysctl_int(name: str) -> int | None:
    for ctype in (ctypes.c_int64, ctypes.c_int32):
        v = ctype(0)
        sz = ctypes.c_size_t(ctypes.sizeof(v))
        if _libc.sysctlbyname(name.encode(), ctypes.byref(v), ctypes.byref(sz), None, ctypes.c_size_t(0)) == 0:
            if sz.value == 4 and ctype is ctypes.c_int64:
                return int(ctypes.c_int32.from_buffer(v).value)
            return int(v.value)
    return None


class _VmStats64(ctypes.Structure):
    _fields_ = [
        ("free_count", ctypes.c_uint32), ("active_count", ctypes.c_uint32),
        ("inactive_count", ctypes.c_uint32), ("wire_count", ctypes.c_uint32),
        ("zero_fill_count", ctypes.c_uint64), ("reactivations", ctypes.c_uint64),
        ("pageins", ctypes.c_uint64), ("pageouts", ctypes.c_uint64), ("faults", ctypes.c_uint64),
        ("cow_faults", ctypes.c_uint64), ("lookups", ctypes.c_uint64), ("hits", ctypes.c_uint64),
        ("purges", ctypes.c_uint64),
        ("purgeable_count", ctypes.c_uint32), ("speculative_count", ctypes.c_uint32),
        ("decompressions", ctypes.c_uint64), ("compressions", ctypes.c_uint64),
        ("swapins", ctypes.c_uint64), ("swapouts", ctypes.c_uint64),
        ("compressor_page_count", ctypes.c_uint32), ("throttled_count", ctypes.c_uint32),
        ("external_page_count", ctypes.c_uint32), ("internal_page_count", ctypes.c_uint32),
        ("total_uncompressed_pages_in_compressor", ctypes.c_uint64),
    ]


class _Mach:
    """host_statistics64(HOST_VM_INFO64) via ctypes -> Activity-Monitor style memory breakdown."""

    def __init__(self):
        self.lib = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
        self.lib.mach_host_self.restype = ctypes.c_uint32
        self.host = self.lib.mach_host_self()
        self.page = _sysctl_int("hw.pagesize") or 16384

    def vm(self) -> _VmStats64 | None:
        st = _VmStats64()
        cnt = ctypes.c_uint32(ctypes.sizeof(st) // 4)
        if self.lib.host_statistics64(self.host, 4, ctypes.byref(st), ctypes.byref(cnt)) != 0:
            return None
        return st


class _Thermal:
    """NSProcessInfo.processInfo.thermalState via the ObjC runtime (no sudo needed)."""
    NAMES = ("nominal", "fair", "serious", "critical")

    def __init__(self):
        ctypes.CDLL("/System/Library/Frameworks/Foundation.framework/Foundation")
        objc = ctypes.CDLL("/usr/lib/libobjc.A.dylib")
        objc.objc_getClass.restype = ctypes.c_void_p
        objc.objc_getClass.argtypes = [ctypes.c_char_p]
        objc.sel_registerName.restype = ctypes.c_void_p
        objc.sel_registerName.argtypes = [ctypes.c_char_p]
        self.send = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(
            ctypes.cast(objc.objc_msgSend, ctypes.c_void_p).value)
        self.pi = self.send(objc.objc_getClass(b"NSProcessInfo"), objc.sel_registerName(b"processInfo"))
        self.sel = objc.sel_registerName(b"thermalState")

    def state(self) -> str | None:
        v = self.send(self.pi, self.sel) or 0
        return self.NAMES[v] if 0 <= v < 4 else None


def _computer_name() -> str:
    try:
        out = subprocess.run(["scutil", "--get", "ComputerName"], capture_output=True, text=True, timeout=2).stdout
        name = _ascii(out)
        if name:
            return name[:24]
    except Exception:
        pass
    return _ascii(socket.gethostname().split(".")[0])[:24]


def _core_kinds(n: int) -> str:
    # Apple Silicon: kernel CPU order lists efficiency cores (perflevel1) first, then performance cores.
    p = _sysctl_int("hw.perflevel0.logicalcpu")
    e = _sysctl_int("hw.perflevel1.logicalcpu")
    if p and e and p + e == n:
        return "E" * e + "P" * p
    return ""


def _proc_name(comm: str) -> str:
    base = comm.rsplit("/", 1)[-1]
    return _ascii(base)[:20] or "?"


class SystemProvider(Provider):
    def __init__(self):
        self.ok = psutil is not None
        if not self.ok:
            log.warning("psutil missing: system metrics disabled (pip install psutil)")
        self.hist: deque[int] = deque(maxlen=HIST_N)
        self.mem_hist: deque[int] = deque(maxlen=HIST_N)
        self.rx_hist: deque[int] = deque(maxlen=NET_HIST_N)
        self.tx_hist: deque[int] = deque(maxlen=NET_HIST_N)
        self.last_sample = 0.0
        self.last_procs = 0.0
        self.data: dict = {}
        self.procs: list = []
        self.claude: dict = {}
        self.host = _computer_name()
        self.ncpu = os.cpu_count() or 1
        self.kinds = _core_kinds(self.ncpu)
        self.node_is_claude: dict[int, bool] = {}
        try:
            self.mach = _Mach()
        except Exception:
            self.mach = None
        try:
            self.thermal = _Thermal()
        except Exception:
            self.thermal = None
        self.net_prev = None
        if self.ok:
            psutil.cpu_percent(percpu=True)
            self.net_prev = (time.time(), self._net_bytes())

    # ---- sampling -------------------------------------------------------------------------------
    def _net_bytes(self) -> tuple[int, int]:
        rx = tx = 0
        for nic, c in psutil.net_io_counters(pernic=True).items():
            if nic.startswith(_SKIP_NIC):
                continue
            rx += c.bytes_recv
            tx += c.bytes_sent
        return rx, tx

    def _memory(self) -> dict:
        mb = 1 << 20
        total = psutil.virtual_memory().total
        m: dict = {"total": round(total / mb)}
        st = self.mach.vm() if self.mach else None
        if st is not None:
            pg = self.mach.page
            app = max(0, st.internal_page_count - st.purgeable_count) * pg
            wired = st.wire_count * pg
            comp = st.compressor_page_count * pg
            cache = (st.external_page_count + st.purgeable_count) * pg
            m.update(used=round((app + wired + comp) / mb), app=round(app / mb), wired=round(wired / mb),
                     comp=round(comp / mb), cache=round(cache / mb))
        else:
            vm = psutil.virtual_memory()
            m.update(used=round((vm.total - vm.available) / mb))
        sw = psutil.swap_memory()
        m["swap"] = round(sw.used / mb)
        m["swap_total"] = round(sw.total / mb)
        lvl = _sysctl_int("kern.memorystatus_level")
        if lvl is not None:
            m["pressure"] = max(0, min(100, 100 - lvl))
        pl = _sysctl_int("kern.memorystatus_vm_pressure_level")
        m["level"] = {1: "normal", 2: "warn", 4: "critical"}.get(pl or 1, "normal")
        return m

    def _is_claude_node(self, pid: int) -> bool:
        if pid not in self.node_is_claude:
            try:
                cmd = " ".join(psutil.Process(pid).cmdline())
                self.node_is_claude[pid] = "claude" in cmd.lower() and ("claude-code" in cmd or "/claude" in cmd)
            except Exception:
                self.node_is_claude[pid] = False
        return self.node_is_claude[pid]

    def _sample_procs(self) -> None:
        out = subprocess.run(["ps", "-A", "-o", "pid=,ppid=,pcpu=,rss=,comm="],
                             capture_output=True, text=True, timeout=3).stdout
        rows = []
        for line in out.splitlines():
            parts = line.split(None, 4)
            if len(parts) < 5:
                continue
            try:
                rows.append((int(parts[0]), int(parts[1]), float(parts[2]), int(parts[3]), parts[4]))
            except ValueError:
                continue
        children: dict[int, list[int]] = {}
        alive = set()
        roots = []
        for pid, ppid, _c, _r, comm in rows:
            alive.add(pid)
            children.setdefault(ppid, []).append(pid)
            base = comm.rsplit("/", 1)[-1]
            if base == "claude" or (base == "node" and self._is_claude_node(pid)):
                roots.append(pid)
        self.node_is_claude = {p: v for p, v in self.node_is_claude.items() if p in alive}
        root_set = set(roots)
        tree = set()
        stack = list(roots)
        while stack:
            p = stack.pop()
            if p in tree:
                continue
            tree.add(p)
            stack += children.get(p, [])
        # nested claude (subagent CLIs launched by claude) count as descendants, not sessions
        n_sessions = sum(1 for p in roots if not any(p in children.get(q, []) for q in root_set))
        ccpu = cmem = 0.0
        procs = []
        for pid, _pp, cpu, rss, comm in rows:
            if pid in tree:
                ccpu += cpu
                cmem += rss
            procs.append((cpu, rss, pid, comm))
        procs.sort(key=lambda r: (-r[0], -r[1]))
        self.procs = [{"n": _proc_name(comm), "c": round(cpu, 1), "m": round(rss / 1024), **({"cl": 1} if pid in tree else {})}
                      for cpu, rss, pid, comm in procs[:TOP_N]]
        self.claude = {"n": n_sessions, "cpu": round(ccpu, 1), "mem": round(cmem / 1024), "kids": len(tree) - len(roots)}

    def _sample(self, now: float) -> None:
        cores = psutil.cpu_percent(percpu=True)
        total = round(sum(cores) / max(1, len(cores)))
        self.hist.append(total)
        d: dict = {"host": self.host, "ncpu": len(cores), "cpu": total, "cores": [round(c) for c in cores],
                   "kinds": self.kinds if len(self.kinds) == len(cores) else "", "hist": list(self.hist)}
        try:
            d["load"] = [round(x, 1) for x in os.getloadavg()]
        except OSError:
            pass
        try:
            d["mem"] = self._memory()
            if d["mem"].get("total"):
                self.mem_hist.append(round(100 * d["mem"]["used"] / d["mem"]["total"]))
                d["mem_hist"] = list(self.mem_hist)
        except Exception:
            log.debug("memory sample failed", exc_info=True)
        try:
            t, (rx, tx) = time.time(), self._net_bytes()
            if self.net_prev:
                pt, (prx, ptx) = self.net_prev
                dt = max(0.2, t - pt)
                self.rx_hist.append(max(0, round((rx - prx) / dt / 1024)))
                self.tx_hist.append(max(0, round((tx - ptx) / dt / 1024)))
            self.net_prev = (t, (rx, tx))
            if self.rx_hist:
                d["net"] = {"rx": self.rx_hist[-1], "tx": self.tx_hist[-1],
                            "rx_hist": list(self.rx_hist), "tx_hist": list(self.tx_hist)}
        except Exception:
            log.debug("net sample failed", exc_info=True)
        try:
            du = psutil.disk_usage("/System/Volumes/Data" if os.path.isdir("/System/Volumes/Data") else "/")
            gb = 1e9
            used = du.total - du.free
            d["disk"] = {"used": round(used / gb), "total": round(du.total / gb), "pct": round(100 * used / du.total)}
        except Exception:
            pass
        try:
            d["uptime"] = int(time.time() - psutil.boot_time())
        except Exception:
            pass
        try:
            b = psutil.sensors_battery()
            if b is not None:
                secs = b.secsleft if isinstance(b.secsleft, int) and b.secsleft >= 0 else -1
                d["batt"] = {"pct": round(b.percent), "plug": int(bool(b.power_plugged)),
                             "mins": secs // 60 if secs >= 0 else -1}
        except Exception:
            pass
        if self.thermal:
            try:
                th = self.thermal.state()
                if th:
                    d["thermal"] = th
            except Exception:
                self.thermal = None
        if now - self.last_procs >= PROC_EVERY_S or not self.procs:
            self.last_procs = now
            try:
                self._sample_procs()
            except Exception:
                log.debug("process sample failed", exc_info=True)
        d["procs"] = self.procs
        if self.claude:
            d["claude"] = self.claude
        self.data = d

    # ---- Provider API ---------------------------------------------------------------------------
    def poll(self, now: float) -> None:
        if not self.ok or now - self.last_sample < 0.95:
            return
        self.last_sample = now
        try:
            self._sample(now)
        except Exception:
            log.exception("system sample failed")

    def contribute(self, state: dict) -> None:
        if self.data:
            state["system"] = self.data


if __name__ == "__main__":  # quick manual check: python -m companion.providers.system
    import json
    sp = SystemProvider()
    for _ in range(3):
        time.sleep(1.0)
        sp.poll(time.time())
    s = json.dumps(sp.data, separators=(",", ":"))
    print(json.dumps(sp.data, indent=1))
    print(len(s), "bytes")
