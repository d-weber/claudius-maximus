// Claudius Maximus — a desk companion that mirrors the Claude Code agents running on the host.
#include <Arduino.h>
#include <lvgl.h>
#include "core/hal.h"
#include "core/model.h"
#include "core/link.h"
#include "core/settings.h"
#include "ui/ui.h"

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
  link_poll();
  ui_tick();
  lv_timer_handler();
  delay(2);
}
