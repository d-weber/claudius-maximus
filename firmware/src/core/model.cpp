#include "model.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

struct SpiRamAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static SpiRamAllocator alloc;

static JsonDocument *state_doc;
static JsonDocument *events[MODEL_MAX_EVENTS];
static size_t ev_head = 0, ev_count = 0;
static uint32_t version = 0;
static uint32_t last_state_ms = 0;
static uint32_t epoch_base = 0, epoch_base_ms = 0;
static int32_t tz = 0;

void model_init() {
  state_doc = new JsonDocument(&alloc);
  state_doc->to<JsonObject>();
  for (auto &e : events) e = new JsonDocument(&alloc);
}

void model_set_state(JsonObjectConst s) {
  state_doc->set(s);
  version++;
  last_state_ms = millis();
  uint32_t ts = s["ts"] | 0;
  if (ts) { epoch_base = ts; epoch_base_ms = millis(); }
  tz = s["tz"] | tz;
}

void model_push_event(JsonObjectConst ev) {
  size_t idx = (ev_head + ev_count) % MODEL_MAX_EVENTS;
  if (ev_count == MODEL_MAX_EVENTS) { idx = ev_head; ev_head = (ev_head + 1) % MODEL_MAX_EVENTS; }
  else ev_count++;
  events[idx]->set(ev);
}

JsonObjectConst model_state() { return state_doc->as<JsonObjectConst>(); }
uint32_t model_state_version() { return version; }
bool model_host_connected() { return version > 0 && millis() - last_state_ms < 6000; }
uint32_t model_now() { return epoch_base ? epoch_base + (millis() - epoch_base_ms) / 1000 : 0; }
int32_t model_tz() { return tz; }

JsonObjectConst model_find_session(const char *sid) {
  for (JsonObjectConst s : model_state()["sessions"].as<JsonArrayConst>())
    if (sid && strcmp(s["id"] | "", sid) == 0) return s;
  return JsonObjectConst();
}

size_t model_event_count() { return ev_count; }
JsonObjectConst model_event(size_t i) {
  if (i >= ev_count) return JsonObjectConst();
  return events[(ev_head + i) % MODEL_MAX_EVENTS]->as<JsonObjectConst>();
}
