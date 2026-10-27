# Desktop app: architecture reference

One installable "Claudius Maximus" application for **Windows, macOS and Linux** that runs the host daemon, lives
in the system tray / menu bar (status, actions, quit), **detects the board and flashes the firmware when
needed**, and is published as GitHub Releases (an installer per OS, built by GitHub Actions). The headless
`companion run` + systemd/launchd path (`tools/install.sh`) keeps working for servers and development.

This file started as the contract between the agents that built the app (2026-10-27); it now describes what was
built and is the reference for changing it. User-facing install docs are in the README; build and release steps
in `packaging/README.md`. Dependencies: stdlib + pyserial, psutil, pillow, esptool 5 (`>=5.0,<6`, see Flasher),
plus pystray (Windows/macOS, with pyobjc-framework-Cocoa on macOS) and dbus-fast (Linux) for the tray, all declared
in `host/pyproject.toml` (the source of truth; `host/requirements.txt` mirrors it). Python 3.11+. No Qt, no Electron, no Rust.

## Process model

```
claudius-maximus app            (the tray app: what the installers launch and autostart)
 ├─ main thread: tray backend (pystray run loop on Windows/macOS; dbus-fast asyncio loop on Linux)
 ├─ worker thread: companion.daemon.run(daemon=Daemon(...))   (serial link, providers, hooks socket, status.json)
 └─ short-lived threads: first-run setup, flashing, menu actions
claudius-maximus run            (headless daemon; what the systemd/launchd service runs)
claudius-maximus status|focus|detect|flash|hook|statusline|autostart|hooks   (CLI)
```

The frozen binary (`packaging/entry.py`) starts `app` when given no arguments; `python -m companion` with no
arguments (or only options) still means `run`, for the existing services.

Only one daemon per user runs: `daemon.run()` takes `~/.claudius-maximus/daemon.lock` (`locking.py`: fcntl /
msvcrt) and returns 1 with `Daemon.error` set when another process holds it. `status` reads
`~/.claudius-maximus/status.json`, which the owning daemon rewrites every few seconds.

**Observer mode.** When the app's daemon cannot get the lock (the headless service of a script install, or a
`run` in a terminal), the app keeps running as an observer: it shows the other daemon's status.json, adds
"Another daemon owns the device (status only)" to the tooltip, disables flashing, and retries the lock every
10 s once the holder's pid is gone. Turning on Start at login (or the first run) retires the headless service, so
the app takes over within that interval.

