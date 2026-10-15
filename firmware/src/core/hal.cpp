#include "hal.h"
#include <Wire.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <TouchDrvGT911.hpp>
#include "WS_CH32_IO.h"
#include "perf.h"

static Arduino_DataBus *bus = new Arduino_SWSPI(GFX_NOT_DEFINED, 42, 2, 1, GFX_NOT_DEFINED);
static Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    40, 39, 38, 41,
    46, 3, 8, 18, 17,
    14, 13, 12, 11, 10, 9,
    5, 45, 48, 47, 21,
    1, 10, 8, 50,
    1, 10, 8, 20);
static Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    480, 480, rgbpanel, 2, true, bus, GFX_NOT_DEFINED,
    st7701_type1_init_operations, sizeof(st7701_type1_init_operations));

static TouchDrvGT911 touch;
static bool touch_ok = false;
static uint8_t brightness = 100;
static uint32_t last_touch = 0;
static lv_disp_draw_buf_t draw_buf;

static void flush_cb(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *c) {
  PerfScope ps(PERF_FLUSH);
  gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)&c->full, a->x2 - a->x1 + 1, a->y2 - a->y1 + 1);
  lv_disp_flush_ready(d);
}

static void touch_cb(lv_indev_drv_t *, lv_indev_data_t *data) {
  static uint32_t last_poll = 0;
  static bool pressed = false;
  static int16_t lx = 0, ly = 0;
  // GT911 has no INT line on V4; poll at most every 10 ms
  if (millis() - last_poll >= 10) {
    last_poll = millis();
    int16_t x[1], y[1];
    pressed = touch_ok && touch.getPoint(x, y, 1) > 0;
    if (pressed) {
      lx = 480 - x[0];
      ly = 480 - y[0];
      last_touch = millis();
    }
  }
  data->point.x = lx;
  data->point.y = ly;
  data->state = pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
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
  if (touch_ok) touch.setMaxTouchPoint(1);

  gfx->begin();
  lv_init();
  size_t n = 480 * 60;
  auto *b1 = (lv_color_t *)heap_caps_malloc(n * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  auto *b2 = (lv_color_t *)heap_caps_malloc(n * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_disp_draw_buf_init(&draw_buf, b1, b2, n);
  static lv_disp_drv_t dd;
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

void hal_send_screenshot() {
  // Framebuffer is stored in panel orientation; display rotation 2 = 180°, host un-rotates.
  uint16_t *fb = gfx->getFramebuffer();
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
