# Development, command line and internals

Everything that is not needed to install and use the device. The user-facing guide is the [README](../README.md).

## Repository layout

```
PROTOCOL.md                 host ⇄ device protocol (source of truth for data)
firmware/                   PlatformIO, Arduino-ESP32 3.x, LVGL 8.4, ArduinoJson 7
  src/core/                 display/touch HAL, state model, serial link, settings, perf probe
  src/ui/                   tile manager, theme (palette, fonts, status colours)
  src/screens/              one file per screen; room/ = Clawd's Studio (the home screen's pixel-art room and its
                              behaviour director, portable C++: tools/room_sim.py renders it on the host)
  src/fx/                   alerts overlay, auto-dim / screensaver / night clock
host/companion/             Python daemon, data providers, tray app (app.py, tray/), flasher, hooks client/installer
host/hooks/                 Claude Code hook + statusLine wrapper scripts (thin shims over companion/)
host/launchd/, host/systemd/  background service templates (macOS LaunchAgent, Linux systemd user unit)
packaging/, .github/        PyInstaller spec, installers, CI and release workflows (packaging/README.md)
tools/                      dev.py (build/flash/screenshot/perf), fw_bundle.py, install/uninstall, bench, udev rule
docs/                       DESKTOP.md (desktop app architecture), PERF.md (measurements), STATUS.md, TEAM.md
```

## How it works

```
 Claude Code sessions ──hooks (unix datagram; UDP on Windows)──┐
 ~/.claude transcripts + session registry ─────────────────────┤
 statusLine JSON (plan limits) ────────────────────────────────┼─> companion daemon ──USB serial, JSON lines──> device
 host metrics ─────────────────────────────────────────────────┘   (tray app, or   <── actions (focus a session)
                                                                     a headless service)
```

- A small **Python daemon** on the host (`host/companion`, macOS, Linux or Windows) reads Claude Code's local data
  and receives real-time hook events. It sends the device a state snapshot whenever something changes, plus partial
  updates and events. A permission prompt shows up on the screen within about 50 ms. The daemon uses about 1 % CPU.
- The **desktop app** (`claudius-maximus`, a tray / menu bar icon) runs that daemon in-process, shows its status,
  installs the Claude Code hooks, and flashes or updates the device firmware it ships with. A headless service
  (systemd / launchd, `tools/install.sh`) runs the same daemon without the tray, for a source checkout.
- The **firmware** (`firmware/`) is an LVGL 8 UI on the ESP32-S3. It only renders what the daemon sends; nothing
  leaves your machine except over the USB cable.
- The wire format is documented in [PROTOCOL.md](../PROTOCOL.md); the desktop app's architecture in
  [DESKTOP.md](DESKTOP.md).

### Providers, hooks, focus, device sharing

- **Providers** (`host/companion/providers/`): `sessions` reads `~/.claude/sessions/<pid>.json` and the
  transcripts; `usage` computes tokens, cost and plan limits; `system` collects host metrics; `hooks` receives
  real-time signals; `demo` fakes a few sessions for `tools/dev.py`. Whichever source saw the most recent status
  change wins, and events are de-duplicated.
- **Hooks** (`host/companion/hookclient.py`; run as `"<app>" hook` by the desktop app, as `host/hooks/cm_hook.py`
  by a script install): Claude Code runs it on 14 events (SessionStart/End, UserPromptSubmit, Pre/PostToolUse,
  PostToolUseFailure, PermissionRequest, PermissionDenied, Notification, Stop, StopFailure, SubagentStart/Stop,
  PreCompact). It sends one ≤ 2 KB datagram to `~/.claudius-maximus/hooks.sock` (on Windows a UDP datagram to the
  `127.0.0.1` port the daemon writes to `hooks.port`, with a per-start key), never prints, always exits 0 and does
  nothing if the daemon is down. Most hooks run `async`; the end-of-turn ones (Stop, StopFailure, SessionEnd,
  SubagentStop) run synchronously, about 30 ms each.
- **statusLine wrapper** (`"<app>" statusline`, or `host/hooks/cm_statusline.py`): saves the plan-limit data
  Claude Code gives the status line (`rate_limits.five_hour/seven_day`, Pro/Max only) to
  `~/.claudius-maximus/statusline.json`, then runs your previous statusLine command.
