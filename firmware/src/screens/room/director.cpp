// The director: Clawd's behaviour in his studio. See director.h for the overview.
#include "director.h"
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
struct MiniDef { int8_t hx, x, y; View view; int8_t slot; };
static const MiniDef MST[] = {{70, 70, 47, BACK, 0}, {9, 9, 50, FRONT, 1}, {104, 104, 55, BACK, 2}, {60, 60, 42, BACK, 7}, {36, 36, 42, BACK, 6}};
static const int N_MST = 5;

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
  if (in.helpers > 3) in.helpers = 3;
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
  int want = in.helpers > 3 ? 3 : in.helpers;
  if (task == TASK_SLEEP || task == TASK_OFFLINE) want = 0;
  auto slotFree = [&](int slot, int self) {
    if (ST[station].slot == slot || ST[taskStation].slot == slot) return false;
    for (int k = 0; k < 3; k++) if (k != self && minis[k].state && minis[k].st >= 0 && MST[minis[k].st].slot == slot) return false;
    return true;
  };
  auto planMini = [&](Mini &m, int st, bool fromDoor) {
    m.pathN = 0;
    auto add = [&](int x, int y) { if (m.pathN < 6) { m.path[m.pathN][0] = x; m.path[m.pathN][1] = y; m.pathN++; } };
    if (!fromDoor && m.st >= 0) add(MST[m.st].hx, HALL);
    if (fromDoor) add(56, HALL);
    if (st >= 0) { add(MST[st].hx, HALL); add(MST[st].x, MST[st].y); }
    else { add(56, HALL); add(56, 98); }
    m.pathI = 0; m.st = st;
  };
  auto pickStation = [&](int self) {
    int order[N_MST] = {0, 1, 2, 3, 4};
    for (int k = N_MST - 1; k > 0; k--) { int j = rng.range(0, k); int t = order[k]; order[k] = order[j]; order[j] = t; }
    for (int k = 0; k < N_MST; k++) if (slotFree(MST[order[k]].slot, self)) return order[k];
    return -1;
  };
  for (int i = 0; i < 3; i++) {
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
  for (int i = 0; i < 3; i++) if (minis[i].state) shownHelpers++;
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
void Director::composePose(ClawdPose &p, int &emote, Idx &emoteCol, int &jump, int &shake) const {
  p.x = cx; p.y = cy; p.tint = task == TASK_OFFLINE ? TINT_GREY : TINT_CORAL;
  emote = -1; emoteCol = OUT; jump = 0; shake = 0;
  p.view = walking ? walkView : view;
  p.legPhase = walking ? legPhase : 0;
  p.arms = ARMS_DOWN;
  p.eyes = blinkLen > 0 ? EYES_BLINK : EYES_OPEN;
  bool settled = !walking && atT > 4;
  if (!walking) {
    switch (task) {
      case TASK_CODE: case TASK_WEB:
        if (station == ST_DESK) { p.arms = typeBurst > 0 ? (((tick / 3) & 1) ? ARMS_TYPE_A : ARMS_TYPE_B) : ARMS_DOWN; }
        if (settled && station == ST_DESK) { emote = task == TASK_WEB ? ICO_WEB : ICO_CODE; emoteCol = task == TASK_WEB ? RUG2 : SCRD; }
        break;
      case TASK_REVIEW: if (settled && station == ST_DESK) { emote = ICO_LOOK; emoteCol = RUG2; } break;
      case TASK_READ: p.arms = ARMS_HOLD; if (settled) { emote = ICO_BOOK; emoteCol = RED; } break;
      case TASK_RUN:
        if (station == ST_RACK) p.arms = ((tick % 36) < 3) ? ARMS_UP_L : ARMS_DOWN;
        if (settled && station == ST_RACK) { emote = ICO_PLAY; emoteCol = GRN2; }
        break;
      case TASK_SEARCH:
        if (station == ST_SHELF) { p.view = FRONT; p.arms = ((tick / 4) & 1) ? ARMS_UP_L : ARMS_UP_R; }
        else if (station == ST_DRAWER) p.arms = ARMS_UP_R;
        else p.legPhase = ((tick / 4) & 1) ? 1 : 0;
        if (settled) { emote = ICO_LOOK; emoteCol = RUG2; }
        break;
      case TASK_DELEGATE: if (shownHelpers) p.arms = ((tick / 6) & 1) ? ARMS_WAVE_A : ARMS_WAVE_B; break;
      case TASK_PLAN:
        if (station == ST_BOARD) p.arms = ((tick / 2) & 1) ? ARMS_UP_R : ARMS_UP;
        if (settled && station == ST_BOARD) { emote = ICO_PEN; emoteCol = PUR; }
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
        case FID_SHRUG: p.arms = ARMS_UP; p.view = FRONT; emote = ICO_QUESTION; emoteCol = RUG2; break;
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
      case REACT_HOP: p.view = FRONT; jump = t < 4 ? t : (t < 8 ? 8 - t : 0); emote = ICO_CHECK; emoteCol = GRN; break;
      case REACT_SURPRISE: p.view = FRONT; p.eyes = EYES_WIDE; p.mouth = MOUTH_O; p.arms = ARMS_UP; emote = ICO_BANG; emoteCol = QO; break;
      case REACT_WINK: p.view = FRONT; p.eyes = EYES_WINK; p.mouth = MOUTH_SMILE; emote = ICO_HEART; emoteCol = RED; break;
      case REACT_GIGGLE: p.view = FRONT; p.eyes = EYES_HAPPY; p.mouth = MOUTH_SMILE; p.blush = true; jump = ((t / 2) & 1); emote = ICO_NOTE; emoteCol = PUR; break;
      case REACT_HAPPY: { p.view = FRONT; p.eyes = EYES_HAPPY; p.mouth = MOUTH_SMILE; p.arms = ARMS_UP; int k = t % 10; jump = k < 5 ? k : 10 - k; if (t > 20) jump = 0; emote = ICO_STAR; emoteCol = QO; break; }
      case REACT_WINCE: p.view = FRONT; p.eyes = EYES_WINCE; p.mouth = MOUTH_FLAT; shake = t < 8 ? ((t & 1) ? 1 : -1) : 0; emote = ICO_CROSS; emoteCol = RED; break;
      default: break;
    }
  }
  p.x += shake; p.y -= jump;
}

void Director::drawClawdNow(Canvas &c) {
  ClawdPose p; int emote, jump, shake; Idx ecol;
  composePose(p, emote, ecol, jump, shake);
  drawClawd(c, p);
  // held props
  if (p.arms == ARMS_HOLD) drawBook(c, p.x + 2, p.y + 6, page);
  if (fid == FID_SIP || (fid == ERR_COFFEE && errPhase == 1)) drawMugHeld(c, p.x + 15, p.y - 2);
  if (fid == ERR_WATER && errPhase == 1) drawCan(c, p.x + 16, p.y + 3, true, (int)tick);
  if (task == TASK_SEARCH && station == ST_UNDERDESK && !walking) drawTorch(c, p.x + 17, p.y + 2);
  if (task == TASK_WAIT) {
    int lift = (tick / 6) % 2;
    drawQuestBang(c, p.x - 3, p.y - 40, lift);
  } else if (emote >= 0) {
    // above the head, except where that would cover the monitor / whiteboard: then beside him
    int ex = p.x + 3, ey = p.y - 13 - emoteBob; bool tailRight = false;
    if (station == ST_DESK || station == ST_RACK || station == ST_DRAWER) { ex = p.x - 13; ey = p.y - 7 - emoteBob; tailRight = true; }
    else if (station == ST_BOARD || station == ST_WINDOW) { ex = p.x + 19; ey = p.y - 4 - emoteBob; }
    else if (p.x > 95) { ex = p.x - 2; tailRight = true; }
    drawEmote(c, ex, ey, (Icon)emote, ecol, tailRight);
  }
  if ((task == TASK_THINK) && !walking && fid == FID_NONE && reactId < 0 && atT > 4) {
    drawThought(c, p.x + 10, p.y - 24, thinkDots, p.x + 16, p.y - 4);
  }
  if (reactId == REACT_HAPPY) {
    int t = reactT;
    for (int i = 0; i < 8; i++) {
      float a = i * 3.14159f / 4 + 0.4f;
      int rr = 17 + ((t / 3) % 2) * 4;
      static const Idx SC[4] = {QY, QW, SCR, QY};
      drawSparkle(c, (int)(p.x + 9 + cosf(a) * rr), (int)(p.y + 6 + sinf(a) * rr * 0.75f), SC[i % 4], true);
    }
    static const int8_t CF[6][2] = {{-8, -17}, {26, -14}, {-2, -22}, {20, -22}, {-12, -4}, {30, -2}};
    static const Idx CC[6] = {RED, RUG, GRN, PUR, YEL, PINK};
    for (int i = 0; i < 6; i++) c.r(p.x + CF[i][0], p.y + CF[i][1] + t / 2, p.x + CF[i][0] + 1, p.y + CF[i][1] + t / 2, CC[i]);
  }
  if (reactId == REACT_WINCE) drawSweat(c, p.x + 17, p.y - 3);
}

void Director::drawSprites(Canvas &c) {
  // painter's order by feet y: minis, Clawd, the plant
  struct D { int key, kind, idx; } d[5]; int n = 0;
  for (int i = 0; i < 3; i++) if (minis[i].state) d[n++] = {(int)minis[i].y, 1, i};
  if (!inBed) d[n++] = {cy + 11, 0, 0};
  d[n++] = {97, 2, 0};
  for (int i = 1; i < n; i++) { D t = d[i]; int j = i - 1; while (j >= 0 && d[j].key > t.key) { d[j + 1] = d[j]; j--; } d[j + 1] = t; }
  for (int i = 0; i < n; i++) {
    if (d[i].kind == 2) drawPlant(c, plantSway);
    else if (d[i].kind == 0) drawClawdNow(c);
    else {
      const Mini &m = minis[d[i].idx];
      int mx = (int)(m.x + 0.5f), my = (int)(m.y + 0.5f) - 8;
      drawMini(c, mx, my, m.view, m.state == 2 ? m.legPhase : m.legPhase, TINT_VIOLET, m.blink);
      if (m.state == 2 && m.st == 1) { c.r(mx + 1, my + 4, mx + 10, my + 8, OUT); c.r(mx + 2, my + 5, mx + 5, my + 7, CRM); c.r(mx + 6, my + 5, mx + 9, my + 7, WHT); }
    }
  }
}

void Director::drawFx(Canvas &c) {
  if (inBed) {
    drawClawdInBed(c, sleepShift, sleepEyes > 0, task == TASK_OFFLINE ? TINT_GREY : TINT_CORAL);
    if (sleepEyes == 0) drawZzz(c, 24 + sleepShift, 56, (int)tick);
  }
  if (task == TASK_DELEGATE && !walking && reactId < 0) {
    for (int i = 0; i < 3; i++) if (minis[i].state == 2) drawDash(c, cx + 9, cy + 2, (int)minis[i].x + 5, (int)minis[i].y - 4, (int)tick, QY);
  }
  if (task == TASK_WAIT) drawAlarmRing(c, alarmPhase);
}

void Director::draw(Canvas &c) {
  c.clear(OUT);
  drawWalls(c);
  drawFloor(c);
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
  drawWindow(c, win);
  drawWhiteboard(c, in.todoTotal, in.todoDone);
  drawBookshelf(c, shelfGap);
  drawDesk(c, desk);
  drawRack(c, task == TASK_RUN && station == ST_RACK && !walking, ledT, (task == TASK_RUN && !walking) ? runProg : -1, runSeed);
  drawRug(c);
  drawBed(c, inBed, breath);
  drawNightstand(c, lampOn);
  if (task == TASK_SEARCH && station == ST_UNDERDESK && !walking) {
    drawUnderDeskBeam(c, (int)tick);
    drawPaper(c, 88, 44);
    if ((tick / 4) % 3 == 0) drawSparkle(c, 92, 43, QW);
  }
  drawSprites(c);
  drawFx(c);
}

const char *Director::activity() const {
  static char buf[32];
  if (reactId >= 0) { static const char *R[] = {"glance", "hop", "surprise", "wink", "giggle", "happy", "wince"}; snprintf(buf, sizeof buf, "react:%s", R[reactId]); return buf; }
  if (walking) { static const char *SN[] = {"desk", "shelf", "rack", "rug", "door", "bed", "window", "board", "plant", "underdesk", "drawer"}; snprintf(buf, sizeof buf, "walk>%s", SN[station]); return buf; }
  if (fid != FID_NONE) { snprintf(buf, sizeof buf, "%s:%s", isErrand(fid) ? "errand" : "fidget", FID[fid].name); return buf; }
  return TASK_NAMES[task];
}

}  // namespace room
