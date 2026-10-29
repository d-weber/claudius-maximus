# Working on Claudius Maximus

A desk companion for Claude Code: an ESP32-S3 touch screen (Waveshare ESP32-S3-Touch-LCD-4B, 480x480) shows every
Claude Code session on the machine, fed by a Python daemon over USB. This file is the map for an agent; the detail
lives in the docs it points to. Read the relevant doc before changing a subsystem.

## Layout and docs

| Path | What | Read first |
|---|---|---|
| `firmware/` | ESP-IDF 5.5 via PlatformIO (pioarduino), LVGL 8.4, ArduinoJson 7. `src/core` HAL/link/settings/perf, `src/ui` theme, `src/screens` one file per screen, `src/screens/room` Clawd's Studio (portable C++, also builds on the host), `src/fx` alerts/screensaver | `docs/DEVELOPMENT.md`, `docs/TEAM.md` "Firmware guidelines", `firmware/src/screens/room/README.md` |
| `host/companion/` | the daemon, tray app, flasher, updater, hooks client; `host/hooks/` the hook shims; `host/tests/` unittest suite | `docs/DESKTOP.md` (architecture + "Rules for changes") |
| `PROTOCOL.md` | host ⇄ device JSON lines, the source of truth for data | before touching either side of the link |
| `tools/` | `dev.py` (build/flash/shot/perf/send), `fw_bundle.py`, `sprites.py` + `sprites/` (sheet converter), `sprites_prebuild.py` (PlatformIO hook), `room_sim.py`, install/uninstall | `docs/DEVELOPMENT.md` "Building the firmware" |
| `firmware/assets/sprites/` | the room's sprite sheets (indexed PNGs + `atlas.json`): the masters of its pixel art, edited by hand | `tools/sprites/README.md` |
| `packaging/`, `.github/` | PyInstaller spec, installers, CI (`ci.yml`) and release (`release.yml`) | `packaging/README.md` |
| `docs/PERF.md` | every measurement so far, the probe method, and what was tried and rejected | before any performance work |
| `docs/SCANOUT.md`, `docs/STATUS.md` | the LCD scan-out investigation; status snapshots | when frames jump or underruns appear |

## Commands that work

```bash
# host (Python 3.11+; the repo venv is host/.venv)
host/.venv/bin/pip install -e host                      # deps; add platformio for firmware builds via dev.py
HOME=$(mktemp -d) host/.venv/bin/python -m unittest discover -s host/tests     # 200+ tests, ~7 s
cd host && .venv/bin/python -m companion run --dry-run  # the daemon's JSON stream without a device
cd host && .venv/bin/python -m companion status         # or: "/Applications/Claudius Maximus.app/Contents/MacOS/claudius-maximus" status

# firmware (needs a PlatformIO whose Python is 3.10-3.14: the pioarduino platform refuses 3.15)
DEV_NAME=<you> host/.venv/bin/python tools/dev.py build          # own build dir firmware/.pio/build-<you>
DEV_NAME=<you> host/.venv/bin/python tools/dev.py flash          # build + flash (takes <repo>/.devlock)
DEV_NAME=<you> host/.venv/bin/python tools/dev.py shot out.png --screen home --calm --settle 4
DEV_NAME=<you> host/.venv/bin/python tools/dev.py perf --screen home --seconds 30 [--nodemo] [--cycle]
python3 tools/room_sim.py states                                 # the room on the host, no device
host/.venv/bin/python tools/sprites.py check                    # validate the sheets after editing one
host/.venv/bin/python tools/room_view.py                        # live browser preview of the room while editing sheets
host/.venv/bin/python tools/sprites.py preview --out /tmp/spr     # 4x views of the sheets, outside the repo
```

- Run the test suite with `HOME` pointing at an empty directory: three flasher tests read the real
  `~/.claudius-maximus/settings.json` and fail on a machine where a device port is chosen.
