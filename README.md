# Claudius Maximus

A desk companion for Claude Code: a 4" 480×480 touch screen (Waveshare ESP32-S3-Touch-LCD-4) plugged in over
USB that shows what your Claude Code agents are doing. You can see from across the room which session is
working, which one **needs you** (a permission prompt or a question), what each agent and subagent is doing,
and how much of your plan budget is left.

```
 Claude Code sessions ──hooks (unix datagram)──┐
 ~/.claude transcripts + session registry ─────┤
 statusLine JSON (plan limits) ────────────────┼─> companion daemon ──USB serial, JSON lines──> device
 host metrics ─────────────────────────────────┘        (launchd)   <── actions (focus a session)
```

## Install

Requirements: macOS, Python 3.12+ (Homebrew), Claude Code, the device flashed with `firmware/`
(`host/.venv/bin/python tools/dev.py flash`).

```bash
tools/install.sh            # venv + Claude Code hooks/statusLine + LaunchAgent (starts the daemon)
host/.venv/bin/python -m companion status    # run from host/ (or: cd host && .venv/bin/python -m companion status)
```

`install.sh` is idempotent, so you can re-run it after an update or after moving the repo. It:
1. creates `host/.venv` with `pyserial` (and `psutil`, which is optional and used by the System screen);
2. merges async hooks and a statusLine wrapper into `~/.claude/settings.json`. It first saves a timestamped backup
   (`settings.json.bak-claudius-*`). Your own hooks are left untouched, and an existing statusLine command
   keeps working because the wrapper runs it and prints its output;
3. installs `~/Library/LaunchAgents/com.claudius-maximus.daemon.plist`, which uses RunAtLoad and KeepAlive.

Options: `--no-launchd`, `--no-hooks`, `--no-statusline`. Hooks apply to Claude Code sessions started after
the install.

To remove everything: `tools/uninstall.sh [--keep-logs]`. It stops the agent, removes only our entries from
settings.json (restoring your previous statusLine) and deletes `~/.claudius-maximus`.

## Usage

```bash
cd host
.venv/bin/python -m companion                 # run in the foreground (stop the LaunchAgent first)
.venv/bin/python -m companion --dry-run       # print the JSON stream instead of using the device
.venv/bin/python -m companion --demo          # stream fake sessions to the device
.venv/bin/python -m companion status [--json] # daemon / device / hooks / sessions / plan limits
.venv/bin/python -m companion focus 969ca7a9  # raise the terminal running that session
```
Other flags: `--port /dev/cu.usbmodemXXX`, `--log [FILE]`, `--seconds N`, `-v`.

Daemon control: `launchctl kickstart -k gui/$(id -u)/com.claudius-maximus.daemon` (restart),
`launchctl bootout gui/$(id -u)/com.claudius-maximus.daemon` (stop).

## How it works

- **Daemon** (`host/companion/daemon.py`). Every 250 ms it polls the providers and builds a `state` snapshot
  (PROTOCOL.md). It sends the snapshot when it changes, plus a heartbeat every 2 s, and sends `event`s as they
  happen. A hook datagram wakes the loop at once, so a permission prompt shows on the device within about
  50 ms. CPU use is about 1 % when idle.
- **Providers** (`host/companion/providers/`):
  - `sessions`: `~/.claude/sessions/<pid>.json` and the transcripts.
  - `usage`: tokens, cost and plan limits.
  - `system`: host metrics.
  - `hooks`: real-time signals.

  The daemon merges hooks into sessions: whichever source saw the most recent status change wins. It also
  de-duplicates events.
- **Hooks** (`host/hooks/cm_hook.py`). Claude Code runs this tiny script on 14 events: SessionStart/End,
  UserPromptSubmit, Pre/PostToolUse, PostToolUseFailure, PermissionRequest, PermissionDenied, Notification,
  Stop, StopFailure, SubagentStart/Stop and PreCompact. The script sends one ≤2 KB datagram to
  `~/.claudius-maximus/hooks.sock`. It never prints, always exits 0, and does nothing if the daemon is down.
  Most of these hooks run `async`. The end-of-turn events (Stop, StopFailure, SessionEnd, SubagentStop) run
  synchronously, which costs about 30 ms each.
- **statusLine wrapper** (`host/hooks/cm_statusline.py`). It saves the plan-limit data that Claude Code gives
  the status line (`rate_limits.five_hour/seven_day`, Pro/Max only) to `~/.claudius-maximus/statusline.json`,
  then runs your previous statusLine command.
- **Device sharing**. The serial port is opened only while holding the flock on `<repo>/.devlock`, the same
  lock `tools/dev.py` uses. The daemon gives the lock up for about 1 s every 20 s (`CLAUDIUS_YIELD_S`; 0
  means never), so dev tools can take a turn. When it reconnects (after unplug, replug or reflash) it re-sends
  the full state, and it does the same when the device sends `hello`.
- **Device actions**. `{"t":"action","action":"focus","sid":…}` brings that session's terminal to the front:
  - Terminal.app and iTerm2: the exact tab, matched by tty.
  - VS Code, Cursor and similar: the window whose title has the project name. This needs Accessibility
    permission; without it the app is just activated.
  - tmux: the pane is selected.
  - Other apps (Ghostty, WezTerm, kitty and so on) are activated.

  `{"action":"refresh"}` re-sends the state.

Files: the state lives in `~/.claudius-maximus/` (socket, status.json, statusline.json, locks). Logs go to
`~/Library/Logs/claudius-maximus.log`, which is rotated at 1 MB × 3.

## Troubleshooting

| Symptom | Check |
|---|---|
| Device stuck on "waiting for host" | `companion status`. Check that the daemon is running and the device port is found. If the status says `device lock held by another tool`, a `tools/dev.py` command is using the device. |
| No real-time updates | `status` should show hooks `installed=yes listening=yes received>0`. Restart Claude Code sessions after installing, and run `/hooks` in Claude Code to see them. |
| Plan limits unknown | They need a claude.ai Pro/Max login and the statusLine wrapper. Limits appear after the first reply in a session. |
| "another companion daemon is already running" | The LaunchAgent is already running. Stop it before running in the foreground. |
| Focus does nothing in VS Code | Give Accessibility permission to the daemon's Python, or to osascript, in System Settings → Privacy & Security. |
| Logs | `tail -f ~/Library/Logs/claudius-maximus.log`, or run in the foreground with `-v`. |

Development: see `docs/TEAM.md`, `PROTOCOL.md`, and `tools/dev.py --help`.
