"""Bring the terminal running a given Claude Code session to the front (device action "focus").

Best effort. macOS:
  short sid -> ~/.claude/sessions/<pid>.json (sessionId startswith sid) -> pid, cwd
  pid -> controlling TTY (ps) and the GUI app hosting it (walk parent processes up to an .app bundle)
  tmux: if the session runs inside tmux, select its window/pane, then focus the app of the tmux client
  Terminal.app / iTerm2: AppleScript selects the exact tab whose tty matches
  VS Code / Cursor / Windsurf / Zed: raise the window whose title contains the project folder name
    (System Events — needs Accessibility permission; otherwise just activates the app)
  anything else (Ghostty, WezTerm, kitty, Alacritty, Hyper, Warp...): activate the app.
Runs osascript with a timeout; never raises.

Linux (best effort): tmux pane selection as above, then a window tool activates the window owned by the
session's terminal process (walking up the parents until a window is found; VS Code & co. are matched by
the project folder name in the window title). Tools, first available wins: KWin's D-Bus scripting (KDE,
X11 and Wayland, nothing to install), kdotool, xdotool (X11), wmctrl (X11). Without any, nothing is raised.
"""
from __future__ import annotations

import glob
import json
import logging
import os
import shutil
import subprocess
import sys

log = logging.getLogger("companion.focus")


def _run(args: list[str], timeout: float = 5.0, input: str | None = None) -> str:
    try:
        r = subprocess.run(args, capture_output=True, text=True, timeout=timeout, input=input)
        return r.stdout.strip()
    except Exception as e:
        log.debug("run %s failed: %s", args[0], e)
        return ""


def find_session(sid: str) -> dict | None:
    """Return the ~/.claude/sessions/<pid>.json record for a (short or full) session id."""
    if not sid:
        return None
    best = None
    for f in glob.glob(os.path.expanduser("~/.claude/sessions/*.json")):
        try:
            with open(f) as fh:
                d = json.load(fh)
        except Exception:
            continue
        if isinstance(d, dict) and str(d.get("sessionId", "")).startswith(sid):
            if best is None or d.get("updatedAt", 0) > best.get("updatedAt", 0):
                best = d
    return best


def _ps(pid: int) -> tuple[int, str, str] | None:
    out = _run(["ps", "-o", "ppid=,tty=,comm=", "-p", str(pid)], 2)
    if not out:
        return None
    parts = out.split(None, 2)
    if len(parts) < 3:
        return None
    try:
        return int(parts[0]), parts[1], parts[2]
    except ValueError:
        return None


def _host_app(pid: int) -> tuple[str | None, list[str]]:
    """Walk up parents. Return (outermost .app bundle path or None, list of ancestor comm names)."""
    app = None
    names = []
    seen = 0
    while pid > 1 and seen < 40:
        seen += 1
        info = _ps(pid)
        if not info:
            break
        ppid, _tty, comm = info
        names.append(comm)
        i = comm.find(".app/")
        if i >= 0:
            app = comm[: i + 4]  # keep walking: helpers live inside the outer bundle
            j = app.find(".app/")
            if j >= 0:
                app = app[: j + 4]
        pid = ppid
    return app, names


def _osascript(script: str) -> str:
    return _run(["osascript", "-"], 5, input=script)


def _focus_terminal_app(tty: str) -> bool:
    dev = "/dev/" + tty
    out = _osascript(f'''
tell application "Terminal"
  repeat with w in windows
    repeat with t in tabs of w
      if tty of t is "{dev}" then
        set selected of t to true
        set index of w to 1
        activate
        return "ok"
      end if
    end repeat
  end repeat
end tell
return "no"''')
    return out == "ok"


def _focus_iterm(tty: str) -> bool:
    dev = "/dev/" + tty
    out = _osascript(f'''
tell application "iTerm2"
  repeat with w in windows
    repeat with t in tabs of w
      repeat with s in sessions of t
        if tty of s is "{dev}" then
          select w
          select t
          select s
          activate
          return "ok"
        end if
      end repeat
    end repeat
  end repeat
end tell
return "no"''')
    return out == "ok"


def _raise_window_titled(app_path: str, needle: str) -> bool:
    proc = os.path.basename(app_path)[:-4]
    procname = {"Visual Studio Code": "Code"}.get(proc, proc)
    needle = needle.replace('"', "")
    out = _osascript(f'''
tell application "System Events"
  if exists process "{procname}" then
    tell process "{procname}"
      repeat with w in windows
        if name of w contains "{needle}" then
          perform action "AXRaise" of w
          set frontmost to true
          return "ok"
        end if
      end repeat
    end tell
  end if
end tell
return "no"''')
    return out == "ok"


