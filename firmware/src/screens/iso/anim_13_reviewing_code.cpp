// 13 Reviewing code: port of iso-clawd/anims/13-reviewing-code.html (the 240 x 121 page; the
// "-full" page is the 160 x 160 full-screen variant and does not fit the home face).
#include "anim.h"
#include <math.h>
using namespace iso;

namespace {
// ---------------------------------------------------------------- layout
const Pt MON = {76, 56};            // monitor, screen faces down-right (toward Clawd and the viewer)
const Pt CL = {128, 77};            // Clawd's feet (nudged right to free the printout spot)
const Pt PAPER = {100, 79};         // printed diff, flat on the desk, front-left of Clawd
const Pt STAMP_REST = {122, 98};    // the LGTM stamp waits here
const Pt MAG_REST = {170, 86};      // where the magnifier lands when he tosses it over his shoulder
const int STAMP_H = 16;             // base 4 + handle 8 + knob 4

// ---------------------------------------------------------------- timeline (10 fps, 40 frames)
enum Ph { SCANA, SCANB, OK, SPOT, SHOCK, CRAWL, GRAB, WIND, SLAM, LGTM, RESET };
const Beat<Ph> TL[] = {
    {SCANA, 4}, {SCANB, 4}, {OK, 4},   // 0-11  squint at the diff, a good line: green check + nod
    {SPOT, 2}, {SHOCK, 3},             // 12-16 a line flashes red, a bug pops out; huge eye goes wide, "!"
    {CRAWL, 7},                        // 17-23 bug climbs down the monitor onto the printout, lens tracks it
    {GRAB, 2}, {WIND, 3},              // 24-28 drop the glass, grab the stamp, wind up (stretch)
    {SLAM, 3},                         // 29-31 STAMP! squash, impact ticks, shake
    {LGTM, 5},                         // 32-36 lift: green LGTM + flat bug; happy, blush
    {RESET, 3},                        // 37-39 printout poofs (merged), glass back up to the eye
};
int startOf(Ph ph) {
  int f = 0;
  for (size_t k = 0; k < sizeof(TL) / sizeof(TL[0]); k++) { if (TL[k].v == ph) return f; f += TL[k].hold; }
  return 0;
}

// ---------------------------------------------------------------- helpers
struct Lens { int gx, gy; EyeShape shape; };

// Big magnifying glass held at the eye. (cx, cy) = lens centre. The magnified eye is drawn inside.
void bigLens(Ctx &c, int cx, int cy, bool hasHand, Pt hand, const Lens &eye) {
  const int r = 7;
  if (hasHand) {   // 2 px wooden handle from the rim to the hand
    int hx0 = cx - 5, hy0 = cy + 5;
    line(c, hx0, hy0, hand.x, hand.y, C::woodLeft);
    line(c, hx0 + 1, hy0, hand.x + 1, hand.y, C::woodRight);
  }
  // glass: sparse blue dither so the face shows through
  for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) {
    if (dx * dx + dy * dy <= (r - 1.5f) * (r - 1.5f) && ((dx + dy) & 3) == 0 && (dx & 1) == 0) px(c, cx + dx, cy + dy, C::blueLight);
  }
  {   // the HUGE eye
    int ex = cx + eye.gx, ey = cy + eye.gy;
    if (eye.shape == EYE_HAPPY) {
      static const char *const arc[] = {"..####..", ".#....#.", "#......#"};
      sprite(c, ex - 4, ey - 1, arc, ISO_MAP({'#', C::eye}));
      sprite(c, ex - 4, ey, arc, ISO_MAP({'#', C::eye}));
    } else if (eye.shape == EYE_BLINK) {
      rect(c, ex - 3, ey, 7, 2, C::eye);
    } else if (eye.shape == EYE_WIDE) {
      rect(c, ex - 3, ey - 5, 7, 11, C::eye);
      rect(c, ex - 2, ey - 4, 2, 2, C::text); px(c, ex + 2, ey + 3, C::text);
    } else {
      rect(c, ex - 2, ey - 4, 5, 9, C::eye);
      rect(c, ex - 1, ey - 3, 2, 2, C::text);
    }
  }
  // rim, lit from the top-left
  for (int dy = -r - 1; dy <= r + 1; dy++) for (int dx = -r - 1; dx <= r + 1; dx++) {
    float d = sqrtf((float)(dx * dx + dy * dy));
    if (d > r - 1.2f && d <= r + 0.5f) px(c, cx + dx, cy + dy, (dx + dy < -2) ? C::text : (dx + dy > 3 ? C::greyDark : C::muted));
  }
  px(c, cx - 3, cy - 4, C::text); px(c, cx - 4, cy - 3, C::text); px(c, cx - 4, cy - 2, C::text);   // glare
}

