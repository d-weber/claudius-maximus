"""Claudius Maximus — Claude Code hook relay.  MUST stay tiny, fast and silent.

Installed in ~/.claude/settings.json for several hook events (see tools/install.sh) as an *async* command:
    "<root>/host/.venv/bin/python" -I -S "<root>/host/hooks/cm_hook.py"

It reads the hook JSON from stdin, keeps only the few fields the companion needs (truncated), and sends
ONE unix datagram (<= 2000 bytes) to the daemon's socket. If the daemon is not running, the socket is
missing, or anything at all goes wrong, it silently does nothing. It never prints to stdout (stdout of
UserPromptSubmit/SessionStart hooks would be injected into Claude's context) and always exits 0.

Datagram format (JSON object, short keys):
    v=1, e=hook_event_name, ts=epoch float, sid=session_id, cwd, aid=agent_id, at=agent_type,
    tool=tool_name, inp={few tool_input keys, truncated}, tid=tool_use_id, msg=Notification message,
    nt=notification_type, prompt, last=last_assistant_message, err=error, src=source, why=reason,
    trig=trigger, intr=is_interrupt, title=session_title, model
"""
import os
import sys
import time

MAXDGRAM = 2000


def sock_path():
    base = os.environ.get("CLAUDIUS_HOME") or os.path.join(os.path.expanduser("~"), ".claudius-maximus")
    return os.path.join(base, "hooks.sock")


def cut(v, n):
    if v is None:
        return None
    if not isinstance(v, str):
        v = str(v)
    return v if len(v) <= n else v[:n]


INPUT_KEYS = ("command", "description", "file_path", "path", "pattern", "url", "query", "prompt",
              "subagent_type", "skill", "notebook_path", "glob", "questions", "plan", "content")


def compact(p):
    import json
    ev = p.get("hook_event_name") or (sys.argv[1] if len(sys.argv) > 1 else "")
    r = {"v": 1, "e": ev, "ts": round(time.time(), 3), "sid": p.get("session_id")}
    for src, dst, n in (("cwd", "cwd", 300), ("agent_id", "aid", 40), ("agent_type", "at", 60),
                        ("tool_name", "tool", 80), ("tool_use_id", "tid", 80), ("message", "msg", 200),
                        ("notification_type", "nt", 40), ("prompt", "prompt", 240),
                        ("last_assistant_message", "last", 240), ("error", "err", 200), ("source", "src", 30),
                        ("reason", "why", 60), ("trigger", "trig", 20), ("session_title", "title", 120),
                        ("model", "model", 60)):
        v = p.get(src)
        if v is not None and v != "":
            r[dst] = cut(v, n) if not isinstance(v, (bool, int, float)) else v
    if p.get("is_interrupt"):
        r["intr"] = True
    ti = p.get("tool_input")
    if isinstance(ti, dict):
        inp = {}
        for k in INPUT_KEYS:
            v = ti.get(k)
            if v is None:
                continue
            if k == "questions" and isinstance(v, list) and v:
                q = v[0] if isinstance(v[0], dict) else {}
                v = q.get("question") or q.get("header") or ""
            elif not isinstance(v, str):
                continue
            inp[k] = cut(v, 160)
        if inp:
            r["inp"] = inp
    data = json.dumps(r, ensure_ascii=False, separators=(",", ":")).encode("utf-8", "replace")
    # shrink until it fits in one datagram
    for key in ("inp", "last", "prompt", "msg", "err", "cwd", "title"):
        if len(data) <= MAXDGRAM:
            break
        if key in r:
            if key == "inp":
                r["inp"] = {k: cut(v, 60) for k, v in r["inp"].items()}
            else:
                r[key] = cut(r[key], 60)
            data = json.dumps(r, ensure_ascii=False, separators=(",", ":")).encode("utf-8", "replace")
    return data if len(data) <= MAXDGRAM else None


def main():
    raw = sys.stdin.buffer.read()  # always drain stdin so Claude Code never sees a broken pipe
    path = sock_path()
    if not os.path.exists(path):  # daemon not running: cheapest possible exit
        return
    import json
    import socket
    p = json.loads(raw.decode("utf-8", "replace"))
    if not isinstance(p, dict):
        return
    data = compact(p)
    if not data:
        return
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.setblocking(False)
    try:
        s.sendto(data, path)
    finally:
        s.close()


if __name__ == "__main__":
    try:
        main()
    except BaseException:
        pass
    try:
        sys.stdout.flush()
    except BaseException:
        pass
    os._exit(0)
