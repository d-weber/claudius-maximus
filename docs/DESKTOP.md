# Desktop app: architecture contract

Goal: one installable "Claudius Maximus" application for **Windows, macOS and Linux** that runs the host daemon,
lives in the system tray / menu bar (shows the data, lets you quit), **detects the board and flashes the firmware
when needed**, and is published as GitHub Releases (installer per OS, built by GitHub Actions). The headless
`companion run` + systemd/launchd path keeps working for servers and development.

This document is the contract between the modules. Each module is owned by one agent/author; shared files are
listed at the end. Keep to the Python already in the repo: stdlib + pyserial + psutil + pillow + esptool, plus
pystray (Windows/macOS) and dbus-fast (Linux) for the tray. No Qt, no Electron, no Rust.

## Process model

```
claudius-maximus app            (the tray app: what the installers launch and autostart)
 ├─ main thread: tray backend (pystray run loop on Windows/macOS; dbus-fast asyncio loop on Linux)
 └─ daemon thread: companion.daemon.Daemon().run()   (serial link, providers, hooks socket, status.json)
claudius-maximus run            (headless daemon, unchanged behaviour; systemd/launchd use it)
claudius-maximus status|focus|flash|detect|hook|statusline|autostart|hooks   (CLI)
```

Only one daemon per user may run (existing `daemon.lock` single-instance lock in `~/.claudius-maximus`). The app
owns the daemon when it runs; `status` keeps reading `~/.claudius-maximus/status.json`.

Frozen (PyInstaller) builds: `sys.frozen` is set and `sys.executable` is the app binary. Everything that spawns
"ourselves" (hooks command, autostart entry) must use `companion.hostos.self_command()`:

```python
def self_command() -> list[str]:
    """argv prefix that runs this program: [sys.executable] when frozen, else [python, "-m", "companion"]."""
```

## Module map and owners

| Module | Owner | Responsibility |
|---|---|---|
| `companion/version.py` | seed (done) | `__version__` = app version, single source; release tag must be `v<__version__>` |
| `companion/locking.py` | seed (done) | `open_lock_file`, `try_lock`, `unlock`, `write_holder` (fcntl / msvcrt) |
| `companion/hostos.py` | core-xplat | `IS_WINDOWS`, paths per OS (see below), `self_command()`, `open_path(path)` (xdg-open / open / os.startfile) |
| `companion/daemon.py`, `companion/link.py` | core-xplat | use `locking.py` instead of fcntl; `Daemon` must be runnable from a non-main thread (`signal` only in `run()` when on the main thread); add `Daemon.pause_device()` / `resume_device()` (see flasher) and `Daemon.public_status() -> dict` |
| `companion/providers/hooks.py`, `host/hooks/cm_hook.py`, `cm_statusline.py`, `cm_settings.py` | core-xplat | hooks IPC that also works on Windows; hook command for frozen builds |
| `companion/autostart.py` | core-xplat | `enable()`, `disable()`, `is_enabled()`: start the **app** at login (LaunchAgent / autostart .desktop / HKCU Run) |
| `companion/__main__.py` | core-xplat | all subcommands; lazy-import the heavy ones (`app`, `flash`, `detect`); `hook`/`statusline` fast paths must import nothing heavy |
| `companion/flasher.py`, `companion/firmware/` (manifest format), `tools/fw_bundle.py`, `firmware/src/core/version.h` + hello | flasher | board detection, esptool flashing, bundled firmware images, firmware version |
| `companion/tray/*`, `companion/app.py` | tray | tray icon + menu on the 3 OSes, app lifecycle, first-run setup, notifications |
| `companion/runtime_focus.py`, `providers/system.py`, `providers/sessions.py`, `sessions_text.py`, `providers/usage.py` | windows-port | make the providers and Focus work on Windows (psutil, ctypes user32), keep macOS/Linux behaviour byte-for-byte |
| `host/pyproject.toml`, `packaging/*`, `.github/workflows/*`, `.gitignore` | packaging-ci | PyInstaller spec, installers (Inno Setup, dmg, AppImage), CI tests + release workflow, icons from `companion.tray.icon` |
| `README.md`, `docs/*.md`, `tools/install.sh`, `tools/uninstall.sh` | docs (last) | user-facing install docs per OS, migration from the script install |

## Paths (hostos.py)

