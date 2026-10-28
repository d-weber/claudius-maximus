"""CLI: claudius-maximus (= python -m companion) <command> ...

    run [--demo] [--port P] [--dry-run] [--seconds N] [--log [FILE]] [-v]   headless daemon (the default)
    status [--json]              daemon, device, hooks, sessions, plan limits
    focus SID                    raise the terminal running that session
    app                          tray app (companion/app.py)
    detect                       serial ports + bundled firmware (companion/flasher.py)
    flash [--board 4b] [--port P] [--erase]
    hook | statusline            what Claude Code runs (fast path: companion/hookclient.py, nothing else)
    autostart enable|disable|status
    port [NAME|auto]             which USB port is the board (several ESP32-S3 plugged in)
    update [--install] [auto on|off]   check the GitHub Releases page for a newer app (and install it)
    hooks install|uninstall|check [--no-statusline]

Backward compatible: no command, or only options (`python -m companion --demo`), means `run`.
"""
import os
import sys

COMMANDS = ("run", "status", "focus", "app", "detect", "flash", "hook", "statusline", "autostart", "hooks", "port",
            "update")


def _default_log():
    from .hostos import log_path
    return log_path()


def setup_logging(verbose: bool, log_file: str | None):
    import logging
    import logging.handlers
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
    if sys.stderr is not None and (not log_file or sys.stderr.isatty()):
        h = logging.StreamHandler(sys.stderr)
        h.setFormatter(fmt)
        root.addHandler(h)


def _ago(ts) -> str:
    import time
    if not ts:
        return "never"
    d = int(time.time() - ts)
    if d < 60:
        return f"{d}s ago"
    if d < 3600:
        return f"{d // 60}m ago"
    return f"{d // 3600}h{(d % 3600) // 60:02d}m ago"


def _pid_alive(pid) -> bool:
    from .hostos import pid_alive  # os.kill(pid, 0) would terminate the process on Windows
    return pid_alive(pid)


def cmd_status(as_json: bool) -> int:
    import json
    import time
    from .daemon import state_dir, status_path, ROOT
    from .hooksetup import installed
    from . import autostart
    from .hostos import service_installed, service_name, service_path
    from .link import find_port
    st = {}
    try:
        with open(status_path()) as f:
            st = json.load(f)
    except (OSError, ValueError):
        pass
    running = bool(st.get("pid")) and _pid_alive(st.get("pid")) and time.time() - st.get("updated", 0) < 30
    inst = installed()
    hooks_installed, statusline_installed = inst["hooks"], inst["statusline"]
    sl = {}
    try:
        with open(os.path.join(state_dir(), "statusline.json")) as f:
            sl = json.load(f)
    except (OSError, ValueError):
        pass
    # "launch_agent" / "service" describe the headless service of a source checkout (tools/install.sh), kept
    # under their old names for scripts; "autostart" is the app's own start-at-login entry (#12)
    report = {
        "daemon_running": running, "daemon": st, "device_port": find_port(),
        "hooks_installed": hooks_installed, "statusline_installed": statusline_installed,
        "autostart": autostart.is_enabled(),
        "launch_agent": service_installed(), "service": service_name(), "service_path": service_path(),
        "statusline_cache": sl, "root": str(ROOT),
    }
    if as_json:
        print(json.dumps(report, indent=1))
        return 0 if running else 1
    yes = lambda b: "yes" if b else "no"  # noqa: E731
    print(f"daemon         : {'running (pid %s, up %s)' % (st.get('pid'), _ago(st.get('started')).replace(' ago', '')) if running else 'not running'}")
    print(f"start at login : {yes(report['autostart'])}")
    if report["launch_agent"]:   # the headless service is the exception now; say what the line checks
        print(f"headless daemon: {service_name()} installed ({report['service_path']})")
    choice = st.get("port_choice") if running else None
    if choice is None:
        try:
            from .ports import selection
            choice = selection()[0] or "auto"
        except Exception:
            choice = "auto"
    print(f"device port    : {report['device_port'] or 'not found'}"
          + ("" if choice == "auto" else f"  (chosen: {choice})"))
    if running:
        if st.get("connected"):
            board = f"  board={st['board']}" if st.get("board") else ""
            print(f"device         : connected on {st.get('port')}  fw={st.get('fw') or '?'}{board}  hello {_ago(st.get('last_hello'))}")
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
    print(f"log            : {_default_log()}")
    return 0 if running else 1