- **Focus action**: tapping Focus on the device raises that session's terminal. On macOS, Terminal.app and iTerm2
  get the exact tab (matched by tty); VS Code, Cursor and similar get the window whose title has the project name
  (needs Accessibility permission); tmux panes are selected; other terminals are just activated. On Linux, tmux
  panes are selected too, then the window of the terminal process (or, for VS Code and similar, the window titled
  with the project name) is raised through KWin's D-Bus scripting on KDE (X11 and Wayland, nothing to install) or
  `kdotool`, `xdotool` or `wmctrl` where one of them is installed. On Windows, the editor window titled with the
  project name, else the console / mintty window of the session's shell, else the topmost Windows Terminal window.
- **Device sharing**: one daemon per user (`~/.claudius-maximus/daemon.lock`). The serial port is opened only while
  holding a lock on `.devlock`: `<repo>/.devlock` in a checkout, the same lock `tools/dev.py` uses (the daemon
  steps aside while a dev command wants the device), and `~/.claudius-maximus/.devlock` in the packaged app. The
  flasher pauses the daemon's link while it writes. After an unplug, replug or reflash the daemon re-sends the
  full state.
- **Observer mode**: only one daemon per user can own the device. If another one is already running (the headless
  service of a script install, or `companion run` in a terminal), the app shows that daemon's status, marks it
  "Another daemon owns the device (status only)", cannot flash, and takes over by itself once the other daemon
  stops. Turning **Start at login** on retires the headless service.

## Command line

The app binary is also the CLI. With no arguments it starts the tray app; with a command it runs that command:

| OS | Binary |
|---|---|
| Windows | `%LOCALAPPDATA%\Programs\ClaudiusMaximus\ClaudiusMaximus.exe` (a windowed program: in a terminal the prompt can come back before the output; `… status \| more` waits) |
| macOS | `"/Applications/Claudius Maximus.app/Contents/MacOS/claudius-maximus"` |
| Linux | the `.AppImage` file, or `claudius-maximus/claudius-maximus` from the tar.gz |

From a source checkout, the same commands are `python -m companion <command>`, run from `host/` with the venv's
Python (`cd host && .venv/bin/python -m companion status`); there, no command means `run`, not `app`.

| Command | |
|---|---|
| `app [--no-setup] [--demo] [--port P] [--seconds N] [-v]` | the tray app (default for the binary); `--no-setup` skips the first-run setup, `--seconds` quits after N s (testing) |
| `run [--demo] [--port P] [--dry-run] [--seconds N] [--log [FILE]] [-v]` | the headless daemon, no tray (what the systemd / launchd service runs); `--dry-run` prints the JSON stream instead of using the device |
| `status [--json]` | daemon, start at login, device, hooks, sessions, plan limits, log path (a headless service from `tools/install.sh` is named only when installed) |
| `focus SID` | raise the terminal running that session (8-char id from `status`) |
| `detect` | serial ports (native / uart / other), link port, bundled firmware |
| `flash [--port P] [--erase]` | flash the bundled firmware; asks a running daemon to release the port first, prints progress on stderr, exits 0 or 1 |
| `hooks install\|uninstall\|check [--no-statusline]` | the Claude Code hooks and statusLine wrapper in `settings.json` |
| `autostart enable\|disable\|status` | start the tray app at login |
| `hook`, `statusline` | what Claude Code runs (reads the hook JSON on stdin; not for manual use) |

State lives in `~/.claudius-maximus/` (`%USERPROFILE%\.claudius-maximus` on Windows): socket or port file,
status.json, statusline.json, locks, the app's settings.json.

The tray menu, top to bottom:

