#pragma once
// Holds the latest `state` snapshot from the host (raw JSON, PSRAM-backed) and recent events.
// See PROTOCOL.md for the schema. Read fields defensively: obj["x"] | default.
#include <ArduinoJson.h>

void model_init();
void model_set_state(JsonObjectConst state);   // called by link on a `state` message
void model_push_event(JsonObjectConst ev);     // called by link on an `event` message

JsonObjectConst model_state();                 // latest snapshot (empty object before first state)
uint32_t model_state_version();                // increments on every new snapshot
bool model_host_connected();                   // a state arrived within the last 6 s
uint32_t model_now();                          // current epoch seconds (synced from host, 0 if never)
int32_t model_tz();                            // UTC offset seconds

// Find a session object by short id in the latest snapshot (null object if missing).
JsonObjectConst model_find_session(const char *sid);

// Recent events, newest last. Up to MODEL_MAX_EVENTS kept.
#define MODEL_MAX_EVENTS 64
size_t model_event_count();
JsonObjectConst model_event(size_t i);         // 0 = oldest