| | macOS | Linux | Windows |
|---|---|---|---|
| state dir (socket, status.json, locks, settings) | `~/.claudius-maximus` | same | `%USERPROFILE%\.claudius-maximus` |
| logs | `~/Library/Logs/claudius-maximus.log` | `$XDG_STATE_HOME/claudius-maximus/` | `%LOCALAPPDATA%\claudius-maximus\logs\` |
| app settings (board choice, autostart...) | `~/.claudius-maximus/settings.json` | same | same |
| Claude Code config | `$CLAUDE_CONFIG_DIR` or `~/.claude` | same | `%USERPROFILE%\.claude` |

`companion.hostos.settings` is a tiny JSON key/value store (`get(key, default)`, `set(key, value)`) at the app
settings path, used by tray (board choice, "auto flash") and autostart.

## Hooks IPC (core-xplat)

- macOS/Linux: unchanged, `AF_UNIX` `SOCK_DGRAM` at `~/.claudius-maximus/hooks.sock`.
- Windows: `AF_INET` UDP bound to `127.0.0.1:0`; the daemon writes the port number to
  `~/.claudius-maximus/hooks.port`; `cm_hook.py` / `cm_statusline.py` read it and send one datagram. Same
  payload (≤ 2 KB JSON) on all platforms. The hook scripts stay stdlib-only and must run with `python -I -S`.
- Hook command (`cm_settings.install`): when a `python3` exists, `"<python>" -I -S "<cm_hook.py>"`; in a frozen
  app use `"<exe>" hook` / `"<exe>" statusline` (the exe must handle these before importing anything heavy;
  target < 150 ms). Quote with double quotes so the command works in bash and cmd.
- `cm_settings.install(root, python, statusline)` keeps its signature; add `install_for_app()` which picks the
  right command for the running program.

## Daemon API used by the app (core-xplat)

```python
class Daemon:
    def run(self) -> None                      # blocking loop, returns when self.stop is True
    def public_status(self) -> dict            # same dict as status.json, cheap, thread-safe (copy)
    def pause_device(self, why: str) -> None   # close the serial port and stop reconnecting (flasher needs the port)
    def resume_device(self) -> None
    stop: bool
```

`status.json` keys the tray relies on (already there unless noted): `pid`, `started`, `connected`, `port`, `fw`,
`board` (new: from the device hello), `last_hello`, `device_error`, `hooks{listening,received,last_at}`,
`sessions[{id,status,name}]`, `rate_limits` (new: copy of statusline cache `rate_limits`, or `{}`), `updated`.

## Flasher (flasher)

```python
@dataclass
class Port:
    device: str            # "/dev/ttyACM0", "COM5", "/dev/cu.usbmodem..."
    kind: str              # "native" (303a:1001 USB-Serial-JTAG = the link port) | "uart" (1a86:55d3 CH343 = 4B's USB-TO-UART port) | "other"
    vid: int; pid: int; serial: str | None; description: str

def detect() -> list[Port]                          # no device I/O, just enumeration
def link_port(ports=None) -> Port | None            # the native port, if any
def bundled() -> dict[str, Manifest]                # board id -> manifest of the firmware shipped with this app
def needs_update(fw: str | None, board: str | None) -> tuple[bool, str]   # (True, "0.2.0 -> 0.3.0") / (False, "up to date")
def flash(board: str, port: str | None = None, progress=None, log=None, erase: bool = False) -> FlashResult
    # board: "4b" | "v4"; port None = auto (prefer native, else uart). progress(pct: int, msg: str).
    # Runs esptool in-process (esptool.main([...]) or the API), never a subprocess. Raises FlashError with a
    # human sentence on failure. Must work over the native USB-Serial-JTAG port AND the CH343 UART port.
```

Manifest (`companion/firmware/<board>/manifest.json`, written by `tools/fw_bundle.py` from the PlatformIO build):

```json
{"board": "4b", "board_name": "ESP32-S3-Touch-LCD-4B", "fw_version": "0.3.0", "tree": "abcdef1234",
 "chip": "esp32s3", "flash_size": "16MB", "flash_mode": "qio", "flash_freq": "80m",
 "parts": [{"file": "bootloader.bin", "offset": "0x0"}, {"file": "partitions.bin", "offset": "0x8000"},
           {"file": "boot_app0.bin", "offset": "0xe000"}, {"file": "firmware.bin", "offset": "0x10000"}],
 "built": "2026-10-27T23:00:00Z"}
```

Board ids: `4b` (default) and `v4`. The device hello reports `board` (its `BOARD_NAME`); map names to ids in
`flasher.BOARDS`. Firmware version lives in `firmware/src/core/version.h` (`FW_VERSION`), read by `fw_bundle.py`.
The device may also accept `{"t":"cmd","dl":1}` = reboot into the ROM download mode, as a fallback when the
DTR/RTS reset doesn't work on a platform; the flasher sends it when the link port is open and the daemon paused.

CLI: `claudius-maximus detect` (table of ports + bundled firmware), `claudius-maximus flash [--board 4b|v4]
[--port X] [--erase]` (progress on stderr, exit 0/1).

## Tray app (tray)

`companion/app.py`:

```python
class App:
    def __init__(self, argv_opts)       # starts Daemon in a thread, loads settings, installs hooks on first run
    def status(self) -> dict            # daemon.public_status() + {"update": needs_update(...), "ports": detect()}
    def flash(self, board=None)         # pause daemon device, flasher.flash with progress -> tray tooltip/notification, resume
    def set_autostart(self, on: bool); def autostart(self) -> bool
    def open_log(self); def open_usage_page(self)
    def quit(self)
