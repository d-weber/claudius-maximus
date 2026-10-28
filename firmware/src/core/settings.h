#pragma once
// Persistent user settings (NVS). Owned by alerts-settings; read by anyone.
// Existing fields are stable (ambient.cpp etc. read them). New fields are appended at the end.
#include "sys.h"

struct Settings {
  uint8_t brightness = 80;          // active backlight %
  uint8_t dim_brightness = 12;      // backlight % when dimmed
  uint16_t dim_after_s = 180;       // no touch & no agent activity for this long -> dim (0 = never)
  uint16_t saver_after_s = 900;     // ... -> screensaver / night clock (0 = never)
  bool alert_waiting = true;        // full alert when an agent needs the human
  bool alert_done = true;           // celebrate when an agent finishes
  bool night_mode = true;           // extra dim + clock between night_start_h and night_end_h
  uint8_t night_start_h = 22;
  uint8_t night_end_h = 7;
  // --- appended fields ---
  bool alert_errors = true;         // red flash + toast on tool_error
  bool alert_subagents = true;      // tiny violet chips on subagent start/stop
  bool home_mirror = false;         // flip the isometric home scene horizontally
};

Settings &settings();
void settings_load();               // call once at boot (applies brightness)
void settings_save();               // write to NVS now
void settings_save_later();         // debounced save (~1.5 s after the last call); use from UI controls
bool settings_night_now(uint32_t epoch, int32_t tz);  // true if night mode is on and local time is in the window
