"""Idempotently add / remove the Claudius Maximus hooks + statusLine wrapper in ~/.claude/settings.json.

    python host/hooks/cm_settings.py install   --root <project root> --python <venv python> [--no-statusline]
    python host/hooks/cm_settings.py uninstall
    python host/hooks/cm_settings.py check      (exit 0 if installed)
    claudius-maximus hooks install|uninstall|check [--no-statusline]   (install_for_app: right command for
                                                                         the running program)

Lives in the companion package (host/hooks/cm_settings.py is a shim) so a frozen desktop app has it too.

Command forms written (double quotes: they work in bash, zsh and cmd; Claude Code on Windows runs hooks in
Git Bash or cmd):
    script install   "<python>" -I -S "<root>/host/hooks/cm_hook.py"   /   ... cm_statusline.py
    frozen app       "<exe>" hook                                       /   "<exe>" statusline
    pip install      "<python>" -m companion hook                       /   ... statusline
Ours are recognised in all three forms (is_ours_command), so switching between the script install and the
app replaces the entries instead of duplicating them.

* Makes a timestamped backup (settings.json.bak-claudius-YYYYmmdd-HHMMSS) before any change.
* Never touches hook entries it didn't create: ours are recognised by is_ours_command() (see above).
  Re-running install replaces our entries (e.g. after moving the repo) and leaves everything else alone.
* statusLine: if the user already has one, it is saved to ~/.claudius-maximus/statusline-chain.json and
  our wrapper runs it and prints its output (so the status line looks the same). Uninstall restores it.
* A manifest (~/.claudius-maximus/install-manifest.json) remembers what existed before, so uninstall
  reverses exactly (e.g. removes an empty "hooks" key only if install created it).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import sys
import time

HOOK_MARK = "cm_hook.py"
SL_MARK = "cm_statusline.py"
# `"<exe>" hook` where the exe is our app (claudius-maximus / ClaudiusMaximus / Claudius Maximus.app...), or
# `... -m companion hook`. Matched on the whole command, case-insensitive.
_APP_RE = {
    "hook": re.compile(r'(?i)(claudius[ _-]?maximus[^"\s]*"?|-m\s+companion)\s+hook\s*$'),
    "statusline": re.compile(r'(?i)(claudius[ _-]?maximus[^"\s]*"?|-m\s+companion)\s+statusline\s*$'),
}

# event -> matcher (None = all)
EVENTS = {
    "SessionStart": None,
    "SessionEnd": None,
    "UserPromptSubmit": None,
    "PreToolUse": None,
    "PostToolUse": None,
    "PostToolUseFailure": None,
    "PermissionRequest": None,
    "PermissionDenied": None,
    "Notification": "permission_prompt|idle_prompt|elicitation_dialog",
    "Stop": None,
    "StopFailure": None,
    "SubagentStart": None,
    "SubagentStop": None,
    "PreCompact": None,
}
# Run synchronously (our hook takes ~30 ms and prints nothing): end-of-turn/session events, where an async
# hook can be lost if the claude process exits right after (seen with `claude -p`). Everything else is async.
SYNC_EVENTS = {"Stop", "StopFailure", "SessionEnd", "SubagentStop"}


def home() -> str:
    return os.path.expanduser("~")


def state_dir() -> str:
    return os.environ.get("CLAUDIUS_HOME") or os.path.join(home(), ".claudius-maximus")


def settings_path() -> str:
    """Claude Code's user settings ($CLAUDE_CONFIG_DIR or ~/.claude)."""
    base = os.environ.get("CLAUDE_CONFIG_DIR") or os.path.join(home(), ".claude")
    return os.path.join(base, "settings.json")


# ---------------------------------------------------------------------------------------------
# command forms
# ---------------------------------------------------------------------------------------------
def quote(arg: str) -> str:
    """Double-quote for both POSIX shells and cmd. Inside POSIX double quotes $ ` \\ " are still special:
    fall back to single quotes there if the path has any (cmd can't run such paths anyway)."""
    if os.name != "nt" and any(c in arg for c in '$`"\\'):
        return shlex.quote(arg)
    return f'"{arg}"'


def script_command(python: str, script: str) -> str:
    return f"{quote(python)} -I -S {quote(script)}"


def hooks_script_dir() -> str | None:
    """host/hooks in a repo checkout (where cm_hook.py lives next to the companion package), else None."""
    d = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "hooks")
    return d if os.path.exists(os.path.join(d, HOOK_MARK)) else None


def hook_command(which: str = "hook") -> str:
    """The command that runs our hook (`which`="hook") or statusLine wrapper ("statusline") for the running
    program: frozen app -> "<exe>" hook; repo checkout -> "<python>" -I -S "<host/hooks/cm_hook.py>";
    otherwise (pip install) -> "<python>" -m companion hook."""
    if getattr(sys, "frozen", False):
        exe = os.environ.get("APPIMAGE") or sys.executable
        return f"{quote(exe)} {which}"
    d = hooks_script_dir()
    if d:
        return script_command(sys.executable, os.path.join(d, HOOK_MARK if which == "hook" else SL_MARK))
    return f"{quote(sys.executable)} -m companion {which}"


