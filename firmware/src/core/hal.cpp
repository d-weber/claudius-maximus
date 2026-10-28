#pragma GCC diagnostic ignored "-Wvolatile"   // counter++ on the volatile probe counters is intentional (gnu++2b note)
#include "link.h"
#include "hal.h"
#include <lvgl.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_panel_ops.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <rom/ets_sys.h>
#include <soc/gpio_reg.h>
#include "board.h"
#include "perf.h"
#include "st7701_init.h"

// ST7701 is configured over a 3-wire SPI bit-banged on pins of the TCA9554 expander (over I2C, init only);
// pixels go through the ESP32-S3 RGB LCD peripheral, driven with esp_lcd. The GT911 touch controller sits on
// the same I2C bus. IDF i2c_master driver: one bus, one device handle per chip (the clock speed is a property
// of the device handle, see hal_probe_i2c_hz).
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t i2c_exp, i2c_gt;   // TCA9554 @0x20, GT911 @0x5D or 0x14
static volatile uint32_t i2c_hz = 400000, i2c_hz_pending = 0;   // PROBE: a speed change is applied by the touch task
static esp_lcd_panel_handle_t panel;
static uint16_t *fb;  // 480x480 RGB565 in PSRAM, panel orientation (= the UI rotated by 180 degrees if HAL_ROT180)

static i2c_master_dev_handle_t i2c_add(uint8_t addr) {
  i2c_device_config_t dc = {};
  dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dc.device_address = addr;
  dc.scl_speed_hz = i2c_hz;
  i2c_master_dev_handle_t h = nullptr;
  return i2c_master_bus_add_device(i2c_bus, &dc, &h) == ESP_OK ? h : nullptr;
}
static bool i2c_wr(i2c_master_dev_handle_t d, const uint8_t *b, size_t n) {
  return d && i2c_master_transmit(d, b, n, 50) == ESP_OK;
}
static bool i2c_wr_rd(i2c_master_dev_handle_t d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) {
  return d && i2c_master_transmit_receive(d, w, wn, r, rn, 50) == ESP_OK;   // write, repeated start, read
}

// TCA9554: registers 0x01 output, 0x02 polarity, 0x03 direction (1 = input); shadow copies, as Arduino_GFX kept.
static uint8_t exp_out = 0, exp_dir = 0xFF;
static bool exp_reg(uint8_t reg, uint8_t v) { uint8_t b[2] = {reg, v}; return i2c_wr(i2c_exp, b, 2); }
static void exp_mode(uint8_t pin, bool output) {
  if (output) exp_dir &= (uint8_t)~(1u << pin); else exp_dir |= (uint8_t)(1u << pin);
  exp_reg(0x03, exp_dir);
}
static void exp_write(uint8_t pin, bool hi) {
  if (hi) exp_out |= (uint8_t)(1u << pin); else exp_out &= (uint8_t)~(1u << pin);
  exp_reg(0x01, exp_out);
}

// 3-wire SPI to the ST7701 over the expander pins: 9-bit words, the D/C bit first then the byte MSB first,
// sampled on the rising clock edge; SDA is only rewritten when the bit changes (each pin write is one I2C
// transaction, about 0.1 ms at 400 kHz; the whole init table takes ~0.6 s, as it did with Arduino_GFX).
static void spi9(bool dc, uint8_t v) {
  bool last = dc;
  exp_write(BOARD_EXIO_LCD_SDA, dc);
  exp_write(BOARD_EXIO_LCD_SCL, 0);
  exp_write(BOARD_EXIO_LCD_SCL, 1);
  for (uint8_t bit = 0x80; bit; bit >>= 1) {
    bool b = (v & bit) != 0;
    if (b != last) { last = b; exp_write(BOARD_EXIO_LCD_SDA, b); }
    exp_write(BOARD_EXIO_LCD_SCL, 0);
    exp_write(BOARD_EXIO_LCD_SCL, 1);
  }
}
static void st7701_run_init(const uint8_t *ops, size_t len) {   // runs a st7701_init.h byte code sequence
  for (size_t i = 0; i < len; i++) {
    switch (ops[i]) {
      case BEGIN_WRITE: exp_write(BOARD_EXIO_LCD_CS, 0); break;
      case END_WRITE: exp_write(BOARD_EXIO_LCD_CS, 1); break;
      case WRITE_COMMAND_8: spi9(0, ops[++i]); break;
      case WRITE_C8_D8: spi9(0, ops[i + 1]); spi9(1, ops[i + 2]); i += 2; break;
      case WRITE_C8_D16: spi9(0, ops[i + 1]); spi9(1, ops[i + 2]); spi9(1, ops[i + 3]); i += 3; break;
      case WRITE_BYTES: { uint8_t n = ops[++i]; while (n--) spi9(1, ops[++i]); break; }
      case DELAY: delay(ops[++i]); break;
    }
  }
}

