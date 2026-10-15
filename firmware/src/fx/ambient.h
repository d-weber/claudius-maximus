#pragma once
// Ambient behaviour: auto-dim, screensaver / night clock, wake on touch or activity. (ambient.cpp)
#include <ArduinoJson.h>
void ambient_init();
void ambient_update(JsonObjectConst state);
void ambient_on_event(JsonObjectConst ev);
void ambient_tick();