def _daemon_ports() -> list[dict] | None:
    """The running daemon's view of the ports (status.json `ports`: which one is ours, its firmware), or None."""
    import json
    import time
    from .daemon import status_path
    try:
        with open(status_path()) as f:
            st = json.load(f)
    except (OSError, ValueError):
        return None
    if not (st.get("pid") and _pid_alive(st.get("pid")) and time.time() - st.get("updated", 0) < 30):
        return None
    return st.get("ports") if isinstance(st.get("ports"), list) else None


def cmd_port(name: str | None) -> int:
    """`port`: list the USB serial ports and the choice; `port NAME` / `port auto`: choose (the running daemon
    picks the change up within a few seconds, and uses that port for flashing too)."""
    from . import flasher
    from .ports import AUTO, port_label, resolve, select, selection
    ports = flasher.detect()
    if name:
        if name.lower() != AUTO and not any(p.device == name for p in ports):
            print(f"warning: {name} is not a serial port right now; kept as the choice anyway", file=sys.stderr)
        print(select(name, ports))
        return 0
    known = {p["device"]: p for p in _daemon_ports() or [] if isinstance(p, dict)}
    device, serial = selection()
    chosen = resolve(ports, device, serial)
    print(f"choice: {device or 'automatic'}" + (f" (USB serial {serial})" if serial else ""))
    if not ports:
        print("no USB serial ports found")
    for p in ports:
        d = known.get(p.device) or {"device": p.device, "kind": p.kind, "description": p.description}
        marks = ("connected" if d.get("connected") else "") + (" chosen" if chosen is p else "")
        print(f"  {port_label(d)}" + (f"   [{marks.strip()}]" if marks.strip() else ""))
    return 0


def cmd_update(install: bool, auto: str | None) -> int:
    """`update`: compare with the latest GitHub release (exit 0 up to date, 3 when newer exists, 1 on error);
    `--install`: download and install it, stopping a running app first; `update auto on|off`: the daily check."""
    from . import updater
    if auto:
        print(updater.set_auto(auto == "on"))
        return 0
    try:
        rel = updater.check()
    except updater.UpdateError as e:
        print(f"update check failed: {e}", file=sys.stderr)
        return 1
    cur = updater.current_version()
    if not updater.is_newer(rel.version, cur):
        print(f"up to date: {cur} (latest release {rel.version})")
        return 0
    print(f"update available: {rel.version} (you run {cur})  {rel.url}")
    if not install:
        why = updater.can_self_install()
        print(f"run `update --install` to install it" if not why else f"cannot self-install: {why}")
        return 3
    why = updater.can_self_install()
    if why:
        print(f"cannot self-install: {why}", file=sys.stderr)
        return 2

    def progress(pct, msg):
        print(f"\r[{pct:3d}%] {msg[:70]:70}", end="", file=sys.stderr, flush=True)
    try:
        path = updater.fetch(rel, progress)
        print(file=sys.stderr)
        # a running tray app must let go (Linux/macOS: ask it to quit; the Windows installer closes it itself)
        _stop_running_app()
        print(updater.install(path, rel.version))
    except updater.UpdateError as e:
        print(f"\nupdate failed: {e}", file=sys.stderr)
        return 1
    return 0


def _stop_running_app() -> None:
    import json
    import signal
    import time
    from .daemon import status_path
    try:
        with open(status_path()) as f:
            pid = json.load(f).get("pid")
    except (OSError, ValueError):
        return
    if not pid or pid == os.getpid() or not _pid_alive(pid) or sys.platform == "win32":
        return
    try:
        os.kill(int(pid), signal.SIGTERM)
    except OSError:
        return
    for _ in range(40):
        if not _pid_alive(pid):
            break
        time.sleep(0.25)


