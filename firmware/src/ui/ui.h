#pragma once
// Screen manager. Main screens live in a horizontal swipeable tileview (order in ui.cpp).
// Overlays: session detail (screens/screen_detail.cpp) and alerts (fx/alerts.cpp) sit above.
#include <lvgl.h>
#include <ArduinoJson.h>

struct ScreenDef {
  const char *id;                          // "home", "agents", ...
  const char *title;
  void (*create)(lv_obj_t *parent);        // build widgets inside parent (480x480 tile)
  void (*update)(JsonObjectConst state);   // new state snapshot (called while visible + on show); may be null
  void (*on_event)(JsonObjectConst ev);    // every incoming event (visible or not); may be null
  void (*on_show)(bool visible);           // tile became visible / hidden; may be null
};

void ui_init();
void ui_tick();                            // call from loop: pushes new state to visible screen + overlays
void ui_nav(const char *id);               // switch tile (animated)
const char *ui_current();                  // id of visible tile
void ui_open(const char *what, const char *sid);  // "detail" + session id
void ui_dispatch_event(JsonObjectConst ev);
void ui_demo_alert(const char *kind);
