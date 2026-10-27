#pragma once
// "Claudius" — the animated spark character on the home screen (home_face.cpp).
// Procedurally drawn with LVGL draw primitives inside one full-width object; only the
// character's bounding box is invalidated each frame.
#include <lvgl.h>

enum HomeMood {
  HOME_MOOD_OFFLINE,   // no host: grey, fast asleep
  HOME_MOOD_SLEEP,     // host ok, no sessions: napping
  HOME_MOOD_IDLE,      // sessions, none busy: calm, smiling, looking around
  HOME_MOOD_BUSY,      // agents working: focused, energetic rays
  HOME_MOOD_WAITING,   // an agent needs the human: amber, eyebrows up, "!" badge, hops
};

enum HomeReact {
  HOME_REACT_GLANCE,   // quick look down at the activity card
  HOME_REACT_HOP,      // small happy hop (todo done)
  HOME_REACT_SURPRISE, // eyes wide (session start, wake-up)
  HOME_REACT_WINK,
  HOME_REACT_GIGGLE,   // tapped
  HOME_REACT_HAPPY,    // agent finished: bounce + sparkles
  HOME_REACT_WINCE,    // tool error: > < and a shake
  HOME_REACT_COUNT
};

// Creates the character area: full width, `h` tall, at y=`y` inside parent.
void home_face_create(lv_obj_t *parent, lv_coord_t y, lv_coord_t h);
// tool_cat: 'r' read/search, 'e' edit, 'x' exec, 'w' web, 't' think, 'a' delegate, 0 none
void home_face_set_mood(HomeMood m, float energy, int helpers, char tool_cat);
void home_face_react(HomeReact r);
// Speech bubble. Lower priority bubbles don't interrupt a fresh higher-priority one.
void home_face_say(const char *text, uint32_t ms, int prio);
bool home_face_bubble_visible();
void home_face_on_tap(void (*cb)());
void home_face_tick();            // call at ~25-30 fps while visible
void home_face_resume();          // call when becoming visible (resets frame clock)
// Dev/QA: force an animation by name ("" = release), a frame (-1 = play), helpers count and grey tint.
void home_face_debug(const char *anim, int frame, int helpers, bool grey);
void home_face_debug_mirror(int m);     // QA: 0/1 overrides Settings.home_mirror, -1 follows it
void home_face_status(char *buf, size_t n);   // JSON object: current animation, frame, pacing (perf report)
