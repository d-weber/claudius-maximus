"""CLI: python -m companion [run|status|focus SID] [--demo] [--dry-run] ..."""
import argparse
import json
import logging
import logging.handlers
import os
import sys
import time

DEFAULT_LOG = os.path.expanduser("~/Library/Logs/claudius-maximus.log")


def setup_logging(verbose: bool, log_file: str | None):
    level = logging.DEBUG if verbose else logging.INFO
    fmt = logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s")
    root = logging.getLogger()
    root.setLevel(level)
    if log_file:
        try:
            os.makedirs(os.path.dirname(log_file), exist_ok=True)
            h = logging.handlers.RotatingFileHandler(log_file, maxBytes=1_000_000, backupCount=3)
            h.setFormatter(fmt)
            root.addHandler(h)
        except OSError as e:
            print(f"cannot open log file {log_file}: {e}", file=sys.stderr)
    if not log_file or sys.stderr.isatty():
        h = logging.StreamHandler(sys.stderr)
        h.setFormatter(fmt)
        root.addHandler(h)


def _ago(ts) -> str:
    if not ts:
        return "never"
    d = int(time.time() - ts)
    if d < 60:
        return f"{d}s ago"
    if d < 3600:
        return f"{d // 60}m ago"
    return f"{d // 3600}h{(d % 3600) // 60:02d}m ago"


def _pid_alive(pid) -> bool:
    try:
        os.kill(int(pid), 0)
        return True
    except (OSError, TypeError, ValueError):
        return False


def cmd_status(as_json: bool) -> int:
    from .daemon import state_dir, status_path, ROOT
    from .link import find_port
    st = {}
    try:
        with open(status_path()) as f:
            st = json.load(f)
    except (OSError, ValueError):
        pass
    running = bool(st.get("pid")) and _pid_alive(st.get("pid")) and time.time() - st.get("updated", 0) < 30
    settings = os.path.expanduser("~/.claude/settings.json")
    hooks_installed = statusline_installed = False
    try:
        with open(settings) as f:
            s = f.read()
        hooks_installed = "cm_hook.py" in s
        statusline_installed = "cm_statusline.py" in s
    except OSError:
        pass
    sl = {}
    try:
        with open(os.path.join(state_dir(), "statusline.json")) as f:
            sl = json.load(f)
    except (OSError, ValueError):
        pass
    plist = os.path.expanduser("~/Library/LaunchAgents/com.claudius-maximus.daemon.plist")
    report = {
        "daemon_running": running, "daemon": st, "device_port": find_port(),
        "hooks_installed": hooks_installed, "statusline_installed": statusline_installed,
        "launch_agent": os.path.exists(plist), "statusline_cache": sl, "root": str(ROOT),
    }
    if as_json:
        print(json.dumps(report, indent=1))
        return 0 if running else 1
    yes = lambda b: "yes" if b else "no"  # noqa: E731
    print(f"daemon         : {'running (pid %s, up %s)' % (st.get('pid'), _ago(st.get('started')).replace(' ago', '')) if running else 'not running'}")
    print(f"launch agent   : {yes(report['launch_agent'])}")
    print(f"device port    : {report['device_port'] or 'not found'}")
    if running:
        if st.get("connected"):
            print(f"device         : connected on {st.get('port')}  fw={st.get('fw') or '?'}  hello {_ago(st.get('last_hello'))}")
        else:
            why = st.get("device_error") or "?"
            if st.get("lock_holder") and "lock" in why:
                why += f" ({st['lock_holder']})"
            print(f"device         : not connected — {why}")
        h = st.get("hooks") or {}
        print(f"hooks          : installed={yes(hooks_installed)} listening={yes(h.get('listening'))} "
              f"received={h.get('received', 0)} last {_ago(h.get('last_at'))}")
        ss = st.get("sessions") or []
        print(f"sessions       : {len(ss)}")
        for s in ss[:12]:
            print(f"   {s.get('id')}  {s.get('status') or '?':8} {s.get('name') or ''}")
    else:
        print(f"hooks          : installed={yes(hooks_installed)}")
    rl = sl.get("rate_limits") or {}
    if rl:
        parts = [f"{k} {v.get('used_percentage')}%" for k, v in rl.items() if isinstance(v, dict)]
        print(f"plan limits    : {', '.join(parts)} (seen {_ago(sl.get('rate_limits_at'))})")
    else:
        print(f"plan limits    : {'unknown (statusLine wrapper not installed)' if not statusline_installed else 'not seen yet'}")
    print(f"log            : {DEFAULT_LOG}")
    return 0 if running else 1


def main():
    ap = argparse.ArgumentParser(prog="companion", description="Claudius Maximus host daemon",
                                 epilog="commands: run (default) | status [--json] | focus SID")
    ap.add_argument("command", nargs="?", default="run", choices=["run", "status", "focus"])
    ap.add_argument("arg", nargs="?", help="session id for `focus`")
    ap.add_argument("--demo", action="store_true", help="stream fake data instead of real sessions")
    ap.add_argument("--port", help="serial port (auto-detected by default)")
    ap.add_argument("--dry-run", action="store_true", help="print the JSON stream to stdout instead of the device")
    ap.add_argument("--seconds", type=float, default=0, help="exit after N seconds (testing)")
    ap.add_argument("--log", nargs="?", const=DEFAULT_LOG, default=None,
                    help=f"log to a rotating file (default {DEFAULT_LOG})")
    ap.add_argument("--json", action="store_true", help="status: machine-readable output")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    if a.command == "status":
        sys.exit(cmd_status(a.json))
    setup_logging(a.verbose, a.log)
    if a.command == "focus":
        from .runtime_focus import focus_session
        print(focus_session(a.arg or ""))
        return
    from .daemon import run
    sys.exit(run(demo=a.demo, port=a.port, dry_run=a.dry_run, seconds=a.seconds) or 0)


if __name__ == "__main__":
    main()
