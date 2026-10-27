// 04 Happy Done: port of iso-clawd/anims/04-happy-done.html (DONE! one-shot, ends on neutral).
#include "anim.h"
#include <math.h>
using namespace iso;

namespace {
// Math.round (half toward +inf), for the confetti ballistics.
inline int jsRound(float v) { return (int)floorf(v + 0.5f); }

// ---- poses -----------------------------------------------------------------
// RAISE/RAISE2 = custom straight-up screen-right arm (the lib's right-side 'up' reads as sideways in iso).
constexpr int RAISE = 100, RAISE2 = 101;
struct Frame { EyeShape eye; MouthKind mouth; bool blush; ArmPose al; int ar; int z, sq; };
// HAPPY = ^ ^ eyes, laugh, blush, arms \o/ (l 'up', r RAISE); YAY2 = l 'waveB', r RAISE2 (flail variant).
// One entry per frame (24 frames @10fps = 2.4 s). z = lift px, sq = squash.
constexpr Frame F[] = {
    /* 0  */ {EYE_OPEN, MOUTH_NONE, false, ARM_OUT, ARM_OUT, 0, 0},                 // neutral
    /* 1  */ {EYE_CLOSED, MOUTH_SMILE, false, ARM_DOWN, ARM_DOWN, 0, 2},            // crouch...
    /* 2  */ {EYE_CLOSED, MOUTH_SMILE, false, ARM_DOWN, ARM_DOWN, 0, 5},            // ...deeper
    /* 3  */ {EYE_HAPPY, MOUTH_LAUGH, true, ARM_UP, RAISE, 6, -4},                  // LAUNCH
    /* 4  */ {EYE_HAPPY, MOUTH_LAUGH, true, ARM_UP, RAISE, 15, 0},                  // apex (confetti)
    /* 5  */ {EYE_HAPPY, MOUTH_LAUGH, true, ARM_WAVEB, RAISE2, 17, 0},
    /* 6  */ {EYE_HAPPY, MOUTH_LAUGH, true, ARM_UP, RAISE, 9, -2},
    /* 7  */ {EYE_HAPPY, MOUTH_LAUGH, true, ARM_HIGH, ARM_HIGH, 0, 6},              // SPLAT
    /* 8  */ {EYE_HAPPY, MOUTH_LAUGH, true, ARM_UP, RAISE, 0, 2},
    /* 9  */ {EYE_HAPPY, MOUTH_LAUGH, true, ARM_UP, RAISE, 6, -3},                  // hop 2
    /* 10 */ {EYE_HAPPY, MOUTH_LAUGH, true, ARM_WAVEB, RAISE2, 10, 0},
    /* 11 */ {EYE_HAPPY, MOUTH_LAUGH, true, ARM_UP, RAISE, 6, 0},
    /* 12 */ {EYE_HAPPY, MOUTH_LAUGH, true, ARM_HIGH, ARM_HIGH, 0, 4},
    /* 13 */ {EYE_HAPPY, MOUTH_LAUGH, true, ARM_UP, RAISE, 3, -2},                  // hop 3
    /* 14 */ {EYE_HAPPY, MOUTH_LAUGH, true, ARM_WAVEB, RAISE2, 5, 0},
    /* 15 */ {EYE_HAPPY, MOUTH_LAUGH, true, ARM_UP, RAISE, 2, 0},
    /* 16 */ {EYE_HAPPY, MOUTH_LAUGH, true, ARM_HIGH, ARM_HIGH, 0, 2},
    /* 17 */ {EYE_WINK, MOUTH_SMILE, true, ARM_HIGH, RAISE, 0, -1},                 // proud
    /* 18 */ {EYE_WINK, MOUTH_SMILE, true, ARM_HIGH, RAISE, 0, 0},
    /* 19 */ {EYE_WINK, MOUTH_SMILE, true, ARM_HIGH, RAISE, 0, 0},
    /* 20 */ {EYE_WINK, MOUTH_SMILE, true, ARM_HIGH, RAISE, 0, 0},
    /* 21 */ {EYE_OPEN, MOUTH_SMILE, true, ARM_OUT, ARM_OUT, 0, 0},
    /* 22 */ {EYE_OPEN, MOUTH_NONE, false, ARM_OUT, ARM_OUT, 0, 0},
    /* 23 */ {EYE_OPEN, MOUTH_NONE, false, ARM_OUT, ARM_OUT, 0, 0},                 // neutral (chains to idle)
};
constexpr int NFRAMES = sizeof(F) / sizeof(F[0]);

// Mug hops in sympathy, one frame late and smaller.
constexpr int MUG_Z[NFRAMES] = {0, 0, 0, 0, 5, 10, 7, 0, 0, 0, 4, 6, 2, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0};

// Sparkles popping round the head: {frame born, dx, dy (from head), color}. Life = 4 frames.
struct Spark { int born, dx, dy; Col col; };
constexpr Spark SPARKS[] = {
    {4, -36, -6, C::amber}, {4, 36, 0, C::text},
    {6, 40, -14, C::orangeLight}, {7, -40, 6, C::amber},
    {9, -30, -16, C::text}, {10, 38, 6, C::amber},
    {12, -38, -8, C::orangeLight}, {13, 32, -16, C::text},
    {16, 36, -4, C::amber}, {17, -36, 2, C::text},
};
constexpr int SPARK_SIZE[4] = {1, 3, 2, 1};

// Confetti: burst from above the head at frame 4, deterministic ballistic pixels.
constexpr Col CONF_COLORS[7] = {C::amber, C::orangeLight, C::text, C::green, C::violet, C::blueLight, C::blush};
constexpr int CONF_VX[18] = {-6, -5, -4, -2, -1, 1, 2, 4, 5, 6, -7, 7, -3, 3, -1, 1, -5, 5};
constexpr int CONF_VY[18] = {-3, -4, -5, -5, -4, -4, -5, -4, -3, -3, -2, -2, -3, -3, -5, -5, -1, -1};
constexpr int CONF_START = 4;
constexpr Pt CONF_ORIGIN = {120, 16};

void drawConfetti(Ctx &c, int i) {
  int k = i - CONF_START;
  if (k < 0 || k > 12) return;
  for (int n = 0; n < 18; n++) {
    int vx = CONF_VX[n], vy = CONF_VY[n], ph = n % 2;
    // gravity 1 px/frame^2, horizontal drag after 4 frames
    float kx = imin(k, 4) + imax(0, k - 4) * 0.4f;
    int x = jsRound(CONF_ORIGIN.x + vx * kx);
    int y = jsRound(CONF_ORIGIN.y + vy * k + (k * k) * 0.5f);
    if (y > 104) continue;
    // flutter: alternate between a 2x1 and 1x2 chip
    bool flat = ((k + ph) & 1) == 0;
    rect(c, x, y, flat ? 2 : 1, flat ? 1 : 2, CONF_COLORS[n % 7]);
  }
}

// Clawd with an optional straight-up screen-right arm. The JS probes the 'out' hand anchor on a scratch
// canvas; here the probe draws with an all-NONE tint (every lib draw skips NONE), so it costs no buffer.
// The real draw uses r:'none' and adds an upright arm box at the shoulder.
ClawdRes drawClawdX(Ctx &c, ClawdOpts o, int ar) {
  if (ar != RAISE && ar != RAISE2) { o.armR = (ArmPose)ar; return drawClawd(c, o); }
  ClawdOpts probe = o;
  probe.armR = ARM_OUT;
  probe.tint = {NONE, NONE, NONE, NONE}; probe.shadow = false; probe.blush = false;
  probe.mouth = MOUTH_NONE; probe.eyeColor = NONE; probe.shineOff = true;
  ClawdRes pr = drawClawd(c, probe);
  o.armR = ARM_NONE;
  ClawdRes a = drawClawd(c, o);
  Pt h = pr.handR;
  int tall = ar == RAISE ? 21 : 18, dx = ar == RAISE ? 0 : 2;
  int bx = h.x + 2 + dx, by = h.y - 7;
  box(c, bx, by, 0, 2, 2, tall, o.tint);
  a.hasHandR = true; a.handR = {bx, by - tall};
  return a;
}

// Green "done" badge with a dark check, centred. pop: 0 = small, 1 = big (overshoot), 2 = normal.
void doneBadge(Ctx &c, int x, int y, int pop) {
  if (pop == 0) { fx::check(c, x, y, C::green); return; }
  static const char *const BIG[] = {"..ggggggg..", ".ggggggggg.", "ggggggggddg", "gggggggddgg", "gddgggddggg", "ggddgddgggg", "gggdddggggg", ".gggdgggggg", "..ggggggg.."};
  static const char *const NORMAL[] = {"..ggggg..", ".gggggdg.", "ggggggddg", "gddgdddgg", "ggdddgggg", "gggdggggg", ".ggggggg.", "..ggggg.."};
  bool big = pop == 1;
  const char *const *s = big ? BIG : NORMAL;
  int w = big ? 11 : 9, n = big ? 9 : 8;
  sprite(c, x - (w >> 1), y - (n >> 1), s, n, ISO_MAP({'g', C::green}, {'d', C::bg}));
}

void draw(Ctx &c, int i, const AnimEnv &env) {
  drawDesk(c);

  // mug at the right, behind Clawd; it hops too
  int mz = MUG_Z[i];
  MugOpts m; m.frame = i; m.steam = mz == 0; m.z = mz;
  mug(c, SPOT_mug.x, SPOT_mug.y, m);
  if (mz >= 5) fx::ticks(c, SPOT_mug.x, SPOT_mug.y - 20 - mz, "up", C::muted);

  const Frame &f = F[i];
  ClawdOpts o;
  o.x = SPOT_clawd.x; o.y = SPOT_clawd.y; o.z = f.z; o.squash = f.sq;
  o.tint = env.tint();
  o.eyes = {f.eye, 0, 0};
  o.mouth = f.mouth;
  o.blush = f.blush;
  o.armL = f.al;
  o.legs = LEGS_STAND;
  ClawdRes a = drawClawdX(c, o, f.ar);

  // take-off / landing dust
  if (i == 3) { fx::ticks(c, a.feet.x - 22, a.feet.y + 2 - f.z, "left", C::muted); fx::ticks(c, a.feet.x + 22, a.feet.y + 2 - f.z, "right", C::muted); }
  if (i == 7 || i == 8) {
    int d = i == 7 ? 0 : 1;
    fx::poof(c, a.feet.x - 26 - d * 2, a.feet.y + 2, d + 1, C::dim);
    fx::poof(c, a.feet.x + 26 + d * 2, a.feet.y + 2, d + 1, C::dim);
  }

  // green check badge above head (pops in at apex, bobs with him, fades by shrinking)
  if (i >= 4 && i <= 20) {
    int pop = i == 4 ? 0 : i == 5 ? 1 : (i >= 19 ? 0 : 2);
    int bob = (i >= 6 && i <= 18) ? ((i >> 1) & 1) : 0;
    doneBadge(c, a.head.x, a.head.y - 9 - bob, pop);
  }

  // sparkles
  for (const Spark &s : SPARKS) {
    int k = i - s.born;
    if (k >= 0 && k < 4) fx::sparkle(c, a.head.x + s.dx, a.head.y + 14 + s.dy, SPARK_SIZE[k], s.col);
  }

  drawConfetti(c, i);

  // wink glint at the closed (right) eye during the proud pose
  if (i >= 17 && i <= 20) {
    static const int GLINT[4] = {1, 3, 2, 1};
    Pt e = a.eyeR;
    fx::sparkle(c, e.x + 9, e.y - 7, GLINT[i - 17], C::text);
  }
}
}  // namespace

const IsoAnim anim_04_happy_done = {"04-happy-done", 10, NFRAMES, false, false, draw};
