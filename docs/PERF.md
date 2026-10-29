# Performance optimization round

## How to measure (everyone uses the same probe)
```
DEV_NAME=<you> host/.venv/bin/python tools/dev.py flash
DEV_NAME=<you> host/.venv/bin/python tools/dev.py perf --screen <id> --seconds 10 [--nodemo] [--detail] [--compact]
```
Output (window since last reset): `fps` (LVGL refreshes/s), `frame_ms_avg/max` (render time per refresh),
`px_per_frame` (invalidated pixels), `cpu_pct` (main-loop work / wall time; 100% = saturated),
`loop_max_ms` (worst loop iteration ⇒ touch latency), per-stage `link`/`tick`/`lvgl`/`flush`/`parse`, internal heap.
Demo stream (`shot`/`perf` default) is ~3x busier than real life and always has a waiting session (alerts glow on).
Use `--nodemo` to measure a static screen (nothing changes ⇒ should be ~0 fps / low CPU). The installed daemon steps
aside automatically while a dev tool holds the device.

## Targets
- Static screen, no data changes: fps ≈ 0–2, cpu_pct < 10%.
- Demo stream: cpu_pct < 50%, frame_ms_max < 80, loop_max_ms < 60 (touch stays responsive), animations smooth
  (small animated regions ≥ 20 fps).
- Internal heap free ≥ 60 KB, no leaks over time (heap stable across 60 s).

## Baseline (2026-10-15, demo stream, 8 s)
| screen | fps | frame avg/max ms | px/frame | cpu % | loop max ms | lvgl avg ms | link avg ms |
|---|---|---|---|---|---|---|---|
| home | 12.0 | 57 / 154 | 98k | 88 | 196 | 11.8 | 2.2 |
| agents | 8.6 | 93 / 218 | 131k | 98 | 258 | 93.8 | 15.0 |
| feed | 6.7 | 108 / 224 | 155k | 95 | 279 | 27.4 | 6.9 |
| usage | 8.1 | 90 / 222 | 131k | 93 | 265 | 20.9 | 3.8 |
| system | 9.1 | 77 / 204 | 77k | 91 | 301 | 17.0 | 3.1 |
| orbit | 7.0 | 92 / 155 | 153k | 96 | 422 | 41.8 | 6.8 |
| settings | 14.1 | 44 / 123 | 53k | 81 | 160 | 6.9 | 1.4 |
JSON parse of a state ≈ 5.6 ms (≈ 6/s). Flush (copy to PSRAM framebuffer) ≈ 2 ms per 60-line band.

