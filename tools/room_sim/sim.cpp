// Host simulator for Clawd's Studio (firmware/src/screens/room). Built and driven by tools/room_sim.py.
// Writes raw RGB888 frames (120x106) to <out>/frames.bin and an activity log to <out>/log.txt.
#include "room.h"
#include "director.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

using namespace room;

struct Ev { int t; const char *mood; char tool; int helpers; const char *react; };
// A scripted work session: a morning with several agents coming and going.
static const Ev DAY[] = {
    {0, "offline", 0, 0, nullptr},      {5, "sleep", 0, 0, nullptr},        {20, "idle", 0, 0, "surprise"},
    {40, "busy", 'r', 0, nullptr},      {70, "busy", 'e', 0, nullptr},      {130, "busy", 'x', 0, nullptr},
    {150, "busy", 'e', 0, "wince"},     {170, "busy", 'e', 0, "hop"},       {200, "busy", 's', 0, nullptr},
    {225, "busy", 't', 0, nullptr},     {250, "busy", 'a', 2, nullptr},     {300, "busy", 'e', 2, nullptr},
    {330, "waiting", 0, 2, "surprise"}, {360, "busy", 'x', 1, nullptr},     {380, "busy", 'e', 0, "happy"},
    {400, "idle", 0, 0, nullptr},       {520, "busy", 'w', 0, nullptr},     {560, "busy", 'p', 0, nullptr},
    {590, "busy", 'v', 0, nullptr},     {620, "busy", 'r', 0, nullptr},     {700, "idle", 0, 0, "happy"},
    {1300, "sleep", 0, 0, nullptr},     {1400, "busy", 'e', 0, "surprise"}, {1500, "busy", 'e', 3, nullptr},
    {1600, "waiting", 0, 3, nullptr},   {1630, "busy", 'e', 0, nullptr},    {1700, "idle", 0, 0, "happy"},
};

static Mood moodOf(const char *s) {
  if (!strcmp(s, "offline")) return MOOD_OFFLINE;
  if (!strcmp(s, "sleep")) return MOOD_SLEEP;
  if (!strcmp(s, "idle")) return MOOD_IDLE;
  if (!strcmp(s, "waiting")) return MOOD_WAITING;
  return MOOD_BUSY;
}
static React reactOf(const char *s) {
  static const char *R[] = {"glance", "hop", "surprise", "wink", "giggle", "happy", "wince"};
  for (int i = 0; i < 7; i++) if (!strcmp(R[i], s)) return (React)i;
  return REACT_GLANCE;
}

int main(int argc, char **argv) {
  const char *task = ""; const char *out = "."; float seconds = 30, tod = 10.f, todSpeed = 1.f; int every = 1; uint32_t seed = 1; bool day = false;
  int helpers = 0; int todo = 0, done = 0; const char *reactEvery = nullptr; float reactPeriod = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--task")) task = argv[++i];
    else if (!strcmp(argv[i], "--out")) out = argv[++i];
    else if (!strcmp(argv[i], "--seconds")) seconds = atof(argv[++i]);
    else if (!strcmp(argv[i], "--tod")) tod = atof(argv[++i]);
    else if (!strcmp(argv[i], "--todspeed")) todSpeed = atof(argv[++i]);
    else if (!strcmp(argv[i], "--every")) every = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--seed")) seed = (uint32_t)atol(argv[++i]);
    else if (!strcmp(argv[i], "--helpers")) helpers = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--todo")) { todo = atoi(argv[++i]); done = atoi(argv[++i]); }
    else if (!strcmp(argv[i], "--react")) { reactEvery = argv[++i]; reactPeriod = atof(argv[++i]); }
    else if (!strcmp(argv[i], "--day")) day = true;
  }
  std::vector<Idx> buf(W * H);
  Canvas c{buf.data()};
  Director d;
  d.init(seed, tod);
  Input in; in.tod = tod; in.helpers = helpers; in.todoTotal = todo; in.todoDone = done;
  if (*task) { d.force(task); in = d.in; in.helpers = helpers; in.todoTotal = todo; in.todoDone = done; d.setInput(in); }
  std::string fb = std::string(out) + "/frames.bin", lg = std::string(out) + "/log.txt";
  FILE *ff = fopen(fb.c_str(), "wb"), *fl = fopen(lg.c_str(), "w");
  if (!ff || !fl) { fprintf(stderr, "cannot write to %s\n", out); return 1; }
  int frames = (int)(seconds * FPS);
  std::string last;
  int evi = 0; const int NEV = sizeof(DAY) / sizeof(DAY[0]);
  int nextReact = reactPeriod > 0 ? (int)(reactPeriod * FPS) : -1;
  std::vector<uint8_t> rgb(W * H * 3);
  int palChanges = 0;
  for (int f = 0; f < frames; f++) {
    float t = f / (float)FPS;
    if (day) {
      while (evi < NEV && DAY[evi].t <= t) {
        in.mood = moodOf(DAY[evi].mood); in.tool = DAY[evi].tool; in.helpers = DAY[evi].helpers;
        in.energy = in.mood == MOOD_BUSY ? 0.5f : 0.1f;
        d.setInput(in);
        if (DAY[evi].react) d.react(reactOf(DAY[evi].react));
        fprintf(fl, "%8.2f  INPUT mood=%s tool=%c helpers=%d%s%s\n", t, DAY[evi].mood, DAY[evi].tool ? DAY[evi].tool : '-', DAY[evi].helpers,
                DAY[evi].react ? " react=" : "", DAY[evi].react ? DAY[evi].react : "");
        evi++;
      }
    }
    if (nextReact > 0 && f % nextReact == 0 && f > 0) { d.react(reactOf(reactEvery)); }
    if (todSpeed != 1.f) { in.tod = tod + t * todSpeed / 3600.f; while (in.tod >= 24) in.tod -= 24; if (!*task) d.setInput(in); else d.in.tod = in.tod; }
    d.step();
    std::string a = d.activity();
    if (a != last) { fprintf(fl, "%8.2f  %s  (tod %.2f)\n", t, a.c_str(), d.in.tod); last = a; }
    bool dirty = false;
    const uint16_t *pal = d.palette(&dirty);
    if (dirty) palChanges++;
    if (f % every == 0 || f == frames - 1) {
      d.draw(c);
      for (int i = 0; i < W * H; i++) {
        uint16_t v = pal[buf[i]];
        rgb[i * 3] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
        rgb[i * 3 + 1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
        rgb[i * 3 + 2] = (uint8_t)((v & 31) * 255 / 31);
      }
      fwrite(rgb.data(), 1, rgb.size(), ff);
    }
  }
  fprintf(fl, "# frames=%d written=%d palette_rebuilds=%d\n", frames, (frames + every - 1) / every, palChanges);
  fclose(ff); fclose(fl);
  printf("ok %d frames, %d palette rebuilds\n", frames, palChanges);
  return 0;
}
