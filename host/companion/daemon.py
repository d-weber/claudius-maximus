"""Main loop: gather state from providers, stream it to the device, relay device actions.

Robustness rules:
  * never crash on provider / device errors (everything is caught and logged, throttled);
  * the serial port is opened only while holding <root>/.devlock (shared with tools/dev.py). While
    connected the daemon periodically yields the lock for a moment (YIELD_EVERY_S) so dev tools waiting
    on it get a turn; if someone takes it we back off and retry every few seconds;
  * the device is (re)detected every 2 s: unplug/replug/reflash just reconnects; the full state is re-sent
    on every (re)connect and whenever the device says `hello`;
  * hooks arrive on a unix datagram socket and wake the loop immediately (select), so `waiting` reaches the
    device within ~tens of ms; otherwise the loop ticks every TICK_S.

Device -> host actions (`{"t":"action","action":...}`):
    focus   {"sid": "969ca7a9"}  raise the terminal tab/window running that session (runtime_focus.py)
    refresh {}                   re-send the full state now
"""
from __future__ import annotations

import fcntl
import json
import logging
import os
import select
import signal
import sys
import threading
import time
import unicodedata
from pathlib import Path

from .link import DeviceLink

log = logging.getLogger("companion")

ROOT = Path(__file__).resolve().parents[2]
DEVLOCK = ROOT / ".devlock"

HEARTBEAT_S = 2.0
TICK_S = 0.25
RETRY_NO_DEVICE_S = 2.0
RETRY_LOCKED_S = 3.0
YIELD_EVERY_S = float(os.environ.get("CLAUDIUS_YIELD_S", "20"))  # 0 = never yield the device lock
YIELD_FOR_S = 1.2
DEDUPE_S = 5.0
STATUS_EVERY_S = 5.0


def state_dir() -> str:
    return os.environ.get("CLAUDIUS_HOME") or os.path.join(os.path.expanduser("~"), ".claudius-maximus")


def status_path() -> str:
    return os.path.join(state_dir(), "status.json")


# ---------------------------------------------------------------------------------------------
# providers
# ---------------------------------------------------------------------------------------------
def build_providers(demo: bool):
    if demo:
        from .providers.demo import DemoProvider
        out = [DemoProvider()]
        try:
            from .providers.system import SystemProvider
            out.append(SystemProvider())
        except Exception:
            log.exception("provider SystemProvider unavailable")
        return out
    # tell providers/usage.py where the statusLine tee lives (read at import time)
    os.environ.setdefault("CLAUDIUS_RATE_LIMITS", os.path.join(state_dir(), "statusline.json"))
    out = []
    for mod, cls in (("sessions", "SessionsProvider"), ("usage", "UsageProvider"),
                     ("system", "SystemProvider"), ("hooks", "HooksProvider")):
        try:
            m = __import__(f"companion.providers.{mod}", fromlist=[cls])
            out.append(getattr(m, cls)())
        except Exception:
            log.exception("provider %s unavailable", cls)
    return out


def local_tz() -> int:
    return int(time.localtime().tm_gmtoff or 0)


def snapshot(providers) -> dict:
    state: dict = {"t": "state", "ts": int(time.time()), "tz": local_tz()}
    hooks = None
    for p in providers:
        if type(p).__name__ == "HooksProvider":
            hooks = p
        try:
            p.contribute(state)
        except Exception:
            _log_throttled("contribute:" + type(p).__name__, "contribute failed: %s", type(p).__name__)
    if hooks is not None:
        try:
            merge_hooks(state, hooks)
        except Exception:
            _log_throttled("merge", "merge_hooks failed")
    return state


_last_logged: dict[str, float] = {}


def _log_throttled(key: str, msg: str, *args, every: float = 60.0):
    now = time.time()
    if now - _last_logged.get(key, 0) > every:
        _last_logged[key] = now
        log.exception(msg, *args)


# ---------------------------------------------------------------------------------------------
# merge hooks (fast, real-time) into the sessions-provider view (complete, transcript-based)
# ---------------------------------------------------------------------------------------------
def _tilde(p: str) -> str:
    home = os.path.expanduser("~")
    return "~" + p[len(home):] if p and p.startswith(home) else (p or "")


