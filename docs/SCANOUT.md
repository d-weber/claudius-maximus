# Scan-out underruns (the "flicker" on the home screen)

Status as of 2026-10-28, late evening: fixed by switching the scan-out to refresh-on-demand mode (0 lost frames under every load that lost frames before); re-measured at 0 on the native ESP-IDF build on 2026-10-29 ("Native build" below). The record below is in the order the work happened; the conclusion is in "Where this stands". This page is
the record of what the glitch is, how to measure it, what was tried, and what is known. Short version in
[PERF.md](PERF.md) under "scan-out-stalls"; the measurement notes of the first round are in [STATUS.md](STATUS.md).

## What you see

A one-frame horizontal jump of the whole image, a few times a minute, on the home screen (Clawd's Studio), most
noticeable when the room is quiet (the companion app not running: grey Clawd asleep). The image snaps back on the
next frame. It is not a rendering glitch: screenshots of the framebuffer taken while it happens are fine.

## What it is

The ESP32-S3 scans the 480×480 RGB panel straight out of a framebuffer in PSRAM: the GDMA reads PSRAM through the
cache and feeds the LCD_CAM peripheral at the pixel clock (12 MHz here, 42.3 frames/s). If the DMA falls behind
the pixel clock for long enough, the panel clocks out stale data and the frame shifts. The driver option
`CONFIG_LCD_RGB_RESTART_IN_VSYNC` (on in the Arduino core) restarts the DMA at the next vsync, so the shift lasts
one frame instead of staying permanent. That restart is the jump.

The probe in `firmware/src/core/hal.cpp` registers the panel's `on_frame_buf_complete` (DMA end of frame) and
`on_vsync` callbacks and times them. Normally the frame completes a steady 465 µs before vsync. A vsync that
arrives with no frame-complete since the previous one is logged as a `999` and counted in `lcd_underruns`.
Frames that completed late but still before vsync are counted by how late (`lcd_n10`, `lcd_n30`, `lcd_n100`).

## How to measure

Hold the USB port open and send nothing (the demo stream of `tools/dev.py perf` is about three times real load
and masks the quiet-room case), then read the counters after a fixed window:

```python
# scratch script: hold the port, reset the window, wait, read
link = dev.open_link()
link.send({"t": "cmd", "nav": "home"})
time.sleep(8)
dev.read_perf(link)          # resets the per-window counters
time.sleep(120)
p = dev.read_perf(link)      # p["lcd_underruns"], p["lcd_n10"], p["vs_n10"], p["lcd_log"]
```

A 120 s window is the unit of comparison. The two `999` entries right after boot are panel initialisation and are
ignored. The companion app and the headless service must not be running (they hold the port and stream state).

`lcd_log` entries read `stall/late/p<phase>/+ms`: stall in µs or `999`, vsync lateness in µs, the main loop's
phase at that moment (0 idle, 1 link, 2 tick, 3 lvgl, 4 flush, 5 parse, +10 when another task was running on
that core), and the time since the previous entry.

## Timeline

| Date | Build | Underruns per 120 s, quiet room | Notes |
|---|---|---|---|
| 2026-10-28 early | Clawd's Studio first firmware | 10-40 per minute visible | Director state in internal `.bss` |
| 2026-10-28 | e340ec9, Director allocated in PSRAM | 0 in every window | Taken as the fix |
| 2026-10-28 | 49725f3 (main), screensaver drift removed | 3, 3, 6, 7 | Only firmware change since e340ec9 is 14 lines in `fx/ambient.cpp` |

## What was established on 2026-10-28 (second round)

1. **e340ec9 measures 0 and main measures 3 to 7**, each repeated, same device, same protocol, quiet room, host
   silent. The only firmware difference is the screensaver edit, which cannot touch the LCD path.
2. **RAM layout is not the variable.** main's build has 8 bytes less `.bss` than e340ec9, so the internal heap
   starts 8 bytes lower. Padding main's `.bss` back to the identical `.bss` size and `_heap_start` still gave
   7 underruns. The remaining difference between the two binaries is the code and rodata layout in flash.
3. **Both LCD callbacks run on core 1** (the LVGL core), and vsync lateness stays under 10 µs even on the `999`
   frames. So interrupt latency does not explain a missing frame-complete: the DMA genuinely did not finish the
   frame in time, by at least 465 µs.
4. **Partial stalls never occur.** Across every window, `lcd_n10` and `lcd_n30` stayed 0: the DMA is either on
   time or more than 465 µs late. Bandwidth contention would give a spread of small stalls. An all-or-nothing
   pattern points at something that blocks the external memory path outright for a while, such as the cache being
   unavailable, rather than at steady competition for PSRAM.
5. **The glitch happens while the main loop is idle.** Almost every `999` is logged in phase 0 or 10, that is
   during `delay()` at the bottom of the loop with the idle task (or the touch poll on core 0) running. The room
   renders at about 2.7 frames/s in the quiet state and the LVGL and flush phases are not where it happens.
6. **Layout sensitivity is real and fine-grained.** Adding the probe code itself (a few hundred bytes) moved main
   from 3-7 back to 0. A sweep over code padding is recorded below.
7. **Disabling the GT911 touch poll gave 0**, but that build also changed the layout, so on its own it proves
   nothing. The runtime toggle (`dev_touch` event, same binary) settles it; see the sweep.

## Layout sweep

Same source, `-DLAYOUT_PAD=<n>` NOPs inserted in flash, 60 s windows, quiet room, host silent. "Poll" is the GT911
touch poll over I2C on core 0, switched at runtime inside the same binary (`dev_touch` event).

| Padding | Poll on | Poll off | Poll on again | Where the `999`s fell |
|---|---|---|---|---|
| 0 | 0 | | | |
| 32 | 7 | 0 | 8 | idle, 3-30 s apart |
| 64 | 1 | 0 | 0 | |
| 128 | 1 | 5 | 2 | LVGL phase, clusters 2.5 s apart |
| 256 | 0 | | | |
| 512 | 3 | 0 | 2 | idle |
| 1024 | 1 | 1 | 0 | |
| 2048 | 1 | 7 | | LVGL phase, clusters 2.5 s apart |
| 4096 | 0 | | | |

Two separate triggers, both layout dependent at 32-byte granularity: the touch poll (pads 32 and 512: off takes
it to 0 and on brings it back) and the room's own animation step (pads 128 and 2048: clusters at the ~2.5 s cadence
with the poll off).