// Rubber stamp. (x, y) = footprint centre on the desk, z = lift. Knob top sits at y - z - STAMP_H.
void stamp(Ctx &c, int x, int y, int z) {
  if (z > 0) tile(c, x, y, 3, 3, C::woodShadow);
  box(c, x, y, z, 4, 4, 4, SHADE::red);
  box(c, x, y, z + 4, 2, 2, 8, SHADE::wood);
  box(c, x, y, z + 12, 3, 3, 4, SHADE::red, true);
}
// Stamp hanging from a hand: footprint stays over the printout, the knob meets the hand.
void stampAtHand(Ctx &c, float hx, float hy) {   // JS: Math.round after the subtraction (half-pixel hands)
  int z = imax(0, jsRound(PAPER.y - hy - STAMP_H));
  stamp(c, jsRound(hx), PAPER.y, z);
}

// Tiny pixel bug, side view. dir 1 = walks right, -1 = left. step alternates the legs.
void bug(Ctx &c, int x, int y, int step, int dir) {
  static const char *const bodyA[] = {"......k", ".hhh.k.", "hrrrrkw", "rrrrrkk", ".k.k.k."};
  static const char *const bodyB[] = {"......k", ".hhh.k.", "hrrrrkw", "rrrrrkk", "k.k.k.."};
  sprite(c, x - 3, y - 4, step ? bodyA : bodyB, ISO_MAP({'h', SHADE::red.top}, {'r', C::red}, {'k', C::eye}, {'w', C::text}), dir < 0);
}
void bugSplat(Ctx &c, int x, int y) {
  static const char *const rows[] = {"k...r..k..", ".rrhrrrrkw", "k.r...k..k"};
  sprite(c, x - 5, y - 1, rows, ISO_MAP({'h', SHADE::red.top}, {'r', C::red}, {'k', C::eye}, {'w', C::text}));
}

// Printed diff: + / - gutters on a sheet, optional LGTM imprint.
void printout(Ctx &c, int x, int y, bool lgtm, bool splat) {
  BoxRes r = box(c, x, y, 0, 9, 7, 1, SHADE::paper);
  static const int rows[5][3] = {{1, 5, 0}, {2, 4, 1}, {3, 6, 0}, {4, 3, -1}, {5, 5, 0}};
  for (int k = 0; k < 5; k++) {
    int j = rows[k][0], n = rows[k][1], g = rows[k][2];
    Col col = g > 0 ? C::green : g < 0 ? C::red : C::muted;
    if (g) r.top.cell(1, j, col);
    for (int i = 2; i < 2 + n; i++) r.top.cell(i, j, col);
  }
  if (lgtm) {
    rect(c, x - 9, y - 5, 19, 9, C::paper);
    text(c, x - 7, y - 2, "LGTM", C::greenDark);
    rect(c, x - 9, y - 4, 19, 1, C::greenDark); rect(c, x - 9, y + 4, 19, 1, C::greenDark);
    rect(c, x - 9, y - 4, 1, 9, C::greenDark); rect(c, x + 9, y - 4, 1, 9, C::greenDark);
  }
  if (splat) bugSplat(c, x - 13, y + 1);
}

// Monitor content: a scrolling diff with + / - gutters. bad = row 2 highlighted red; fixed = all green.
struct DiffState { int scroll; bool bad, fixed; };
void diffScreen(const FacePainter &p, const Area &A, int, void *ud) {
  const DiffState &ds = *(const DiffState *)ud;
  struct Row { int ind, len; char g; };
  static const Row rows[8] = {{0, 7, '+'}, {2, 9, ' '}, {2, 5, '-'}, {4, 8, '+'}, {2, 6, ' '}, {0, 4, '+'}, {2, 9, '-'}, {4, 5, ' '}};
  for (int r = 0; r * 2 + 1 < A.h; r++) {
    const Row &q = rows[(r + ds.scroll) % 8];
    int row = 1 + r * 2;
    bool isBad = ds.bad && r == 2;
    if (isBad) p.rect(A.u, A.v + row - 1, A.w, 3, C::redDark);
    Col g = q.g == '+' ? C::green : q.g == '-' ? C::red : C::dim;
    if (ds.fixed && q.g == '-') g = C::green;
    p.rect(A.u + 1, A.v + row, 1, 1, g);
    p.rect(A.u + 3 + q.ind, A.v + row, imin(q.len, A.w - 4 - q.ind), 1, isBad ? C::text : (q.g == ' ' ? C::muted : g));
  }
}