def merge_hooks(state: dict, hooks) -> None:
    """See the MERGE CONTRACT in providers/hooks.py."""
    live = hooks.live()
    sessions = state.setdefault("sessions", [])
    if not isinstance(sessions, list):
        sessions = state["sessions"] = []
    now = time.time()
    seen = set()
    for s in sessions:
        if not isinstance(s, dict):
            continue
        sid = str(s.get("id", ""))[:8]
        h = live.get(sid)
        if not h:
            continue
        seen.add(sid)
        if h["status"] and h["since"] >= (s.get("since") or 0) - 1:
            s["status"] = h["status"]
            s["since"] = h["since"]
        ha = h.get("activity")
        sa = s.get("activity") if isinstance(s.get("activity"), dict) else None
        if ha and (not sa or ha.get("ts", 0) >= (sa.get("ts") or 0)):
            s["activity"] = dict(ha)
        if h.get("ctx_pct") is not None:
            s["ctx_pct"] = h["ctx_pct"]
        if h.get("cost") is not None and h["cost"] >= (s.get("cost") or 0):
            s["cost"] = h["cost"]
        if s.get("status") == "waiting" and h.get("waiting_text"):
            s["waiting"] = h["waiting_text"]
        if h.get("last_prompt") and not s.get("last_prompt"):
            s["last_prompt"] = h["last_prompt"]
        if h.get("subagents"):
            subs = s.get("subagents") if isinstance(s.get("subagents"), list) else []
            by_id = {str(x.get("id", ""))[:8]: x for x in subs if isinstance(x, dict)}
            for hs in h["subagents"]:
                x = by_id.get(hs["id"])
                if x is None:
                    subs.append({k: hs[k] for k in ("id", "desc", "type", "status", "tool", "detail", "started")})
                else:
                    if hs["status"] == "done":
                        x["status"] = "done"
                    if hs.get("tool"):
                        x["tool"], x["detail"] = hs["tool"], hs["detail"]
            s["subagents"] = subs
    # sessions only hooks know about (e.g. sessions provider lagging or unavailable)
    for sid, h in live.items():
        if sid in seen or not h["status"]:
            continue
        if h["status"] == "offline" and now - h["since"] > 600:
            continue
        if h["status"] == "idle" and now - h["last"] > 3600:
            continue
        s = {"id": sid, "name": h["name"], "project": h["name"], "cwd": _tilde(h["cwd"]),
             "status": h["status"], "since": h["since"], "started": h["started"],
             "activity": dict(h["activity"]) if h.get("activity") else {"tool": "", "detail": "", "ts": h["since"]},
             "subagents": [{k: x[k] for k in ("id", "desc", "type", "status", "tool", "detail", "started")}
                           for x in h.get("subagents") or []]}
        for k in ("title", "model", "last_prompt", "ctx_pct", "cost"):
            if h.get(k) is not None:
                s[k] = h[k]
        if h["status"] == "waiting" and h.get("waiting_text"):
            s["waiting"] = h["waiting_text"]
        sessions.append(s)

    def recency(s):
        a = s.get("activity") if isinstance(s.get("activity"), dict) else {}
        return max(s.get("since") or 0, a.get("ts") or 0)
    if live:
        sessions.sort(key=lambda s: (s.get("status") == "offline", -recency(s)) if isinstance(s, dict) else (True, 0))

    totals = state.setdefault("totals", {})
    if isinstance(totals, dict):
        st = [s.get("status") for s in sessions if isinstance(s, dict)]
        totals["sessions"] = sum(1 for x in st if x != "offline")
        for k in ("busy", "waiting", "idle"):
            totals[k] = st.count(k)
        totals["subagents_active"] = sum(
            1 for s in sessions if isinstance(s, dict)
            for x in (s.get("subagents") or []) if isinstance(x, dict) and x.get("status") == "busy")

    # plan limits from the statusLine tee, if the usage provider didn't supply them
    rl = getattr(hooks, "rate_limits", None)
    if isinstance(rl, dict):
        usage = state.setdefault("usage", {})
        if isinstance(usage, dict):
            for key in ("five_hour", "seven_day"):
                w = rl.get(key)
                if key in usage or not isinstance(w, dict) or w.get("used_percentage") is None:
                    continue
                ra = int(w.get("resets_at") or 0)
                if ra and ra < now:
                    continue
                usage[key] = {"pct": int(round(float(w["used_percentage"]))), "resets_at": ra}


# ---------------------------------------------------------------------------------------------
# event dedupe
# ---------------------------------------------------------------------------------------------
class EventFilter:
    def __init__(self):
        self.recent: dict[tuple, float] = {}

    def filter(self, source, events: list, hook_sids: set) -> list:
        from .providers.hooks import HOOK_KINDS
        now = time.time()
        out = []
        is_hooks = type(source).__name__ == "HooksProvider"
        for e in events:
            if not isinstance(e, dict):
                continue
            e.setdefault("t", "event")
            sid = str(e.get("sid", ""))[:8]
            if not is_hooks and sid in hook_sids and e.get("kind") in HOOK_KINDS:
                continue
            key = (e.get("kind"), sid, e.get("tool"), e.get("text"), e.get("agent"))
            if now - self.recent.get(key, 0) < DEDUPE_S:
                continue
            self.recent[key] = now
            out.append(e)
        if len(self.recent) > 500:
            self.recent = {k: v for k, v in self.recent.items() if now - v < DEDUPE_S}
        return out


