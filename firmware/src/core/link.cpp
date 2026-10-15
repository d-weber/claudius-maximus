#include "link.h"
#include <lvgl.h>
#include <Arduino.h>
#include "model.h"
#include "hal.h"
#include "perf.h"
#include "../ui/ui.h"

#define FW_VERSION "0.2.0"
static char *buf;
static size_t len = 0;
static bool overflow = false;            // current line exceeded CAP: drop it up to the next '\n'
static const size_t CAP = 32 * 1024;
static JsonDocument *in_doc;             // parse target; swapped into the model (no deep copy)
static char sent_screen[16] = "";        // last visible screen id reported to the host

// link-level stats, reported as {"t":"linkperf",...} right before the {"t":"perf"} reply (same window)
enum { LS_FULL, LS_PART, LS_EVENT, LS_CMD, LS__N };
static const char *LS_NAME[LS__N] = {"state_full", "state_part", "event", "cmd"};
struct LinkStat { uint32_t n, bytes, parse_us, parse_max, apply_us, apply_max; };
static LinkStat lstat[LS__N];

static void link_report() {
  Serial.print("{\"t\":\"linkperf\"");
  for (int i = 0; i < LS__N; i++) {
    const LinkStat &st = lstat[i];
    Serial.printf(",\"%s\":{\"n\":%u,\"bytes\":%u,\"parse_avg_ms\":%.2f,\"parse_max_ms\":%.1f,\"apply_avg_ms\":%.2f,\"apply_max_ms\":%.1f}",
                  LS_NAME[i], st.n, st.bytes, st.n ? st.parse_us / 1000.0 / st.n : 0.0, st.parse_max / 1000.0,
                  st.n ? st.apply_us / 1000.0 / st.n : 0.0, st.apply_max / 1000.0);
  }
  Serial.print("}\n");
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
  Serial.printf("{\"t\":\"hello\",\"fw\":\"%s\",\"w\":480,\"h\":480,\"caps\":[\"part\",\"screen\"],\"screen\":\"%s\"}\n",
                FW_VERSION, scr ? scr : "");
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
  Serial.printf("{\"t\":\"screen\",\"id\":\"%s\"}\n", scr);
}

void link_poll() {
  report_screen();
  int budget = 64 * 1024;  // bytes per loop iteration, keep UI responsive
  while (budget > 0) {
    int avail = Serial.available();
    if (avail <= 0) break;
    size_t room = CAP - 1 - len;
    if (room == 0) { overflow = true; len = 0; room = CAP - 1; }   // runaway line: discard
    size_t want = (size_t)avail < room ? (size_t)avail : room;
    size_t got = Serial.read((uint8_t *)buf + len, want);
    if (got == 0) break;
    budget -= (int)got;
    size_t scan = len, end = len + got, start = 0;
    while (scan < end) {
      char *nl = (char *)memchr(buf + scan, '\n', end - scan);
      if (!nl) break;
      size_t pos = nl - buf;
      if (!overflow && pos > start) dispatch(buf + start, pos - start);
      overflow = false;
      start = scan = pos + 1;
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
  serializeJson(doc, Serial);
  Serial.print('\n');
}

void link_action(const char *action, const char *sid) {
  JsonDocument d;
  d["t"] = "action";
  d["action"] = action;
  if (sid) d["sid"] = sid;
  link_send(d);
}