- The packaged app and a source checkout use different device locks (`~/.claudius-maximus/.devlock` vs
  `<repo>/.devlock`). Before `dev.py flash/perf`, quit the app (`osascript -e 'tell application "Claudius Maximus"
  to quit'`), check `lsof /dev/cu.usbmodem*` is empty, and afterwards restore the release firmware with
  `"<app>" flash` and `open -a "Claudius Maximus"`. Never kill a process you did not start.
- The board's host link is the **USB-OTG** port (native USB-JTAG, VID 303a). The "USB TO UART" port (CH343) can
  flash but the link does not work there. Scope is LCD + touch only: no buzzer, speaker, mic, SD, RTC or IMU.
- `firmware/src/CMakeLists.txt` globs every `.cpp` under `src/`: a generated or scratch file left there is compiled
  into the firmware. `src/screens/room/sprites_gen.h/.cpp` are generated from the sheets by the PlatformIO
  pre-build hook (`tools/sprites_prebuild.py`, needs Pillow) and by `room_sim.py`; they are gitignored.

## How things are done here

- **Comments say why, not what.** Short, in the style of the surrounding code; a docstring on every function that
  is not obvious; no narration of the obvious.
- **Commits**: `Area: short summary` on the first line (`Flasher: …`, `Firmware: …`, `Release 0.5.1`), then a body
  that explains the reasoning and the measurement. Branch per change, PR into `main` with a `## What` / measured /
  tests description; merge with a merge commit. Nothing is pushed to `main` directly except release commits.
- **Measure before and after** anything that touches rendering, the link or the daemon's hot path, with the probe
  in `docs/PERF.md` (same screen, same seconds, two runs, `lcd_underruns` must stay 0), and record the numbers
  there as a dated entry, including what did not work.
- **Host rules**: Python 3.11+ stdlib + the listed deps; `hookclient.py` stays stdlib-only and silent; never modify
  the user's real `~/.claude/settings.json`, LaunchAgents or `~/.claudius-maximus` while developing, test installers
  against a temp `HOME`; `~/.claude` data is read-only and never leaves the machine except over USB.
- **Firmware rules**: everything LVGL runs on the main loop task; big buffers go to PSRAM
  (`heap_caps_malloc(n, MALLOC_CAP_SPIRAM)`; plain `malloc` ≥ 4 KB also lands there); keep hot per-tick state out of
  internal `.bss` (it collided with the LCD scan-out DMA, see `docs/SCANOUT.md`); the `Director` is calloc'd in PSRAM.
- **Generated images stay in the repo** (`firmware/assets/`, `docs/img/`); no copies elsewhere, no `@4x` previews
  next to the assets (`sprites.py preview --out DIR` writes them somewhere else on request).
- **Release**: bump `host/companion/version.py` (app) and, if the firmware changed, `firmware/src/core/version.h`
  (`FW_VERSION`, what installed apps compare); commit `Release X.Y.Z`; `git tag vX.Y.Z && git push origin main
  vX.Y.Z`; `release.yml` builds the four installers and the firmware zip (~10 min). macOS CI runners are sometimes
  never acquired: `gh run rerun <id> --failed`.

## The room (Clawd's Studio) in one paragraph

A 120x106 canvas of palette indices (`room::Idx`, 61 entries in `room::BASE`, index 0 transparent), drawn by
`Director::draw` at 12 fps and upscaled 4x into LVGL by `home_face.cpp`. Lighting is a palette rebuild, not a
redraw, so art must use palette indices. All the art is sprites blitted from the sheets in
`firmware/assets/sprites/` (indexed PNGs, edited by hand: `tools/sprites/README.md`), which the pre-build hook
converts into compact RLE tables (`sprites_gen.h/.cpp`, in flash); only lighting effects are still code. The same
sources build on the host (`tools/room_sim.py`), which is how the room is previewed. `docs/PERF.md` measures the
sprite tables: the room draw is ~2 % of the CPU.

## Known gaps and open threads

- Small oddities carried over from the procedural art into the sheets (rain never drifts, a bird frame crosses
  the curtain, the `ARMS_HOLD` frames equal `ARMS_DOWN`); none fixed.
- An on-screen hint when the cable is in the USB-TO-UART port is possible (`usb_serial_jtag_is_connected()`),
  not built.
