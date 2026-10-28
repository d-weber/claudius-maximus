#pragma once
// Hardware abstraction: LCD (ST7701 RGB 480x480), GT911 touch, backlight. Board pin map: board.h.
#include <Arduino.h>

void hal_init();                       // power/reset panel, init touch + display + LVGL drivers
void hal_set_brightness(uint8_t pct);  // 0..100 (0 = backlight off)
uint8_t hal_get_brightness();
uint32_t hal_last_touch_ms();          // millis() of the last touch (for auto-dim / screensaver)
uint32_t hal_touch_polls();            // GT911 polls so far (touch task, ~100/s) — perf probe
uint32_t hal_touch_presses();          // polls that saw a finger
uint32_t hal_touch_glitches();         // one-scan presses/releases the debounce swallowed
uint32_t hal_touch_scans();            // fresh GT911 scans seen (controller rate, finger or not)
uint32_t hal_lcd_frames();             // panel vsyncs so far
uint32_t hal_lcd_underruns();          // frames whose DMA did not finish before the vsync at all
int32_t hal_lcd_ref_us();              // reference gap frame-complete -> vsync (probe calibration)
void hal_lcd_stall_stats(int32_t *smax, uint32_t *n10, uint32_t *n30, uint32_t *n100);  // scan-out stalls since last call: max us, frames > 10/30/100 us (a 10 us stall = 120 px sideways jump)
void hal_lcd_vsync_stats(int32_t *lmax, uint32_t *n10, uint32_t *n30);  // vsync ISR lateness since last call: max us, frames > 10/30 us
void hal_touch_set_poll(bool on);               // PROBE
void hal_probe_set(int mode);                   // PROBE
void hal_probe_poll_ms(int ms);                 // PROBE
void hal_probe_i2c_hz(int hz);                  // PROBE
void hal_probe_pclk(int hz);                    // PROBE
int hal_probe_mode();                           // PROBE
void hal_lcd_probe_extra(char *buf, size_t n);   // PROBE: ",\"fc_core\":..,\"vs_core\":..,\"late_eof\":[..]"
void hal_lcd_anomaly_log(char *buf, size_t n);   // JSON array of the last anomalies: "stall_us/late_us/p<phase>/+ms_since_previous"
uint32_t hal_touch_errors();           // I2C reads of the status register that failed
const char *hal_touch_ic();            // GT911 product id string read at init ("911"), "?" if unreadable
uint8_t hal_touch_rate_ms();           // controller scan period from its config
void hal_send_screenshot();            // stream framebuffer to host (see PROTOCOL.md)
