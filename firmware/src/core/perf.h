#pragma once
// Lightweight performance counters. Query from host: {"t":"cmd","perf":1} -> {"t":"perf",...} (resets window).
#include <Arduino.h>
#include <lvgl.h>

enum PerfSlot { PERF_LINK, PERF_UI_TICK, PERF_LVGL, PERF_FLUSH, PERF_PARSE, PERF__N };

void perf_init(lv_disp_drv_t *drv);           // installs monitor_cb (chains with others)
static inline uint32_t perf_now_us() { return (uint32_t)esp_timer_get_time(); }
void perf_add(PerfSlot s, uint32_t us);        // accumulate a measured span
void perf_loop(uint32_t busy_us);              // once per main loop iteration (work time, excl. delay)
void perf_report();                            // send {"t":"perf"} line and reset window

struct PerfScope {                              // RAII helper: { PerfScope p(PERF_LINK); ... }
  PerfSlot s; uint32_t t0;
  explicit PerfScope(PerfSlot slot) : s(slot), t0(perf_now_us()) {}
  ~PerfScope() { perf_add(s, perf_now_us() - t0); }
};
