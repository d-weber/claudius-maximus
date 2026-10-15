#include "perf.h"
#include <esp_heap_caps.h>
#include "../ui/ui.h"

static const char *NAMES[PERF__N] = {"link", "tick", "lvgl", "flush", "parse"};
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
  o += ",\"lv_objs_note\":\"cpu_pct = loop work time / wall time (excl. delay)\"}";
  Serial.println(o);
  reset();
}
