#pragma once
// The director: decides what Clawd and his room do every tick and draws the frame. Portable (no LVGL).
//
// Inputs are the aggregate agent mood + the current tool, the helper count, the plan progress and the local time
// of day. From those it derives a *task* (code at the desk, read at the shelf, run the rack, think on the rug,
// wait at the door, sleep in bed...), walks Clawd there along the room's waypoints, plays the task's base loop
// and sprinkles in fidgets (stretch, sip, glance, look around...) and errands (water the plant, check the
// whiteboard, grab a book, gaze out of the window) at random, non-repeating intervals. The room itself lives on
// its own clock: the sky, sun patch and lamp follow the real time of day, clouds drift, it sometimes rains, a bird
// lands on the sill, stars twinkle, the server rack blinks, the monitor goes to its screensaver when nobody uses it.
#include "room.h"

namespace room {

enum Mood { MOOD_OFFLINE, MOOD_SLEEP, MOOD_IDLE, MOOD_BUSY, MOOD_WAITING };
enum React { REACT_GLANCE, REACT_HOP, REACT_SURPRISE, REACT_WINK, REACT_GIGGLE, REACT_HAPPY, REACT_WINCE, REACT_COUNT };

struct Input {
  Mood mood = MOOD_OFFLINE;
  char tool = 0;            // 'e' edit/write, 'r' read, 's' search (grep/glob), 'x' exec, 'w' web, 't' think, 'a' delegate, 'p' plan, 'v' review
  int helpers = 0;          // active subagents (0..3 shown)
  float energy = 0;         // 0..1
  float tod = 12.f;         // local time of day, hours
  int yday = 0;             // day of year (moon phase)
  int todoTotal = 0, todoDone = 0;
};

enum Task { TASK_CODE, TASK_WEB, TASK_REVIEW, TASK_READ, TASK_RUN, TASK_SEARCH, TASK_THINK, TASK_DELEGATE, TASK_PLAN,
            TASK_IDLE, TASK_WAIT, TASK_SLEEP, TASK_OFFLINE, TASK_COUNT };
enum Station { ST_DESK, ST_SHELF, ST_RACK, ST_RUG, ST_DOOR, ST_BED, ST_WINDOW, ST_BOARD, ST_PLANT, ST_UNDERDESK, ST_DRAWER,
               ST_COUNT };
enum Fidget { FID_NONE = -1, FID_GLANCE = 0, FID_STRETCH, FID_SCRATCH, FID_SIP, FID_LOOK_AROUND, FID_PAUSE, FID_NOD, FID_HOP,
              FID_TAP_FOOT, FID_SHRUG, FID_SPIN, FID_WAVE, FID_YAWN, FID_DOZE,
              ERR_WATER, ERR_WINDOW, ERR_COFFEE, ERR_BOOK, ERR_BOARD, ERR_RELOCATE,
              FID_COUNT };

struct Director {
  void init(uint32_t seed, float tod);
  void setInput(const Input &in);
  void react(React r);
  void step();                          // advance one tick (1 / FPS s)
  void draw(Canvas &c);
  // Lighting: the RGB565 table to show the canvas with. dirty is set when it changed since the last call.
  const uint16_t *palette(bool *dirty);
  int headX() const { return cx + 9; }  // logical coords of Clawd's head (speech bubble anchor)
  int headY() const { return inBed ? 64 : cy; }
  bool clawdVisible() const { return !inBed; }
  const char *activity() const;         // "code", "walk>desk", "fidget:sip", "errand:water", "react:happy", ...
  // Debug: force a task by name ("" releases): code web review read run search think delegate plan idle wait sleep offline
  bool force(const char *name);
  const char *forcedName() const { return forced ? TASK_NAMES[task] : ""; }

  static const char *const TASK_NAMES[TASK_COUNT];

