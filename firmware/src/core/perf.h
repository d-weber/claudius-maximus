#pragma once
// Lightweight performance counters. Query from host: {"t":"cmd","perf":1} -> {"t":"perf",...} (resets window).
#include "sys.h"
#include <lvgl.h>

enum PerfSlot { PERF_LINK, PERF_UI_TICK, PERF_LVGL, PERF_FLUSH, PERF_PARSE, PERF__N };

void perf_init(lv_disp_drv_t *drv);           // installs monitor_cb (chains with others)
static inline uint32_t perf_now_us() { return (uint32_t)esp_timer_get_time(); }
void perf_add(PerfSlot s, uint32_t us);        // accumulate a measured span
void perf_loop(uint32_t busy_us);              // once per main loop iteration (work time, excl. delay)
void perf_report();                            // send {"t":"perf"} line and reset window
// PROBE: the time of one home-room Director::draw
void perf_room_draw(uint32_t us);

// What the main loop is doing right now (0 = idle/delay, else PerfSlot + 1); read by the LCD vsync ISR to
// attribute late interrupts (hal.cpp probe). `perf_loop_task` = the main loop task, set by perf_init.
extern volatile uint8_t perf_phase;
extern void *perf_loop_task;

struct PerfScope {                              // RAII helper: { PerfScope p(PERF_LINK); ... }
  PerfSlot s; uint32_t t0; uint8_t prev;
  explicit PerfScope(PerfSlot slot) : s(slot), t0(perf_now_us()), prev(perf_phase) { perf_phase = (uint8_t)(slot + 1); }
  ~PerfScope() { perf_add(s, perf_now_us() - t0); perf_phase = prev; }
};