def _activate(app_path: str) -> None:
    _run(["open", "-a", app_path], 5)


def _tmux_select(tty: str) -> str | None:
    """If tty is a tmux pane, select it and return the tty of a client attached to that session."""
    out = _run(["tmux", "list-panes", "-a", "-F", "#{pane_tty} #{session_name} #{window_id} #{pane_id}"], 2)
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[0] == "/dev/" + tty:
            _, sess, win, pane = parts
            _run(["tmux", "select-window", "-t", win], 2)
            _run(["tmux", "select-pane", "-t", pane], 2)
            clients = _run(["tmux", "list-clients", "-t", sess, "-F", "#{client_tty}"], 2).split()
            return clients[0].replace("/dev/", "") if clients else None
    return None


# ---- Linux ------------------------------------------------------------------------------------------
def _ancestors(pid: int, limit: int = 40) -> list[tuple[int, str]]:
    """[(pid, comm), ...] from `pid` up to (not including) init."""
    out = []
    while pid > 1 and len(out) < limit:
        info = _ps(pid)
        if not info:
            break
        out.append((pid, info[2]))
        pid = info[0]
    return out


_KWIN_JS = """(function () {
  var pids = %s, needle = %s;
  var list = (typeof workspace.windowList === "function") ? workspace.windowList() : workspace.clientList();
  function activate(w) { if ("activeWindow" in workspace) workspace.activeWindow = w; else workspace.activeClient = w; }
  for (var k = 0; k < pids.length; k++)
    for (var i = 0; i < list.length; i++)
      if (list[i].pid === pids[k] && !list[i].skipTaskbar) { activate(list[i]); return; }
  if (needle)
    for (var j = 0; j < list.length; j++)
      if (list[j].caption && list[j].caption.indexOf(needle) >= 0) { activate(list[j]); return; }
})();
"""


def _kwin_available() -> bool:
    if not shutil.which("gdbus"):
        return False
    out = _run(["gdbus", "call", "--session", "--dest", "org.freedesktop.DBus", "--object-path", "/org/freedesktop/DBus",
                "--method", "org.freedesktop.DBus.NameHasOwner", "org.kde.KWin"], 2)
    return "true" in out


def _kwin_activate(pids: list[int], needle: str = "") -> bool:
    """Run a one-shot KWin script that activates the first window owned by one of `pids` (in order), else the
    first whose title contains `needle` (Plasma 5 and 6). True = the script ran (KWin can't report a miss)."""
    import tempfile
    name = f"cm-focus-{os.getpid()}"
    fd, path = tempfile.mkstemp(prefix="cm-focus-", suffix=".js")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(_KWIN_JS % (json.dumps([int(p) for p in pids]), json.dumps(needle)))
        os.chmod(path, 0o644)
        base = ["gdbus", "call", "--session", "--dest", "org.kde.KWin"]
        _run(base + ["--object-path", "/Scripting", "--method", "org.kde.kwin.Scripting.unloadScript", name], 2)
        out = _run(base + ["--object-path", "/Scripting", "--method", "org.kde.kwin.Scripting.loadScript", path, name], 3)
        digits = "".join(ch for ch in out if ch.isdigit() or ch == "-")
        try:
            sid = int(digits)
        except ValueError:
            return False
        if sid < 0:
            return False
        ok = False
        for obj in (f"/Scripting/Script{sid}", f"/{sid}"):   # Plasma 6, Plasma 5
            r = subprocess.run(base + ["--object-path", obj, "--method", "org.kde.kwin.Script.run"],
                               capture_output=True, text=True, timeout=3)
            if r.returncode == 0:
                ok = True
                break
        _run(base + ["--object-path", "/Scripting", "--method", "org.kde.kwin.Scripting.unloadScript", name], 2)
        return ok
    except Exception as e:
        log.debug("kwin focus failed: %s", e)
        return False
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


def _linux_window_tool() -> str | None:
    for t in ("kdotool", "xdotool", "wmctrl"):
        if shutil.which(t):
            return t
    if _kwin_available():
        return "kwin"
    return None


def _linux_windows_of_pid(tool: str, pid: int) -> list[str]:
    if tool == "kdotool":
        return _run(["kdotool", "search", "--pid", str(pid)], 3).split()
    if tool == "xdotool":
        return _run(["xdotool", "search", "--onlyvisible", "--pid", str(pid)], 3).split()
    out = []
    for line in _run(["wmctrl", "-l", "-p"], 3).splitlines():   # wid desktop pid host title
        parts = line.split(None, 3)
        if len(parts) >= 3 and parts[2] == str(pid):
            out.append(parts[0])
    return out