// Scan-out: the GDMA reads the PSRAM framebuffer directly (no bounce buffer), in refresh-on-demand mode: the
// DMA stops after each frame and the vsync callback starts the next one from an idle channel. In the driver's
// default stream mode the descriptor ring runs continuously and CONFIG_LCD_RGB_RESTART_IN_VSYNC (on, as in the
// Arduino core: sdkconfig.defaults) resets and restarts the running channel at every vsync; when that reset lands on a channel with
// a PSRAM burst in flight the channel stays stuck until the next vsync and the frame is lost (one frame jumped
// sideways). How often depends on CPU traffic to PSRAM, the idle timing and even the code layout at 32-byte
// granularity, which is why earlier "fixes" came and went. Refresh on demand measured 0 lost frames under every
// synthetic load that lost 45-140 frames/min in stream mode (docs/SCANOUT.md, 2026-10-28). -DHAL_STREAM_MODE=1
// selects stream mode again for comparisons.
// Bounce buffers (an ISR memcpy of the whole 460 KB framebuffer into SRAM every refresh) also measured 0 but cost
// 12 % of the LVGL core, a third of the frame rate on busy pages and 77 KB of RAM; -DHAL_BOUNCE_PX=19200 (40 lines)
// still selects them for experiments. The flush blit is chunked (HAL_BLIT_ROWS / HAL_BLIT_PAUSE_US) and the
// vsync/frame-complete callbacks count lost frames (perf: lcd_underruns) so all of this can be measured.
// Tried and rejected: GDMA memcpy into the framebuffer (no cache traffic, but 9 ms per band vs 0.9 ms CPU).
#ifndef HAL_BOUNCE_PX
#define HAL_BOUNCE_PX 0
#endif
#ifndef HAL_PCLK_HZ
#define HAL_PCLK_HZ (12 * 1000 * 1000)
#endif
#ifndef HAL_BLIT_ROWS
#define HAL_BLIT_ROWS 4        // rows blitted + written back between pauses
#endif
#ifndef HAL_BLIT_PAUSE_US
#define HAL_BLIT_PAUSE_US 0    // pause after each chunk (0 = none)
#endif

// Scan-out probe (perf: lcd_* / vs_*). The DMA normally finishes pushing a frame a fixed time before the next
// vsync (a steady ~465 us at 12 MHz, jitter < 10 us). A later finish = the scan-out stalled by that much, and
// the panel content is displaced by as many pixel clocks for the rest of the frame (a sideways jump; 10 us =
// 120 px). The driver (CONFIG_LCD_RGB_RESTART_IN_VSYNC) restarts the DMA inside the vsync ISR, so a late
// vsync ISR shifts the frame the same way: the probe also times the vsync ISR against the fixed frame period.
// This is how the 2026-10-16 "now and then a sideways jump" was traced to Arduino's HWCDC ISR (see link.h);
// after the fix both histograms read zero under the demo stream. 999 = no frame-complete before the vsync.
static volatile uint32_t lcd_frames, lcd_underruns;   // underruns: no frame-complete at all before the vsync
static volatile int64_t lcd_fc_t;
static volatile bool lcd_fc_seen;
static volatile int32_t lcd_ref_x16;                  // reference gap * 16 (0 = not calibrated)
static volatile int32_t lcd_stall_max;                // since last perf read
static volatile uint32_t lcd_n10, lcd_n30, lcd_n100;  // frames stalled > 10 / 30 / 100 us since last perf read
// vsync ISR latency: the frame period is fixed by the pixel clock, so a vsync timestamp later than expected
// means core 1 had interrupts disabled (the driver restarts the DMA in this ISR: a late restart shifts the frame)
static volatile int64_t lcd_vs_prev;
static volatile int32_t lcd_vs_ref_x16, lcd_vs_late_max;
static volatile uint32_t lcd_vs_n10, lcd_vs_n30;
static volatile int32_t lcd_log[8][4];     // last anomalies: (stall, vsync lateness) us, phase code, ms since previous
static volatile int64_t lcd_log_t;