  // ---- state (public for the simulator's logs)
  Input in;
  bool forced = false;
  Rng rng;
  uint32_t tick = 0;
  Task task = TASK_OFFLINE;
  Station station = ST_BED;             // where the task lives (or the errand target while on an errand)
  Station taskStation = ST_BED;
  // Clawd
  int cx = 10, cy = 76;                 // sprite top-left
  float fx = 10, fy = 87;               // feet position (walking is sub-pixel)
  bool walking = false;
  struct Node { int8_t x, y, st, wp; };  // feet position + which station waypoint it is (wp -1 = hallway)
  Node path[8]; int pathN = 0, pathI = 0;
  Node here = {10, 87, ST_BED, 1};        // last node reached
  Station pending = ST_COUNT;            // retarget requested mid-walk (applied at the next node)
  Station pendingTask = ST_COUNT;        // task change held back by the dwell time
  Task pendingTaskId = TASK_COUNT;
  int dwell = 0;
  float speed = 1.5f;
  View view = FRONT, walkView = FRONT;
  int legPhase = 0, legT = 0;
  bool inBed = false;
  int atT = 0;                          // ticks since arriving at the station
  int blinkT = 0, blinkLen = 0;
  // fidgets / errands
  Fidget fid = FID_NONE; int fidT = 0, fidLen = 0;
  int nextFid = 0;
  int8_t fidHist[4] = {-1, -1, -1, -1};
  int errPhase = 0;                     // 0 going, 1 doing, 2 returning
  // reactions
  int reactId = -1, reactT = 0;
  // task loop state
  int typeBurst = 0, typePause = 0;     // coding: ticks left in the current burst / pause
  int page = 0, pageT = 0;              // reading
  int runProg = -1, runT = 0; uint32_t runSeed = 1;
  int thinkDots = 0, dotsT = 0;
  int searchSpot = 0, searchT = 0;
  int idleSpot = 0, idleT = 0, idleFor = 0;
  int waitVar = 0, waitT = 0, waitNext = 0;
  int sleepShift = 0, sleepEyes = 0, sleepNext = 0, breath = 0;
  int emoteBob = 0;
  // helpers (mini Clawds)
  struct Mini { int state = 0; /*0 off 1 enter 2 at 3 leave*/ int st = -1; float x = 56, y = 98; int path[6][2]; int pathN = 0, pathI = 0; int legPhase = 0; View view = FRONT; int blink = 0; };
  Mini minis[3];
  int shownHelpers = 0;
  // room
  WindowState win;
  DeskState desk;
  int steam = 0;
  int saverT = 0;
  int plantSway = 0, plantT = 0, plantNext = 0;
  int shelfGap = -1;
  bool rain = false; int rainLeft = 0;
  int birdNext = 0, birdT = 0;
  int shootNext = 0;
  int ledT = 0;
  bool lampOn = false;
  // lighting
  Light light, lightTarget, builtLight;
  uint32_t skyA = 0x9fd8f0, skyB = 0xd4f2fb, builtSkyA = 0, builtSkyB = 0;
  uint16_t pal[N_COL];
  bool palDirty = true;
  int alarmPhase = 0;

 private:
  void chooseTask();
  Station stationFor(Task t);
  void goTo(Station s, float spd);
  void planPath(Station s);
  void applyTask(Task t);
  void stepWalk();
  void arrive();
  void stepTaskLoop();
  void stepFidgets();
  void startFidget(Fidget f);
  bool fidgetAllowed(Fidget f) const;
  void stepMinis();
  void stepAmbient();
  void stepLighting();
  void drawSprites(Canvas &c);
  void drawClawdNow(Canvas &c);
  void drawFx(Canvas &c);
  void composePose(ClawdPose &p, int &emote, Idx &emoteCol, int &jump, int &shake) const;
  bool atDesk() const { return station == ST_DESK && !walking; }
  bool busyTask() const { return task <= TASK_PLAN; }
};

}  // namespace room
