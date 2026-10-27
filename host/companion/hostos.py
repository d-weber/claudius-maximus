"""Where things live on this OS, and the few OS services the app needs (macOS, Linux and Windows).

macOS:   logs in ~/Library/Logs; the headless daemon is a LaunchAgent (host/launchd/*.plist).
Linux:   logs in $XDG_STATE_HOME/claudius-maximus (default ~/.local/state/...); the headless daemon is a systemd
         user service (host/systemd/claudius-maximus.service).
Windows: logs in %LOCALAPPDATA%\\claudius-maximus\\logs; no headless service (the tray app autostarts instead).
State (socket/port file, status.json, statusline.json, locks, settings.json) is ~/.claudius-maximus everywhere
(%USERPROFILE%\\.claudius-maximus on Windows), overridable with $CLAUDIUS_HOME (tests).

This module must stay cheap to import (stdlib only, no subprocess at import time): the `hook` fast path and the
CLI import it.
"""
from __future__ import annotations

import json
import os
import sys
import threading

IS_MAC = sys.platform == "darwin"
IS_LINUX = sys.platform.startswith("linux")
IS_WINDOWS = sys.platform == "win32"

SERVICE_LABEL = "com.claudius-maximus.daemon"      # launchd label (headless daemon, tools/install.sh)
SERVICE_UNIT = "claudius-maximus.service"          # systemd user unit (headless daemon, tools/install.sh)
APP_NAME = "Claudius Maximus"


# ---------------------------------------------------------------------------------------------
# paths
# ---------------------------------------------------------------------------------------------
def home() -> str:
    return os.path.expanduser("~")


def state_dir() -> str:
    """~/.claudius-maximus (socket / port file, status.json, statusline.json, locks, settings.json)."""
    return os.environ.get("CLAUDIUS_HOME") or os.path.join(home(), ".claudius-maximus")


def log_dir() -> str:
    if IS_MAC:
        return os.path.join(home(), "Library", "Logs")
    if IS_WINDOWS:
        base = os.environ.get("LOCALAPPDATA") or os.path.join(home(), "AppData", "Local")
        return os.path.join(base, "claudius-maximus", "logs")
    base = os.environ.get("XDG_STATE_HOME") or os.path.join(home(), ".local", "state")
    return os.path.join(base, "claudius-maximus")


def log_path() -> str:
    return os.path.join(log_dir(), "claudius-maximus.log")


def claude_config_dir() -> str:
    """Claude Code's config dir ($CLAUDE_CONFIG_DIR or ~/.claude; %USERPROFILE%\\.claude on Windows)."""
    return os.environ.get("CLAUDE_CONFIG_DIR") or os.path.join(home(), ".claude")


claude_dir = claude_config_dir


def settings_path() -> str:
    """The app's own settings (board choice, auto flash, first_run_done...), not Claude Code's."""
    return os.path.join(state_dir(), "settings.json")


def service_path() -> str | None:
    """The installed headless background-service definition (LaunchAgent plist / systemd user unit).
    None on Windows, which has no headless service."""
    if IS_MAC:
        return os.path.join(home(), "Library", "LaunchAgents", f"{SERVICE_LABEL}.plist")
    if IS_WINDOWS:
        return None
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(home(), ".config")
    return os.path.join(base, "systemd", "user", SERVICE_UNIT)


def service_name() -> str:
    return "launch agent" if IS_MAC else "service" if IS_WINDOWS else "systemd unit"


def service_installed() -> bool:
    p = service_path()
    return bool(p) and os.path.exists(p)


# ---------------------------------------------------------------------------------------------
# running ourselves
# ---------------------------------------------------------------------------------------------
def is_frozen() -> bool:
    return bool(getattr(sys, "frozen", False))


def package_root() -> str:
    """Directory that contains the `companion` package (host/ in the repo)."""
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def self_command() -> list[str]:
    """argv prefix that runs this program: [sys.executable] when frozen, else [python, "-m", "companion"].
    An AppImage runs from a temporary mount point that changes every launch, so use $APPIMAGE (the .AppImage
    file itself) when set. The non-frozen form needs package_root() as cwd (or companion installed); see
    self_cwd()."""
    if is_frozen():
        return [os.environ.get("APPIMAGE") or sys.executable]
    return [sys.executable, "-m", "companion"]


def self_cwd() -> str | None:
    """Working directory self_command() needs (None when frozen: the binary finds its own modules)."""
    return None if is_frozen() else package_root()


def pid_alive(pid) -> bool:
    """Is a process with this pid running? (os.kill(pid, 0) would *terminate* it on Windows.)"""
    try:
        pid = int(pid)
    except (TypeError, ValueError):
        return False
    if pid <= 0:
        return False
    if IS_WINDOWS:
        try:
            import ctypes
            k32 = ctypes.windll.kernel32
            h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
            if not h:
                return ctypes.GetLastError() == 5  # access denied: exists, owned by someone else
            try:
                code = ctypes.c_ulong()
                if not k32.GetExitCodeProcess(h, ctypes.byref(code)):
                    return True
                return code.value == 259  # STILL_ACTIVE
            finally:
                k32.CloseHandle(h)
        except Exception:
            return False
    try:
        os.kill(pid, 0)
        return True
    except PermissionError:
        return True
    except OSError:
        return False


def open_path(path: str) -> bool:
    """Open a file / folder / URL with the desktop's default handler. Best effort, never blocks."""
    import subprocess
    try:
        if IS_WINDOWS:
            os.startfile(path)  # type: ignore[attr-defined]
            return True
        cmd = ["open", path] if IS_MAC else ["xdg-open", path]
        subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
        return True
    except (OSError, ValueError):
        return False


# ---------------------------------------------------------------------------------------------
# app settings: tiny JSON key/value store at settings_path()
# ---------------------------------------------------------------------------------------------
class Settings:
    """Read on every get (the CLI and the app may both write), written atomically (tmp + replace)."""

    def __init__(self, path: str | None = None):
        self._path = path
        self._lock = threading.Lock()

    @property
    def path(self) -> str:
        return self._path or settings_path()

    def all(self) -> dict:
        try:
            with open(self.path, encoding="utf-8") as f:
                d = json.load(f)
            return d if isinstance(d, dict) else {}
        except (OSError, ValueError):
            return {}

    def get(self, key: str, default=None):
        return self.all().get(key, default)

    def set(self, key: str, value) -> None:
        with self._lock:
            d = self.all()
            if value is None:
                d.pop(key, None)
            else:
                d[key] = value
            self._write(d)

    def _write(self, d: dict) -> None:
        import tempfile
        path = self.path
        folder = os.path.dirname(path)
        os.makedirs(folder, mode=0o700, exist_ok=True)
        fd, tmp = tempfile.mkstemp(prefix=".settings.", suffix=".tmp", dir=folder)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(d, f, indent=1, sort_keys=True)
                f.write("\n")
            os.replace(tmp, path)
        except BaseException:
            try:
                os.unlink(tmp)
            except OSError:
                pass
            raise


settings = Settings()


def settings_get(key: str, default=None):
    return settings.get(key, default)


def settings_set(key: str, value) -> None:
    """Set a key (None removes it). Raises OSError if the state dir is not writable."""
    settings.set(key, value)
