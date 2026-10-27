// 12 Creating a plan: port of iso-clawd/anims/12-creating-plan.html.
#include "anim.h"
using namespace iso;

namespace {
// ---------------------------------------------------------------- board (custom easel whiteboard)
// Whiteboard on the left of the desk, its writing face turned down-right (toward Clawd and us).
struct BoardGeom { int x, y, len, h, z; };
const BoardGeom BOARD = {80, 70, 24, 36, 8};

FacePainter drawEasel(Ctx &c) {
  int x = BOARD.x, y = BOARD.y, len = BOARD.len, h = BOARD.h, z = BOARD.z;
  // back leg (behind board), then the two front legs
  box(c, x - 4, y - 2, 0, 1, 1, z + 20, SHADE::wood);
  BoxRes r = box(c, x, y, z, 1, len, h, {C::woodEdge, C::woodLeft, C::paper, NONE});
  box(c, x + 18, y - 9, 0, 1, 1, z, SHADE::wood);
  box(c, x - 18, y + 9, 0, 1, 1, z, SHADE::wood);
  // marker tray along the bottom
  box(c, x + 2, y + 1, z - 2, 1, len - 2, 2, {C::woodTop, C::woodLeft, C::woodRight, NONE});
  return r.right;
}

struct Stick { int u, v; Col c, s; };
const Stick STICK[3] = {
    {2, 13, C::amber, C::amberDark},
    {21, 13, C::blueLight, C::blue},
    {34, 24, C::green, C::greenDark},
};
void stickyNote(const FacePainter &p, int k, bool fresh) {
  const Stick &s = STICK[k];
  p.rect(s.u, s.v, 12, 9, s.c);
  p.rect(s.u + 2, s.v + 2, 8, 1, s.s);
  p.rect(s.u + 2, s.v + 4, 5, 1, s.s);
  p.rect(s.u + 2, s.v + 6, 6, 1, s.s);
  if (fresh) { p.rect(s.u + 9, s.v + 6, 3, 3, C::paperShade); p.px(s.u + 9, s.v + 6, s.s); }   // corner still curling
  else p.rect(s.u, s.v + 8, 12, 1, s.s);
}
// arrow 1 -> 2 (horizontal), n = pixels revealed
const int A12[][2] = {{14, 17}, {15, 17}, {16, 17}, {17, 17}, {18, 17}, {19, 17}, {18, 16}, {18, 18}, {17, 16}, {17, 18}, {16, 15}, {16, 19}};
constexpr int NA12 = sizeof(A12) / sizeof(A12[0]);
// the doomed arrow: wanders off sticky 2 and scribbles off the board edge
const int AWRONG[][2] = {{33, 17}, {32, 17}, {32, 16}, {33, 15}, {34, 14}, {35, 14}, {36, 15}, {37, 17}, {36, 19}, {35, 20}, {34, 19}, {33, 17}, {34, 15},
                         {35, 13}, {37, 11}, {38, 10}, {39, 9}, {40, 8}, {41, 7}, {42, 6}, {43, 5}, {44, 4}, {45, 3}, {46, 2}, {47, 1}};
constexpr int NAWRONG = sizeof(AWRONG) / sizeof(AWRONG[0]);
// arrow 2 -> 3 (down then right)
const int A23[][2] = {{27, 22}, {27, 23}, {27, 24}, {27, 25}, {27, 26}, {27, 27}, {27, 28}, {28, 28}, {29, 28}, {30, 28}, {31, 28}, {32, 28}, {31, 27},
                      {31, 29}, {30, 26}, {30, 30}, {33, 28}};
constexpr int NA23 = sizeof(A23) / sizeof(A23[0]);

// Board state. fresh: sticky index still curling (-1 none). flip: -1 none, 0 / 1 page-flip frames.
struct Board { int s, fresh, a12, wrong, erase, a23; bool check; int flip; };
const Board B0 = {0, -1, 0, 0, 0, 0, false, -1};
const Board FINAL = {3, -1, 12, 0, 0, 17, true, -1};   // "prev" of the flip-0 frame

void drawHeader(const FacePainter &p) {
  p.rect(0, 0, p.cols, 1, C::woodEdge);
  // title lettered ON the board (sheared with it), own 3x5 glyphs
  static const char *const G[4][5] = {
      {"##.", "#.#", "##.", "#..", "#.."},   // P
      {"#..", "#..", "#..", "#..", "###"},   // L
      {".#.", "#.#", "###", "#.#", "#.#"},   // A
      {"#.#", "###", "###", "#.#", "#.#"},   // N
  };
  int u = 3;
  for (int g = 0; g < 4; g++) {
    for (int v = 0; v < 5; v++) for (int cc = 0; cc < 3; cc++) if (G[g][v][cc] == '#') p.px(u + cc, 3 + v, C::ink);
    u += 4;
  }
  p.rect(3, 9, 16, 1, C::orange);   // underline
}
void drawContent(const FacePainter &p, const Board &st) {
  drawHeader(p);
  if (st.s >= 1) stickyNote(p, 0, st.fresh == 0);
  if (st.s >= 2) stickyNote(p, 1, st.fresh == 1);
  for (int k = 0; k < imin(st.a12, NA12); k++) p.px(A12[k][0], A12[k][1], C::ink);
  // wrong arrow: drawn, then rubbed out from the tip back (eraser smudge)
  int w = st.wrong, e = st.erase;
  for (int k = 0; k < imin(w, NAWRONG); k++) {
    bool gone = NAWRONG - k <= e * 5;
    p.px(AWRONG[k][0], AWRONG[k][1], gone ? C::paperShade : C::red);
  }
  if (e > 0 && e < 6) for (int k = 0; k < 5; k++) p.px(33 + ((k * 5 + e * 3) % 13), 3 + k * 3, C::paperShade);
  if (st.s >= 3) stickyNote(p, 2, st.fresh == 2);
  for (int k = 0; k < imin(st.a23, NA23); k++) p.px(A23[k][0], A23[k][1], C::ink);
  if (st.check) {
    static const int chk[8][2] = {{37, 28}, {38, 29}, {39, 30}, {40, 29}, {41, 28}, {42, 27}, {43, 26}, {44, 25}};
    const Stick &s3 = STICK[2];
    p.rect(s3.u + 1, s3.v + 1, 10, 6, s3.c);
    for (int q = 0; q < 8; q++) { p.px(chk[q][0], chk[q][1], C::text); p.px(chk[q][0], chk[q][1] + 1, C::greenDark); }
  }
}
FacePainter drawBoard(Ctx &c, const Board &st) {
  FacePainter p = drawEasel(c);
  if (st.flip >= 0) {
    // page flip: a fresh sheet swings down over the old one (2 frames)
    if (st.flip == 0) { drawContent(p, FINAL); p.rect(0, 0, p.cols, 16, C::paperShade); p.rect(0, 16, p.cols, 1, C::muted); }
    else { p.rect(0, 0, p.cols, 1, C::woodEdge); p.rect(0, 32, p.cols, 3, C::paperShade); p.rect(0, 35, p.cols, 1, C::muted); }
    if (st.flip == 1) drawHeader(p);
    return p;
  }
  drawContent(p, st);
  return p;
}

// lightbulb FX (amber, with base and rays)
void bulb(Ctx &c, int x, int y, bool big) {
  static const char *const rows[] = {
      "...aaa...",
      ".aaaaaaa.",
      ".awwaaaa.",
      "aawaaaaaa",
      "aaaaaaaaa",
      ".aaaaaaa.",
      "..aaaaa..",
      "...ddd...",
      "...ggg...",
      "...ddd...",
  };
  sprite(c, x - 4, y - 6, rows, ISO_MAP({'a', C::amber}, {'w', C::text}, {'d', C::grey}, {'g', C::greyLight}));
  if (big) {
    rect(c, x, y - 11, 1, 3, C::amber);
    rect(c, x - 10, y - 2, 3, 1, C::amber); rect(c, x + 8, y - 2, 3, 1, C::amber);
    px(c, x - 7, y - 9, C::amber); px(c, x - 6, y - 8, C::amber);
    px(c, x + 7, y - 9, C::amber); px(c, x + 6, y - 8, C::amber);
  }
}
// marker in hand
void marker(Ctx &c, const ClawdRes &a, Col tip) {
  if (!a.hasHandL) return;
  Pt h = a.handL;
  rect(c, h.x - 2, h.y - 1, 3, 2, C::text);
  rect(c, h.x - 3, h.y - 1, 1, 2, tip);
}
void stickyInHand(Ctx &c, const ClawdRes &a, Col col) {
  if (!a.hasHandL) return;
  Pt h = a.handL;
  rect(c, h.x - 4, h.y - 4, 5, 5, col);
}

// ---------------------------------------------------------------- timeline
const Pt HOME = {130, 80};
enum Hold { H_NONE, H_STICKY0, H_STICKY1, H_STICKY2, H_MARKER };
// fx: 0 none, 't' ticks n, 's' sweat, 'd' dust n, 'k' think n, 'b' bulb n, 'p' proud
struct Fx { char kind; int n; };
struct Step {
  Eyes eyes; MouthKind mouth; ArmPose armL, armR; LegsPose legs; int dx, dy, z, squash, shake; bool blush;
  Board board; Hold hold; Fx fx; bool scratch;
};
#define LOOK {EYE_OPEN, -1, 0}
#define NOFX {0, 0}
const Beat<Step> STEPS[] = {
    // eye up the empty board, sticky ready in hand
    {{LOOK, MOUTH_NONE, ARM_OUT, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, false, B0, H_STICKY0, NOFX, false}, 2},
    {{{EYE_BLINK, -1, 0}, MOUTH_NONE, ARM_OUT, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, false, B0, H_STICKY0, NOFX, false}, 1},
    // slap sticky 1: wind-up, SLAP (squash), settle
    {{LOOK, MOUTH_NONE, ARM_OUT, ARM_OUT, LEGS_STAND, 0, 0, 1, -3, 0, false, B0, H_STICKY0, NOFX, false}, 2},
    {{{EYE_CLOSED, 0, 0}, MOUTH_NONE, ARM_HIGH, ARM_OUT, LEGS_STAND, 0, 0, 0, 3, 0, false, {1, 0, 0, 0, 0, 0, false, -1}, H_NONE, {'t', 0}, false}, 2},
    {{LOOK, MOUTH_NONE, ARM_OUT, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, false, {1, -1, 0, 0, 0, 0, false, -1}, H_STICKY1, NOFX, false}, 1},
    // slap sticky 2
    {{LOOK, MOUTH_NONE, ARM_OUT, ARM_OUT, LEGS_STAND, 0, 0, 1, -3, 0, false, {1, -1, 0, 0, 0, 0, false, -1}, H_STICKY1, NOFX, false}, 1},
    {{{EYE_CLOSED, 0, 0}, MOUTH_NONE, ARM_HIGH, ARM_OUT, LEGS_STAND, 0, 0, 0, 3, 0, false, {2, 1, 0, 0, 0, 0, false, -1}, H_NONE, {'t', 1}, false}, 2},
    // draw arrow 1 -> 2 (marker squeaks along)
    {{{EYE_OPEN, -1, 1}, MOUTH_NONE, ARM_HIGH, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, false, {2, -1, 4, 0, 0, 0, false, -1}, H_MARKER, NOFX, false}, 1},
    {{{EYE_OPEN, -1, 1}, MOUTH_NONE, ARM_OUT, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, false, {2, -1, 8, 0, 0, 0, false, -1}, H_MARKER, NOFX, false}, 1},
    {{{EYE_OPEN, -1, 1}, MOUTH_NONE, ARM_HIGH, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, false, {2, -1, 12, 0, 0, 0, false, -1}, H_MARKER, NOFX, false}, 1},
    // the next arrow goes... wrong
    {{{EYE_OPEN, -1, 0}, MOUTH_NONE, ARM_OUT, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, false, {2, -1, 12, 7, 0, 0, false, -1}, H_MARKER, NOFX, false}, 1},
    {{{EYE_OPEN, -1, -1}, MOUTH_NONE, ARM_HIGH, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, false, {2, -1, 12, 14, 0, 0, false, -1}, H_MARKER, NOFX, false}, 1},
    {{{EYE_OPEN, -1, -2}, MOUTH_NONE, ARM_UP, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, false, {2, -1, 12, 24, 0, 0, false, -1}, H_MARKER, NOFX, false}, 1},
    {{{EYE_WIDE, -1, 0}, MOUTH_WOBBLY, ARM_UP, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, false, {2, -1, 12, 24, 0, 0, false, -1}, H_MARKER, {'s', 0}, false}, 3},
    // erase it, furiously
    {{{EYE_WINCE, 0, 0}, MOUTH_NONE, ARM_WAVEA, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, -1, false, {2, -1, 12, 24, 1, 0, false, -1}, H_NONE, {'d', 0}, false}, 1},
    {{{EYE_WINCE, 0, 0}, MOUTH_NONE, ARM_WAVEB, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 1, false, {2, -1, 12, 24, 2, 0, false, -1}, H_NONE, {'d', 1}, false}, 1},
    {{{EYE_WINCE, 0, 0}, MOUTH_NONE, ARM_WAVEA, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, -1, false, {2, -1, 12, 24, 4, 0, false, -1}, H_NONE, {'d', 2}, false}, 1},
    {{{EYE_WINCE, 0, 0}, MOUTH_NONE, ARM_WAVEB, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 1, false, {2, -1, 12, 24, 6, 0, false, -1}, H_NONE, {'d', 3}, false}, 1},
    // step back, head-scratch, thinking dots
    {{LOOK, MOUTH_NONE, ARM_OUT, ARM_OUT, LEGS_STEPA, 4, 2, 0, 0, 0, false, {2, -1, 12, 0, 0, 0, false, -1}, H_NONE, NOFX, false}, 1},
    {{LOOK, MOUTH_NONE, ARM_OUT, ARM_OUT, LEGS_STEPB, 8, 4, 0, 0, 0, false, {2, -1, 12, 0, 0, 0, false, -1}, H_NONE, NOFX, false}, 1},
    {{{EYE_OPEN, -1, -1}, MOUTH_FLAT, ARM_UP, ARM_OUT, LEGS_STAND, 8, 4, 0, 0, 0, false, {2, -1, 12, 0, 0, 0, false, -1}, H_NONE, {'k', 1}, true}, 2},
    {{{EYE_OPEN, -1, -2}, MOUTH_FLAT, ARM_HIGH, ARM_OUT, LEGS_STAND, 8, 4, 0, 0, 0, false, {2, -1, 12, 0, 0, 0, false, -1}, H_NONE, {'k', 2}, false}, 2},
    {{{EYE_BLINK, 0, 0}, MOUTH_FLAT, ARM_UP, ARM_OUT, LEGS_STAND, 8, 4, 0, 0, 0, false, {2, -1, 12, 0, 0, 0, false, -1}, H_NONE, {'k', 3}, true}, 1},
    {{{EYE_OPEN, -1, -2}, MOUTH_FLAT, ARM_HIGH, ARM_OUT, LEGS_STAND, 8, 4, 0, 0, 0, false, {2, -1, 12, 0, 0, 0, false, -1}, H_NONE, {'k', 3}, false}, 1},
    // AHA!
    {{{EYE_WIDE, 0, -1}, MOUTH_O, ARM_OUT, ARM_OUT, LEGS_STAND, 8, 4, 0, 3, 0, false, {2, -1, 12, 0, 0, 0, false, -1}, H_NONE, {'b', 0}, false}, 1},
    {{{EYE_WIDE, 0, -1}, MOUTH_O, ARM_OUT, ARM_UP, LEGS_STAND, 8, 4, 5, -3, 0, false, {2, -1, 12, 0, 0, 0, false, -1}, H_NONE, {'b', 1}, false}, 2},
    {{{EYE_HAPPY, 0, 0}, MOUTH_SMILE, ARM_OUT, ARM_UP, LEGS_STAND, 8, 4, 0, 2, 0, false, {2, -1, 12, 0, 0, 0, false, -1}, H_NONE, {'b', 1}, false}, 1},
    // hustle back in: one more sticky + a properly routed arrow
    {{LOOK, MOUTH_NONE, ARM_OUT, ARM_OUT, LEGS_STEPA, 4, 2, 0, 0, 0, false, {2, -1, 12, 0, 0, 0, false, -1}, H_STICKY2, NOFX, false}, 1},
    {{LOOK, MOUTH_NONE, ARM_OUT, ARM_OUT, LEGS_STAND, 0, 0, 1, -3, 0, false, {2, -1, 12, 0, 0, 0, false, -1}, H_STICKY2, NOFX, false}, 1},
    {{{EYE_CLOSED, 0, 0}, MOUTH_NONE, ARM_HIGH, ARM_OUT, LEGS_STAND, 0, 0, 0, 3, 0, false, {3, 2, 12, 0, 0, 0, false, -1}, H_NONE, {'t', 2}, false}, 2},
    {{{EYE_OPEN, -1, 1}, MOUTH_NONE, ARM_OUT, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, false, {3, -1, 12, 0, 0, 6, false, -1}, H_MARKER, NOFX, false}, 1},
    {{{EYE_OPEN, -1, 1}, MOUTH_NONE, ARM_HIGH, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, false, {3, -1, 12, 0, 0, 12, false, -1}, H_MARKER, NOFX, false}, 1},
    {{{EYE_OPEN, -1, 1}, MOUTH_NONE, ARM_OUT, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, false, {3, -1, 12, 0, 0, 17, true, -1}, H_MARKER, NOFX, false}, 1},
    // proud general
    {{{EYE_HAPPY, 0, 0}, MOUTH_SMILE, ARM_OUT, ARM_HIGH, LEGS_STAND, 0, 0, 0, 0, 0, true, {3, -1, 12, 0, 0, 17, true, -1}, H_NONE, {'p', 0}, false}, 3},
    // flip to a fresh page (this is how the loop resets to the empty board)
    {{LOOK, MOUTH_NONE, ARM_HIGH, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, false, {0, -1, 0, 0, 0, 0, false, 0}, H_NONE, NOFX, false}, 1},
    {{LOOK, MOUTH_NONE, ARM_OUT, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, false, {0, -1, 0, 0, 0, 0, false, 1}, H_NONE, NOFX, false}, 1},
};
#undef LOOK
#undef NOFX

void draw(Ctx &c, int i, const AnimEnv &env) {
  const Step &st = seq(STEPS, i);
  drawDesk(c);
  // back props: a couple of spare stickies on the desk (the mug is drawn after Clawd: its spot is in front of him)
  MugOpts mo; mo.frame = i;
  StickyOpts s1; s1.color = C::blush; iso::sticky(c, 98, 92, s1);
  StickyOpts s2; s2.color = C::violet; iso::sticky(c, 105, 95, s2);

  FacePainter p = drawBoard(c, st.board);

  ClawdOpts o;
  o.x = HOME.x + st.dx; o.y = HOME.y + st.dy; o.z = st.z;
  o.tint = env.tint();
  o.eyes = st.eyes; o.mouth = st.mouth; o.blush = st.blush;
  o.armL = st.armL; o.armR = st.armR; o.legs = st.legs; o.squash = st.squash; o.shake = st.shake;
  ClawdRes a = drawClawd(c, o);
  mug(c, SPOT_mug.x, SPOT_mug.y, mo);   // the mug spot is in front of him

  // held things
  if (st.hold == H_MARKER) marker(c, a, C::ink);
  else if (st.hold != H_NONE) stickyInHand(c, a, STICK[st.hold - H_STICKY0].c);
  if (st.scratch && a.hasHandL) {
    px(c, a.handL.x - 3, a.handL.y - 2, C::text); px(c, a.handL.x - 4, a.handL.y, C::text); px(c, a.handL.x + 3, a.handL.y - 3, C::text);
  }

  // FX
  switch (st.fx.kind) {
    case 't': { const Stick &s = STICK[st.fx.n]; Pt q = p.at(s.u + 3, s.v + 2); fx::ticks(c, q.x + 7, q.y - 1, "right", C::text); break; }
    case 's': fx::sweat(c, a.head.x + 14, a.head.y + 4); break;
    case 'd': { Pt q = p.at(40, 10); fx::poof(c, q.x, q.y, st.fx.n, C::paperShade); break; }
    case 'k': fx::dots(c, a.head.x - 2, a.head.y - 7, st.fx.n, C::text); break;
    case 'b': bulb(c, a.head.x + 2, a.head.y - 9, st.fx.n == 1); break;
    case 'p': {
      static const int C1[4] = {1, 2, 3, 2}, C2[4] = {3, 2, 1, 2};
      fx::sparkle(c, a.head.x + 18, a.head.y - 2, cycle(C1, i), C::amber);
      fx::sparkle(c, 60, 36, cycle(C2, i), C::text);
      break;
    }
    default: break;
  }
}
}  // namespace

const IsoAnim anim_12_creating_plan = {"12-creating-plan", 12, seqLen(STEPS), true, false, draw, 0, 2};   // calm segment: see anim.h