## Results
(agents append a row per change: who, what, before → after)
- **perf-home** — home: Clawd re-recorded as a display list of solid rects (fixed slots per element) and only the
  pixel-exact difference between the old and new list is invalidated (rect XOR, coalesced, capped at 6 areas), instead of
  Clawd's whole ~350x200 box per pose change; resume no longer invalidates the 480x242 face; bubble fade in 24-opa steps;
  refresh() only touches labels/styles/flags whose value changed; update() skips when sessions/totals/usage versions,
  event count and the second are unchanged. A/B under one device lock, same tree otherwise (fps / frame avg/max ms / px/frame / cpu %):
  demo 13.1-16.1 / 32-37/85-138 / 75-79k / 65-70 → 15.9-17.4 / 17-20/84-118 / 26-28k / 50-51;
  demo w/o waiting sessions (no alert glow) 14.5 / 27.5/79 / 73k / 59 → 14.5 / 15.3/75 / 26k / 43;
  all idle, static data 6.5 / 25.5/52 / 66k / 38 → 4.1 / 5.4/20 / 6.8k / 24.5;
  --nodemo (offline, sleeping Clawd + z's) 4.0 / 6/29 / 24k / 12.4 → 4.5 / 4/28 / 8k / 12.5.
  Remaining demo pixels are mostly card/stats label changes, bubble pop/fade, and alert toasts/confetti (perf-alerts).
- **perf-feed** — feed + detail. Feed: row widgets stay bound to their item (stable uid/version); a new event recycles
  one widget to the top (one list re-layout) instead of rebinding all 18 rows, and only rows whose content changed
  (new / merged "x N") get labels/styles touched, change-only; renders coalesced to ≤ 1 per 250 ms; on_event only
  records when the tile is hidden or the detail overlay covers it; whole-list slide + row fade replaced by a 700 ms glow
  on the new row's 40x40 glyph disc; age labels 1 s steps < 15 s then 5 s steps, set only when the string changes;
  sparkline bars / rate / chips updated only on change. Detail: every style/flag/text set is change-only (show() no
  longer re-invalidates visible cards; bg/text/border colors compared first), recent-events list rebuilt only on
  events / status change; uptime/ages repaint just their labels. A/B under one device lock, same tree otherwise
  (fps / frame avg/max ms / px/frame / cpu % / loop max ms):
  feed demo 7.1-7.6 / 94-96/207-223 / 144k / 91-92 / 244-277 → 27.6-28.0 / 24-25/176-180 / 27.5k / 87.5 / 218;
  feed --nodemo 1.0 / 30-36/38 / 10-12k / 12-13 → 1.0 / 21-25/32 / 5.5k / 11;
  detail demo 7.3 / 91-93/187 / 167-173k / 93-95 / 285 → 11.7-12.1 / 40-42/131-138 / 51-52k / 69-70 / 169-183;
  detail with data, no stream 25.6 / 29/40 / 37k / 81 → 25.3 / 29/55 / 39k / 80 (identical: this is the alert edge
  glow + toasts, not the detail). Remaining feed/detail cost under demo is dominated by the alert glow/toasts
  (perf-alerts); feed's own cost is one ~150k px list repaint per row shift (≤ 4/s) — inherent to moving every row.
- **[bench] suite** — `tools/bench.py` (perf-bench): fixed demo seed, per screen (home agents feed usage system orbit
  settings detail) × {static (fixed state, no waiting, nothing sent), wait (same + 1 waiting session → glow),
  stream (8 s demo), burst (30 events in 1 s + 1.5 s tail)} + nav (one window per tile jump) + alerts (stop/error/waiting
  demo) + 120 s soak (heap per screen visit, cycle 2 − cycle 1). One device-lock hold per scenario (≤ ~10 s, soak 126 s);
  it waits for the previous scenario's leftovers to die out before measuring. Full run ≈ 9 min.
  `DEV_NAME=<you> host/.venv/bin/python tools/bench.py run --label x [--screens agents,feed] [--scenarios static,stream,burst]`
  → table + JSON in firmware/.pio/bench/ (or $BENCH_OUT); `bench.py compare A.json B.json` → per-metric A>B with
  `+` green better / `!` red worse (>10 % and a per-metric floor). `--pin firmware` snapshots+builds+flashes the tree
  first and marks scenarios that ran after someone else flashed. `dev.py flash` now stamps firmware/.pio/lastflash.json
  (who/when/source-tree hash) so every bench row records which build it measured.
- **[bench] finding (mid-round, mixed builds 16:50–16:56):** no heap leak — 120 s soak heap_int +0 B on every screen,
  psram −2.6 KB. Outliers for owners:
  - **agents** (perf-agents): *static* screen with nothing changing still 10–22 fps and **loop_max 121–128 ms /
    frame_max ~120 ms** (reproduced 3× in isolation) — something periodic does a ~120 ms repaint/block. Tile jump
    `nav>agents` = frame avg 112 ms, lvgl avg 114 ms, 128k px/frame (worst transition by far).
  - **orbit** (perf-orbit): static 14 fps / 29 % cpu (9k px/frame — small, but above the 0–2 fps static target).
  - **bursts** (perf-alerts): 30 events/s costs 70–94 % cpu on *every* screen, even settings (65k px/frame, lvgl
    avg 29 ms) and detail (frame avg 72 ms) — toasts/confetti/chips on lv_layer_top dominate, not the tiles.
  - **stream loop_max** is 150–240 ms on all screens (target < 60): frame_max 120–190 ms, parse max ~10 ms, so it's
    full-tile repaints, not JSON.
- **perf-usage** — usage: labels only re-set when the formatted string changes (`lv_label_set_text` always
  invalidates, even with identical text); ring indicator color only re-applied when it changes (a same-value
  `lv_obj_set_style_arc_color` invalidated each whole 200x200 ring = 2x40k px per state); model-mix segments only
  resized/hidden when their width changes; chart: new points diffed against the shown ones, only changed bar columns
  invalidated (`lv_chart_set_value_by_id`), whole chart only when >half the bars change (tpm shift, once/min in real
  life) or the mode changes; no more `lv_chart_refresh` per state. Rings still animate only on a real value change;
  countdowns are minute-granular strings, so they redraw once/min. A/B under one device lock, same tree
  (fps / frame avg/max ms / px/frame / cpu %): demo 6.6-7.1 / 87-93/185-190 / 123-132k / 80 →
  8.0-8.1 / 30/96-102 / 34k / 44-45 (settings on the same tree, ≈ global alert/toast overhead: 9.3 / 22/90 / 24k / 39);
  --nodemo 0 fps / 9.2% → 0 fps / 9.8% (unchanged, already static). Remaining ≈10k px/state over the global floor:
  demo shifts tpm every state → whole 424x62 chart redraw + stats/card labels; real host only shifts tpm once a minute.
  Host hint (perf-link, usage.py): keep `tpm`/`hourly` arrays and `today.*` stable between minute boundaries
  (no sub-minute jitter) and round `cost` to cents so strings stay unchanged.
- **perf-link** — data path (firmware link/model + host daemon/providers). **Protocol: partial states** (PROTOCOL.md
  "Partial `state`"): fw 0.2.0 hello carries `"caps":["part","screen"]`; the host then sends a full state only on
  connect/hello/refresh and afterwards `{"t":"state","part":1,ts,tz,<only changed top-level parts>}` (+ 48-byte empty
  heartbeat every 2 s). Old firmware / dev.py demo streams still get full states (verified both). Device reports the
  visible screen (`{"t":"screen","id":..}`); host streams `system` only while the system screen shows (else every 30 s)
  and runs `ps` every 30 s instead of 4 s. `sessions`/`totals` changes of only counters (tokens/cost/ctx_pct/tool_count/
  *_today) throttled to 5 s; `usage` recomputed only on minute boundaries (stable tpm/hourly/today, cents);
  `usage.*.at` floored to the minute; event bursts capped at 12/tick (older `tool` events dropped).
  Firmware: bulk `Serial.read(buf,n)` + memchr; parse into a reusable doc that is **swapped** into the model (full
  state / event: zero deep copy); partials either merge in place (small) or carry the unchanged parts over and swap
  (big, e.g. sessions), whichever copies less. `{"t":"cmd","perf":1}` now also emits `{"t":"linkperf",...}`
  (per message type: n, bytes, parse/apply avg+max ms).
  Real daemon feed, home (before → after): state msgs 1.5/s × 6.8 KB = 10.2 KB/s → ~0.5/s, 0.9 KB/s;
  link total 7.9 % → 1.2 % of wall (avg 0.24 → 0.03 ms/loop), link max 59 → 22 ms, parse avg 9.0 → 2.1 ms,
  cpu_pct 35 → 19-29 % (other teams' changes mixed in); usage screen: 0 state parts in 12 s (heartbeats only), link
  max 5 ms; system screen: 1 × 1.3 KB/s instead of 7.7 KB states. Demo stream (full states, dev.py): link avg
  2.2 → 0.43 ms (home), full-state apply 3.3 ms → swap (~0).
  Host daemon CPU (in-process, 60 s, busy machine w/ ~10 agents): self 0.8-1.0 % + ps children 0.6-1.0 % →
  self 0.4-0.7 % + children 0-0.3 % on non-system screens (TICK 0.25 → 0.5 s with burst coalescing, non-live
  transcripts stat'ed every 2 s, usage rescan 10 s, no snapshot while disconnected).
  **Screen owners — adopt `model_part_version(part)` (model.h)**: returns the state version at which that top-level
  part last changed; `update()` should early-out when the parts it shows are unchanged:
  home: sessions+totals+usage; agents: sessions+totals; feed/detail: sessions; usage: usage+totals; system: system;
  orbit: sessions; alerts/ambient: sessions(+totals). `model_state_version()` no longer bumps on empty heartbeats
  (still at least every 10 s), so relative times should come from the screens' own 1 s timers.
- **perf-agents** — agents: (1) guarded style setters (`sp_col`/`sp_num` compare the local value first) for every
  per-update style write — `lv_obj_set_style_opa(list)` alone invalidated the whole 456x402 list on every state;
  label long-mode only set when it differs; (2) update() skipped when `model_part_version("sessions")` and the link
  state are unchanged; (3) stable order within a status (cards keep their on-screen slot; the host re-sorts by recency on
  every tool call, which re-bound + fully redrew two cards per tool call: rebinds 6-20 → 0 per 5 s); (4) pulses = one
  90 ms timer stepping only the thin strips' bg_opa (quantized), replacing per-card full-rate lv_anims; (5) new-session
  fade uses text_opa on the name (no whole-card opa layer) and only for genuinely new sessions; (6) 4 cards pre-created
  at boot (first-show update 106 → 60 ms). Measured update() cost: avg 4.3 ms, max ~11 ms; on_event < 1 ms.
  demo (fps / frame avg/max ms / px/frame / cpu % / loop max ms / lvgl avg ms):
  11.1-11.5 / 69-72/218 / 99-103k / 98 / 257 / 71-74 → 43-45 / 12/129-131 / 8.9-9.0k / 73 / 148-158 / 4.5
  (high fps = small strip/flash frames). --nodemo (host offline, static): 36 / 11/126 / 11k / 51 → 1.0 / 23/24 / 28k / 11.6
  (one 5.5-fps window right after the stream stops = the offline dim transition). Pulse-off A/B: strips cost ~6-10% cpu.
  Remaining: ~130 ms frames on status changes (card refill + re-sort) and alert toast/glow (perf-alerts); JSON parse 6 ms.
- **perf-alerts** — alerts overlay (fx/alerts.cpp) + settings About:
  (1) glow: halo 15 px + mid 7 px are now static; only the 4 px core breathes, quantized to 6 levels (~6 redraws/s,
  70 ms check timer, was 25 Hz on all 12 bars); style writes skipped when the level is unchanged; error flash 20 Hz.
  (2) **LVGL 8.4 gotcha for everyone**: every object invalidation is grown by 5 px on each side
  (`lv_obj_get_transformed_area` → `lv_area_increase(area,5,5)`), so a 4 px bar redraws 9-14 px. The glow sets its
  styles with `lv_disp_enable_invalidation(d,false)` and invalidates the exact rects via `_lv_inv_area` → 16.9k → 7.4k px/frame.
  (3) card/pill/toast/chip labels only rewritten when the text changed; card waiting time has minute resolution
  ("just now" / "N min" / "1h05") → a static card costs 0 px. (4) `lv_obj_move_foreground` invalidates the WHOLE
  screen when the order changes: now only raised above foreign layer_top objects (ambient), never among our own.
  (5) confetti 22 → 10 pieces @25 fps, ~1.2 s, auto-stop (22 pieces = 44 inv areas > LV_INV_BUF_SIZE 32 ⇒ full-screen
  redraw every confetti frame). (6) card slide 150 px/380 ms → 70 px/260 ms, shadow 24 → 14. (7) alerts_update()
  early-outs on unchanged `model_part_version("sessions"/"totals")` (queue pruning moved to the 1 s tick).
  (8) Settings About: labels/colour only set when changed, heap shown at KB resolution.
  Glow cost isolated (crafted static state, 1 waiting session, no stream; settings tile):
  25+ fps / 41k px/frame / cpu ~72 % (old build) → 6 fps / 7.4k px / 8.7-11 ms / +5-6 % cpu over the no-wait idle
  (no-wait: 0 fps). home static waiting: 10.4 fps / 5.7k px (incl. home's own anim).
  Demo stream (fps / frame avg/max / px/frame / cpu % / loop max), before → after (other teams' changes mixed in):
  settings 13.9 / 44/124 / 53.8k / 80 / 171 → 14.3 / 20/90 / 20.8k / 42 / 97;
  home 11.9 / 58/159 / 100k / 88 / 198 → 16.9 / 16/58 / 22.8k / 42 / 93;
  agents (same tree, mid-round) 52.4 / 9.1/110 / 7.3k / 69 / 155 → 55.4 / 8.4/88 / 5.6k / 62 / 125.
  NB: `dev.py flash` builds *before* waiting for the lock, so a flash that queued for minutes ships an old tree —
  numbers showing ~25 fps of glow on static screens come from such stale builds.
  Remaining: card/toast slide-ins (~260-280 ms of ~60-90k px frames) and 1 full-screen frame per celebration
  (toast raise); glow ≈ 6 redraws/s × 7.4k px while anyone waits.
- **perf-system** — system: profiled update() (esp_timer, temp log): old cost ≈ 46 ms per 1 Hz change
  (mem 15 ms + claude 12 ms = `lv_obj_align_to`, which forces a whole-screen layout pass; procs 10 ms; cpu 8 ms).
  Changes: (1) no `lv_obj_align_to` — "right of" positions computed with `lv_txt_get_width`, only when the text
  changed; (2) every label/text color/bg color/hidden flag set change-only (`lv_label_set_text`, `set_style_*`,
  `clear_flag(HIDDEN)` all invalidate even when equal); colors bucketed to 5 % steps (`load_color_q`);
  (3) bars: no 500 ms animation (22 bars × anim = ~30 extra frames/s), solid bucketed color instead of a horizontal
  gradient; (4) CPU (310×74) and net (62×48) charts are now opaque RGB565 canvases in PSRAM rendered by hand
  (row-major fill with a per-row 4-color table, 2-px AA line, grid in-canvas), re-rendered only when points/scale/
  color change — LVGL just blits them instead of masked faded-area fills; (5) update() early-outs on
  `model_part_version("system")`, then hashes 6 sections (header/cpu/mem/procs/net/claude, ArduinoJson hash writer,
  no buffer) and only rebinds changed ones (procs: every 4-30 s). update() now ≈ 0.5 ms on unchanged states and
  ≈ 12-14 ms on a 1 Hz change (cpu chart render 3.9 ms — PSRAM-write bound, ~160 ns/px; 16 core rows 1.5-3 ms;
  mem labels 2-3.5 ms; net 1.5 ms). A/B under the same tree, demo stream + real Mac metrics (fps / frame avg/max ms /
  px/frame / cpu % / loop max ms / tick max ms): 14.8-17.2 / 29-33/197 / 30-35k / 69 / 281 / 51-53 →
  8.1-8.3 / 32-35/185 / 33-36k / 45-47 / 230 / 14.7-14.9; --nodemo 0 fps / 9.4 % → 0 fps / 8.6 %.
  Remaining: frame max/loop max ~185-230 ms are the alert glow/toast frames (perf-alerts), JSON parse 6.5 ms × 6/s.
  Possible next step if heap allows: 4-bit indexed CPU canvas in internal RAM (11.5 KB) would cut the 3.9 ms chart
  render to < 0.5 ms. Host hint (perf-link, system.py): round `load` to 1 decimal is already done; sending `cores`
  quantized to 2 % and `mem.*` to 0.1 GB would skip more label updates.
- **perf-orbit** — screens/screen_orbit.cpp + fx/ambient.cpp:
  (1) **The 47 ms "calc" was the ring re-bake**: every status change re-ran `lv_canvas_fill_bg` + 52 stars + Clawd +
  8 `lv_canvas_draw_arc` on the 480x480 canvas (~0.4-0.7 s per bake, ~1/s under demo) and then invalidated the
  whole canvas (full-screen frame). Now stars + Clawd are drawn once into a pristine PSRAM copy (`base_buf`), and
  rings are plotted straight into the canvas buffer with an 8-way Wu circle (~1.2 k px, < 0.5 ms), erased by
  restoring from `base_buf`, **one ring per frame**, invalidating only 16 sector rects around that ring (~25-35 k px).
  (2) Own dirty-rect collector: planets/labels/moons/sparks/ripples move by shifting `coords` and register old+new
  rects in a ≤10-entry merging list flushed once per frame via `_lv_inv_area` (no LVGL +5 px slack, no 32-area
  overflow ⇒ no accidental full-screen frames; was ~40-80 areas/frame with sparks). Spark/ripple opacity lives in
  per-object `lv_style_t`s changed without LVGL's refresh path (quantized 16/32 steps); sparks/ripples shown/hidden
  and resized through the collector (no set_size → no layout pass snapping them to style positions).
  (3) Labels: opaque square C_BG tag (was 70 % + radius 4: blended + corner mask), transparent only while crossing
  Clawd; 3 px move hysteresis (was 2). (4) Adaptive timer: 70 ms (~14 fps) while busy/subagents/fx/settling,
  110 ms if only waiting (sonar), 125 ms when the backlight is dimmed, 250 ms when everything is calm (was fixed
  60 ms). (5) update() and ambient_update() early-out on unchanged `model_part_version("sessions"/"totals")`.
  (6) ambient: the night clock re-set the 120 px clock's text colour every 100 ms (style set = invalidate ⇒ ~10
  redraws/s of the digits); now colours/opacity only on change and the saver work runs once per second.
  Orbit hidden: timer paused, on_event returns early, update not called (verified: other screens show no orbit cost).
  orbit demo (fps / frame avg/max ms / px/frame / cpu % / loop max ms / lvgl avg ms), before → after, 2 runs each:
  8.6 / 72/157-181 / 102-110k / 92 / 412-413 / 19.6-20.7 → 14.1-14.4 / 38-39/122-131 / 22.0-22.4k / 70-72 / 155-170 / 4.1-4.4.
  Screen's own log: calc 47 ms → 1.5 ms; render avg 116 → 42 ms. --nodemo (last demo state, 4 busy):
  16.5 / 27/41 / 16k / 53 / 43 → 14.0 / 20-22/33 / 8.2-9.7k / 36-39 / 34-35.
  Night clock (forced saver, no stream): 0 fps, 0 px, cpu 9 % (link polling floor), loop max 4 ms.
  Remaining: frame max ~120-130 ms / loop max ~155-170 ms are not orbit's: they stay with planets frozen + fx off
  (`dev_orbit` flags 6) ⇒ alert card/toast slide-ins + JSON parse; a ring change frame ≈ 30 k px (~20 ms).
- **perf-core** — global render pipeline (2026-10-16, device used exclusively, one device-lock hold per variant,
  same tree (HEAD 8d69ad6) otherwise, demo 8 s per screen; frame avg ms home/agents/feed/usage/system/orbit/settings).
  Root cause of the ≈0.6 µs/px: Arduino_GFX's RGB panel uses **bounce buffers**, i.e. an ISR on the LVGL core
  memcpy's the whole 460 KB PSRAM framebuffer into SRAM on every panel refresh (~42 Hz ≈ 19 MB/s), stealing
  25–40 % of core 1 and 77 KB of internal RAM.
  - base: 16.6 / 15.0 / 14.6 / 25.6 / 26.5 / 41.4 / 19.2; static cpu 9.4 %; heap_int 79 KB.
  - **KEPT — own esp_lcd RGB panel setup in hal.cpp, no bounce buffer** (GDMA scans PSRAM directly; ST7701 init
    still via Arduino_GFX SWSPI + init table; 180° blit + esp_cache_msync in flush): 12.2 / 9.3 / 9.0 / 17.5 / 20.0 /
    26.6 / 13.6 (−26…−38 %), cpu −9…−15 pts, **heap_int 79 → 156 KB**. Same driver with the 40-line bounce buffer =
    base numbers (control). Fallback if the panel ever shows shifted/garbled lines: `-DHAL_BOUNCE_PX=19200`.
  - **KEPT — async flush on core 0** (flush task: blit+cache writeback overlaps LVGL rendering the next band into the
    2nd draw buffer): 10.5 / 8.0 / 7.8 / 15.4 / 18.0 / 23.8 / 12.3 (≈ −10 %). Note: `flush` stage time is now core-0
    time, no longer inside cpu_pct.
  - **KEPT — GT911 polling in a core-0 task** (100 Hz, touch_cb only reads the last sample) **+ loop sleeps until the
    next LVGL timer (1–5 ms) instead of delay(2)**: static cpu 6.4 → 1.2 %, demo cpu −3…−4 pts. perf now reports
    `touch_hz` (≈100), `touch_presses`, `up_s`.
  - **KEPT — `-O2` instead of `-Os`** (our code + LVGL/GFX/JSON; prebuilt Arduino libs unaffected): −3…−8 % frame
    time, +96 KB flash (1.24 MB of 6.5 MB).
  - REJECTED (no measurable gain): LVGL heap internal-first with 64 KB reserve, or big (≥1 KB) allocs internal
    (frame times identical to PSRAM heap); `LV_ATTRIBUTE_FAST_MEM IRAM_ATTR` (≤ 5 %, costs 12 KB internal RAM);
    `LV_MEMCPY_MEMSET_STD`, `LV_CIRCLE_CACHE_SIZE 16`, refresh period 16 ms (identical); draw buffers 2×480×96
    instead of 2×480×60 (identical, −69 KB RAM).
  - REJECTED — the earlier WIP (docs/patches/perf-core-dualcore-WIP.patch): it moved the *bounce-buffer ISR* to core 0
    via gfx->begin() in a core-0 task. Same speed gain, but **USB-CDC bulk TX then breaks**: screenshots truncate
    (0/8 right after boot, intermittently later), also with the CDC ISR on core 0, a staging buffer or a 1 s TX timeout.
    Removing the bounce buffer instead has no such issue (8/8 shots after flash, 9/9 + 3/3 during soaks).
  - Stability: 10-min soak (demo stream, screen cycle every 25 s incl. detail, brightness toggle every 7 s, shots):
    no reset (uptime monotonic), heap_int flat 147 484 B, psram flat, touch_hz 100, all shots intact; 60 s of
    brightness writes every 0.5 s (CH32 I2C on core 1) vs touch polling (core 0): zero error lines. 4-min soak on
    the final -O2 build: same. **Not verifiable remotely: physical touch and the panel image itself (only the
    framebuffer is captured) — check by eye/finger: no shifted/flickering lines, taps/swipes/sliders work.**
  - Final (all kept changes, 2 runs each), base → after: see docs/STATUS.md table.
  - Notes for owners: remaining frame_max / loop_max spikes (55–125 ms) are single full-area repaints from screen
    content (alert toasts sliding over a tile, orbit's ~23k px/frame at 18 fps = orbit's own animation), not the
    pipeline. Orbit is still the most expensive per pixel (~1.1 µs/px: many overlapping AA arcs/circles).

- **touch-fix / scan-out** (2026-10-16, first use by hand on the panel): swipes became taps (SensorLib getPoint()
  ignoring the GT911 buffer-ready bit: own status-bit-aware poll, perf `touch_*`), and the image jumped sideways
  for a frame now and then. New scan-out probe in hal.cpp (RGB driver vsync + frame-complete callbacks): the DMA
  finishes a frame a steady 465 us before vsync; the gap's shortfall = stall = sideways displacement (10 us =
  120 px); the vsync ISR's own lateness against the fixed frame period is measured too. Findings, demo stream 15-
  20 s: baseline `lcd_n10` 10-15, `vs_n10` 10-19, max ~87 us. Unchanged by: blit pauses (HAL_BLIT_PAUSE_US=30),
  pclk 10 MHz, 2-row chunks, a build that never writes the framebuffer, synthetic flash/PSRAM streaming from
  core 0 (zero stalls on a static screen), LVGL on its own TLSF pool. Zero with rendering but no host stream.
  Cause: Arduino HWCDC's USB-serial ISR (one xQueueSendFromISR per byte, 60-90 us per 64-byte packet with
  interrupts masked on core 1) delaying the vsync ISR in which the driver restarts the DMA. **KEPT — IDF
  usb_serial_jtag driver installed from core 0** (`Link` in core/link.cpp): `lcd_n10` = `vs_n10` = 0 on agents,
  home --cycle, orbit; agents 58 → 60 fps, heap_int 147 → 181 KB. Rejected: bounce buffers (frame desync with
  the core-0 flush competing for PSRAM: vertical offset + heavy flicker on page changes), GDMA memcpy flush
  (9.4 ms per 60-line band, 64-byte bursts, vs 0.9 ms for the cached CPU blit). perf `--cycle` changes page every
  1.2 s. `HAL_BLIT_ROWS` (4) chunks the blit's cache write-back; `HAL_BLIT_PAUSE_US`, `HAL_PCLK_HZ`, `HAL_BOUNCE_PX`
  remain as experiment knobs.
- **render-perf-levers** (2026-10-29, on the native build) — "what would improve fps or lower CPU": 26 builds
  measured on the board through one flash-and-measure queue (quiet room 60 s with the clock set to 14:00; home
  --cycle 60 s twice; per-screen demo 12 s; screenshots), each experiment on its own branch (`exp/*`, not merged).
  Baseline = native-idf as committed: 15.6 ms/frame, 46 % cpu, loop_max 189 ms in the page-cycling demo (two runs
  two hours apart agree to 1 ms), heap_int 174 KB.
  - **Why the renderer is slow**: a draw-op profiler (`exp/profiler`, `core/drawprof.cpp`, per-op timing in the
    perf JSON) splits a demo frame into ≈49 % gaps between draw ops (LVGL's object-tree walk, style lookups, draw
    descriptors: ≈170 µs per object), 44 % draw ops (translucent rects 12 %, rounded rects 10 %, text 15 % with
    layout, the one shadow 6.5 % at ≈2.9 ms each, gradients 1.5 %), pixel pushing (blend) 21 %, flush wait 1 %, no
    layer blends (LVGL 8 `opa` does not create layers). The work is code-bound, so instruction fetch is the lever
    and faster pixel code is not.
  - **KEPT — 32 KB instruction cache** (`CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB`, −16 KB internal RAM): 11.3 ms /
    39 % / loop_max 127 ms (−28 % frame time), orbit 20.5 → 13.1 ms per frame; no pixel change.
  - **KEPT — LVGL shadow cache 40 + circle cache 16** (1.6 KB .bss): with the cache above (= C1) 10.1 / 37 / 121
    (two runs 10.4 and 9.7 ms); alone 14.4 ms. The alert card's shadow was recomputed on every frame it was in.
  - Measured, available, not taken (RAM): `LV_ATTRIBUTE_FAST_MEM IRAM_ATTR` another ≈1 ms for 16 KB (C1 + IRAM +
    96-line draw buffers = C2: 9.2 / 35 / 112, heap_int 75 KB, too close to the 60 KB floor); 96-line draw buffers
    alone 13.4 ms for 69 KB.
  - Measured, not taken (UX): refresh period 24 ms + indev 20 ms (cpu −4…−11 pts but fps halves: feed 59 → 30,
    agents 37 → 22, animations alias); removing the alert/toast/chip/bubble fade animations (`exp/pixels` 225b23e:
    13.3 ms / 40 %, i.e. −3 ms for 0 RAM, but hides are abrupt); invalidating fewer pixels in the agents pulse strip
    and feed glow (`exp/pxframe`: agents cpu 36 → 25 %, feed 57 → 40 %, demo cpu −5…−7 pts; pulses step in 16
    levels and new feed rows show up to 0.25 s later; kept on the branch for later).
  - Measured, nothing: -O3 for LVGL + our code (15.7 ms, +131 KB flash, −16 KB RAM); 64 KB data cache; opaque tints,
    flat corners, no gradients (same as the fades alone); 30-line draw buffers (worse: 17.0 ms, home +6 ms);
    `LV_ATTRIBUTE_FAST_MEM` was ≤ 5 % on the Arduino build with the 16 KB cache — it is the I-cache that was short.
  - **LVGL 9.6 is out** (`exp/lvgl9`: full migration, 27 files, private API + -fpermissive): 26.1 ms / 62 % alone,
    25.1 with the ESP32-S3 SIMD assembly from esp_lvgl_port, 36.4 / 69 % with two draw units; +55 % frame time on
    this UI, one visual regression (feed row title wraps). Not a direction.
  - Idle: the quiet-room floor (3.7 % cpu at 2.7 fps) is the room tick (`home_face_tick`, 36 ms timer), not polling;
    an event-driven loop (`exp/idle`: block on USB RX / touch notification) gets 3.1 %, kept on the branch.
  - As merged (`perf-c1`, re-measured from the committed tree): page-cycling demo 31.9 / 10.3 / 39 and 28.4 / 10.1 / 35
    (fps / frame ms / cpu %) against the baseline's 25.5 / 16.4 / 49 and 22.2 / 16.2 / 42; loop_max 138 / 107 ms
    (211 / 175); per screen (frame ms, cpu %): home 17.7 / 55 (21.9 / 58), agents 6.0 / 29 (8.0 / 36), orbit 12.0 / 27
    (20.5 / 40), feed 6.1 / 48 (8.5 / 57); quiet room 1.8 ms, 3.0 % (2.5 ms, 3.7 %); 0 lost frames in every window;
    heap_int 178.5 → 160.3 KB; screenshots of home, agents, feed and detail identical to the baseline's.
- **native-idf** (2026-10-29) — the firmware is a plain ESP-IDF 5.5 project now (no Arduino core; same LVGL 8.4 /
  ArduinoJson 7 from the IDF component registry, `sdkconfig.defaults` = the Arduino core's configuration plus -O2 for
  the IDF libraries). Same board, released Arduino 0.4.1 image vs the native build, home --cycle 60 s (fps / frame
  avg ms / cpu %): 30.7 / 12.9 / 47 and 29.3 / 13.5 / 47 → 29.1 / 14.7 / 50 and 25.9 / 15.8 / 47 (eight native runs
  average 14.5 ms; single runs scatter ±1.5 ms); quiet room identical (2.7 fps, 3.7 % cpu, 0 lost frames in every
  120 s window); heap_int 150 → 178 KB; image 1.4 → 1.17 MB. Also measured, not adopted: 64 KB D-cache / 64 B lines
  (no frame-time change, −32 KB RAM) and stream-mode scan-out with the driver's restart-in-vsync off (0 lost frames,
  0 stalls: a data point for SCANOUT.md's mechanism). Details and tables in [SCANOUT.md](SCANOUT.md) "Native build".
- **scan-out-on-demand** (2026-10-28, later) — the stalls below came back with unrelated code changes; root cause and fix in
  [SCANOUT.md](SCANOUT.md): the driver's restart-in-vsync of a running DMA channel; scan-out now runs in
  refresh-on-demand mode (0 lost frames under loads that lost 45-140/min before).
- **scan-out-stalls** (2026-10-28) — `999` underruns with Clawd's Studio: 10-40/min visible, 2-4/min hidden,
  0 on the pre-room firmware. Device bisect with synthetic bursts: register-only compute harmless, a `.bss` word
  hammered per tick up to 30/2 min, layout dependent. **KEPT — the `Director` state is allocated in PSRAM**
  (`home_face.cpp`): 0 in every 120 s window (code / wait / idle visible, hidden). Rejected as unnecessary: engine in
  IRAM (+29 KB), `vsync_front_porch` 10 -> 40, canvases in PSRAM (worse while the state was internal). Measured with
  raw-serial perf windows: port held open, nothing sent, no demo stream (`tools/dev.py perf` with the daemon's demo
  stream is 3x busier than real life).