def cmd_detect() -> int:
    from . import flasher
    from .ports import selection
    ports = flasher.detect()
    known = {p["device"]: p for p in _daemon_ports() or [] if isinstance(p, dict)}
    if not ports:
        print("no serial ports found")
    else:
        print(f"{'port':24} {'kind':7} {'vid:pid':10} description")
        for p in ports:
            k = known.get(p.device) or {}
            ours = f"  [Claudius Maximus fw {k['fw']}]" if k.get("fw") else "  [Claudius Maximus]" if k.get("ours") else ""
            print(f"{p.device:24} {p.kind:7} {p.vid or 0:04x}:{p.pid or 0:04x}  {p.description}{ours}")
    link = flasher.link_port(ports)
    device, _serial = selection()
    print(f"link port: {link.device if link else 'none'}" + (f"  (chosen: {device})" if device else ""))
    b = flasher.bundled()
    if not b:
        print("bundled firmware: none in this build")
    for board, m in sorted(b.items()):
        print(f"bundled firmware: {board:3} fw {m.fw_version} (built {m.built})")
    return 0


def cmd_flash(board: str | None, port: str | None, erase: bool) -> int:
    from contextlib import ExitStack
    from . import flasher
    from .daemon import claim_device
    from .hostos import settings_get
    board = board or settings_get("board") or "4b"

    def progress(pct, msg):
        print(f"\r[{pct:3d}%] {msg[:70]:70}", end="", file=sys.stderr, flush=True)

    try:
        # ask a running daemon (headless or the tray app) to release the port; flash() itself doesn't lock
        with ExitStack() as stack:
            try:
                stack.enter_context(claim_device("flash", timeout=30))
            except TimeoutError as e:
                print(f"\nflash failed: {e}", file=sys.stderr)
                return 1
            except OSError as e:   # lock file not creatable (#11): one line and exit 1, not a traceback
                where = f" {e.filename}" if e.filename else ""
                print(f"\nflash failed: cannot create the device lock{where}: {e.strerror or e}", file=sys.stderr)
                return 1
            res = flasher.flash(board, port=port, progress=progress, erase=erase)
    except flasher.FlashError as e:
        print(f"\nflash failed: {e}", file=sys.stderr)
        return 1
    print(f"\nflashed {res.board} fw {res.fw_version} via {res.port} in {res.seconds:.0f}s", file=sys.stderr)
    print(f"link port: {res.link_port or 'not back yet'}", file=sys.stderr)
    kind = next((p.kind for p in flasher.detect() if p.device == res.port), None)
    if res.link_port is None and kind == "uart":
        print("warning: flashed over the USB-TO-UART port; move the cable to USB-OTG for the host link",
              file=sys.stderr)
    return 0


def cmd_autostart(action: str) -> int:
    from . import autostart
    if action == "enable":
        print(autostart.enable())
    elif action == "disable":
        print(autostart.disable())
    else:
        print(autostart.describe())   # a query: exit 0 whether enabled or not
    return 0


def cmd_hooks(action: str, statusline: bool) -> int:
    from . import hooksetup
    try:
        if action == "install":
            return hooksetup.install_for_app(statusline)
        if action == "uninstall":
            return hooksetup.uninstall()
    except (ValueError, OSError) as e:
        print(f"error: cannot update {hooksetup.settings_path()}: {e}", file=sys.stderr)
        return 2
    rc = hooksetup.check()
    inst = hooksetup.installed()
    print(f"hooks: {'installed' if inst['hooks'] else 'not installed'}, "
          f"statusLine wrapper: {'installed' if inst['statusline'] else 'not installed'} "
          f"({hooksetup.settings_path()})")
    for c in inst["commands"]:
        print(f"  {c}")
    return rc