Frozen (PyInstaller) builds: `sys.frozen` is set and `sys.executable` is the app binary. Everything that spawns
"ourselves" (hook commands, autostart entry) uses `companion.hostos.self_command()`: `[sys.executable]` when
frozen (`$APPIMAGE` instead when set, since an AppImage's mount point changes every launch), else
`[python, "-m", "companion"]` with `hostos.self_cwd()` = `host/` as the working directory.

## Modules

| Module | Responsibility |
|---|---|
| `companion/version.py` | `__version__` = app version, single source; the release tag must be `v<__version__>` (`packaging/check_version.py`) |
| `companion/locking.py` | `open_lock_file`, `try_lock`, `unlock`, `write_holder` (fcntl / msvcrt) |
| `companion/hostos.py` | `IS_MAC/IS_LINUX/IS_WINDOWS`, paths per OS (below), `self_command()`, `self_cwd()`, `pid_alive()`, `open_path()`, the app settings store, the headless service's label/unit/path |
| `companion/daemon.py`, `companion/link.py` | the daemon loop and serial link; runnable from a non-main thread; `public_status()`, `pause_device()`, `resume_device()`; `claim_device()` for other processes (CLI flash) |
| `companion/providers/hooks.py` | daemon side of the hooks IPC (unix datagram socket, or loopback UDP + `hooks.port` on Windows) |
| `companion/hookclient.py` | client side: what Claude Code runs (`hook`, `statusline`). Stdlib only, imports nothing else from `companion`, runs under `python -I -S` |
| `companion/hooksetup.py` | adds / removes our hooks and the statusLine wrapper in Claude Code's `settings.json` (backup, manifest, statusLine chaining); `install()` for the script install, `install_for_app()` for the running program |
| `host/hooks/cm_hook.py`, `cm_statusline.py`, `cm_settings.py` | thin shims (put `host/` on `sys.path`, call `hookclient` / `hooksetup`), kept so script installs and existing `settings.json` entries keep working |
| `companion/autostart.py` | `enable()`, `disable()`, `is_enabled()`, `describe()`: start the **app** at login; retires the headless service |
| `companion/__main__.py` | all subcommands; heavy modules (`app`, `flasher`) are imported lazily; `hook` / `statusline` are handled before anything else is imported |
| `companion/flasher.py`, `companion/firmware/<board>/`, `tools/fw_bundle.py` | board detection, bundled firmware images + manifest, in-process esptool flashing |
| `firmware/src/core/version.h`, `link.cpp` | `FW_VERSION`, the hello `board` field, the `dl` command (PROTOCOL.md) |
| `companion/app.py`, `companion/tray/` | app lifecycle, first run, observer mode, menu / tooltip (pure functions of `App.status()`), tray backends, icons |
| `companion/runtime_focus.py`, `providers/system.py`, `providers/sessions.py`, `sessions_text.py`, `providers/usage.py` | Windows support (psutil, ctypes user32) next to the macOS / Linux code |
| `host/pyproject.toml`, `packaging/*`, `.github/workflows/*` | PyInstaller spec, installers, CI and release workflows (`packaging/README.md`) |

## Paths (hostos.py)

| | macOS | Linux | Windows |
|---|---|---|---|
| state dir (socket or port file, status.json, statusline.json, locks, settings.json) | `~/.claudius-maximus` | same | `%USERPROFILE%\.claudius-maximus` |
| log file (`hostos.log_path()`, rotated 1 MB × 3) | `~/Library/Logs/claudius-maximus.log` | `$XDG_STATE_HOME/claudius-maximus/claudius-maximus.log` (default `~/.local/state/...`) | `%LOCALAPPDATA%\claudius-maximus\logs\claudius-maximus.log` |
| app settings (`board`, `first_run_done`, ...) | `<state dir>/settings.json` | same | same |
| Claude Code config | `$CLAUDE_CONFIG_DIR` or `~/.claude` | same | same (`%USERPROFILE%\.claude`) |
| device lock `.devlock` + `.devwant.<pid>` markers (`daemon.dev_dir()`) | repo root in a checkout (shared with `tools/dev.py`); the state dir when frozen | same | same |
| start at login (`autostart.py`) | `~/Library/LaunchAgents/com.claudius-maximus.app.plist` | `$XDG_CONFIG_HOME/autostart/claudius-maximus.desktop` | `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` value `ClaudiusMaximus` |
| headless service (`tools/install.sh`) | `~/Library/LaunchAgents/com.claudius-maximus.daemon.plist` | `~/.config/systemd/user/claudius-maximus.service` | none |

`$CLAUDIUS_HOME` overrides the state dir (tests). `hostos.settings` is a tiny JSON key/value store
(`settings_get(key, default)`, `settings_set(key, value)`, `None` removes), read on every get and written
atomically.

## Hooks IPC

- macOS/Linux: `AF_UNIX` `SOCK_DGRAM` at `~/.claudius-maximus/hooks.sock`; when that path is too long for
  `sun_path` (deep `$HOME` / `$CLAUDIUS_HOME`), `/tmp/claudius-maximus-<uid>-<crc>/hooks.sock`, used only if the
  directory is ours and `0700` (`hookclient.unix_sock_path`, shared by both sides).
- Windows (no `AF_UNIX` in CPython there): the daemon binds UDP `127.0.0.1:0` and writes `"<port> <key>\n"` to
  `~/.claudius-maximus/hooks.port` (deleted when it stops). The key is random per daemon start; the client puts
  it in the datagram as `"k"` and the daemon drops datagrams without it, so other local programs cannot inject
  fake events by guessing the port.
- Same payload (≤ 2000 bytes JSON, short keys documented in `hookclient.py`) on all platforms. No socket / port
  file = no daemon = the client does nothing. The client never prints and always exits 0.
- Hook command forms written by `hooksetup` (double-quoted, so they work in bash, zsh and cmd):

  | install | hook | statusLine |
  |---|---|---|
  | script (`tools/install.sh`), or app run from a checkout | `"<python>" -I -S "<root>/host/hooks/cm_hook.py"` | `... cm_statusline.py` |
  | frozen app | `"<exe>" hook` (`$APPIMAGE` on Linux) | `"<exe>" statusline` |
  | pip install | `"<python>" -m companion hook` | `... statusline` |

  All three are recognised as ours (`is_ours_command`), so switching between the script install and the app
  replaces the entries instead of duplicating them. `installed()` reports our entries in any form; it does not
  check that the path in them still exists.
- `settings.json` is backed up (`settings.json.bak-claudius-YYYYmmdd-HHMMSS`) before every change; an existing
  statusLine command is saved to `statusline-chain.json` and run by our wrapper; `install-manifest.json` lets
  `uninstall` restore the original shape.

## Daemon API used by the app

```python
class Daemon:
    def __init__(self, demo=False, port=None, dry_run=False, out=None)
    def run(self) -> None                      # blocking loop, returns when self.stop is True
    def public_status(self) -> dict            # same dict as status.json, built now, a deep copy, thread-safe
    def pause_device(self, why: str) -> None   # close the port, release .devlock, stop reconnecting (flasher)
    def resume_device(self) -> None            # reconnect on the next tick and send the full state
    stop: bool
    error: str                                 # set by run() when another daemon holds the lock

def run(demo=False, port=None, dry_run=False, seconds=0, daemon=None) -> int   # 0, or 1 = lock held elsewhere
def claim_device(what: str, timeout: float = 30.0)                            # context manager for other processes
def instance_lock_holder() -> int | None
```

`status.json` keys the tray uses: `pid`, `started`, `updated`, `connected`, `port`, `fw`, `board` (from the
hello), `last_hello`, `device_error`, `lock_holder`, `paused`, `error`, `hooks{listening,received,last_at,address}`,
`sessions[{id,status,name}]`, `rate_limits` (statusLine cache) and `rate_limits_at`.

## Flasher

```python
@dataclass
class Port:
    device: str            # "/dev/ttyACM0", "COM5", "/dev/cu.usbmodem..."
    kind: str              # "native" (303a:1001 USB-Serial-JTAG = the link port) | "uart" (1a86:55d3 CH343 = the 4B's USB-TO-UART port) | "other"
    vid: int; pid: int; serial: str | None; description: str

BOARDS = {"4b": "ESP32-S3-Touch-LCD-4B"}   # id -> hello BOARD_NAME (one board; the id keeps the manifest layout)
def board_id(board) -> str | None                   # id or BOARD_NAME -> "4b"
def detect() -> list[Port]                          # enumeration only, no device I/O
def link_port(ports=None) -> Port | None            # the native port, if any
def bundled() -> dict[str, Manifest]                # board id -> manifest of the firmware shipped with this app
def needs_update(fw, board) -> tuple[bool, str]     # (True, "0.2.0 -> 0.3.0") / (True, "install 0.3.0") / (False, "up to date")
def flash(board, port=None, progress=None, log=None, erase=False, *, lock=False) -> FlashResult
    # port None = auto (native first, else the CH343). progress(pct, msg). The caller frees the port
    # (app: daemon.pause_device(); CLI: daemon.claim_device()); lock=True takes the device lock itself.
    # Raises FlashError with a sentence for the user. FlashResult: board, fw_version, port, link_port
    # (native port after the reset, None if not back within 15 s), method ("reset" | "dl"), seconds.
```

esptool runs **in-process** (a frozen app has no interpreter for a subprocess; progress comes from esptool 5's
logger object). Pinned `esptool>=5.0,<6` (verified with 5.6.0); 4.8+ spellings and stdout progress are still
handled. Arguments, verified on the 4B and identical to PlatformIO's upload:

```
--chip esp32s3 --port P --baud 921600 --before default-reset --after hard-reset
write-flash -z --flash-mode dio --flash-freq 80m --flash-size 16MB
0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

Fallback, native port only, when the first attempt fails before writing anything: send `{"t":"cmd","dl":1}`
(firmware ≥ 0.3.0 reboots into the ROM download mode, PROTOCOL.md), wait for the port to come back, then run
esptool with `--before no-reset --after watchdog-reset`. `hard-reset` does not work there: DTR stays asserted
from pyserial's open, the USB-Serial-JTAG ignores the RTS pulse, and the stub keeps running.

After flashing, the native port re-enumerates (also after a CH343 flash: the chip resets); `flash()` waits for it.
Over the CH343 the link does not work (the firmware talks over the native port only), so the app and the CLI tell
the user to move the cable to USB-OTG.

Manifest (`companion/firmware/<board>/manifest.json`, written by `tools/fw_bundle.py` from the PlatformIO build;
the release zips `firmware-<board>-<fwver>.zip` hold the same files):

```json
{"board": "4b", "board_name": "ESP32-S3-Touch-LCD-4B", "fw_version": "0.3.0", "tree": "abcdef1234",
 "chip": "esp32s3", "flash_size": "16MB", "flash_mode": "dio", "flash_freq": "80m",
 "parts": [{"file": "bootloader.bin", "offset": "0x0"}, {"file": "partitions.bin", "offset": "0x8000"},
           {"file": "boot_app0.bin", "offset": "0xe000"}, {"file": "firmware.bin", "offset": "0x10000"}],
 "built": "2026-10-27T23:00:00Z"}
```

`flash_mode` is `dio` although the boards run QIO: that is what PlatformIO writes into the image header for
`board_build.flash_mode = qio`; the 2nd-stage bootloader (built for QIO) switches the flash mode itself. A
manifest whose part files are missing is ignored. Bundles are searched in `$CLAUDIUS_FIRMWARE_DIR`, the
package's `companion/firmware/`, and `<_MEIPASS>/companion/firmware/`.

Firmware version: `FW_VERSION` in `firmware/src/core/version.h`, read by `fw_bundle.py`; bump it on every release
that changes firmware, it is how installed apps know to offer an update.

CLI: `claudius-maximus detect` (ports, link port, bundled firmware), `claudius-maximus flash [--board 4b]
[--port X] [--erase]` (board default: the `board` setting, else `4b`; claims the device from a running daemon;
progress on stderr; exit 0/1).

## Tray app

`companion/app.py`:

```python
class App:
    def __init__(self, argv_opts=None, backend=None, start_daemon=True)  # starts the daemon thread; first run unless --no-setup
    def status(self) -> dict          # daemon.public_status() (or status.json when observing) + ports, update, bundled,
                                      # install_needed, autostart, hooks_installed, flash, board_choice, observer, tray_state
    def flash(self, board=None)       # worker thread: pause_device, flasher.flash with progress, resume_device; refused when observing
    def set_autostart(self, on); def autostart(self) -> bool
    def install_hooks(self, notify=True) -> bool; def hooks_installed(self) -> bool
    def open_log(self); def open_usage_page(self); def quit(self)
def main(argv) -> int                 # claudius-maximus app [--no-setup] [--demo] [--port X] [--seconds N] [-v]
```

`--no-setup` skips the first-run setup (development, tests); `--seconds N` quits after N seconds (CI smoke test).
`build_menu()`, `device_line()`, `tooltip_lines()`, `tray_state()` are pure functions of the status dict (unit
tested without a tray). The other modules are imported lazily and every call is guarded: a failing module
degrades one menu entry, never the tray.

Tray backends (`companion/tray/`, chosen by `make_backend()`): `backend_pystray.py` (Windows, macOS) and
`backend_sni.py` (Linux: StatusNotifierItem + com.canonical.dbusmenu over dbus-fast, registered with
`org.kde.StatusNotifierWatcher` and re-registered when the watcher restarts; GNOME needs the AppIndicator
extension). Common interface in `tray/base.py`: `run()`, `set_icon(image)`, `set_tooltip(title, lines)`,
`set_menu(items)` with `MenuItem(label, action=None, enabled=True, checked=None, children=None, separator=False)`,
`notify(title, body)`, `stop()`. Menu actions run off the tray thread.

Icon (`tray/icon.py`): `render_app_icon(size)` (Clawd, also the application icon: `packaging/make_icons.py`)
and `render_tray_icon(size, status)` with a badge (grey none, green ok, orange update / flashing, red error) and
the 5-hour / 7-day bars when `rate_limits` is known.

Menu (top to bottom), refreshed every 2 s: device line (`Device: connected on COM5 · fw 0.3.0 (4B)` / `Device:
not found` / `Device: found on X, not answering` / `Device: busy on X (holder)` / `Board on USB-TO-UART port: move
the cable to USB-OTG` / `Flashing firmware: N%`); `Sessions: 2 (1 working, 1 needs you)`; `Plan: 5h 42% · 7d 12%`
(when known); separator; one of `Update firmware (0.2.0 → 0.3.0)…` (connected, older fw), `Install firmware…`
(a board port seen but no hello for 10 s) or `Flash firmware…`; `Start at login` (check);
`Claude Code hooks: installed` / `Install Claude Code hooks`; `Open log`; `Open claude.ai usage`; separator;
`Quit`.

**First run** (once, `first_run_done` in the app settings): `install_hooks()` (hooksetup's timestamped backup),
then `set_autostart(True)` unless the Windows installer manages it (detected by `unins000.exe` next to the exe:
its "Start at login" task owns the Run value), then one notification listing what was done.

## Start at login (autostart.py)

The entry runs `self_command() + ["app"]` (Windows from a checkout: `pythonw.exe -c <bootstrap>`, no console).
`enable()` first retires the headless service: macOS `launchctl bootout` and the plist renamed
`*.plist.disabled-by-app`; Linux `systemctl --user disable --now` (the unit file stays). It writes the plist /
`.desktop` / Run value and returns a sentence for the notification.

Deliberately not done: `enable()` never `launchctl bootstrap`s the app's LaunchAgent (RunAtLoad would start a
second app next to the one calling it) and `disable()` never boots it out (that would kill a running app started
by launchd); the plist is read at the next login. Same on Linux: the `.desktop` file is only read at login. When
`$HOME` is not the real home directory (tests), launchctl / systemctl are never run.

`tools/uninstall.sh` removes the app's autostart entries too (shell, best effort).

## Packaging and CI

- `packaging/claudius-maximus.spec`: PyInstaller **onedir**. Windows: windowed (no console; `entry.py` re-attaches
  to the parent console for CLI commands), `.ico`. macOS: `Claudius Maximus.app` with `LSUIElement` (no Dock
  icon), `.icns`. Linux: onedir, packed as AppImage + tar.gz. Bundles `companion/firmware/**`.
- Installers: Windows Inno Setup `packaging/windows/installer.iss` (per-user, `%LOCALAPPDATA%\Programs\ClaudiusMaximus`,
  "Start at login" task = the same HKCU Run value as autostart.py, runs the app afterwards, the uninstaller runs
  `hooks uninstall` and kills the app); macOS `.dmg` (unsigned); Linux `.AppImage` + `.tar.gz`.
- Release assets: `ClaudiusMaximus-<ver>-windows-x64.exe`, `-macos-arm64.dmg`, `-macos-x64.dmg`,
  `-linux-x64.AppImage`, `-linux-x64.tar.gz`, and `firmware-<board>-<fwver>.zip`.
- `.github/workflows/ci.yml` (push / PR): unit tests on Ubuntu, macOS, Windows (Python 3.12), both firmware envs,
  a PyInstaller smoke build per OS. `.github/workflows/release.yml` (tags `v*`, or by hand): firmware job, app
  matrix (windows-latest x64, macos-14 arm64, macos-15-intel x64, ubuntu-22.04 x64), release with generated notes.
  The tag must equal `v` + `__version__`.

## Tests

`python -m unittest discover -s host/tests -v`. Every module has tests that run on all three OSes without
hardware (pyserial, esptool and dbus mocked); platform branches are tested with `unittest.mock.patch("sys.platform", ...)`
or the `hostos.IS_*` flags where feasible.

## Rules for changes

- Python 3.11+ syntax, no new runtime dependencies without a reason; keep the code style (plain functions,
  docstrings that explain the why).
- `hookclient.py` stays stdlib-only and fast (< 150 ms in a frozen build); never print from a hook.
- Only the flasher (and `tools/dev.py`) touches the real device; tests mock it.