static volatile uint32_t lcd_log_n;
// PROBE: which core each LCD callback runs on, and when a frame-complete that was missing at vsync finally
// arrives (us after that vsync; a value well under the frame period = the previous frame's EOF came late)
static volatile int lcd_fc_core = -1, lcd_vs_core = -1;
static volatile bool touch_poll_on = true;   // PROBE: dev_touch event toggles the I2C poll without changing the binary
void hal_touch_set_poll(bool on) { touch_poll_on = on; }
// PROBE: synthetic load toggles (dev_probe event): 1 flash stream, 2 psram stream, 4 skip msync, 8 skip blit, 16 busy idle
static volatile int probe_mode = 0;
static volatile int probe_poll_ms = 10;   // PROBE: touch poll period
void hal_probe_set(int m) { probe_mode = m; }
void hal_probe_poll_ms(int ms) { probe_poll_ms = ms < 1 ? 1 : ms; }
void hal_probe_i2c_hz(int hz) { if (hz > 0) i2c_hz_pending = (uint32_t)hz; }   // applied by the touch task between polls
int hal_probe_mode() { return probe_mode; }
static const uint8_t probe_table[65536] = {1, 2, 3};   // 64 KB of rodata in flash to stream through the cache
static volatile uint32_t probe_sink;
extern "C" void Cache_WriteBack_All(void);
// 1024: execute 32 KB of NOPs from flash on core 0 every 10 ms (instruction-fetch stream, twice the ICache)
// 2048: 2000 APB register reads (GPIO input register) on core 0 every 10 ms (what an I2C driver does to its FIFO)
extern "C" void __attribute__((noinline)) probe_icache_fn() { asm volatile(".rept 32768\n nop\n .endr"); }
// 32: write back the whole data cache every 1 ms (keeps dirty PSRAM lines near zero)
// 64: PSRAM read-only stream   128: PSRAM write-only stream
static void probe_task(void *) {
  uint8_t *pa = (uint8_t *)heap_caps_malloc(65536, MALLOC_CAP_SPIRAM), *pb = (uint8_t *)heap_caps_malloc(65536, MALLOC_CAP_SPIRAM);
  for (;;) {
    int m = probe_mode;
    if (m & 1) { uint32_t acc = 0; for (size_t i = 0; i < sizeof probe_table; i += 32) acc += probe_table[i]; probe_sink = acc; }
    if (m & 2) { memcpy(pa, pb, 65536); probe_sink = pa[12345]; }
    if (m & 64) { uint32_t acc = 0; for (size_t i = 0; i < 65536; i += 32) acc += pb[i]; probe_sink = acc; }
    if (m & 128) { for (size_t i = 0; i < 65536; i += 32) pa[i] = (uint8_t)i; probe_sink = pa[32]; }
    // 256: write stream followed by an immediate, bounded writeback of what was written (the blit's discipline)
    if (m & 256) { for (size_t i = 0; i < 65536; i += 32) pa[i] = (uint8_t)i; esp_cache_msync(pa, 65536, ESP_CACHE_MSYNC_FLAG_DIR_C2M); probe_sink = pa[32]; }
    if (m & 32) Cache_WriteBack_All();
    if (m & 1024) probe_icache_fn();
    if (m & 2048) { uint32_t acc = 0; for (int i = 0; i < 2000; i++) acc += REG_READ(GPIO_IN_REG); probe_sink = acc; }
    if (!(m & (3 | 64 | 128 | 256))) vTaskDelay(pdMS_TO_TICKS((m & 32) ? 1 : 10));
    else vTaskDelay(1);
  }
}
#ifndef LAYOUT_PAD
#define LAYOUT_PAD 0   // -DLAYOUT_PAD=<n> inserts n NOPs in flash to shift the code layout (docs/SCANOUT.md)
#endif
#define LP_STR2(x) #x
#define LP_STR(x) LP_STR2(x)
extern "C" void __attribute__((noinline)) layout_pad_fn() { asm volatile(".rept " LP_STR(LAYOUT_PAD) "\n nop\n .endr"); }
static volatile bool lcd_fc_late_pending;
static volatile int32_t lcd_late_eof[8];
static volatile uint32_t lcd_late_eof_n;

