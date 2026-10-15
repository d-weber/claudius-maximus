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
