#pragma once
// Attention system on lv_layer_top(): toasts, edge glow, celebrations. (alerts.cpp)
#include <ArduinoJson.h>
void alerts_init();
void alerts_update(JsonObjectConst state);
void alerts_on_event(JsonObjectConst ev);
void alerts_demo(const char *kind);