static bool IRAM_ATTR lcd_on_frame_complete(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *) {
  lcd_fc_t = esp_timer_get_time();
  lcd_fc_seen = true;
  lcd_fc_core = xPortGetCoreID();
  if (lcd_fc_late_pending) {
    lcd_fc_late_pending = false;
    lcd_late_eof[lcd_late_eof_n & 7] = (int32_t)(lcd_fc_t - lcd_vs_prev);
    lcd_late_eof_n++;
  }
  return false;
}

static bool IRAM_ATTR lcd_on_vsync(esp_lcd_panel_handle_t p, const esp_lcd_rgb_panel_event_data_t *, void *) {
#ifndef HAL_STREAM_MODE
  esp_lcd_rgb_panel_refresh(p);   // start the next frame from an idle channel (see the scan-out note)
#endif
  lcd_frames++;
  int64_t now = esp_timer_get_time();
  int32_t late = 0;
  if (lcd_vs_prev) {
    int32_t iv = (int32_t)(now - lcd_vs_prev);
    if (lcd_vs_ref_x16 == 0) lcd_vs_ref_x16 = iv * 16;
    int32_t ref = lcd_vs_ref_x16 / 16;
    if (iv > ref - 10 && iv < ref + 10) lcd_vs_ref_x16 += iv - ref;
    late = iv - ref;   // > 0: this ISR ran late (or the previous one early, which cannot happen)
    if (late > lcd_vs_late_max) lcd_vs_late_max = late;
    if (late > 10) lcd_vs_n10++;
    if (late > 30) lcd_vs_n30++;
  }
  lcd_vs_prev = now;
  lcd_vs_core = xPortGetCoreID();
  int32_t stall = 0;
  if (!lcd_fc_seen) {
    lcd_underruns++;
    lcd_n100++;
    stall = 999;   // no frame-complete at all: the DMA did not finish this frame before the restart
    lcd_fc_late_pending = true;
  } else {
    lcd_fc_seen = false;
    int32_t d = (int32_t)(now - lcd_fc_t);
    if (lcd_ref_x16 == 0) { lcd_ref_x16 = d * 16; return false; }
    int32_t ref = lcd_ref_x16 / 16;
    if (d > ref - 10 && d < ref + 10) lcd_ref_x16 += d - ref;   // track slow drift, ignore stalls and outliers
    stall = ref - d;
    if (stall > lcd_stall_max) lcd_stall_max = stall;
    if (stall > 10) lcd_n10++;
    if (stall > 30) lcd_n30++;
    if (stall > 100) lcd_n100++;
  }
  if (stall > 10 || late > 10) {
    lcd_log[lcd_log_n & 7][0] = stall;
    lcd_log[lcd_log_n & 7][1] = late;
    // phase of the main loop (0 idle, 1 link, 2 tick, 3 lvgl, 4 flush, 5 parse) + 10 if another task was running
    lcd_log[lcd_log_n & 7][2] = perf_phase + (xTaskGetCurrentTaskHandle() == perf_loop_task ? 0 : 10);
    lcd_log[lcd_log_n & 7][3] = (int32_t)((now - lcd_log_t) / 1000);
    lcd_log_t = now;
    lcd_log_n++;
  }
  return false;
}

