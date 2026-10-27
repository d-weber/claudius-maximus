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

## Touch + panel fixes — 2026-10-16 (first real use by hand on the panel)
Reported: the image jumped sideways for a frame now and then, and swipes mostly registered as taps on cards.
1. **Touch**: SensorLib's `getPoint()` ignores the GT911 "buffer ready" bit and clears the status register on every
   100 Hz poll, so a poll landing before the next scan read 0 touches: a 1-poll release mid-swipe = release + new
   press = click. `hal.cpp` now reads the status register itself and holds the state while no new scan is ready
   (the GT911 flags only scans with data: every scan under a finger, one empty scan on lift, nothing when idle).
   A press needs 2 fresh scans, a release is the first empty scan, 4 silent periods release as a backstop.
   perf: `touch_ic`, `touch_rate_ms`, `touch_errors`, `touch_scan_hz`, `touch_glitches`.
2. **Sideways jumps**: measured with a new scan-out probe (vsync / frame-complete callbacks of the RGB driver,
   perf: `lcd_n10`, `vs_n10`, `lcd_log`). Not PSRAM bandwidth (synthetic flash/PSRAM streams from core 0 and
   even a build that never writes the framebuffer all showed the same stalls) and not rendering (orbit with no
   host stream: zero). Cause: Arduino's HWCDC USB-serial ISR pushes each byte of a 64-byte packet through a
   FreeRTOS queue, 60-90 us with interrupts masked on core 1, which delays the vsync ISR where the driver
   restarts the DMA every frame (`CONFIG_LCD_RGB_RESTART_IN_VSYNC`); a late restart shifts that frame. Fix: the
   host link uses the IDF `usb_serial_jtag` driver (one ring-buffer push per packet), installed from core 0 so
   its ISR never runs on the LVGL core (`core/link.cpp`, `Link` object; `ARDUINO_USB_CDC_ON_BOOT=0`). Both probe
   histograms read 0 under the demo stream with page cycling. Bonus: +34 KB internal heap.
   Rejected on the way: bounce buffers (worse: with the core-0 flush competing for PSRAM the copy ISR falls
   behind and the frame desyncs vertically); GDMA memcpy into the framebuffer (9 ms per band vs 0.9 ms CPU).
3. Also: LVGL now uses its own TLSF pool in PSRAM (`LV_MEM_CUSTOM 0`, 2 MB) instead of the IDF heap, whose
   spinlock masks interrupts on every call (not the cause above, but removes that class of latency).
Residual: a frame whose DMA does not finish before the restart (`lcd_underruns`, logged as `999`) still shows up
0-5 times per 30 s in some runs and 0 in others, with the CPU side unaffected; cause unknown, watch by eye.

## Isometric home — 2026-10-16
The home screen is the isometric desk (iso-clawd/ ported to C++: `firmware/src/screens/iso/`), full width at 3x
(logical columns 40..199 of the 240 x 121 stage), the counters under it; the 2D face and the activity card are gone.
`home_face.cpp` is the director: mood -> loop animation (sleep / idle / waiting / per tool: reading, writing,
command, web, thinking, delegating, plan), reactions -> one-shots (hop, surprise, wink, giggle, happy, wince),
mini helpers for subagents, the speech bubble as before. Rendering: logical canvas in PSRAM, diffed per frame,
only the changed rows upscaled into a PSRAM image drawn by the face object (home demo: ~27 % CPU at 15 fps,
no scan-out stalls). Settings > Clawd > "Mirror the desk" flips the scene (`home_mirror`).
QA: `tools/iso_qa.py NN-name --frames a,b,c` forces the animation/frame on the device (`{"t":"cmd","anim":..}`),
renders the JS page headless and diffs the logical pixels (01-idle: 0 differing pixels on 5 frames).
iso_qa holds the ambient dimming / night screensaver off while it drives the panel (`dev_ambient` event) and
prints the diff bounding box per frame, so a face-only mismatch is easy to tell from a layout one.
Dev: `tools/dev.py show mock.png` displays a PNG on the panel (`{"t":"cmd","img":N}` + raw RGB565).
Pacing: each loop carries a calm segment (anim.h `calmFrom/calmTo`); the director plays a full pass, then rests in
the segment for a random 4-12 s (70 %) before the next pass, so gags come at irregular, longer intervals.
perf reports `home: {anim, frame, mode, noblink}`. 17-delegating has no segment (periodic choreography).
Blinks while resting are rolled per calm pass (~1 per 3.5 s) instead of once per 1 s loop (`iso::blinkSuppressed`).
The mug has one home, `SPOT.mug` (150, 90) front-right, in all 17 animations (JS and C++); idle's sniff gag leans
right, delegating's rookie veers off the lane into it, reviewing's magnifier and web-search's globe moved aside.
Mouths: redrawn as sheared sprites that follow the face plane (like the eyes and blush), slightly bigger, with a
narrow tall "o" so the shear does not smear it; changed in lib/iso.js first, then iso.cpp (QA: 0 diffs on 22 frames).
Adversarial review (fresh agent): no blockers; fixed `link_send()` still writing to the UART (Focus action never
reached the host since the USB driver switch), torn JSON lines on a slow host (now terminated so the host resyncs on
one bad line), QA/show tools always releasing the device, QA mirror as a non-persisted override, dead code from the 2D
face. Not done: a 9 px overlap of the counters row over the (empty) bottom rows of the scene.

## Daily use
The LaunchAgent (macOS) / systemd user service (Linux) starts at login and feeds the device. Check: `cd host && .venv/bin/python -m companion status`.
Dev: `tools/dev.py flash|shot|perf` (see docs/TEAM.md, docs/PERF.md). Uninstall: `tools/uninstall.sh`.
