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


def _restore_child_env() -> None:
    """PyInstaller's bootloader points LD_LIBRARY_PATH (Linux) / DYLD_LIBRARY_PATH (macOS) at the bundle so the
    frozen Python finds its own libraries, and every child process inherits that: `systemctl` then loads the
    bundled libcrypto and dies with "version OPENSSL_3.4.0 not found" (seen on the 0.3.0 AppImage: the first run
    never retired the systemd service), xdg-open/kdotool/hostnamectl are at the same risk. The bootloader keeps
    the original value in *_ORIG; our own libraries are already loaded, so put the user's environment back for
    the children (os.environ only affects processes we start)."""
    if not getattr(sys, "frozen", False):
        return
    for var in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"):
        orig = os.environ.pop(var + "_ORIG", None)
        if orig is not None:
            os.environ[var] = orig
        elif var in os.environ:
            del os.environ[var]


def _hook_fast_path() -> None:
    """`hook` / `statusline`: what Claude Code runs on every event. Straight to companion.hookclient, before
    _fix_std_streams (it would replace a missing stdin with os.devnull, hiding Claude Code's pipe from the
    GetStdHandle fallback in hookclient.read_stdin, and AttachConsole is pointless here) and without importing
    companion.__main__. Never raises, never returns: exit status 0 whatever happens (a windowed build would
    otherwise show an error dialog, and a non-zero hook exit is reported by Claude Code)."""
    try:
        from companion import hookclient
        if sys.argv[1] == "hook":
            hookclient.run_hook(sys.argv[2:])
        else:
            hookclient.run_statusline()
    except BaseException:
        pass
    os._exit(0)


def run() -> int:
    _restore_child_env()
    if len(sys.argv) > 1 and sys.argv[1] in ("hook", "statusline"):
        _hook_fast_path()
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
