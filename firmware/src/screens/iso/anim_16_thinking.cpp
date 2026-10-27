// 16 Thinking: port of iso-clawd/anims/16-thinking.html.
#include "anim.h"
using namespace iso;

namespace {
// ---------------------------------------------------------------- local sprites
const char *const GEAR0[] = {"....#....", "..#####..", ".##...##.", ".#.....#.", "##..#..##", ".#.....#.", ".##...##.", "..#####..", "....#...."};
const char *const GEAR1[] = {".#.....#.", "..#####..", ".##...##.", "##.....##", ".#..#..#.", "##.....##", ".##...##.", "..#####..", ".#.....#."};
const char *const QMARK[] = {".###.", "#...#", "...#.", "..#..", ".....", "..#.."};
const char *const SQUIG0[] = {".#...#...#.", "#.#.#.#.#.#", "...#...#..."};
const char *const SQUIG1[] = {"#...#...#..", ".#.#.#.#.#.", "..#...#...#"};
// light bulb, 9 wide x 13 tall. g = glass, h = highlight, o = outline, b = base, d = base dark
const char *const BULB[] = {
    "..ooooo..",
    ".ohggggo.",
    "ohhgggggo",
    "ohggggggo",
    "ogggggggo",
    "ogggggggo",
    ".ogggggo.",
    "..ogggo..",
    "...ogo...",
    "..bbbbb..",
    "..ddddd..",
    "..bbbbb..",
    "...ddd...",
};

enum Bulb { BULB_NONE, BULB_OFF, BULB_DIM, BULB_ON };
void bulb(Ctx &c, int x, int y, Bulb state) {
  if (state == BULB_OFF) sprite(c, x - 4, y - 6, BULB, ISO_MAP({'o', C::dim}, {'g', C::surface2}, {'h', C::muted}, {'b', C::grey}, {'d', C::greyDark}));
  else if (state == BULB_DIM) sprite(c, x - 4, y - 6, BULB, ISO_MAP({'o', C::amberDark}, {'g', C::amberDark}, {'h', C::amber}, {'b', C::grey}, {'d', C::greyDark}));
  else sprite(c, x - 4, y - 6, BULB, ISO_MAP({'o', C::amber}, {'g', C::amber}, {'h', C::text}, {'b', C::greyLight}, {'d', C::grey}));
  if (state == BULB_ON) {
    // rays
    rect(c, x, y - 11, 1, 3, C::amber);
    rect(c, x - 9, y - 2, 3, 1, C::amber);
    rect(c, x + 7, y - 2, 3, 1, C::amber);
    px(c, x - 6, y - 8, C::amber); px(c, x - 7, y - 9, C::amber);
    px(c, x + 6, y - 8, C::amber); px(c, x + 7, y - 9, C::amber);
  } else if (state == BULB_DIM) {
    px(c, x, y - 10, C::amberDark);
    px(c, x - 8, y - 2, C::amberDark); px(c, x + 8, y - 2, C::amberDark);
  }
}

// hand-on-chin: a small voxel fist + forearm standing proud of the face (real 3D boxes so they
// read against the face). Built from face texel points, pushed out along the face normal.
void chinHand(Ctx &c, const FacePainter &F, Facing facing, const Pal &tint) {
  const int cols = F.cols, mid = cols >> 1; const bool L = facing == LEFT;
  const Pal &pal = tint;
  const Pal hi = {tint.top, tint.top, tint.left, NONE};
  // box whose footprint runs along the face (len units) and sticks out `out` units, bottom at face row vb
  auto slab = [&](int u, int vb, int len, int out, int h, const Pal &p) {
    Pt q = F.at(u, vb);
    int nx = L ? -out : out, ny = out >> 1;          // half the depth along the normal
    if (L) box(c, q.x + nx, q.y + ny, 0, len, out, h, p);
    else box(c, q.x + nx, q.y + ny, 0, out, len, h, p);
  };
  // forearm: from the near (front) edge of the face to the fist, low on the face
  slab(L ? cols - 8 : 8, 25, 4, 2, 3, pal);
  // fist propping the chin
  slab(L ? mid + 2 : mid - 2, 23, 3, 3, 6, hi);
}

// ---------------------------------------------------------------- timeline
// Positions along the desk's world-x axis (2 px right, 1 px down per unit).
const Pt A = {110, 71}, B = {132, 82};
Pt at(Pt p, Pt q, int k, int n) { return {lerp(p.x, q.x, k / (float)n), lerp(p.y, q.y, k / (float)n)}; }

enum Puff { PUFF_NONE, PUFF_DOTS, PUFF_GEAR, PUFF_Q, PUFF_SQUIG };
constexpr int ARM_UNSET = -1;
struct Frame {
  Pt pos; Facing facing; Eyes eyes; bool chin; LegsPose legs; int bob; Puff puff; int dots, g, q; int squash;
  int armL, armR;        // ARM_UNSET = derive from chin/facing like the JS
  MouthKind mouth; Bulb bulb; bool spark; int smoke;   // smoke -1 = none
};
const Eyes gazeUL = {EYE_OPEN, -1, -2};
const Eyes gazeUR = {EYE_OPEN, 1, -2};

// 5) lightbulb gag (11 f): anticipation, flicker, almost on, fizzle, deflate, back to chin
struct Gag { Bulb bulb; Eyes eyes; bool chin; int armL, armR; int squash; MouthKind mouth; bool spark; int smoke; };
const Gag GAG[] = {
    {BULB_OFF, {EYE_OPEN, 0, -2}, true, ARM_UNSET, ARM_UNSET, 0, MOUTH_NONE, false, -1},
    {BULB_DIM, {EYE_WIDE, 0, -1}, true, ARM_UNSET, ARM_UNSET, 0, MOUTH_NONE, false, -1},
    {BULB_OFF, {EYE_WIDE, 0, -1}, true, ARM_UNSET, ARM_UNSET, 0, MOUTH_NONE, false, -1},
    {BULB_ON, {EYE_WIDE, 0, -2}, false, ARM_OUT, ARM_UP, -3, MOUTH_O, false, -1},
    {BULB_ON, {EYE_WIDE, 0, -2}, false, ARM_OUT, ARM_UP, -3, MOUTH_O, true, -1},
    {BULB_DIM, {EYE_WIDE, 0, -2}, false, ARM_OUT, ARM_UP, -2, MOUTH_O, false, -1},
    {BULB_OFF, {EYE_OPEN, 0, -2}, false, ARM_OUT, ARM_HIGH, 0, MOUTH_FLAT, false, 0},
    {BULB_OFF, {EYE_CLOSED}, false, ARM_DOWN, ARM_DOWN, 2, MOUTH_FLAT, false, 1},
    {BULB_OFF, {EYE_CLOSED}, false, ARM_DOWN, ARM_DOWN, 2, MOUTH_FLAT, false, 2},
    {BULB_NONE, {EYE_BLINK, -1, -2}, true, ARM_UNSET, ARM_UNSET, 0, MOUTH_NONE, false, 3},
    {BULB_NONE, {EYE_OPEN, -1, -2}, true, ARM_UNSET, ARM_UNSET, 0, MOUTH_NONE, false, -1},
};
constexpr int N = 9 + 6 + 6 + 6 + 11;   // 38 frames @ 10 fps = 3.8 s

// Each frame: full description, a pure function of i (the JS builds the same table once).
Frame frameAt(int i) {
  Frame f = {A, LEFT, gazeUL, false, LEGS_STAND, 0, PUFF_NONE, 0, 0, 0, 0, ARM_UNSET, ARM_UNSET, MOUTH_NONE, BULB_NONE, false, -1};
  if (i < 9) {
    // 1) ponder at A: hand on chin, eyes up-left, "..." builds in the thought puff (9 f)
    int k = i;
    f.eyes = k == 6 ? Eyes{EYE_BLINK, -1, -2} : gazeUL;
    f.chin = true; f.legs = k >= 4 && k < 6 ? LEGS_TAP : LEGS_STAND; f.puff = PUFF_DOTS; f.dots = imin(3, 1 + (k >> 1));
  } else if (i < 15) {
    // 2) pace to B facing right, eyes still up; gear turns in the puff (6 f)
    int k = i - 9;
    f.pos = at(A, B, k + 1, 6); f.facing = RIGHT; f.eyes = gazeUR; f.chin = true;
    f.legs = k % 2 ? LEGS_STEPB : LEGS_STEPA; f.bob = k % 2 ? 0 : 1; f.puff = PUFF_GEAR; f.g = k;
  } else if (i < 21) {
    // 3) stop at B: look up, foot tap, "?" in the puff (6 f)
    int k = i - 15;
    f.pos = B; f.facing = RIGHT; f.eyes = k == 3 ? Eyes{EYE_BLINK, 1, -2} : Eyes{EYE_OPEN, 0, -2};
    f.chin = true; f.legs = k % 2 ? LEGS_TAP : LEGS_STAND; f.squash = k == 0 ? 1 : 0; f.puff = PUFF_Q; f.q = k;
  } else if (i < 27) {
    // 4) turn, pace back to A facing left, squiggles (6 f)
    int k = i - 21;
    f.pos = at(B, A, k + 1, 6); f.facing = LEFT; f.eyes = gazeUL; f.chin = true;
    f.legs = k % 2 ? LEGS_STEPB : LEGS_STEPA; f.bob = k % 2 ? 0 : 1; f.puff = PUFF_SQUIG; f.g = k;
  } else {
    const Gag &g = GAG[i - 27];
    f.pos = A; f.facing = LEFT; f.legs = LEGS_STAND;
    f.bulb = g.bulb; f.eyes = g.eyes; f.chin = g.chin; f.armL = g.armL; f.armR = g.armR; f.squash = g.squash;
    f.mouth = g.mouth; f.spark = g.spark; f.smoke = g.smoke;
  }
  return f;
}

void draw(Ctx &c, int i, const AnimEnv &env) {
  const Frame f = frameAt(((i % N) + N) % N);
  drawDesk(c);
  notepad(c, SPOT_right.x + 8, SPOT_right.y - 2, true);

  const bool chin = f.chin;
  ClawdOpts o;
  o.x = f.pos.x; o.y = f.pos.y; o.z = f.bob; o.facing = f.facing; o.eyes = f.eyes; o.legs = f.legs;
  o.squash = f.squash; o.mouth = f.mouth; o.tint = env.tint();
  if (f.armL != ARM_UNSET) { o.armL = (ArmPose)f.armL; o.armR = (ArmPose)f.armR; }
  else if (chin) { o.armL = f.facing == LEFT ? ARM_OUT : ARM_NONE; o.armR = f.facing == LEFT ? ARM_NONE : ARM_OUT; }
  else o.armL = o.armR = ARM_OUT;
  ClawdRes a = drawClawd(c, o);
  mug(c, SPOT_mug.x, SPOT_mug.y, {i, true});   // the mug spot is in front of him
  if (chin) chinHand(c, a.F, f.facing, env.tint());

  pencil(c, SPOT_frontLeft.x - 6, SPOT_frontLeft.y + 4, 5, 'x');

  // thought puff: above and behind the head, on the side he looks toward
  if (f.puff != PUFF_NONE) {
    int hx = a.head.x, hy = a.head.y;
    bool left = f.facing == LEFT;
    const int pw = 28, ph = 14;
    int px0 = left ? hx - pw - 2 : hx + 4, py0 = imax(1, hy - ph - 9);
    fx::PuffOpts po; po.thought = true; po.tail = left ? 'r' : 'l';
    fx::Rect inner = fx::puff(c, px0, py0, pw, ph, po);
    int cx = inner.x + (inner.w >> 1), cy = inner.y + (inner.h >> 1);
    if (f.puff == PUFF_DOTS) fx::dots(c, cx + 1, cy - 1, f.dots, C::text);
    else if (f.puff == PUFF_GEAR) {
      sprite(c, cx - 10, cy - 4, f.g % 2 ? GEAR1 : GEAR0, ISO_MAP({'#', C::muted}));
      sprite(c, cx + 1, cy - 4, (f.g + 1) % 2 ? GEAR1 : GEAR0, ISO_MAP({'#', C::blueLight}));
    } else if (f.puff == PUFF_Q) {
      int bobq = f.q % 2;
      sprite(c, cx - 7, cy - 3 - bobq, QMARK, ISO_MAP({'#', C::amber}));
      sprite(c, cx + 2, cy - 3 - (1 - bobq), QMARK, ISO_MAP({'#', C::muted}));
    } else if (f.puff == PUFF_SQUIG) sprite(c, cx - 5, cy - 1, f.g % 2 ? SQUIG1 : SQUIG0, ISO_MAP({'#', C::violetLight}));
  }

  // light bulb gag
  int bx = a.head.x - 1, by = imax(12, a.head.y - 14);
  if (f.bulb != BULB_NONE) bulb(c, bx, by, f.bulb);
  if (f.spark) { fx::sparkle(c, bx - 10, by - 6, 2, C::amber); fx::sparkle(c, bx + 11, by - 1, 1, C::text); }
  if (f.smoke >= 0) {
    fx::poof(c, bx + 1, by - 11 - f.smoke * 2, f.smoke, C::grey);
    if (f.smoke < 2) { px(c, bx - 2, by - 7, C::dim); px(c, bx + 3, by - 8, C::dim); }
  }
}
}  // namespace

const IsoAnim anim_16_thinking = {"16-thinking", 10, N, true, false, draw, 0, 8};   // calm segment: see anim.h