static void panel_init() {
  // the expander was set up and the panel reset pulsed in board_init
  static const uint8_t swreset[] = {BEGIN_WRITE, WRITE_COMMAND_8, 0x01, END_WRITE};   // software reset
  st7701_run_init(swreset, sizeof swreset);
  delay(120);
  st7701_run_init(st7701_type1_init_operations, sizeof(st7701_type1_init_operations));

  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = HAL_PCLK_HZ;
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
  cfg.dma_burst_size = 64;   // IDF 5.5 name of psram_trans_align (same union member): 64-byte PSRAM bursts
  cfg.hsync_gpio_num = BOARD_LCD_HSYNC;
  cfg.vsync_gpio_num = BOARD_LCD_VSYNC;
  cfg.de_gpio_num = BOARD_LCD_DE;
  cfg.pclk_gpio_num = BOARD_LCD_PCLK;
  cfg.disp_gpio_num = GPIO_NUM_NC;
  const int data_pins[16] = BOARD_LCD_DATA;  // B0-4 G0-5 R0-4
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = data_pins[i];
  cfg.flags.fb_in_psram = 1;
#ifndef HAL_STREAM_MODE
  cfg.flags.refresh_on_demand = 1;   // see the scan-out note above
#endif
  ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_vsync = lcd_on_vsync;
  cbs.on_frame_buf_complete = lcd_on_frame_complete;
  esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, nullptr);
#ifndef HAL_STREAM_MODE
  esp_lcd_rgb_panel_refresh(panel);   // the driver starts the first frame by itself only in stream mode
#endif
  void *p = nullptr;
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &p));
  fb = (uint16_t *)p;
}

// Copy an LVGL band into the framebuffer (rotated by 180 degrees if HAL_ROT180), HAL_BLIT_ROWS rows at a time:
// each chunk is written back from the data cache right away (small bursts) and, if configured, followed by a
// pause that gives the LCD DMA exclusive PSRAM access to refill its FIFO. With bounce buffers the copy to the
// panel is the driver's own CPU memcpy through the shared cache, so the write-back is skipped.
static void fb_blit(const lv_area_t *a, const uint16_t *src) {
  if (probe_mode & 8) return;   // PROBE: skip the blit entirely
  const int w = a->x2 - a->x1 + 1;
  for (int y0 = a->y1; y0 <= a->y2; y0 += HAL_BLIT_ROWS) {
    int y1 = y0 + HAL_BLIT_ROWS - 1;
    if (y1 > a->y2) y1 = a->y2;
    for (int y = y0; y <= y1; y++) {
#if HAL_ROT180
      uint16_t *d = fb + (479 - y) * 480 + (479 - a->x1);
      for (int i = 0; i < w; i++) *d-- = *src++;
#else
      memcpy(fb + y * 480 + a->x1, src, (size_t)w * 2);
      src += w;
#endif
    }
#if HAL_BOUNCE_PX == 0
    uint8_t *start = (uint8_t *)(fb + (HAL_ROT180 ? 479 - y1 : y0) * 480);
    size_t len = (size_t)(y1 - y0 + 1) * 480 * 2;
    if (!(probe_mode & 4)) esp_cache_msync(start, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);   // PROBE: 4 skips
#endif
#if HAL_BLIT_PAUSE_US > 0
    esp_rom_delay_us(HAL_BLIT_PAUSE_US);
#endif
  }
}

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
    fb_blit(a, (const uint16_t *)&flush_px->full);
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

// The GT911 INT line does not reach the ESP32 (it ends on the expander), so it
// is polled. The I2C transactions (~0.3 ms every 10 ms) run in a task on core 0; touch_cb (LVGL, core 1)
// only reads the latest sample.
//
// The controller rescans at its own rate (5 + N ms, N = register 0x8056) and sets bit 7 of the status register
// 0x814E when a new scan is ready; the host clears the register after reading it. SensorLib's getPoint() ignores
// that bit and clears the register on every call, so a poll that lands before the next scan reads "0 touches":
// a single-poll release in the middle of a swipe, which LVGL turns into release + new press = a click on
// whatever is under the finger (and the tileview never scrolls). Here: a sample is only taken when the scan is
// fresh, otherwise the previous state is held. The GT911 flags only scans that carry data: every scan while a
// finger is down, then exactly one "0 points" scan when it lifts, nothing while idle (measured: touch_scan_hz
// is 0 with no finger). So a press is confirmed by two fresh scans (a one-scan phantom is dropped), a release
// is taken from the first empty scan, and a press with no fresh scan for 4 periods is released anyway.
static volatile uint32_t touch_sample;  // bit31 = pressed, bits 16..25 = x, bits 0..9 = y
static volatile uint32_t touch_polls, touch_scans, touch_presses, touch_glitches, touch_errors;
static uint8_t gt_addr;
static char gt_id[5] = "?";   // product id register 0x8140 ("911")
static uint8_t gt_rate = 0;    // refresh-rate register 0x8056: scan period = 5 + N ms

