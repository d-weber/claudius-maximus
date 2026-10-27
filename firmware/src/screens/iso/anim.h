#pragma once
// One isometric animation = one file (iso/anim_NN_name.cpp), ported 1:1 from iso-clawd/anims/NN-name.html.
// draw(c, i, env) must be a pure function of the frame index i (and env). Loops wrap; one-shots end on the
// neutral pose and the director returns to the mood loop.
#include "iso.h"

struct AnimEnv {
  bool grey = false;        // host offline: Clawd is grey
  int helpers = 0;          // active subagents (the director draws mini helpers unless the anim does)
  char toolCat = 0;         // 'r' read, 'e' edit, 'x' exec, 'w' web, 't' think, 'a' delegate, 'p' plan, 0 none
  float energy = 0;         // 0..1 busyness
  const iso::Pal &tint() const { return grey ? iso::SHADE::grey : iso::SHADE::orange; }
};

struct IsoAnim {
  const char *name;
  int fps, frames;
  bool loop;
  bool drawsHelpers;        // true: the animation places the mini helpers itself (17-delegating)
  void (*draw)(iso::Ctx &c, int i, const AnimEnv &env);
  // Loops only: a "calm segment" [calmFrom, calmTo] that loops naturally on its own (breathing, blinking,
  // typing...). After a full pass the director rests in it for a random while before the next full pass, so a
  // screen watched for minutes does not replay the same 3 s gag over and over. -1 = none (loop continuously).
  int calmFrom = -1, calmTo = -1;
};

extern const IsoAnim anim_01_idle, anim_02_sleep, anim_03_waiting, anim_04_happy_done, anim_05_wince_error,
    anim_06_surprise_wake, anim_07_giggle_tap, anim_08_wink, anim_09_hop_todo, anim_10_reading_doc,
    anim_11_writing_code, anim_12_creating_plan, anim_13_reviewing_code, anim_14_running_command,
    anim_15_web_search, anim_16_thinking, anim_17_delegating;
