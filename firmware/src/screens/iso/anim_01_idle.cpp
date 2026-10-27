// 01 Idle: port of iso-clawd/anims/01-idle.html (reference for the other ports).
#include "anim.h"
#include <math.h>
using namespace iso;

namespace {
const Pt MUG = SPOT_mug;

struct Pose { EyeShape eye; int g, gy, lean, sq; MouthKind mouth; bool tap; int sniff; };
constexpr int NOSQ = -99;
// Beats: [pose, holdFrames]. 40 frames = 4.0 s @ 10 fps.
const Beat<Pose> TL[] = {
    {{EYE_OPEN, 0, 0, 0, NOSQ, MOUTH_NONE, false, 0}, 7},
    {{EYE_BLINK, 0, 0, 0, NOSQ, MOUTH_NONE, false, 0}, 1},
    {{EYE_OPEN, 0, 0, 0, NOSQ, MOUTH_NONE, false, 0}, 2},
    {{EYE_OPEN, -1, 0, 0, NOSQ, MOUTH_NONE, false, 0}, 5},   // glance away left
    {{EYE_OPEN, 1, 0, 0, NOSQ, MOUTH_NONE, false, 0}, 3},    // ...back across: oh, the mug (front-right)
    {{EYE_OPEN, 1, 1, 1, NOSQ, MOUTH_NONE, false, 0}, 1},
    {{EYE_OPEN, 1, 1, 2, NOSQ, MOUTH_NONE, false, 1}, 2},
    {{EYE_CLOSED, 0, 0, 2, 0, MOUTH_O, false, 2}, 2},
    {{EYE_HAPPY, 0, 0, 2, 2, MOUTH_SMILE, false, 0}, 1},
    {{EYE_HAPPY, 0, 0, 1, 1, MOUTH_SMILE, false, 0}, 2},
    {{EYE_HAPPY, 0, 0, 0, 0, MOUTH_SMILE, false, 0}, 3},
    {{EYE_OPEN, 0, 0, 0, NOSQ, MOUTH_SMILE, true, 0}, 6},
    {{EYE_BLINK, 0, 0, 0, NOSQ, MOUTH_NONE, false, 0}, 1},
    {{EYE_OPEN, 0, 0, 0, NOSQ, MOUTH_NONE, false, 0}, 1},
    {{EYE_BLINK, 0, 0, 0, NOSQ, MOUTH_NONE, false, 0}, 1},
    {{EYE_OPEN, 0, 0, 0, NOSQ, MOUTH_NONE, false, 0}, 2},
};
const Beat<int> BREATH[] = {{0, 5}, {1, 5}};   // 1 px squash, 10-frame cycle

// steam wisp drifting from the mug to Clawd's face while he sniffs
void sniffSteam(Ctx &c, int stage, const ClawdRes &a) {
  Pt top = {MUG.x, MUG.y - 12};
  Pt tgt = {a.eyeR.x + 4, a.eyeR.y + 6};
  int from = stage == 1 ? 0 : 3, to = stage == 1 ? 5 : 8;
  for (int k = from; k < to; k++) {
    float t = k / 8.f;
    int x = lerp(top.x, tgt.x, t);
    int y = lerp(top.y, tgt.y, t) - (int)lroundf(sinf(t * 3.14159265f) * 6);
    rect(c, x, y + (k & 1), 2, 1, k == to - 1 ? C::greyLight : C::muted);
  }
}

void draw(Ctx &c, int i, const AnimEnv &env) {
  drawDesk(c);
  mug(c, MUG.x, MUG.y, {i});
  const Pose &p = seq(TL, i);
  int breath = seq(BREATH, i);
  int sq = p.sq != NOSQ ? p.sq : breath;
  bool tapOn = p.tap && (i % 3 == 0);
  ClawdOpts o;
  o.x = SPOT_clawd.x; o.y = SPOT_clawd.y;
  o.tint = env.tint();
  o.eyes = {p.eye, p.g, p.gy};
  o.mouth = p.mouth;
  o.blush = true;
  o.armL = o.armR = ARM_OUT;
  o.legs = tapOn ? LEGS_TAP : LEGS_STAND;
  o.squash = sq;
  o.shake = p.lean;
  ClawdRes a = drawClawd(c, o);
  if (p.sniff) sniffSteam(c, p.sniff, a);
  if (i >= 23 && i <= 27) {   // a little heart floats up off the contented squash
    int k = i - 23;
    fx::heart(c, a.head.x + 14 + (k >> 1), a.head.y - 3 - k * 2, 1, k < 4 ? C::blush : C::orangeDark);
  }
  if (p.tap) {   // music note while he hums
    int k = (i - 29) / 2;
    static const char *const note[] = {"..##", "..#.#", "..#", "###", "###"};
    sprite(c, a.head.x + 20 + k, a.head.y - 2 - k * 2, note, ISO_MAP({'#', k > 1 ? C::muted : C::text}));
  }
}
}  // namespace

const IsoAnim anim_01_idle = {"01-idle", 10, seqLen(TL), true, false, draw, 0, 9};   // calm segment: see anim.h
