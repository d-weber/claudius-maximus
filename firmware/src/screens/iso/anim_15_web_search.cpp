// 15 Web search: port of iso-clawd/anims/15-web-search.html.
#include "anim.h"
#include <math.h>
using namespace iso;

namespace {
// ---- beats (10 fps) ---------------------------------------------------------
// load:  browser spinner + "..." bubble, Clawd stares, foot taps impatiently
// tap:   he taps the window twice (window jolts)
// pop:   results pop in row by row, eyes go wide + sparkle
// yay:   happy hop
// spin:  he swats the desk globe, it spins fast
// peer:  magnifier up, globe slows... stops; "!" he found a spot
// back:  glances back at browser, which clears for the next query (loop)
enum B { LOAD, TAP, POP, YAY, SPIN, PEER, FOUND, BACK };
const Beat<B> BEATS[] = {
    {LOAD, 10}, {TAP, 4}, {POP, 6}, {YAY, 4}, {SPIN, 5}, {PEER, 6}, {FOUND, 4}, {BACK, 1},
};
int beatStart(B b) { int s = 0; for (const auto &e : BEATS) { if (e.v == b) return s; s += e.hold; } return s; }
B beat(int i) { return seq(BEATS, i); }
int local(int i) { return i - beatStart(beat(i)); }

// globe angle as a pure function of i: accumulate per-frame speed
int globeSpeed(int i) {
  B b = beat(i); int k = local(i);
  if (b == SPIN) return k == 0 ? 0 : 3;
  if (b == PEER) { static const int S[] = {3, 2, 1, 1, 1, 0}; return S[k]; }
  if (b == LOAD) return k % 3 == 0 ? 1 : 0; // lazy idle drift
  return 0;
}
int globeFrame(int i) { int f = 0; for (int j = 0; j <= i; j++) f += globeSpeed(j); return f; }

const Pt BROWSER = {70, 74};
const Pt GLOBE = {174, 80};

// ---- custom big browser window (upright panel facing down-right, toward Clawd) ----
struct BrowserState { bool loading; int progress; int results; int newest; int frame; };
void browser(Ctx &c, int x, int y, const BrowserState &st) {
  FacePainter p = plane(c, x, y, 6, 18, 30, 'R', C::surface);
  const int W = p.cols; // 36
  p.rect(0, 0, W, 4, C::surface2);
  p.rect(0, 4, 1, 26, C::surface2); p.rect(W - 1, 4, 1, 26, C::surface2); p.rect(0, 29, W, 1, C::surface2);
  p.rect(2, 1, 2, 2, C::red); p.rect(5, 1, 2, 2, C::amber); p.rect(8, 1, 2, 2, C::green);
  // address bar with query
  p.rect(3, 6, 30, 3, C::paper);
  p.px(4, 7, C::blue);
  p.rect(7, 7, 11, 1, C::ink);
  if (st.loading) {
    static const int SP[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const int *sp = SP[st.frame % 4];
    p.rect(29 + sp[0], 6 + sp[1], 2, 2, C::orange);
    p.rect(3, 10, st.progress, 1, C::blue);
    int n = (st.frame >> 1) % 4;
    for (int d = 0; d < n; d++) p.rect(12 + d * 5, 18, 3, 3, C::muted);
  }
  static const int RW[] = {22, 16, 20}, SW[] = {18, 14, 16};
  for (int r = 0; r < st.results; r++) {
    int v = 12 + r * 6;
    p.rect(3, v, RW[r], 2, r == st.newest ? C::amber : C::blue);
    p.rect(3, v + 3, 7, 1, C::green);
    p.rect(12, v + 3, SW[r], 1, C::muted);
  }
}

// ---- hollow magnifier held up to one eye (eye stays visible through the ring) ----
void lens(Ctx &c, int cx, int cy, bool glint) {
  const int R = 8;
  // magnified eye behind the glass: a giant version of his eye (the gag)
  rect(c, cx - 4, cy - 7, 8, 15, C::eye);
  rect(c, cx - 3, cy - 6, 3, 3, C::text);
  px(c, cx + 2, cy + 4, C::dim);
  for (int j = -R - 1; j <= R + 1; j++) for (int i = -R - 1; i <= R + 1; i++) {
    double d = sqrt((double)(i * i + j * j));
    if (fabs(d - R) < 0.7) px(c, cx + i, cy + j, (i + j < -4) ? C::text : C::muted);
  }
  if (glint) { px(c, cx - 5, cy - 3, C::text); px(c, cx - 4, cy - 5, C::text); }
  for (int s = 0; s < 4; s++) rect(c, cx + 6 + 2 * s, cy + 6 + s, 2, 2, s ? C::woodRight : C::muted);
}

// ---- custom desk globe (bigger than prop.globe so the spin reads across the desk) ----
const char *const MAP[] = {
    ".####......#####....##..",
    "######....########.####.",
    ".#####.....#######..###.",
    "..####......####....##..",
    "...###......####........",
    "....##.......###....###.",
    ".....#.......##....####.",
    "..............#.....##..",
};
struct GlobeRes { int x, y, r; };
GlobeRes globe(Ctx &c, int x, int y, int f, bool pin) {
  box(c, x, y, 0, 4, 4, 2, SHADE::wood);
  rect(c, x, y - 10, 1, 9, C::woodLeft);
  const int cx = x, cy = y - 19, r = 8;
  for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) {
    if (i * i + j * j > r * r + 2) continue;
    double half = sqrt((double)imax(1, r * r + 2 - j * j));
    double q = i / half; if (q < -1) q = -1; if (q > 1) q = 1;
    double a = asin(q);
    int lon = ((int)floor((a / M_PI + 0.5) * 12) + f) % 24;
    int lat = imin(7, (j + r) * 8 / (2 * r + 1));
    bool land = MAP[lat][(lon + 24) % 24] == '#';
    Col col = land ? C::green : C::blue;
    if (i >= 5 || (i >= 3 && (j < 0 ? -j : j) >= 5) || j >= 6) col = land ? C::greenDark : C::blueDark;
    else if (i <= -3 && j <= -2 && i + j <= -6) col = land ? C::green : C::blueLight;
    px(c, cx + i, cy + j, col);
  }
  // brass half-meridian on the right
  for (int d = -70; d <= 100; d += 6) {
    double rr = r + 2, t = d * M_PI / 180;
    px(c, (int)lround(cx + cos(t) * rr), (int)lround(cy + sin(t) * rr), C::amberDark);
  }
  px(c, cx, cy - r - 2, C::amberDark); px(c, cx, cy - r - 1, C::amberDark);
  if (pin) { rect(c, cx - 4, cy - 6, 3, 3, C::red); px(c, cx - 4, cy - 6, C::blush); rect(c, cx - 3, cy - 3, 1, 2, C::text); }
  return {cx, cy, r};
}

void draw(Ctx &c, int i, const AnimEnv &env) {
  const int N = seqLen(BEATS);
  i = ((i % N) + N) % N;
  const B b = beat(i); const int k = local(i);
  drawDesk(c);
  mug(c, SPOT_mug.x, SPOT_mug.y, {i});

  // ---- browser (behind Clawd) ----
  int jolt = 0;
  BrowserState st = {true, 0, 0, -1, i};
  if (b == LOAD) { static const int PR[] = {2, 8, 14, 18, 20, 21, 22, 22, 22, 22}; st.progress = PR[k]; } // crawls... stalls
  if (b == TAP) { st.progress = 22; jolt = (k == 0 || k == 2) ? -2 : 0; }
  if (b == POP) { st.loading = false; st.results = imin(3, k + 1); st.newest = k <= 2 ? k : -1; }
  if (b == YAY || b == SPIN || b == PEER || b == FOUND) { st.loading = false; st.results = 3; }
  if (b == BACK) { st.loading = k > 0; st.results = k > 0 ? 0 : 3; st.progress = 0; }
  browser(c, BROWSER.x + jolt, BROWSER.y + (jolt ? -1 : 0), st);

  // ---- Clawd pose ----
  ClawdOpts P;
  P.x = SPOT_clawd.x; P.y = SPOT_clawd.y; P.tint = env.tint();
  P.eyes = {EYE_OPEN, -1, 0}; P.armL = P.armR = ARM_OUT; P.legs = LEGS_STAND; P.mouth = MOUTH_NONE;
  if (b == LOAD) {
    P.legs = (k >= 4 && k % 2 == 0) ? LEGS_TAP : LEGS_STAND;
    P.mouth = k >= 6 ? MOUTH_FLAT : MOUTH_NONE;
    if (k == 3) P.eyes = {EYE_BLINK, -1, 0};
    if (k >= 7) P.eyes = {EYE_OPEN, -1, -1};
  } else if (b == TAP) {
    P.armL = (k == 0 || k == 2) ? ARM_FORWARD : ARM_HIGH; P.armR = ARM_OUT;
    P.mouth = MOUTH_FLAT;
    P.squash = (k == 0 || k == 2) ? 1 : 0;
  } else if (b == POP) {
    P.eyes = {EYE_WIDE, -1, -1};
    P.mouth = MOUTH_O;
    P.squash = k == 0 ? -3 : 0;
    P.armL = P.armR = k == 0 ? ARM_UP : ARM_OUT;
  } else if (b == YAY) {
    static const int Z[] = {2, 4, 4, 0};
    P.eyes = {EYE_HAPPY};
    P.mouth = MOUTH_LAUGH;
    P.blush = true;
    P.z = Z[k];
    P.squash = k == 0 ? -3 : (k == 3 ? 3 : 0);
    P.armL = P.armR = ARM_HIGH;
  } else if (b == SPIN) {
    P.eyes = {EYE_OPEN, 1, 1};
    P.armL = ARM_OUT; P.armR = k == 0 ? ARM_HIGH : (k == 1 ? ARM_FORWARD : ARM_OUT);
    P.mouth = k >= 1 ? MOUTH_SMILE : MOUTH_NONE;
    P.squash = k == 1 ? 2 : 0;
  } else if (b == PEER) {
    P.eyes = {EYE_OPEN, 1, 1, EYE_OPEN, EYE_WIDE};
    P.armL = P.armR = ARM_OUT;
    P.mouth = MOUTH_O;
  } else if (b == FOUND) {
    P.eyes = {EYE_WIDE, 1, 1};
    P.armL = ARM_UP; P.armR = ARM_OUT;
    P.mouth = MOUTH_SMILE;
    P.blush = true;
    P.z = k == 0 ? 1 : 0;
  } else if (b == BACK) {
    P.eyes = {EYE_OPEN, -1, 0};
  }
  ClawdRes a = drawClawd(c, P);

  // ---- globe (front-right of Clawd, drawn after him) ----
  GlobeRes g = globe(c, GLOBE.x, GLOBE.y, globeFrame(i), b == FOUND);

  // ---- held magnifier ----
  if (b == PEER || b == FOUND) {
    lens(c, a.eyeR.x, a.eyeR.y, k % 4 == 0);
  }

  // ---- FX ----
  if (b == LOAD && k >= 7) fx::sweat(c, a.head.x + 22, a.head.y + 8);
  if (b == TAP && (k == 0 || k == 2) && a.hasHandL) fx::ticks(c, a.handL.x - 4, a.handL.y - 2, "left", C::text);
  if (b == POP && k >= 1) {
    static const int S1[] = {1, 2, 3, 2}, S2[] = {3, 2, 1, 2};
    fx::sparkle(c, a.eyeL.x - 3, a.eyeL.y - 6, cycle(S1, k), C::amber);
    fx::sparkle(c, a.eyeR.x + 4, a.eyeR.y - 7, cycle(S2, k), C::text);
  }
  if (b == YAY) {
    static const int S1[] = {2, 3}, S2[] = {3, 2};
    fx::sparkle(c, a.head.x - 18, a.head.y + 2, cycle(S1, k), C::amber);
    fx::sparkle(c, a.head.x + 20, a.head.y - 2, cycle(S2, k), C::amber);
  }
  if (b == SPIN && k >= 1) { fx::ticks(c, g.x, g.y - g.r - 4, "up", C::text); fx::ticks(c, g.x + 2, g.y, "right", C::text); }
  if (b == FOUND) {
    static const int S1[] = {1, 2, 3, 2};
    fx::bang(c, a.head.x, a.head.y - 8, true);
    fx::sparkle(c, g.x - 6, g.y - 7, cycle(S1, k), C::amber);
  }
}
}  // namespace

const IsoAnim anim_15_web_search = {"15-web-search", 10, seqLen(BEATS), true, false, draw, 0, 9};   // calm segment: see anim.h
