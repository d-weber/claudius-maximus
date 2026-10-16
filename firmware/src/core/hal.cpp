#include "hal.h"
#include <Wire.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_panel_ops.h>
#include <esp_cache.h>
#include <TouchDrvGT911.hpp>
#include "WS_CH32_IO.h"
#include "perf.h"

// ST7701 is configured over a bit-banged 3-wire SPI (Arduino_GFX bus + init table); pixels go through the
// ESP32-S3 RGB LCD peripheral, driven directly with esp_lcd so the panel config (bounce buffers) is ours.
static Arduino_DataBus *bus = new Arduino_SWSPI(GFX_NOT_DEFINED, 42, 2, 1, GFX_NOT_DEFINED);
static esp_lcd_panel_handle_t panel;
static uint16_t *fb;  // 480x480 RGB565 in PSRAM, panel orientation (= UI rotated by 180 degrees)

// Bounce buffers make an ISR memcpy the whole PSRAM framebuffer into SRAM on every panel refresh (~460 KB x
// ~42 Hz): that cost 25-40 % of render time on the LVGL core (measured, docs/PERF.md) plus 77 KB of internal
// RAM. At 12 MHz pclk (24 MB/s) the GDMA can scan PSRAM directly; CONFIG_LCD_RGB_RESTART_IN_VSYNC (on in
// the Arduino sdkconfig) re-syncs the panel at the next vsync if it ever underruns.
// Fallback if the image ever shows shifts/glitches: -DHAL_BOUNCE_PX=19200 (40 lines, the Arduino_GFX default).
#ifndef HAL_BOUNCE_PX
#define HAL_BOUNCE_PX 0
#endif

static void panel_init() {
  bus->begin();
  bus->sendCommand(0x01);  // software reset
  delay(120);
  bus->batchOperation(st7701_type1_init_operations, sizeof(st7701_type1_init_operations));

  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = 12 * 1000 * 1000;
  cfg.timings.h_res = 480;
  cfg.timings.v_res = 480;
  cfg.timings.hsync_pulse_width = 8;
  cfg.timings.hsync_back_porch = 50;
  cfg.timings.hsync_front_porch = 10;
  cfg.timings.vsync_pulse_width = 8;
  cfg.timings.vsync_back_porch = 20;
  cfg.timings.vsync_front_porch = 10;
  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
  cfg.num_fbs = 1;
  cfg.bounce_buffer_size_px = HAL_BOUNCE_PX;
  cfg.sram_trans_align = 8;
  cfg.psram_trans_align = 64;
  cfg.hsync_gpio_num = 38;
  cfg.vsync_gpio_num = 39;
  cfg.de_gpio_num = 40;
  cfg.pclk_gpio_num = 41;
  cfg.disp_gpio_num = GPIO_NUM_NC;
  const int data_pins[16] = {5, 45, 48, 47, 21, 14, 13, 12, 11, 10, 9, 46, 3, 8, 18, 17};  // B0-4 G0-5 R0-4
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = data_pins[i];
  cfg.flags.fb_in_psram = 1;
  ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  void *p = nullptr;
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &p));
  fb = (uint16_t *)p;
}

// Copy an LVGL band into the framebuffer rotated by 180 degrees, then write the touched rows back from the
// data cache (the LCD reads PSRAM through GDMA or the bounce-buffer copy).
static void fb_blit_rot180(const lv_area_t *a, const uint16_t *src) {
  const int w = a->x2 - a->x1 + 1;
  for (int y = a->y1; y <= a->y2; y++) {
    uint16_t *d = fb + (479 - y) * 480 + (479 - a->x1);
    for (int i = 0; i < w; i++) *d-- = *src++;
  }
  uint8_t *start = (uint8_t *)(fb + (479 - a->y2) * 480);
  size_t len = (size_t)(a->y2 - a->y1 + 1) * 480 * 2;
  esp_cache_msync(start, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static TouchDrvGT911 touch;
static bool touch_ok = false;
static uint8_t brightness = 100;
static uint32_t last_touch = 0;
static lv_disp_draw_buf_t draw_buf;

static lv_disp_drv_t disp_drv;

// Flush runs on core 0 (flush_task) so the rotate+copy into the PSRAM framebuffer overlaps with LVGL
// rendering the next band into the other draw buffer on core 1.
static TaskHandle_t flush_task_h;
static lv_area_t flush_area;
static lv_color_t *flush_px;

static void flush_task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint32_t t0 = perf_now_us();
    const lv_area_t *a = &flush_area;
    fb_blit_rot180(a, (const uint16_t *)&flush_px->full);
    perf_add(PERF_FLUSH, perf_now_us() - t0);
    lv_disp_flush_ready(&disp_drv);
  }
}

static void flush_cb(lv_disp_drv_t *, const lv_area_t *a, lv_color_t *c) {
  flush_area = *a;  // LVGL passes a stack copy: take it by value
  flush_px = c;
  xTaskNotifyGive(flush_task_h);
}