def _linux_windows_titled(tool: str, needle: str) -> list[str]:
    if tool == "kdotool":
        return _run(["kdotool", "search", "--name", needle], 3).split()
    if tool == "xdotool":
        return _run(["xdotool", "search", "--onlyvisible", "--name", needle], 3).split()
    out = []
    for line in _run(["wmctrl", "-l", "-p"], 3).splitlines():
        parts = line.split(None, 4)
        if len(parts) == 5 and needle in parts[4]:
            out.append(parts[0])
    return out


def _linux_activate(tool: str, wid: str) -> bool:
    if tool == "kdotool":
        _run(["kdotool", "windowactivate", wid], 3)
        return True
    if tool == "xdotool":
        _run(["xdotool", "windowactivate", "--sync", wid], 3)
        return True
    _run(["wmctrl", "-i", "-a", wid], 3)
    return True


_LINUX_EDITORS = ("code", "code-insiders", "codium", "cursor", "windsurf", "zed", "zed-editor")


def _focus_linux(sid: str, pid: int, tty: str, cwd: str) -> str:
    tool = _linux_window_tool()
    if not tool:
        return f"session {sid}: no window tool (install kdotool, xdotool or wmctrl)"
    chain = _ancestors(pid)
    names = [c for _, c in chain]
    if tty not in ("??", "-", "?") and any("tmux" in n for n in names):
        ctty = _tmux_select(tty)
        if ctty:
            tty = ctty
            pids = _run(["ps", "-t", ctty, "-o", "pid="], 2).split()
            for p in pids:
                try:
                    chain = _ancestors(int(p)) + chain
                except ValueError:
                    pass
    # editors first: their terminal lives in a renderer process, the window belongs to the main process
    needle = os.path.basename(cwd.rstrip("/")) if cwd else ""
    in_editor = bool(needle) and any(n.lower() in _LINUX_EDITORS for n in names)
    if tool == "kwin":
        pids = [] if in_editor else [p for p, _ in chain]
        if _kwin_activate(pids, needle if in_editor else ""):
            return f"focused {'window ' + repr(needle) if in_editor else 'terminal'} for {sid} (tty {tty}, kwin)"
        return f"session {sid}: KWin scripting call failed (tty {tty})"
    if in_editor:
        for wid in _linux_windows_titled(tool, needle):
            _linux_activate(tool, wid)
            return f"focused window '{needle}' for {sid} ({tool})"
    for p, comm in chain:
        wids = _linux_windows_of_pid(tool, p)
        if wids:
            _linux_activate(tool, wids[0])
            return f"focused {comm} for {sid} (tty {tty}, {tool})"
    return f"session {sid}: no window found for the process tree (tty {tty}, {tool})"


def focus_session(sid: str) -> str:
    """Focus the terminal hosting session `sid`. Returns a short description of what happened."""
    rec = find_session(sid)
    if not rec or not rec.get("pid"):
        return f"session {sid} not found"
    pid = int(rec["pid"])
    info = _ps(pid)
    if not info:
        return f"session {sid}: process {pid} gone"
    tty = info[1]
    cwd = rec.get("cwd") or ""
    if sys.platform != "darwin":
        return _focus_linux(sid, pid, tty, cwd)
    app, names = _host_app(pid)
    if app is None and any("tmux" in n for n in names) and tty not in ("??", "-"):
        ctty = _tmux_select(tty)
        if ctty:
            # find the GUI app of the tmux client process
            pids = _run(["ps", "-t", ctty, "-o", "pid="], 2).split()
            for p in pids:
                a, _ = _host_app(int(p))
                if a:
                    app, tty = a, ctty
                    break
    if not app:
        return f"session {sid}: no GUI app found (tty {tty})"
    name = os.path.basename(app)
    ok = False
    if name == "Terminal.app":
        ok = _focus_terminal_app(tty)
    elif name.startswith("iTerm"):
        ok = _focus_iterm(tty)
    elif name in ("Visual Studio Code.app", "Cursor.app", "Windsurf.app", "Zed.app", "Visual Studio Code - Insiders.app"):
        ok = _raise_window_titled(app, os.path.basename(cwd.rstrip("/")))
    if not ok:
        _activate(app)
    return f"focused {name} for {sid} (tty {tty}{', exact' if ok else ''})"