def build_parser():
    import argparse
    ap = argparse.ArgumentParser(prog="claudius-maximus", description="Claudius Maximus desk companion")
    sub = ap.add_subparsers(dest="command", metavar="command")

    def run_opts(p):
        p.add_argument("--demo", action="store_true", help="stream fake data instead of real sessions")
        p.add_argument("--port", help="serial port (auto-detected by default)")
        p.add_argument("--dry-run", action="store_true", help="print the JSON stream to stdout instead of the device")
        p.add_argument("--seconds", type=float, default=0, help="exit after N seconds (testing)")
        p.add_argument("--log", nargs="?", const=_default_log(), default=None,
                       help=f"log to a rotating file (default {_default_log()})")
        p.add_argument("-v", "--verbose", action="store_true")

    run_opts(sub.add_parser("run", help="run the headless daemon (default)"))
    p = sub.add_parser("status", help="daemon, device, hooks, sessions, plan limits")
    p.add_argument("--json", action="store_true", help="machine-readable output")
    p = sub.add_parser("focus", help="raise the terminal running a session")
    p.add_argument("sid", nargs="?", default="")
    p.add_argument("-v", "--verbose", action="store_true")
    sub.add_parser("app", help="tray / menu bar app (runs the daemon)", add_help=False)
    sub.add_parser("detect", help="list serial ports and the bundled firmware")
    p = sub.add_parser("flash", help="flash the bundled firmware")
    p.add_argument("--board", choices=["4b"])
    p.add_argument("--port")
    p.add_argument("--erase", action="store_true", help="erase the whole flash first")
    sub.add_parser("hook", help="Claude Code hook relay (reads the hook JSON on stdin)")
    sub.add_parser("statusline", help="Claude Code statusLine wrapper")
    p = sub.add_parser("autostart", help="start the tray app at login")
    p.add_argument("action", choices=["enable", "disable", "status"], nargs="?", default="status")
    p = sub.add_parser("port", help="which USB serial port is the board (list, or choose NAME / auto)")
    p.add_argument("name", nargs="?", default=None, metavar="NAME|auto")
    p = sub.add_parser("update", help="check the GitHub Releases page for a newer app")
    p.add_argument("--install", action="store_true", help="download and install it")
    p.add_argument("auto", nargs="*", help="`auto on` / `auto off`: the daily check")
    p = sub.add_parser("hooks", help="Claude Code hooks in ~/.claude/settings.json")
    p.add_argument("action", choices=["install", "uninstall", "check"], nargs="?", default="check")
    p.add_argument("--no-statusline", action="store_true")
    return ap


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    # fast paths first: nothing but companion.hookclient (stdlib) gets imported
    if argv and argv[0] == "hook":
        from .hookclient import run_hook
        run_hook(argv[1:])
    if argv and argv[0] == "statusline":
        from .hookclient import run_statusline
        run_statusline()
    if argv and argv[0] == "app":
        from .app import main as app_main
        return app_main(argv[1:]) or 0
    if not argv or (argv[0].startswith("-") and argv[0] not in ("-h", "--help")):
        argv = ["run"] + argv  # backward compatible default
    a = build_parser().parse_args(argv)
    if a.command == "status":
        return cmd_status(a.json)
    if a.command == "detect":
        return cmd_detect()
    if a.command == "flash":
        return cmd_flash(a.board, a.port, a.erase)
    if a.command == "autostart":
        setup_logging(False, None)
        return cmd_autostart(a.action)
    if a.command == "hooks":
        return cmd_hooks(a.action, not a.no_statusline)
    if a.command == "port":
        return cmd_port(a.name)
    if a.command == "update":
        setup_logging(False, None)
        auto = None
        if a.auto:
            if len(a.auto) != 2 or a.auto[0] != "auto" or a.auto[1] not in ("on", "off"):
                print("usage: update [--install] | update auto on|off", file=sys.stderr)
                return 2
            auto = a.auto[1]
        return cmd_update(a.install, auto)
    if a.command == "focus":
        setup_logging(a.verbose, None)
        from .runtime_focus import focus_session
        print(focus_session(a.sid or ""))
        return 0
    setup_logging(a.verbose, a.log)
    from .daemon import run
    return run(demo=a.demo, port=a.port, dry_run=a.dry_run, seconds=a.seconds) or 0


if __name__ == "__main__":
    sys.exit(main())
