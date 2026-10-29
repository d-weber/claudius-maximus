#include "model.h"
#include "sys.h"
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
static uint32_t last_state_ms = 0, last_bump_ms = 0;
static uint32_t epoch_base = 0, epoch_base_ms = 0;
static int32_t tz = 0;
static const uint32_t FORCE_BUMP_MS = 10000;

// per-part versions (top-level keys of the state)
#define MODEL_MAX_PARTS 16
struct Part { char name[16]; uint32_t ver; uint32_t bytes; };   // bytes = approx. JSON size
static Part parts[MODEL_MAX_PARTS];
static int nparts = 0;

static bool is_meta(const char *k) { return !strcmp(k, "t") || !strcmp(k, "ts") || !strcmp(k, "tz") || !strcmp(k, "part"); }

static Part *find_part(const char *k, bool create) {
  for (int i = 0; i < nparts; i++) if (!strcmp(parts[i].name, k)) return &parts[i];
  if (!create || nparts >= MODEL_MAX_PARTS || strlen(k) >= sizeof(parts[0].name)) return nullptr;
  Part *p = &parts[nparts++];
  strlcpy(p->name, k, sizeof(p->name));
  p->ver = 0;
  p->bytes = 0;
  return p;
}

void model_init() {
  state_doc = new JsonDocument(&alloc);
  state_doc->to<JsonObject>();
  for (auto &e : events) e = new JsonDocument(&alloc);
}

static void stamp(JsonObjectConst s) {
  last_state_ms = millis();
  uint32_t ts = s["ts"] | 0;
  if (ts) { epoch_base = ts; epoch_base_ms = last_state_ms; }
  tz = s["tz"] | tz;
}

static void bump() { version++; last_bump_ms = millis(); }

static uint32_t full_bytes = 0;    // line length of the last full state (size estimate before parts are known)

void model_adopt_state(JsonDocument *&doc, size_t line_bytes) {
  full_bytes = line_bytes;
  JsonDocument *old = state_doc;
  state_doc = doc;
  doc = old;
  bump();
  for (int i = 0; i < nparts; i++) parts[i].ver = version;   // full snapshot: everything may have changed
  for (JsonPairConst kv : state_doc->as<JsonObjectConst>()) {
    const char *k = kv.key().c_str();
    if (is_meta(k)) continue;
    if (Part *p = find_part(k, true)) { p->ver = version; p->bytes = 0; }   // sizes learnt from partials
  }
  stamp(state_doc->as<JsonObjectConst>());
}

// Partial state. Two strategies, whichever copies less:
//  - merge: copy the parts present in `in` into the current snapshot (small partials, e.g. `system`);
//  - rebuild: copy the parts *absent* from `in` into `in`, then swap (big partials, e.g. `sessions`).
void model_merge_state(JsonDocument *&doc, size_t line_bytes) {
  JsonObject src = doc->as<JsonObject>();
  int nkeys = 0;
  for (JsonPair kv : src) if (!is_meta(kv.key().c_str())) nkeys++;
  uint32_t nv = version + 1;
  uint32_t in_bytes = 0, total = 0;
  for (JsonPair kv : src) {
    const char *k = kv.key().c_str();
    if (is_meta(k)) continue;
    Part *p = find_part(k, true);
    if (!p) continue;
    p->ver = nv;
    p->bytes = nkeys == 1 ? line_bytes : measureJson(kv.value());
    in_bytes += p->bytes;
  }
  for (int i = 0; i < nparts; i++) total += parts[i].bytes;
  if (total < full_bytes) total = full_bytes;
  JsonObject dst = state_doc->as<JsonObject>();
  if (nkeys > 0 && !dst.isNull() && in_bytes * 2 > total) {
    // rebuild: carry the unchanged parts over into the incoming doc, then swap
    const char *present[MODEL_MAX_PARTS + 8];
    const char *nulls[MODEL_MAX_PARTS + 8];
    int np = 0, nn = 0;
    for (JsonPair kv : src) {
      if (np < MODEL_MAX_PARTS + 8) present[np++] = kv.key().c_str();
      if (kv.value().isNull() && nn < MODEL_MAX_PARTS + 8) nulls[nn++] = kv.key().c_str();
    }
    for (JsonPair kv : dst) {
      const char *k = kv.key().c_str();
      if (is_meta(k)) continue;
      bool in_src = false;
      for (int i = 0; i < np && !in_src; i++) in_src = !strcmp(present[i], k);
      if (!in_src) src[k].set(kv.value());
    }
    for (int i = 0; i < nn; i++) src.remove(nulls[i]);   // "key": null deletes
    src.remove("part");
    JsonDocument *old = state_doc;
    state_doc = doc;
    doc = old;
  } else if (nkeys > 0) {
    if (dst.isNull()) dst = state_doc->to<JsonObject>();
    for (JsonPair kv : src) {
      const char *k = kv.key().c_str();
      if (is_meta(k)) continue;
      if (kv.value().isNull()) dst.remove(k);
      else dst[k].set(kv.value());
    }
    // keep ts/tz in the snapshot current (some screens read state["ts"])
    if (!src["ts"].isNull()) dst["ts"] = src["ts"];
    if (!src["tz"].isNull()) dst["tz"] = src["tz"];
  } else if (!dst.isNull()) {
    if (!src["ts"].isNull()) dst["ts"] = src["ts"];
  }
  if (nkeys > 0 || millis() - last_bump_ms >= FORCE_BUMP_MS) bump();
  stamp(state_doc->as<JsonObjectConst>());
}

void model_set_state(JsonObjectConst s) {
  JsonDocument *d = new JsonDocument(&alloc);
  d->set(s);
  model_adopt_state(d, measureJson(s));
  delete d;
}

static size_t ev_slot() {
  size_t idx = (ev_head + ev_count) % MODEL_MAX_EVENTS;
  if (ev_count == MODEL_MAX_EVENTS) { idx = ev_head; ev_head = (ev_head + 1) % MODEL_MAX_EVENTS; }
  else ev_count++;
  return idx;
}

void model_adopt_event(JsonDocument *&doc) {
  size_t idx = ev_slot();
  JsonDocument *old = events[idx];
  events[idx] = doc;
  doc = old;
}

void model_push_event(JsonObjectConst ev) { events[ev_slot()]->set(ev); }

JsonObjectConst model_state() { return state_doc->as<JsonObjectConst>(); }
uint32_t model_state_version() { return version; }
uint32_t model_part_version(const char *part) {
  Part *p = part ? find_part(part, false) : nullptr;
  return p ? p->ver : 0;
}
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
