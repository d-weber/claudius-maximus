"""Claudius Maximus — Claude Code hook relay.  MUST stay tiny, fast and silent.

Installed in ~/.claude/settings.json for several hook events (see host/hooks/cm_settings.py) as a command:
    "<root>/host/.venv/bin/python" -I -S "<root>/host/hooks/cm_hook.py"
(a frozen desktop app installs `"<exe>" hook` instead, which runs the same code).

The logic lives in host/companion/hookclient.py (stdlib only; `companion/__init__.py` is empty, so importing
it pulls in nothing else): read the hook JSON from stdin, send ONE datagram (<= 2000 bytes) to the daemon
(unix socket on macOS/Linux, UDP to 127.0.0.1:<hooks.port> on Windows). If the daemon is not running or
anything at all goes wrong, it silently does nothing. Never prints to stdout, always exits 0.
"""
import os
import sys

if __name__ == "__main__":
    try:
        sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        from companion.hookclient import run_hook
    except BaseException:
        try:
            sys.stdin.buffer.read()
        except BaseException:
            pass
        os._exit(0)
    run_hook(sys.argv[1:])