# ---------------------------------------------------------------------------------------------
# text sanitizing: device fonts are ASCII only
# ---------------------------------------------------------------------------------------------
_TRANS = str.maketrans({"‘": "'", "’": "'", "“": '"', "”": '"', "–": "-", "—": "-",
                        "…": "...", "•": "*", " ": " ", "→": "->", "←": "<-",
                        "✓": "v", "✔": "v", "✗": "x", "×": "x", "\t": " "})


def asciify(o):
    if isinstance(o, str):
        if o.isascii():
            return o
        s = unicodedata.normalize("NFKD", o.translate(_TRANS))
        return s.encode("ascii", "ignore").decode()
    if isinstance(o, dict):
        return {k: asciify(v) for k, v in o.items()}
    if isinstance(o, list):
        return [asciify(v) for v in o]
    return o


# ---------------------------------------------------------------------------------------------
# single instance + status file
# ---------------------------------------------------------------------------------------------
def acquire_instance_lock():
    os.makedirs(state_dir(), mode=0o700, exist_ok=True)
    fd = os.open(os.path.join(state_dir(), "daemon.lock"), os.O_CREAT | os.O_RDWR, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        os.close(fd)
        return None
    os.ftruncate(fd, 0)
    os.write(fd, f"{os.getpid()}\n".encode())
    return fd


def write_status(d: dict):
    try:
        os.makedirs(state_dir(), mode=0o700, exist_ok=True)
        p = status_path()
        tmp = p + ".tmp"
        with open(tmp, "w") as f:
            json.dump(d, f, indent=1)
        os.replace(tmp, p)
    except OSError:
        pass


# ---------------------------------------------------------------------------------------------
# main loop
# ---------------------------------------------------------------------------------------------
class Daemon:
    def __init__(self, demo=False, port=None, dry_run=False, out=None):
        self.demo = demo
        self.dry_run = dry_run
        self.out = out or sys.stdout
        self.providers = build_providers(demo)
        self.hooks = next((p for p in self.providers if type(p).__name__ == "HooksProvider"), None)
        self.link = DeviceLink(port, lock_path=DEVLOCK)
        self.filter = EventFilter()
        self.stop = False
        self.last_body = None
        self.last_sent_at = 0.0
        self.next_connect = 0.0
        self.yield_at = 0.0
        self.fw = None
        self.last_hello = 0
        self.started = int(time.time())
        self.next_status = 0.0
        self.last_state: dict = {}
        self.focus_log: list[str] = []

    # -- output ---------------------------------------------------------------------------
    def emit(self, msg: dict) -> bool:
        msg = asciify(msg)
        if self.dry_run:
            try:
                self.out.write(json.dumps(msg) + "\n")
                self.out.flush()
            except (BrokenPipeError, ValueError):
                self.stop = True
            return True
        return self.link.send(msg)

    def _on_disconnect(self, why: str):
        log.info("device disconnected (%s)", why or "?")
        self.next_connect = time.time() + RETRY_NO_DEVICE_S
        self.next_status = 0

    # -- device ---------------------------------------------------------------------------
    def ensure_connected(self, now: float):
        if self.dry_run:
            return
        if self.link.connected:
            # yield the shared device lock now and then so tools/dev.py users get a turn
            if YIELD_EVERY_S > 0 and now >= self.yield_at:
                self.link.close()
                log.debug("yielding device lock")
                self.next_connect = now + YIELD_FOR_S
            return
        if now < self.next_connect:
            return
        if self.link.connect():
            log.info("device connected on %s", self.link.port)
            self.yield_at = now + YIELD_EVERY_S
            self.last_body = None  # force full state
            self.next_status = 0
            self.link.send({"t": "cmd", "hello": 1})
        else:
            err = self.link.last_error
            if "lock" in err:
                self.next_connect = now + RETRY_LOCKED_S
                log.debug("device busy: %s (%s)", err, self.link.lock_holder())
            else:
                self.next_connect = now + RETRY_NO_DEVICE_S
                if err and err != "no device found":
                    _key = "connect:" + err[:40]
                    if now - _last_logged.get(_key, 0) > 300:
                        _last_logged[_key] = now
                        log.warning("cannot open device: %s", err)

    def handle_device(self, msg: dict):
        t = msg.get("t")
        if t == "hello":
            self.fw = msg.get("fw")
            self.last_hello = int(time.time())
            log.info("device hello fw=%s", self.fw)
            self.last_body = None  # re-send full state on next tick
        elif t == "log":
            log.info("device: %s", str(msg.get("msg"))[:300])
        elif t == "action":
            act = msg.get("action")
            if act == "focus":
                sid = str(msg.get("sid") or "")[:36]
                threading.Thread(target=self._focus, args=(sid,), daemon=True).start()
            elif act == "refresh":
                self.last_body = None
            else:
                log.info("unknown device action %r", act)
        for p in self.providers:
            try:
                p.on_device_message(msg)
            except Exception:
                _log_throttled("devmsg", "on_device_message failed")

    def _focus(self, sid: str):
        try:
            from .runtime_focus import focus_session
            r = focus_session(sid)
        except Exception as e:
            r = f"focus {sid} failed: {e}"
        log.info("%s", r)

    # -- loop -----------------------------------------------------------------------------
    def tick(self):
        now = time.time()
        for p in self.providers:
            try:
                p.poll(now)
            except Exception:
                _log_throttled("poll:" + type(p).__name__, "poll failed: %s", type(p).__name__)
        hook_sids = self.hooks.active_sids(now) if self.hooks else set()
        events = []
        for p in self.providers:
            try:
                events += self.filter.filter(p, p.drain_events(), hook_sids)
            except Exception:
                _log_throttled("drain:" + type(p).__name__, "drain_events failed: %s", type(p).__name__)
        state = snapshot(self.providers)
        self.last_state = state
        try:
            body = json.dumps({k: v for k, v in state.items() if k != "ts"}, sort_keys=True, default=str)
        except Exception:
            _log_throttled("dumps", "state not serializable")
            return

        self.ensure_connected(now)
        was_connected = self.link.connected
        if self.dry_run or was_connected:
            for e in events:
                self.emit(e)
            if body != self.last_body or now - self.last_sent_at > HEARTBEAT_S:
                if self.emit(state):
                    self.last_body, self.last_sent_at = body, now
            for msg in self.link.read_messages():
                self.handle_device(msg)
            if was_connected and not self.link.connected:
                self._on_disconnect(self.link.last_error)
        if not self.dry_run and now >= self.next_status:
            self.next_status = now + STATUS_EVERY_S
            self.write_status(now)

    def write_status(self, now: float):
        sess = [{"id": s.get("id"), "name": s.get("name"), "status": s.get("status")}
                for s in self.last_state.get("sessions", []) if isinstance(s, dict)]
        write_status({
            "pid": os.getpid(), "started": self.started, "updated": int(now), "root": str(ROOT),
            "connected": self.link.connected, "port": self.link.port, "fw": self.fw, "last_hello": self.last_hello,
            "device_error": "" if self.link.connected else self.link.last_error,
            "lock_holder": "" if self.link.connected else self.link.lock_holder(),
            "providers": [type(p).__name__ for p in self.providers],
            "hooks": {"listening": bool(self.hooks and self.hooks.sock), "received": getattr(self.hooks, "received", 0),
                      "last_at": int(getattr(self.hooks, "last_hook_at", 0))},
            "rate_limits_at": getattr(self.hooks, "rate_limits_at", 0),
            "sessions": sess,
        })

    def wait(self):
        fds = []
        for o in (self.hooks, self.link):
            try:
                fd = o.fileno() if o is not None else None
            except Exception:
                fd = None
            if fd is not None:
                fds.append(fd)
        try:
            if fds:
                select.select(fds, [], [], TICK_S)
            else:
                time.sleep(TICK_S)
        except (OSError, ValueError, InterruptedError):
            time.sleep(TICK_S)

    def run(self):
        while not self.stop:
            try:
                self.tick()
            except Exception:
                _log_throttled("tick", "tick failed")
            self.wait()
        self.shutdown()

    def shutdown(self):
        self.link.close()
        if self.hooks:
            self.hooks.close()
        if not self.dry_run:
            write_status({"pid": None, "stopped": int(time.time()), "connected": False})


def run(demo: bool = False, port: str | None = None, dry_run: bool = False, seconds: float = 0):
    lock_fd = None
    if not dry_run:
        lock_fd = acquire_instance_lock()
        if lock_fd is None:
            log.error("another companion daemon is already running (lock %s/daemon.lock)", state_dir())
            return 1
    d = Daemon(demo=demo, port=port, dry_run=dry_run)

    def _stop(*_):
        d.stop = True
    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)
    if seconds:
        threading.Timer(seconds, _stop).start()
    log.info("companion daemon started (pid %d, %s%s)", os.getpid(), "demo, " if demo else "",
             "dry-run" if dry_run else "device")
    d.run()
    log.info("companion daemon stopped")
    if lock_fd is not None:
        os.close(lock_fd)
    return 0
