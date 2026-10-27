// 02 Sleep: port of iso-clawd/anims/02-sleep.html. The page's two variants ('sleep' = host connected,
// 'offline' = no host) are selected by env.grey (grey tint == offline).
#include "anim.h"
#include <stdlib.h>
using namespace iso;

namespace {
struct Variant { Col z, bubble, bubbleFill, bubbleHi, shadow, sticky; bool blush, lamp, steam; };
constexpr Variant SLEEP = {C::muted, C::blueLight, C::blue, C::text, C::woodShadow, C::amber, true, true, true};
constexpr Variant OFFLINE = {C::dim, C::paperShade, C::greyLight, C::text, C::woodRight, C::paperShade, false, false, false};
constexpr Pal OFFLINE_DESK = {C::woodShadow, C::woodRight, C::surface2, C::woodLeft};

// ---- breathing / snore timeline (36 frames = 3.6 s @ 10 fps) ----
// phase per frame: inhale -> exhale (bubble inflates, head droops) -> POP (snap back, note flaps) -> settle
enum Note { NOTE_FLAT, NOTE_UP, NOTE_MID };
struct Pose { int squash, gazeY, bubble; MouthKind mouth; int shake; Note note; bool snap; };
constexpr Beat<Pose> PHASE[] = {
    {{0, 0, 0, MOUTH_NONE, 0, NOTE_FLAT, false}, 3},    // in0: inhale: body stretches 1 px taller, bubble sucked in
    {{-1, 0, 0, MOUTH_NONE, 0, NOTE_FLAT, false}, 8},   // in1
    {{0, 0, 2, MOUTH_O, 0, NOTE_FLAT, false}, 4},       // ex0: exhale: bubble grows 2..8, head droops
    {{1, 1, 4, MOUTH_O, 0, NOTE_FLAT, false}, 5},       // ex1
    {{1, 1, 6, MOUTH_O, 0, NOTE_FLAT, false}, 5},       // ex2
    {{2, 2, 8, MOUTH_O, 0, NOTE_FLAT, false}, 4},       // ex3
    {{2, 2, -1, MOUTH_O, 0, NOTE_FLAT, false}, 1},      // pop! body jolts, sticky note flips up
    {{-2, -1, 0, MOUTH_FLAT, 1, NOTE_UP, true}, 2},     // snap
    {{0, 0, 0, MOUTH_NONE, 0, NOTE_MID, false}, 2},     // flap
    {{1, 0, 0, MOUTH_NONE, 0, NOTE_FLAT, false}, 2},    // set: settle back to neutral nap
};
const int N = seqLen(PHASE);   // 36

// filled pixel disc with outline + highlight (radius r)
void bubble(Ctx &c, int cx, int cy, int r, Col edge, Col hi, Col fill) {
  const float R2 = r * r + r * 0.8f;
  if (r >= 3)
    for (int y = -r + 1; y < r; y++)
      for (int x = -r + 1; x < r; x++)
        if (x * x + y * y <= R2 && (x + y + 64) % 2 == 0 && (y + 64) % 2 == 0) px(c, cx + x, cy + y, fill);
  for (int y = -r; y <= r; y++)
    for (int x = -r; x <= r; x++) {
      int d = x * x + y * y;
      if (d <= R2) {
        int ax = abs(x) + 1, ay = abs(y) + 1;
        bool inner = ax * ax + y * y > R2 || x * x + ay * ay > R2;
        if (inner) px(c, cx + x, cy + y, edge);
      }
    }
  if (r >= 2) px(c, cx - imax(1, r >> 1), cy - imax(1, r >> 1), hi);
}
// pop burst: 8 two-pixel dashes radiating from a ring of radius ~8, plus droplets
void popBurst(Ctx &c, int cx, int cy, Col col) {
  static const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
  for (auto &d : dirs) {
    int dx = d[0], dy = d[1];
    for (int r = 8; r <= 9; r++) px(c, cx + dx * (dx && dy ? r - 2 : r), cy + dy * (dx && dy ? r - 2 : r), col);
  }
  px(c, cx - 2, cy - 1, col); px(c, cx + 2, cy + 1, col); px(c, cx + 1, cy - 2, col);   // droplets
}

// three z's drifting up on staggered phases; pure function of i (period = N)
void zees(Ctx &c, int hx, int hy, int i, const Variant &V) {
  for (int k = 0; k < 3; k++) {
    int p = (i + k * 12) % N;                         // 0..35
    int rise = p >> 1;                                // 0..17 px, 1 px every 2 frames
    int x = hx + 10 + (rise >> 1) + ((p >> 3) % 2);   // drift up-right with a little wobble
    int y = hy - 2 - rise;
    int size = p < 10 ? 1 : p < 24 ? 2 : 3;
    if (p > 32) continue;                             // fade out (gap) before wrap
    fx::zee(c, x, y, size, p > 28 ? C::dim : V.z);
  }
}

void draw(Ctx &c, int i, const AnimEnv &env) {
  const Variant &V = env.grey ? OFFLINE : SLEEP;
  const Pose &p = seq(PHASE, i);

  // 1) scene, back to front
  if (env.grey) { DeskOpts d; d.pal = &OFFLINE_DESK; d.grain = false; drawDesk(c, &d); }
  else drawDesk(c);
  lamp(c, SPOT_left.x - 6, SPOT_left.y - 2, true, V.lamp);
  MugOpts m; m.frame = i >> 1; m.steam = V.steam;
  mug(c, SPOT_mug.x, SPOT_mug.y, m);

  // 2) Clawd, sitting nap
  ClawdOpts o;
  o.x = SPOT_clawd.x; o.y = SPOT_clawd.y;
  o.tint = env.tint();
  o.legs = LEGS_SIT;
  o.armL = o.armR = ARM_DOWN;
  o.eyes = {EYE_CLOSED, 0, p.gazeY};
  o.mouth = p.mouth;
  o.blush = V.blush;
  o.squash = p.squash;
  o.shake = p.shake;
  o.shadowColor = V.shadow;
  ClawdRes a = drawClawd(c, o);

  // sticky note on his head: flat, flips upright on the pop, half-way on the way back
  int nx = a.top.x, ny = a.top.y + 1;
  StickyOpts s; s.color = V.sticky;
  if (p.note == NOTE_UP) { s.plane = 'L'; s.z = 4; sticky(c, nx - 1, ny, s); }
  else if (p.note == NOTE_MID) { s.z = 3; sticky(c, nx + 1, ny, s); }
  else sticky(c, nx, ny, s);

  // 3) front props
  paper(c, SPOT_frontLeft.x - 4, SPOT_frontLeft.y);

  // 4) FX: snore bubble at the mouth (face looks down-left, so the bubble hangs left of the mouth)
  int bx = a.mouth.x - 3, by = a.mouth.y + 2;
  if (p.bubble > 0) bubble(c, bx - p.bubble, by + (p.bubble >> 2), p.bubble, V.bubble, V.bubbleHi, V.bubbleFill);
  if (p.bubble < 0) popBurst(c, bx - 9, by + 2, V.bubble);
  if (p.snap) fx::ticks(c, a.head.x, a.head.y - 6, "up", V.z);
  zees(c, a.head.x, a.head.y, i, V);
}
}  // namespace

const IsoAnim anim_02_sleep = {"02-sleep", 10, N, true, false, draw, 0, 10};   // calm segment: see anim.h
