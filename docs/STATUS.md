# Status — 2026-10-15 (end of day)

## Device
Flashed with the merged build of commit below (all perf-round screen + link optimizations; core render
pipeline unchanged). Verified: builds clean, all 7 screens render, perf measured on device.

## Perf round results (demo stream, 8 s, same probe as docs/PERF.md baseline)
| screen | fps before→after | frame avg ms | px/frame | cpu % | loop max ms |
|---|---|---|---|---|---|
| home | 12.0 → 17.4 | 57 → 18 | 98k → 24k | 88 → 51 | 196 → 128 |
| agents | 8.6 → 39.9 | 93 → 14 | 131k → 10k | 98 → 75 | 258 → 181 |
| feed | 6.7 → 38.3 | 108 → 15 | 155k → 15k | 95 → 74 | 279 → 173 |
| usage | 8.1 → 10.9 | 90 → 25 | 131k → 25k | 93 → 46 | 265 → 111 |
| system | 9.1 → 9.8 | 77 → 30 | 77k → 29k | 91 → 48 | 301 → 228 |
| orbit | 7.0 → 14.5 | 92 → 40 | 153k → 25k | 96 → 74 | 422 → 159 |
| settings | 14.1 → 11.7 | 44 → 19 | 53k → 20k | 81 → 41 | 160 → 96 |
Internal heap ~79 KB free. Host: state traffic 10.2 → ~0.9 KB/s (partial states), daemon CPU < 1%.
Demo stream is ~3x busier than real use and always has a waiting session; real use is much lighter.

## Render pipeline (perf-core, 2026-10-16) — demo stream, 8 s, avg of 2 runs, same tree before/after
| screen | fps | frame avg ms | frame max ms | cpu % | loop max ms |
|---|---|---|---|---|---|
| home | 18.5 → 22.4 | 16.7 → 10.6 | 93–106 → 57–71 | 49.9 → 32.8 | 120–129 → 77–90 |
| agents | 37.9 → 58.2 | 15.1 → 8.4 | 116–125 → 73–87 | 75.5 → 58.1 | 151–164 → 92–113 |
| feed | 38.3 → 55.7 | 14.7 → 8.2 | 141 → 91–98 | 73.6 → 54.5 | 173–176 → 111–122 |
| usage | 10.4 → 13.9 | 25.7 → 16.4 | 83–89 → 59 | 46.1 → 31.7 | 112–119 → 70–77 |
| system | 10.4 → 13.3 | 26.6 → 18.6 | 139–145 → 101–111 | 46.2 → 32.9 | 171–190 → 128–137 |
| orbit | 14.4 → 18.6 | 41.4 → 25.9 | 130–159 → 90–92 | 75.6 → 56.6 | 161–168 → 93–98 |
| settings | 11.5 → 14.7 | 19.3 → 12.6 | 89–91 → 55–56 | 40.9 → 26.4 | 100–101 → 64–65 |
| settings, static (--nodemo) | 0 → 0 | – | – | 9.4 → 1.1 | 7.3 → 4.6–5.2 |
Internal heap free 79 KB → 147 KB. Render cost ≈ 0.75 → 0.5 µs/px (home), 1.6 → 1.1 (orbit).
Kept: own esp_lcd RGB panel without bounce buffer, async flush on core 0, touch polling task on core 0, adaptive loop
sleep, -O2. Details, rejected experiments and the stability soak in docs/PERF.md (perf-core entry).
To verify by eye/finger (not possible remotely): panel image has no shifted/flickering lines; touch works.

## Not finished / next steps
1. perf-core render pipeline: done (see table above); the superseded dual-core WIP patch was removed.
2. **perf-bench** was stopped before its final run; tools/bench.py exists (untested by the lead).
3. Remaining spikes (loop max 110–230 ms) come from alert card/toast slide-ins and full card redraws on status change.
4. Not yet verified on hardware by touch: tapping cards/planets, scrolling, swipe-to-close detail, settings sliders.
5. Suggestion pending: host system.py could round cores to 2% / mem to 0.1 GB to skip more redraws.
6. `iso-clawd/`: work-in-progress isometric Clawd animation prototypes (browser only, not firmware).

## Linux support — 2026-10-16
Host side runs on Linux as well as macOS: `companion/hostos.py` picks log/service paths per OS, `tools/install.sh`
installs a systemd user unit (`host/systemd/`) instead of a LaunchAgent, `providers/system.py` uses psutil + PSI
+ sysfs where macOS used sysctl/Mach, `runtime_focus.py` has a Linux backend (KWin D-Bus / kdotool / xdotool /
wmctrl). Optional udev rule in `tools/udev/` (ModemManager ignore + port access). `tools/dev.py` finds `pio` in
the venv. Verified on Debian 13 / KDE Plasma 6 Wayland: flash, daemon as a service, hooks, focus, screenshots.
Also fixed: a transcript found by a rescan was not read until the next 2 s rest cadence (headless sessions
appeared late; `test_dead_pid_and_headless` was failing).

## Daily use
The LaunchAgent (macOS) / systemd user service (Linux) starts at login and feeds the device. Check: `cd host && .venv/bin/python -m companion status`.
Dev: `tools/dev.py flash|shot|perf` (see docs/TEAM.md, docs/PERF.md). Uninstall: `tools/uninstall.sh`.