## Toggle matrix (one binary, pad 128 of a later build, 12 per 60 s baseline)

Runtime switches (`dev_probe` event) in the same binary:

| Switch | Underruns / 60 s |
|---|---|
| baseline, poll on | 9 (8 when repeated at the end) |
| poll off | 1 |
| poll off + core 0 reads 64 KB of flash rodata continuously | 0 |
| poll off + core 0 copies 64 KB PSRAM to PSRAM continuously | 45 |
| poll on, blit without the cache writeback | 9 |
| poll on, no framebuffer writes at all | 12 |
| poll on, main loop spins instead of `delay()` | 24 |

What this rules out: flash traffic (the SPI0-sharing explanation from the Espressif README does not apply here),
the framebuffer blit and its writeback (removing them changes nothing). What it rules in: CPU traffic to PSRAM.
A PSRAM memcpy multiplies the rate by five, and the Director state, the one hot thing the room keeps in PSRAM,
is still written every animation tick when the blit is off.

What the driver does (ESP-IDF `esp_lcd_panel_rgb.c`, with `CONFIG_LCD_RGB_RESTART_IN_VSYNC`): at every vsync it
resets and restarts the GDMA from the start of the framebuffer, unconditionally, with the comment "reset the GDMA
channel every VBlank to stop permanent desyncs from happening" and "this fix can lead to single-frame desyncs
itself". So a `999` is a frame whose DMA, started at vsync, did not reach the end of the buffer within the frame
period, and the next vsync throws it away. Since the probe never sees a frame that is late by 10 to 465 µs, the
DMA does not merely fall behind: a FIFO dry-out stops it for the rest of the frame.

## Synthetic load matrices (layout independent)

A probe task on core 0 generates one kind of traffic at a time while the room idles; `dev_probe` switches it at
runtime. Build of 2026-10-28 evening, pad 0, which happened to stall on its own (8, 13 and 5 per 60 s at baseline
with the poll on).

