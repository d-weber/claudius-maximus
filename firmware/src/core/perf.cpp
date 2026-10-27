#include "link.h"
#include "perf.h"
#include <esp_heap_caps.h>
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

void perf_loop(uint32_t busy_us) {
  loops++;
  busy_total += busy_us;
  if (busy_us > loop_max_us) loop_max_us = busy_us;
}

void perf_report() {
  float win = (millis() - window_start_ms) / 1000.0f;
  if (win <= 0) win = 0.001f;
  String o;
  o.reserve(512);
  o += "{\"t\":\"perf\",\"screen\":\"";
  o += ui_current();
  o += "\",\"win_s\":" + String(win, 1);
  o += ",\"fps\":" + String(frames / win, 1);
  o += ",\"frame_ms_avg\":" + String(frames ? (float)frame_ms_sum / frames : 0, 1);
  o += ",\"frame_ms_max\":" + String(frame_ms_max);
  o += ",\"px_per_frame\":" + String(frames ? px_sum / frames : 0);
  o += ",\"cpu_pct\":" + String(100.0f * busy_total / (win * 1e6f), 1);
  o += ",\"loop_hz\":" + String(loops / win, 0);
  o += ",\"loop_max_ms\":" + String(loop_max_us / 1000.0f, 1);
  for (int i = 0; i < PERF__N; i++) {
    o += ",\""; o += NAMES[i]; o += "\":{\"n\":" + String(cnt[i]);
    o += ",\"avg_ms\":" + String(cnt[i] ? sum_us[i] / 1000.0f / cnt[i] : 0, 2);
    o += ",\"max_ms\":" + String(max_us[i] / 1000.0f, 1) + "}";
  }
  o += ",\"heap_int\":" + String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  o += ",\"heap_int_min\":" + String(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  o += ",\"heap_int_largest\":" + String(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  o += ",\"psram_free\":" + String(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  static uint32_t polls0, scans0, presses0, glitches0;
  uint32_t polls = hal_touch_polls(), scans = hal_touch_scans(), presses = hal_touch_presses(),
           glitches = hal_touch_glitches();
  o += ",\"touch_hz\":" + String((polls - polls0) / win, 0) + ",\"touch_scan_hz\":" + String((scans - scans0) / win, 0)
       + ",\"touch_presses\":" + String(presses - presses0) + ",\"touch_glitches\":" + String(glitches - glitches0);
  o += ",\"touch_ic\":\"" + String(hal_touch_ic()) + "\",\"touch_rate_ms\":" + String(hal_touch_rate_ms())
       + ",\"touch_errors\":" + String(hal_touch_errors());
  static uint32_t frames0, under0;
  uint32_t frames = hal_lcd_frames(), under = hal_lcd_underruns();
  int32_t smax; uint32_t n10, n30, n100;
  hal_lcd_stall_stats(&smax, &n10, &n30, &n100);
  o += ",\"lcd_frames\":" + String(frames - frames0) + ",\"lcd_underruns\":" + String(under - under0)
       + ",\"lcd_ref_us\":" + String(hal_lcd_ref_us()) + ",\"lcd_stall_max\":" + String(smax)
       + ",\"lcd_n10\":" + String(n10) + ",\"lcd_n30\":" + String(n30) + ",\"lcd_n100\":" + String(n100);
  int32_t lmax; uint32_t v10, v30;
  hal_lcd_vsync_stats(&lmax, &v10, &v30);
  char al[160];
  hal_lcd_anomaly_log(al, sizeof(al));
  o += ",\"vs_late_max\":" + String(lmax) + ",\"vs_n10\":" + String(v10) + ",\"vs_n30\":" + String(v30)
       + ",\"lcd_log\":" + String(al);
  frames0 = frames; under0 = under;
  char hs[160];
  home_face_status(hs, sizeof hs);
  o += ",\"home\":" + String(hs);
#if LV_MEM_CUSTOM == 0
  lv_mem_monitor_t mm;
  lv_mem_monitor(&mm);
  o += ",\"lv_mem_used_kb\":" + String((mm.total_size - mm.free_size) / 1024)
       + ",\"lv_mem_max_kb\":" + String(mm.max_used / 1024) + ",\"lv_mem_frag_pct\":" + String(mm.frag_pct);
#endif
  polls0 = polls; scans0 = scans; presses0 = presses; glitches0 = glitches;
  o += ",\"up_s\":" + String(millis() / 1000);
  o += ",\"lv_objs_note\":\"cpu_pct = loop work time / wall time (excl. delay)\"}";
  Link.println(o);
  reset();
}