static bool gt_read(uint16_t reg, uint8_t *buf, uint8_t n) {
  uint8_t a[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
  return i2c_wr_rd(i2c_gt, a, 2, buf, n);
}

static bool gt_write8(uint16_t reg, uint8_t v) {
  uint8_t b[3] = {(uint8_t)(reg >> 8), (uint8_t)reg, v};
  return i2c_wr(i2c_gt, b, 3);
}

static void touch_task(void *) {
  TickType_t wake = xTaskGetTickCount();
  bool pressed = false;
  uint8_t pr_run = 0;                // consecutive fresh scans that saw a finger
  uint32_t fresh_ms = 0;             // millis() of the last fresh scan
  const uint32_t hold_ms = 4u * (5u + gt_rate);
  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(probe_poll_ms));
    if (i2c_hz_pending) {   // PROBE: re-register both devices at the new bus speed (the speed is per device)
      i2c_hz = i2c_hz_pending;
      i2c_hz_pending = 0;
      if (i2c_gt) { i2c_master_bus_rm_device(i2c_gt); i2c_gt = i2c_add(gt_addr); }
      if (i2c_exp) { i2c_master_bus_rm_device(i2c_exp); i2c_exp = i2c_add(BOARD_EXPANDER_ADDR); }
    }
    if (!touch_poll_on) continue;   // PROBE
    touch_polls++;
    uint8_t st = 0;
    bool ok = gt_read(0x814E, &st, 1);
    if (!ok) touch_errors++;
    bool fresh = ok && (st & 0x80);
    if (!fresh) {
      // no new scan since we cleared the register (or a bus error): hold the current state, but never
      // stay pressed forever if the controller goes quiet
      if (pressed && millis() - fresh_ms > hold_ms) {
        pressed = false;
        pr_run = 0;
        touch_sample &= ~0x80000000u;
      }
      continue;
    }
    fresh_ms = millis();
    touch_scans++;
    bool finger = false;
    int16_t lx = 0, ly = 0;
    if (st & 0x0F) {
      uint8_t b[4];
      if (gt_read(0x8150, b, 4)) {   // point 1: x lo, x hi, y lo, y hi
        int x = b[0] | (b[1] << 8), y = b[2] | (b[3] << 8);
        if (x < 480 && y < 480) {
          finger = true;
#if HAL_ROT180
          lx = (int16_t)(479 - x);
          ly = (int16_t)(479 - y);
#else
          lx = (int16_t)x;
          ly = (int16_t)y;
#endif
        }
      }
    }
    gt_write8(0x814E, 0);
    if (finger) {
      if (pr_run < 255) pr_run++;
      if (!pressed && pr_run < 2) continue;      // first scan: wait for the next one to confirm
      pressed = true;
      touch_sample = 0x80000000u | ((uint32_t)lx << 16) | (uint32_t)ly;
      last_touch = millis();
      touch_presses++;
    } else {
      if (pr_run == 1) touch_glitches++;         // a one-scan press: ignored
      pr_run = 0;
      if (pressed) touch_sample &= ~0x80000000u;  // released: keep the last position
      pressed = false;
    }
  }
}