static void flush_wait() {
  while (disp_drv.draw_buf && disp_drv.draw_buf->flushing) taskYIELD();
}

// GT911 has no INT line on V4, so it must be polled. The I2C transactions (~0.3 ms every 10 ms) run in a
// task on core 0; touch_cb (LVGL, core 1) only reads the latest sample.
static volatile uint32_t touch_sample;  // bit31 = pressed, bits 16..25 = x, bits 0..9 = y
static volatile uint32_t touch_polls, touch_presses;

static void touch_task(void *) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(10));
    int16_t x[1], y[1];
    touch_polls++;
    if (touch.getPoint(x, y, 1) > 0) {
      int16_t lx = constrain(480 - x[0], 0, 479), ly = constrain(480 - y[0], 0, 479);
      touch_sample = 0x80000000u | ((uint32_t)lx << 16) | (uint32_t)ly;
      last_touch = millis();
      touch_presses++;
    } else {
      touch_sample &= ~0x80000000u;  // released: keep the last position
    }
  }
}

static void touch_cb(lv_indev_drv_t *, lv_indev_data_t *data) {
  uint32_t s = touch_sample;
  data->point.x = (s >> 16) & 0x3FF;
  data->point.y = s & 0x3FF;
  data->state = (s & 0x80000000u) ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

// The CH32 helper keeps its outputs latched across ESP32 resets (it is only reset by power-cycling),
// so a glitched I2C write during reflashing can leave the buzzer on. Re-assert the safe output state
// periodically: power/LCD/touch enabled, buzzer off. We never use the buzzer.
static void ch32_guard_cb(lv_timer_t *) {
  WS_CH32_IO::writeRegister(Wire, WS_CH32_IO::REG_OUTPUT, WS_CH32_IO::OUT_DISPLAY_ON, WS_CH32_IO::I2C_ADDRESS, 1);
}

void hal_init() {
  WS_CH32_IO::begin(Wire, 15, 7, 400000, nullptr);
  for (uint8_t addr : {0x5D, 0x14}) {
    touch.setPins(-1, -1);
    if (touch.begin(Wire, addr, 15, 7)) { touch_ok = true; break; }
  }
  if (touch_ok) {
    touch.setMaxTouchPoint(1);
    xTaskCreatePinnedToCore(touch_task, "touch", 3072, nullptr, 5, nullptr, 0);
  }

  panel_init();
  lv_init();
  size_t n = 480 * 60;
  auto *b1 = (lv_color_t *)heap_caps_malloc(n * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  auto *b2 = (lv_color_t *)heap_caps_malloc(n * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_disp_draw_buf_init(&draw_buf, b1, b2, n);
  xTaskCreatePinnedToCore(flush_task, "lv_flush", 4096, nullptr, configMAX_PRIORITIES - 3, &flush_task_h, 0);
  lv_disp_drv_t &dd = disp_drv;
  lv_disp_drv_init(&dd);
  dd.hor_res = 480;
  dd.ver_res = 480;
  dd.flush_cb = flush_cb;
  dd.draw_buf = &draw_buf;
  perf_init(&dd);
  lv_disp_drv_register(&dd);
  static lv_indev_drv_t id;
  lv_indev_drv_init(&id);
  id.type = LV_INDEV_TYPE_POINTER;
  id.read_cb = touch_cb;
  lv_indev_drv_register(&id);
  last_touch = millis();
  hal_set_brightness(100);
  ch32_guard_cb(nullptr);
  lv_timer_create(ch32_guard_cb, 3000, nullptr);
}

void hal_set_brightness(uint8_t pct) {
  if (pct > 100) pct = 100;
  brightness = pct;
  // CH32 PWM is inverted: 0 = full brightness, 255 = off
  WS_CH32_IO::setPwm(Wire, (uint8_t)((100 - pct) * 255 / 100));
}

uint8_t hal_get_brightness() { return brightness; }
uint32_t hal_last_touch_ms() { return last_touch; }
uint32_t hal_touch_polls() { return touch_polls; }
uint32_t hal_touch_presses() { return touch_presses; }

void hal_send_screenshot() {
  // Framebuffer is stored in panel orientation; display rotation 2 = 180°, host un-rotates.
  flush_wait();
  const size_t len = 480 * 480 * 2;
  Serial.printf("{\"t\":\"shot\",\"w\":480,\"h\":480,\"fmt\":\"rgb565le\",\"rot\":180,\"len\":%u}\n", (unsigned)len);
  const uint8_t *p = (const uint8_t *)fb;
  for (size_t off = 0; off < len; off += 4096) {
    size_t chunk = min((size_t)4096, len - off);
    size_t sent = 0;
    while (sent < chunk) sent += Serial.write(p + off + sent, chunk - sent);
  }
  Serial.flush();
}
