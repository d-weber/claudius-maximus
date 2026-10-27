// 08 Wink: port of iso-clawd/anims/08-wink.html.
#include "anim.h"
using namespace iso;

namespace {
// Poses. Facing left (toward the viewer); screen-right arm is the front arm, so it does the finger gun.
// gun: 0 none, 1 drawn, 2 recoil. click: muzzle spark (the 'wink' beat). blow: smoke wisp off the fingertip.
struct Pose {
  EyeShape eye; int gx, gy; EyeShape eyeR; ArmPose armL, armR; MouthKind mouth; bool blush; int squash, shake;
  int gun; bool click, blow;
};
constexpr Pose REST = {EYE_OPEN, 0, 0, EYE_DEFAULT, ARM_OUT, ARM_OUT, MOUTH_NONE, false, 0, 0, 0, false, false};
// casual "too cool" glance away, arm cocks back
constexpr Pose COOL = {EYE_OPEN, 1, -1, EYE_DEFAULT, ARM_OUT, ARM_HIGH, MOUTH_NONE, false, 1, 0, 0, false, false};
// snap to the viewer: lean in (shake toward down-left), stretch, gun drawn
constexpr Pose SNAP = {EYE_WIDE, 0, 0, EYE_DEFAULT, ARM_OUT, ARM_UP, MOUTH_NONE, false, -3, -2, 1, false, false};
// WINK + click: recoil, blush, smile
constexpr Pose WINK = {EYE_OPEN, -1, 0, EYE_HAPPY, ARM_OUT, ARM_UP, MOUTH_SMILE, true, 2, -2, 2, true, false};
constexpr Pose HOLD = {EYE_OPEN, -1, 0, EYE_HAPPY, ARM_OUT, ARM_UP, MOUTH_SMILE, true, 0, -1, 1, false, false};
constexpr Pose HOLD_B = {EYE_OPEN, -1, 0, EYE_HAPPY, ARM_OUT, ARM_UP, MOUTH_SMILE, true, 0, -1, 1, false, true};
// settle back
constexpr Pose SETTLE = {EYE_BLINK, 0, 0, EYE_DEFAULT, ARM_OUT, ARM_OUT, MOUTH_NONE, true, 1, 0, 0, false, false};
// [pose, hold]; total 11 frames @ 10 fps = 1.1 s
const Beat<Pose> TL[] = {{REST, 1}, {COOL, 2}, {SNAP, 1}, {WINK, 1}, {HOLD, 2}, {HOLD_B, 2}, {SETTLE, 1}, {REST, 1}};
// sparkle "ting" timeline, frame index -> size (0 = none)
const int TING[11] = {0, 0, 0, 0, 1, 3, 2, 1, 0, 0, 0};

// finger gun off the raised screen-right hand: thumb up, index finger pointing out to the right
// L = lit, M = mid, D = shade
const char *const GUN[] = {
    ".LL.........",
    ".LM.........",
    ".LM.........",
    "LLMLLLLLLLL.",
    "MMMMMMMMMMMD",
    "MMMMD.......",
    "DDDD........",
};
const char *const GUN_RECOIL[] = {   // "click": thumb hammer drops, barrel kicks up
    "..........L.",
    ".LL......LMD",
    "LLMLLLLLLMD.",
    "MMMMMMMMMD..",
    "MMMMD.......",
    "DDDD........",
    "............",
};
void finger(Ctx &c, Pt h, int kind, const Pal &T) {
  sprite(c, h.x - 2, h.y - 4, kind == 2 ? GUN_RECOIL : GUN, 7, ISO_MAP({'L', T.top}, {'M', T.left}, {'D', T.right}));
}

void draw(Ctx &c, int i, const AnimEnv &env) {
  const Pal &T = env.tint();
  drawDesk(c);
  mug(c, SPOT_mug.x, SPOT_mug.y, {i});

  const Pose &p = seq(TL, i);
  ClawdOpts o;
  o.x = SPOT_clawd.x; o.y = SPOT_clawd.y;
  o.tint = T;
  o.eyes = {p.eye, p.gx, p.gy, EYE_DEFAULT, p.eyeR};
  o.armL = p.armL; o.armR = p.armR;
  o.mouth = p.mouth;
  o.blush = p.blush;
  o.squash = p.squash; o.shake = p.shake;
  ClawdRes a = drawClawd(c, o);

  // finger gun on the front (screen-right) hand
  if (p.gun && a.hasHandR) {
    finger(c, a.handR, p.gun, T);
    if (p.click) {   // "click": muzzle spark at the fingertip
      fx::sparkle(c, a.handR.x + 14, a.handR.y - 4, 2, C::amber);
    }
    if (p.blow) {    // a wisp of "smoke" off the fingertip
      int k = i % 2;
      px(c, a.handR.x + 11, a.handR.y - 3 - k, C::muted);
      px(c, a.handR.x + 12, a.handR.y - 5 - k, C::dim);
      px(c, a.handR.x + 11, a.handR.y - 7 - k, C::dim);
    }
  }

  // "ting!" glint off the open (screen-left) eye
  int s = (i >= 0 && i < 11) ? TING[i] : 0;
  if (s) fx::sparkle(c, a.eyeL.x - 5, a.eyeL.y - 7, s, C::text);
  if (s == 3) text(c, a.eyeL.x - 26, a.eyeL.y - 18, "TING", C::amber);
}
}  // namespace

const IsoAnim anim_08_wink = {"08-wink", 10, seqLen(TL), false, false, draw};
