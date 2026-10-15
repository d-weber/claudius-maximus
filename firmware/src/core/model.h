#pragma once
// Holds the latest `state` snapshot from the host (raw JSON, PSRAM-backed) and recent events.
// See PROTOCOL.md for the schema. Read fields defensively: obj["x"] | default.
#include <ArduinoJson.h>

void model_init();
// Called by link (documents are swapped, not copied: the caller's pointer receives the previous doc).
void model_adopt_state(JsonDocument *&doc, size_t line_bytes);  // full `state`
void model_merge_state(JsonDocument *&doc, size_t line_bytes); // partial `state` ("part":1): keys present replace ours
void model_adopt_event(JsonDocument *&doc);    // `event`
// Legacy copy-in API (kept for tools/tests).
void model_set_state(JsonObjectConst state);
void model_push_event(JsonObjectConst ev);

JsonObjectConst model_state();                 // latest snapshot (empty object before first state)
uint32_t model_state_version();                // increments when the snapshot changed (not on empty heartbeats,
                                               // except at least every 10 s so relative times refresh)
// Version of one top-level part of the state ("sessions", "totals", "usage", "system", ...): equals the
// model_state_version() at which that part last changed (0 = never received). Cheap; use it in update() to skip
// work when the parts a screen shows did not change:
//   static uint32_t seen; uint32_t v = model_part_version("usage"); if (v == seen) return; seen = v;
// The host only re-sends a part when its content changed, so equal versions mean identical data.
uint32_t model_part_version(const char *part);
bool model_host_connected();                   // a state arrived within the last 6 s
uint32_t model_now();                          // current epoch seconds (synced from host, 0 if never)
int32_t model_tz();                            // UTC offset seconds

// Find a session object by short id in the latest snapshot (null object if missing).
JsonObjectConst model_find_session(const char *sid);

// Recent events, newest last. Up to MODEL_MAX_EVENTS kept.
#define MODEL_MAX_EVENTS 64
size_t model_event_count();
JsonObjectConst model_event(size_t i);         // 0 = oldest
