#pragma once
// Hardware abstraction: LCD (ST7701 RGB 480x480), GT911 touch, backlight via CH32V003 expander.
#include <Arduino.h>

void hal_init();                       // power/reset panel, init touch + display + LVGL drivers
void hal_set_brightness(uint8_t pct);  // 0..100 (0 = backlight off)
uint8_t hal_get_brightness();
uint32_t hal_last_touch_ms();          // millis() of the last touch (for auto-dim / screensaver)
void hal_send_screenshot();            // stream framebuffer to host (see PROTOCOL.md)
