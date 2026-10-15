# Team rules — read fully before writing code

> **Current round: performance optimization** — see docs/PERF.md (probe, targets, baseline) and the
> "Perf round ownership" table below, which overrides the original table for this round.


We are building **Claudius Maximus**: a 4" 480×480 touch screen (Waveshare ESP32-S3-Touch-LCD-4 V4) that
sits on a developer's desk and mirrors what their Claude Code agents are doing — a friendly, glanceable
companion. Audience: developers running several agents/subagents in parallel. It must be *useful at a
glance from 1 m away* (who's working, who needs me, what are they doing, how much budget is left) and
*delightful* (personality, smooth motion, tasteful celebrations) without being noisy.

Only the **LCD + touch** are used. Do not use buzzer, SD, RTC, IMU, RS485, CAN.

## Layout
```
PROTOCOL.md                  host⇄device JSON-lines protocol (source of truth for data)
firmware/                    PlatformIO, Arduino-ESP32 3.x, LVGL 8.4.0, ArduinoJson 7
  src/core/                  hal (display/touch/backlight/screenshot), model (state store), link (serial), settings
  src/ui/ui.{h,cpp}          tileview screen manager + page dots   (lead-owned)
  src/ui/theme.h             palette, fonts, status_color()          (lead-owned)
  src/screens/*.cpp          one file per screen (owners below)
  src/fx/alerts.cpp          attention overlay on lv_layer_top()
  src/fx/ambient.cpp         auto-dim / screensaver / night clock
host/companion/              Python 3.15 daemon (venv: host/.venv, has pyserial)
  daemon.py link.py          main loop & serial
  providers/*.py             data providers (sessions, usage, hooks, demo)
tools/dev.py                 build / flash / screenshot tool (shared device lock)
```

## Ownership (only edit files you own; create new files with your prefix)
| Owner | Files |
|---|---|
| host-sessions | host/companion/providers/sessions.py, host/companion/providers/demo.py, new host/companion/sessions_*.py |
| host-runtime | host/companion/providers/hooks.py, daemon.py, __main__.py, link.py, host/hooks/*, tools/install*.sh, tools/uninstall*.sh, host/launchd/*, README.md |
| usage | host/companion/providers/usage.py, firmware/src/screens/screen_usage.cpp, new usage_* files |
| companion | firmware/src/screens/screen_home.cpp, new firmware/src/screens/home_* files |
| agents | firmware/src/screens/screen_agents.cpp, new agents_* files |
| detail-feed | firmware/src/screens/screen_detail.cpp, screen_feed.cpp, new detail_*/feed_* files |
| alerts-settings | firmware/src/fx/alerts.cpp, firmware/src/core/settings.{h,cpp}, firmware/src/screens/screen_settings.cpp |
| system | host/companion/providers/system.py, firmware/src/screens/screen_system.cpp, new system_* files |
| orbit-ambient | firmware/src/screens/screen_orbit.cpp, firmware/src/fx/ambient.cpp, new orbit_*/ambient_* files |

### Perf round ownership
| Owner | Files |
|---|---|
| perf-core | firmware/platformio.ini, include/lv_conf.h, include/lv_psram_alloc.h, src/main.cpp, src/core/hal.*, src/ui/ui.cpp, src/ui/theme.h |
| perf-link | src/core/link.*, src/core/model.*, host/companion/daemon.py, link.py, providers/hooks.py, providers/sessions.py, sessions_*.py, providers/system.py, providers/usage.py |
| perf-home | src/screens/screen_home.cpp, home_* |
| perf-agents | src/screens/screen_agents.cpp, agents_* |
| perf-feed | src/screens/screen_feed.cpp, screen_detail.cpp, feed_*, detail_* |
| perf-usage | src/screens/screen_usage.cpp, usage_* (firmware only) |
| perf-system | src/screens/screen_system.cpp, system_* (firmware only) |
| perf-orbit | src/screens/screen_orbit.cpp, orbit_*, src/fx/ambient.*, ambient_* |
| perf-alerts | src/fx/alerts.*, src/core/settings.*, src/screens/screen_settings.cpp |
| perf-bench | tools/bench.py (new), docs/PERF.md results section, tools/dev.py (additive only) |
Everyone may append to docs/PERF.md "## Results". src/core/perf.* is lead-owned (ask perf-core via the lead if you need more probes).

Lead-owned (ask the lead, don't edit): ui.h, ui.cpp, theme.h, screens.h, hal.*, model.*, link.*, main.cpp, platformio.ini, lv_conf.h, PROTOCOL.md, tools/dev.py.
Exception: you may *append* a short request to the `## Requests` section at the end of PROTOCOL.md if you need data
that the protocol doesn't carry, then work with defaults until it arrives. Host owners: watch that section.

You may add **new** files freely within your prefix (e.g. `firmware/src/screens/agents_card.cpp`). PlatformIO compiles every
file in src/ automatically. Keep public symbols `static` or prefixed with your area to avoid link collisions.

## Building, flashing, seeing your work
Always set your name: `export DEV_NAME=<your-owner-id>` (gives you a private build dir).
```
host/.venv/bin/python tools/dev.py build
host/.venv/bin/python tools/dev.py flash                       # build + upload (waits for the device lock)
host/.venv/bin/python tools/dev.py shot /path/out.png --screen agents [--detail] [--alert waiting] [--seconds 4]
```
`shot` streams demo data (host/companion/providers/demo.py) to the device, navigates, and saves a PNG of the real
framebuffer — **Read the PNG to look at your work.** Iterate visually; check legibility and alignment.
- Put screenshots in your scratchpad, not in the repo.
- The device is shared by ~8 people: the tool queues for it. Hold it briefly. Others flash too, so the device may be
  running someone else's build of the *whole* tree — always `flash` right before `shot` if you changed firmware.
- The whole tree compiles together. **Never leave your files in a non-compiling state** (write complete files).
  If the build breaks in a file you don't own, wait a minute and retry; don't fix others' files. If it persists >5 min,
  report it in your final message.
- Don't commit to git; the lead does that.

## Firmware guidelines
- LVGL 8.4 API (lv_obj_*, lv_label_*, lv_anim_*, lv_timer_*). Everything runs on the Arduino loop thread — no extra tasks touching LVGL.
- Screen API: see src/ui/ui.h `ScreenDef`. `update(state)` is called on new state only while your tile is visible (and on show).
  `on_event(ev)` always. Pause your lv_timers / animations in `on_show(false)` to save CPU.
- Read JSON defensively: `s["name"] | "?"`, `s["tokens"]["out"] | 0`. Don't hold JsonObjectConst across calls (the doc is replaced).
- Don't rebuild the whole widget tree on every update; create once, then update text/values; reuse/pool list rows.
- Memory: ~140 KB internal heap free; 8 MB PSRAM. Big buffers (canvas, images) → `heap_caps_malloc(n, MALLOC_CAP_SPIRAM)`.
- Performance: framebuffer is in PSRAM, rendering is CPU. Keep continuously animated areas small (<~120×120) or low frame-rate.
  Full-screen redraw ≈ 30–60 ms. Avoid big shadows/opacity on large areas in animations.
- Fonts: Montserrat 8–48 (even sizes) built-in, ASCII + LV_SYMBOL_* only. Use theme.h F_* / C_* tokens.
  Custom fonts: allowed via `npx lv_font_conv` into your own prefixed file (keep total < 300 KB).
- Touch: 480×480, finger-sized targets (≥ 48 px). Horizontal swipes belong to the tileview — use vertical scroll inside tiles.
- Round-cornered square screen; keep content ≥ 12 px from edges; bottom 20 px holds page dots.
- Tapping a session anywhere should open its detail: `detail_open(sid)` (screens.h).
- Visual language: warm dark, Claude orange accent; status colors are fixed (theme.h): busy=orange, waiting=amber,
  idle=green, offline=grey, subagents=violet. "waiting" (needs the human) is the most important signal on the device.

## Host guidelines
- Python 3.15 stdlib + pyserial only (no other deps unless essential). Run: `host/.venv/bin/python -m companion [--demo] [--dry-run]`.
- Never modify the user's real `~/.claude/settings.json`, launchd agents, or other global config while developing —
  write installers and test them against a temp HOME. The lead will run installation at the end.
- Treat ~/.claude data as private: read-only, never send it anywhere except the device over USB.
- `--dry-run` prints the JSON stream; use it to test without the device. The device is held by `tools/dev.py`
  users during development, so don't run the daemon against the device for more than ~30 s at a time, and use the
  same lock: wrap with `tools/dev.py`'s `device_lock` or just test with --dry-run.

## Definition of done
Builds clean, flashed, screenshot-verified on the device with demo data (firmware) or verified against real
~/.claude data + `--dry-run` (host). Final message: what you built, files touched, how to use it, known issues,
and ideas you didn't get to.

## Requests
<!-- append: `- [from <owner>] need <field> in <message> because <reason>` -->