def is_ours_command(cmd, which: str = "hook") -> bool:
    """Is this hook / statusLine command one of ours (any of the three forms)?"""
    cmd = str(cmd or "")
    if (HOOK_MARK if which == "hook" else SL_MARK) in cmd:
        return True
    return bool(_APP_RE[which].search(cmd.strip()))


def load_settings(path: str) -> dict:
    if not os.path.exists(path):
        return {}
    # utf-8-sig: Claude Code writes UTF-8 (non-ASCII paths / user names); an editor may have added a BOM. The
    # locale encoding (cp1252 on Windows) would mangle or reject such a file.
    with open(path, encoding="utf-8-sig") as f:
        txt = f.read()
    if not txt.strip():
        return {}
    d = json.loads(txt)  # raise on invalid JSON: never clobber a file we can't parse
    if not isinstance(d, dict):
        raise ValueError("settings.json is not a JSON object")
    return d


def save_settings(path: str, d: dict, backup: bool = True):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if backup and os.path.exists(path):
        b = path + time.strftime(".bak-claudius-%Y%m%d-%H%M%S")
        with open(path, "rb") as src, open(b, "wb") as dst:
            dst.write(src.read())
        print(f"  backup: {b}")
    mode = os.stat(path).st_mode & 0o777 if os.path.exists(path) else 0o644
    tmp = path + ".claudius.tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(d, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.chmod(tmp, mode)
    os.replace(tmp, path)


def _is_ours(handler, mark=HOOK_MARK) -> bool:
    which = "statusline" if mark == SL_MARK else "hook"
    return isinstance(handler, dict) and is_ours_command(handler.get("command", ""), which)


def strip_hooks(d: dict) -> bool:
    """Remove our hook handlers. Returns True if anything changed."""
    hooks = d.get("hooks")
    if not isinstance(hooks, dict):
        return False
    changed = False
    for ev in list(hooks.keys()):
        groups = hooks[ev]
        if not isinstance(groups, list):
            continue
        new_groups = []
        for g in groups:
            if isinstance(g, dict) and isinstance(g.get("hooks"), list):
                kept = [h for h in g["hooks"] if not _is_ours(h, HOOK_MARK)]
                if len(kept) != len(g["hooks"]):
                    changed = True
                    if not kept:
                        continue  # group only contained our handler
                    g = dict(g, hooks=kept)
            new_groups.append(g)
        if new_groups:
            hooks[ev] = new_groups
        else:
            del hooks[ev]
            changed = True
    return changed