| Load | Underruns / 60 s |
|---|---|
| none, poll on (three windows) | 8, 13, 5 |
| none, poll off | 1 |
| flash rodata read stream (64 KB, every 32 B) | 0 |
| PSRAM read stream (64 KB, every 32 B) | 0 |
| PSRAM write stream (64 KB, one byte per 32 B line) | 140 |
| PSRAM write stream + `esp_cache_msync` of the 64 KB after each pass | 625 |
| PSRAM write stream + `Cache_WriteBack_All()` every 1 ms | 6 |
| PSRAM memcpy 64 KB to 64 KB | 45, 46 |
| `Cache_WriteBack_All()` every 1 ms alone, poll on | 5 |

Reading PSRAM or flash through the cache, however hard, does not starve the LCD DMA. Writing PSRAM does: each
written line is a dirty line, and dirty lines are written back when the shared 32 KB data cache evicts them,
interleaved with the fills that caused the eviction. That mix of reads and writes on the PSRAM is what the DMA
loses against. A large address-range writeback (`esp_cache_msync` over 64 KB) is the worst case: the cache walks
2048 line addresses with the DMA waiting. A whole-cache writeback every millisecond keeps the synthetic stream's
dirty set small enough to be harmless, but does not change the natural rate, so the natural stalls are not simply
"too many dirty lines".

Size of the hot PSRAM state in the quiet room: the Director is 2,312 bytes (72 lines), written every animation
tick. The framebuffer blit writes PSRAM too, chunked and written back at once per 4 rows; removing it entirely did
not change the natural rate, so it is not the natural trigger either.

## Director placement and animation (one binary, pad 64, 60 s windows, poll on)

The Director can be moved between PSRAM and internal RAM at runtime (`dev_probe {"dirloc":1}`, state copied
across), and the animation can be paused (`mode` bit 512: `home_face_tick` returns at once, nothing is rendered).

| State | Underruns / 60 s |
|---|---|
| Director in PSRAM | 7, then 13, then 7 |
| Director in internal RAM | 7, then 7 |
| Director in PSRAM, animation paused (static screen) | 6 |

Neither the Director's placement nor the animation matters for the natural stalls. On a completely static screen
the touch poll alone keeps producing them, at a rate set by the code layout. The 28 Oct "Director in PSRAM" fix
therefore worked by moving code, not by moving data; the same is true of this page's probe builds, whose rates
jumped between 0 and 13 per minute as code was added.

## Pixel clock, I2C speed (one binary, pad 224, 60 s windows, poll on)

`esp_lcd_rgb_panel_set_pclk` at runtime; the probe's reference gap re-calibrates after each change, so only the
underrun count is comparable.

| Setting | Underruns / 60 s |
|---|---|
| 12 MHz (baseline, twice) | 6, 3 |
| 10 MHz | 3 |
| 11 MHz | 1 |
| 13 MHz | 6 |
| 14 MHz | 7 |
| 12 MHz, I2C bus at 100 kHz instead of 400 kHz | 6 |

A lower pixel clock helps a little, in line with a FIFO-margin model, but does not remove the stalls, and 10 MHz
already means 35 frames/s. A slower I2C bus changes nothing, so it is the poll's transactions, not their duration.

## Instruction fetch versus APB traffic (one binary, pad 0 of a later build, 60 s windows)

| State | Underruns / 60 s |
|---|---|
| baseline, poll on (three windows) | 6, 21, 14 |
| poll off | 14 |
| poll off + core 0 executes 32 KB of NOPs from flash every 10 ms (instruction stream, twice the ICache) | 0, 0 |
| poll off + core 0 reads a GPIO register 2000 times every 10 ms (APB traffic, 2 % duty) | 10, 13 |

In this layout the stalls happen with the poll off as well, so the poll is one trigger among several and not the
mechanism. The striking result repeats the earlier one: any sustained cache-miss stream on core 0, be it
instruction fetch from flash, data reads from flash or data reads from PSRAM, takes the rate to 0, while a
busy-spinning core 1 makes it worse. APB traffic does nothing. Within one binary and one setting the rate moved
between 6 and 21, so single windows are noisy; the fix commit's build re-measured 0, 0 right after this run,
which rules out a slow drift such as temperature (the chip temperature is now in the perf report as `temp_c`).

