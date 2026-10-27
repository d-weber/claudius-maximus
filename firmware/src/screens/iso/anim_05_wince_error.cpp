// 05 Wince Error: port of iso-clawd/anims/05-wince-error.html (one-shot, 15 frames, ends on neutral).
#include "anim.h"
using namespace iso;

namespace {
// Laptop sits on the left of the desk with its screen toward the viewer, Clawd types on it.
constexpr Pt LAP = {SPOT_left.x + 2, SPOT_left.y + 2};

// One entry per frame (10 fps). dx = Clawd x offset (he hops back to the right), z = lift.
// scr: code | red | dead | boot. poof: smoke frame 0..3 or -1. cross: y offset of the red x or NOCROSS.
// spark: sparkle size or 0. sweat: drop slide step or NOSWEAT.
enum Scr { SCR_CODE_, SCR_RED, SCR_DEAD, SCR_BOOT };
constexpr int NOCROSS = -99, NOSWEAT = -1;
struct Frame { EyeShape eye; int gx, gy; MouthKind mouth; ArmPose al, ar; int dx, z, sq; LegsPose legs; int shake; bool blush;
               Scr scr; int poof, spark, cross; bool bonk, crossPop; int sweat; };
constexpr Frame F[] = {
    // 0-1 typing away, all fine
    {EYE_OPEN, -1, 1, MOUTH_NONE, ARM_TYPEA, ARM_TYPEB, 0, 0, 0, LEGS_STAND, 0, false, SCR_CODE_, -1, 0, NOCROSS, false, false, NOSWEAT},
    {EYE_OPEN, -1, 1, MOUTH_NONE, ARM_TYPEB, ARM_TYPEA, 0, 0, 0, LEGS_STAND, 0, false, SCR_CODE_, -1, 0, NOCROSS, false, false, NOSWEAT},
    // 2 ERROR: screen goes red, eyes pop wide (anticipation squash)
    {EYE_WIDE, -1, 0, MOUTH_O, ARM_OUT, ARM_OUT, 0, 0, 2, LEGS_STAND, 0, false, SCR_RED, -1, 1, NOCROSS, false, false, NOSWEAT},
    // 3-4 jump back "> <", laptop puffs smoke, a big red x falls from the sky
    {EYE_WINCE, 0, 0, MOUTH_WOBBLY, ARM_UP, ARM_UP, 4, 4, -3, LEGS_STEPA, 0, false, SCR_RED, 0, 3, -24, false, false, NOSWEAT},
    {EYE_WINCE, 0, 0, MOUTH_WOBBLY, ARM_HIGH, ARM_HIGH, 7, 5, -2, LEGS_STEPB, 0, false, SCR_DEAD, 1, 0, -18, false, false, NOSWEAT},
    // 5 land + BONK
    {EYE_WINCE, 0, 0, MOUTH_WOBBLY, ARM_DOWN, ARM_DOWN, 8, 0, 5, LEGS_STAND, 0, false, SCR_DEAD, 2, 0, 3, true, false, NOSWEAT},
    // 6-7 dizzy shake, x bounces off and hovers
    {EYE_WINCE, 0, 0, MOUTH_WOBBLY, ARM_DOWN, ARM_DOWN, 8, 0, 2, LEGS_STAND, 2, false, SCR_DEAD, 3, 0, -13, false, false, 0},
    {EYE_WINCE, 0, 0, MOUTH_WOBBLY, ARM_DOWN, ARM_DOWN, 8, 0, 0, LEGS_STAND, -2, false, SCR_DEAD, -1, 0, -11, false, false, 0},
    // 8-9 peek at the laptop, sheepish; the x pops away
    {EYE_OPEN, -1, 1, MOUTH_WOBBLY, ARM_DOWN, ARM_DOWN, 8, 0, 0, LEGS_STAND, 0, true, SCR_DEAD, -1, 0, NOCROSS, false, true, 1},
    {EYE_OPEN, -1, 1, MOUTH_WOBBLY, ARM_DOWN, ARM_DOWN, 8, 0, 0, LEGS_STAND, 0, true, SCR_BOOT, -1, 0, NOCROSS, false, false, 2},
    // 10-11 shrug: "oops, heh"
    {EYE_HAPPY, 0, 0, MOUTH_WOBBLY, ARM_HIGH, ARM_HIGH, 8, 0, -1, LEGS_STAND, 0, true, SCR_BOOT, -1, 0, NOCROSS, false, false, 3},
    {EYE_HAPPY, 0, 0, MOUTH_SMILE, ARM_HIGH, ARM_HIGH, 8, 0, 0, LEGS_STAND, 0, true, SCR_CODE_, -1, 0, NOCROSS, false, false, NOSWEAT},
    // 12-13 shuffle back to the spot
    {EYE_OPEN, 0, 0, MOUTH_NONE, ARM_OUT, ARM_OUT, 4, 0, 0, LEGS_STEPA, 0, false, SCR_CODE_, -1, 0, NOCROSS, false, false, NOSWEAT},
    {EYE_OPEN, 0, 0, MOUTH_NONE, ARM_OUT, ARM_OUT, 0, 0, 1, LEGS_STAND, 0, false, SCR_CODE_, -1, 0, NOCROSS, false, false, NOSWEAT},
    // 14 neutral
    {EYE_OPEN, 0, 0, MOUTH_NONE, ARM_OUT, ARM_OUT, 0, 0, 0, LEGS_STAND, 0, false, SCR_CODE_, -1, 0, NOCROSS, false, false, NOSWEAT},
};
constexpr int NFRAMES = sizeof(F) / sizeof(F[0]);

// Big red "x" with a dark outline so it still reads on top of Clawd's orange head.
const char *const XROWS[] = {"rr...rr", "rrr.rrr", ".rrrrr.", "..rrr..", ".rrrrr.", "rrr.rrr", "rr...rr"};
inline bool xOn(int r, int c) { return r >= 0 && r < 7 && c >= 0 && c < 7 && XROWS[r][c] == 'r'; }
void bigCross(Ctx &c, int cx, int cy) {
  int x0 = cx - 3, y0 = cy - 3;
  for (int r = -1; r <= 7; r++)
    for (int col = -1; col <= 7; col++) {
      if (xOn(r, col)) continue;
      if (xOn(r - 1, col) || xOn(r + 1, col) || xOn(r, col - 1) || xOn(r, col + 1)) px(c, x0 + col, y0 + r, C::redDark);
    }
  sprite(c, x0, y0, XROWS, ISO_MAP({'r', C::red}));
  px(c, x0, y0, C::orangeLight); px(c, x0 + 5, y0, C::orangeLight);   // tiny shine
}

// Screen fillers drawn through the laptop's face painter.
void redScreen(const FacePainter &p, const Area &A, int, void *) {
  p.rect(A.u, A.v, A.w, A.h, C::red);
  // chunky white x in the middle
  int cu = A.u + (A.w >> 1) - 3, cv = A.v + 1;
  static const char *const X[] = {"##....##", ".##..##.", "..####..", "..####..", ".##..##.", "##....##"};
  for (int r = 0; r < 6; r++) for (int col = 0; X[r][col]; col++) if (X[r][col] == '#') p.px(cu + col, cv + r, C::text);
}
void deadScreen(const FacePainter &p, const Area &A, int, void *) {
  p.rect(A.u, A.v, A.w, A.h, C::surface);
  p.rect(A.u + (A.w >> 1) - 1, A.v + (A.h >> 1), 2, 1, C::redDark);   // dying ember dot
}
void bootScreen(const FacePainter &p, const Area &A, int, void *) {
  p.rect(A.u, A.v, A.w, A.h, C::screen);
  p.rect(A.u + 1, A.v + 1, 2, 1, C::green);
  p.rect(A.u + 4, A.v + 1, 4, 1, C::muted);
}

void draw(Ctx &c, int i, const AnimEnv &env) {
  const Frame &f = F[i];
  drawDesk(c);

  // laptop (behind / beside Clawd)
  LaptopOpts lo;
  lo.facing = RIGHT;
  lo.frame = i < 2 ? i : i + 3;
  if (f.scr == SCR_CODE_) lo.screen = SCR_CODE;
  else { lo.screen = SCR_CUSTOM; lo.fn = f.scr == SCR_RED ? redScreen : f.scr == SCR_DEAD ? deadScreen : bootScreen; }
  LaptopRes lap = laptop(c, LAP.x, LAP.y, lo);
  // lid-top point for smoke / sparks
  Pt lidTop = lap.screen.at(lap.area.u + (lap.area.w >> 1), -1);

  // Clawd
  ClawdOpts o;
  o.x = SPOT_clawd.x + f.dx; o.y = SPOT_clawd.y + (f.dx >> 1); o.z = f.z;
  o.tint = env.tint();
  o.eyes = {f.eye, f.gx, f.gy};
  o.mouth = f.mouth;
  o.armL = f.al; o.armR = f.ar;
  o.legs = f.legs;
  o.squash = f.sq; o.shake = f.shake; o.blush = f.blush;
  ClawdRes a = drawClawd(c, o);
  mug(c, SPOT_mug.x, SPOT_mug.y, {i});   // the mug spot is in front of him

  // FX
  if (f.spark) {
    fx::sparkle(c, lidTop.x - 9, lidTop.y + 2, f.spark, C::amber);
    fx::sparkle(c, lidTop.x + 10, lidTop.y - 1, f.spark == 3 ? 2 : 1, C::amber);
  }
  if (f.poof >= 0) {
    // two puffs rising and drifting apart, dark grey once they thin out
    Col col = f.poof < 2 ? C::muted : C::dim;
    fx::poof(c, lidTop.x - 2, lidTop.y - 5 - f.poof * 3, imin(3, f.poof + 1), col);
    fx::poof(c, lidTop.x + 7 + f.poof, lidTop.y - 10 - f.poof * 3, f.poof, col);
  }
  int hx = a.head.x + 4;
  if (f.cross != NOCROSS) bigCross(c, hx, a.head.y + f.cross);
  if (f.bonk) {
    fx::ticks(c, hx - 9, a.head.y - 4, "left", C::text);
    fx::ticks(c, hx + 9, a.head.y - 4, "right", C::text);
  }
  if (f.crossPop) fx::poof(c, hx, a.head.y - 12, 3, C::red);
  if (f.sweat != NOSWEAT) {
    // on the face side (screen-left upper corner of the face)
    fx::sweat(c, a.face.x - 20, a.face.y - 11 + f.sweat * 2);
  }
}
}  // namespace

const IsoAnim anim_05_wince_error = {"05-wince-error", 10, NFRAMES, false, false, draw};
