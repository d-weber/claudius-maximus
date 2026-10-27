"""Client side of the hooks IPC: what Claude Code runs on every hook event and every status-line refresh.

MUST stay tiny, fast and silent: stdlib only, importable with `python -I -S`, no import of anything else in
`companion` (so `claudius-maximus hook` in a frozen build, `python -m companion hook` and the standalone
host/hooks/cm_hook.py / cm_statusline.py wrappers all run this same code in a few tens of ms).

Transport (the daemon side is providers/hooks.py):
  * macOS/Linux: one AF_UNIX SOCK_DGRAM datagram to ~/.claudius-maximus/hooks.sock;
  * Windows (no AF_UNIX in CPython there): one UDP datagram to 127.0.0.1:<port>, where the daemon writes
    "<port> <key>\\n" to ~/.claudius-maximus/hooks.port. The key (random per daemon start) goes in the JSON as
    "k" so other local programs cannot inject fake hook events by guessing the port.
  Whichever file exists is used (hooks.sock first where AF_UNIX exists). No file = daemon not running = do
  nothing. Any error is swallowed.

hook_main(): reads the hook JSON from stdin, keeps only the few fields the companion needs (truncated) and sends
ONE datagram (<= 2000 bytes). Never prints to stdout (stdout of UserPromptSubmit/SessionStart hooks would be
injected into Claude's context) and the caller always exits 0.

Datagram format (JSON object, short keys):
    v=1, e=hook_event_name, ts=epoch float, sid=session_id, cwd, aid=agent_id, at=agent_type,
    tool=tool_name, inp={few tool_input keys, truncated}, tid=tool_use_id, msg=Notification message,
    nt=notification_type, prompt, last=last_assistant_message, err=error, src=source, why=reason,
    trig=trigger, intr=is_interrupt, title=session_title, model, k=key (UDP only)

statusline_main(): the statusLine wrapper. Claude Code pipes a JSON blob to the status line command after each
assistant message (and on its refresh timer). For claude.ai Pro/Max subscribers it contains the plan limits:
    "rate_limits": {"five_hour": {"used_percentage": 23.5, "resets_at": 1738425600},
                    "seven_day": {"used_percentage": 41.2, "resets_at": 1738857600}}
 1. writes the useful parts atomically to ~/.claudius-maximus/statusline.json (format documented in
    providers/hooks.py, read by providers/usage.py),
 2. sends a {"e":"StatusLine",...} datagram to the daemon (live per-session ctx%/cost/model),
 3. chains to the user's previous status line command (saved by the installer in
    ~/.claudius-maximus/statusline-chain.json as {"command": "..."}), feeding it the same stdin and printing
    its output, so the user's status line looks exactly as before.
Any failure in 1/2 is ignored; 3 always runs.
"""
import os
import sys
import time

MAXDGRAM = 2000
IS_WINDOWS = sys.platform == "win32"

INPUT_KEYS = ("command", "description", "file_path", "path", "pattern", "url", "query", "prompt",
              "subagent_type", "skill", "notebook_path", "glob", "questions", "plan", "content")


def home():
    return os.environ.get("CLAUDIUS_HOME") or os.path.join(os.path.expanduser("~"), ".claudius-maximus")


def sock_path():
    return os.path.join(home(), "hooks.sock")


def port_path():
    return os.path.join(home(), "hooks.port")


def read_port_file(path=None):
    """(port, key) from hooks.port, or None."""
    try:
        with open(path or port_path(), "rb") as f:
            parts = f.read(200).decode("ascii", "replace").split()
        port = int(parts[0])
        if not 0 < port < 65536:
            return None
        return port, (parts[1] if len(parts) > 1 else "")
    except (OSError, ValueError, IndexError):
        return None


def target():
    """Where to send: ("unix", path, "") | ("udp", ("127.0.0.1", port), key) | None (daemon not running)."""
    sp = sock_path()
    if not IS_WINDOWS and os.path.exists(sp):
        return ("unix", sp, "")
    pk = read_port_file()
    if pk:
        return ("udp", ("127.0.0.1", pk[0]), pk[1])
    return None


def send(data, tgt):
    import socket
    if tgt[0] == "unix":
        s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    else:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setblocking(False)
    try:
        s.sendto(data, tgt[1])
    finally:
        s.close()


def cut(v, n):
    if v is None:
        return None
    if not isinstance(v, str):
        v = str(v)
    return v if len(v) <= n else v[:n]


def compact(p, event="", key=""):
    """Hook JSON -> datagram bytes (<= MAXDGRAM), or None if it can't be made to fit."""
    import json
    ev = p.get("hook_event_name") or event
    r = {"v": 1, "e": ev, "ts": round(time.time(), 3), "sid": p.get("session_id")}
    if key:
        r["k"] = key
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
    for k in ("inp", "last", "prompt", "msg", "err", "cwd", "title"):
        if len(data) <= MAXDGRAM:
            break
        if k in r:
            if k == "inp":
                r["inp"] = {ik: cut(iv, 60) for ik, iv in r["inp"].items()}
            else:
                r[k] = cut(r[k], 60)
            data = json.dumps(r, ensure_ascii=False, separators=(",", ":")).encode("utf-8", "replace")
    return data if len(data) <= MAXDGRAM else None


