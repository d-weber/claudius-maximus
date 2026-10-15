"""Bring the terminal running a given Claude Code session to the front (device action "focus").

Best effort, macOS only:
  short sid -> ~/.claude/sessions/<pid>.json (sessionId startswith sid) -> pid, cwd
  pid -> controlling TTY (ps) and the GUI app hosting it (walk parent processes up to an .app bundle)
  tmux: if the session runs inside tmux, select its window/pane, then focus the app of the tmux client
  Terminal.app / iTerm2: AppleScript selects the exact tab whose tty matches
  VS Code / Cursor / Windsurf / Zed: raise the window whose title contains the project folder name
    (System Events — needs Accessibility permission; otherwise just activates the app)
  anything else (Ghostty, WezTerm, kitty, Alacritty, Hyper, Warp...): activate the app.
Runs osascript with a timeout; never raises.
"""
from __future__ import annotations

import glob
import json
import logging
import os
import subprocess

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
