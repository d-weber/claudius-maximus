// 03 Waiting: port of iso-clawd/anims/03-waiting.html (NEEDS YOU: blocked on the human).
#include "anim.h"
using namespace iso;

namespace {
// Pose. hey: which side the "HEY!" shout pops on ('l', 'r', 0). sign: PLS sign lift (0 none, 1 half, 2 full).
struct Pose { EyeShape eye; int gy; MouthKind mouth; bool blush; ArmPose al, ar; LegsPose legs; int sq, z, shake; char hey; int wave, sign; };
// Expression presets: ALERT = wide eyes + "o"; PLEAD = wide eyes looking up, wobbly mouth, blush.
constexpr Pose alert   = {EYE_WIDE, 0, MOUTH_O, false, ARM_OUT, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, 0};
constexpr Pose alertB  = {EYE_WIDE, 0, MOUTH_O, false, ARM_OUT, ARM_OUT, LEGS_TAP, 0, 0, 0, 0, 0, 0};
constexpr Pose crouch  = {EYE_CLOSED, 0, MOUTH_O, false, ARM_DOWN, ARM_DOWN, LEGS_STAND, 3, 0, 0, 0, 0, 0};
constexpr Pose launch  = {EYE_WIDE, 0, MOUTH_O, false, ARM_HIGH, ARM_HIGH, LEGS_STAND, -3, 5, 0, 0, 0, 0};
constexpr Pose apexL   = {EYE_WIDE, 0, MOUTH_O, false, ARM_UP, ARM_HIGH, LEGS_STAND, 0, 10, 0, 'l', 0, 0};
constexpr Pose apexR   = {EYE_WIDE, 0, MOUTH_O, false, ARM_WAVEB, ARM_HIGH, LEGS_STAND, 0, 10, 0, 'r', 0, 0};
constexpr Pose fall    = {EYE_WIDE, 0, MOUTH_O, false, ARM_HIGH, ARM_HIGH, LEGS_STAND, 0, 4, 0, 0, 0, 0};
constexpr Pose land    = {EYE_WINCE, 0, MOUTH_O, false, ARM_OUT, ARM_OUT, LEGS_STAND, 4, 0, 0, 0, 0, 0};
constexpr Pose waveA   = {EYE_WIDE, 0, MOUTH_LAUGH, false, ARM_WAVEB, ARM_OUT, LEGS_STAND, 0, 0, -1, 0, 1, 0};
constexpr Pose waveB   = {EYE_WIDE, 0, MOUTH_LAUGH, false, ARM_HIGH, ARM_OUT, LEGS_STAND, 0, 0, 1, 0, 2, 0};
constexpr Pose signUp  = {EYE_WIDE, 0, MOUTH_O, false, ARM_HIGH, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, 1};
constexpr Pose plead   = {EYE_WIDE, -2, MOUTH_WOBBLY, true, ARM_UP, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, 2};
constexpr Pose pleadT  = {EYE_WIDE, -2, MOUTH_WOBBLY, true, ARM_UP, ARM_OUT, LEGS_TAP, 0, 0, 0, 0, 0, 2};
constexpr Pose pleadBl = {EYE_BLINK, 0, MOUTH_WOBBLY, true, ARM_UP, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, 2};   // eyes replaced: gazeY lost, as in the JS
constexpr Pose signDn  = {EYE_WIDE, 0, MOUTH_O, false, ARM_HIGH, ARM_OUT, LEGS_STAND, 0, 0, 0, 0, 0, 1};

// 29 frames = 2.9 s @ 10 fps
constexpr Beat<Pose> TIMELINE[] = {
    {alert, 1}, {alertB, 1},
    // hop 1: "HEY!"
    {crouch, 1}, {launch, 1}, {apexL, 2}, {fall, 1}, {land, 1},
    // hop 2: "HEY!"
    {crouch, 1}, {launch, 1}, {apexR, 2}, {fall, 1}, {land, 1},
    // friendly wave at the viewer
    {waveA, 2}, {waveB, 1}, {waveA, 1}, {waveB, 2},
    // the gag: up comes a tiny "PLS" sign + puppy eyes, foot tapping
    {signUp, 1}, {plead, 2}, {pleadT, 1}, {pleadBl, 1}, {plead, 1}, {pleadT, 1}, {plead, 1},
    {signDn, 1},
};

// Tiny hand-held sign: a stick from the hand plus an amber card with "PLS" (or "?" while it rises).
void drawSign(Ctx &c, Pt hand, int lift, int frame) {
  const char *txt = lift == 2 ? "PLS" : "?";
  int tw = textWidth(txt);
  int w = tw + 6, h = 10;
  int stick = lift == 2 ? 6 : 3;
  int cx = hand.x, top = hand.y - stick - h;
  rect(c, cx, hand.y - stick, 1, stick, C::woodLeft);              // stick
  int x0 = cx - (w >> 1);
  rect(c, x0 + 1, top, w - 2, h, C::amberDark);                    // card border
  rect(c, x0, top + 1, w, h - 2, C::amberDark);
  rect(c, x0 + 1, top + 1, w - 2, h - 2, C::amber);
  rect(c, x0 + 1, top + h - 2, w - 2, 1, C::amberDark);            // card thickness
  text(c, x0 + 3, top + 2, txt, C::eye);
  rect(c, cx - 1, hand.y - stick, 3, 1, C::amberDark);
  // tiny wiggle ticks so it reads as "shaking the sign"
  if (lift == 2 && frame % 2 == 0) fx::ticks(c, x0 - 1, top + 4, "left", C::amber);
}

void draw(Ctx &c, int i, const AnimEnv &env) {
  const Pose &p = seq(TIMELINE, i);

  drawDesk(c);
  // amber-headed notepad on the desk: the pending question
  notepad(c, SPOT_backRight.x + 8, SPOT_backRight.y + 2);
  MugOpts m; m.frame = i; m.steam = true;
  mug(c, SPOT_mug.x, SPOT_mug.y, m);

  ClawdOpts o;
  o.x = SPOT_clawd.x; o.y = SPOT_clawd.y; o.z = p.z;
  o.tint = env.tint();
  o.eyes = {p.eye, 0, p.gy};
  o.mouth = p.mouth;
  o.blush = p.blush;
  o.armL = p.al; o.armR = p.ar;
  o.legs = p.legs;
  o.squash = p.sq;
  o.shake = p.shake;
  ClawdRes a = drawClawd(c, o);

  paper(c, SPOT_frontLeft.x - 4, SPOT_frontLeft.y);

  // ---- FX
  // "!" badge: bobs on a 4-frame cycle, rides the head through the hops
  static const int BOB[] = {0, -1, -2, -1};
  int bob = cycle(BOB, i);
  int bx = a.head.x, by = a.head.y - 9 + bob;
  fx::bang(c, bx, by, true);
  // urgency pulse around the badge, on and off
  if (i % 4 < 2) fx::ticks(c, bx, by - 6, "up", C::amber);

  // HEY! shouts at the hop apexes
  if (p.hey == 'l') {
    text(c, a.bx - 20, a.head.y + 12, "HEY!", C::amber);
    fx::ticks(c, a.bx - 22, a.head.y + 14, "left", C::amber);
  } else if (p.hey == 'r') {
    text(c, a.bx + a.bw - 2, a.head.y + 2, "HEY!", C::amber);
    fx::ticks(c, a.bx + a.bw + 14, a.head.y + 4, "right", C::amber);
  }

  // wave motion ticks beside the waving hand
  if (p.wave && a.hasHandL) fx::ticks(c, a.handL.x - 3, a.handL.y, "left", C::amber);

  // hand-held sign
  if (p.sign && a.hasHandL) drawSign(c, a.handL, p.sign, i);

  // pleading: a tiny sweat drop for "polite but urgent"
  if (p.sign == 2) fx::sweat(c, a.head.x + 14, a.head.y + 4);
}
}  // namespace

const IsoAnim anim_03_waiting = {"03-waiting", 10, seqLen(TIMELINE), true, false, draw, 19, 25};   // calm segment: see anim.h
