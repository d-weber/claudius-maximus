// 07 Giggle tap: port of iso-clawd/anims/07-giggle-tap.html.
#include "anim.h"
using namespace iso;

namespace {
// One entry per frame (one-shot, 14 frames @ 10 fps = 1.4 s).
// finger: tip offset above the top face (-1 = gone). he: 'l' | 'r' giggle text side, 0 none.
struct Frame {
  EyeShape eye; int gy; MouthKind mouth; int squash, z; LegsPose legs; ArmPose armL, armR; int shake; bool blush;
  int finger; bool ticks; char he;
};
// Giggle face pieces: eyes happy, mouth laugh, blush. L_UP = {l high, r out}, R_UP = {l out, r high}.
const Frame FRAMES[] = {
    // 0 anticipation: fingertip descends, Clawd glances up
    {EYE_OPEN, -2, MOUTH_NONE, 0, 0, LEGS_STAND, ARM_OUT, ARM_OUT, 0, false, 12, false, 0},
    // 1 POKE: contact, squash
    {EYE_WINCE, 0, MOUTH_O, 3, 0, LEGS_STAND, ARM_OUT, ARM_OUT, 0, false, 0, true, 0},
    // 2 deeper press
    {EYE_WINCE, 0, MOUTH_WOBBLY, 5, 0, LEGS_STAND, ARM_UP, ARM_UP, 0, false, 0, true, 0},
    // 3 rebound: stretch, eyes wide, finger retreats
    {EYE_WIDE, 0, MOUTH_O, -3, 2, LEGS_STEPA, ARM_OUT, ARM_OUT, 0, false, 14, false, 0},
    // 4..9 giggle hops with alternating arm flaps
    {EYE_HAPPY, 0, MOUTH_LAUGH, 0, 7, LEGS_STEPA, ARM_HIGH, ARM_OUT, 0, true, -1, false, 0},
    {EYE_HAPPY, 0, MOUTH_LAUGH, 2, 0, LEGS_STAND, ARM_OUT, ARM_HIGH, 0, true, -1, false, 'r'},
    {EYE_HAPPY, 0, MOUTH_LAUGH, 0, 6, LEGS_STEPB, ARM_HIGH, ARM_OUT, 0, true, -1, false, 0},
    {EYE_HAPPY, 0, MOUTH_LAUGH, 2, 0, LEGS_STAND, ARM_OUT, ARM_HIGH, 0, true, -1, false, 'l'},
    {EYE_HAPPY, 0, MOUTH_LAUGH, 0, 4, LEGS_STEPA, ARM_HIGH, ARM_OUT, 0, true, -1, false, 0},
    {EYE_HAPPY, 0, MOUTH_LAUGH, 1, 0, LEGS_STAND, ARM_OUT, ARM_HIGH, 1, true, -1, false, 'r'},
    // 10..11 giggle winds down: wiggle, then a satisfied squint
    {EYE_HAPPY, 0, MOUTH_LAUGH, 0, 0, LEGS_STAND, ARM_OUT, ARM_OUT, -1, true, -1, false, 0},
    {EYE_HAPPY, 0, MOUTH_SMILE, 0, 0, LEGS_STAND, ARM_OUT, ARM_OUT, 0, true, -1, false, 0},
    // 12 eyes reopen, content smile
    {EYE_BLINK, 0, MOUTH_SMILE, 0, 0, LEGS_STAND, ARM_OUT, ARM_OUT, 0, false, -1, false, 0},
    // 13 neutral (+ content smile)
    {EYE_OPEN, 0, MOUTH_SMILE, 0, 0, LEGS_STAND, ARM_OUT, ARM_OUT, 0, false, -1, false, 0},
};
constexpr int NFRAMES = sizeof(FRAMES) / sizeof(FRAMES[0]);

// Pixel fingertip coming straight down from the top of the stage; tip at (x, tipY).
void finger(Ctx &c, int x, int tipY) {
  const int w = 11, x0 = x - 5, bodyBot = tipY - 4;
  if (bodyBot > 0) {
    rect(c, x0, 0, w, bodyBot, C::muted);                 // outline
    rect(c, x0 + 1, 0, w - 2, bodyBot, C::paper);         // skin
    rect(c, x0 + w - 3, 0, 2, bodyBot, C::paperShade);    // shaded side
    rect(c, x0 + 1, 0, 1, bodyBot, C::text);              // highlight
    if (bodyBot - 12 > 0) rect(c, x0 + 3, bodyBot - 12, 4, 1, C::paperShade);   // knuckle crease
    if (bodyBot - 14 > 0) rect(c, x0 + 4, bodyBot - 14, 3, 1, C::paperShade);
  }
  static const char *const tip[] = {
      "mhppppppssm",
      "mhppppppssm",
      ".mppppppsm.",
      "..mmmmmmm..",
  };
  sprite(c, x0, tipY - 4, tip, ISO_MAP({'m', C::muted}, {'p', C::paper}, {'s', C::paperShade}, {'h', C::text}));
}

void draw(Ctx &c, int i, const AnimEnv &env) {
  const Frame &F = FRAMES[((i % NFRAMES) + NFRAMES) % NFRAMES];
  drawDesk(c);
  mug(c, SPOT_mug.x, SPOT_mug.y, {i, true});

  ClawdOpts o;
  o.x = SPOT_clawd.x; o.y = SPOT_clawd.y; o.z = F.z;
  o.tint = env.tint();
  o.eyes = {F.eye, 0, F.gy};
  o.mouth = F.mouth;
  o.squash = F.squash; o.shake = F.shake;
  o.legs = F.legs;
  o.armL = F.armL; o.armR = F.armR;
  o.blush = F.blush;
  ClawdRes a = drawClawd(c, o);

  // fingertip (lands slightly toward the face side of the top)
  if (F.finger >= 0) finger(c, a.top.x - 2, a.top.y - F.finger);
  if (F.ticks) {
    fx::ticks(c, a.top.x - 16, a.top.y - 2, "left", C::text);
    fx::ticks(c, a.top.x + 12, a.top.y - 2, "right", C::text);
  }

  // giggle text
  if (F.he == 'l') text(c, a.head.x - 30, a.head.y - 2, "HE", C::amber);
  if (F.he == 'r') text(c, a.head.x + 22, a.head.y - 4, "HE", C::amber);

  // hearts rising from frame 5
  if (i >= 5 && i <= 12) {
    int k = i - 5;
    fx::heart(c, SPOT_clawd.x + 30 + (k & 1), 36 - k * 3, k < 1 ? 1 : 2, C::red);
    if (k >= 2 && k <= 7) fx::heart(c, SPOT_clawd.x - 44 - ((k + 1) & 1), 38 - (k - 2) * 3, k < 3 ? 1 : 2, C::blush);
  }
  // little sparkle as he settles
  if (i == 11 || i == 12) fx::sparkle(c, a.head.x + 16, a.head.y + 2, i == 11 ? 2 : 1, C::amber);
}
}  // namespace

const IsoAnim anim_07_giggle_tap = {"07-giggle-tap", 10, NFRAMES, false, false, draw};
