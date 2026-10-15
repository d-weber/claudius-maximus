"""Idempotently add / remove the Claudius Maximus hooks + statusLine wrapper in ~/.claude/settings.json.

    python cm_settings.py install   --root <project root> --python <venv python> [--no-statusline]
    python cm_settings.py uninstall
    python cm_settings.py check      (exit 0 if installed)

* Makes a timestamped backup (settings.json.bak-claudius-YYYYmmdd-HHMMSS) before any change.
* Never touches hook entries it didn't create: ours are recognised by "cm_hook.py" in the command.
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
import shlex
import sys
import time

HOOK_MARK = "cm_hook.py"
SL_MARK = "cm_statusline.py"

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
    return os.path.join(home(), ".claude", "settings.json")


def load_settings(path: str) -> dict:
    if not os.path.exists(path):
        return {}
    with open(path) as f:
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
    with open(tmp, "w") as f:
        json.dump(d, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.chmod(tmp, mode)
    os.replace(tmp, path)


def _is_ours(handler, mark) -> bool:
    return isinstance(handler, dict) and mark in str(handler.get("command", ""))


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
        with open(os.path.join(state_dir(), "install-manifest.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def write_manifest(m: dict):
    os.makedirs(state_dir(), mode=0o700, exist_ok=True)
    with open(os.path.join(state_dir(), "install-manifest.json"), "w") as f:
        json.dump(m, f, indent=1)


def install(root: str, python: str, statusline: bool) -> int:
    path = settings_path()
    existed = os.path.exists(path)
    d = load_settings(path)
    m = read_manifest()
    if "had_hooks_key" not in m:  # first install: remember the original shape
        m["had_settings_file"] = existed
    if "had_hooks_key" not in m:  # first install: remember the original shape
        m["had_hooks_key"] = "hooks" in d
        m["had_statusline"] = "statusLine" in d
    hook_cmd = f"{shlex.quote(python)} -I -S {shlex.quote(os.path.join(root, 'host', 'hooks', 'cm_hook.py'))}"
    sl_cmd = f"{shlex.quote(python)} -I -S {shlex.quote(os.path.join(root, 'host', 'hooks', 'cm_statusline.py'))}"

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
        if isinstance(sl, dict) and SL_MARK in str(sl.get("command", "")):
            sl["command"] = sl_cmd  # already ours: refresh path
        else:
            if isinstance(sl, dict) and sl.get("command"):
                os.makedirs(state_dir(), mode=0o700, exist_ok=True)
                with open(os.path.join(state_dir(), "statusline-chain.json"), "w") as f:
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
    if isinstance(sl, dict) and SL_MARK in str(sl.get("command", "")):
        prev = m.get("statusline_prev")
        if prev is None:
            try:
                with open(os.path.join(state_dir(), "statusline-chain.json")) as f:
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


def check() -> int:
    try:
        d = load_settings(settings_path())
    except Exception:
        return 2
    s = json.dumps(d)
    return 0 if HOOK_MARK in s else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["install", "uninstall", "check"])
    ap.add_argument("--root")
    ap.add_argument("--python")
    ap.add_argument("--no-statusline", action="store_true")
    a = ap.parse_args()
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