def read_manifest() -> dict:
    try:
        with open(os.path.join(state_dir(), "install-manifest.json"), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def write_manifest(m: dict):
    os.makedirs(state_dir(), mode=0o700, exist_ok=True)
    with open(os.path.join(state_dir(), "install-manifest.json"), "w", encoding="utf-8") as f:
        json.dump(m, f, indent=1)


def install(root: str, python: str, statusline: bool) -> int:
    """Script install (tools/install.sh): hooks run <root>/host/hooks/cm_*.py with `python -I -S`."""
    return install_commands(script_command(python, os.path.join(root, "host", "hooks", HOOK_MARK)),
                            script_command(python, os.path.join(root, "host", "hooks", SL_MARK)),
                            statusline, root=root)


def install_for_app(statusline: bool = True) -> int:
    """Install with the right command for the running program (see hook_command)."""
    root = os.path.dirname(os.path.dirname(hooks_script_dir())) if hooks_script_dir() and not getattr(
        sys, "frozen", False) else ""
    return install_commands(hook_command("hook"), hook_command("statusline"), statusline, root=root)


def install_commands(hook_cmd: str, sl_cmd: str, statusline: bool, root: str = "") -> int:
    path = settings_path()
    existed = os.path.exists(path)
    d = load_settings(path)
    m = read_manifest()
    if "had_hooks_key" not in m:  # first install: remember the original shape
        m["had_settings_file"] = existed
        m["had_hooks_key"] = "hooks" in d
        m["had_statusline"] = "statusLine" in d

    before = json.dumps(d, sort_keys=True)
    strip_hooks(d)
    hooks = d.setdefault("hooks", {})
    if not isinstance(hooks, dict):
        print("error: settings.json 'hooks' is not an object; aborting", file=sys.stderr)
        return 2
    for ev, matcher in EVENTS.items():
        h = {"type": "command", "command": hook_cmd, "timeout": 5}
        if ev not in SYNC_EVENTS:
            h["async"] = True
        group = {"hooks": [h]}
        if matcher:
            group = {"matcher": matcher, **group}
        lst = hooks.setdefault(ev, [])
        if not isinstance(lst, list):
            print(f"warning: hooks.{ev} is not a list; skipping", file=sys.stderr)
            continue
        lst.append(group)

    if statusline:
        sl = d.get("statusLine")
        if isinstance(sl, dict) and is_ours_command(sl.get("command", ""), "statusline"):
            sl["command"] = sl_cmd  # already ours: refresh path / switch form
        else:
            if isinstance(sl, dict) and sl.get("command"):
                os.makedirs(state_dir(), mode=0o700, exist_ok=True)
                with open(os.path.join(state_dir(), "statusline-chain.json"), "w", encoding="utf-8") as f:
                    json.dump({"command": sl["command"]}, f)
                m["statusline_prev"] = sl
                print(f"  statusLine: chaining your existing command: {sl['command']}")
            new = dict(sl) if isinstance(sl, dict) else {"type": "command", "padding": 0}
            new["type"] = "command"
            new["command"] = sl_cmd
            d["statusLine"] = new
        m["statusline_installed"] = True

    m["installed_at"] = int(time.time())
    m["root"] = root
    m["hook_command"] = hook_cmd
    if json.dumps(d, sort_keys=True) != before:
        save_settings(path, d)
        print(f"  updated {path}")
    else:
        print(f"  {path} already up to date")
    write_manifest(m)
    return 0


def uninstall() -> int:
    path = settings_path()
    try:
        d = load_settings(path)
    except (ValueError, OSError) as e:
        print(f"error: cannot parse {path}: {e}; not touching it", file=sys.stderr)
        return 2
    m = read_manifest()
    before = json.dumps(d, sort_keys=True)
    strip_hooks(d)
    if d.get("hooks") == {} and not m.get("had_hooks_key", True):
        del d["hooks"]
    sl = d.get("statusLine")
    if isinstance(sl, dict) and is_ours_command(sl.get("command", ""), "statusline"):
        prev = m.get("statusline_prev")
        if prev is None:
            try:
                with open(os.path.join(state_dir(), "statusline-chain.json"), encoding="utf-8") as f:
                    c = json.load(f)
                prev = dict(sl, command=c["command"])
            except (OSError, ValueError, KeyError):
                prev = None
        if prev:
            d["statusLine"] = prev
            print(f"  statusLine restored: {prev.get('command')}")
        else:
            del d["statusLine"]
    if d == {} and m.get("had_settings_file") is False and os.path.exists(path):
        os.remove(path)  # install created the file: remove it
        print(f"  removed {path} (created by install)")
    elif json.dumps(d, sort_keys=True) != before:
        save_settings(path, d)
        print(f"  cleaned {path}")
    else:
        print(f"  {path}: nothing of ours found")
    return 0


def installed() -> dict:
    """{"hooks": bool, "statusline": bool, "commands": [our hook commands]} from Claude Code's settings."""
    out = {"hooks": False, "statusline": False, "commands": []}
    try:
        d = load_settings(settings_path())
    except Exception:
        return out
    hooks = d.get("hooks") if isinstance(d.get("hooks"), dict) else {}
    for groups in hooks.values():
        for g in groups if isinstance(groups, list) else []:
            for h in (g.get("hooks") if isinstance(g, dict) and isinstance(g.get("hooks"), list) else []):
                if _is_ours(h, HOOK_MARK):
                    out["hooks"] = True
                    if h.get("command") not in out["commands"]:
                        out["commands"].append(h.get("command"))
    sl = d.get("statusLine")
    out["statusline"] = isinstance(sl, dict) and is_ours_command(sl.get("command", ""), "statusline")
    return out


def installed_matches_self(statusline: bool | None = None) -> bool:
    """True if our hooks are installed and all run the current program (hook_command()), and so does our
    statusLine wrapper if present. False when nothing is installed, or after the program moved (new AppImage
    path, .app copied to /Applications, repo moved): the app then re-runs install_for_app() on start.
    statusline=True also requires the wrapper; None (default) accepts it absent; False ignores it."""
    inst = installed()
    if not inst["hooks"] or inst["commands"] != [hook_command("hook")]:
        return False
    if statusline is False:
        return True
    try:
        sl = load_settings(settings_path()).get("statusLine")
    except Exception:
        return False
    cur = sl.get("command") if isinstance(sl, dict) and is_ours_command(sl.get("command", ""), "statusline") \
        else None
    if cur is None:
        return not statusline
    return cur == hook_command("statusline")


def check() -> int:
    try:
        load_settings(settings_path())
    except Exception:
        return 2
    return 0 if installed()["hooks"] else 1


def main(argv=None):
    ap = argparse.ArgumentParser(prog="cm_settings.py")
    ap.add_argument("action", choices=["install", "uninstall", "check"])
    ap.add_argument("--root")
    ap.add_argument("--python")
    ap.add_argument("--no-statusline", action="store_true")
    a = ap.parse_args(argv)
    if a.action == "install":
        if not a.root or not a.python:
            ap.error("install needs --root and --python")
        try:
            return install(os.path.abspath(a.root), a.python, not a.no_statusline)
        except (ValueError, OSError) as e:
            print(f"error: cannot update {settings_path()}: {e}", file=sys.stderr)
            return 2
    if a.action == "uninstall":
        return uninstall()
    return check()


if __name__ == "__main__":
    sys.exit(main())