void draw(Ctx &c, int i, const AnimEnv &env) {
  const int N = seqLen(TL);
  int im = ((i % N) + N) % N;
  Ph ph = seq(TL, im);
  int k = im - startOf(ph);
  drawDesk(c);
  mug(c, SPOT_mug.x, SPOT_mug.y, {i});

  // ---- monitor
  bool scanning = ph == SCANA || ph == SCANB;
  int scroll = scanning ? (i / 3) % 8 : 3;
  bool bad = (ph == SPOT || ph == SHOCK || ph == CRAWL || ph == GRAB || ph == WIND || ph == SLAM) && (ph != SPOT || k == 0);
  bool fixed = ph == LGTM || ph == RESET;
  DiffState ds = {scroll, bad, fixed};
  LaptopOpts mo; mo.facing = RIGHT; mo.screen = SCR_CUSTOM; mo.fn = diffScreen; mo.ud = &ds;
  LaptopRes m = monitor(c, MON.x, MON.y, mo);
  const FacePainter &scr = m.screen;
  const Area &A = m.area;

  // ---- printout (flat, so it goes under Clawd's feet) and the bug on it
  bool showPaper = !(ph == RESET && k < 2);
  if (showPaper) printout(c, PAPER.x, PAPER.y, ph == LGTM, ph == LGTM);

  bool hasBug = false; Pt bugPos = {0, 0}; int bugStep = i & 1, bugDir = 1; bool bugAlarm = false;
  Pt out = scr.at(A.u + 9, A.v + 5);
  if (ph == SHOCK) { hasBug = true; bugPos = {out.x, out.y + 1 - imin(k, 1)}; }
  if (ph == CRAWL) {
    const Pt path[7] = {{out.x + 1, out.y + 4}, {out.x + 2, out.y + 9}, {out.x + 2, out.y + 14}, {out.x + 4, out.y + 18},
                        {out.x + 8, out.y + 20}, {PAPER.x - 4, PAPER.y - 1}, {PAPER.x, PAPER.y}};
    hasBug = true; bugPos = path[imin(k, 6)]; bugDir = 1;
  }
  if (ph == GRAB || ph == WIND) { hasBug = true; bugPos = {PAPER.x, PAPER.y}; bugStep = 0; bugAlarm = ph == WIND; }

  // ---- Clawd's pose for this frame
  ClawdOpts o;
  o.x = CL.x; o.y = CL.y;
  o.tint = env.tint();
  bool hasLens = false; Lens lens = {0, 0, EYE_OPEN};
  bool stampHeld = false;
  bool fxCheck = false, fxBang = false, fxSweat = false, fxTicks = false, fxSpark = false, fxPoof = false;
  switch (ph) {
    case SCANA: case SCANB:
      o.eyes = {EYE_OPEN, 0, -1, EYE_DEFAULT, EYE_CLOSED}; o.armL = ARM_FORWARD; o.armR = ARM_OUT; o.mouth = MOUTH_FLAT;
      hasLens = true; lens = {ph == SCANA ? -2 : 0, k == 3 ? -1 : -2, (ph == SCANB && k == 2) ? EYE_BLINK : EYE_OPEN};
      break;
    case OK:
      o.eyes = {EYE_OPEN, 0, -1, EYE_DEFAULT, EYE_CLOSED}; o.armL = ARM_FORWARD; o.armR = ARM_OUT; o.mouth = MOUTH_SMILE;
      o.squash = k == 1 ? 2 : 0;
      hasLens = true; lens = {0, k == 1 ? 0 : -1, EYE_OPEN};
      fxCheck = true;
      break;
    case SPOT:
      o.eyes = {EYE_OPEN, 0, 0, EYE_DEFAULT, EYE_CLOSED}; o.armL = ARM_FORWARD; o.armR = ARM_OUT; o.mouth = MOUTH_FLAT;
      hasLens = true; lens = {-2, -2, EYE_OPEN};
      break;
    case SHOCK:
      o.eyes = {EYE_WIDE, 0, 0}; o.armL = ARM_FORWARD; o.armR = ARM_UP; o.mouth = MOUTH_O; o.squash = k == 0 ? -3 : 0;
      hasLens = true; lens = {-2, -2, EYE_WIDE};
      fxBang = true;
      break;
    case CRAWL:
      o.eyes = {EYE_WIDE, -1, imin(2, k - 1)}; o.armL = ARM_FORWARD; o.armR = ARM_OUT; o.mouth = MOUTH_WOBBLY;
      hasLens = true; lens = {-2 + (k >= 5 ? 1 : 0), imin(3, -2 + k), EYE_WIDE};
      if (k >= 3) fxSweat = true;
      break;
    case GRAB:
      o.eyes = {EYE_WINCE, 0, 0}; o.armL = ARM_HIGH; o.armR = ARM_OUT; o.mouth = MOUTH_FLAT;
      stampHeld = true;
      break;
    case WIND:
      o.eyes = {EYE_WINCE, 0, 0}; o.armL = ARM_UP; o.armR = ARM_UP; o.mouth = MOUTH_FLAT; o.squash = k == 2 ? -4 : -3;
      stampHeld = true;
      break;
    case SLAM: {
      static const int shakes[3] = {-1, 1, 0};
      o.eyes = {EYE_WINCE, 0, 0}; o.armL = ARM_FORWARD; o.armR = ARM_DOWN; o.mouth = MOUTH_LAUGH; o.squash = k == 0 ? 4 : 2;
      o.shake = shakes[imin(k, 2)];
      stampHeld = true;
      fxTicks = true;
      break;
    }
    case LGTM:
      o.eyes = {EYE_HAPPY, 0, 0}; o.armL = k == 0 ? ARM_HIGH : ARM_OUT; o.armR = ARM_HIGH; o.mouth = MOUTH_SMILE; o.blush = true;
      o.squash = k == 2 ? 1 : 0;
      fxSpark = true;
      break;
    case RESET:
      o.eyes = {EYE_OPEN, -1, 0}; o.armL = ARM_FORWARD; o.armR = ARM_OUT; o.mouth = MOUTH_NONE;
      if (k == 2) { hasLens = true; lens = {-1, -1, EYE_OPEN}; }
      fxPoof = true;
      break;
  }

  // the bug crawling down the monitor is in front of the monitor, behind Clawd
  if (hasBug) {
    bug(c, bugPos.x, bugPos.y, bugStep, bugDir);
    if (bugAlarm) { rect(c, bugPos.x, bugPos.y - 9, 1, 3, C::amber); px(c, bugPos.x, bugPos.y - 5, C::amber); }
  }

  ClawdRes a = drawClawd(c, o);

  // ---- front props
  if (stampHeld) stampAtHand(c, a.handLx, a.handLy);
  else stamp(c, STAMP_REST.x, STAMP_REST.y, 0);
  if (!hasLens) {
    // tossed glass: arcs over his head on 'grab', lies on the desk, gets picked up on 'reset'
    int mx = MAG_REST.x, my = MAG_REST.y;
    if (ph == GRAB && k == 0) { mx = 140; my = 26; }   // flung over his shoulder
    if (ph == GRAB && k == 1) { mx = 158; my = 60; }
    if (ph == WIND && k == 0) my -= 2;     // little bounce
    if (ph == RESET && k == 1) { mx = a.handL.x - 2; my = a.handL.y - 4; }
    magnifier(c, mx, my, false);
  }

  // ---- the magnifier at his eye (on top of the face)
  if (hasLens) bigLens(c, a.eyeL.x - 1, a.eyeL.y + 5, a.hasHandL, a.handL, lens);

  // ---- FX
  if (fxCheck) {
    Pt p = scr.at(A.u + A.w, A.v + 2);
    fx::check(c, p.x + 5, p.y - 4 - (k == 0 ? 1 : 0), C::green);
    if (k < 2) fx::sparkle(c, p.x + 11, p.y - 9, k + 1, C::green);
  }
  if (fxBang) fx::bang(c, a.head.x - 6, a.head.y - 9);
  if (fxSweat) fx::sweat(c, a.head.x + 14, a.head.y + 4 + (i & 1));
  if (fxTicks && k < 2) {
    fx::ticks(c, PAPER.x - 14, PAPER.y - 4, "left", C::amber);
    fx::ticks(c, PAPER.x + 14, PAPER.y - 4, "right", C::amber);
  }
  if (fxSpark) {
    static const int s1[5] = {1, 2, 3, 2, 1}, s2[5] = {0, 1, 2, 3, 2};
    fx::sparkle(c, PAPER.x - 14, PAPER.y - 8, s1[imin(k, 4)], C::green);
    fx::sparkle(c, a.head.x + 18, a.head.y - 4, s2[imin(k, 4)], C::amber);
  }
  if (fxPoof) fx::poof(c, PAPER.x, PAPER.y - 3, k + 1, C::muted);
}
}  // namespace

const IsoAnim anim_13_reviewing_code = {"13-reviewing-code", 10, seqLen(TL), true, false, draw, 0, 7};   // calm segment: see anim.h
