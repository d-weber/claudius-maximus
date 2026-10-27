"""The desktop app: tray icon + menu on the main thread, the companion daemon on a worker thread.

    claudius-maximus app [--no-setup] [--demo] [--port X] [--seconds N] [-v]

* Single instance: the daemon thread starts only while this process holds ~/.claudius-maximus/daemon.lock
  (same lock as `companion run`). If another daemon holds it (e.g. the systemd/launchd service of a script
  install) the app runs as an observer: it shows that daemon's status.json and retries the lock every refresh,
  but cannot flash (it does not own the serial port).
* Every 2 s the tray gets a fresh icon, tooltip and menu built from App.status(). build_menu() and the
  *_line() helpers are pure functions of that dict, so they are unit-tested without a tray.
* Flashing runs on a worker thread: pause the daemon's device link, flasher.flash(), resume.
* The modules owned by other authors (flasher, autostart, hostos settings/open_path, cm_settings) are imported
  lazily and every call is guarded: a missing or failing module degrades one menu entry, never the tray.
"""
from __future__ import annotations

import argparse
import dataclasses
import importlib.util
import json
import logging
import logging.handlers
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

log = logging.getLogger("companion.app")

USAGE_URL = "https://claude.ai/settings/usage"
REFRESH_S = 2.0
NO_HELLO_INSTALL_S = 10.0     # a board port without a hello for this long => offer "Install firmware…"
SLOW_REFRESH_S = 10.0
DAEMON_RETRY_S = 10.0         # observer mode: how often to try to take over the daemon lock
FLASH_ERROR_SHOW_S = 300.0    # a failed flash keeps the badge red this long         # autostart / hooks checks touch files: not every 2 s
BOARD_LABELS = {"4b": "4B", "v4": "V4"}
DEFAULT_BOARD = "4b"
APP_TITLE = "Claudius Maximus"


# ---------------------------------------------------------------------------------------------
# guarded access to the other modules
# ---------------------------------------------------------------------------------------------
def _guard(what: str, fn, *args, default=None, **kw):
    try:
        return fn(*args, **kw)
    except Exception as e:
        _warn_once(what, f"{what} failed: {type(e).__name__}: {e}")
        return default


_warned: dict[str, float] = {}


def _warn_once(key: str, msg: str, every: float = 300.0):
    now = time.time()
    if now - _warned.get(key, 0) > every:
        _warned[key] = now
        log.warning("%s", msg)


def _state_dir() -> str:
    return os.environ.get("CLAUDIUS_HOME") or os.path.join(os.path.expanduser("~"), ".claudius-maximus")


def _fallback_settings_path() -> str:
    return os.path.join(_state_dir(), "settings.json")


def settings_get(key: str, default=None):
    """hostos.settings_get (or hostos.settings.get); falls back to reading settings.json directly."""
    try:
        from . import hostos
        fn = getattr(hostos, "settings_get", None) or hostos.settings.get
        return fn(key, default)
    except Exception:
        try:
            with open(_fallback_settings_path()) as f:
                return json.load(f).get(key, default)
        except (OSError, ValueError, AttributeError):
            return default


def settings_set(key: str, value) -> None:
    try:
        from . import hostos
        fn = getattr(hostos, "settings_set", None) or hostos.settings.set
        fn(key, value)
        return
    except Exception:
        pass
    p = _fallback_settings_path()
    try:
        try:
            with open(p) as f:
                d = json.load(f)
        except (OSError, ValueError):
            d = {}
        d[key] = value
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p + ".tmp", "w") as f:
            json.dump(d, f, indent=1)
        os.replace(p + ".tmp", p)
    except OSError as e:
        log.warning("cannot save setting %s: %s", key, e)


def log_file() -> str:
    try:
        from .hostos import log_path
        return log_path()
    except Exception:
        return os.path.join(_state_dir(), "claudius-maximus.log")