static void touch_cb(lv_indev_drv_t *, lv_indev_data_t *data) {
  uint32_t s = touch_sample;
  data->point.x = (s >> 16) & 0x3FF;
  data->point.y = s & 0x3FF;
  data->state = (s & 0x80000000u) ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

// Bring up the I2C bus, the IO expander and whatever the board needs before the panel and touch init.
static void board_init() {
  i2c_master_bus_config_t bc = {};
  bc.i2c_port = I2C_NUM_0;
  bc.sda_io_num = (gpio_num_t)BOARD_I2C_SDA;
  bc.scl_io_num = (gpio_num_t)BOARD_I2C_SCL;
  bc.clk_source = I2C_CLK_SRC_DEFAULT;
  bc.glitch_ignore_cnt = 7;
  bc.flags.enable_internal_pullup = true;
  ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &i2c_bus));
  // TCA9554, set up as Arduino_GFX did: no polarity inversion, all pins input, outputs low; the panel reset
  // (EXIO7) pulsed low for 10 ms then 100 ms to settle; CS/SCL/SDA driven high
  if (i2c_master_probe(i2c_bus, BOARD_EXPANDER_ADDR, 50) == ESP_OK) i2c_exp = i2c_add(BOARD_EXPANDER_ADDR);
  exp_reg(0x02, 0x00);
  exp_reg(0x03, exp_dir = 0xFF);
  exp_reg(0x01, exp_out = 0x00);
  exp_mode(BOARD_EXIO_LCD_RST, true);
  exp_write(BOARD_EXIO_LCD_RST, 0);
  delay(10);
  exp_write(BOARD_EXIO_LCD_RST, 1);
  delay(100);
  exp_mode(BOARD_EXIO_LCD_CS, true);
  exp_write(BOARD_EXIO_LCD_CS, 1);
  exp_mode(BOARD_EXIO_LCD_SCL, true);
  exp_write(BOARD_EXIO_LCD_SCL, 1);
  exp_mode(BOARD_EXIO_LCD_SDA, true);
  exp_write(BOARD_EXIO_LCD_SDA, 1);
  // GT911 reset with INT held low: the controller boots at 0x5D (Waveshare BSP sequence); INT is then
  // released as an input (it is not routed to the ESP32: the touch task polls)
  exp_mode(BOARD_EXIO_TP_INT, true);
  exp_write(BOARD_EXIO_TP_INT, 0);
  exp_mode(BOARD_EXIO_TP_RST, true);
  exp_write(BOARD_EXIO_TP_RST, 0);
  delay(20);
  exp_write(BOARD_EXIO_TP_RST, 1);
  delay(120);
  exp_mode(BOARD_EXIO_TP_INT, false);
  // backlight: AP3032 boost driver, PWM fed into its feedback node (duty 0 = full brightness, max = off),
  // same 5 kHz / 10 bit as the Waveshare BSP (LEDC timer 0 / channel 0). Left floating at boot the panel is at
  // full brightness.
  ledc_timer_config_t tc = {};
  tc.speed_mode = LEDC_LOW_SPEED_MODE;
  tc.duty_resolution = LEDC_TIMER_10_BIT;
  tc.timer_num = LEDC_TIMER_0;
  tc.freq_hz = 5000;
  tc.clk_cfg = LEDC_AUTO_CLK;
  ledc_timer_config(&tc);
  ledc_channel_config_t cc = {};
  cc.gpio_num = BOARD_BL_PIN;
  cc.speed_mode = LEDC_LOW_SPEED_MODE;
  cc.channel = LEDC_CHANNEL_0;
  cc.timer_sel = LEDC_TIMER_0;
  cc.duty = 0;
  cc.hpoint = 0;
  ledc_channel_config(&cc);
}

void hal_init() {
  board_init();
  // GT911 at 0x5D after the reset sequence above (0x14 if INT was high at reset). Firmware <= 0.4.1 also rewrote
  // the controller's configuration block at every boot to report one point (SensorLib's setMaxTouchPoint);
  // only point 1 is ever read, so the controller's own configuration is left alone now.
  for (uint8_t addr : {0x5D, 0x14}) {   // accepted only if the product id register reads "911" (as SensorLib did)
    if (i2c_master_probe(i2c_bus, addr, 50) != ESP_OK || (i2c_gt = i2c_add(addr)) == nullptr) continue;
    uint8_t b[4];
    if (gt_read(0x8140, b, 4) && b[0] == '9' && b[1] == '1' && b[2] == '1') {
      memcpy(gt_id, b, 4); gt_id[4] = 0; for (char &c : gt_id) if (c && (c < 32 || c > 126)) c = '.';
      touch_ok = true;
      gt_addr = addr;
      break;
    }
    i2c_master_bus_rm_device(i2c_gt);
    i2c_gt = nullptr;
  }
  if (touch_ok) {
    gt_read(0x8056, &gt_rate, 1);
    xTaskCreatePinnedToCore(touch_task, "touch", 3072, nullptr, 5, nullptr, 0);
  }
  xTaskCreatePinnedToCore(probe_task, "probe", 3072, nullptr, 4, nullptr, 0);   // PROBE

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
}

