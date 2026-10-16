// Claudius Maximus — a desk companion that mirrors the Claude Code agents running on the host.
#include <Arduino.h>
#include <lvgl.h>
#include "core/hal.h"
#include "core/model.h"
#include "core/link.h"
#include "core/settings.h"
#include "ui/ui.h"
#include "core/perf.h"

void setup() {
  Serial.setRxBufferSize(32 * 1024);
  Serial.begin(115200);
  model_init();
  hal_init();
  settings_load();
  ui_init();
  link_init();
}

void loop() {
  uint32_t t0 = perf_now_us();
  { PerfScope p(PERF_LINK); link_poll(); }
  { PerfScope p(PERF_UI_TICK); ui_tick(); }
  uint32_t idle_ms;
  { PerfScope p(PERF_LVGL); idle_ms = lv_timer_handler(); }
  perf_loop(perf_now_us() - t0);
  // Sleep until the next LVGL timer is due (capped so serial input is still drained every few ms).
  delay(constrain(idle_ms, 1, 5));
}
