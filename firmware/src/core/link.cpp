#include "link.h"
#include "settings.h"
#include "../screens/home_face.h"
#include <lvgl.h>
#include <Arduino.h>
#include "model.h"
#include "hal.h"
#include "board.h"
#include "perf.h"
#include "../ui/ui.h"

#include <driver/usb_serial_jtag.h>
#include <stdarg.h>

LinkSerial Link;
static volatile bool usj_ready = false;

static void usj_install_task(void *arg) {
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  cfg.rx_buffer_size = 32 * 1024;   // a full state line can be ~10 KB; the host may burst several
  cfg.tx_buffer_size = 8 * 1024;
  if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) usj_ready = true;
  xTaskNotifyGive((TaskHandle_t)arg);
  vTaskDelete(nullptr);
}

void link_serial_init() {
  // core 0: the driver allocates its interrupt on the calling core
  xTaskCreatePinnedToCore(usj_install_task, "usj_init", 3072, xTaskGetCurrentTaskHandle(), 5, nullptr, 0);
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
}

size_t LinkSerial::write(const void *buf, size_t n, uint32_t timeout_ms) {
  if (!usj_ready || n == 0) return 0;
  int w = usb_serial_jtag_write_bytes(buf, n, pdMS_TO_TICKS(timeout_ms));
  if (w < 0) w = 0;
  if ((size_t)w < n && ((const char *)buf)[n - 1] == '\n') {
    // the host is not draining: the line is torn. Terminate it so the receiver drops one bad line and
    // stays in sync (the host ignores lines that are not valid JSON).
    usb_serial_jtag_write_bytes("\n", 1, pdMS_TO_TICKS(timeout_ms));
  }
  return (size_t)w;
}

size_t LinkSerial::printf(const char *fmt, ...) {
  char stackbuf[256];
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
  va_end(ap);
  if (n < 0) { va_end(ap2); return 0; }
  if ((size_t)n < sizeof(stackbuf)) { va_end(ap2); return write(stackbuf, (size_t)n); }
  char *big = (char *)malloc((size_t)n + 1);
  if (!big) { va_end(ap2); return 0; }
  vsnprintf(big, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  size_t w = write(big, (size_t)n);
  free(big);
  return w;
}

size_t LinkSerial::read(uint8_t *buf, size_t n) {
  if (!usj_ready || n == 0) return 0;
  int r = usb_serial_jtag_read_bytes(buf, n, 0);
  return r < 0 ? 0 : (size_t)r;
}

void LinkSerial::flush(uint32_t timeout_ms) {
  if (usj_ready) usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(timeout_ms));
}

#define FW_VERSION "0.2.0"
static char *buf;
static size_t len = 0;
static bool overflow = false;            // current line exceeded CAP: drop it up to the next '\n'
static const size_t CAP = 32 * 1024;

// Dev-only: {"t":"cmd","img":<bytes>} followed by that many raw RGB565 bytes (480x480, LVGL native order)
// shows the picture full screen on the top layer until {"t":"cmd","img":0} (tools/dev.py show). Lets a design
// mock-up be looked at on the real panel without a firmware build.
static uint8_t *img_buf;                 // PSRAM, 480*480*2
static size_t img_need = 0, img_got = 0; // raw bytes still expected / received for the current picture
static uint32_t img_last_ms = 0;         // a picture that stalls > 2 s is abandoned (the line parser resumes)
static lv_obj_t *img_obj;
static lv_img_dsc_t img_dsc;

static void img_show() {
  if (!img_obj) {
    img_dsc.header.always_zero = 0;
    img_dsc.header.w = 480;
    img_dsc.header.h = 480;
    img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    img_dsc.data_size = 480 * 480 * 2;
    img_dsc.data = img_buf;
    img_obj = lv_img_create(lv_layer_top());
    lv_obj_set_pos(img_obj, 0, 0);
    lv_obj_add_flag(img_obj, LV_OBJ_FLAG_CLICKABLE);   // swallow taps while a mock-up is shown
  }
  lv_img_set_src(img_obj, &img_dsc);
  lv_img_cache_invalidate_src(&img_dsc);
  lv_obj_move_foreground(img_obj);
  lv_obj_clear_flag(img_obj, LV_OBJ_FLAG_HIDDEN);
  lv_obj_invalidate(img_obj);
}

