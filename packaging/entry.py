"""Entry point of the frozen (PyInstaller) Claudius Maximus app.

The installers, the autostart entry and a double-click all start the binary with no arguments: that means the
tray app (`app`). Any argument is passed through to the normal CLI (`status`, `hook`, `statusline`, `flash`...),
so `ClaudiusMaximus.exe hook` keeps working as the Claude Code hook command. Keep this file tiny: `hook` and
`statusline` must stay fast, so nothing heavy is imported here.
"""
import os
import sys


def _fix_std_streams() -> None:
    """Windowed Windows builds have no console: sys.stdin/stdout/stderr are None unless the parent gave us pipes
    (Claude Code does, for hooks). For CLI use from a terminal, attach to the parent's console so `status` prints;
    otherwise point the streams at os.devnull so print()/logging/argparse never crash on None."""
    if sys.platform == "win32" and sys.stdout is None and len(sys.argv) > 1 and sys.argv[1] != "app":
        try:
            import ctypes
            if ctypes.windll.kernel32.AttachConsole(-1):   # ATTACH_PARENT_PROCESS
                sys.stdout = open("CONOUT$", "w", encoding="utf-8", errors="replace")
                sys.stderr = open("CONOUT$", "w", encoding="utf-8", errors="replace")
                if sys.stdin is None:
                    sys.stdin = open("CONIN$", "r", encoding="utf-8", errors="replace")
        except Exception:
            pass
    for name, mode in (("stdin", "r"), ("stdout", "w"), ("stderr", "w")):
        if getattr(sys, name) is None:
            setattr(sys, name, open(os.devnull, mode))


def run() -> int:
    if len(sys.argv) == 1:
        sys.argv.append("app")
    # macOS passes -psn_0_12345 to apps started from Finder on old systems: treat it as "no arguments".
    if len(sys.argv) == 2 and sys.argv[1].startswith("-psn_"):
        sys.argv[1] = "app"
    _fix_std_streams()
    from companion.__main__ import main
    rc = main()
    return rc if isinstance(rc, int) else 0


if __name__ == "__main__":
    sys.exit(run())
