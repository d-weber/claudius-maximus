#pragma once
// The home screen's HUD band in pixel art (home_hud.cpp): clock and date on the left, the plan-limit bars on the
// right, drawn into a small index canvas and shown at 2x (half the room's zoom: finer text and gauges in the same
// pixel world as Clawd's Studio). Nothing runs per frame: the canvas is rebuilt when a value changes and LVGL
// repaints it from there.
#include <lvgl.h>

struct HudBar {
  const char *caption;   // "5H", "7D", a model name...; drawn upper-case, at most 6 characters
  int pct;               // 0..100, or -1 when unknown (dim caption, empty track)
  bool stale;            // the value is old: grey bar instead of the level colour
};

// Full-width band of h device px at y inside parent (h must be a multiple of the zoom, at most 28 logical rows).
lv_obj_t *home_hud_create(lv_obj_t *parent, lv_coord_t y, lv_coord_t h);
void home_hud_set_clock(const char *hhmm, const char *date);   // "" hides a line
void home_hud_set_limits(const HudBar *bars, int n);           // n = 0 hides the bars
// Rectangle of the bars block in device px relative to the band (for the tap target), valid once set_limits had n > 0.
void home_hud_limits_area(lv_area_t *out);
