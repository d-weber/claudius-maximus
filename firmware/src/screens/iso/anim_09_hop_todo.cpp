// 09 Hop todo: port of iso-clawd/anims/09-hop-todo.html.
#include "anim.h"
using namespace iso;

namespace {
// Todo clipboard propped upright beside Clawd on the screen-left (drawn before him), facing the viewer.
const Pt CB = {SPOT_left.x + 4, SPOT_left.y + 8};
const int ROWS[3] = {5, 10, 15};   // v of each todo row on the board
// Upright clipboard on a little wooden foot. Returns the face painter.
FacePainter todoBoard(Ctx &c, int x, int y, int z, int done) {
  box(c, x + 1, y, z, 9, 2, 2, SHADE::wood);                        // foot
  FacePainter p = plane(c, x, y - 1, z + 2, 8, 22, 'L', C::woodLeft);   // board
  p.rect(0, 0, p.cols, 1, C::woodTop);
  p.rect(1, 2, p.cols - 2, 19, C::paper);                              // sheet
  p.rect(1, 20, p.cols - 2, 1, C::paperShade);
  p.rect(6, 0, 4, 3, C::greyLight); p.rect(7, 0, 2, 1, C::grey);       // clip
  for (int k = 0; k < 3; k++) {
    int v = ROWS[k];
    bool isDone = k < done;
    p.rect(2, v, 3, 3, isDone ? C::green : C::paperShade);             // tick box
    if (!isDone) p.rect(3, v + 1, 1, 1, C::paper);
    p.rect(6, v, k == 2 ? 6 : 8, 1, isDone ? C::paperShade : C::muted);   // task line
    p.rect(6, v + 2, k == 1 ? 5 : 4, 1, isDone ? C::paperShade : C::dim);
  }
  return p;
}

// Big "stamped" check: 2x version of fx.check with a dark outline so it pops off the desk.
const char *const BIG_CHECK[] = {
    "..........KKK",
    ".........KGGK",
    "........KGGK.",
    ".KKK...KGGK..",
    "KGGK..KGGK...",
    ".KGGKKGGK....",
    "..KGGGGK.....",
    "...KGGK......",
    "....KK.......",
};
const char *const MID_CHECK[] = {
    "......KK",
    ".....KGK",
    "KK..KGK.",
    "KGKKGK..",
    ".KGGK...",
    "..KK....",
};
void stampCheck(Ctx &c, int x, int y, int size) {
  if (size == 2) sprite(c, x - 6, y - 4, BIG_CHECK, ISO_MAP({'G', C::green}, {'K', C::greenDark}));
  else if (size == 1) sprite(c, x - 4, y - 3, MID_CHECK, ISO_MAP({'G', C::green}, {'K', C::greenDark}));
}

// Pose table.
struct Pose { EyeShape eye; int gx, gy; ArmPose arms; MouthKind mouth; int squash, z; bool blush; };
constexpr Pose LOOK = {EYE_OPEN, -1, 1, ARM_OUT, MOUTH_NONE, 0, 0, false};
constexpr Pose NOD = {EYE_OPEN, -1, 2, ARM_OUT, MOUTH_SMILE, 0, 0, false};
constexpr Pose CROUCH = {EYE_CLOSED, 0, 0, ARM_DOWN, MOUTH_SMILE, 3, 0, false};
constexpr Pose UP = {EYE_HAPPY, 0, 0, ARM_HIGH, MOUTH_SMILE, -3, 6, false};
constexpr Pose APEX = {EYE_HAPPY, 0, 0, ARM_UP, MOUTH_LAUGH, -1, 13, true};
constexpr Pose FALL = {EYE_HAPPY, 0, 0, ARM_HIGH, MOUTH_LAUGH, 0, 5, true};
constexpr Pose LANDP = {EYE_CLOSED, 0, 0, ARM_OUT, MOUTH_SMILE, 4, 0, true};
constexpr Pose SETTLE = {EYE_HAPPY, 0, 0, ARM_OUT, MOUTH_SMILE, 1, 0, true};
constexpr Pose YEP = {EYE_WINK, 0, 0, ARM_OUT, MOUTH_SMILE, 0, 0, true};
constexpr Pose REST = {EYE_OPEN, 0, 0, ARM_OUT, MOUTH_NONE, 0, 0, false};
// [pose, holds]; ~1.0 s at 10 fps.
const Beat<Pose> TL[] = {
    {LOOK, 1}, {NOD, 1},          // glance at the list
    {CROUCH, 1},                  // anticipation
    {UP, 1}, {APEX, 2}, {FALL, 1},
    {LANDP, 1},                   // THUMP -> clipboard jumps, check stamped
    {SETTLE, 1}, {YEP, 1},
    {REST, 1},
};
const int LAND = 7;   // frame index of 'land'

void draw(Ctx &c, int i, const AnimEnv &env) {
  drawDesk(c);

  // --- clipboard: bounces on the landing impact, row 2 checks off at landing
  int cbZ = i == LAND ? 3 : i == LAND + 1 ? 1 : 0;
  if (cbZ) shadow(c, CB.x + 1, CB.y, 9, 2);
  FacePainter board = todoBoard(c, CB.x, CB.y, cbZ, i >= LAND ? 2 : 1);
  Pt row2 = board.at(3, ROWS[1]);

  // --- Clawd
  const Pose &p = seq(TL, i);
  ClawdOpts o;
  o.x = SPOT_clawd.x; o.y = SPOT_clawd.y; o.z = p.z;
  o.tint = env.tint();
  o.eyes = {p.eye, p.gx, p.gy};
  o.armL = o.armR = p.arms;
  o.mouth = p.mouth;
  o.squash = p.squash;
  o.blush = p.blush;
  ClawdRes a = drawClawd(c, o);
  mug(c, SPOT_mug.x, SPOT_mug.y, {i});   // the mug spot is in front of him

  // --- landing dust ticks under the feet
  if (i == LAND) {
    fx::ticks(c, a.feet.x - 22, a.feet.y - 2, "left", C::muted);
    fx::ticks(c, a.feet.x + 22, a.feet.y - 2, "right", C::muted);
  }

  // --- stamped check: pops big out of the clipboard, then sparkles away
  int sx = row2.x + 1, sy = row2.y;
  if (i == LAND) {                                    // stamped right onto row 2
    stampCheck(c, sx + 2, sy - 2, 2);
    fx::sparkle(c, sx - 10, sy - 8, 1, C::amber);
    fx::sparkle(c, sx + 14, sy - 12, 1, C::green);
  } else if (i == LAND + 1) {                         // pops up off the board
    stampCheck(c, sx + 2, sy - 16, 1);
    fx::sparkle(c, sx - 12, sy - 14, 2, C::amber);
    fx::sparkle(c, sx + 14, sy - 20, 2, C::green);
  } else if (i == LAND + 2) {                         // ...and becomes a sparkle
    fx::sparkle(c, sx + 2, sy - 22, 3, C::green);
    fx::sparkle(c, sx - 12, sy - 18, 1, C::amber);
  } else if (i == LAND + 3) {
    fx::sparkle(c, sx + 2, sy - 25, 1, C::green);
  }

  // --- tiny motion ticks at take-off (JS: C.orangeLight; tinted so they match a grey Clawd too)
  if (i == 3) fx::ticks(c, a.head.x, a.head.y - 6, "up", env.tint().top);
}
}  // namespace

const IsoAnim anim_09_hop_todo = {"09-hop-todo", 10, seqLen(TL), false, false, draw};
