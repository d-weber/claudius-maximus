"""Main loop: gather state from providers, stream it to the device, relay device actions.

Robustness rules:
  * never crash on provider / device errors (everything is caught and logged, throttled);
  * the serial port is opened only while holding <root>/.devlock (shared with tools/dev.py). While
    connected the daemon periodically yields the lock for a moment (YIELD_EVERY_S) so dev tools waiting
    on it get a turn; if someone takes it we back off and retry every few seconds;
  * the device is (re)detected every 2 s: unplug/replug/reflash just reconnects; the full state is re-sent
    on every (re)connect and whenever the device says `hello`;
  * hooks arrive on a datagram socket (unix on macOS/Linux, loopback UDP on Windows) and wake the loop
    immediately (select), so `waiting` reaches the device within ~tens of ms; otherwise the loop ticks every
    TICK_S. On Windows the serial port has no selectable handle: wait() polls it every WIN_POLL_S instead;
  * the Daemon can run in a non-main thread (the tray app): signals are only installed by run() on the main
    thread, and public_status() / pause_device() / resume_device() may be called from any thread.

Device -> host actions (`{"t":"action","action":...}`):
    focus   {"sid": "969ca7a9"}  raise the terminal tab/window running that session (runtime_focus.py)
    refresh {}                   re-send the full state now
"""
from __future__ import annotations

import copy
import json
import logging
import os
import select
import signal
import sys
import threading
import time
import unicodedata
from contextlib import contextmanager
from pathlib import Path

from . import hostos, locking
from .link import DeviceLink, read_lock_holder

log = logging.getLogger("companion")

ROOT = Path(__file__).resolve().parents[2]
DEVWANT_GLOB = ".devwant.*"   # tools/dev.py drops <root>/.devwant.<pid> while it waits for / holds the device


def dev_dir() -> Path:
    """Where the shared device lock (.devlock) and the .devwant.<pid> markers live: the repo root in a
    checkout (shared with tools/dev.py), the state dir in a frozen app (its bundle dir may be read-only)."""
    if not hostos.is_frozen() and (ROOT / "host" / "companion").is_dir():
        return ROOT
    return Path(state_dir())


DEVLOCK = dev_dir() / ".devlock"


def dev_wants_device(now: float) -> bool:
    """True while a dev tool is waiting for or using the device (fresh marker file from a live pid)."""
    for f in DEVLOCK.parent.glob(DEVWANT_GLOB):
        try:
            pid = int(f.suffix[1:])
            if not hostos.pid_alive(pid):
                raise ProcessLookupError
            if now - f.stat().st_mtime < 900:
                return True
        except (ValueError, ProcessLookupError, FileNotFoundError):
            try:
                f.unlink()
            except OSError:
                pass
        except OSError:
            pass
    return False


@contextmanager
def claim_device(what: str, timeout: float = 30.0):
    """For a separate process that needs the serial port (`claudius-maximus flash`): ask any running daemon to
    step aside (.devwant.<pid> marker, honoured within a tick) and hold the device lock meanwhile, exactly like
    tools/dev.py device_lock. Raises TimeoutError if the lock can't be had within `timeout` seconds."""
    want = DEVLOCK.parent / f".devwant.{os.getpid()}"
    try:
        want.touch()
    except OSError:
        want = None
    fd = None
    try:
        fd = locking.open_lock_file(DEVLOCK)
        t0 = time.time()
        while not locking.try_lock(fd):
            if time.time() - t0 > timeout:
                raise TimeoutError(f"device busy ({read_lock_holder(DEVLOCK) or 'lock held'})")
            time.sleep(0.25)
        locking.write_holder(fd, f"{what} pid={os.getpid()}\n")
        yield
    finally:
        if fd is not None:
            locking.unlock(fd)
            os.close(fd)
        if want is not None:
            try:
                want.unlink()
            except OSError:
                pass