void hal_set_brightness(uint8_t pct) {
  if (pct > 100) pct = 100;
  brightness = pct;
  uint32_t duty = (uint32_t)(100 - pct) * 1023 / 100;   // inverted, see board_init
  if (duty == 1023) duty = 1024;                         // all ones = fully on (backlight off), as Arduino's ledcWrite did
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

uint8_t hal_get_brightness() { return brightness; }
uint32_t hal_last_touch_ms() { return last_touch; }
uint32_t hal_touch_polls() { return touch_polls; }
uint32_t hal_touch_presses() { return touch_presses; }
uint32_t hal_touch_glitches() { return touch_glitches; }
uint32_t hal_touch_scans() { return touch_scans; }
uint32_t hal_lcd_frames() { return lcd_frames; }
uint32_t hal_lcd_underruns() { return lcd_underruns; }
int32_t hal_lcd_ref_us() { return lcd_ref_x16 / 16; }
void hal_lcd_stall_stats(int32_t *smax, uint32_t *n10, uint32_t *n30, uint32_t *n100) {  // and reset
  *smax = lcd_stall_max; *n10 = lcd_n10; *n30 = lcd_n30; *n100 = lcd_n100;
  lcd_stall_max = 0; lcd_n10 = lcd_n30 = lcd_n100 = 0;
}
void hal_lcd_vsync_stats(int32_t *lmax, uint32_t *n10, uint32_t *n30) {  // and reset
  *lmax = lcd_vs_late_max; *n10 = lcd_vs_n10; *n30 = lcd_vs_n30;
  lcd_vs_late_max = 0; lcd_vs_n10 = lcd_vs_n30 = 0;
}
void hal_probe_pclk(int hz) { if (panel && hz > 0) esp_lcd_rgb_panel_set_pclk(panel, (uint32_t)hz); }   // PROBE
void hal_lcd_probe_extra(char *buf, size_t n) {   // ",\"fc_core\":1,\"vs_core\":1,\"late_eof\":[us,...]"
  if (n == 1) layout_pad_fn();   // keep the pad linked (never true)
  size_t o = snprintf(buf, n, ",\"fc_core\":%d,\"vs_core\":%d,\"poll\":%d,\"pmode\":%d,\"late_eof\":[", lcd_fc_core, lcd_vs_core, (int)touch_poll_on, probe_mode);
  uint32_t cnt = lcd_late_eof_n, from = cnt > 8 ? cnt - 8 : 0;
  for (uint32_t i = from; i < cnt && o < n - 16; i++)
    o += snprintf(buf + o, n - o, "%s%ld", i > from ? "," : "", (long)lcd_late_eof[i & 7]);
  snprintf(buf + o, n - o, "]");
}
void hal_lcd_anomaly_log(char *buf, size_t n) {   // JSON array "stall/late" of the last anomalies, oldest first
  size_t o = snprintf(buf, n, "[");
  uint32_t cnt = lcd_log_n, from = cnt > 8 ? cnt - 8 : 0;
  for (uint32_t i = from; i < cnt && o < n - 20; i++)
    o += snprintf(buf + o, n - o, "%s\"%ld/%ld/p%ld/+%ld\"", i > from ? "," : "", (long)lcd_log[i & 7][0],
                  (long)lcd_log[i & 7][1], (long)lcd_log[i & 7][2], (long)lcd_log[i & 7][3]);
  snprintf(buf + o, n - o, "]");
}
uint32_t hal_touch_errors() { return touch_errors; }
const char *hal_touch_ic() { return gt_id; }
uint8_t hal_touch_rate_ms() { return (uint8_t)(5 + gt_rate); }

void hal_send_screenshot() {
  // Framebuffer is stored in panel orientation; "rot" tells the host how much to un-rotate it.
  flush_wait();
  const size_t len = 480 * 480 * 2;
  Link.printf("{\"t\":\"shot\",\"w\":480,\"h\":480,\"fmt\":\"rgb565le\",\"rot\":%d,\"len\":%u}\n", HAL_ROT180 ? 180 : 0,
              (unsigned)len);
  const uint8_t *p = (const uint8_t *)fb;
  for (size_t off = 0; off < len; off += 4096) {
    size_t chunk = min((size_t)4096, len - off);
    size_t sent = 0;
    while (sent < chunk) sent += Link.write(p + off + sent, chunk - sent);
  }
  Link.flush();
}
