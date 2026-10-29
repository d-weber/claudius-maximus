// Claudius Maximus — a desk companion that mirrors the Claude Code agents running on the host.
#include "core/sys.h"
#include <lvgl.h>
#include <nvs_flash.h>
#include <rom/ets_sys.h>
#include "core/hal.h"
#include "core/model.h"
#include "core/link.h"
#include "core/settings.h"
#include "ui/ui.h"
#include "core/perf.h"

// Everything runs in one task on core 1 (the Arduino core's loopTask: priority 1, 8 KB stack); the LCD flush
// and the touch poll are tasks of their own on core 0 (core/hal.cpp), the USB serial driver's interrupt is on
// core 0 as well (core/link.cpp).
static void loop_task(void *) {
  link_serial_init();
  model_init();
  hal_init();
  settings_load();
  ui_init();
  link_init();
  for (;;) {
    uint32_t t0 = perf_now_us();
    { PerfScope p(PERF_LINK); link_poll(); }
    { PerfScope p(PERF_UI_TICK); ui_tick(); }
    uint32_t idle_ms;
    { PerfScope p(PERF_LVGL); idle_ms = lv_timer_handler(); }
    perf_loop(perf_now_us() - t0);
    // Sleep until the next LVGL timer is due (capped so serial input is still drained every few ms).
    if (idle_ms < 1) idle_ms = 1;
    if (idle_ms > 5) idle_ms = 5;
    if (hal_probe_mode() & 16) esp_rom_delay_us(idle_ms * 1000);   // PROBE: busy idle
    else delay(idle_ms);
  }
}

extern "C" void app_main() {
  // NVS backs the settings (core/settings.cpp); a partition from an incompatible NVS version is wiped
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }
  xTaskCreatePinnedToCore(loop_task, "loop", 8192, nullptr, 1, nullptr, 1);
}