static void img_hide() {
  if (img_obj) lv_obj_add_flag(img_obj, LV_OBJ_FLAG_HIDDEN);
}

// consume raw picture bytes from [p, p+n); returns how many were taken
static size_t img_feed(const char *p, size_t n) {
  size_t take = n < img_need - img_got ? n : img_need - img_got;
  if (img_buf) memcpy(img_buf + img_got, p, take);
  img_got += take;
  img_last_ms = millis();
  if (img_got >= img_need) {
    img_need = img_got = 0;
    if (img_buf) img_show();
  }
  return take;
}
static JsonDocument *in_doc;             // parse target; swapped into the model (no deep copy)
static char sent_screen[16] = "";        // last visible screen id reported to the host

// link-level stats, reported as {"t":"linkperf",...} right before the {"t":"perf"} reply (same window)
enum { LS_FULL, LS_PART, LS_EVENT, LS_CMD, LS__N };
static const char *LS_NAME[LS__N] = {"state_full", "state_part", "event", "cmd"};
struct LinkStat { uint32_t n, bytes, parse_us, parse_max, apply_us, apply_max; };
static LinkStat lstat[LS__N];

static void link_report() {
  Link.print("{\"t\":\"linkperf\"");
  for (int i = 0; i < LS__N; i++) {
    const LinkStat &st = lstat[i];
    Link.printf(",\"%s\":{\"n\":%u,\"bytes\":%u,\"parse_avg_ms\":%.2f,\"parse_max_ms\":%.1f,\"apply_avg_ms\":%.2f,\"apply_max_ms\":%.1f}",
                  LS_NAME[i], st.n, st.bytes, st.n ? st.parse_us / 1000.0 / st.n : 0.0, st.parse_max / 1000.0,
                  st.n ? st.apply_us / 1000.0 / st.n : 0.0, st.apply_max / 1000.0);
  }
  Link.print("}\n");
  memset(lstat, 0, sizeof(lstat));
}

struct SpiRamAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static SpiRamAllocator alloc;

// caps: "part" = understands partial states ({"t":"state","part":1,...}); "screen" = reports the visible screen.
static void send_hello() {
  const char *scr = ui_current();
  Link.printf("{\"t\":\"hello\",\"fw\":\"%s\",\"board\":\"%s\",\"w\":480,\"h\":480,\"caps\":[\"part\",\"screen\"],\"screen\":\"%s\"}\n",
                FW_VERSION, BOARD_NAME, scr ? scr : "");
  strlcpy(sent_screen, scr ? scr : "", sizeof(sent_screen));
}

void link_init() {
  buf = (char *)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
  in_doc = new JsonDocument(&alloc);
  send_hello();
}

