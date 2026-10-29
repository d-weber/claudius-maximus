#include "link.h"
#include "perf.h"
#include <esp_heap_caps.h>
#include <driver/temperature_sensor.h>
#include "../ui/ui.h"
#include "hal.h"
#include "../screens/home_face.h"

static const char *NAMES[PERF__N] = {"link", "tick", "lvgl", "flush", "parse"};
volatile uint8_t perf_phase;
void *perf_loop_task;
static uint64_t sum_us[PERF__N];
static uint32_t max_us[PERF__N], cnt[PERF__N];
static uint64_t busy_total;
static uint32_t loops, loop_max_us;
static uint32_t frames, frame_ms_sum, frame_ms_max, px_sum;
static uint32_t window_start_ms;
static uint64_t room_sum;
static uint32_t room_n, room_max, room_min = UINT32_MAX, room_first;
static void (*prev_monitor)(lv_disp_drv_t *, uint32_t, uint32_t);

static void monitor_cb(lv_disp_drv_t *d, uint32_t ms, uint32_t px) {
  frames++;
  frame_ms_sum += ms;
  if (ms > frame_ms_max) frame_ms_max = ms;
  px_sum += px;
  if (prev_monitor) prev_monitor(d, ms, px);
}

static void reset() {
  memset(sum_us, 0, sizeof(sum_us));
  memset(max_us, 0, sizeof(max_us));
  memset(cnt, 0, sizeof(cnt));
  busy_total = 0; loops = 0; loop_max_us = 0;
  frames = frame_ms_sum = frame_ms_max = px_sum = 0;
  room_sum = 0; room_n = room_max = 0; room_min = UINT32_MAX;
  window_start_ms = millis();
}

void perf_init(lv_disp_drv_t *drv) {
  perf_loop_task = xTaskGetCurrentTaskHandle();
  prev_monitor = drv->monitor_cb;
  drv->monitor_cb = monitor_cb;
  reset();
}

void perf_add(PerfSlot s, uint32_t us) {
  sum_us[s] += us;
  cnt[s]++;
  if (us > max_us[s]) max_us[s] = us;
}

void perf_room_draw(uint32_t us) {
  if (!room_first) room_first = us ? us : 1;   // the very first draw since boot (cold caches)
  room_sum += us;
  room_n++;
  if (us > room_max) room_max = us;
  if (us < room_min) room_min = us;
}

void perf_loop(uint32_t busy_us) {
  loops++;
  busy_total += busy_us;
  if (busy_us > loop_max_us) loop_max_us = busy_us;
}

// PROBE: chip temperature (octal PSRAM refresh is temperature compensated). Same sensor range (10..50 C, which
// picks the calibration offset) as the Arduino core's temperatureRead(), so the values stay comparable; NaN when
// the sensor cannot be read, as before.
static float chip_temp_c() {
  static temperature_sensor_handle_t h;
  if (!h) {
    temperature_sensor_config_t c = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
    if (temperature_sensor_install(&c, &h) != ESP_OK) { h = nullptr; return NAN; }
    temperature_sensor_enable(h);
  }
  float t = NAN;
  if (temperature_sensor_get_celsius(h, &t) != ESP_OK) return NAN;
  return t;
}