HEARTBEAT_S = 2.0
TICK_S = 0.5            # periodic tick; hook datagrams and device messages wake the loop immediately
MIN_TICK_GAP_S = 0.04   # coalesce bursts of wake-ups (several hooks in a row) into one tick
SYSTEM_HIDDEN_S = 30.0  # partial-capable device not showing the system screen: refresh `system` this often only
MAX_EVENTS_PER_TICK = 12
# counters that tick with every assistant message: a change of only these is sent at most every MINOR_EVERY_S
MINOR_EVERY_S = 5.0
MINOR_KEYS = frozenset(("tokens", "cost", "ctx_pct", "tool_count", "tokens_today", "cost_today", "tools_today",
                        "prompts_today"))
META_KEYS = ("t", "ts", "tz", "part")
RETRY_NO_DEVICE_S = 2.0
RETRY_LOCKED_S = 3.0
YIELD_EVERY_S = float(os.environ.get("CLAUDIUS_YIELD_S", "20"))  # 0 = never yield the device lock
YIELD_FOR_S = 1.2
DEDUPE_S = 5.0
STATUS_EVERY_S = 5.0
WIN_POLL_S = 0.03       # Windows: serial input poll period inside wait() (no select on serial handles)


def state_dir() -> str:
    return hostos.state_dir()


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
    """~-relative display path, same rules as the sessions provider (case-insensitive on Windows)."""
    from .sessions_text import tilde
    return tilde(p or "")


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


def _strip_minor(o):
    if isinstance(o, dict):
        return {k: _strip_minor(v) for k, v in o.items() if k not in MINOR_KEYS}
    if isinstance(o, list):
        return [_strip_minor(v) for v in o]
    return o


def coalesce_events(events: list, limit: int) -> list:
    """A burst of events in one tick: keep every non-`tool` event, and only the most recent `tool` events."""
    keep_tools = max(0, limit - sum(1 for e in events if e.get("kind") != "tool"))
    out = []
    for e in reversed(events):
        if e.get("kind") == "tool":
            if keep_tools <= 0:
                continue
            keep_tools -= 1
        out.append(e)
    out.reverse()
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
def instance_lock_path() -> str:
    return os.path.join(state_dir(), "daemon.lock")


def acquire_instance_lock() -> int | None:
    """Single-instance lock (one daemon per user: headless `run` or the tray app). Returns the fd to keep
    open for the daemon's lifetime (release_instance_lock), or None if another daemon holds it."""
    os.makedirs(state_dir(), mode=0o700, exist_ok=True)
    fd = locking.open_lock_file(instance_lock_path())
    if not locking.try_lock(fd):
        os.close(fd)
        return None
    locking.write_holder(fd, f"{os.getpid()}\n")
    return fd


def release_instance_lock(fd: int | None) -> None:
    if fd is None:
        return
    locking.unlock(fd)
    try:
        os.close(fd)
    except OSError:
        pass


def instance_lock_holder() -> int | None:
    """pid written by the daemon holding daemon.lock (None if unknown)."""
    try:
        return int(read_lock_holder(instance_lock_path()).split()[0])
    except (IndexError, ValueError):
        return None


