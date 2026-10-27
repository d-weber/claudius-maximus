"""Start the tray app at login: macOS LaunchAgent, Linux XDG autostart entry, Windows HKCU Run key.

    enable() -> str     install the entry (and first disable+stop the old headless service, if any); returns a
                        human sentence saying what was done (for a notification / the CLI)
    disable() -> str    remove the entry
    is_enabled() -> bool
    legacy_service_installed() -> bool   the headless daemon from tools/install.sh is still set up

The entry runs `self_command() + ["app"]`. Two daemons would fight over the single-instance lock (the loser
just exits, but then the tray app shows "another daemon is running"), hence the legacy-service handling.

Deliberately not done: `launchctl bootstrap` on enable (RunAtLoad would start a second app right now, next to
the one calling us) and `launchctl bootout` on disable (would kill the running app if launchd started it). The
plist is picked up at the next login and simply ignored once removed. The same goes for Linux: nothing is
launched now, the .desktop file is read at the next login.

When $HOME is not the real home directory (tests, like tools/install.sh's test mode) launchctl / systemctl
are never called.
"""
from __future__ import annotations

import logging
import os
import plistlib
import subprocess
import sys

from . import hostos

log = logging.getLogger("companion.autostart")

APP_LABEL = "com.claudius-maximus.app"
DESKTOP_NAME = "claudius-maximus.desktop"
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
RUN_VALUE = "ClaudiusMaximus"


# ---------------------------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------------------------
def app_command() -> list[str]:
    """argv that starts the tray app. Windows, not frozen: pythonw (no console window) and a -c bootstrap,
    because a Run entry has no working directory to find the `companion` package from."""
    cmd = hostos.self_command() + ["app"]
    if hostos.IS_WINDOWS and not hostos.is_frozen():
        py = cmd[0]
        w = os.path.join(os.path.dirname(py), "pythonw.exe")
        if os.path.exists(w):
            py = w
        boot = (f"import sys; sys.path.insert(0, {hostos.package_root()!r}); "
                "from companion.__main__ import main; sys.exit(main(['app']))")
        cmd = [py, "-c", boot]
    return cmd


def mac_unstable_location(path: str | None = None) -> str:
    """macOS: why the running .app can't be the target of a login item ("" if it can). Opened from the .dmg
    it runs from /Volumes/... (gone after eject); opened from ~/Downloads with the quarantine flag, Gatekeeper
    App Translocation runs it from a random read-only /private/var/folders/.../AppTranslocation/... copy. A
    LaunchAgent pointing there would fail at the next login."""
    p = path or (hostos.self_command()[0] if hostos.is_frozen() else "")
    if "/AppTranslocation/" in p:
        return "it runs from a temporary App Translocation copy"
    if p.startswith("/Volumes/"):
        return "it runs from the disk image"
    return ""


def _real_home() -> bool:
    try:
        import pwd
        return os.path.realpath(pwd.getpwuid(os.getuid()).pw_dir) == os.path.realpath(hostos.home())
    except Exception:
        return True


def _run(cmd: list[str]) -> int:
    if not _real_home():
        log.info("test mode (HOME is not the real home): not running %s", " ".join(cmd))
        return 0
    try:
        return subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, timeout=20).returncode
    except (OSError, subprocess.SubprocessError) as e:
        log.warning("%s failed: %s", cmd[0], e)
        return -1


def _write_atomic(path: str, data: bytes) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def plist_path() -> str:
    return os.path.join(hostos.home(), "Library", "LaunchAgents", f"{APP_LABEL}.plist")


def desktop_path() -> str:
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(hostos.home(), ".config")
    return os.path.join(base, "autostart", DESKTOP_NAME)


def plist_bytes(cmd: list[str] | None = None) -> bytes:
    cmd = cmd or app_command()
    d = {"Label": APP_LABEL, "ProgramArguments": cmd, "RunAtLoad": True, "KeepAlive": False,
         "ProcessType": "Interactive", "LimitLoadToSessionType": "Aqua"}
    cwd = hostos.self_cwd()
    if cwd:
        d["WorkingDirectory"] = cwd
    return plistlib.dumps(d)


def _desktop_quote(arg: str) -> str:
    """Desktop Entry spec Exec quoting."""
    if arg and not any(c in arg for c in ' \t\n"\'\\><~|&;$*?#()`'):
        return arg.replace("%", "%%")
    esc = arg.replace("\\", "\\\\").replace('"', '\\"').replace("`", "\\`").replace("$", "\\$")
    return '"' + esc.replace("%", "%%") + '"'


def desktop_bytes(cmd: list[str] | None = None) -> bytes:
    cmd = cmd or app_command()
    lines = ["[Desktop Entry]", "Type=Application", f"Name={hostos.APP_NAME}",
             "Comment=Desk companion for Claude Code (tray app)",
             "Exec=" + " ".join(_desktop_quote(a) for a in cmd), "Icon=claudius-maximus", "Terminal=false",
             "X-GNOME-Autostart-enabled=true", "X-KDE-autostart-after=panel"]
    cwd = hostos.self_cwd()
    if cwd:
        lines.append(f"Path={cwd}")
    return ("\n".join(lines) + "\n").encode()