void perf_report() {
  float win = (millis() - window_start_ms) / 1000.0f;
  if (win <= 0) win = 0.001f;
  static char o[2048];
  size_t n = 0;
  auto P = [&](const char *fmt, auto... args) {   // appends; a truncated field never pushes n past the buffer
    if (n >= sizeof o - 1) return;
    int k = snprintf(o + n, sizeof o - n, fmt, args...);
    if (k > 0) n += (size_t)k < sizeof o - 1 - n ? (size_t)k : sizeof o - 1 - n;
  };
  const char *scr = ui_current();
  P("{\"t\":\"perf\",\"screen\":\"%s\",\"win_s\":%.1f,\"fps\":%.1f,\"frame_ms_avg\":%.1f,\"frame_ms_max\":%lu",
    scr ? scr : "", (double)win, (double)(frames / win), (double)(frames ? (float)frame_ms_sum / frames : 0),
    (unsigned long)frame_ms_max);
  P(",\"px_per_frame\":%lu,\"cpu_pct\":%.1f,\"loop_hz\":%.0f,\"loop_max_ms\":%.1f", (unsigned long)(frames ? px_sum / frames : 0),
    (double)(100.0f * busy_total / (win * 1e6f)), (double)(loops / win), (double)(loop_max_us / 1000.0f));
  for (int i = 0; i < PERF__N; i++)
    P(",\"%s\":{\"n\":%lu,\"avg_ms\":%.2f,\"max_ms\":%.1f}", NAMES[i], (unsigned long)cnt[i],
      (double)(cnt[i] ? sum_us[i] / 1000.0f / cnt[i] : 0), (double)(max_us[i] / 1000.0f));
  P(",\"room_draw_n\":%lu,\"room_draw_us_avg\":%lu,\"room_draw_us_min\":%lu,\"room_draw_us_max\":%lu,\"room_draw_us_first\":%lu",
    (unsigned long)room_n, (unsigned long)(room_n ? room_sum / room_n : 0), (unsigned long)(room_n ? room_min : 0),
    (unsigned long)room_max, (unsigned long)room_first);
  P(",\"heap_int\":%u,\"heap_int_min\":%u,\"heap_int_largest\":%u,\"psram_free\":%u",
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  static uint32_t polls0, scans0, presses0, glitches0;
  uint32_t polls = hal_touch_polls(), scans = hal_touch_scans(), presses = hal_touch_presses(),
           glitches = hal_touch_glitches();
  P(",\"touch_hz\":%.0f,\"touch_scan_hz\":%.0f,\"touch_presses\":%lu,\"touch_glitches\":%lu", (double)((polls - polls0) / win),
    (double)((scans - scans0) / win), (unsigned long)(presses - presses0), (unsigned long)(glitches - glitches0));
  P(",\"touch_ic\":\"%s\",\"touch_rate_ms\":%u,\"touch_errors\":%lu", hal_touch_ic(), (unsigned)hal_touch_rate_ms(),
    (unsigned long)hal_touch_errors());
  static uint32_t frames0, under0;
  uint32_t lframes = hal_lcd_frames(), under = hal_lcd_underruns();
  int32_t smax; uint32_t n10, n30, n100;
  hal_lcd_stall_stats(&smax, &n10, &n30, &n100);
  P(",\"lcd_frames\":%lu,\"lcd_underruns\":%lu,\"lcd_ref_us\":%ld,\"lcd_stall_max\":%ld,\"lcd_n10\":%lu,\"lcd_n30\":%lu,\"lcd_n100\":%lu",
    (unsigned long)(lframes - frames0), (unsigned long)(under - under0), (long)hal_lcd_ref_us(), (long)smax,
    (unsigned long)n10, (unsigned long)n30, (unsigned long)n100);
  int32_t lmax; uint32_t v10, v30;
  hal_lcd_vsync_stats(&lmax, &v10, &v30);
  char al[160];
  hal_lcd_anomaly_log(al, sizeof(al));
  char px[120];
  hal_lcd_probe_extra(px, sizeof(px));
  P(",\"temp_c\":%.1f,\"vs_late_max\":%ld,\"vs_n10\":%lu,\"vs_n30\":%lu,\"lcd_log\":%s%s", (double)chip_temp_c(), (long)lmax,
    (unsigned long)v10, (unsigned long)v30, al, px);
  frames0 = lframes; under0 = under;
  char hs[160];
  home_face_status(hs, sizeof hs);
  P(",\"home\":%s", hs);
#if LV_MEM_CUSTOM == 0
  lv_mem_monitor_t mm;
  lv_mem_monitor(&mm);
  P(",\"lv_mem_used_kb\":%lu,\"lv_mem_max_kb\":%lu,\"lv_mem_frag_pct\":%u", (unsigned long)((mm.total_size - mm.free_size) / 1024),
    (unsigned long)(mm.max_used / 1024), (unsigned)mm.frag_pct);
#endif
  polls0 = polls; scans0 = scans; presses0 = presses; glitches0 = glitches;
  P(",\"up_s\":%lu,\"lv_objs_note\":\"cpu_pct = loop work time / wall time (excl. delay)\"}\n", (unsigned long)(millis() / 1000));
  Link.write(o, n);
  reset();
}
