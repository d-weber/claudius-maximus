#pragma once
// Clawd's Studio on the home screen (home_face.cpp): the top-down pixel-art room from screens/room/, drawn
// into a 120 x 106 index canvas by the director and shown at 4x (480 x 424) above the HUD band.
#include <lvgl.h>

enum HomeMood {
  HOME_MOOD_OFFLINE,   // no host: grey room, Clawd asleep
  HOME_MOOD_SLEEP,     // host ok, no sessions: asleep in bed
  HOME_MOOD_IDLE,      // sessions, none busy: wandering, sitting, looking out of the window
  HOME_MOOD_BUSY,      // agents working: at the station of the current tool
  HOME_MOOD_WAITING,   // an agent needs the human: amber room, Clawd at the door with the "!"
};

enum HomeReact {
  HOME_REACT_GLANCE,   // quick look at the viewer
  HOME_REACT_HOP,      // small hop + check (todo done)
  HOME_REACT_SURPRISE, // eyes wide (session start, wake-up)
  HOME_REACT_WINK,
  HOME_REACT_GIGGLE,   // tapped
  HOME_REACT_HAPPY,    // agent finished: jumps + sparkles
  HOME_REACT_WINCE,    // tool error: shake + sweat
  HOME_REACT_COUNT
};

// Creates the room: full width, 424 px tall, at y inside parent.
void home_face_create(lv_obj_t *parent, lv_coord_t y);
// tool_cat: 'e' edit, 'r' read, 's' search, 'x' exec, 'w' web, 't' think, 'a' delegate, 'p' plan, 'v' review, 0 none
void home_face_set_mood(HomeMood m, float energy, int helpers, char tool_cat);
void home_face_set_plan(int done, int total);   // shown on the whiteboard
void home_face_react(HomeReact r);
// Speech bubble. Lower priority bubbles don't interrupt a fresh higher-priority one.
void home_face_say(const char *text, uint32_t ms, int prio);
bool home_face_bubble_visible();
void home_face_on_tap(void (*cb)());
void home_face_tick(bool visible);  // call at ~25-30 fps; the room keeps living while hidden, it just does not draw
void home_face_resume();            // call when becoming visible
// Dev/QA ({"t":"cmd","anim":...}): force a task by name ("" = release): code web review read run search think
// delegate plan idle wait sleep offline. frame is ignored; helpers forces the helper count; grey is ignored.
void home_face_debug(const char *task, int frame, int helpers, bool grey);
void home_face_debug_mirror(int m);     // QA: 0/1 overrides Settings.home_mirror, -1 follows it
void home_face_status(char *buf, size_t n);   // JSON object: current activity, task, pacing (perf report)