def read_statusline_cache() -> dict:
    try:
        with open(os.path.join(state_dir(), "statusline.json")) as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


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
        self.last_body = None       # None => next state goes out in full (connect / hello / refresh)
        self.last_parts: dict[str, str] = {}   # top-level key -> json of what the device has
        self.last_sent_at = 0.0
        self.last_system_at = 0.0
        self.last_major: dict[str, str] = {}   # part -> json without MINOR_KEYS, as last sent
        self.part_sent_at: dict[str, float] = {}
        self.dev_caps: set[str] = set()       # from the device hello: "part", "screen"
        self.dev_screen: str | None = None
        self.last_tick = 0.0
        self.next_connect = 0.0
        self.yield_at = 0.0
        self.fw = None
        self.last_hello = 0
        self.board = None           # BOARD_NAME from the device hello
        self.started = int(time.time())
        self.next_status = 0.0
        self.last_state: dict = {}
        self.focus_log: list[str] = []
        self.paused: str | None = None   # pause_device(why): port closed, no reconnects until resume_device()
        self.error = ""             # fatal start error (e.g. another daemon holds the instance lock), see run()
        # Held for a whole tick and by the cross-thread API, so callers see a consistent state and
        # pause_device() returns only once the port is really closed.
        self._lock = threading.RLock()

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

    def _notify_providers(self, msg: dict):
        for p in self.providers:
            try:
                p.on_device_message(msg)
            except Exception:
                _log_throttled("devmsg", "on_device_message failed")

    def _on_disconnect(self, why: str):
        self._notify_providers({"t": "disconnected"})
        log.info("device disconnected (%s)", why or "?")
        self.next_connect = time.time() + RETRY_NO_DEVICE_S
        self.next_status = 0

    # -- device ---------------------------------------------------------------------------
    def ensure_connected(self, now: float):
        if self.dry_run or self.paused is not None:
            return
        if dev_wants_device(now):
            if self.link.connected:
                self.link.close()
                log.debug("dev tool wants the device: stepping aside")
                self._notify_providers({"t": "disconnected"})
            self.next_connect = now + 1.0
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
            self.dev_caps, self.dev_screen = set(), None   # until the device's hello says otherwise
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
            self.board = msg.get("board") or self.board
            self.last_hello = int(time.time())
            caps = msg.get("caps")
            self.dev_caps = {str(c) for c in caps} if isinstance(caps, list) else set()
            self.dev_screen = msg.get("screen") if "screen" in self.dev_caps else None
            log.info("device hello fw=%s", self.fw)
            self.last_body = None  # re-send full state on next tick
        elif t == "screen":
            prev, self.dev_screen = self.dev_screen, msg.get("id")
            log.debug("device screen %s", self.dev_screen)
            if self.dev_screen == "system" and prev != "system":
                self.last_system_at = 0.0   # stream system metrics right away
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
        if len(events) > MAX_EVENTS_PER_TICK:
            events = coalesce_events(events, MAX_EVENTS_PER_TICK)

        self.ensure_connected(now)
        was_connected = self.link.connected
        status_due = not self.dry_run and now >= self.next_status
        if self.dry_run or was_connected or status_due:   # nobody to send to: skip building the state
            self.last_state = state = snapshot(self.providers)
        if self.dry_run or was_connected:
            for e in events:
                self.emit(e)
            self.send_state(state, now)
            for msg in self.link.read_messages():
                self.handle_device(msg)
            if was_connected and not self.link.connected:
                self._on_disconnect(self.link.last_error)
        if status_due:
            self.next_status = now + STATUS_EVERY_S
            self.write_status(now)

    def send_state(self, state: dict, now: float) -> None:
        """Send policy. Full snapshot after (re)connect / hello / refresh, or always for firmware without the
        "part" capability (then only on change + heartbeat, like before). Otherwise only the top-level parts
        whose content changed go out as a partial state; nothing changed => tiny heartbeat every HEARTBEAT_S.
        `system` (1 Hz metrics, only shown on the system screen) is refreshed every SYSTEM_HIDDEN_S while the
        device reports another screen."""
        try:
            parts = {k: json.dumps(v, sort_keys=True, separators=(",", ":"), default=str)
                     for k, v in state.items() if k not in META_KEYS}
        except Exception:
            _log_throttled("dumps", "state not serializable")
            return
        partial_ok = "part" in self.dev_caps and not self.dry_run
        if self.last_body is None or not partial_ok:
            body = "".join(f"{k}={v}\n" for k, v in sorted(parts.items())) + str(state.get("tz"))
            if body != self.last_body or now - self.last_sent_at > HEARTBEAT_S:
                if self.emit(state):
                    self.last_body, self.last_sent_at = body, now
                    self.last_parts, self.last_system_at = parts, now
            return
        changed = [k for k, v in parts.items() if self.last_parts.get(k) != v]
        removed = [k for k in self.last_parts if k not in parts]
        if "system" in changed and self.dev_screen not in (None, "system") \
                and now - self.last_system_at < SYSTEM_HIDDEN_S:
            changed.remove("system")
        majors = {}
        for k in [k for k in changed if k in ("sessions", "totals")]:
            majors[k] = json.dumps(_strip_minor(state[k]), sort_keys=True, separators=(",", ":"), default=str)
            if majors[k] == self.last_major.get(k) and now - self.part_sent_at.get(k, 0) < MINOR_EVERY_S:
                changed.remove(k)
        if not changed and not removed and now - self.last_sent_at <= HEARTBEAT_S:
            return
        msg = {"t": "state", "part": 1, "ts": state.get("ts"), "tz": state.get("tz")}
        for k in changed:
            msg[k] = state[k]
        for k in removed:
            msg[k] = None
        if self.emit(msg):
            self.last_sent_at = now
            for k in changed:
                self.last_parts[k] = parts[k]
                self.part_sent_at[k] = now
                if k in majors:
                    self.last_major[k] = majors[k]
                if k == "system":
                    self.last_system_at = now
            for k in removed:
                self.last_parts.pop(k, None)

    def status_dict(self, now: float | None = None) -> dict:
        """What status.json holds (and public_status() returns). Call with self._lock held."""
        now = time.time() if now is None else now
        sess = [{"id": s.get("id"), "name": s.get("name"), "status": s.get("status")}
                for s in list(self.last_state.get("sessions", []) or []) if isinstance(s, dict)]
        connected = self.link.connected
        if self.paused is not None:
            err = f"paused: {self.paused}"
        elif self.dry_run:
            err = "dry run"
        else:
            err = "" if connected else self.link.last_error
        rl = getattr(self.hooks, "rate_limits", None)
        rl_at = getattr(self.hooks, "rate_limits_at", 0)
        if not isinstance(rl, dict) or not rl:
            cache = read_statusline_cache()
            rl = cache.get("rate_limits") if isinstance(cache.get("rate_limits"), dict) else {}
            rl_at = rl_at or cache.get("rate_limits_at") or 0
        return {
            "pid": os.getpid(), "started": self.started, "updated": int(now), "root": str(ROOT),
            "connected": connected, "port": self.link.port, "fw": self.fw, "board": self.board,
            "last_hello": self.last_hello, "device_error": err,
            "lock_holder": "" if connected or self.paused is not None else self.link.lock_holder(),
            "paused": self.paused is not None, "error": self.error,
            "providers": [type(p).__name__ for p in self.providers],
            "device_caps": sorted(self.dev_caps), "device_screen": self.dev_screen,
            "hooks": {"listening": bool(self.hooks and self.hooks.sock), "received": getattr(self.hooks, "received", 0),
                      "last_at": int(getattr(self.hooks, "last_hook_at", 0)),
                      "address": getattr(self.hooks, "address", "")},
            "rate_limits": copy.deepcopy(rl), "rate_limits_at": rl_at,
            "sessions": sess,
        }

    def write_status(self, now: float):
        write_status(self.status_dict(now))

    # -- API for the tray app / flasher (any thread) ------------------------------------------------
    def public_status(self) -> dict:
        """Same dict as status.json, built now; a copy the caller may keep. Thread-safe (waits for the
        current tick to finish, typically a few ms)."""
        with self._lock:
            return copy.deepcopy(self.status_dict())

    def pause_device(self, why: str) -> None:
        """Close the serial port (and release the device lock) and stop reconnecting until resume_device().
        Returns once the port is closed, so the caller (flasher) can open it right away."""
        with self._lock:
            self.paused = why or "paused"
            if self.link.connected:
                self.link.close()
                self._notify_providers({"t": "disconnected"})
            log.info("device paused: %s", self.paused)
            self.next_status = 0

    def resume_device(self) -> None:
        """Reconnect on the next tick and send the full state (the device may have been reflashed)."""
        with self._lock:
            if self.paused is None:
                return
            log.info("device resumed (was paused: %s)", self.paused)
            self.paused = None
            self.next_connect = 0.0
            self.last_body = None
            self.fw = self.board = None   # until the (possibly new) firmware says hello
            self.dev_caps, self.dev_screen = set(), None
            self.next_status = 0

    def wait(self):
        if sys.platform == "win32":
            return self._wait_windows()
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

    def _wait_windows(self):
        """Windows select() only takes sockets: select on the hooks UDP socket in WIN_POLL_S slices and check
        the serial input queue between slices (cheap ClearCommError call), up to TICK_S in total."""
        deadline = time.time() + TICK_S
        sock = getattr(self.hooks, "sock", None) if self.hooks is not None else None
        while not self.stop:
            ser = self.link.ser
            if ser is not None:
                try:
                    if ser.in_waiting:
                        return
                except Exception:
                    return  # let tick() notice the error
            left = deadline - time.time()
            if left <= 0:
                return
            step = min(left, WIN_POLL_S if ser is not None else TICK_S)
            try:
                if sock is not None:
                    r, _, _ = select.select([sock], [], [], step)
                    if r:
                        return
                else:
                    time.sleep(step)
            except (OSError, ValueError):
                time.sleep(step)

    def run(self):
        while not self.stop:
            gap = self.last_tick + MIN_TICK_GAP_S - time.time()
            if gap > 0:
                time.sleep(gap)
            self.last_tick = time.time()
            try:
                with self._lock:
                    self.tick()
            except Exception:
                _log_throttled("tick", "tick failed")
            self.wait()
        self.shutdown()

    def shutdown(self):
        with self._lock:
            self._shutdown()

    def _shutdown(self):
        self.link.close()
        if self.hooks:
            self.hooks.close()
        if not self.dry_run:
            write_status({"pid": None, "stopped": int(time.time()), "connected": False})