Reading of the evidence: a `999` frame never shows a partial stall, so the DMA is not merely slowed, it stops for
the rest of the frame. With `CONFIG_LCD_RGB_RESTART_IN_VSYNC` the driver resets and restarts the GDMA channel at
every vsync while, in stream mode, that channel is already running the next frame's descriptors and may have a
PSRAM burst outstanding through the cache. A reset that lands on an in-flight burst is the kind of thing that
leaves a channel stuck until the next reset, and whether the burst is in flight at that instant is a timing
question that code layout and idle behaviour can plausibly shift. Refresh-on-demand mode avoids this: the DMA
stops after each frame and is started from an idle channel. Tested below.

## Refresh on demand (`-DREFRESH_ON_DEMAND=1`)

`cfg.flags.refresh_on_demand = 1`, one `esp_lcd_rgb_panel_refresh()` after panel init (the driver starts the
first frame only in stream mode: a first attempt without it measured a panel that never scanned, frame count 0,
and those "results" are discarded), and `esp_lcd_rgb_panel_refresh()` from the vsync callback to start each next
frame. Frame rate unchanged at 42.3 frames/s (2536 per 60 s window). Two layouts, 60 s windows:

| Load | Underruns / 60 s, pad 0 | pad 224 |
|---|---|---|
| none, poll on (twice) | 0, 0 | 0, 0 |
| none, poll off | 0 | 0 |
| PSRAM write stream (140 in stream mode) | 0 | 0 |
| PSRAM memcpy (45 in stream mode) | 0 | 0 |
| main loop busy-spinning (24 in stream mode) | 0 | 0 |

The two boot-time `999`s disappear as well. The probe's stall and vsync-lateness columns are not meaningful in
this mode: starting each frame from the vsync interrupt adds about 30 µs of period jitter that the probe's
running reference cannot follow (70 % of frames show "stalls" of 25 to 40 µs with an equal and opposite vsync
lateness).

This is the first configuration that survives every synthetic load with zero drops, at no memory or CPU cost on
the quiet room. What it says about the mechanism: the frame drops were caused by the driver's unconditional
reset-and-restart of a running DMA channel at every vsync (`CONFIG_LCD_RGB_RESTART_IN_VSYNC`, on in the Arduino
core), which in stream mode lands on a channel already prefetching the next frame; when that reset coincides with
an outstanding PSRAM burst, the channel stays stuck until the next vsync and the frame is lost. Any CPU traffic to
PSRAM raises the odds (hence the write-stream numbers), and the exact timing of idle code and interrupts decides
the rest (hence the layout lottery and the touch poll). Still to check with eyes on the panel: the restart
re-emits a vsync pulse at the frame boundary, which a panel driven in DE mode should ignore.

## Bounce buffers (`-DHAL_BOUNCE_PX=19200`, 40 lines, two 38 KB buffers in internal SRAM)

The DMA reads internal SRAM; an interrupt on the LVGL core copies the next 40 lines from the PSRAM framebuffer
into the other buffer. Same probe, 60 s windows, pad 0.

| Load | Underruns / 60 s |
|---|---|
| none, poll on (twice) | 0, 0 |
| PSRAM write stream (140 in direct mode) | 0 |
| PSRAM memcpy (45 in direct mode) | 0 |
| demo stream with page cycling, 60 s | 0 |

Costs on the page-cycling demo: CPU 42 → 55 %, LVGL 33 → 24 frames/s, worst frame 197 ms, internal heap
158 → 75 KB free. The vsync interrupt now arrives more than 10 µs late 250 to 740 times a minute (up to 0.9 ms
under the write stream) because the 38 KB fill copy runs in interrupt context on the same core; the probe's
stall figures are not meaningful in this mode. What the probe cannot see is whether a slow fill ever lets the DMA
read a half-filled buffer (the "vertical offset and flicker on page changes" that got bounce buffers rejected
earlier); that needs eyes on the panel under load. Robust against the measured mechanism; expensive.

## Touch interrupt wiring

The GT911's INT pin (`TP_INT`) ends on the TCA9554 expander (EXIO6), and the expander's own INT output goes to a
resistor with no net to any ESP32 pin (schematic: U9 pin 13 → R41). Interrupt-driven touch is not possible on
this board: the I2C poll stays, and the question becomes why a poll disturbs the LCD DMA at all.

## Where this stands (2026-10-28, late evening)

