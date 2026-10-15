#pragma once
// Ambient behaviour: auto-dim, screensaver / night clock, wake on touch or activity. (ambient.cpp)
//
//  ACTIVE --(no touch & no agent activity for dim_after_s)--> DIM --(saver_after_s)--> SAVER (night clock)
//  Night hours (settings night_mode): lower active brightness; idle goes straight to the clock at dim_after_s,
//  and only touch or a `waiting` event wakes the screen (tool chatter at 3 am does not).
//  Any touch / event / tile change wakes instantly. The touch that wakes is swallowed (shield / saver layer).
//  The saver lives at the *bottom* of lv_layer_top(), so alert cards/glow (fx/alerts.cpp) stay visible above it.
//
//  Dev hook (via `tools/dev.py send`, RAM only, nothing persisted):
//    {"t":"event","kind":"dev_ambient","saver":1}           force the night clock (0 = release)
//    {"t":"event","kind":"dev_ambient","dim_s":5,"saver_s":10}  override timers (0 = back to settings)
#include <ArduinoJson.h>
void ambient_init();
void ambient_update(JsonObjectConst state);
void ambient_on_event(JsonObjectConst ev);
void ambient_tick();

void ambient_wake();                 // treat as user activity: restore brightness, hide saver
void ambient_force_saver(bool on);   // dev/testing: show the night clock until touch or force(false)
bool ambient_saver_active();         // night clock is covering the screen (screens may pause animations)
bool ambient_dimmed();               // backlight currently dimmed (DIM or SAVER)
