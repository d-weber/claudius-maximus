#pragma once
#include "../ui/ui.h"

// Main tiles (owner in parentheses — only edit your own files)
extern const ScreenDef screen_home;      // screen_home.cpp      (companion)
extern const ScreenDef screen_agents;    // screen_agents.cpp    (agents overview)
extern const ScreenDef screen_feed;      // screen_feed.cpp      (activity feed)
extern const ScreenDef screen_usage;     // screen_usage.cpp     (usage & stats)
extern const ScreenDef screen_orbit;     // screen_orbit.cpp     (ambient visualization)
extern const ScreenDef screen_settings;  // screen_settings.cpp  (settings)

// Session detail overlay (screen_detail.cpp). Opened by tapping a session anywhere.
void detail_create(lv_obj_t *scr);
void detail_open(const char *sid);
void detail_close();
bool detail_is_open();
void detail_update(JsonObjectConst state);
void detail_on_event(JsonObjectConst ev);
