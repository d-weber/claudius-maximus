// 17 Delegating: port of iso-clawd/anims/17-delegating.html. Helper count follows env.helpers (page: ?n=1..4).
#include "anim.h"
using namespace iso;

namespace {
// ---------------------------------------------------------------- layout
constexpr int P = 48;                           // loop length (frames, 12 fps = 4 s); divisible by 1..4
constexpr int MAXN = 4;                         // the scene has room for 4 minis (page max)
const Pt BOSS = {138, 62};                      // Clawd, the foreman (back-right, facing left, eyes on the lane)
const Pt S = {62, 68};                          // spawn point (left wing)
Pt lane(int t) { return {S.x + 2 * t, S.y + t}; }   // long down-right lane across the front, minis face right
constexpr int END_T = 24;                       // normal hand-off spot
const Pt TRAY = {138, 80};                      // outbox, behind the lane (no-look toss over the head)
const Pt MUG = SPOT_mug;                        // the mug on its spot (front-right); the rookie veers off the lane into it
const Pt BONK = {MUG.x - 9, MUG.y + 1};         // where the rookie hits the mug (in front of the tray)
const Pt RECOIL = {MUG.x - 29, MUG.y + 1};      // where it lands on its bottom (bounced back toward the lane)
const Pt SPRINT[4] = {{S.x + 12, S.y + 6}, {S.x + 26, S.y + 13}, {106, 85}, {124, 88}};   // lane(6), lane(13), then straight at the mug

// tiny sheet of paper carried over the head / flying
const char *const PAPER[] = {"..pppppp", ".pkkkkpp", "pkkkpps.", "pppppss."};
void drawPaper(Ctx &c, int x, int y) { sprite(c, x - 4, y - 2, PAPER, ISO_MAP({'p', C::paper}, {'k', C::paperShade}, {'s', C::muted})); }

// ---------------------------------------------------------------- mini timelines (local frame f)
struct Arc { float u, h; };
const Arc ARC[] = {{0.25f, 8}, {0.6f, 9}, {0.9f, 4}};   // toss arc samples (u along, h up)

struct Mini {
  bool present;            // false: gone
  int poofOnly;            // >= 0: only a poof at (x, y)
  int x, y, z;
  Facing facing; LegsPose legs; ArmPose armL, armR; Eyes eyes; MouthKind mouth;
  int squash, shake; bool blush;
  bool paper; int paperDx, paperDy;
  bool fly; Arc flyArc; Pt from;
  int poof;                // >= 0: poof frame
  bool landed, sweat, ticks, bonk, stars, question;
  int k, f;
};
Mini none() {
  Mini m = {};
  m.present = false; m.poofOnly = -1; m.poof = -1;
  m.facing = RIGHT; m.legs = LEGS_STAND; m.armL = m.armR = ARM_OUT; m.eyes = {EYE_OPEN, 0, 0}; m.mouth = MOUTH_NONE;
  return m;
}
Mini miniAt(Pt p) { Mini m = none(); m.present = true; m.x = p.x; m.y = p.y; m.from = p; return m; }
Mini poofOnly(int n, Pt p) { Mini m = none(); m.present = true; m.poofOnly = n; m.x = p.x; m.y = p.y; return m; }
void arms(Mini &m, ArmPose l, ArmPose r) { m.armL = l; m.armR = r; }
void holdPaper(Mini &m, int dx = 0, int dy = 0) { m.paper = true; m.paperDx = dx; m.paperDy = dy; }

const int TOSS[] = {0, 1, 2, 3, 4, 4, 5, 6, 7, 8};     // toss phase keys with holds (10 frames)
Mini tossState(Pt p, int j, bool sweatExtra) {       // no-look toss over the head into the outbox, hop, poof (from p)
  if (j < 0 || j >= (int)(sizeof(TOSS) / sizeof(TOSS[0]))) return none();
  const int k = TOSS[j];
  if (k == 7) return poofOnly(2, p);
  if (k == 8) return poofOnly(3, p);
  Mini st = miniAt(p);
  st.sweat = sweatExtra;
  if (k == 0) { st.squash = 2; arms(st, ARM_UP, ARM_UP); st.eyes = {EYE_OPEN, 0, -1}; holdPaper(st, 0, 2); return st; }
  if (k <= 3) { st.squash = k == 1 ? -3 : 0; arms(st, k == 1 ? ARM_UP : ARM_OUT, k == 1 ? ARM_UP : ARM_OUT); st.eyes = {k == 1 ? EYE_CLOSED : EYE_HAPPY}; st.fly = true; st.flyArc = ARC[k - 1]; return st; }
  if (k == 4) { st.z = 4; arms(st, ARM_UP, ARM_UP); st.eyes = {EYE_HAPPY}; st.mouth = MOUTH_O; st.landed = true; st.sweat = false; return st; }
  if (k == 5) { st.z = 1; arms(st, ARM_UP, ARM_UP); st.eyes = {EYE_HAPPY}; st.landed = true; st.sweat = false; return st; }
  st.squash = 3; arms(st, ARM_OUT, ARM_OUT); st.eyes = {EYE_HAPPY}; st.poof = 0; st.landed = true; st.sweat = false;   // k == 6
  return st;
}

Mini spawnState(int f, bool eager) {
  if (f == 0) return poofOnly(1, S);
  Mini st = miniAt(S);
  arms(st, ARM_UP, ARM_UP); holdPaper(st); st.poof = f == 1 ? 2 : f == 2 ? 3 : -1;
  if (f == 1) { st.squash = 3; st.eyes = {EYE_WIDE}; return st; }
  // f 2..3: normal minis salute the boss; the eager rookie crouches to sprint
  if (eager) { st.squash = 3; st.legs = LEGS_STEPA; st.eyes = {EYE_WIDE, 1, 0}; st.mouth = MOUTH_FLAT; return st; }
  arms(st, ARM_OUT, ARM_HIGH); st.eyes = {EYE_HAPPY};   // salute
  return st;
}

constexpr int WALK0 = 4, WALK_N = END_T / 2;    // scurry: 2 steps per frame
Mini normalMini(int f) {
  if (f < WALK0) return spawnState(f, false);
  if (f < WALK0 + WALK_N) {
    int n = f - WALK0;
    Mini st = miniAt(lane(2 * (n + 1)));
    st.legs = n % 2 ? LEGS_STEPA : LEGS_STEPB; arms(st, ARM_UP, ARM_UP);
    st.eyes = {EYE_OPEN, 1, 0}; holdPaper(st, 0, n % 2 ? -1 : 0);
    return st;
  }
  return tossState(lane(END_T), f - WALK0 - WALK_N, false);
}

Mini gagMini(int f) {                 // the eager rookie: sprints past the hand-off and bonks the mug
  if (f < WALK0) return spawnState(f, true);
  if (f < 8) {                        // sprint along SPRINT
    Mini st = miniAt(SPRINT[f - 4]);
    st.legs = f % 2 ? LEGS_STEPA : LEGS_STEPB; arms(st, ARM_UP, ARM_UP); st.squash = -3;
    st.eyes = {EYE_OPEN, 1, 1}; holdPaper(st, -2, -1); st.ticks = true;
    return st;
  }
  const Pt Bk = BONK, R = RECOIL;
  Mini st = miniAt(R);
  if (f == 8) { st.x = Bk.x; st.y = Bk.y; st.squash = 3; st.shake = 1; st.eyes = {EYE_WINCE}; st.mouth = MOUTH_O; arms(st, ARM_UP, ARM_UP); holdPaper(st, 0, -8); st.bonk = true; return st; }
  if (f == 9) { st.x = R.x + 8; st.y = R.y; st.z = 5; st.squash = -3; st.eyes = {EYE_WINCE}; st.mouth = MOUTH_O; arms(st, ARM_HIGH, ARM_HIGH); holdPaper(st, 0, -14); st.bonk = true; return st; }
  if (f == 10) { st.legs = LEGS_SIT; st.eyes = {EYE_WINCE}; st.mouth = MOUTH_O; arms(st, ARM_UP, ARM_UP); holdPaper(st, 0, -10); return st; }
  if (f <= 12) { st.legs = LEGS_SIT; st.eyes = {EYE_CLOSED}; arms(st, ARM_OUT, ARM_OUT); holdPaper(st, 0, f == 11 ? -4 : 0); st.stars = true; return st; }
  if (f == 13) { st.legs = LEGS_SIT; st.eyes = {EYE_OPEN, -1, -2}; arms(st, ARM_OUT, ARM_OUT); holdPaper(st); st.question = true; return st; }
  if (f == 14) { st.squash = -3; st.eyes = {EYE_WIDE, -1, -2}; arms(st, ARM_UP, ARM_UP); holdPaper(st); st.sweat = true; return st; }
  if (f <= 17) { st.eyes = {EYE_HAPPY}; arms(st, ARM_OUT, ARM_HIGH); holdPaper(st); st.blush = f >= 16; return st; }   // salute
  return tossState(RECOIL, f - 18, true);   // sheepish toss from where it stands
}

int helperCount(const AnimEnv &env) { return imax(0, imin(MAXN, env.helpers)); }

// minis for frame i; returns the number filled into out[]
int minisAt(int i, int N, Mini *out) {
  int n = 0;
  const int STEP = N > 0 ? P / N : P;             // spawn interval
  for (int k = 0; k < N; k++) {
    int f = (((i - k * STEP) % P) + P) % P;
    Mini st = k == 0 ? gagMini(f) : normalMini(f);
    if (st.present) { st.k = k; st.f = f; out[n++] = st; }
  }
  return n;
}

// ---------------------------------------------------------------- the boss
struct BossPose { ClawdOpts o; bool bang, point, go; };
BossPose bossPose(int i, int N, const AnimEnv &env) {
  const int g = i;                    // the rookie spawns at frame 0
  const Eyes look = {EYE_OPEN, -1, 2};
  static const LegsPose LEGS[] = {LEGS_STAND, LEGS_STAND, LEGS_TAP};
  static const ArmPose WAVE[] = {ARM_WAVEA, ARM_WAVEB};
  BossPose b = {ClawdOpts(), false, false, false};
  ClawdOpts &o = b.o;
  o.x = BOSS.x; o.y = BOSS.y; o.tint = env.tint(); o.eyes = look; o.legs = cycle(LEGS, i, 2); o.armL = cycle(WAVE, i, 3); o.armR = ARM_OUT;
  if (N > 0) {   // reactions to the rookie's bonk (there is no rookie without helpers)
    if (g >= 8 && g <= 10) { o.eyes = {EYE_WIDE, -1, 2}; o.mouth = MOUTH_O; o.armL = o.armR = ARM_UP; o.squash = g == 8 ? -3 : 0; b.bang = true; return b; }
    if (g >= 11 && g <= 14) { o.eyes = look; o.mouth = MOUTH_FLAT; o.armL = g % 2 ? ARM_FORWARD : ARM_OUT; o.armR = ARM_OUT; b.point = g % 2 == 1; return b; }
    if (g >= 15 && g <= 17) { o.eyes = {EYE_HAPPY}; o.mouth = MOUTH_SMILE; o.blush = true; o.armL = ARM_OUT; o.armR = ARM_HIGH; return b; }
  }
  // conductor beat before each spawn: wind-up, then the downbeat "go!"
  const int STEP = N > 0 ? P / N : P;
  for (int k = 0; k < N; k++) {
    int d = (((i - k * STEP) % P) + P) % P;
    if (d == P - 3 || d == P - 2) { o.armL = o.armR = ARM_UP; o.squash = -3; o.eyes = {EYE_OPEN, -1, -1}; return b; }
    if (d == P - 1) { o.armL = ARM_FORWARD; o.armR = ARM_HIGH; o.squash = 2; o.mouth = MOUTH_O; b.go = true; return b; }
    if (d == 0) { o.armL = ARM_FORWARD; o.armR = ARM_OUT; o.mouth = MOUTH_O; return b; }
  }
  if (i == 38) o.eyes = {EYE_BLINK};
  return b;
}

// ---------------------------------------------------------------- drawing helpers
void drawTray(Ctx &c, bool landed) {
  box(c, TRAY.x, TRAY.y, 0, 5, 5, 3, SHADE::metal);
  PaperOpts p1; p1.a = 4; p1.b = 4; p1.z = 3; p1.lines = false;
  paper(c, TRAY.x, TRAY.y, p1);
  PaperOpts p2; p2.a = 4; p2.b = 4; p2.z = 4 + (landed ? 1 : 0);
  paper(c, TRAY.x, TRAY.y - 1, p2);
}

void drawMini(Ctx &c, const Mini &m) {
  if (m.poofOnly >= 0) { fx::poof(c, m.x, m.y - 8, m.poofOnly, C::violetLight); return; }
  ClawdOpts o;
  o.x = m.x; o.y = m.y; o.z = m.z; o.mini = true; o.tint = SHADE::violet; o.facing = m.facing; o.legs = m.legs;
  o.armL = m.armL; o.armR = m.armR; o.eyes = m.eyes; o.mouth = m.mouth; o.squash = m.squash;
  o.shake = m.shake; o.blush = m.blush;
  ClawdRes a = drawClawd(c, o);
  if (m.paper) drawPaper(c, a.head.x + m.paperDx, a.head.y + m.paperDy);
  if (m.ticks) fx::ticks(c, a.bx - 1, a.face.y - 2, "left", C::violetLight);
  if (m.bonk) fx::sparkle(c, a.bx + a.bw - 2, a.face.y - 6, m.f == 8 ? 3 : 2, C::amber);
  if (m.stars) {                      // two little stars circling the head
    static const int O1[2][2] = {{-7, -3}, {6, -6}}, O0[2][2] = {{-4, -7}, {7, -2}};
    const int (*o2)[2] = m.f % 2 ? O1 : O0;
    for (int q = 0; q < 2; q++) fx::sparkle(c, a.head.x + o2[q][0], a.head.y + o2[q][1], 1, C::amber);
  }
  if (m.question) fx::question(c, a.head.x - 8, a.head.y - 10, true, C::violetLight);
  if (m.sweat) fx::sweat(c, a.head.x - 7, a.head.y + 1);
  if (m.poof >= 0) fx::poof(c, m.x - 10, m.y - 6, m.poof, C::violetLight);
}

void drawFlyingPaper(Ctx &c, const Mini &m) {
  if (!m.fly) return;
  int sx = m.from.x, sy = m.from.y - 17, tx = TRAY.x, ty = TRAY.y - 8;
  drawPaper(c, lerp(sx, tx, m.flyArc.u), lerp(sy, ty, m.flyArc.u) - (int)m.flyArc.h);
}

// ---------------------------------------------------------------- draw
void draw(Ctx &c, int i, const AnimEnv &env) {
  i = ((i % P) + P) % P;
  const int N = helperCount(env);
  drawDesk(c);
  Mini minis[MAXN];
  const int nm = minisAt(i, N, minis);
  bool landed = false; const Mini *bonk = nullptr;
  for (int q = 0; q < nm; q++) { if (minis[q].landed) landed = true; if (!bonk && minis[q].bonk) bonk = &minis[q]; }

  // back to front: tray (behind the lane), boss, then the lane sorted by depth
  drawTray(c, landed);
  const BossPose bp = bossPose(i, N, env);
  ClawdRes b = drawClawd(c, bp.o);
  // items: minis then the mug; stable sort by y (index -1 = mug)
  int idx[MAXN + 1], ys[MAXN + 1], ni = 0;
  for (int q = 0; q < nm; q++) { idx[ni] = q; ys[ni] = minis[q].y; ni++; }
  idx[ni] = -1; ys[ni] = MUG.y + 1; ni++;
  for (int q = 1; q < ni; q++) {   // insertion sort (stable, like Array.prototype.sort)
    int ki = idx[q], ky = ys[q], r = q - 1;
    while (r >= 0 && ys[r] > ky) { idx[r + 1] = idx[r]; ys[r + 1] = ys[r]; r--; }
    idx[r + 1] = ki; ys[r + 1] = ky;
  }
  for (int q = 0; q < ni; q++) {
    if (idx[q] < 0) mug(c, MUG.x + (bonk ? (bonk->f == 8 ? 2 : 1) : 0), MUG.y, {i});
    else drawMini(c, minis[idx[q]]);
  }

  // FX on top
  for (int q = 0; q < nm; q++) drawFlyingPaper(c, minis[q]);
  if (landed) fx::check(c, TRAY.x + 16, TRAY.y - 8, C::green);
  if (bp.bang) fx::bang(c, b.head.x - 4, b.head.y - 8);
  if (bp.point && b.hasHandL) fx::ticks(c, b.handL.x - 2, b.handL.y, "left", env.tint().top);
  if (bp.go && b.hasHandL) fx::sparkle(c, b.handL.x - 6, b.handL.y - 2, 2, C::violetLight);
}
}  // namespace

const IsoAnim anim_17_delegating = {"17-delegating", 12, P, true, true, draw};