def run(demo: bool = False, port: str | None = None, dry_run: bool = False, seconds: float = 0,
        daemon: Daemon | None = None) -> int:
    """Take the single-instance lock, run the loop until stopped, release. Returns 0, or 1 if another daemon
    is already running (then `daemon.error` says so, for the tray app).

    Headless (`companion run`): called on the main thread, SIGTERM/SIGINT stop it.
    Tray app: build `d = Daemon(...)` on any thread, then call `run(daemon=d)` in a worker thread and set
    `d.stop = True` to end it (no signal handlers are installed off the main thread)."""
    dry_run = daemon.dry_run if daemon is not None else dry_run
    lock_fd = None
    if not dry_run:
        lock_fd = acquire_instance_lock()
        if lock_fd is None:
            holder = instance_lock_holder()
            msg = ("another Claudius Maximus daemon is already running"
                   + (f" (pid {holder})" if holder else "") + f" (lock {instance_lock_path()})")
            log.error("%s", msg)
            if daemon is not None:
                daemon.error = msg
            return 1
    timer = None
    try:
        d = daemon or Daemon(demo=demo, port=port, dry_run=dry_run)

        def _stop(*_):
            d.stop = True
        if threading.current_thread() is threading.main_thread():
            signal.signal(signal.SIGTERM, _stop)
            signal.signal(signal.SIGINT, _stop)
        if seconds:
            timer = threading.Timer(seconds, _stop)
            timer.daemon = True
            timer.start()
        log.info("companion daemon started (pid %d, %s%s)", os.getpid(), "demo, " if d.demo else "",
                 "dry-run" if d.dry_run else "device")
        d.run()
        log.info("companion daemon stopped")
    finally:
        if timer is not None:
            timer.cancel()
        release_instance_lock(lock_fd)
    return 0