def main(argv) -> int                   # `claudius-maximus app`; also the frozen app's default entry point
```

Tray backends (`companion/tray/`): `backend_pystray.py` (Windows, macOS: pystray + Pillow images),
`backend_sni.py` (Linux: StatusNotifierItem + com.canonical.dbusmenu over dbus-fast, modelled on the existing
`~/projects/claude_system_indicator/claude_tray.py` which works on this KDE Wayland box; register with
`org.kde.StatusNotifierWatcher`, re-register when the watcher restarts). Common interface in `tray/base.py`:

```python
class TrayBackend:
    def __init__(self, on_ready, on_quit): ...
    def run(self) -> None                       # blocks on the main thread
    def set_icon(self, image: PIL.Image.Image)  # several sizes derived internally
    def set_tooltip(self, title: str, lines: list[str])
    def set_menu(self, items: list[MenuItem])   # MenuItem(label, action=None, enabled=True, checked=None, children=None, separator=False)
    def notify(self, title: str, body: str)     # desktop notification, best effort
    def stop(self)
```

Icon (`tray/icon.py`): `render_app_icon(size) -> Image` (Clawd: orange rounded square with eyes, the
application icon used by packaging-ci) and `render_tray_icon(size, status) -> Image` (same face, with a state
badge: grey = no device, green = connected, orange = update available / flashing, red = error; plus the two usage
bars 5-hour / 7-day as in the claude_tray helper when `rate_limits` is known). Must look right at 16/22/32 px and
in both light and dark trays (use a template-style monochrome variant on macOS if simpler).

Menu (top to bottom): status lines (disabled items): `Device: connected on COM5 · fw 0.3.0 (4B)` / `Device: not
found` / `Board on USB-TO-UART port: move the cable to USB-OTG`; `Sessions: 2 (1 working, 1 needs you)`;
`Plan: 5h 42% · 7d 12%`; separator; `Update firmware (0.2.0 → 0.3.0)…` (shown only when needed) or `Install
firmware…` (device port seen but no hello for 10 s) or `Flash firmware ▸ 4B / V4` submenu; `Start at login`
(check item); `Claude Code hooks: installed` / `Install Claude Code hooks`; `Open log`; `Open claude.ai usage`;
separator; `Quit`. Refresh every 2 s from `app.status()`.

First run: install the hooks + statusLine wrapper (with the existing timestamped settings.json backup) and
enable autostart, then notify once. Setting `first_run_done` in app settings.

## Packaging and CI (packaging-ci)

- `packaging/claudius-maximus.spec` (PyInstaller **onedir**; Windows: windowed/no console, `.ico`; macOS: `.app`
  bundle with `LSUIElement = 1` (no Dock icon), `.icns`; Linux: onedir + AppRun). Entry: `packaging/entry.py`
  calling `companion.__main__.main()` with `app` as the default subcommand when no args are given. Hidden imports
  for the platform-conditional deps; bundle `companion/firmware/**` as data.
- Installers: Windows Inno Setup `packaging/windows/installer.iss` (per-user install, "Start at login" task, runs
  the app after install, uninstall also runs `claudius-maximus hooks uninstall`); macOS `.dmg` (hdiutil; unsigned,
  README says how to open); Linux `.AppImage` (appimagetool) + `.tar.gz`; all named
  `ClaudiusMaximus-<version>-<os>-<arch>.<ext>`.
- `.github/workflows/ci.yml`: on push/PR: unittest on ubuntu/macos/windows (Python 3.12) + firmware build
  (both envs) + PyInstaller smoke build. `.github/workflows/release.yml`: on tag `v*` (and workflow_dispatch):
  firmware job → artifacts; app matrix (windows-latest x64, macos-14 arm64, macos-13 x64, ubuntu-22.04 x64) builds
  with the firmware images in `companion/firmware/`; release job attaches installers + the raw firmware bundles
  (`firmware-<board>-<fwver>.zip`) with auto-generated notes. Tag must equal `v` + `companion.version.__version__`.
- `packaging/make_icons.py`: writes `packaging/icons/app.{png,ico,icns}` from `companion.tray.icon.render_app_icon`.

## Tests

`python -m unittest discover -s host/tests -v` (unittest, like `test_sessions.py`). Every module ships tests that
run on all three OSes without hardware (mock pyserial/esptool/dbus); platform-specific branches are tested with
`unittest.mock.patch("sys.platform", ...)` where feasible.

## Shared files and rules

- Do not run `git commit`, `git checkout`, `git stash` or anything that changes branches: the integrator commits.
- Only the owner edits a module. If you need a change in someone else's module, write the exact request in your
  final report (file, function, what and why) and code against the contract above in the meantime.
- Touch the real device (`/dev/ttyACM0`) only if you are the flasher agent; everyone else mocks it. The daemon
  is running as a systemd user service on this machine (`systemctl --user status claudius-maximus`).
- Python 3.11+ syntax, no new runtime dependencies beyond the ones in `host/pyproject.toml` without a reason in
  the report. Keep the existing code style (plain functions, docstrings that explain the why).
