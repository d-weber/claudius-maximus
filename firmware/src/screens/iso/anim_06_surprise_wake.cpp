// 06 Surprise wake: port of iso-clawd/anims/06-surprise-wake.html.
#include "anim.h"
using namespace iso;

namespace {
// Clawd pose per step (JS: pose = clawd opts).
struct Pose { LegsPose legs; EyeShape eye; int gx; MouthKind mouth; ArmPose armL, armR; int squash; bool blush; };
enum Sticky { S_NONE, S_HEAD, S_FLY0, S_FLY1, S_FLY2, S_FLY3, S_FLY4, S_DESK };
// z, sticky, fx flags. scatter -1 = none (the JS uses `!= null`, so 0 counts). screen: code or off.
struct Step { Pose pose; int z; Sticky sticky; bool zz; int drool; bool code; bool ping; int scatter; int bang; bool dust; bool sweat; int wipe; };

constexpr Pose NAP = {LEGS_SIT, EYE_CLOSED, 0, MOUTH_NONE, ARM_DOWN, ARM_DOWN, 0, false};
constexpr Pose NAP_WIDE = {LEGS_SIT, EYE_WIDE, 0, MOUTH_O, ARM_DOWN, ARM_DOWN, 0, false};
constexpr Pose NAP_SQUASH = {LEGS_SIT, EYE_WIDE, 0, MOUTH_O, ARM_OUT, ARM_OUT, 3, false};
constexpr Pose SHEEPISH = {LEGS_STAND, EYE_HAPPY, 0, MOUTH_WOBBLY, ARM_OUT, ARM_NONE, 0, true};

// One-shot, 10 fps. 23 frames.
const Beat<Step> STEPS[] = {
    // napping: z's drift, drool drip
    {{NAP, 0, S_HEAD, true, 1, false, false, -1, 0, false, false, 0}, 2},
    {{NAP, 0, S_HEAD, true, 2, false, false, -1, 0, false, false, 0}, 1},
    // laptop pings: new session
    {{NAP, 0, S_HEAD, true, 2, true, true, -1, 0, false, false, 0}, 2},
    // eyes snap open (still seated), z's start to scatter
    {{NAP_WIDE, 0, S_HEAD, false, 2, true, false, 0, 0, false, false, 0}, 1},
    // anticipation squash
    {{NAP_SQUASH, 0, S_HEAD, false, 2, true, false, 1, 0, false, false, 0}, 1},
    // launch!
    {{{LEGS_STAND, EYE_WIDE, 0, MOUTH_O, ARM_UP, ARM_UP, -4, false}, 6, S_FLY0, false, 0, true, false, 2, 1, false, false, 0}, 1},
    {{{LEGS_STAND, EYE_WIDE, 0, MOUTH_O, ARM_UP, ARM_UP, -3, false}, 11, S_FLY1, false, 0, true, false, 3, 2, false, false, 0}, 1},
    {{{LEGS_STAND, EYE_WIDE, 0, MOUTH_O, ARM_UP, ARM_UP, 0, false}, 13, S_FLY2, false, 0, true, false, -1, 2, false, false, 0}, 2},
    {{{LEGS_STAND, EYE_WIDE, 0, MOUTH_O, ARM_HIGH, ARM_HIGH, 0, false}, 7, S_FLY3, false, 0, true, false, -1, 2, false, false, 0}, 1},
    // land
    {{{LEGS_STAND, EYE_WINCE, 0, MOUTH_FLAT, ARM_OUT, ARM_OUT, 5, false}, 0, S_FLY4, false, 0, true, false, -1, 2, true, false, 0}, 1},
    {{{LEGS_STAND, EYE_OPEN, 0, MOUTH_NONE, ARM_OUT, ARM_OUT, 2, false}, 0, S_DESK, false, 2, true, false, -1, 0, false, false, 0}, 1},
    // sheepish: wipe the drool, pretend nothing happened
    {{SHEEPISH, 0, S_DESK, false, 2, true, false, -1, 0, false, true, 1}, 1},
    {{SHEEPISH, 0, S_DESK, false, 0, true, false, -1, 0, false, true, 2}, 1},
    {{SHEEPISH, 0, S_DESK, false, 0, true, false, -1, 0, false, true, 3}, 1},
    // straighten up, glance at the screen as if ready all along
    {{{LEGS_STAND, EYE_OPEN, -1, MOUTH_FLAT, ARM_OUT, ARM_OUT, -2, false}, 0, S_DESK, false, 0, true, false, -1, 0, false, false, 0}, 2},
    {{{LEGS_STAND, EYE_BLINK, 0, MOUTH_NONE, ARM_OUT, ARM_OUT, 0, false}, 0, S_DESK, false, 0, true, false, -1, 0, false, false, 0}, 1},
    {{{LEGS_STAND, EYE_OPEN, 0, MOUTH_NONE, ARM_OUT, ARM_OUT, 0, false}, 0, S_DESK, false, 0, true, false, -1, 0, false, false, 0}, 3},
};

// Sticky-note flight path relative to its start on Clawd's head: {dx, dy, plane}. Tumbles via plane swaps.
struct Fly { int dx, dy; char plane; };   // plane 't' (top) | 'R' | 'L'
const Fly FLY[5] = {{6, -12, 't'}, {16, -24, 'R'}, {26, -30, 'L'}, {34, -24, 't'}, {42, 22, 'R'}};
const Pt STICKY_DESK = {164, 90};
// Seated head-top anchor (JS: drawClawd(NAP).top, computed once off-screen). Full spec, sitting (zb = 1),
// no squash/shake/lift: ox = 120 - (16 - 6) = 110, oy = 76 - 11 = 65, head = w2s(ox, oy, 8, 3, 29) = (120, 47).
const Pt HEAD0 = {120, 47};

// Big 5x5 sticky note (the lib one is 3x3, too small to read on his head). Flat, or upright while tumbling.
void note(Ctx &c, int x, int y, char plane) {
  if (plane == 't') {
    BoxRes r = box(c, x, y, 0, 5, 5, 0, {C::amber, NONE, NONE, NONE});
    r.top.cell(1, 1, C::amberDark); r.top.cell(2, 1, C::amberDark); r.top.cell(1, 3, C::amberDark); r.top.cell(3, 2, C::amberDark);
    return;
  }
  FacePainter p = iso::plane(c, x, y + 5, 0, 5, 9, plane, C::amber);
  p.rect(2, 2, 6, 1, C::amberDark); p.rect(2, 5, 4, 1, C::amberDark);
}
void drawSticky(Ctx &c, Sticky s, Pt head) {
  if (s == S_NONE) return;
  if (s == S_HEAD) { note(c, head.x, head.y, 't'); return; }
  if (s == S_DESK) { note(c, STICKY_DESK.x, STICKY_DESK.y, 't'); return; }
  const Fly &f = FLY[s - S_FLY0];
  // fly path is relative to the seated head position (fixed), so it doesn't follow Clawd's jump
  note(c, HEAD0.x + f.dx, HEAD0.y + f.dy, f.plane);
}

void draw(Ctx &c, int i, const AnimEnv &env) {
  const Step &s = seq(STEPS, i);
  const Pal &T = env.tint();
  drawDesk(c);
  // laptop at the left spot, screen toward Clawd's side (facing right), behind/beside him
  LaptopOpts lo;
  lo.facing = RIGHT; lo.screen = s.code ? SCR_CODE : SCR_OFF; lo.frame = i; lo.glow = s.code;
  laptop(c, SPOT_left.x - 8, SPOT_left.y - 2, lo);
  if (s.ping) {
    static const int sz[] = {1, 2};
    fx::ticks(c, SPOT_left.x - 8, SPOT_left.y - 24, "up", C::amber);
    fx::sparkle(c, SPOT_left.x + 2, SPOT_left.y - 20, cycle(sz, i), C::amber);
  }

  ClawdOpts o;
  o.x = SPOT_clawd.x; o.y = SPOT_clawd.y; o.z = s.z;
  o.tint = T;
  o.legs = s.pose.legs;
  o.eyes = {s.pose.eye, s.pose.gx, 0};
  o.mouth = s.pose.mouth;
  o.armL = s.pose.armL; o.armR = s.pose.armR;
  o.squash = s.pose.squash;
  o.blush = s.pose.blush;
  ClawdRes a = drawClawd(c, o);
  mug(c, SPOT_mug.x, SPOT_mug.y, {i});   // the mug spot is in front of him

  // drool drip at the mouth corner
  if (s.drool) {
    const FacePainter &fp = a.F;
    fp.rect(19, 19, 2, 2, C::blueLight);
    fp.rect(19, 21, 2, s.drool == 2 ? 3 : 1, C::blueLight);
    if (s.drool == 2) fp.rect(19, 24, 2, 1, C::blue);
  }
  // wipe: his (screen-right) hand comes up and swipes across the mouth corner, right to left
  if (s.wipe) {
    static const int U[] = {0, 22, 17, 12};
    int u = U[s.wipe];
    a.F.rect(u - 1, 17, 8, 7, T.right);
    a.F.rect(u, 18, 6, 4, T.top);
    if (s.wipe > 1) fx::ticks(c, a.mouth.x + 14, a.mouth.y + 4, "right", C::muted);
  }

  drawSticky(c, s.sticky, a.top);

  // z's: lazy drift while napping
  if (s.zz) {
    int ph = i % 6;
    int hx = a.head.x + 14, hy = a.head.y - 2;
    fx::zee(c, hx + (ph >> 1), hy - ph, 1, C::muted);
    fx::zee(c, hx + 6 + (ph >> 1), hy - 9 - ph, 2, C::muted);
    fx::zee(c, hx + 12 + (ph >> 1), hy - 19 - ph, 3, C::dim);
  }
  // z's scatter outward when startled
  if (s.scatter >= 0) {
    int k = s.scatter, hx = HEAD0.x + 14, hy = HEAD0.y - 2;
    Col col = k >= 2 ? C::dim : C::muted;
    fx::zee(c, hx + 6 + k * 6, hy - 2 + k * 2, 1, col);
    fx::zee(c, hx + 10 + k * 7, hy - 12 - k * 4, 2, col);
    if (k < 3) fx::zee(c, hx - 30 - k * 8, hy - 16 - k * 3, k < 2 ? 3 : 2, col);
  }
  if (s.bang) fx::bang(c, a.head.x - 14, a.head.y - (s.bang == 1 ? 6 : 10), true);
  if (s.sweat) fx::sweat(c, a.head.x - 22, a.head.y + 1);
  if (s.dust) {
    fx::ticks(c, a.feet.x - 26, a.feet.y - 2, "left", C::muted);
    fx::ticks(c, a.feet.x + 26, a.feet.y - 2, "right", C::muted);
  }
}
}  // namespace

const IsoAnim anim_06_surprise_wake = {"06-surprise-wake", 10, seqLen(STEPS), false, false, draw};
