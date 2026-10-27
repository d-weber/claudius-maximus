// 10 Reading a doc: port of iso-clawd/anims/10-reading-doc.html.
#include "anim.h"
#include <math.h>
#include <limits.h>
using namespace iso;

namespace {
// ---------------------------------------------------------------- scene layout
const Pt CL = {128, 72};     // Clawd feet (sitting, nudged back-right to make room)
const Pt BK = {102, 85};     // open book, front-left of Clawd (he faces down-left onto it)
const Pt LAMP = {70, 74};    // reading lamp, back-left
const Pt MUG = SPOT_mug;     // cocoa, on the mug spot

// ---------------------------------------------------------------- helpers (page-local)
struct FPt { float x, y; };

// Integer scanline fill of a convex polygon (n <= 8 points).
void fillPoly(Ctx &c, const FPt *pts, int n, Col color) {
  int PX[8], PY[8];
  int y0 = INT_MAX, y1 = INT_MIN;
  for (int k = 0; k < n; k++) {
    PX[k] = (int)lroundf(pts[k].x); PY[k] = (int)lroundf(pts[k].y);
    y0 = imin(y0, PY[k]); y1 = imax(y1, PY[k]);
  }
  for (int y = y0; y <= y1; y++) {
    int lo = INT_MAX, hi = INT_MIN;
    for (int k = 0; k < n; k++) {
      int ax = PX[k], ay = PY[k], bx = PX[(k + 1) % n], by = PY[(k + 1) % n];
      if (y < imin(ay, by) || y > imax(ay, by)) continue;
      if (ay == by) { lo = imin(lo, imin(ax, bx)); hi = imax(hi, imax(ax, bx)); continue; }
      float x = ax + ((y - ay) * (bx - ax)) / (float)(by - ay);
      int rx = (int)lroundf(x);
      lo = imin(lo, rx); hi = imax(hi, rx);
    }
    if (hi >= lo) rect(c, lo, y, hi - lo + 1, 1, color);
  }
}

// Page "words": [page 0|1][line] -> list of {i0, n} word runs (cells), on the book's top grid.
// Left page spans i 1..4, right page i 6..9; lines on j 1, 3, 5.
struct Word { int i0, n; };
struct Line { int nw; Word w[2]; };            // nw = 0: fine print (dithered)
const Line WORDS[2][3] = {
    {{2, {{1, 2}, {4, 1}}}, {2, {{1, 1}, {3, 2}}}, {1, {{1, 3}, {0, 0}}}},
    {{2, {{6, 1}, {8, 2}}}, {1, {{6, 3}, {0, 0}}}, {0, {{0, 0}, {0, 0}}}},   // right page line 3 = fine print
};

// g: gaze; mark: reading marker {mp, ml, mk} (mp < 0 none); gl: 0 none, 1 in hand, 2 on face (low)
struct Pose {
  int gx, gy; EyeShape eyes; ArmPose armL, armR; int squash, gl, flip; MouthKind mouth;
  int mp, ml, mk; bool sweat, spark, lick, flutter; int hmm;
};

// Open book with reading marker + optional flipping page. st.flip = 0 none, 1..4 page angle.
void drawBook(Ctx &c, const Pose &st) {
  // hard cover (a bit larger than the pages), then the page block on top
  box(c, BK.x, BK.y, 0, 12, 9, 1, SHADE::red);
  BoxRes r = box(c, BK.x, BK.y, 1, 10, 7, 3, {C::paper, C::paperShade, C::muted, NONE});
  // bookmark ribbon hanging out of the gutter
  Pt rb = r.left.at(10, 2); rect(c, rb.x, rb.y, 1, 4, C::amber); px(c, rb.x + 1, rb.y + 4, C::amber);
  // page edges stack on the side faces
  r.left.rect(0, 1, r.left.cols, 1, C::paper);
  r.right.rect(0, 1, r.right.cols, 1, C::paper);
  for (int j = 0; j < 7; j++) r.top.cell(5, j, C::paperShade);   // gutter
  // words
  for (int p = 0; p < 2; p++) for (int l = 0; l < 3; l++) {
    const Line &ln = WORDS[p][l];
    int j = 1 + l * 2;
    if (ln.nw == 0) {   // fine print: tiny broken dots
      for (int cc = 6; cc < 9; cc++) { Pt q = r.top.at(cc, j); px(c, q.x, q.y + 1, C::muted); px(c, q.x + 1, q.y + 2, C::paperShade); }
      if (st.mp == 1 && st.ml == 2) {
        Pt q = r.top.at(6 + st.mk, j); px(c, q.x, q.y + 1, C::orange); px(c, q.x + 1, q.y + 2, C::orange);
      }
      continue;
    }
    for (int k = 0; k < ln.nw; k++) {
      bool on = st.mp == p && st.ml == l && st.mk == k;
      for (int cc = ln.w[k].i0; cc < ln.w[k].i0 + ln.w[k].n; cc++) r.top.cell(cc, j, on ? C::orange : C::ink);
    }
  }
  // flipping page: hinged on the gutter (world x = 5.5), sweeps right page -> left page
  if (st.flip) {
    static const int ANG[5] = {0, 35, 80, 125, 165};
    double ang = ANG[st.flip] * 3.141592653589793 / 180;
    const double Wp = 4.4;
    double tx = 5.5 + Wp * cos(ang);
    int tz = (int)lround(Wp * 2.2 * sin(ang));
    auto pt = [&](double wx, double wy, double wz) -> FPt {
      return {(float)(r.top.Xback + 2 * (wx - wy)), (float)(r.top.Yback + wx + wy - wz)};
    };
    int curl = (st.flip == 2 || st.flip == 3) ? 1 : 0;   // a little flutter at the top
    FPt quad[4] = {pt(5.5, 0.5, 0), pt(tx, 0.5, tz + curl), pt(tx, 6.5, tz), pt(5.5, 6.5, 0)};
    fillPoly(c, quad, 4, st.flip >= 3 ? C::paperShade : C::paper);
    // outline the free edge so it reads against the pages behind
    line(c, (int)lroundf(quad[1].x), (int)lroundf(quad[1].y), (int)lroundf(quad[2].x), (int)lroundf(quad[2].y), C::muted);
    if (st.flip <= 2) {   // ghost lines of text on the lifting page's face
      FPt a = pt(5.5 + (tx - 5.5) * 0.4, 2, tz * 0.4), b = pt(5.5 + (tx - 5.5) * 0.4, 4, tz * 0.4);
      rect(c, (int)lroundf(a.x), (int)lroundf(a.y), 2, 1, C::paperShade);
      rect(c, (int)lroundf(b.x), (int)lroundf(b.y), 2, 1, C::paperShade);
    }
  }
}

// Half-moon reading glasses painted on the face (sit low, for looking down at the page).
void drawGlasses(const FacePainter &F, bool low) {
  int v = 10 + (low ? 2 : 0);
  Col rim = C::text, bar = C::greyLight;
  static const int U0[2] = {2, 20};
  for (int k = 0; k < 2; k++) {
    int u0 = U0[k];
    F.rect(u0, v, 10, 1, rim);            // top rim
    F.rect(u0, v + 1, 1, 6, rim);         // sides
    F.rect(u0 + 9, v + 1, 1, 6, rim);
    F.rect(u0 + 1, v + 7, 8, 1, rim);     // bottom (half-moon: rounded corners)
  }
  F.rect(12, v + 1, 8, 1, bar);           // bridge
  F.rect(0, v + 1, 2, 1, bar);            // temple arms
  F.rect(30, v + 1, 2, 1, bar);
}
const char *const GLASSES_HAND[] = {"###.###", "#.###.#", "###.###"};

// ---------------------------------------------------------------- timeline
// Defaults: g [0,2], eyes open, arms forward, squash 0, gl 0, flip 0, mouth none, no mark.
#define NOMARK -1, 0, 0
#define FWD ARM_FORWARD, ARM_FORWARD
const Beat<Pose> TL[] = {
    // line 1 (left page)
    {{-1, 1, EYE_OPEN, FWD, 0, 0, 0, MOUTH_NONE, 0, 0, 0, false, false, false, false, 0}, 3},   // a0
    {{0, 1, EYE_OPEN, FWD, 0, 0, 0, MOUTH_NONE, 0, 0, 1, false, false, false, false, 0}, 2},    // a1
    // drop a line (little head bob)
    {{-1, 2, EYE_OPEN, FWD, 1, 0, 0, MOUTH_NONE, 0, 1, 0, false, false, false, false, 0}, 2},   // b0
    {{0, 2, EYE_OPEN, FWD, 0, 0, 0, MOUTH_NONE, 0, 1, 1, false, false, false, false, 0}, 2},    // b1
    {{0, 2, EYE_BLINK, FWD, 0, 0, 0, MOUTH_NONE, 0, 1, 1, false, false, false, false, 0}, 1},   // blink
    {{0, 2, EYE_OPEN, FWD, 0, 0, 0, MOUTH_NONE, 0, 1, 1, false, false, false, false, 0}, 1},    // b1
    // over to right page
    {{1, 1, EYE_OPEN, FWD, 0, 0, 0, MOUTH_NONE, 1, 0, 0, false, false, false, false, 0}, 2},    // c0
    {{1, 1, EYE_OPEN, FWD, 0, 0, 0, MOUTH_NONE, 1, 0, 1, false, false, false, false, 0}, 1},    // c1
    {{1, 2, EYE_OPEN, FWD, 1, 0, 0, MOUTH_NONE, 1, 1, 0, false, false, false, false, 0}, 1},    // c2
    // fine print! squint, lean in
    {{1, 2, EYE_BLINK, FWD, 2, 0, 0, MOUTH_NONE, 1, 2, 0, false, false, false, false, 0}, 1},   // sq0
    {{1, 2, EYE_WINCE, FWD, 2, 0, 0, MOUTH_NONE, 1, 2, 0, true, false, false, false, 0}, 2},    // sq1
    // fetch reading glasses
    {{1, -1, EYE_OPEN, ARM_FORWARD, ARM_UP, 0, 1, 0, MOUTH_NONE, NOMARK, false, true, false, false, 0}, 2},   // fetch
    {{0, 0, EYE_HAPPY, ARM_FORWARD, ARM_UP, -1, 2, 0, MOUTH_NONE, NOMARK, false, false, false, false, 0}, 1},  // puton
    // ah, now I can see
    {{1, 2, EYE_WIDE, FWD, 0, 2, 0, MOUTH_NONE, 1, 2, 0, false, false, false, false, 0}, 2},    // see0
    {{1, 2, EYE_WIDE, FWD, 0, 2, 0, MOUTH_NONE, 1, 2, 1, false, false, false, false, 0}, 1},    // see1
    {{1, 2, EYE_WIDE, FWD, 0, 2, 0, MOUTH_NONE, 1, 2, 2, false, false, false, false, 0}, 1},    // see2
    // "hmm, interesting" nod
    {{0, 2, EYE_OPEN, FWD, 2, 2, 0, MOUTH_FLAT, NOMARK, false, false, false, false, 1}, 1},     // n0
    {{0, 0, EYE_OPEN, FWD, 0, 2, 0, MOUTH_FLAT, NOMARK, false, false, false, false, 2}, 1},     // n1
    {{0, 2, EYE_CLOSED, FWD, 2, 2, 0, MOUTH_FLAT, NOMARK, false, false, false, false, 2}, 1},   // n2
    {{0, 0, EYE_HAPPY, FWD, 0, 2, 0, MOUTH_SMILE, NOMARK, false, false, false, false, 2}, 2},   // n3
    // lick a finger...
    {{0, -1, EYE_CLOSED, ARM_FORWARD, ARM_NONE, -1, 2, 0, MOUTH_LAUGH, NOMARK, false, false, true, false, 0}, 2},   // lick
    // ...and flip the page
    {{1, 2, EYE_OPEN, FWD, 0, 2, 1, MOUTH_NONE, NOMARK, false, false, false, false, 0}, 1},     // f1
    {{0, 1, EYE_OPEN, FWD, 0, 2, 2, MOUTH_NONE, NOMARK, false, false, false, true, 0}, 1},      // f2
    {{-1, 1, EYE_OPEN, FWD, 0, 2, 3, MOUTH_NONE, NOMARK, false, false, false, true, 0}, 1},     // f3
    {{-1, 2, EYE_OPEN, FWD, 1, 2, 4, MOUTH_NONE, NOMARK, false, false, false, false, 0}, 1},    // f4
    // glasses off (fold them away), settle back into reading
    {{0, 0, EYE_BLINK, ARM_FORWARD, ARM_UP, -1, 1, 0, MOUTH_NONE, NOMARK, false, false, false, false, 0}, 2},   // off
    {{-1, 1, EYE_OPEN, FWD, 1, 0, 0, MOUTH_NONE, NOMARK, false, false, false, false, 0}, 1},    // settle
};
#undef NOMARK
#undef FWD

void draw(Ctx &c, int i, const AnimEnv &env) {
  const Pose &s = seq(TL, i);
  drawDesk(c);
  lamp(c, LAMP.x, LAMP.y, true, true);
  MugOpts mo; mo.frame = i >> 1; mo.color = SHADE::blue;
  mug(c, MUG.x, MUG.y, mo);

  ClawdOpts o;
  o.x = CL.x; o.y = CL.y;
  o.tint = env.tint();
  o.legs = LEGS_SIT;
  o.armL = s.armL; o.armR = s.armR;
  o.squash = s.squash;
  o.mouth = s.mouth;
  o.eyes = {s.eyes, s.gx, s.gy};
  o.blush = s.eyes == EYE_HAPPY;
  ClawdRes a = drawClawd(c, o);
  if (s.gl >= 2) drawGlasses(a.F, true);
  if (s.lick) {   // a stubby finger raised to the tongue (Clawd-coloured)
    const Pal &t = env.tint();
    a.F.rect(18, 19, 4, 7, t.top); a.F.rect(22, 19, 1, 7, t.right); a.F.rect(18, 26, 5, 1, t.right);
  }
  if (s.gl == 1 && a.hasHandR) sprite(c, a.handR.x - 3, a.handR.y - 5, GLASSES_HAND, ISO_MAP({'#', C::text}));

  drawBook(c, s);

  // FX
  if (s.spark && a.hasHandR) fx::sparkle(c, a.handR.x + 6, a.handR.y - 8, 1 + (i & 1), C::amber);
  if (s.sweat) fx::sweat(c, a.head.x + 14, a.head.y + 6);
  if (s.hmm) {
    fx::PuffOpts po; po.thought = true; po.tail = 'l';
    fx::Rect b = fx::puff(c, a.head.x + 6, a.head.y - 16, 25, 11, po);
    text(c, b.x + 1, b.y + 1, s.hmm == 1 ? "HM" : "HMM.", C::text);
  }
  if (s.flutter) fx::ticks(c, BK.x + 6, BK.y - 14, "up", C::muted);
}
}  // namespace

const IsoAnim anim_10_reading_doc = {"10-reading-doc", 10, seqLen(TL), true, false, draw, 0, 14};   // calm segment: see anim.h
