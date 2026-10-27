// 11 Writing code: port of iso-clawd/anims/11-writing-code.html.
#include "anim.h"
using namespace iso;

namespace {
// Layout: laptop beside Clawd (screen faces down-right so we can read it), Clawd facing left,
// eyes glued to the screen, screen-left hand over the keyboard. Mug on his right.
const Pt CL = {138, 74};     // Clawd feet
const Pt LAP = {100, 70};    // laptop footprint centre
const Pt MUG = SPOT_mug;     // mug on its spot (drawn after Clawd)

// ---- timeline: one descriptor per frame (JS builds it from PLAN; expanded here) ----
// PLAN: type x12 (rate 2), hover x2, wind1 x1, wind2 x2, slam x1 (NL), recoil x1, proud x2,
//       type x4 (2), reach x1 (1), sip x7 (0.5), putback x1 (1), type x5 (2)  = 39 frames
enum Kind { K_TYPE, K_HOVER, K_WIND1, K_WIND2, K_SLAM, K_RECOIL, K_PROUD, K_REACH, K_SIP, K_PUTBACK };
struct Fr { Kind kind; int k; };
const Fr FR[] = {
    {K_TYPE, 0}, {K_TYPE, 1}, {K_TYPE, 2}, {K_TYPE, 3}, {K_TYPE, 4}, {K_TYPE, 5}, {K_TYPE, 6}, {K_TYPE, 7},
    {K_TYPE, 8}, {K_TYPE, 9}, {K_TYPE, 10}, {K_TYPE, 11},
    {K_HOVER, 0}, {K_HOVER, 1}, {K_WIND1, 0}, {K_WIND2, 0}, {K_WIND2, 1}, {K_SLAM, 0}, {K_RECOIL, 0},
    {K_PROUD, 0}, {K_PROUD, 1},
    {K_TYPE, 0}, {K_TYPE, 1}, {K_TYPE, 2}, {K_TYPE, 3},
    {K_REACH, 0},
    {K_SIP, 0}, {K_SIP, 1}, {K_SIP, 2}, {K_SIP, 3}, {K_SIP, 4}, {K_SIP, 5}, {K_SIP, 6},
    {K_PUTBACK, 0},
    {K_TYPE, 0}, {K_TYPE, 1}, {K_TYPE, 2}, {K_TYPE, 3}, {K_TYPE, 4},
};
constexpr int NFR = sizeof(FR) / sizeof(FR[0]);   // 39

// Code lines (period of 4 per loop): indent, length, colour
struct CodeLine { int indent, len; Col col; };
const CodeLine LINES[4] = {{0, 10, C::violet}, {2, 14, C::text}, {2, 12, C::amber}, {4, 11, C::blueLight}};

// Screen state per frame: {line: absolute line index, col: px typed on it} (JS integrates the typing
// rate over FR; precomputed here). 4 lines are written per loop, so the loop seam matches frame 0.
struct Scr { int line, col; };
const Scr SCR[NFR] = {
    {0, 2}, {0, 4}, {0, 6}, {0, 8}, {0, 10}, {1, 2}, {1, 4}, {1, 6}, {1, 8}, {1, 10}, {1, 12}, {1, 14},
    {1, 14}, {1, 14}, {1, 14}, {1, 14}, {1, 14}, {2, 0}, {2, 0}, {2, 0}, {2, 0},
    {2, 2}, {2, 4}, {2, 6}, {2, 8}, {2, 9},
    {2, 9}, {2, 10}, {2, 10}, {2, 11}, {2, 11}, {2, 12}, {2, 12},
    {3, 1}, {3, 3}, {3, 5}, {3, 7}, {3, 9}, {3, 11},
};

void drawCode(const FacePainter &p, const Area &A, int i) {
  const Scr &s = SCR[i];
  const int rows = 5;                               // visible rows (v = 1,3,5,7,9)
  for (int r = 0; r < rows; r++) {
    int ln = s.line - (rows - 1 - r);               // current line on the bottom row
    const CodeLine &L = LINES[((ln % 4) + 4) % 4];
    int n = ln == s.line ? s.col : L.len;
    int v = A.v + 1 + r * 2;
    if (n > 0) p.rect(A.u + 1 + L.indent, v, n, 1, L.col);
    if (ln == s.line) {
      Kind kd = FR[i].kind;
      bool typing = kd == K_TYPE || kd == K_SIP || kd == K_REACH;
      if (typing || (i >> 1) % 2 == 0) p.rect(A.u + 2 + L.indent + n, v - 1, 1, 2, C::text);
    }
  }
}

// Bigger laptop than the stock prop (same look), screen facing down-right.
void bigLaptop(Ctx &c, int x, int y, int z, int i, bool flash) {
  const int A = 12, B = 8;
  Obj o(c, x, y, A, B, RIGHT, z);
  o.box(0, 0, 0, A, B, 2, SHADE::metal, 0);
  o.box(0, 0, 2, A, 1, 14, {C::muted, C::grey, C::greyDark, NONE}, 1);
  o.draw();
  for (int a = 1; a < A - 1; a++) for (int b = 2; b < B - 1; b++) if ((a + b) % 2 == 0) {
    Pt p = o.pt(a + 0.5f, b + 0.5f, 2);
    rect(c, p.x - 1, p.y, 2, 1, C::greyDark);
  }
  const FacePainter *s = o.res[1].front();
  if (!s) return;
  Area area = {2, 1, s->cols - 4, 11};
  s->rect(area.u, area.v, area.w, area.h, flash ? C::glow : C::screen);
  if (!flash) drawCode(*s, area, i);
}

// 2D mug sprite for when it's in his hand (matches prop.mug colours). (x, y) = centre.
void heldMug(Ctx &c, int x, int y, int i, bool steam) {
  static const char *const rows[] = {
      ".tttttt..",
      "tcccccct.",
      "llllrrrhh",
      "llllrrr.h",
      "llllrrr.h",
      "llllrrrhh",
      "llllrrr..",
      ".lllrr...",
  };
  sprite(c, x - 4, y - 5, rows,
         ISO_MAP({'t', C::paper}, {'c', C::woodRight}, {'l', SHADE::paper.left}, {'r', SHADE::paper.right}, {'h', SHADE::paper.right}));
  if (steam) {
    int f = i % 4;
    static const int wig[4] = {0, 1, 0, -1};
    for (int k = 0; k < 3; k++) {
      px(c, x - 2 + wig[(f + k) % 4], y - 7 - k * 2, k ? C::dim : C::muted);
      px(c, x + 1 + wig[(f + k + 2) % 4], y - 8 - k * 2, k ? C::dim : C::muted);
    }
  }
}

// Keyboard clacks: tiny specks flying up/out from the typing hand
void clacks(Ctx &c, int hx, int hy, int i) {
  static const int spots[6][2] = {{-3, -2}, {1, -3}, {-2, -3}, {2, -2}, {-4, -1}, {0, -4}};
  for (int age = 0; age < 3; age++) {
    int j = i - age;
    const int *sp = spots[((j % 6) + 6) % 6];
    int dx = sp[0], dy = sp[1];
    int m = age + 1;
    int x = hx - 2 + dx * m, y = hy - 3 + dy * m;
    if (age == 0) rect(c, x, y, 2, 1, C::text);
    else if (age == 1) rect(c, x, y, 1, 1, C::text);
    else px(c, x, y, C::muted);
  }
}

void tongue(const FacePainter &fp, int k) {
  // flat concentrated mouth with the tongue poking out of the screen-left corner, wiggling
  fp.rect(12, 18, 7, 1, C::eye);
  int u = 12 + (k % 2);
  fp.rect(u, 19, 3, 2, C::red);
  fp.px(u + 1, 19, C::blush);
  fp.rect(u + (k % 2 ? 0 : 1), 21, 2, 1, C::red);
}

void draw(Ctx &c, int i, const AnimEnv &env) {
  i = ((i % NFR) + NFR) % NFR;
  const Fr &f = FR[i];
  Kind kind = f.kind;
  drawDesk(c);

  // ---- laptop (behind Clawd) ----
  int lapZ = kind == K_RECOIL ? 1 : 0;
  bigLaptop(c, LAP.x, LAP.y, lapZ, i, kind == K_SLAM);

  // ---- Clawd pose ----
  bool alt = i % 2 == 0;
  ClawdOpts o;
  o.x = CL.x; o.y = CL.y;
  o.tint = env.tint();
  o.eyes = {EYE_OPEN, -1, 1};
  o.armL = alt ? ARM_TYPEA : ARM_TYPEB; o.armR = alt ? ARM_TYPEB : ARM_TYPEA;
  bool tongueOn = false, mugHeld = false;
  switch (kind) {
    case K_TYPE:
      tongueOn = true;
      if (i == 7) o.eyes = {EYE_BLINK, -1, 1};
      o.squash = i % 2;                // bouncing along with the keystrokes
      if (i % 4 == 1) o.shake = -1;    // a little jitter from the frenzy
      break;
    case K_HOVER:
      o.armL = ARM_TYPEB; o.armR = ARM_TYPEB;
      o.eyes = {EYE_OPEN, -1, 0};
      o.mouth = MOUTH_FLAT;
      break;
    case K_WIND1:
      o.armL = ARM_HIGH; o.armR = ARM_TYPEB;
      o.eyes = {EYE_OPEN, -1, -1};
      o.squash = -2; o.mouth = MOUTH_O;
      break;
    case K_WIND2:
      o.armL = ARM_UP; o.armR = ARM_OUT;
      o.eyes = {EYE_WIDE, -1, -1};
      o.squash = -4; o.mouth = MOUTH_O;
      break;
    case K_SLAM:
      o.armL = ARM_TYPEA; o.armR = ARM_OUT;
      o.eyes = {EYE_WINCE, 0, 0};
      o.squash = 5; o.mouth = MOUTH_FLAT;
      break;
    case K_RECOIL:
      o.armL = ARM_TYPEB; o.armR = ARM_OUT;
      o.eyes = {EYE_HAPPY, 0, 0};
      o.squash = 2; o.mouth = MOUTH_LAUGH;
      break;
    case K_PROUD:
      o.armL = ARM_TYPEB; o.armR = ARM_TYPEB;
      o.eyes = {EYE_HAPPY, 0, 0};
      o.mouth = MOUTH_SMILE; o.blush = true;
      break;
    case K_REACH:
    case K_PUTBACK:
      o.armL = alt ? ARM_TYPEA : ARM_TYPEB; o.armR = ARM_FORWARD;
      break;
    case K_SIP:
      o.armL = (i >> 1) % 2 ? ARM_TYPEA : ARM_TYPEB; o.armR = ARM_FORWARD;
      mugHeld = true;
      if (f.k == 4) o.blush = true;    // mmm
      break;
  }
  ClawdRes a = drawClawd(c, o);
  if (tongueOn) tongue(a.F, i >> 1);

  // ---- mug ----
  if (mugHeld) {
    // held at the mouth, steam curling up past his (unmoving) eyes
    heldMug(c, a.mouth.x + 1, a.mouth.y + 1, i, true);
  } else if (kind == K_REACH || kind == K_PUTBACK) {
    heldMug(c, a.handR.x + 3, a.handR.y + 2, i, false);
  } else {
    MugOpts mo; mo.frame = i;
    mug(c, MUG.x, MUG.y, mo);
  }

  // ---- FX ----
  Pt hl = a.handL;
  if (kind == K_TYPE || kind == K_REACH || kind == K_PUTBACK || (kind == K_SIP && i % 2 == 0)) clacks(c, hl.x, hl.y, i);
  if (kind == K_SLAM) {
    fx::sparkle(c, hl.x - 2, hl.y - 4, 3, C::amber);
    fx::ticks(c, hl.x - 2, hl.y - 2, "left", C::text);
    fx::ticks(c, a.head.x, a.head.y - 3, "up", C::amber);
  }
  if (kind == K_RECOIL) {
    fx::sparkle(c, hl.x - 6, hl.y - 8, 2, C::amber);
    fx::sparkle(c, 78, 42, 1, C::text);
  }
  if (kind == K_RECOIL || kind == K_PROUD) fx::check(c, 92, 38 - (kind == K_PROUD ? 1 : 0), C::green);
  if (kind == K_PROUD) fx::sparkle(c, hl.x - 8, hl.y - 10, 1, C::amber);
}
}  // namespace

const IsoAnim anim_11_writing_code = {"11-writing-code", 10, NFR, true, false, draw, 0, 11};   // calm segment: see anim.h
