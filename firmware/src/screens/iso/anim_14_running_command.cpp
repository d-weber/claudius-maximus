// 14 Running a command: port of iso-clawd/anims/14-running-command.html.
#include "anim.h"
using namespace iso;

namespace {
// ---------------------------------------------------------------- timeline (10 fps, 38 frames = 3.8 s)
// phase per frame: wind (raise arm over big button) -> slam -> run (output scrolls, eyes track,
// foot taps, progress fills) -> ding (done, happy hop) -> settle (neutral, loops).
enum Phase { IDLE, WIND, WINDHI, SLAM, RECOIL, RUN, DING, HOP, LAND, GLOW };
const Beat<Phase> PHASE[] = {
    {IDLE, 2}, {WIND, 3}, {WINDHI, 2}, {SLAM, 2}, {RECOIL, 1},
    {RUN, 20},
    {DING, 2}, {HOP, 2}, {LAND, 1}, {GLOW, 3},
};
constexpr int RUN0 = 10, RUN_N = 20, DING0 = 30;

// fake output: {indent, length, colour}
const TermLine OUT[] = {
    {0, 15, C::green}, {2, 24, C::text}, {2, 19, C::muted}, {2, 27, C::muted}, {2, 14, C::text},
    {2, 22, C::muted}, {2, 26, C::blueLight}, {2, 17, C::muted}, {2, 29, C::text}, {2, 12, C::amber},
    {2, 20, C::muted}, {2, 27, C::muted}, {2, 15, C::text}, {2, 24, C::blueLight}, {2, 19, C::muted},
    {2, 26, C::text}, {2, 10, C::muted}, {2, 22, C::muted}, {2, 27, C::text}, {2, 17, C::muted},
    {2, 20, C::blueLight}, {2, 24, C::muted}, {2, 15, C::text}, {2, 26, C::muted},
};
constexpr int NOUT = sizeof(OUT) / sizeof(OUT[0]);
constexpr int ROWS = 9;  // visible output rows

// Floating terminal back-left, plane 'L' (faces the viewer); Clawd looks up-left at it.
void terminal(Ctx &c, int i, Phase ph) {
  const int len = 18, h = 30;
  FacePainter p = plane(c, 62, 62, 12, len, h, 'L', C::bg);
  const int W = p.cols;
  // frame + title bar
  p.rect(0, 0, W, 3, C::surface2);
  p.rect(0, 3, 1, h - 3, C::surface2); p.rect(W - 1, 3, 1, h - 3, C::surface2); p.rect(0, h - 1, W, 1, C::surface2);
  p.px(2, 1, C::red); p.px(4, 1, C::amber); p.px(6, 1, C::green);
  const int u0 = 2, v0 = 4, aw = W - 4;

  // how many lines have been printed
  int printed;
  if (ph == IDLE || ph == WIND || ph == WINDHI) printed = 0;
  else if (ph == SLAM || ph == RECOIL) printed = 1;
  else if (ph == RUN) { int k = i - RUN0; printed = 1 + imin(NOUT - 1, (int)(k * 1.3)); }
  else printed = NOUT;
  const bool done = !(ph == IDLE || ph == WIND || ph == WINDHI || ph == SLAM || ph == RECOIL || ph == RUN);

  // visible window: last ROWS lines (last row reserved for DONE when finished)
  const int avail = done ? ROWS - 1 : ROWS;
  const int first = imax(0, printed - avail);
  for (int r = 0; r < avail && first + r < printed; r++) {
    const TermLine &L = OUT[first + r];
    int v = v0 + r * 2;
    if (L.indent == 0) p.px(u0, v, C::orange);
    int uu = u0 + (L.indent == 0 ? 2 : L.indent);
    p.rect(uu, v, imin(L.len, aw - (uu - u0)), 1, L.col);
  }
  if (printed == 0) {
    // prompt with blinking cursor, waiting for enter
    p.px(u0, v0, C::orange);
    if ((i >> 1) % 2 == 0) p.rect(u0 + 2, v0 - 1, 1, 2, C::text);
  }
  if (done) {
    int v = v0 + (ROWS - 1) * 2;
    p.px(u0, v, C::orange); p.rect(u0 + 2, v, 8, 1, C::green);
  }
  // progress bar at the bottom of the window
  const int bv = h - 4, bw = aw;
  p.rect(u0, bv, bw, 2, C::surface2);
  int fill = 0;
  if (ph == RUN) fill = imin(bw, (int)(((i - RUN0 + 1) / (double)RUN_N) * bw * 0.97));
  else if (done) fill = bw;
  if (fill > 0) p.rect(u0, bv, fill, 2, done ? C::green : C::orange);
  // spinner in the title bar while running
  if (ph == RUN || ph == SLAM || ph == RECOIL) {
    static const int SP[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    p.rect(W - 4 + SP[i % 4][0], 0 + SP[i % 4][1] + 0, 1, 1, C::orange);
  } else if (done) {
    p.rect(W - 4, 1, 2, 1, C::green);
  }
}

const Pt BTN = {168, 89};
// Big red launch button on the desk, right of Clawd (drawn before him).
void button(Ctx &c, bool pressed) {
  int x = BTN.x, y = BTN.y;
  box(c, x, y, 0, 6, 6, 4, SHADE::dark);
  box(c, x, y, 4, 4, 4, pressed ? 1 : 4, SHADE::red);
  if (!pressed) { px(c, x - 2, y - 9, C::blush); px(c, x - 1, y - 9, C::blush); }   // shine
}

// ---------------------------------------------------------------- Clawd poses
ClawdOpts pose(int i, Phase ph, const AnimEnv &env, int N) {
  ClawdOpts o;
  o.x = 134; o.y = 77; o.tint = env.tint();
  o.eyes = {EYE_OPEN, 0, 0}; o.armL = o.armR = ARM_OUT; o.legs = LEGS_STAND;
  switch (ph) {
    case IDLE:   o.eyes = {EYE_OPEN, 1, 1}; break;
    case WIND:   o.eyes = {EYE_OPEN, 1, 1}; o.armL = ARM_OUT; o.armR = ARM_HIGH; o.squash = -3; o.mouth = MOUTH_FLAT; o.shake = -1; break;
    case WINDHI: o.eyes = {EYE_WINCE}; o.armL = ARM_OUT; o.armR = ARM_NONE; o.squash = -4; o.mouth = MOUTH_FLAT; o.shake = -2; break;
    case SLAM:   o.eyes = {EYE_CLOSED}; o.armL = ARM_HIGH; o.armR = ARM_NONE; o.squash = 4; o.mouth = MOUTH_O; o.shake = 3; break;
    case RECOIL: o.eyes = {EYE_WIDE}; o.armL = o.armR = ARM_OUT; o.squash = 2; break;
    case RUN: {
      int k = i - RUN0;
      // eyes zip up and down following the scroll: track the newest line, then flick back up
      static const int GY[] = {-2, -1, 0, 1, 0, -1};
      int gy = cycle(GY, k);
      int lean = k < 3 ? -1 : -2;   // leaning toward the terminal
      bool late = k >= 12;          // impatience builds
      o.shake = lean;
      // (the JS passes eyes.shine = !late, which Iso.drawClawd ignores: wide eyes always shine, open never)
      o.eyes = {k == 9 ? EYE_BLINK : (late ? EYE_OPEN : EYE_WIDE), -1, gy};
      o.legs = late ? (k % 2 ? LEGS_TAP : LEGS_STAND) : (k % 4 < 2 ? LEGS_TAP : LEGS_STAND);
      o.armL = ARM_OUT; o.armR = late ? ARM_DOWN : ARM_OUT;
      o.mouth = late ? MOUTH_WOBBLY : (k % 3 == 0 ? MOUTH_O : MOUTH_NONE);
      break;
    }
    case DING:  o.eyes = {EYE_WIDE, -1, -1}; o.mouth = MOUTH_O; o.squash = 2; break;
    case HOP:   o.eyes = {EYE_HAPPY}; o.mouth = MOUTH_LAUGH; o.armL = o.armR = ARM_UP; o.z = 6; o.squash = -3; o.blush = true; break;
    case LAND:  o.eyes = {EYE_HAPPY}; o.mouth = MOUTH_SMILE; o.armL = o.armR = ARM_OUT; o.squash = 3; o.blush = true; break;
    case GLOW:  o.eyes = {i == N - 1 ? EYE_OPEN : EYE_HAPPY}; o.mouth = MOUTH_SMILE; o.blush = i < N - 1; break;
  }
  return o;
}

void draw(Ctx &c, int i, const AnimEnv &env) {
  const int N = seqLen(PHASE);
  i = ((i % N) + N) % N;
  const Phase ph = seq(PHASE, i);
  const Pal &tint = env.tint();
  drawDesk(c);
  terminal(c, i, ph);
  const bool pressed = ph == SLAM || ph == RECOIL;
  button(c, pressed);
  ClawdRes a = drawClawd(c, pose(i, ph, env, N));
  mug(c, SPOT_mug.x, SPOT_mug.y, {i});

  // FX
  if (ph == WINDHI) {
    // arm cocked high over the button
    for (int k = 0; k <= 4; k++) { rect(c, 150 + 2 * k, 64 - 3 * k, 3, 4, tint.left); rect(c, 152 + 2 * k, 64 - 3 * k, 1, 4, tint.right); }
    box(c, 163, 58, 0, 3, 3, 4, tint);
    fx::ticks(c, 166, 40, "up", C::amber);
  }
  if (ph == SLAM) {
    // the paw comes down on the cap: arm stub from the body + fist on the button
    for (int k = 0; k <= 5; k++) { rect(c, 153 + 2 * k, 66 + 2 * k, 4, 3, tint.left); rect(c, 153 + 2 * k, 69 + 2 * k, 2, 1, tint.right); }
    box(c, BTN.x, BTN.y, 5, 3, 3, 4, tint);
    fx::ticks(c, BTN.x + 12, BTN.y - 10, "right", C::amber);
    fx::ticks(c, BTN.x - 2, BTN.y - 18, "up", C::amber);
  }
  if (ph == RUN) {
    int k = i - RUN0;
    if (k >= 12 && k % 2 == 1) fx::ticks(c, a.feet.x + 16, a.feet.y - 2, "up", C::text);  // tap tap
    if (k >= 14) fx::dots(c, a.head.x + 14, a.head.y - 6, imin(3, (k - 13) >> 1), C::muted);
  }
  if (ph == DING || ph == HOP || ph == LAND) {
    // DING bubble near terminal
    int j = i - DING0;
    int bx = 88, by = 14 - (j > 0 ? 1 : 0);
    text(c, bx, by, "DING", j % 2 ? C::amber : C::text);
    static const int S1[] = {3, 2, 1, 2, 1}, S2[] = {1, 3, 2, 1, 1};
    fx::sparkle(c, bx - 6, by - 3, S1[j], C::amber);
    fx::sparkle(c, bx + 20, by - 2, S2[j], C::amber);
  }
  if (ph == HOP || ph == LAND || ph == GLOW) {
    fx::check(c, a.head.x, a.head.y - 9, C::green);
  }
}
}  // namespace

const IsoAnim anim_14_running_command = {"14-running-command", 10, seqLen(PHASE), true, false, draw, 10, 29};   // calm segment: see anim.h