- **Fix: refresh-on-demand scan-out**, now the default in `firmware/src/core/hal.cpp` (`-DHAL_STREAM_MODE=1`
  restores the driver's stream mode for comparisons). The DMA stops after each frame and the vsync callback starts
  the next one from an idle channel, so the driver's reset of a running channel never happens.
- **Evidence**: 0 lost frames in every window on every layout tried, under the synthetic loads that lost 45 to 140
  frames a minute in stream mode, through a 10 minute quiet soak (25,338 frames, 0 lost) and through the
  page-cycling demo stream. Frame rate unchanged at 42.3 frames/s.
- **Cost**: page-cycling demo 28.8 LVGL frames/s and 45 % CPU against 33.5 and 42 % in stream mode on a different
  build (re-measure before reading much into it); no memory cost.
- **Mechanism, as far as it is established**: with `CONFIG_LCD_RGB_RESTART_IN_VSYNC` the driver resets and restarts
  the GDMA channel at every vsync. In stream mode that channel is already running the next frame's descriptors
  and can have a PSRAM burst outstanding through the cache; a reset at that instant leaves it stuck until the next
  vsync and the frame is lost. CPU writes to PSRAM raise the odds, idle timing and code layout shift them, which
  is why the GT911 poll, the Director's placement and 32-byte padding all appeared to matter.
- **To verify with eyes on the panel**: the restart re-emits a vsync pulse at the frame boundary. The panel runs in
  DE mode and should ignore it; look for any artefact at the top edge or a brightness beat.
- **Not needed after all**: bounce buffers, XIP from PSRAM, a slower poll, moving state between RAMs.
- **2026-10-29, native ESP-IDF build**: no change in lost frames (0 throughout); see "Native build" below for the
  stream-mode-without-restart and 64 KB D-cache data points.
- **Probe tooling kept**: `dev_touch` / `dev_probe` events (poll, synthetic loads, Director placement, animation
  pause, pixel clock, I2C speed, poll period), `LAYOUT_PAD`, the extra perf fields including `temp_c`. In
  on-demand mode only `lcd_underruns` (and `lcd_frames`) are meaningful; the stall and vsync-lateness columns
  read the restart jitter.

## Native build (plain ESP-IDF 5.5, 2026-10-29)

The firmware moved off the Arduino core (same sources, LVGL 8.4 and ArduinoJson from the IDF component registry,
pioarduino 55.03.312-1 / IDF 5.5.5). `sdkconfig.defaults` reproduces the Arduino core's configuration (240 MHz,
octal PSRAM 80 MHz, 16 KB/32 B I-cache, 32 KB/32 B D-cache, 1 ms tick, 4 KB internal-malloc threshold,
`CONFIG_LCD_RGB_RESTART_IN_VSYNC` on) so the two builds compare; the port only adds -O2 for the IDF libraries and
removes the secondary console from the USB-Serial-JTAG port. Measured on the same board, same evening, the released
Arduino 0.4.1 image against the native builds: quiet room = port held, nothing sent, home screen, 120 s windows (as
above); demo = `tools/dev.py perf --screen home --cycle --seconds 60`. The quiet-room render rate depends on what
the room decided to do after boot (2.7 frames/s offline-asleep, up to 10 with a random behaviour running), so the
later rows set the device clock to 14:00 first, which puts every build in the same offline state; the lost-frame
counts do not depend on it.

| Build | Scan-out | Quiet room: lost frames / 120 s | Demo, two 60 s runs: LVGL fps / frame ms / CPU % | heap_int free |
|---|---|---|---|---|
| Arduino 0.4.1 (released image) | on demand | 0, 0 (offline-asleep) and 0, 0 (clock set) | 30.7 / 12.9 / 47, 29.3 / 13.5 / 47 | 150.5 KB |
| native, Arduino's configuration | on demand | 0, 0 | 28.5 / 15.0 / 49, 29.3 / 14.3 / 48 | 178.5 KB |
| native, 64 KB D-cache, 64 B lines | on demand | 0, 0 | 30.5 / 14.1 / 50, 31.1 / 13.3 / 48 | 146.5 KB |
| native, stream mode, `RESTART_IN_VSYNC` off | stream | 0, 0 (stall and lateness counters 0 as well) | 29.5 / 14.4 / 49, 29.8 / 14.0 / 48 | 178.5 KB |
| native as committed (touch poll starts after the panel init) | on demand | 0, 0, 0, 0 (two with the clock set) | 29.1 / 14.7 / 50, 25.9 / 15.8 / 47 | 178.5 KB |