def _winreg():
    import winreg  # noqa: PLC0415 (Windows only)
    return winreg


def windows_command(cmd: list[str] | None = None) -> str:
    return subprocess.list2cmdline(cmd or app_command())


# ---------------------------------------------------------------------------------------------
# the old headless service (tools/install.sh)
# ---------------------------------------------------------------------------------------------
def legacy_service_installed() -> bool:
    """The headless daemon service is set up: LaunchAgent plist present, or systemd unit file present and
    enabled/active (a unit that is merely left on disk, disabled, doesn't count)."""
    p = hostos.service_path()
    if not p or not os.path.exists(p):
        return False
    if hostos.IS_LINUX and _real_home():
        try:
            en = subprocess.run(["systemctl", "--user", "is-enabled", hostos.SERVICE_UNIT],
                                capture_output=True, text=True, timeout=10).stdout.strip()
            ac = subprocess.run(["systemctl", "--user", "is-active", hostos.SERVICE_UNIT],
                                capture_output=True, text=True, timeout=10).stdout.strip()
            return en in ("enabled", "enabled-runtime", "linked") or ac in ("active", "activating")
        except (OSError, subprocess.SubprocessError):
            return True
    return True


def disable_legacy_service() -> str:
    """Stop and disable the headless daemon so the app's daemon can take over. Returns what was done ("" if
    nothing). macOS: bootout + rename the plist to *.plist.disabled-by-app (launchd ignores it, the user can
    rename it back). Linux: systemctl --user disable --now (the unit file stays)."""
    if not legacy_service_installed():
        return ""
    p = hostos.service_path()
    if hostos.IS_MAC:
        _run(["launchctl", "bootout", f"gui/{os.getuid()}/{hostos.SERVICE_LABEL}"])
        try:
            os.replace(p, p + ".disabled-by-app")
        except OSError as e:
            log.warning("cannot move %s aside: %s", p, e)
        msg = f"stopped and disabled the headless LaunchAgent {hostos.SERVICE_LABEL} ({p}.disabled-by-app)"
    else:
        _run(["systemctl", "--user", "disable", "--now", hostos.SERVICE_UNIT])
        msg = f"stopped and disabled the headless systemd user service {hostos.SERVICE_UNIT}"
    log.info("%s", msg)
    return msg


# ---------------------------------------------------------------------------------------------
# API
# ---------------------------------------------------------------------------------------------
def is_enabled() -> bool:
    if hostos.IS_MAC:
        return os.path.exists(plist_path())
    if hostos.IS_WINDOWS:
        try:
            wr = _winreg()
            with wr.OpenKey(wr.HKEY_CURRENT_USER, RUN_KEY) as k:
                v, _ = wr.QueryValueEx(k, RUN_VALUE)
                return bool(v)
        except OSError:
            return False
    return os.path.exists(desktop_path())


def enable() -> str:
    """Start the app at login. Raises OSError if the entry can't be written."""
    if hostos.IS_MAC:   # checked before retiring the legacy service: fail without side effects
        why = mac_unstable_location()
        if why:
            raise OSError(f"move {hostos.APP_NAME} to the Applications folder and open it from there first ({why})")
    notes = []
    legacy = disable_legacy_service()
    if legacy:
        notes.append(legacy)
    if hostos.IS_MAC:
        _write_atomic(plist_path(), plist_bytes())
        where = plist_path()
    elif hostos.IS_WINDOWS:
        wr = _winreg()
        with wr.CreateKey(wr.HKEY_CURRENT_USER, RUN_KEY) as k:
            wr.SetValueEx(k, RUN_VALUE, 0, wr.REG_SZ, windows_command())
        where = "HKCU\\" + RUN_KEY + "\\" + RUN_VALUE
    else:
        _write_atomic(desktop_path(), desktop_bytes())
        where = desktop_path()
    msg = f"{hostos.APP_NAME} will start at login ({where})"
    log.info("autostart enabled: %s", where)
    return "; ".join([msg] + notes)


def disable() -> str:
    if hostos.IS_WINDOWS:
        try:
            wr = _winreg()
            with wr.OpenKey(wr.HKEY_CURRENT_USER, RUN_KEY, 0, wr.KEY_SET_VALUE) as k:
                wr.DeleteValue(k, RUN_VALUE)
        except OSError:
            pass
    else:
        try:
            os.unlink(plist_path() if hostos.IS_MAC else desktop_path())
        except FileNotFoundError:
            pass
    log.info("autostart disabled")
    return f"{hostos.APP_NAME} will not start at login"


def describe() -> str:
    where = (plist_path() if hostos.IS_MAC else "HKCU\\" + RUN_KEY + "\\" + RUN_VALUE if hostos.IS_WINDOWS
             else desktop_path())
    s = f"autostart: {'enabled' if is_enabled() else 'disabled'} ({where})"
    if legacy_service_installed():
        s += f"\nheadless {hostos.service_name()} also installed: {hostos.service_path()}" \
             " (enable autostart to replace it)"
    return s


if __name__ == "__main__":  # pragma: no cover
    print(describe(), file=sys.stdout)