static void dispatch(const char *line, size_t n) {
  uint32_t t0 = perf_now_us();
  {
    PerfScope ps(PERF_PARSE);
    if (deserializeJson(*in_doc, line, n)) return;
  }
  uint32_t t1 = perf_now_us();
  JsonDocument &doc = *in_doc;
  const char *t = doc["t"] | "";
  int kind = !strcmp(t, "state") ? ((doc["part"] | 0) ? LS_PART : LS_FULL) : !strcmp(t, "event") ? LS_EVENT : LS_CMD;
  bool perf_req = kind == LS_CMD && doc["perf"];
  if (!strcmp(t, "state")) {
    if (doc["part"] | 0) model_merge_state(in_doc, n);  // partial: only the top-level keys present change
    else model_adopt_state(in_doc, n);                // full: swap documents (in_doc now holds the old state)
  } else if (!strcmp(t, "event")) {
    model_adopt_event(in_doc);                      // swap into the event ring
    ui_dispatch_event(model_event(model_event_count() - 1));
  } else if (!strcmp(t, "cmd")) {
    if (doc["bl"].is<int>()) hal_set_brightness(doc["bl"].as<int>());
    if (doc["nav"].is<const char *>()) ui_nav(doc["nav"]);
    if (doc["open"].is<const char *>()) ui_open(doc["open"], doc["sid"] | "");
    if (doc["demo_alert"].is<const char *>()) ui_demo_alert(doc["demo_alert"]);
    if (doc["shot"]) { lv_refr_now(NULL); hal_send_screenshot(); }
    if (doc["hello"]) send_hello();
    if (doc["mirror"].is<int>()) home_face_debug_mirror(doc["mirror"] | -1);   // QA override, -1 = follow Settings
    if (doc["anim"].is<const char *>())   // QA: force a home animation ("" releases)
      home_face_debug(doc["anim"], doc["frame"] | -1, doc["helpers"] | 0, (doc["grey"] | 0) != 0);
    if (doc["img"].is<int>()) {
      size_t n = doc["img"].as<int>();
      if (n == 0) { img_hide(); img_need = img_got = 0; }
      else if (n == 480 * 480 * 2) {
        if (!img_buf) img_buf = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
        img_need = n; img_got = 0; img_last_ms = millis();
      }
    }
    if (perf_req) { link_report(); perf_report(); }
  }
  if (perf_req) return;
  uint32_t t2 = perf_now_us();
  LinkStat &st = lstat[kind];
  st.n++; st.bytes += n;
  st.parse_us += t1 - t0; if (t1 - t0 > st.parse_max) st.parse_max = t1 - t0;
  st.apply_us += t2 - t1; if (t2 - t1 > st.apply_max) st.apply_max = t2 - t1;
}

// Tell the host which screen is visible (it streams `system` only while that screen shows, etc.).
static void report_screen() {
  const char *scr = ui_current();
  if (!scr || !strcmp(scr, sent_screen)) return;
  strlcpy(sent_screen, scr, sizeof(sent_screen));
  Link.printf("{\"t\":\"screen\",\"id\":\"%s\"}\n", scr);
}

void link_poll() {
  report_screen();
  if (img_need && millis() - img_last_ms > 2000) img_need = img_got = 0;   // host went away mid-picture
  int budget = 64 * 1024;  // bytes per loop iteration, keep UI responsive
  while (budget > 0) {
    size_t room = CAP - 1 - len;
    if (room == 0) { overflow = true; len = 0; room = CAP - 1; }   // runaway line: discard
    size_t got = Link.read((uint8_t *)buf + len, room);
    if (got == 0) break;
    budget -= (int)got;
    size_t scan = len, end = len + got, start = 0;
    if (img_need) {                    // raw picture bytes in flight: they are not lines
      size_t took = img_feed(buf + len, got);
      start = scan = len + took;
    }
    while (scan < end) {
      char *nl = (char *)memchr(buf + scan, '\n', end - scan);
      if (!nl) break;
      size_t pos = nl - buf;
      if (!overflow && pos > start) dispatch(buf + start, pos - start);
      overflow = false;
      start = scan = pos + 1;
      if (img_need) {                  // the cmd just parsed announced a picture: the rest is raw
        size_t took = img_feed(buf + scan, end - scan);
        start = scan = scan + took;
      }
    }
    if (start) {                       // keep the partial tail at the start of buf
      memmove(buf, buf + start, end - start);
      len = end - start;
    } else {
      len = end;
    }
  }
}

void link_send(JsonDocument &doc) {
  char tmp[768];
  size_t n = serializeJson(doc, tmp, sizeof tmp - 1);
  tmp[n++] = '\n';
  Link.write(tmp, n, 50);
}

void link_action(const char *action, const char *sid) {
  JsonDocument d;
  d["t"] = "action";
  d["action"] = action;
  if (sid) d["sid"] = sid;
  link_send(d);
}
