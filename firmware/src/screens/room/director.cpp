// The director: Clawd's behaviour in his studio. See director.h for the overview.
#include "director.h"
#include "sprites_gen.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

namespace room {

static inline int S(float sec) { return (int)(sec * FPS + 0.5f); }
static const int HALL = 53;   // the "hallway": a free row of floor (feet y) every station connects to

const char *const Director::TASK_NAMES[TASK_COUNT] = {"code", "web", "review", "read", "run", "search", "think", "delegate",
                                                      "plan", "idle", "wait", "sleep", "offline"};

// ---------------------------------------------------------------- stations
struct StationDef { int8_t hx; int8_t wp[2][2]; int8_t n; View view; int8_t slot; };
static const StationDef ST[ST_COUNT] = {
    {79, {{79, 50}, {0, 0}}, 1, BACK, 0},     // ST_DESK
    {5, {{5, 54}, {0, 0}}, 1, FRONT, 1},      // ST_SHELF
    {96, {{96, 57}, {0, 0}}, 1, BACK, 2},     // ST_RACK
    {52, {{52, 73}, {0, 0}}, 1, FRONT, 3},    // ST_RUG
    {52, {{52, 98}, {0, 0}}, 1, FRONT, 4},    // ST_DOOR
    {38, {{38, 87}, {10, 87}}, 2, FRONT, 5},  // ST_BED (climb on from the right)
    {32, {{32, 42}, {0, 0}}, 1, BACK, 6},     // ST_WINDOW
    {56, {{56, 42}, {0, 0}}, 1, BACK, 7},     // ST_BOARD
    {84, {{84, 97}, {0, 0}}, 1, RIGHTV, 8},   // ST_PLANT
    {49, {{49, 53}, {0, 0}}, 1, RIGHTV, 0},   // ST_UNDERDESK
    {54, {{54, 53}, {0, 0}}, 1, RIGHTV, 0},   // ST_DRAWER
};
struct MiniDef { int8_t hx, x, y; View view; int8_t slot; int8_t vx, vy; };   // vx/vy: optional waypoint between the hallway and the spot
static const MiniDef MST[] = {
    {70, 70, 47, BACK, 0, -1, -1},    // desk, left of Clawd
    {9, 9, 50, FRONT, 1, -1, -1},     // bookshelf
    {104, 104, 55, BACK, 2, -1, -1},  // server rack
    {60, 60, 42, BACK, 7, -1, -1},    // whiteboard
    {36, 36, 42, BACK, 6, -1, -1},    // window
    {46, 46, 77, FRONT, 3, -1, -1},   // rug
    {88, 88, 97, RIGHTV, 8, -1, -1},  // by the plant
    {40, 40, 101, FRONT, 9, -1, -1},  // by the door mat
    {40, 14, 101, FRONT, 10, 40, 101}, // foot of the bed (walks round the bed)
    // overflow spots on the open floor (a crowd of helpers)
    {44, 44, 90, FRONT, 11, -1, -1},  {58, 58, 90, FRONT, 12, -1, -1},  {72, 72, 90, FRONT, 13, -1, -1},
    {86, 86, 88, FRONT, 14, -1, -1},  {40, 30, 90, FRONT, 15, 40, 90},  {72, 72, 101, FRONT, 16, -1, -1},
    {86, 86, 101, FRONT, 17, -1, -1}, {40, 28, 101, FRONT, 18, 40, 101}, {96, 96, 70, LEFTV, 19, -1, -1},
    {98, 98, 80, LEFTV, 20, -1, -1},  {40, 30, 80, RIGHTV, 21, 40, 80},
};
static const int N_MST = 20;

struct FidDef { const char *name; int len; uint16_t mask; int weight; };
#define M(t) (1u << (t))
static const uint16_t AWAKE = M(TASK_CODE) | M(TASK_WEB) | M(TASK_REVIEW) | M(TASK_READ) | M(TASK_RUN) | M(TASK_SEARCH) | M(TASK_THINK) |
                             M(TASK_DELEGATE) | M(TASK_PLAN) | M(TASK_IDLE);
static const uint16_t DESKT = M(TASK_CODE) | M(TASK_WEB) | M(TASK_REVIEW);
static const uint16_t FRONTT = M(TASK_READ) | M(TASK_THINK) | M(TASK_IDLE) | M(TASK_DELEGATE) | M(TASK_SEARCH) | M(TASK_WAIT);
static const FidDef FID[FID_COUNT] = {
    {"glance", S(1.0f), AWAKE, 8},
    {"stretch", S(1.4f), AWAKE, 5},
    {"scratch", S(1.2f), AWAKE, 5},
    {"sip", S(1.8f), DESKT, 7},
    {"look", S(1.6f), FRONTT, 6},
    {"pause", S(2.2f), M(TASK_CODE) | M(TASK_WEB) | M(TASK_RUN), 5},
    {"nod", S(0.9f), M(TASK_READ) | M(TASK_REVIEW) | M(TASK_PLAN) | M(TASK_RUN), 5},
    {"hop", S(0.7f), M(TASK_IDLE) | M(TASK_THINK) | M(TASK_DELEGATE), 3},
    {"tapfoot", S(1.4f), M(TASK_WAIT) | M(TASK_IDLE) | M(TASK_RUN), 4},
    {"shrug", S(1.2f), M(TASK_THINK) | M(TASK_SEARCH), 4},
    {"spin", S(0.7f), M(TASK_IDLE) | M(TASK_THINK), 2},
    {"wave", S(1.4f), M(TASK_IDLE) | M(TASK_WAIT) | M(TASK_DELEGATE), 3},
    {"yawn", S(1.5f), M(TASK_IDLE), 2},
    {"doze", S(1.0f), 0, 0},
    {"water", S(3.0f), M(TASK_IDLE) | M(TASK_THINK), 3},
    {"window", S(5.0f), M(TASK_IDLE) | M(TASK_THINK) | M(TASK_READ), 3},
    {"coffee", S(2.0f), M(TASK_READ) | M(TASK_THINK) | M(TASK_IDLE) | M(TASK_RUN) | M(TASK_PLAN), 3},
    {"book", S(3.2f), M(TASK_CODE) | M(TASK_WEB) | M(TASK_RUN) | M(TASK_PLAN) | M(TASK_REVIEW), 3},
    {"board", S(3.0f), M(TASK_CODE) | M(TASK_WEB) | M(TASK_RUN) | M(TASK_THINK) | M(TASK_READ) | M(TASK_IDLE), 3},
    {"relocate", S(0.5f), M(TASK_READ) | M(TASK_THINK), 3},
};
static bool isErrand(Fidget f) { return f >= ERR_WATER; }

// ---------------------------------------------------------------- init / input
void Director::init(uint32_t seed, float tod) {
  rng.s = seed ? seed : 0x9E3779B9u;
  for (int i = 0; i < 8; i++) rng.next();
  in = Input();
  in.tod = tod;
  task = TASK_OFFLINE; station = taskStation = ST_BED;
  inBed = true; walking = false;
  here = {10, 87, ST_BED, 1}; fx = 10; fy = 87; cx = 10; cy = 76;
  nextFid = S(12);
  blinkT = S(3);
  plantNext = S(10); birdNext = 0; shootNext = 0;
  waitNext = S(5);
  sleepNext = S(20);
  win.moonPhase = 4;
  stepLighting();
  light = lightTarget;
  buildPalette(light, skyA, skyB, pal);
  builtLight = light; builtSkyA = skyA; builtSkyB = skyB;
  palDirty = true;
}

void Director::setInput(const Input &i) {
  Input prev = in;
  if (forced) { in.tod = i.tod; in.yday = i.yday; in.helpers = i.helpers; in.todoTotal = i.todoTotal; in.todoDone = i.todoDone; return; }
  in = i;
  if (in.helpers > MAX_MINIS) in.helpers = MAX_MINIS;
  if (in.mood != prev.mood || in.tool != prev.tool) chooseTask();
}

bool Director::force(const char *name) {
  if (!name || !*name) { forced = false; chooseTask(); return true; }
  for (int t = 0; t < TASK_COUNT; t++) {
    if (!strcmp(TASK_NAMES[t], name)) {
      forced = true;
      Task tt = (Task)t;
      in.mood = tt == TASK_OFFLINE ? MOOD_OFFLINE : tt == TASK_SLEEP ? MOOD_SLEEP : tt == TASK_WAIT ? MOOD_WAITING : tt == TASK_IDLE ? MOOD_IDLE : MOOD_BUSY;
      static const char TOOLS[] = {'e', 'w', 'v', 'r', 'x', 's', 't', 'a', 'p', 0, 0, 0, 0};
      in.tool = TOOLS[t];
      in.energy = in.mood == MOOD_BUSY ? 0.6f : 0.1f;
      applyTask(tt);
      return true;
    }
  }
  return false;
}

static Task taskOf(const Input &in) {
  switch (in.mood) {
    case MOOD_OFFLINE: return TASK_OFFLINE;
    case MOOD_SLEEP: return TASK_SLEEP;
    case MOOD_WAITING: return TASK_WAIT;
    case MOOD_IDLE: return TASK_IDLE;
    default: break;
  }
  switch (in.tool) {
    case 'w': return TASK_WEB;
    case 'v': return TASK_REVIEW;
    case 'r': return TASK_READ;
    case 'x': return TASK_RUN;
    case 's': return TASK_SEARCH;
    case 't': return TASK_THINK;
    case 'a': return TASK_DELEGATE;
    case 'p': return TASK_PLAN;
    default: return TASK_CODE;
  }
}

void Director::chooseTask() {
  Task t = taskOf(in);
  if (t == task) { pendingTaskId = TASK_COUNT; return; }
  bool moodChange = (t >= TASK_IDLE) || (task >= TASK_IDLE);
  // Tool-driven switches wait for the dwell time so a quick Read in the middle of edits does not send Clawd
  // across the room and back. Mood changes (waiting, sleep, idle) are honoured at once.
  if (!moodChange && dwell > 0) { pendingTaskId = t; return; }
  applyTask(t);
}

Station Director::stationFor(Task t) {
  switch (t) {
    case TASK_CODE: case TASK_WEB: case TASK_REVIEW: return ST_DESK;
    case TASK_READ: return ST_SHELF;
    case TASK_RUN: return ST_RACK;
    case TASK_SEARCH: return ST_UNDERDESK;
    case TASK_THINK: case TASK_DELEGATE: return ST_RUG;
    case TASK_PLAN: return ST_BOARD;
    case TASK_WAIT: return ST_DOOR;
    case TASK_IDLE: { static const Station IDLE_SPOTS[] = {ST_RUG, ST_BED, ST_WINDOW, ST_BOARD, ST_DOOR}; return IDLE_SPOTS[rng.range(0, 4)]; }
    default: return ST_BED;
  }
}

void Director::applyTask(Task t) {
  Task old = task;
  task = t;
  pendingTaskId = TASK_COUNT;
  taskStation = stationFor(t);
  fid = FID_NONE; errPhase = 0;
  typeBurst = 0; typePause = 0; runProg = -1; thinkDots = 0; dotsT = 0; searchSpot = 0; searchT = 0;
  idleT = 0; idleFor = S(rng.range(20, 70)); waitT = 0; waitVar = 0; waitNext = S(rng.range(4, 8));
  shelfGap = (t == TASK_READ) ? rng.range(0, 8) : -1;
  if (t == TASK_RUN) { runProg = 0; runSeed = rng.next(); runT = S(rng.range(1, 2)); }
  if (t == TASK_READ) { page = 0; pageT = S(rng.range(3, 7)); }
  dwell = S(rng.range(7, 11));
  nextFid = S(rng.range(5, 12));
  float spd = t == TASK_WAIT ? 2.6f : 1.5f;
  if (inBed) {
    if (t == TASK_SLEEP || t == TASK_OFFLINE) return;      // stays in bed
    // wake up: sit up, yawn, then go
    inBed = false;
    here = {10, 87, ST_BED, 1}; fx = 10; fy = 87; station = ST_BED;
    startFidget(FID_YAWN);
    pending = taskStation;
    return;
  }
  (void)old;
  goTo(taskStation, spd);
}

// ---------------------------------------------------------------- walking
void Director::planPath(Station tgt) {
  pathN = 0; pathI = 0;
  auto add = [&](int x, int y, int st, int wp) {
    if (pathN > 0 && path[pathN - 1].x == x && path[pathN - 1].y == y) return;
    if (pathN < 8) path[pathN++] = {(int8_t)x, (int8_t)y, (int8_t)st, (int8_t)wp};
  };
  const StationDef &cur = ST[here.st];
  if (here.wp >= 0) {
    for (int k = here.wp - 1; k >= 0; k--) add(cur.wp[k][0], cur.wp[k][1], here.st, k);
    add(cur.hx, HALL, here.st, -1);
  }
  const StationDef &t = ST[tgt];
  add(t.hx, HALL, tgt, -1);
  for (int k = 0; k < t.n; k++) add(t.wp[k][0], t.wp[k][1], tgt, k);
  // drop a leading node equal to where we stand
  if (pathN > 0 && path[0].x == (int)fx && path[0].y == (int)fy) pathI = 1;
}

void Director::goTo(Station s, float spd) {
  speed = spd;
  if (walking) { pending = s; return; }
  if (here.st == s && here.wp == ST[s].n - 1) { station = s; return; }
  planPath(s);
  if (pathI >= pathN) { station = s; here = {ST[s].wp[ST[s].n - 1][0], ST[s].wp[ST[s].n - 1][1], (int8_t)s, (int8_t)(ST[s].n - 1)}; arrive(); return; }
  walking = true;
  station = s;
  pending = ST_COUNT;
}

void Director::stepWalk() {
  if (reactId >= 0) return;
  const Node &n = path[pathI];
  float dx = n.x - fx, dy = n.y - fy;
  float d = sqrtf(dx * dx + dy * dy);
  if (d <= speed) {
    fx = n.x; fy = n.y; here = n; pathI++;
    if (pending != ST_COUNT) { Station p = pending; pending = ST_COUNT; planPath(p); station = p; if (pathI >= pathN) { walking = false; arrive(); return; } }
    if (pathI >= pathN) { walking = false; arrive(); return; }
  } else {
    fx += dx / d * speed; fy += dy / d * speed;
    if (fabsf(dx) > fabsf(dy)) walkView = dx > 0 ? RIGHTV : LEFTV;
    else walkView = dy > 0 ? FRONT : BACK;
  }
  if (++legT >= 2) { legT = 0; legPhase = (legPhase + 1) & 3; }
}

void Director::arrive() {
  atT = 0; legPhase = 0;
  view = ST[station].view;
  if (task == TASK_IDLE && station == ST_DOOR) view = BACK;      // looking out of the door
  if ((task == TASK_SLEEP || task == TASK_OFFLINE) && station == ST_BED) { inBed = true; sleepNext = S(rng.range(15, 50)); }
  if (station == ST_DESK) steam = 1;
  if (fid != FID_NONE && isErrand(fid)) {
    if (errPhase == 0) { errPhase = 1; fidT = 0; }
    else if (errPhase == 2) { fid = FID_NONE; errPhase = 0; }
  }
}

// ---------------------------------------------------------------- fidgets
bool Director::fidgetAllowed(Fidget f) const {
  const FidDef &d = FID[f];
  if (!(d.mask & M(task))) return false;
  for (int k = 0; k < 4; k++) if (fidHist[k] == f) return false;
  if (f == FID_SIP && !atDesk()) return false;
  if (f == ERR_COFFEE && station == ST_DESK) return false;
  if (f == ERR_BOOK && station == ST_SHELF) return false;
  if (f == ERR_BOARD && station == ST_BOARD) return false;
  if (f == ERR_WINDOW && station == ST_WINDOW) return false;
  if (f == ERR_WATER && station == ST_PLANT) return false;
  if (f == FID_GLANCE && (view == FRONT)) return false;
  if (f == FID_LOOK_AROUND && view != FRONT) return false;
  if (f == FID_WAVE && view != FRONT) return false;
  if (isErrand(f) && in.energy > 0.85f) return false;
  return true;
}

void Director::startFidget(Fidget f) {
  fid = f; fidT = 0; fidLen = FID[f].len; errPhase = 0;
  for (int k = 3; k > 0; k--) fidHist[k] = fidHist[k - 1];
  fidHist[0] = (int8_t)f;
  if (f == FID_SIP) desk.mug = false;
  if (isErrand(f)) {
    Station s = ST_RUG;
    switch (f) {
      case ERR_WATER: s = ST_PLANT; break;
      case ERR_WINDOW: s = ST_WINDOW; break;
      case ERR_COFFEE: s = ST_DESK; break;
      case ERR_BOOK: s = ST_SHELF; break;
      case ERR_BOARD: s = ST_BOARD; break;
      case ERR_RELOCATE: {
        if (task == TASK_READ) { static const Station R[] = {ST_SHELF, ST_BED, ST_RUG}; do { s = R[rng.range(0, 2)]; } while (s == taskStation); }
        else { static const Station R[] = {ST_RUG, ST_WINDOW, ST_BOARD}; do { s = R[rng.range(0, 2)]; } while (s == taskStation); }
        taskStation = s;
        fid = FID_NONE;
        goTo(s, 1.5f);
        return;
      }
      default: break;
    }
    if (f == ERR_WINDOW || f == ERR_BOARD) fidLen = S(rng.range(3, 7));
    goTo(s, 1.6f);
  }
}

void Director::stepFidgets() {
  if (fid != FID_NONE) {
    if (isErrand(fid)) {
      if (errPhase == 1) {
        fidT++;
        if (fidT >= fidLen) {
          if (fid == ERR_COFFEE) { steam = 1; desk.mug = true; }
          errPhase = 2;
          goTo(taskStation, 1.5f);
          if (!walking) { fid = FID_NONE; errPhase = 0; }
        }
      }
      return;
    }
    fidT++;
    if (fidT >= fidLen) {
      if (fid == FID_SIP) { desk.mug = true; steam = 1; }
      if (fid == FID_YAWN && pending != ST_COUNT) { Station p = pending; pending = ST_COUNT; fid = FID_NONE; goTo(p, task == TASK_WAIT ? 2.6f : 1.5f); return; }
      fid = FID_NONE;
      nextFid = S(rng.range(6, 22)) * (in.energy > 0.5f ? 0.8f : 1.f);
    }
    return;
  }
  if (walking || reactId >= 0 || inBed) return;
  if (task == TASK_SLEEP || task == TASK_OFFLINE) return;
  if (--nextFid > 0) return;
  // weighted pick among the allowed fidgets that were not used recently
  int total = 0;
  for (int f = 0; f < FID_COUNT; f++) if (fidgetAllowed((Fidget)f)) total += FID[f].weight;
  if (total == 0) { nextFid = S(5); return; }
  int r = rng.range(0, total - 1);
  for (int f = 0; f < FID_COUNT; f++) {
    if (!fidgetAllowed((Fidget)f)) continue;
    r -= FID[f].weight;
    if (r < 0) { startFidget((Fidget)f); break; }
  }
  nextFid = S(rng.range(8, 26));
}

// ---------------------------------------------------------------- reactions
static const int REACT_LEN[REACT_COUNT] = {S(0.8f), S(1.0f), S(1.2f), S(1.2f), S(1.6f), S(2.6f), S(1.5f)};

void Director::react(React r) {
  if (inBed && r != REACT_SURPRISE) return;
  reactId = r; reactT = 0;
  if (fid != FID_NONE && !isErrand(fid)) fid = FID_NONE;
}

// ---------------------------------------------------------------- task loops
void Director::stepTaskLoop() {
  if (walking) return;
  atT++;
  switch (task) {
    case TASK_CODE: case TASK_WEB:
      if (fid == FID_NONE && reactId < 0) {
        if (typeBurst > 0) { typeBurst--; desk.t++; if (typeBurst == 0) typePause = rng.range(S(0.5f), S(2.5f)); }
        else if (typePause > 0) { typePause--; if (typePause == 0) typeBurst = rng.range(S(1.5f), S(6.5f)); }
        else typeBurst = rng.range(S(1.5f), S(5.f));
      }
      break;
    case TASK_REVIEW:
      if ((tick % 4) == 0) desk.t++;
      break;
    case TASK_READ:
      if (--pageT <= 0) { page++; pageT = S(rng.range(3, 8)); }
      break;
    case TASK_RUN:
      if (--runT <= 0) {
        if (runProg >= 16) { runProg = 0; runSeed = rng.next(); runT = S(rng.range(1, 3)); }
        else { runProg += rng.range(0, 2); if (runProg >= 16) { runProg = 16; runT = S(1.2f); } else runT = rng.range(S(0.3f), S(1.2f)); }
      }
      break;
    case TASK_SEARCH:
      if (--searchT <= 0) {
        searchT = S(rng.range(7, 14));
        if (atT > S(3)) {
          static const Station SP[] = {ST_UNDERDESK, ST_DRAWER, ST_SHELF};
          Station s;
          do { s = SP[rng.range(0, 2)]; } while (s == taskStation);
          taskStation = s;
          goTo(s, 1.6f);
        }
      }
      break;
    case TASK_THINK: case TASK_DELEGATE:
      if (++dotsT >= 8) { dotsT = 0; thinkDots = (thinkDots + 1) % 4; }
      break;
    case TASK_IDLE:
      idleT++;
      if (++idleFor <= 0) {}
      if (atT > idleFor) {
        Station s;
        int tries = 0;
        do { s = stationFor(TASK_IDLE); } while (s == taskStation && ++tries < 5);
        if (idleT > S(8 * 60) && rng.chance(60)) s = ST_BED;    // long idle: tends to sit on the bed and doze
        taskStation = s;
        idleFor = S(rng.range(20, 90));
        if (s != station) goTo(s, 1.3f);
        else atT = 0;
      }
      break;
    case TASK_WAIT:
      waitT++;
      if (waitVar) { if (--waitT <= 0) waitVar = 0; }
      else if (--waitNext <= 0) { waitVar = rng.range(1, 4); waitT = S(1.5f); waitNext = S(rng.range(4, 9)); }
      break;
    case TASK_SLEEP: case TASK_OFFLINE:
      if (--sleepNext <= 0) {
        int r = rng.range(0, 2);
        if (r == 0) sleepShift = sleepShift ? 0 : rng.range(-1, 1);
        else if (r == 1) sleepEyes = S(0.5f);
        sleepNext = S(rng.range(15, 50));
      }
      if (sleepEyes > 0) sleepEyes--;
      break;
    default: break;
  }
}

// ---------------------------------------------------------------- helpers
void Director::stepMinis() {
  int want = in.helpers > MAX_MINIS ? MAX_MINIS : in.helpers;
  if (task == TASK_SLEEP || task == TASK_OFFLINE) want = 0;
  auto slotFree = [&](int slot, int self) {
    if (ST[station].slot == slot || ST[taskStation].slot == slot) return false;
    for (int k = 0; k < MAX_MINIS; k++) if (k != self && minis[k].state && minis[k].st >= 0 && MST[minis[k].st].slot == slot) return false;
    return true;
  };
  auto planMini = [&](Mini &m, int st, bool fromDoor) {
    m.pathN = 0;
    auto add = [&](int x, int y) { if (m.pathN < 6) { m.path[m.pathN][0] = x; m.path[m.pathN][1] = y; m.pathN++; } };
    if (!fromDoor && m.st >= 0) { if (MST[m.st].vx >= 0) add(MST[m.st].vx, MST[m.st].vy); add(MST[m.st].hx, HALL); }
    if (fromDoor) add(56, HALL);
    if (st >= 0) { add(MST[st].hx, HALL); if (MST[st].vx >= 0) add(MST[st].vx, MST[st].vy); add(MST[st].x, MST[st].y); }
    else { add(56, HALL); add(56, 98); }
    m.pathI = 0; m.st = st;
  };
  auto pickStation = [&](int self) {
    int order[N_MST];
    for (int k = 0; k < N_MST; k++) order[k] = k;
    for (int k = N_MST - 1; k > 0; k--) { int j = rng.range(0, k); int t = order[k]; order[k] = order[j]; order[j] = t; }
    for (int k = 0; k < N_MST; k++) if (slotFree(MST[order[k]].slot, self)) return order[k];
    return -1;
  };
  for (int i = 0; i < MAX_MINIS; i++) {
    Mini &m = minis[i];
    if (i < want && m.state == 0) {
      int st = pickStation(i);
      if (st < 0) continue;
      m.state = 1; m.x = 56; m.y = 98; m.view = BACK;
      planMini(m, st, true);
    } else if (i >= want && (m.state == 1 || m.state == 2)) {
      m.state = 3;
      planMini(m, -1, false);
    } else if (m.state == 2 && m.st >= 0 && !slotFree(MST[m.st].slot, i)) {   // Clawd wants this spot: move over
      int st = pickStation(i);
      if (st >= 0 && st != m.st) { m.state = 1; planMini(m, st, false); }
    }
    if (m.state == 1 || m.state == 3) {
      if (m.pathI >= m.pathN) { if (m.state == 3) m.state = 0; else { m.state = 2; m.view = MST[m.st].view; m.legPhase = 0; } continue; }
      float tx = m.path[m.pathI][0], ty = m.path[m.pathI][1];
      float dx = tx - m.x, dy = ty - m.y, d = sqrtf(dx * dx + dy * dy);
      const float spd = 1.3f;
      if (d <= spd) { m.x = tx; m.y = ty; m.pathI++; }
      else { m.x += dx / d * spd; m.y += dy / d * spd; m.view = fabsf(dx) > fabsf(dy) ? (dx > 0 ? RIGHTV : LEFTV) : (dy > 0 ? FRONT : BACK); }
      if ((tick & 1) == 0) m.legPhase = (m.legPhase + 1) & 3;
    } else if (m.state == 2) {
      m.legPhase = ((tick / 8) + i) & 1;   // arms bob: working
      m.blink = ((tick + i * 17) % 60) < 2;
    }
  }
  shownHelpers = 0;
  for (int i = 0; i < MAX_MINIS; i++) if (minis[i].state) shownHelpers++;
}

// ---------------------------------------------------------------- ambient
void Director::stepAmbient() {
  float tod = in.tod;
  bool night = tod < 5.5f || tod >= 21.f;
  bool day = !night;
  win.mode = night ? SKY_NIGHT : SKY_DAY;
  win.moonPhase = ((in.yday * 8) / 30 + 4) % 8;
  if (day && (tick % 36) == 0) win.cloudX = (win.cloudX + 1) % 22;
  win.starT = (int)(tick % 100000);
  // rain
  if (rain) { if (--rainLeft <= 0) rain = false; }
  else if (day && tod >= 6.5f && tod < 20.5f && rng.range(0, 108000) == 0) { rain = true; rainLeft = S(60 * rng.range(3, 9)); }
  win.rain = rain && day; win.rainT = (int)(tick % 1000);
  // a bird on the sill
  if (win.bird >= 0) {
    birdT++;
    int total = S(5);
    if (birdT >= total) win.bird = -1;
    else if (birdT >= total - 6) win.bird = 8 + (birdT - (total - 6));
    else win.bird = ((birdT / 6) % 4 == 1) ? 1 : 0;
  } else if (day && !rain && rng.range(0, 18000) == 0) { win.bird = 0; birdT = 0; }
  // shooting star
  if (win.shootingStar) { if (++win.shootT > 6) win.shootingStar = false; }
  else if (night && rng.range(0, 8600) == 0) { win.shootingStar = true; win.shootT = 0; }
  // plant
  if (plantT > 0) { plantT--; plantSway = (plantT / 2) % 2 ? 1 : -1; if (plantT == 0) plantSway = 0; }
  else if (--plantNext <= 0) { plantT = 8; plantNext = S(rng.range(8, 25)); }
  // lamp & monitor
  bool awake = task != TASK_SLEEP && task != TASK_OFFLINE;
  lampOn = awake && (tod < 7.5f || tod >= 18.f || rain);
  bool deskUse = atDesk() && (task == TASK_CODE || task == TASK_WEB || task == TASK_REVIEW || fid == ERR_COFFEE);
  desk.lit = atDesk();
  if (deskUse) saverT = 0; else saverT++;
  if (task == TASK_OFFLINE) desk.screen = SCREEN_PLUG;
  else if (task == TASK_SLEEP) desk.screen = SCREEN_OFF;
  else if (deskUse && task == TASK_WEB) desk.screen = SCREEN_WEB;
  else if (deskUse) desk.screen = SCREEN_CODE;
  else desk.screen = saverT > S(20) ? SCREEN_SAVER : SCREEN_DIM;
  if (desk.screen == SCREEN_SAVER || desk.screen == SCREEN_WEB) desk.t++;
  if (steam > 0) { steam++; if (steam > S(14)) steam = 0; }
  desk.steam = atDesk() || task == TASK_IDLE ? steam : 0;
  desk.drawer = (task == TASK_SEARCH && station == ST_DRAWER && !walking);
  ledT++;
  emoteBob = (tick / 6) % 2;
  if (task == TASK_WAIT) alarmPhase = (tick / 6) % 2;
  breath = (tick / 15) % 2;
  if (--blinkT <= 0) { blinkLen = 2; blinkT = S(rng.range(2, 6)); }
  if (blinkLen > 0) blinkLen--;
}

// ---------------------------------------------------------------- lighting
static const struct { float h; uint32_t a, b; } SKYK[] = {
    {0.f, 0x1f2b55, 0x1f2b55}, {5.f, 0x1f2b55, 0x1f2b55}, {6.f, 0xd88a7a, 0xf5c9a0}, {7.5f, 0x9fd8f0, 0xd4f2fb},
    {17.5f, 0x9fd8f0, 0xd4f2fb}, {19.f, 0xe08a58, 0xf7c38a}, {20.5f, 0x4a3d7a, 0x7a5a90}, {21.5f, 0x1f2b55, 0x1f2b55},
    {24.f, 0x1f2b55, 0x1f2b55},
};
static uint32_t mixRgb(uint32_t a, uint32_t b, float t) {
  uint32_t o = 0;
  for (int s = 0; s < 24; s += 8) {
    float x = ((a >> s) & 0xFF) + (((b >> s) & 0xFF) - (float)((a >> s) & 0xFF)) * t;
    o |= (uint32_t)(x + 0.5f) << s;
  }
  return o;
}

void Director::stepLighting() {
  float tod = in.tod;
  bool asleep = task == TASK_SLEEP;
  const Light &nightish = asleep ? LIGHTS::NIGHT : LIGHTS::EVENING;
  Light base;
  if (tod < 5.5f) base = nightish;
  else if (tod < 7.f) base = mixLight(nightish, LIGHTS::DAWN, (tod - 5.5f) / 1.5f);
  else if (tod < 8.f) base = mixLight(LIGHTS::DAWN, LIGHTS::DAY, tod - 7.f);
  else if (tod < 17.5f) base = LIGHTS::DAY;
  else if (tod < 19.f) base = mixLight(LIGHTS::DAY, LIGHTS::DUSK, (tod - 17.5f) / 1.5f);
  else if (tod < 20.5f) base = mixLight(LIGHTS::DUSK, nightish, (tod - 19.f) / 1.5f);
  else base = nightish;
  if (rain && tod >= 6.5f && tod < 20.5f) base = mixLight(base, LIGHTS::OVERCAST, 0.5f);
  if (task == TASK_WAIT) base = LIGHTS::ALARM;
  if (task == TASK_OFFLINE) base = LIGHTS::OFFLINE;
  lightTarget = base;
  // sky
  int k = 0;
  while (k < 7 && tod >= SKYK[k + 1].h) k++;
  float t = (tod - SKYK[k].h) / (SKYK[k + 1].h - SKYK[k].h);
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  skyA = mixRgb(SKYK[k].a, SKYK[k + 1].a, t);
  skyB = mixRgb(SKYK[k].b, SKYK[k + 1].b, t);
  if (rain && win.mode == SKY_DAY) { skyA = mixRgb(skyA, 0x8a98a8, 0.8f); skyB = mixRgb(skyB, 0xb0bcc8, 0.8f); }
  if (task == TASK_OFFLINE) { skyA = mixRgb(skyA, 0x7a7a7a, 0.6f); skyB = mixRgb(skyB, 0x8a8a8a, 0.6f); }
}

const uint16_t *Director::palette(bool *dirty) {
  if (dirty) { *dirty = palDirty; palDirty = false; }
  return pal;
}

// ---------------------------------------------------------------- step
void Director::step() {
  tick++;
  in.tod += 1.f / (3600.f * FPS);
  if (in.tod >= 24.f) in.tod -= 24.f;
  if (dwell > 0 && --dwell == 0 && pendingTaskId != TASK_COUNT && !forced) chooseTask();
  if (reactId >= 0 && ++reactT >= REACT_LEN[reactId]) reactId = -1;
  if (walking) stepWalk();
  stepFidgets();
  stepTaskLoop();
  stepMinis();
  stepAmbient();
  stepLighting();
  // smooth the light toward its target; rebuild the palette only when it moved
  float k = (task == TASK_WAIT || lightTarget == LIGHTS::ALARM) ? 0.35f : 0.12f;
  float maxd = 0;
  Light nl = mixLight(light, lightTarget, k);
  for (int i = 0; i < 3; i++) { maxd = fmaxf(maxd, fabsf(lightTarget.mul[i] - light.mul[i])); maxd = fmaxf(maxd, fabsf(lightTarget.add[i] - light.add[i]) / 255.f); }
  maxd = fmaxf(maxd, fabsf(lightTarget.desat - light.desat));
  light = maxd < 0.004f ? lightTarget : nl;
  bool lchg = false;
  for (int i = 0; i < 3 && !lchg; i++) lchg = fabsf(light.mul[i] - builtLight.mul[i]) > 0.003f || fabsf(light.add[i] - builtLight.add[i]) > 0.6f;
  lchg = lchg || fabsf(light.desat - builtLight.desat) > 0.003f;
  bool schg = rgb565(skyA) != rgb565(builtSkyA) || rgb565(skyB) != rgb565(builtSkyB);
  if (lchg || schg) {
    buildPalette(light, skyA, skyB, pal);
    builtLight = light; builtSkyA = skyA; builtSkyB = skyB;
    palDirty = true;
  }
  cx = (int)(fx + 0.5f); cy = (int)(fy + 0.5f) - 11;
}

// ---------------------------------------------------------------- drawing
void Director::composePose(ClawdPose &p, int &emote, int &jump, int &shake) const {
  p.x = cx; p.y = cy; p.tint = task == TASK_OFFLINE ? TINT_GREY : TINT_CORAL;
  emote = -1; jump = 0; shake = 0;
  p.view = walking ? walkView : view;
  p.legPhase = walking ? legPhase : 0;
  p.arms = ARMS_DOWN;
  p.eyes = blinkLen > 0 ? EYES_BLINK : EYES_OPEN;
  bool settled = !walking && atT > 4;
  if (!walking) {
    switch (task) {
      case TASK_CODE: case TASK_WEB:
        if (station == ST_DESK) { p.arms = typeBurst > 0 ? (((tick / 3) & 1) ? ARMS_TYPE_A : ARMS_TYPE_B) : ARMS_DOWN; }
        if (settled && station == ST_DESK) { emote = task == TASK_WEB ? ICO_WEB : ICO_CODE; }
        break;
      case TASK_REVIEW: if (settled && station == ST_DESK) { emote = ICO_LOOK; } break;
      case TASK_READ: p.arms = ARMS_HOLD; if (settled) { emote = ICO_BOOK; } break;
      case TASK_RUN:
        if (station == ST_RACK) p.arms = ((tick % 36) < 3) ? ARMS_UP_L : ARMS_DOWN;
        if (settled && station == ST_RACK) { emote = ICO_PLAY; }
        break;
      case TASK_SEARCH:
        if (station == ST_SHELF) { p.view = FRONT; p.arms = ((tick / 4) & 1) ? ARMS_UP_L : ARMS_UP_R; }
        else if (station == ST_DRAWER) p.arms = ARMS_UP_R;
        else p.legPhase = ((tick / 4) & 1) ? 1 : 0;
        if (settled) { emote = ICO_LOOK; }
        break;
      case TASK_DELEGATE: if (shownHelpers) p.arms = ((tick / 6) & 1) ? ARMS_WAVE_A : ARMS_WAVE_B; break;
      case TASK_PLAN:
        if (station == ST_BOARD) p.arms = ((tick / 2) & 1) ? ARMS_UP_R : ARMS_UP;
        if (settled && station == ST_BOARD) { emote = ICO_PEN; }
        break;
      case TASK_WAIT:
        p.eyes = EYES_WIDE; p.arms = ARMS_UP; p.mouth = MOUTH_O;
        if (waitVar == 1) jump = ((waitT / 3) & 1) ? 2 : 0;
        else if (waitVar == 2) p.legPhase = ((waitT / 2) & 1) ? 1 : 0;
        else if (waitVar == 3) p.eyes = (waitT > S(0.75f)) ? EYES_LOOK_L : EYES_LOOK_R;
        else if (waitVar == 4) p.arms = ((waitT / 3) & 1) ? ARMS_WAVE_A : ARMS_WAVE_B;
        break;
      case TASK_IDLE:
        if (station == ST_BED) { p.legPhase = ((tick / 9) & 1) ? 1 : 0; if (idleT > S(8 * 60)) { p.eyes = EYES_SLEEP; p.mouth = MOUTH_NONE; } }
        if (station == ST_RUG && ((tick / 40) % 5) == 0) p.eyes = (tick / 40) % 2 ? EYES_LOOK_L : EYES_LOOK_R;
        break;
      default: break;
    }
  } else if (task == TASK_READ) {
    p.arms = ARMS_HOLD;
  }
  if (task == TASK_WAIT) { p.eyes = EYES_WIDE; p.mouth = MOUTH_O; }
  // fidgets
  if (fid != FID_NONE) {
    int t = fidT;
    if (isErrand(fid)) {
      if (errPhase == 1) {
        switch (fid) {
          case ERR_WATER: p.view = RIGHTV; p.arms = ARMS_UP_R; break;
          case ERR_WINDOW: p.view = BACK; p.arms = ARMS_DOWN; break;
          case ERR_COFFEE: p.view = FRONT; p.arms = ARMS_UP_R; break;
          case ERR_BOOK: p.view = FRONT; p.arms = ARMS_HOLD; emote = -1; break;
          case ERR_BOARD: p.view = BACK; p.arms = ((t / 12) & 1) ? ARMS_UP_R : ARMS_DOWN; break;
          default: break;
        }
      }
    } else {
      emote = -1;
      switch (fid) {
        case FID_GLANCE: p.view = FRONT; p.arms = ARMS_DOWN; break;
        case FID_STRETCH: p.arms = ARMS_UP; if (p.view == FRONT) p.mouth = MOUTH_O; jump = (t > 4 && t < 12) ? 1 : 0; break;
        case FID_SCRATCH: p.arms = ((t / 2) & 1) ? ARMS_UP_R : ARMS_DOWN; break;
        case FID_SIP: p.view = FRONT; p.arms = ARMS_UP_R; p.eyes = t > 6 && t < 16 ? EYES_BLINK : p.eyes; break;
        case FID_LOOK_AROUND: p.view = FRONT; p.eyes = t < fidLen / 2 ? EYES_LOOK_L : EYES_LOOK_R; break;
        case FID_PAUSE: p.arms = ARMS_DOWN; break;
        case FID_NOD: jump = ((t / 3) & 1); break;
        case FID_HOP: jump = t < 4 ? t : 8 - t; if (jump < 0) jump = 0; break;
        case FID_TAP_FOOT: p.legPhase = ((t / 2) & 1) ? 1 : 0; break;
        case FID_SHRUG: p.arms = ARMS_UP; p.view = FRONT; emote = ICO_QUESTION; break;
        case FID_SPIN: { static const View V[] = {FRONT, RIGHTV, BACK, LEFTV}; p.view = V[(t / 2) % 4]; break; }
        case FID_WAVE: p.view = FRONT; p.arms = ((t / 3) & 1) ? ARMS_WAVE_A : ARMS_WAVE_B; p.mouth = MOUTH_SMILE; break;
        case FID_YAWN: p.view = FRONT; p.mouth = MOUTH_O; p.eyes = EYES_BLINK; p.arms = (t > 5 && t < 14) ? ARMS_UP : ARMS_DOWN; break;
        default: break;
      }
    }
  }
  // reactions
  if (reactId >= 0) {
    int t = reactT;
    emote = -1;
    switch (reactId) {
      case REACT_GLANCE: p.view = FRONT; break;
      case REACT_HOP: p.view = FRONT; jump = t < 4 ? t : (t < 8 ? 8 - t : 0); emote = ICO_CHECK; break;
      case REACT_SURPRISE: p.view = FRONT; p.eyes = EYES_WIDE; p.mouth = MOUTH_O; p.arms = ARMS_UP; emote = ICO_BANG; break;
      case REACT_WINK: p.view = FRONT; p.eyes = EYES_WINK; p.mouth = MOUTH_SMILE; emote = ICO_HEART; break;
      case REACT_GIGGLE: p.view = FRONT; p.eyes = EYES_HAPPY; p.mouth = MOUTH_SMILE; p.blush = true; jump = ((t / 2) & 1); emote = ICO_NOTE; break;
      case REACT_HAPPY: { p.view = FRONT; p.eyes = EYES_HAPPY; p.mouth = MOUTH_SMILE; p.arms = ARMS_UP; int k = t % 10; jump = k < 5 ? k : 10 - k; if (t > 20) jump = 0; emote = ICO_STAR; break; }
      case REACT_WINCE: p.view = FRONT; p.eyes = EYES_WINCE; p.mouth = MOUTH_FLAT; shake = t < 8 ? ((t & 1) ? 1 : -1) : 0; emote = ICO_CROSS; break;
      default: break;
    }
  }
  p.x += shake; p.y -= jump;
}

// Every element comes from the sheets (sprites_gen.h, generated from firmware/assets/sprites by tools/sprites.py).
// Only the lighting passes that recolour what is drawn (sun / moon patch, lamp pool, door spot, under-desk beam,
// shadows) are code; which frame is shown, and where, is the director's.
namespace {
using spr::Sprite;
const Sprite *const *const NIGHT[8] = {spr::window_night_m0, spr::window_night_m1, spr::window_night_m2, spr::window_night_m3,
                                       spr::window_night_m4, spr::window_night_m5, spr::window_night_m6, spr::window_night_m7};
const Sprite *const *const BOARD[5] = {spr::whiteboard_1, spr::whiteboard_2, spr::whiteboard_3, spr::whiteboard_4, spr::whiteboard_5};
const Sprite *const *const CWALK[4] = {spr::clawd_front_walk, spr::clawd_back_walk, spr::clawd_left_walk, spr::clawd_right_walk};
const Sprite *const *const CARMS[4] = {spr::clawd_front_arms, spr::clawd_back_arms, spr::clawd_left_arms, spr::clawd_right_arms};
const Sprite *const *const MWALK[4] = {spr::mini_front_walk, spr::mini_back_walk, spr::mini_left_walk, spr::mini_right_walk};
// clawd_expr frames (firmware/assets/sprites/atlas.json, same order): arms, eyes (SLEEP folded into BLINK), mouth, legPhase, blush
struct Expr { int8_t a, e, m, leg; bool blush; };
const Expr EXPR[] = {
    {ARMS_UP, EYES_WIDE, MOUTH_O, 0, false},     {ARMS_UP, EYES_LOOK_L, MOUTH_O, 0, false},    {ARMS_UP, EYES_LOOK_R, MOUTH_O, 0, false},
    {ARMS_WAVE_A, EYES_WIDE, MOUTH_O, 0, false}, {ARMS_WAVE_B, EYES_WIDE, MOUTH_O, 0, false},  {ARMS_UP, EYES_WIDE, MOUTH_O, 1, false},
    {ARMS_DOWN, EYES_WINK, MOUTH_SMILE, 0, false}, {ARMS_DOWN, EYES_HAPPY, MOUTH_SMILE, 0, true}, {ARMS_UP, EYES_HAPPY, MOUTH_SMILE, 0, false},
    {ARMS_DOWN, EYES_WINCE, MOUTH_FLAT, 0, false}, {ARMS_UP, EYES_OPEN, MOUTH_O, 0, false},     {ARMS_DOWN, EYES_BLINK, MOUTH_O, 0, false},
    {ARMS_UP, EYES_BLINK, MOUTH_O, 0, false},    {ARMS_WAVE_A, EYES_OPEN, MOUTH_SMILE, 0, false}, {ARMS_WAVE_B, EYES_OPEN, MOUTH_SMILE, 0, false},
    {ARMS_UP_R, EYES_BLINK, MOUTH_NONE, 0, false}, {ARMS_DOWN, EYES_BLINK, MOUTH_NONE, 0, false}, {ARMS_DOWN, EYES_BLINK, MOUTH_NONE, 1, false},
};
static_assert(sizeof EXPR / sizeof *EXPR == spr::clawd_expr_n, "clawd_expr out of sync with the clawd sheet (atlas.json)");
// A grey / violet Clawd in a pose clawd_tint does not have: the coral sprite with its three body entries swapped.
struct TintMaps { Idx m[3][N_COL]; };
constexpr TintMaps makeTintMaps() {
  TintMaps t{};
  const Idx to[3][3] = {{C, CS, CH}, {GC, GCS, GCH}, {VC, VCS, VCH}};
  for (int k = 0; k < 3; k++) {
    for (int i = 0; i < N_COL; i++) t.m[k][i] = (Idx)i;
    for (int j = 0; j < 3; j++) t.m[k][to[0][j]] = to[k][j];
  }
  return t;
}
constexpr TintMaps TINT_MAPS = makeTintMaps();

// The sprite that draws exactly this (normalised) pose, or null.
const Sprite *clawdExact(int v, int a, int e, int m, int leg, bool b) {
  bool face0 = e == EYES_OPEN && m == MOUTH_NONE && !b;
  if (a == ARMS_DOWN && face0) return CWALK[v][leg];
  if (leg == 0 && face0) return CARMS[v][a];
  if (v == FRONT && leg == 0 && a == ARMS_DOWN && m == MOUTH_NONE && !b) return spr::clawd_eyes[e];
  if (v == FRONT && leg == 0 && a == ARMS_DOWN && e == EYES_OPEN && !b) return spr::clawd_mouth[m];
  if (v != FRONT && v != BACK && leg == 0 && a == ARMS_DOWN && e == EYES_BLINK) return spr::clawd_side_blink[v == LEFTV ? 0 : 1];
  if (v == FRONT)
    for (int i = 0; i < spr::clawd_expr_n; i++)
      if (EXPR[i].a == a && EXPR[i].e == e && EXPR[i].m == m && EXPR[i].leg == leg && EXPR[i].blush == b) return spr::clawd_expr[i];
  return nullptr;
}

// Clawd's pose -> the sheet sprite that draws it, or the closest one; *remap is set for a tint swap. The sheet
// has walk cycles, every arm pose standing, every eye / mouth alone and the director's usual face combinations
// (clawd_expr), not the product of all of them. Normalised first (what the pose draws: ARMS_HOLD draws as
// ARMS_DOWN, SLEEP as BLINK, the back view has no face, the side views only open / closed eyes). Then, when no
// frame matches exactly:
//   1. a blink (2 ticks) is dropped: the eyes stay open;
//   2. moving legs with arms down: the walk frame, face ignored; with arms raised the arms win and the legs stand;
//   3. standing: drop the blush, then the mouth; then raised arms win over the eyes (arms frame, eyes open);
//      arms down: the eyes frame (front) or the plain / blink frame (sides, back).
// Grey / violet: clawd_tint when the pose is its (front, standing, plain), else the coral choice tint-swapped.
const Sprite *clawdSprite(const ClawdPose &p, const Idx **remap) {
  int v = p.view & 3, e = p.eyes == EYES_SLEEP ? EYES_BLINK : p.eyes, m = p.mouth, leg = p.legPhase & 3;
  int a = p.arms == ARMS_HOLD ? ARMS_DOWN : p.arms;
  bool b = p.blush;
  if (v == BACK) { e = EYES_OPEN; m = MOUTH_NONE; b = false; }
  else if (v != FRONT) { m = MOUTH_NONE; b = false; e = e == EYES_BLINK ? EYES_BLINK : EYES_OPEN; }
  *remap = nullptr;
  if (p.tint != TINT_CORAL) {
    if (v == FRONT && leg == 0 && a == ARMS_DOWN && e == EYES_OPEN && m == MOUTH_NONE && !b) return spr::clawd_tint[p.tint];
    *remap = TINT_MAPS.m[p.tint];
  }
  if (const Sprite *s = clawdExact(v, a, e, m, leg, b)) return s;
  if (p.eyes == EYES_BLINK) {
    e = EYES_OPEN;
    if (const Sprite *s = clawdExact(v, a, e, m, leg, b)) return s;
  }
  if (leg) {
    if (a == ARMS_DOWN) return CWALK[v][leg];
    leg = 0;
    if (const Sprite *s = clawdExact(v, a, e, m, leg, b)) return s;
  }
  if (b) { b = false; if (const Sprite *s = clawdExact(v, a, e, m, 0, b)) return s; }
  if (m) { m = MOUTH_NONE; if (const Sprite *s = clawdExact(v, a, e, m, 0, b)) return s; }
  if (a != ARMS_DOWN) return CARMS[v][a];
  if (v == FRONT) return spr::clawd_eyes[e];
  return e == EYES_BLINK && v != BACK ? spr::clawd_side_blink[v == LEFTV ? 0 : 1] : CWALK[v][0];
}

// Redraw the canvas rect (x0..x1, y0..y1) from a fixed-anchor opaque sprite (floor, walls, background).
void copyRect(Canvas &c, const Sprite &s, int x0, int y0, int x1, int y1) { c.blitRect(0, 0, s, false, x0, y0, x1, y1); }
}  // namespace

void Director::draw(Canvas &c) {
  // walls, floor, rug, furniture at rest (screen dim, day window at cloudX 0, empty board, idle rack), plant sway 0
  if (!bgCache) {
    bgCache = (Idx *)malloc(W * H);
    Canvas bg{bgCache};
    bg.clear(OUT);
    bg.blit(0, 0, spr::background_0);
  }
  memcpy(c.buf, bgCache, W * H);
  // the plant is part of the painter pass below (it can stand in front of / behind Clawd): floor back under it
  copyRect(c, spr::floor_0, 100, 82, 113, 98);
  // lighting reads the floor; furniture has no floor colours, so it may run first
  float tod = in.tod;
  bool night = win.mode == SKY_NIGHT;
  if (!night && !rain && tod >= 7.f && tod < 19.5f) {
    int off = (int)lroundf((13.f - tod) * 2.6f);
    if (off > 14) off = 14;
    if (off < -14) off = -14;
    drawSunPatch(c, off, 20 + iabs(off) / 2, FL);
  } else if (night && task != TASK_OFFLINE) {
    float mt = tod >= 21.f ? tod - 24.f : tod;
    int off = (int)lroundf(mt * -2.5f);
    if (off > 10) off = 10;
    if (off < -10) off = -10;
    drawSunPatch(c, off, 16, MOONF);
  }
  if (lampOn) drawLampPool(c);
  if (task == TASK_WAIT) drawDoorSpot(c);
  // window: one full frame per sky state. The sheet has the shooting star over the full moon only (another moon
  // phase keeps its plain night frame: a moon changing shape for half a second reads as a glitch), and a bird
  // over the cloudX 0 sky only: elsewhere the bird sprite goes over the frame, clipped at the right curtain as
  // the window draws it (behind the curtain).
  {
    const Sprite *w = nullptr;
    int bird = win.bird;
    static const int8_t BIRDF[14] = {0, 1, -1, -1, -1, -1, -1, -1, 2, 3, 4, 5, 6, 7};
    if (win.mode == SKY_NIGHT) {
      int ph = win.moonPhase & 7, tw = (win.starT / 7) % 5;
      w = win.shootingStar && ph == 4 ? spr::window_shoot[imin(imax(win.shootT, 0), spr::window_shoot_n - 1)] : NIGHT[ph][tw];
    } else if (win.rain) {
      w = spr::window_rain[win.rainT % 40];
    } else if (bird >= 0 && bird < 14 && BIRDF[bird] >= 0 && win.cloudX % 22 == 0) {
      w = spr::window_bird[BIRDF[bird]];
      bird = -1;
    } else if (win.cloudX % 22 || bird >= 0) {
      w = spr::window_day[win.cloudX % 22];
    }
    if (w) {
      copyRect(c, spr::walls_0, 25, 15, 25, 15);   // the background's cloud pixel left of the curtain
      c.blit(0, 0, *w);
    }
    if (bird >= 0) {
      const Sprite &b = bird >= 8 ? *spr::bird_fly[imin(bird - 8, spr::bird_fly_n - 1)] : *spr::bird_sit[bird % 4 == 1 ? 1 : 0];
      c.blitRect(40, 23, b, false, 0, 0, 47, H - 1);
    }
  }
  if (in.todoTotal > 0) {
    int tot = in.todoTotal, n = tot > 5 ? 5 : tot;
    int dn = tot > 5 ? (in.todoDone * 5 + tot / 2) / tot : in.todoDone;
    dn = dn < 0 ? 0 : dn > n ? n : dn;
    c.blit(0, 0, *BOARD[n - 1][dn]);
  }
  if (shelfGap >= 0) c.blit(0, 0, *spr::bookshelf[shelfGap + 1]);
  // desk: full base (screen off) + overlays, unless it is the background's (dim, mug, nothing else)
  {
    const DeskState &d = desk;
    bool steam = d.mug && d.steam > 0;
    if (!(d.screen == SCREEN_DIM && d.mug && !steam && !d.drawer && !d.lit)) {
      const Sprite *scr = nullptr;
      switch (d.screen) {
        case SCREEN_DIM: scr = spr::desk_scr_dim[0]; break;
        case SCREEN_PLUG: scr = spr::desk_scr_plug[0]; break;
        case SCREEN_CODE: scr = spr::desk_scr_code[d.t % spr::desk_scr_code_n]; break;
        case SCREEN_WEB: scr = spr::desk_scr_web[(d.t / 8) % spr::desk_scr_web_n]; break;
        case SCREEN_SAVER: scr = spr::desk_scr_saver[d.t % spr::desk_scr_saver_n]; break;   // loops the sheet's frames
        default: break;
      }
      c.blit(0, 0, *spr::desk[SCREEN_OFF]);
      if (scr) c.blit(0, 0, *scr);
      if (steam) c.blit(0, 0, *spr::desk_steam[((d.steam / 3) & 1) | ((d.steam / 4) & 1) << 1 | ((d.steam / 5) & 1) << 2]);
      if (d.drawer) c.blit(0, 0, *spr::desk_drawer[0]);
      if (!d.mug) c.blit(0, 0, *spr::desk_nomug[0]);
      if (d.lit) c.blit(0, 0, *spr::desk_lit[0]);
      copyRect(c, spr::background_0, 101, 45, 101, 45);   // the rack's foot, drawn over the desk's edge
    }
  }
  // rack: the active LEDs loop the sheet's frames (one pattern for every run, whatever its seed)
  {
    bool active = task == TASK_RUN && station == ST_RACK && !walking;
    int prog = (task == TASK_RUN && !walking) ? runProg : -1;
    if (active) c.blit(0, 0, *spr::rack_active[(ledT / 2) % spr::rack_active_n]);
    else if ((ledT / 24) % spr::rack_idle_n) c.blit(0, 0, *spr::rack_idle[(ledT / 24) % spr::rack_idle_n]);
    if (prog >= 0) c.blit(0, 0, *spr::rack_progress[prog > 16 ? 16 : prog]);
  }
  if (inBed) c.blit(0, 0, *spr::bed_sleep[breath & 1]);
  if (lampOn) c.blit(0, 0, *spr::nightstand[1]);
  if (task == TASK_SEARCH && station == ST_UNDERDESK && !walking) {
    drawUnderDeskBeam(c, (int)tick);
    c.blit(88, 44, *spr::paper[0]);
    if ((tick / 4) % 3 == 0) c.blit(92, 43, *spr::sparkle[0]);
  }
  drawSprites(c);
  drawFx(c);
}

void Director::drawClawdNow(Canvas &c) {
  ClawdPose p; int emote, jump, shake;
  composePose(p, emote, jump, shake);
  const Idx *remap;
  const Sprite *s = clawdSprite(p, &remap);
  if (p.shadow) c.darken(p.x + 3, p.x + 14, p.y + 12);
  c.blit(p.x, p.y, *s, false, remap);
  if (p.arms == ARMS_HOLD) c.blit(p.x + 2, p.y + 6, *spr::book[page % spr::book_n]);
  if (fid == FID_SIP || (fid == ERR_COFFEE && errPhase == 1)) c.blit(p.x + 15, p.y - 2, *spr::mug_held[0]);
  if (fid == ERR_WATER && errPhase == 1) c.blit(p.x + 16, p.y + 3, *spr::can_pour[tick % spr::can_pour_n]);
  if (task == TASK_SEARCH && station == ST_UNDERDESK && !walking) c.blit(p.x + 17, p.y + 2, *spr::torch[0]);
  if (task == TASK_WAIT) {
    int lift = (tick / 6) % 2;
    c.blit(p.x - 3, p.y - 40, *spr::quest_bang[lift]);
  } else if (emote >= 0) {
    // above the head, except where that would cover the monitor / whiteboard: then beside him
    int ex = p.x + 3, ey = p.y - 13 - emoteBob; bool tailRight = false;
    if (station == ST_DESK || station == ST_RACK || station == ST_DRAWER) { ex = p.x - 13; ey = p.y - 7 - emoteBob; tailRight = true; }
    else if (station == ST_BOARD || station == ST_WINDOW) { ex = p.x + 19; ey = p.y - 4 - emoteBob; }
    else if (p.x > 95) { ex = p.x - 2; tailRight = true; }
    c.blit(ex, ey, *(tailRight ? spr::emote_r : spr::emote_l)[emote]);
  }
  if ((task == TASK_THINK) && !walking && fid == FID_NONE && reactId < 0 && atT > 4)
    c.blit(p.x + 10, p.y - 24, *spr::thought[thinkDots & 3]);
  if (reactId == REACT_HAPPY) {   // a ring of big sparkles and falling confetti (small sparkles)
    int t = reactT;
    for (int i = 0; i < 8; i++) {
      float a = i * 3.14159f / 4 + 0.4f;
      int rr = 17 + ((t / 3) % 2) * 4;
      static const uint8_t SC[4] = {0, 1, 2, 0};   // sparkle_big: QY, QW, SCR
      c.blit((int)(p.x + 9 + cosf(a) * rr), (int)(p.y + 6 + sinf(a) * rr * 0.75f), *spr::sparkle_big[SC[i % 4] % spr::sparkle_big_n]);
    }
    static const int8_t CF[6][2] = {{-8, -17}, {26, -14}, {-2, -22}, {20, -22}, {-12, -4}, {30, -2}};
    for (int i = 0; i < 6; i++) c.blit(p.x + CF[i][0], p.y + CF[i][1] + t / 2, *spr::sparkle[0]);
  }
  if (reactId == REACT_WINCE) c.blit(p.x + 17, p.y - 3, *spr::sweat[0]);
}

void Director::drawSprites(Canvas &c) {
  // painter's order by feet y: minis, Clawd, the plant
  struct D { int key, kind, idx; } d[MAX_MINIS + 2]; int n = 0;
  for (int i = 0; i < MAX_MINIS; i++) if (minis[i].state) d[n++] = {(int)minis[i].y, 1, i};
  if (!inBed) d[n++] = {cy + 11, 0, 0};
  d[n++] = {97, 2, 0};
  for (int i = 1; i < n; i++) { D t = d[i]; int j = i - 1; while (j >= 0 && d[j].key > t.key) { d[j + 1] = d[j]; j--; } d[j + 1] = t; }
  for (int i = 0; i < n; i++) {
    if (d[i].kind == 2) c.blit(0, 0, *spr::plant[imin(imax(plantSway, -1), 1) + 1]);
    else if (d[i].kind == 0) drawClawdNow(c);
    else {
      const Mini &m = minis[d[i].idx];
      int mx = (int)(m.x + 0.5f), my = (int)(m.y + 0.5f) - 8, leg = m.legPhase & 3;
      // the blink frames exist standing and mid-step (legPhase 0 / 1, what a working mini uses); walking, it is dropped
      const Sprite &s = m.blink && m.view == FRONT && leg < spr::mini_front_blink_n ? *spr::mini_front_blink[leg] : *MWALK[m.view & 3][leg];
      c.darken(mx + 2, mx + 9, my + 9);
      c.blit(mx, my, s);
      if (m.state == 2 && m.st == 1) c.blit(mx - 1, my + 4, *spr::book[0]);   // reading at the shelf
    }
  }
}

void Director::drawFx(Canvas &c) {
  if (inBed) {
    int sh = imin(imax(sleepShift, -1), 1);
    c.blit(0, 0, *(task == TASK_OFFLINE ? spr::clawd_bed_grey : spr::clawd_bed)[(sh + 1) * 2 + (sleepEyes > 0 ? 1 : 0)]);
    if (sleepEyes == 0) c.blit(24 + sleepShift, 56, *spr::zzz[(tick / 6) % spr::zzz_n]);
  }
  if (task == TASK_DELEGATE && !walking && reactId < 0) {
    // The dash runs from Clawd to each working helper, so its geometry is code; each dot is the first dot of the
    // dash sprite (its top-left pixel: the line runs down-right from it), so its colour comes from the sheet.
    const Sprite &dot = *spr::dash[0];
    for (int i = 0; i < MAX_MINIS; i++) {
      if (minis[i].state != 2) continue;
      int x0 = cx + 9, y0 = cy + 2, x1 = (int)minis[i].x + 5, y1 = (int)minis[i].y - 4;
      const int steps = 16;
      for (int k = 2; k < steps - 1; k++) {
        if ((k + (int)tick / 4) % 3 == 0) continue;
        int x = x0 + (x1 - x0) * k / steps, y = y0 + (y1 - y0) * k / steps;
        c.blitRect(x + dot.ax, y + dot.ay, dot, false, x, y, x, y);
      }
    }
  }
  if (task == TASK_WAIT) c.blit(0, 0, *spr::alarm_ring[alarmPhase & 1]);
}

const char *Director::activity() const {
  static char buf[32];
  if (reactId >= 0) { static const char *R[] = {"glance", "hop", "surprise", "wink", "giggle", "happy", "wince"}; snprintf(buf, sizeof buf, "react:%s", R[reactId]); return buf; }
  if (walking) { static const char *SN[] = {"desk", "shelf", "rack", "rug", "door", "bed", "window", "board", "plant", "underdesk", "drawer"}; snprintf(buf, sizeof buf, "walk>%s", SN[station]); return buf; }
  if (fid != FID_NONE) { snprintf(buf, sizeof buf, "%s:%s", isErrand(fid) ? "errand" : "fidget", FID[fid].name); return buf; }
  return TASK_NAMES[task];
}

}  // namespace room