# ---------------------------------------------------------------------------------------------
# stdio that also works in a windowed (no console) frozen Windows build, where sys.stdin/stdout are None
# but Claude Code's pipes are still the process's standard handles.
# ---------------------------------------------------------------------------------------------
def _win_std_fd(which):
    import ctypes
    import msvcrt
    h = ctypes.windll.kernel32.GetStdHandle(which)  # -10 stdin, -11 stdout
    if not h or h == ctypes.c_void_p(-1).value:
        return None
    return msvcrt.open_osfhandle(h, os.O_RDONLY if which == -10 else os.O_WRONLY)


def read_stdin():
    """All of stdin as bytes (always drained, so Claude Code never sees a broken pipe)."""
    stream = getattr(sys.stdin, "buffer", None)
    if stream is not None:
        return stream.read()
    if IS_WINDOWS:
        fd = _win_std_fd(-10)
        if fd is not None:
            with os.fdopen(fd, "rb") as f:
                return f.read()
    return b""


def write_stdout(data):
    stream = getattr(sys.stdout, "buffer", None)
    if stream is not None:
        stream.write(data)
        stream.flush()
        return
    if IS_WINDOWS:
        fd = _win_std_fd(-11)
        if fd is not None:
            os.write(fd, data)


# ---------------------------------------------------------------------------------------------
# entry points (callers wrap them: catch BaseException, exit 0)
# ---------------------------------------------------------------------------------------------
def hook_main(argv=None):
    raw = read_stdin()
    tgt = target()
    if tgt is None:  # daemon not running: cheapest possible exit
        return
    import json
    p = json.loads(raw.decode("utf-8", "replace"))
    if not isinstance(p, dict):
        return
    argv = sys.argv[1:] if argv is None else argv
    data = compact(p, argv[0] if argv else "", tgt[2])
    if data:
        send(data, tgt)


def record(p):
    """statusline.json + the StatusLine datagram."""
    import json
    base = home()
    if not os.path.isdir(base):
        return
    now = time.time()
    path = os.path.join(base, "statusline.json")
    try:
        with open(path) as f:
            prev = json.load(f)
        if not isinstance(prev, dict):
            prev = {}
    except Exception:
        prev = {}
    cw = p.get("context_window") or {}
    cost = p.get("cost") or {}
    model = p.get("model") or {}
    out = {
        "v": 1,
        "captured_at": int(now),
        "session_id": p.get("session_id"),
        "model": model.get("display_name") or model.get("id"),
        "model_id": model.get("id"),
        "version": p.get("version"),
        "ctx_pct": cw.get("used_percentage"),
        "ctx_size": cw.get("context_window_size"),
        "cost_usd": cost.get("total_cost_usd"),
        "rate_limits": prev.get("rate_limits"),
        "rate_limits_at": prev.get("rate_limits_at"),
    }
    rl = p.get("rate_limits")
    if isinstance(rl, dict) and rl:
        out["rate_limits"] = rl
        out["rate_limits_at"] = int(now)
    tmp = path + ".%d.tmp" % os.getpid()
    with open(tmp, "w") as f:
        json.dump(out, f)
    os.replace(tmp, path)

    tgt = target()
    if tgt and p.get("session_id"):
        d = {"v": 1, "e": "StatusLine", "ts": round(now, 3), "sid": p.get("session_id"),
             "model": out["model"], "ctx_pct": out["ctx_pct"], "cost_usd": out["cost_usd"],
             "cwd": (p.get("cwd") or "")[:300]}
        if tgt[2]:
            d["k"] = tgt[2]
        if isinstance(rl, dict) and rl:
            d["rl"] = rl
        data = json.dumps(d, separators=(",", ":")).encode()
        if len(data) <= MAXDGRAM:
            send(data, tgt)


def chained_command():
    import json
    cmd = os.environ.get("CLAUDIUS_STATUSLINE_CHAIN")
    if cmd:
        return cmd
    try:
        with open(os.path.join(home(), "statusline-chain.json")) as f:
            c = json.load(f)
        return c.get("command") if isinstance(c, dict) else None
    except Exception:
        return None


def statusline_main(argv=None):
    import json
    raw = read_stdin()
    try:
        p = json.loads(raw.decode("utf-8", "replace"))
        if isinstance(p, dict):
            record(p)
    except Exception:
        pass
    cmd = chained_command()
    if cmd:
        try:
            import subprocess
            kw = {}
            if IS_WINDOWS:
                kw["creationflags"] = 0x08000000  # CREATE_NO_WINDOW
            r = subprocess.run(cmd, shell=True, input=raw, stdout=subprocess.PIPE, timeout=10, **kw)
            write_stdout(r.stdout)
        except Exception:
            pass


def run_hook(argv=None):
    """`hook` entry point: run, swallow everything, flush, hard-exit 0 (skips interpreter teardown).
    argv: the arguments after the command (optional event name fallback)."""
    try:
        hook_main(argv)
    except BaseException:
        pass
    try:
        sys.stdout.flush()
    except BaseException:
        pass
    os._exit(0)


def run_statusline():
    try:
        statusline_main()
    except BaseException:
        pass
    try:
        sys.stdout.flush()
    except BaseException:
        pass
    os._exit(0)