def open_path(target: str) -> None:
    """hostos.open_path, else the platform opener."""
    try:
        from .hostos import open_path as op
        op(target)
        return
    except Exception as e:
        log.debug("hostos.open_path unavailable (%s); using the platform opener", e)
    if sys.platform == "win32":
        os.startfile(target)  # noqa: S606 (Windows only)
    elif sys.platform == "darwin":
        subprocess.Popen(["open", target])
    else:
        subprocess.Popen(["xdg-open", target], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def load_cm_settings():
    """The hooks installer: companion.hooksetup (host/hooks/cm_settings.py is a shim over it). Older trees:
    hooks.cm_settings from a source checkout (cwd host/), else loaded by path next to the companion package."""
    try:
        from . import hooksetup
        return hooksetup
    except Exception:
        pass
    try:
        from hooks import cm_settings  # type: ignore
        return cm_settings
    except Exception:
        pass
    p = Path(__file__).resolve().parents[1] / "hooks" / "cm_settings.py"
    spec = importlib.util.spec_from_file_location("cm_settings", p)
    if spec is None or spec.loader is None:
        raise ImportError(f"cm_settings not found at {p}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _quiet(fn, *a, **kw):
    """cm_settings prints progress for its CLI; keep it out of a windowed app's (possibly absent) stdout."""
    import contextlib
    import io
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        r = fn(*a, **kw)
    out = buf.getvalue().strip()
    if out:
        log.info("%s", out)
    return r


def _port_dict(p) -> dict:
    if isinstance(p, dict):
        return p
    if dataclasses.is_dataclass(p):
        return dataclasses.asdict(p)
    return {k: getattr(p, k, None) for k in ("device", "kind", "vid", "pid", "serial", "description")}


def board_id(name: str | None) -> str | None:
    """Device hello `board` (its BOARD_NAME, e.g. "ESP32-S3-Touch-LCD-4B") -> "4b" | "v4"."""
    if not name:
        return None
    if name.lower() in BOARD_LABELS:
        return name.lower()
    try:
        from . import flasher
        fn = getattr(flasher, "board_id", None)
        if fn:
            r = fn(name)
            if r:
                return r
        boards = getattr(flasher, "BOARDS", {}) or {}
        for k, v in boards.items():
            if k == name and isinstance(v, str) and v.lower() in BOARD_LABELS:
                return v.lower()
            vals = v.values() if isinstance(v, dict) else (v if isinstance(v, (list, tuple)) else [v])
            if k.lower() in BOARD_LABELS and name in [str(x) for x in vals]:
                return k.lower()
            if k.lower() in BOARD_LABELS and name == getattr(v, "name", None):
                return k.lower()
    except Exception:
        pass
    n = name.upper().replace("_", "-")
    if n.endswith("4B"):
        return "4b"
    if n.endswith("V4") or n.endswith("LCD-4"):
        return "v4"
    return None


def board_label(name_or_id: str | None) -> str:
    bid = board_id(name_or_id)
    return BOARD_LABELS.get(bid, name_or_id or "?")


def _pid_alive(pid) -> bool:
    try:
        from .hostos import pid_alive
        return bool(pid_alive(pid))
    except Exception:
        try:
            os.kill(int(pid), 0)
            return True
        except (OSError, TypeError, ValueError):
            return False


def installer_handles_autostart() -> bool:
    """Windows installer (Inno Setup) build: its "Start at login" task owns the HKCU Run value, so the first
    run must not enable autostart behind the user's choice. Detected by unins000.exe next to the exe."""
    if not getattr(sys, "frozen", False):
        return False
    return os.path.exists(os.path.join(os.path.dirname(sys.executable), "unins000.exe"))


# ---------------------------------------------------------------------------------------------
# status -> text and menu (pure)
# ---------------------------------------------------------------------------------------------
def fmt_reset(ts, now: float | None = None) -> str:
    """Epoch seconds -> 'resets in 2h 05m' (or the weekday when more than a day away), like claude_tray."""
    if not isinstance(ts, (int, float)) or ts <= 0:
        return ""
    now = time.time() if now is None else now
    secs = max(0, int(ts - now))
    h, m = divmod(secs // 60, 60)
    if h >= 24:
        return "resets " + time.strftime("%a %H:%M", time.localtime(ts))
    return f"resets in {h}h {m:02d}m" if h else f"resets in {m}m"


def device_line(st: dict) -> str:
    fl = st.get("flash") or {}
    if fl.get("active"):
        return f"Flashing firmware: {fl.get('pct', 0)}%" + (f" ({fl['msg']})" if fl.get("msg") else "")
    if st.get("connected"):
        port = st.get("port") or "?"
        if st.get("fw"):
            b = st.get("board")
            return f"Device: connected on {port} · fw {st['fw']}" + (f" ({board_label(b)})" if b else "")
        return f"Device: connected on {port} · waiting for hello"
    ports = st.get("ports") or []
    if any(p.get("kind") == "native" for p in ports):
        native = next(p for p in ports if p.get("kind") == "native")
        err = st.get("device_error") or ""
        if "lock" in err:
            holder = st.get("lock_holder")
            return f"Device: busy on {native.get('device')}" + (f" ({holder})" if holder else "")
        return f"Device: found on {native.get('device')}, not answering"
    if any(p.get("kind") == "uart" for p in ports):
        return "Board on USB-TO-UART port: move the cable to USB-OTG"
    return "Device: not found"


def sessions_line(st: dict) -> str:
    ss = [s for s in st.get("sessions") or [] if isinstance(s, dict) and s.get("status") != "offline"]
    if not ss:
        return "Sessions: none"
    busy = sum(1 for s in ss if s.get("status") == "busy")
    waiting = sum(1 for s in ss if s.get("status") == "waiting")
    parts = []
    if busy:
        parts.append(f"{busy} working")
    if waiting:
        parts.append(f"{waiting} needs you" if waiting == 1 else f"{waiting} need you")
    if not parts:
        parts.append("idle")
    return f"Sessions: {len(ss)} ({', '.join(parts)})"


def _pct(rl: dict, key: str):
    v = (rl or {}).get(key)
    if isinstance(v, dict) and isinstance(v.get("used_percentage"), (int, float)):
        return v["used_percentage"]
    return None


def plan_line(st: dict) -> str | None:
    rl = st.get("rate_limits") or {}
    five, week = _pct(rl, "five_hour"), _pct(rl, "seven_day")
    if five is None and week is None:
        return None
    parts = []
    if five is not None:
        parts.append(f"5h {five:.0f}%")
    if week is not None:
        parts.append(f"7d {week:.0f}%")
    return "Plan: " + " · ".join(parts)


def tooltip_lines(st: dict, now: float | None = None) -> list[str]:
    lines = [device_line(st), sessions_line(st)]
    rl = st.get("rate_limits") or {}
    for key, label in (("five_hour", "5-hour"), ("seven_day", "Weekly")):
        p = _pct(rl, key)
        if p is not None:
            r = fmt_reset((rl.get(key) or {}).get("resets_at"), now)
            lines.append(f"{label}: {p:.0f}%" + (f"  ·  {r}" if r else ""))
    upd = st.get("update")
    if isinstance(upd, (list, tuple)) and upd and upd[0] and not (st.get("flash") or {}).get("active"):
        lines.append(f"Firmware update available ({upd[1].replace('->', '→')})")
    if st.get("observer"):
        lines.append("Another daemon owns the device (status only)")
    elif st.get("connected") is False and any(p.get("kind") == "uart" for p in st.get("ports") or []) \
            and not any(p.get("kind") == "native" for p in st.get("ports") or []):
        lines.append("Flashing works on this port; the host link needs USB-OTG")
    if st.get("daemon_error"):
        lines.append(f"⚠ {st['daemon_error']}")
    return lines


def tray_state(st: dict) -> str:
    """Badge colour: busy (flashing) / error / update (also: install needed) / ok / none."""
    if (st.get("flash") or {}).get("active"):
        return "busy"
    fl = st.get("flash") or {}
    if st.get("daemon_error") or (fl.get("error") and time.time() - fl.get("error_at", 0) < FLASH_ERROR_SHOW_S):
        return "error"
    upd = st.get("update")
    if st.get("connected") and st.get("fw"):
        return "update" if isinstance(upd, (list, tuple)) and upd and upd[0] else "ok"
    if st.get("connected") or st.get("install_needed"):
        return "update" if st.get("install_needed") else "ok"
    if any(p.get("kind") in ("native", "uart") for p in st.get("ports") or []):
        return "update"
    return "none"


def build_menu(st: dict, app) -> list:
    """Menu, top to bottom, per docs/DESKTOP.md. `app` provides flash(board), set_autostart(on),
    install_hooks(), open_log(), open_usage_page(), quit()."""
    from .tray.base import MenuItem
    items = [MenuItem(device_line(st), enabled=False), MenuItem(sessions_line(st), enabled=False)]
    pl = plan_line(st)
    if pl:
        items.append(MenuItem(pl, enabled=False))
    items.append(MenuItem.sep())

    fl = st.get("flash") or {}
    upd = st.get("update")
    can_flash = not st.get("observer")
    dev_board = board_id(st.get("board"))
    chosen = st.get("board_choice") or DEFAULT_BOARD
    if fl.get("active"):
        items.append(MenuItem(f"Flashing firmware… {fl.get('pct', 0)}%", enabled=False))
    elif st.get("connected") and st.get("fw") and isinstance(upd, (list, tuple)) and upd and upd[0]:
        b = dev_board or chosen
        items.append(MenuItem(f"Update firmware ({upd[1].replace('->', '→')})…",
                              action=lambda: app.flash(b), enabled=can_flash))
    elif st.get("install_needed"):
        items.append(MenuItem("Install firmware…", action=lambda: app.flash(chosen), enabled=can_flash))
    else:
        bundled = st.get("bundled")
        subs = []
        for bid, label in BOARD_LABELS.items():
            ok = can_flash and (bundled is None or bid in bundled)
            subs.append(MenuItem(label, action=(lambda b=bid: app.flash(b)), enabled=ok,
                                 checked=(bid == (dev_board or chosen))))
        items.append(MenuItem("Flash firmware", children=subs, enabled=can_flash))

    items.append(MenuItem("Start at login", action=lambda: app.set_autostart(not st.get("autostart")),
                          checked=bool(st.get("autostart"))))
    if st.get("hooks_installed"):
        items.append(MenuItem("Claude Code hooks: installed", enabled=False))
    else:
        items.append(MenuItem("Install Claude Code hooks", action=app.install_hooks))
    items.append(MenuItem("Open log", action=app.open_log))
    items.append(MenuItem("Open claude.ai usage", action=app.open_usage_page))
    items.append(MenuItem.sep())
    items.append(MenuItem("Quit", action=app.quit))
    return items


# ---------------------------------------------------------------------------------------------
# the app
# ---------------------------------------------------------------------------------------------
class App:
    def __init__(self, argv_opts=None, backend=None, start_daemon: bool = True):
        self.opts = argv_opts if argv_opts is not None else argparse.Namespace()
        self.backend = backend
        self.daemon = None
        self.daemon_thread: threading.Thread | None = None
        self.daemon_error = ""
        self.observing = ""          # d.error from daemon.run() when another daemon holds the lock
        self._next_daemon_try = 0.0
        self.flash_state: dict = {"active": False, "pct": 0, "msg": "", "error": ""}
        self._flash_lock = threading.Lock()
        self._quit = threading.Event()
        self._refresh_lock = threading.Lock()
        self._last_status: dict = {}
        self._slow: dict = {}
        self._slow_at = 0.0
        self._nohello_since: float | None = None
        self.board = settings_get("board", DEFAULT_BOARD) or DEFAULT_BOARD
        if start_daemon:
            self._start_daemon()
        if not getattr(self.opts, "no_setup", False):
            threading.Thread(target=self._first_run, name="first-run", daemon=True).start()

    # -- daemon ----------------------------------------------------------------------------------
    def _start_daemon(self) -> bool:
        """daemon.run(daemon=d) on a worker thread: it takes the single-instance lock itself and installs no
        signal handlers off the main thread. If another daemon holds the lock, run() returns 1 with d.error
        set: the app then observes that daemon and retries (only once the holder's pid is gone, so the log
        doesn't fill with lock errors)."""
        if self.daemon is not None or self._quit.is_set():
            return self.daemon is not None
        now = time.time()
        if now < self._next_daemon_try:
            return False
        self._next_daemon_try = now + DAEMON_RETRY_S
        try:
            from . import daemon as dm
        except Exception as e:
            self.daemon_error = f"daemon unavailable: {e}"
            log.exception("cannot import the daemon")
            return False
        holder = _guard("instance_lock_holder", getattr(dm, "instance_lock_holder", lambda: None))
        if holder and holder != os.getpid() and self.observing and _pid_alive(holder):
            return False
        try:
            d = dm.Daemon(demo=getattr(self.opts, "demo", False), port=getattr(self.opts, "port", None))
        except Exception as e:
            self.daemon_error = f"daemon failed to start: {e}"
            log.exception("cannot create the daemon")
            return False
        self.daemon, self.daemon_error = d, ""

        def main():
            try:
                rc = dm.run(daemon=d)
            except Exception as e:
                rc = -1
                self.daemon_error = f"daemon stopped: {e}"
                log.exception("daemon thread crashed")
            if self.daemon is d:
                self.daemon = None
            if rc == 1:
                self.observing = getattr(d, "error", "") or "another Claudius Maximus daemon is running"
                _warn_once("observe", f"{self.observing}: showing its status, will take over when it stops")
            elif rc == 0 and not self._quit.is_set():
                self.daemon_error = getattr(d, "error", "") or "daemon stopped"
        self.observing = ""
        self.daemon_thread = threading.Thread(target=main, name="daemon", daemon=True)
        self.daemon_thread.start()
        return True

    def _daemon_status(self) -> dict:
        d = self.daemon
        if d is not None:
            fn = getattr(d, "public_status", None)
            if fn is not None:
                st = _guard("Daemon.public_status", fn)
                if isinstance(st, dict):
                    return dict(st)
        # observer mode, or a daemon without public_status yet: read what it writes every few seconds
        try:
            from .daemon import status_path
            p = status_path()
        except Exception:
            p = os.path.join(_state_dir(), "status.json")
        try:
            with open(p) as f:
                st = json.load(f)
        except (OSError, ValueError):
            st = {}
        if d is None:
            fresh = bool(st.get("pid")) and st.get("pid") != os.getpid() and time.time() - st.get("updated", 0) < 30
            st["observer"] = fresh or bool(self.observing)
            st["observer_msg"] = self.observing
            if not fresh:
                st["connected"] = False
        return st

    # -- status ----------------------------------------------------------------------------------
    def status(self) -> dict:
        if self.daemon is None:
            self._start_daemon()
        st = self._daemon_status()
        if not st.get("rate_limits"):
            try:
                with open(os.path.join(_state_dir(), "statusline.json")) as f:
                    st["rate_limits"] = json.load(f).get("rate_limits") or {}
            except (OSError, ValueError):
                st["rate_limits"] = {}
        try:
            from . import flasher
            ports = _guard("flasher.detect", flasher.detect, default=[]) or []
            st["ports"] = [_port_dict(p) for p in ports]
            if st.get("fw"):
                st["update"] = _guard("flasher.needs_update", flasher.needs_update, st.get("fw"),
                                      st.get("board"), default=(False, ""))
            else:
                st["update"] = (False, "")
            b = _guard("flasher.bundled", flasher.bundled, default=None)
            st["bundled"] = sorted(b) if isinstance(b, dict) else None
        except Exception as e:
            _warn_once("flasher", f"flasher unavailable: {e}")
            st.setdefault("ports", [])
            st["update"] = (False, "")
            st["bundled"] = None
        now = time.time()
        board_port = any(p.get("kind") in ("native", "uart") for p in st["ports"])
        if board_port and not st.get("fw") and not self.flash_state["active"]:
            self._nohello_since = self._nohello_since or now
        else:
            self._nohello_since = None
        st["install_needed"] = bool(self._nohello_since and now - self._nohello_since >= NO_HELLO_INSTALL_S)
        if now - self._slow_at > SLOW_REFRESH_S:
            self._slow_at = now
            self._slow = {"autostart": self.autostart(), "hooks_installed": self.hooks_installed()}
        st.update(self._slow)
        st["flash"] = dict(self.flash_state)
        st["board_choice"] = self.board
        if self.daemon_error:
            st["daemon_error"] = self.daemon_error
        st["tray_state"] = tray_state(st)
        self._last_status = st
        return st

    # -- tray ------------------------------------------------------------------------------------
    def refresh(self, st: dict | None = None):
        if self.backend is None:
            return
        with self._refresh_lock:
            st = self.status() if st is None else st
            from .tray.icon import render_tray_icon
            self.backend.set_icon_renderer(lambda size: render_tray_icon(size, st))
            self.backend.set_tooltip(APP_TITLE, tooltip_lines(st))
            self.backend.set_menu(build_menu(st, self))

    def _refresh_cached(self):
        """During flashing: re-render from the last status with the new progress (no port enumeration)."""
        st = dict(self._last_status)
        st["flash"] = dict(self.flash_state)
        st["tray_state"] = tray_state(st)
        _guard("tray refresh", self.refresh, st)

    def _refresh_loop(self):
        while not self._quit.is_set():
            _guard("tray refresh", self.refresh)
            self._quit.wait(REFRESH_S)

    def on_ready(self):
        threading.Thread(target=self._refresh_loop, name="tray-refresh", daemon=True).start()

    def notify(self, title: str, body: str):
        log.info("notify: %s: %s", title, body)
        if self.backend is not None:
            _guard("notify", self.backend.notify, title, body)

    # -- first run -------------------------------------------------------------------------------
    def _first_run(self):
        """Once: hooks + statusLine wrapper (hooksetup makes the timestamped settings.json backup) and start at
        login. autostart.enable() also retires the old headless systemd/launchd daemon of a script install,
        whose lock the app then takes over within DAEMON_RETRY_S."""
        if settings_get("first_run_done", False):
            return
        done = []
        if self.install_hooks(notify=False):
            done.append("Claude Code hooks installed")
        if installer_handles_autostart():
            log.info("first run: the installer manages start at login; not touching it")
        else:
            note = self.set_autostart(True, notify=False)
            if note:
                done.append(note if isinstance(note, str) else "starts at login")
        self.notify("Claudius Maximus is running",
                    (". ".join(d.rstrip(".") for d in done) + ". " if done else "") + "It lives in the system tray.")
        settings_set("first_run_done", True)

    # -- actions ---------------------------------------------------------------------------------
    def install_hooks(self, notify: bool = True) -> bool:
        try:
            cm = load_cm_settings()
            fn = getattr(cm, "install_for_app", None)
            if fn is None:
                raise AttributeError("cm_settings.install_for_app missing")
            rc = _quiet(fn)
            ok = rc in (0, None, True)
        except Exception as e:
            log.warning("installing the Claude Code hooks failed: %s", e)
            ok = False
        self._slow_at = 0
        if notify:
            self.notify("Claude Code hooks", "Installed: new Claude Code sessions report to the device."
                        if ok else "Could not install the hooks; see the log.")
        return ok

    def hooks_installed(self) -> bool:
        try:
            cm = load_cm_settings()
            return _quiet(cm.check) == 0
        except Exception as e:
            _warn_once("hooks-check", f"cm_settings.check failed: {e}")
            return False

    def autostart(self) -> bool:
        try:
            from . import autostart
            return bool(autostart.is_enabled())
        except Exception as e:
            _warn_once("autostart", f"autostart.is_enabled failed: {e}")
            return False

    def set_autostart(self, on: bool, notify: bool = True):
        """autostart.enable() returns a sentence for the notification (e.g. that the old service was
        disabled). Returns that sentence (or True) on success, False on failure."""
        try:
            from . import autostart
            r = (autostart.enable if on else autostart.disable)()
            result = r if isinstance(r, str) and r else True
        except Exception as e:
            log.warning("autostart %s failed: %s", "enable" if on else "disable", e)
            result = False
            if notify:
                self.notify("Start at login", f"Could not change it: {e}")
        else:
            if notify and isinstance(result, str):
                self.notify("Start at login", result)
        self._slow_at = 0
        self._next_daemon_try = 0.0   # enable() may have stopped the old daemon: try to take over now
        self.refresh_soon()
        return result

    def refresh_soon(self):
        if self.backend is not None and not self._quit.is_set():
            threading.Thread(target=_guard, args=("tray refresh", self.refresh), daemon=True).start()

    def open_log(self):
        _guard("open log", open_path, log_file())

    def open_usage_page(self):
        try:
            import webbrowser
            if webbrowser.open(USAGE_URL):
                return
        except Exception:
            pass
        _guard("open usage page", open_path, USAGE_URL)

    def flash(self, board: str | None = None):
        board = (board or self.board or DEFAULT_BOARD).lower()
        with self._flash_lock:
            if self.flash_state["active"]:
                self.notify("Firmware", "Already flashing.")
                return
            self.flash_state = {"active": True, "pct": 0, "msg": "starting", "error": "", "board": board}
        if board != self.board:
            self.board = board
            settings_set("board", board)
        threading.Thread(target=self._flash_worker, args=(board,), name="flash", daemon=True).start()

    def _flash_worker(self, board: str):
        label = BOARD_LABELS.get(board, board)
        d = self.daemon
        if d is None:
            self.flash_state = {"active": False, "pct": 0, "msg": "", "error": "device owned by another daemon",
                                "error_at": time.time()}
            self.notify("Cannot flash", "Another Claudius Maximus daemon (the background service?) owns the "
                        "device. Stop it, then try again.")
            self._refresh_cached()
            return
        ports = (self._last_status or {}).get("ports") or []
        uart = any(p.get("kind") == "uart" for p in ports) and not any(p.get("kind") == "native" for p in ports)
        self.notify("Flashing firmware", f"Writing the {label} firmware" + (" over the USB-TO-UART port" if uart
                    else "") + ". Don't unplug the device.")
        self._refresh_cached()
        last = [-1, 0.0]

        def progress(pct, msg=""):
            pct = max(0, min(100, int(pct)))
            self.flash_state.update(pct=pct, msg=str(msg or "")[:60])
            now = time.time()
            if pct != last[0] and (now - last[1] > 0.5 or pct == 100):
                last[0], last[1] = pct, now
                self._refresh_cached()

        paused = False
        try:
            from . import flasher
            d.pause_device("flashing")
            paused = True
            res = flasher.flash(board, progress=progress, log=lambda m: log.info("flash: %s", m))
            fw = getattr(res, "fw_version", None) or ""
            self.flash_state = {"active": False, "pct": 100, "msg": "", "error": ""}
            body = f"{label} firmware {fw} written; the device is restarting.".replace("  ", " ")
            if getattr(res, "link_port", "") is None:
                body += (" Move the cable to the USB-OTG port: the host link uses the native USB."
                         if uart else " The device did not come back on USB yet; replug it if it stays off.")
            self.notify("Firmware installed", body)
        except Exception as e:
            log.exception("flashing failed")
            self.flash_state = {"active": False, "pct": 0, "msg": "", "error": str(e), "error_at": time.time()}
            self.notify("Flashing failed", str(e) or type(e).__name__)
        finally:
            if paused:
                _guard("Daemon.resume_device", d.resume_device)
            self._nohello_since = None
            _guard("tray refresh", self.refresh)

    def quit(self):
        if self._quit.is_set():
            return
        self._quit.set()
        log.info("quitting")
        d = self.daemon
        if d is not None:
            d.stop = True
            if self.daemon_thread is not None and self.daemon_thread is not threading.current_thread():
                self.daemon_thread.join(timeout=5)
        if self.backend is not None:
            _guard("tray stop", self.backend.stop)


def setup_logging(verbose: bool = False):
    """Same rotating log file as the daemon (hostos.log_path()); stderr too when it is a terminal."""
    root = logging.getLogger()
    root.setLevel(logging.DEBUG if verbose else logging.INFO)
    fmt = logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s")
    path = log_file()
    if not any(isinstance(h, logging.handlers.RotatingFileHandler) for h in root.handlers):
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            h = logging.handlers.RotatingFileHandler(path, maxBytes=1_000_000, backupCount=3)
            h.setFormatter(fmt)
            root.addHandler(h)
        except OSError as e:
            print(f"cannot open log file {path}: {e}", file=sys.stderr)
    if sys.stderr is not None and sys.stderr.isatty():
        h = logging.StreamHandler(sys.stderr)
        h.setFormatter(fmt)
        root.addHandler(h)


def parse_args(argv):
    argv = list(argv or [])
    if argv and argv[0] == "app":
        argv = argv[1:]
    ap = argparse.ArgumentParser(prog="claudius-maximus app", description="Claudius Maximus tray app")
    ap.add_argument("--no-setup", action="store_true", help="skip the first-run setup (hooks, autostart)")
    ap.add_argument("--demo", action="store_true", help="stream fake data instead of real sessions")
    ap.add_argument("--port", help="serial port (auto-detected by default)")
    ap.add_argument("--seconds", type=float, default=0, help="quit after N seconds (testing)")
    ap.add_argument("-v", "--verbose", action="store_true")
    opts, _unknown = ap.parse_known_args(argv)
    return opts


def main(argv=None) -> int:
    opts = parse_args(sys.argv[1:] if argv is None else argv)
    setup_logging(opts.verbose)
    try:
        from .version import __version__
    except Exception:
        __version__ = "?"
    log.info("Claudius Maximus app %s starting (pid %d, %s)", __version__, os.getpid(), sys.platform)
    from .tray import make_backend
    holder: dict = {}

    def on_ready():
        holder["app"].on_ready()

    def on_quit():
        holder["app"].quit()
    backend = make_backend(on_ready, on_quit)
    app = App(opts, backend=backend)
    holder["app"] = app
    _guard("first refresh", app.refresh)
    if opts.seconds:
        threading.Timer(opts.seconds, app.quit).start()
    if not sys.platform.startswith("linux"):   # the SNI backend handles signals on its asyncio loop
        import signal
        try:
            for s in (signal.SIGINT, signal.SIGTERM):
                signal.signal(s, lambda *_: threading.Thread(target=app.quit, daemon=True).start())
        except (ValueError, OSError):
            pass
    try:
        backend.run()
    finally:
        app.quit()
    log.info("Claudius Maximus app stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
