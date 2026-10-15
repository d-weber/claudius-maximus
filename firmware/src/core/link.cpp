#include "link.h"
#include <lvgl.h>
#include <Arduino.h>
#include "model.h"
#include "hal.h"
#include "perf.h"
#include "../ui/ui.h"

#define FW_VERSION "0.1.0"
static char *buf;
static size_t len = 0;
static const size_t CAP = 32 * 1024;

struct SpiRamAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static SpiRamAllocator alloc;

static void send_hello() {
  Serial.printf("{\"t\":\"hello\",\"fw\":\"%s\",\"w\":480,\"h\":480}\n", FW_VERSION);
}

void link_init() {
  buf = (char *)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
  send_hello();
}

static void dispatch(const char *line, size_t n) {
  JsonDocument doc(&alloc);
  {
    PerfScope ps(PERF_PARSE);
    if (deserializeJson(doc, line, n)) return;
  }
  const char *t = doc["t"] | "";
  if (!strcmp(t, "state")) {
    model_set_state(doc.as<JsonObjectConst>());
  } else if (!strcmp(t, "event")) {
    model_push_event(doc.as<JsonObjectConst>());
    ui_dispatch_event(doc.as<JsonObjectConst>());
  } else if (!strcmp(t, "cmd")) {
    if (doc["bl"].is<int>()) hal_set_brightness(doc["bl"].as<int>());
    if (doc["nav"].is<const char *>()) ui_nav(doc["nav"]);
    if (doc["open"].is<const char *>()) ui_open(doc["open"], doc["sid"] | "");
    if (doc["demo_alert"].is<const char *>()) ui_demo_alert(doc["demo_alert"]);
    if (doc["shot"]) { lv_refr_now(NULL); hal_send_screenshot(); }
    if (doc["hello"]) send_hello();
    if (doc["perf"]) perf_report();
  }
}

void link_poll() {
  int budget = 64 * 1024;  // bytes per loop iteration, keep UI responsive
  while (Serial.available() && budget-- > 0) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\n') {
      if (len) dispatch(buf, len);
      len = 0;
    } else if (len < CAP - 1) {
      buf[len++] = (char)c;
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