| Item | What it does |
|---|---|
| `Device: connected on COM5 · fw 0.3.0 (4B)` | Device status (also: `not found`, `found on …, not answering`, `busy on …` while a dev tool holds it, `Board on USB-TO-UART port: move the cable to USB-OTG`, flashing progress). |
| `Sessions: 2 (1 working, 1 needs you)` | Claude Code sessions alive. |
| `Plan: 5h 42% · 7d 12%` | Plan limits, once Claude Code reported them (claude.ai Pro/Max). |
| `Update firmware (0.2.0 → 0.3.0)…` | Shown when the board runs an older firmware than the app ships. |
| `Install firmware…` | Shown when a board port is seen but no firmware answers for 10 s (blank or foreign firmware). |
| `Flash firmware…` | Otherwise: reflash the bundled firmware on demand. |
| `Start at login` | Toggles the autostart entry. |
| `Claude Code hooks: installed` / `Install Claude Code hooks` | Hook state; the second form installs them. |
| `Open log` | Opens the log file. |
| `Open claude.ai usage` | Opens `https://claude.ai/settings/usage`. |
| `Quit` | Stops the app and its daemon. |

## Flashing by hand

Plug the board into its **USB-OTG** port (the ESP32-S3's native USB, `303a:1001`). The 4B's **USB TO UART** port
(CH343, `1a86:55d3`) can flash too, but the link does not work there: move the cable back afterwards. If the native
port does not reset into the bootloader (some hubs and drivers), the app asks a running firmware (0.3.0 or later)
to reboot into download mode itself. If all else fails, hold **BOOT** while pressing **RESET** (the buttons on the
board's edge) to force the ROM loader, then flash again; `--erase` wipes the flash first.

The raw images are on the Releases page (`firmware-<board>-<fwver>.zip`, with a `manifest.json` listing the
esptool offsets).

## Headless install from a source checkout

For a source checkout (macOS or Linux), `tools/install.sh` sets up the daemon as a background service instead of
the tray app. Requirements: Python 3.11+, Claude Code.

```bash
tools/install.sh
cd host && .venv/bin/python -m companion status
```

`install.sh` is idempotent, so you can re-run it after an update or after moving the repo. It:

1. creates `host/.venv` with `pyserial` (and `psutil`, used by the System screen);
2. merges the hooks and the statusLine wrapper into `~/.claude/settings.json` (timestamped backup), with the
   checkout's `host/hooks/cm_hook.py` as the hook command;
3. installs the background service: on macOS the LaunchAgent `~/Library/LaunchAgents/com.claudius-maximus.daemon.plist`
   (RunAtLoad, KeepAlive), on Linux the systemd user unit `~/.config/systemd/user/claudius-maximus.service`
   (enabled, restarts on failure).

Options: `--no-service`, `--no-hooks`, `--no-statusline`. The service and the app cannot both own the device:
with both installed the app runs in observer mode; enabling the app's **Start at login** stops and disables the
service (on macOS the plist is renamed `*.plist.disabled-by-app`); `tools/uninstall.sh [--keep-logs]` removes the
service, our `settings.json` entries (restoring your previous statusLine), the app's start-at-login entry and
`~/.claudius-maximus`.

```bash
# macOS
launchctl kickstart -k gui/$(id -u)/com.claudius-maximus.daemon   # restart
launchctl bootout gui/$(id -u)/com.claudius-maximus.daemon        # stop
# Linux
systemctl --user restart claudius-maximus
systemctl --user stop claudius-maximus
```

## Building the firmware and running from source

Install the full host package and PlatformIO into the venv:

```bash
python3 -m venv host/.venv && host/.venv/bin/pip install -e host platformio   # or: -r host/requirements.txt platformio
host/.venv/bin/python tools/dev.py flash                       # build + flash with PlatformIO
cd host && .venv/bin/python -m companion app --no-setup -v    # the tray app from source (stop the service first)
```

`host/requirements.txt` lists the same dependencies as `host/pyproject.toml` (pyserial, psutil, pillow, esptool,
pystray / dbus-fast for the tray), so either install works. Building the installers is described in
[packaging/README.md](../packaging/README.md).

`tools/dev.py` drives the device during development (set `DEV_NAME` for a private build directory):

```bash
host/.venv/bin/python tools/dev.py build
host/.venv/bin/python tools/dev.py flash
host/.venv/bin/python tools/dev.py shot out.png --screen agents [--detail] [--alert waiting]   # framebuffer PNG with demo data
host/.venv/bin/python tools/dev.py shot out.png --screen home --calm --settle 4                # ... docs shot: no alerts, toasts faded
host/.venv/bin/python tools/dev.py perf --screen orbit                                          # fps, frame time, CPU, heap
host/.venv/bin/python tools/dev.py show mock.png --seconds 30                                   # a PNG mock-up on the panel
python3 tools/room_sim.py states                                                               # home room: one frame per state (host, no device)
python3 tools/room_sim.py task code --seconds 40 --gif                                         # ... animated GIF + activity log
python3 tools/room_sim.py day --seconds 1800                                                   # ... scripted session: fidget variety stats
```

The README screenshots are `shot --calm --settle 4` (`--settle 9` on Agents, whose demo queues several toasts) of
`home`, `agents`, `feed`, `usage`, `system`, `orbit`, and `--screen agents --detail` for the detail view.

## Troubleshooting

| Symptom | Check |
|---|---|
| Device stuck on "waiting for host" | Look at the tray menu, or run `status`: is the daemon running and the port found? `busy` / `device lock held by another tool` means a `tools/dev.py` command or `flash` is using the device. |
| Menu says "Board on USB-TO-UART port" | The 4B is plugged into its USB TO UART port: move the cable to USB-OTG. |
| "Another daemon owns the device (status only)" | Observer mode: the headless service (or a `run` in a terminal) owns the device. Turn on **Start at login** in the menu (retires the service) or run `tools/uninstall.sh`, or stop that daemon. |
| `another companion daemon is already running` | Same cause, from `run` in a terminal: stop the app or the service first. |
| No tray icon on Linux | GNOME needs the AppIndicator extension; the app keeps running without it (`status` works). |
| Linux: `cannot open device: ... Permission denied` | Add yourself to `dialout` or install `tools/udev/99-claudius-maximus.rules` (see the README). |
| Linux: device disconnects right after plugging in | ModemManager is probing the port; the udev rule stops it. |
| Linux: AppImage does not start | Install `libfuse2` / `fuse2`, or run it with `--appimage-extract-and-run`, or use the tar.gz. |
| Windows: which COM port? | `detect` lists them; the native port shows as "USB Serial Device (COMn)" and needs no driver. The 4B's USB TO UART port is a CH343: Windows 10 and 11 ship its driver, older versions need WCH's CH343 driver. |
| Windows: no real-time updates | Hooks go over UDP to `127.0.0.1`, at the port and key the running daemon writes to `%USERPROFILE%\.claudius-maximus\hooks.port` (deleted when it stops). `status` shows `listening` and `received`; `hooks check` shows the hook command, which must point at the installed `ClaudiusMaximus.exe`. |
| No real-time updates | `status` should show hooks `installed=yes listening=yes received>0`. Restart Claude Code sessions after installing; `/hooks` in Claude Code lists them. If you moved the AppImage or the macOS app, run `hooks install` (or Install Claude Code hooks) again: the hook command contains the app's path. |
| Plan limits unknown | They need a claude.ai Pro/Max login and the statusLine wrapper, and appear after the first reply in a session. |
| Flashing fails | Use a data cable, try the other port (on the 4B, USB TO UART also flashes), or force the ROM loader with BOOT + RESET, then flash again. `--erase` wipes the flash first. Details in the log. |
| Focus does nothing in VS Code | macOS: give Accessibility permission to the app (or, for a script install, the daemon's Python / osascript) in System Settings → Privacy & Security. Linux outside KDE: install `xdotool` (X11) or `kdotool`. |
| Image jumps sideways for a frame, or shifted lines | See [SCANOUT.md](SCANOUT.md) for the mechanism, the measurement protocol and the probe switches. Quick check: `tools/dev.py perf --screen agents --cycle`: `lcd_n10` / `vs_n10` count scan-out stalls and late vsync interrupts per window (both should be 0). Anything that masks interrupts on the LVGL core for tens of µs, or starves PSRAM, shows up there. `-DHAL_BOUNCE_PX=19200` selects the bounce-buffer scan-out for experiments. |
| Logs | **Open log** in the menu, or: macOS `~/Library/Logs/claudius-maximus.log`; Linux `~/.local/state/claudius-maximus/claudius-maximus.log`; Windows `%LOCALAPPDATA%\claudius-maximus\logs\claudius-maximus.log`. Rotated at 1 MB × 3. `-v` for debug output. |

This project was designed and built by a team of Claude Code agents working in parallel;
[TEAM.md](TEAM.md) and [PERF.md](PERF.md) record how.
