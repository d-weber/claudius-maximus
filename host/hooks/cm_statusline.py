"""Claudius Maximus — Claude Code statusLine wrapper.

Installed as the user's `statusLine.command` (tools/install.sh). Claude Code pipes a JSON blob to the status
line command after each assistant message (and on its refresh timer). That JSON contains, for claude.ai
Pro/Max subscribers, the plan limits:
    "rate_limits": {"five_hour": {"used_percentage": 23.5, "resets_at": 1738425600},
                    "seven_day": {"used_percentage": 41.2, "resets_at": 1738857600}}
(each window may be absent; see https://code.claude.com/docs/en/statusline).

This wrapper:
 1. writes the useful parts atomically to ~/.claudius-maximus/statusline.json (format documented in
    host/companion/providers/hooks.py — read by providers/usage.py),
 2. sends a {"e":"StatusLine",...} datagram to the daemon socket (live per-session ctx%/cost/model),
 3. chains to the user's previous status line command (saved by the installer in
    ~/.claudius-maximus/statusline-chain.json as {"command": "..."}), feeding it the same stdin and
    printing its output, so the user's status line looks exactly as before.
Any failure in 1/2 is ignored; 3 always runs.
"""
import json
import os
import subprocess
import sys
import time


def home():
    return os.environ.get("CLAUDIUS_HOME") or os.path.join(os.path.expanduser("~"), ".claudius-maximus")


def record(raw, p):
    base = home()
    if not os.path.isdir(base):
        return
    now = time.time()
    path = os.path.join(base, "statusline.json")
    prev = {}
    try:
        with open(path) as f:
            prev = json.load(f)
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

    sock = os.path.join(base, "hooks.sock")
    if os.path.exists(sock) and p.get("session_id"):
        import socket
        d = {"v": 1, "e": "StatusLine", "ts": round(now, 3), "sid": p.get("session_id"),
             "model": out["model"], "ctx_pct": out["ctx_pct"], "cost_usd": out["cost_usd"],
             "cwd": (p.get("cwd") or "")[:300]}
        if isinstance(rl, dict) and rl:
            d["rl"] = rl
        data = json.dumps(d, separators=(",", ":")).encode()
        if len(data) <= 2000:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            s.setblocking(False)
            try:
                s.sendto(data, sock)
            finally:
                s.close()


def chained_command():
    cmd = os.environ.get("CLAUDIUS_STATUSLINE_CHAIN")
    if cmd:
        return cmd
    try:
        with open(os.path.join(home(), "statusline-chain.json")) as f:
            c = json.load(f)
        return c.get("command") if isinstance(c, dict) else None
    except Exception:
        return None


def main():
    raw = sys.stdin.buffer.read()
    try:
        p = json.loads(raw.decode("utf-8", "replace"))
        if isinstance(p, dict):
            record(raw, p)
    except Exception:
        pass
    cmd = chained_command()
    if cmd:
        try:
            r = subprocess.run(cmd, shell=True, input=raw, stdout=subprocess.PIPE, timeout=10)
            sys.stdout.buffer.write(r.stdout)
            sys.stdout.flush()
        except Exception:
            pass


if __name__ == "__main__":
    try:
        main()
    except BaseException:
        pass
    sys.exit(0)
