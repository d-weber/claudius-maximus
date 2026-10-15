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

## Not finished / next steps
1. **perf-core (render pipeline) was stopped mid-work** — its untested dual-core rework (flush + touch on core 0)
   is saved in docs/patches/perf-core-dualcore-WIP.patch, NOT applied. Unexplored: -O2 vs -Os, LVGL heap
   placement, lv_conf caches/IRAM, draw buffer size. Likely the next big global win (render ≈ 0.6 µs/px).
2. **perf-bench** was stopped before its final run; tools/bench.py exists (untested by the lead).
3. Remaining spikes (loop max 110–230 ms) come from alert card/toast slide-ins and full card redraws on status change.
4. Not yet verified on hardware by touch: tapping cards/planets, scrolling, swipe-to-close detail, settings sliders.
5. Suggestion pending: host system.py could round cores to 2% / mem to 0.1 GB to skip more redraws.
6. `iso-clawd/` in the repo root was not created by this team — left untracked.

## Daily use
The LaunchAgent daemon starts at login and feeds the device. Check: `cd host && .venv/bin/python -m companion status`.
Dev: `tools/dev.py flash|shot|perf` (see docs/TEAM.md, docs/PERF.md). Uninstall: `tools/uninstall.sh`.