What it says:

- **Lost frames: no change.** Refresh-on-demand was already at 0 and stays there on every native build, 5,070 frames
  per window, including the page-cycling demo.
- **Stream mode with the driver's restart switched off** (the setting the Arduino core did not expose) measured 0
  lost frames and, for the first time in stream mode, 0 in the stall and vsync-lateness columns too. That is what
  the mechanism above predicts; it is one binary and two windows, and stream mode drops were layout dependent, so
  it stays an experiment (`-DHAL_STREAM_MODE=1` plus `CONFIG_LCD_RGB_RESTART_IN_VSYNC=n`) and on-demand stays the
  default.
- **The 64 KB data cache** did not change the render time beyond the run-to-run scatter and costs 32 KB of internal
  RAM: not adopted (two lines in `sdkconfig.defaults` if wanted).
- **Render time** on the demo stream reads about 1 ms per frame higher on the native builds (14.5 ms over eight
  runs against 12.9 and 13.5), and 2.6 against 2.2 ms per 10k-pixel frame in the quiet room; single runs scatter by
  ±1.5 ms, so this is at the edge of what two runs resolve and was not chased. Same compiler (GCC 14.2), same -O2
  for LVGL and our code in both builds.
- **Internal RAM**: 28 KB more free (no Arduino core objects, no BT/Wi-Fi stacks linked, no secondary console).
- Touch: the GT911 was polled at 100 Hz with 0 errors in every window once the poll starts after the panel init
  (started before it, the first polls hit the 50 ms I2C timeout behind the ~600 expander transactions of the init:
  8 errors at boot, none after).

## What Espressif says

From the ESP-IDF RGB LCD documentation and the `rgb_panel` example README ([docs](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32s3/api-reference/peripherals/lcd/rgb_lcd.html),
[README](https://github.com/espressif/esp-idf/blob/v5.2/examples/peripherals/lcd/rgb_panel/README.md)):

- "The PSRAM shares the same SPI bus with the main flash", so flash traffic (instruction cache misses, rodata,
  a filesystem, OTA, NVS) competes with the LCD DMA.
- For screen drift: lower the pixel clock; adjust `pclk_active_neg` and the porches; "Enable
  `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` and `CONFIG_SPIRAM_RODATA`, which can save some bandwidth of SPI0 from being
  consumed by ICache".
- Bounce buffers raise the achievable pixel clock but add CPU load, and the LCD "CANNOT function if the external
  memory cache is disabled", for example during flash writes.
- `CONFIG_LCD_RGB_RESTART_IN_VSYNC` restarts the DMA at vsync after a desync; "you may still notice flickering".

The Arduino core this firmware builds on (pioarduino 55.03.31, precompiled IDF) does **not** set
`CONFIG_SPIRAM_FETCH_INSTRUCTIONS` or `CONFIG_SPIRAM_RODATA`: all code and constants are fetched from flash
through the same cache the LCD DMA depends on. It does set `CONFIG_LCD_RGB_RESTART_IN_VSYNC`, flash 80 MHz QIO,
PSRAM 80 MHz octal, 16 KB instruction cache (8-way, 32-byte lines), 32 KB data cache.

The Waveshare wiki for the board ([ESP32-S3-Touch-LCD-4B](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4B))
gives no panel timing, no sdkconfig advice and lists no known display issues; its demos use the Arduino GFX
library. The schematic is at
[ESP32-S3-Touch-LCD-4B.pdf](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-4B/ESP32-S3-Touch-LCD-4B.pdf).

## Knobs that exist in the firmware

`firmware/src/core/hal.cpp`: `HAL_PCLK_HZ` (12 MHz), `HAL_BLIT_ROWS` (4), `HAL_BLIT_PAUSE_US` (0),
`HAL_BOUNCE_PX` (0, bounce buffers off), porches (hsync 8/50/10, vsync 8/20/10), `dma_burst_size` 64 (IDF 5.5 name of `psram_trans_align`).
`LAYOUT_PAD=<n>` (probe only) inserts n NOPs in flash to shift the code layout.
