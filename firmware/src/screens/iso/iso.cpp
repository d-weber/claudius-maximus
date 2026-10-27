// C++ port of iso-clawd/lib/iso.js. Keep the pixels identical to the JS: when in doubt, read the JS.
#include "iso.h"
#include <math.h>
#include <string.h>

namespace iso {
bool blinkSuppressed = false;

static inline int R(float v) { return jsRound(v); }

// ---------------------------------------------------------------- raw pixels
void Ctx::clear(Col c) { for (int i = 0; i < W * H; i++) buf[i] = c; }

void rect(Ctx &c, int x, int y, int w, int h, Col col) {
  if (col == NONE || w <= 0 || h <= 0) return;
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
  for (int yy = y0; yy < y1; yy++) {
    Col *p = c.buf + yy * W;
    for (int xx = x0; xx < x1; xx++) p[xx] = col;
  }
}

void line(Ctx &c, int x0, int y0, int x1, int y1, Col col) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
  for (;;) {
    rect(c, x0, y0, 1, 1, col);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * e;
    if (e2 >= dy) { e += dy; x0 += sx; }
    if (e2 <= dx) { e += dx; y0 += sy; }
  }
}

void isoLine(Ctx &c, int x, int y, int n, const char *dir, Col col) {
  int sx = dir[1] == 'r' ? 1 : -1, sy = dir[0] == 'd' ? 1 : -1;
  for (int i = 0; i < n; i++) rect(c, sx > 0 ? x + 2 * i : x - 2 * i - 1, y + sy * i, 2, 1, col);
}

void dither(Ctx &c, int x, int y, int w, int h, Col col, float density) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) {
      int X = x + i, Y = y + j;
      bool p = density >= 0.75f ? !(X % 2 == 0 && Y % 2 == 0) : density >= 0.5f ? (X + Y) % 2 == 0 : (X % 2 == 0 && Y % 2 == 0);
      if (p) rect(c, X, Y, 1, 1, col);
    }
}

Col SpriteMap::get(char ch) const {
  for (const MapEntry *m = e; m->ch; m++) if (m->ch == ch) return m->col;
  return NONE;
}

Size sprite(Ctx &c, int x, int y, const char *const *rows, int nrows, const SpriteMap &map, bool flipX) {
  int w = 0;
  for (int j = 0; j < nrows; j++) { int l = strlen(rows[j]); if (l > w) w = l; }
  for (int j = 0; j < nrows; j++) {
    const char *r = rows[j];
    for (int i = 0; r[i]; i++) {
      char ch = r[i];
      if (ch == '.' || ch == ' ') continue;
      Col col = map.get(ch);
      if (col == NONE) continue;
      rect(c, flipX ? x + w - 1 - i : x + i, y + j, 1, 1, col);
    }
  }
  return {w, nrows};
}

// ---------------------------------------------------------------- 3x5 font
static const char *glyph(char ch) {
  if (ch >= 'a' && ch <= 'z') ch -= 32;
  switch (ch) {
    case '0': return "75557"; case '1': return "26227"; case '2': return "71747"; case '3': return "71317"; case '4': return "55711";
    case '5': return "74717"; case '6': return "74757"; case '7': return "71122"; case '8': return "75757"; case '9': return "75717";
    case 'A': return "25755"; case 'B': return "65656"; case 'C': return "34443"; case 'D': return "65556"; case 'E': return "74647";
    case 'F': return "74644"; case 'G': return "34553"; case 'H': return "55755"; case 'I': return "72227"; case 'J': return "11152";
    case 'K': return "55655"; case 'L': return "44447"; case 'M': return "57755"; case 'N': return "65555"; case 'O': return "25552";
    case 'P': return "65644"; case 'Q': return "25563"; case 'R': return "65655"; case 'S': return "34216"; case 'T': return "72222";
    case 'U': return "55557"; case 'V': return "55552"; case 'W': return "55775"; case 'X': return "55255"; case 'Y': return "55222";
    case 'Z': return "71247";
    case '!': return "22202"; case '?': return "61202"; case '.': return "00002"; case ',': return "00024"; case ':': return "02020";
    case '-': return "00700"; case '+': return "02720"; case '/': return "11244"; case '>': return "42124"; case '<': return "12421";
    case '_': return "00007"; case '$': return "36236"; case '#': return "57575"; case '%': return "51245"; case '(': return "12221";
    case ')': return "42224"; case '[': return "32223"; case ']': return "62226"; case '=': return "07070"; case '*': return "05250";
    case '\'': return "22000"; case '"': return "55000"; case '~': return "00360"; case ' ': return "00000";
    default: return "61202";
  }
}

int text(Ctx &c, int x, int y, const char *str, Col col) {
  int n = 0;
  for (int k = 0; str[k]; k++, n++) {
    const char *g = glyph(str[k]);
    for (int r = 0; r < 5; r++) {
      int bits = g[r] - '0';
      for (int b = 0; b < 3; b++) if (bits & (4 >> b)) rect(c, x + k * 4 + b, y + r, 1, 1, col);
    }
  }
  return n * 4 - 1;
}
int textWidth(const char *str) { return (int)strlen(str) * 4 - 1; }

// ---------------------------------------------------------------- projection / painters
Pt w2sf(int ox, int oy, float wx, float wy, float wz) { return {R(ox + 2 * (wx - wy)), R(oy + (wx + wy) - wz)}; }

void FacePainter::rect(int u, int v, int w, int h, Col col) const {
  for (int i = 0; i < w; i++) iso::rect(*c, X0 + u + i, yAt(u + i) + v, 1, h, col);
}
Size FacePainter::sprite(int u, int v, const char *const *rows, int nrows, const SpriteMap &map, bool flipX) const {
  return iso::sprite(*c, X0 + u, yAt(u) + v, rows, nrows, map, flipX);
}
void FacePainter::spriteSheared(int u, int v, const char *const *rows, int nrows, const SpriteMap &map, bool flipX) const {
  for (int j = 0; j < nrows; j++) {
    int w = (int)strlen(rows[j]);
    for (int i = 0; i < w; i++) {
      Col col = map.get(rows[j][flipX ? w - 1 - i : i]);
      if (col != NONE) iso::rect(*c, X0 + u + i, yAt(u + i) + v + j, 1, 1, col);
    }
  }
}

static FacePainter facePainter(Ctx &c, int X0, int Y0, int cols, int rows, int slope) { return {&c, X0, Y0, cols, rows, slope}; }

BoxRes boxAt(Ctx &c, int sx, int sy, int z, int a, int b, int h, const Pal &pal, bool round) {
  int Yback = sy - z - h, XL = sx - 2 * b;
  int Yleft = Yback + b, Yright = Yback + a;
  int Wd = 2 * (a + b);
  for (int col = 0; col < Wd; col++) {
    int X = XL + col;
    int top = col < 2 * b ? Yleft - (col >> 1) : Yright - ((Wd - 1 - col) >> 1);
    int ridge = col < 2 * a ? Yleft + (col >> 1) : Yright + ((Wd - 1 - col) >> 1);
    int t0 = top, s1 = ridge + h;
    bool edgeCol = col == 0 || col == Wd - 1;
    if (round && edgeCol) { t0 = top + 1; s1 -= 1; }
    if (pal.top != NONE && ridge > t0) rect(c, X, t0, 1, ridge - t0, pal.top);
    if (pal.edge != NONE && ridge > top && h > 0) rect(c, X, ridge - 1, 1, 1, pal.edge);
    Col side = col < 2 * a ? pal.left : pal.right;
    int s0 = ridge;
    if (round && edgeCol) s0 = ridge + 1;
    if (side != NONE && s1 > s0) rect(c, X, s0, 1, s1 - s0, side);
  }
  int Xback = XL + 2 * b;
  BoxRes r;
  r.left = facePainter(c, XL, Yleft, 2 * a, h, +1);
  r.right = facePainter(c, XL + 2 * a, Yright, 2 * b, h, -1);
  r.top = {&c, Xback, Yback};
  r.x = XL; r.y = Yback + 1; r.w = Wd; r.h = a + b - 1 + h;
  return r;
}
BoxRes box(Ctx &c, int x, int y, int z, int a, int b, int h, const Pal &pal, bool round) {
  return boxAt(c, x - (a - b), y - (a + b) / 2, z, a, b, h, pal, round);
}
FacePainter plane(Ctx &c, int x, int y, int z, int len, int h, char pl, Col fill) {
  if (pl == 'R') return boxAt(c, x + len, y - len / 2, z, 0, len, h, {NONE, NONE, fill, NONE}).right;
  return boxAt(c, x - len, y - len / 2, z, len, 0, h, {NONE, fill, NONE, NONE}).left;
}

// ---------------------------------------------------------------- object builder
Facing facingOf(const char *s) {
  if (!strcmp(s, "right")) return RIGHT;
  if (!strcmp(s, "upRight")) return UP_RIGHT;
  if (!strcmp(s, "upLeft")) return UP_LEFT;
  return LEFT;
}
Obj::Obj(Ctx &c_, int x_, int y_, int A_, int B_, Facing f, int z_) : c(&c_), x(x_), y(y_), A(A_), B(B_), z(z_), facing(f) {
  swap = facing == RIGHT || facing == UP_LEFT;
  refl = facing == UP_RIGHT || facing == UP_LEFT;
  int WA = swap ? B : A, WB = swap ? A : B;
  ox = x - (WA - WB); oy = y - (WA + WB) / 2;
}
void Obj::box(int lx, int ly, int lz, int la, int lb, int lh, const Pal &pal, int tag, bool round) {
  if (n >= 12) return;
  if (refl) ly = B - ly - lb;
  int wx = lx, wy = ly, wa = la, wb = lb;
  if (swap) { wx = ly; wy = lx; wa = lb; wb = la; }
  items[n] = {wx, wy, lz + z, wa, wb, lh, pal, tag, round, n};
  n++;
}
Pt Obj::pt(float lx, float ly, float lz) const {
  if (refl) ly = B - ly;
  float wx = lx, wy = ly;
  if (swap) { wx = ly; wy = lx; }
  return w2sf(ox, oy, wx, wy, lz + z);
}
void Obj::draw() {
  // sort: z, then depth (centre x+y), then insertion order
  for (int i = 1; i < n; i++) {
    Item it = items[i];
    int j = i - 1;
    auto key = [](const Item &p) { return (p.wx + p.wa / 2.f + p.wy + p.wb / 2.f); };
    while (j >= 0 && (items[j].wz > it.wz || (items[j].wz == it.wz && (key(items[j]) > key(it) || (key(items[j]) == key(it) && items[j].k > it.k))))) {
      items[j + 1] = items[j]; j--;
    }
    items[j + 1] = it;
  }
  for (int i = 0; i < n; i++) {
    const Item &it = items[i];
    Pt s = w2s(ox, oy, it.wx, it.wy, 0);
    BoxRes r = boxAt(*c, s.x, s.y, it.wz, it.wa, it.wb, it.lh, it.pal, it.round);
    r.frontSel = refl ? 0 : (swap ? 2 : 1);
    r.backSel = refl ? (swap ? 2 : 1) : 0;
    r.sideSel = swap ? 1 : 2;
    if (it.tag >= 0 && it.tag < 8) res[it.tag] = r;
  }
}

// ---------------------------------------------------------------- Clawd
struct Spec { int FW, D, H, legH, legW, legD, legX[4], eyeW, eyeH, eyeL, eyeR, eyeV, armL, armD; };
static const Spec SPEC_FULL = {16, 6, 28, 6, 2, 2, {1, 4, 10, 13}, 6, 9, -10, 4, 8, 4, 2};
static const Spec SPEC_MINI = {8, 3, 12, 3, 1, 1, {1, 2, 5, 6}, 2, 4, -6, 4, 3, 2, 1};

// Face glyphs (lib/iso.js EYES / MOUTHS / BLUSH). Everything on the face is SHEARED (each sprite column
// follows the face slope). Entry: sprite centred on an anchor column (sprite column w >> 1 lands on it), dv
// rows below the anchor row; sprR/dvR = variant for the screen-right eye (wince). Colours: '#' eye colour,
// 'r' C::red, 'w' C::text (eye colour when shine is off), 'b' blush. Facing right mirrors the glyphs.
struct Glyph { const char *const *spr; int n, dv; const char *const *sprR; int nR, dvR; bool shine; };
#define GL(a) a, (int)(sizeof(a) / sizeof(a[0]))
static const char *const EYE_F_OPEN[] = {"..####", "##ww##", "##ww##", "######", "######", "######", "######", "######", "####.."};
static const char *const EYE_F_WIDE[] = {"..####", "##ww##", "##ww##", "######", "######", "######",
                                         "######", "######", "######", "######", "######", "####.."};
static const char *const EYE_F_BLINK[] = {"######", "######"};
static const char *const EYE_F_CLOSED[] = {"........##", "........##", "......##..", "########..", "######...."};
static const char *const EYE_F_HAPPY[] = {"......##..", "....######", "..########", "..##......", "##........", "##........", "##........"};
static const char *const EYE_F_WINCE[] = {"##....", "####..", "..####", "....##", "..##..", "..##..", "##....", "##...."};
static const char *const EYE_F_WINCE_R[] = {"....##", "....##", "..##..", "..##..", "##....", "####..", "..####", "....##"};
static const char *const EYE_M_OPEN[] = {"##", "##", "##", "##"};
static const char *const EYE_M_WIDE[] = {"##", "##", "##", "##", "##"};
static const char *const EYE_M_BLINK[] = {"##"};
static const char *const EYE_M_CLOSED[] = {"....##", "####.."};
static const char *const EYE_M_HAPPY[] = {"..##..", "....##", "##...."};
static const char *const EYE_M_WINCE[] = {"######"};
// indexed by EyeShape: OPEN, BLINK, CLOSED, HAPPY, WINCE, WIDE (anything else falls back to OPEN, like the JS)
static const Glyph EYES_FULL[] = {
  {GL(EYE_F_OPEN), 0, nullptr, 0, 0, false},  {GL(EYE_F_BLINK), 5, nullptr, 0, 0, false},
  {GL(EYE_F_CLOSED), 1, nullptr, 0, 0, false}, {GL(EYE_F_HAPPY), 1, nullptr, 0, 0, false},
  {GL(EYE_F_WINCE), 1, GL(EYE_F_WINCE_R), -1, false}, {GL(EYE_F_WIDE), -2, nullptr, 0, 0, true},
};
static const Glyph EYES_MINI[] = {
  {GL(EYE_M_OPEN), 0, nullptr, 0, 0, false},  {GL(EYE_M_BLINK), 2, nullptr, 0, 0, false},
  {GL(EYE_M_CLOSED), 1, nullptr, 0, 0, false}, {GL(EYE_M_HAPPY), 0, nullptr, 0, 0, false},
  {GL(EYE_M_WINCE), 1, nullptr, 0, 0, false},  {GL(EYE_M_WIDE), -1, nullptr, 0, 0, false},
};
// Mouths: anchor column = cx + gx, row = mv (2 rows under the eyes + gaze, clamped above the legs).
static const char *const MOUTH_F_O[] = {"..####..", "########", "########", "########", "..####.."};
static const char *const MOUTH_F_O_SMALL[] = {"####", "####", "####", "####"};
static const char *const MOUTH_F_LAUGH[] = {"############", "############", "####rrrr####", "..##rrrrrr..", "....rrrr...."};
static const char *const MOUTH_F_LAUGH_SQ[] = {"########", "########", "####rr##", "..rrrr..", "..####.."};
static const char *const MOUTH_F_O_SQ[] = {"####", "####", "####", "####"};
static const char *const MOUTH_F_SMILE[] = {"#......#", "#......#", ".#....#.", ".#####.."};
static const char *const MOUTH_F_WOBBLY[] = {"..........#.", ".........#.#", "..#.....#...", ".#.##.......", "#....###...."};
static const char *const MOUTH_F_FLAT[] = {"########"};
static const char *const MOUTH_M_O[] = {"####", "####"};
static const char *const MOUTH_M_O_SMALL[] = {"####"};
static const char *const MOUTH_M_LAUGH[] = {"####", "rrrr"};
static const char *const MOUTH_M_SMILE[] = {"...#", "###."};
static const char *const MOUTH_M_WOBBLY[] = {"####"};
static const char *const MOUTH_M_FLAT[] = {"####"};
// indexed by MouthKind - 1: O, O_SMALL, LAUGH, SMILE, WOBBLY, FLAT
static const Glyph MOUTHS_FULL[] = {
  {GL(MOUTH_F_O), -1, nullptr, 0, 0, false},     {GL(MOUTH_F_O_SMALL), 0, nullptr, 0, 0, false},
  {GL(MOUTH_F_LAUGH), -1, nullptr, 0, 0, false}, {GL(MOUTH_F_SMILE), -1, nullptr, 0, 0, false},
  {GL(MOUTH_F_WOBBLY), 0, nullptr, 0, 0, false}, {GL(MOUTH_F_FLAT), 0, nullptr, 0, 0, false},
};
static const Glyph MOUTHS_MINI[] = {
  {GL(MOUTH_M_O), 0, nullptr, 0, 0, false},      {GL(MOUTH_M_O_SMALL), 0, nullptr, 0, 0, false},
  {GL(MOUTH_M_LAUGH), 0, nullptr, 0, 0, false},  {GL(MOUTH_M_SMILE), 0, nullptr, 0, 0, false},
  {GL(MOUTH_M_WOBBLY), 0, nullptr, 0, 0, false}, {GL(MOUTH_M_FLAT), 0, nullptr, 0, 0, false},
};
// '<name>_sq' compact variants, used at squash >= 4 when they exist (full size only)
static const Glyph MOUTH_LAUGH_SQ = {GL(MOUTH_F_LAUGH_SQ), 0, nullptr, 0, 0, false};
static const Glyph MOUTH_O_SQ = {GL(MOUTH_F_O_SQ), 0, nullptr, 0, 0, false};
// Blush: a cheek patch centred du columns either side of cx, one row under the eyes.
static const char *const BLUSH_F[] = {"bbbbbb", "bbbbbb"};
static const char *const BLUSH_M[] = {"bb"};
static const Glyph BLUSH_FULL = {GL(BLUSH_F), 0, nullptr, 0, 0, false}, BLUSH_MINI = {GL(BLUSH_M), 0, nullptr, 0, 0, false};
static const int BLUSH_DU_FULL = 11, BLUSH_DU_MINI = 7;
#undef GL

static const Glyph &eyeGlyph(bool mini, EyeShape sh) {
  const Glyph *t = mini ? EYES_MINI : EYES_FULL;
  return (sh >= EYE_OPEN && sh <= EYE_WIDE) ? t[sh] : t[EYE_OPEN];
}
static const Glyph *mouthGlyph(bool mini, MouthKind m, int squash) {
  if (m < MOUTH_O || m > MOUTH_FLAT) return nullptr;   // 'none' / unknown: no mouth
  if (!mini && squash >= 4) {
    if (m == MOUTH_LAUGH) return &MOUTH_LAUGH_SQ;
    if (m == MOUTH_O) return &MOUTH_O_SQ;
  }
  return &(mini ? MOUTHS_MINI : MOUTHS_FULL)[m - 1];
}

EyeShape eyeOf(const char *s) {
  if (!strcmp(s, "blink")) return EYE_BLINK; if (!strcmp(s, "closed")) return EYE_CLOSED; if (!strcmp(s, "happy")) return EYE_HAPPY;
  if (!strcmp(s, "wince")) return EYE_WINCE; if (!strcmp(s, "wide")) return EYE_WIDE; if (!strcmp(s, "wink")) return EYE_WINK;
  return EYE_OPEN;
}
MouthKind mouthOf(const char *s) {
  if (!strcmp(s, "o")) return MOUTH_O; if (!strcmp(s, "o_small")) return MOUTH_O_SMALL; if (!strcmp(s, "laugh")) return MOUTH_LAUGH;
  if (!strcmp(s, "smile")) return MOUTH_SMILE; if (!strcmp(s, "wobbly")) return MOUTH_WOBBLY; if (!strcmp(s, "flat")) return MOUTH_FLAT;
  return MOUTH_NONE;
}
ArmPose armOf(const char *s) {
  if (!strcmp(s, "high")) return ARM_HIGH; if (!strcmp(s, "up")) return ARM_UP; if (!strcmp(s, "waveA")) return ARM_WAVEA;
  if (!strcmp(s, "waveB")) return ARM_WAVEB; if (!strcmp(s, "down")) return ARM_DOWN; if (!strcmp(s, "forward")) return ARM_FORWARD;
  if (!strcmp(s, "hold")) return ARM_HOLD; if (!strcmp(s, "typeA")) return ARM_TYPEA; if (!strcmp(s, "typeB")) return ARM_TYPEB;
  if (!strcmp(s, "none")) return ARM_NONE;
  return ARM_OUT;
}
LegsPose legsOf(const char *s) {
  if (!strcmp(s, "sit")) return LEGS_SIT; if (!strcmp(s, "dangle")) return LEGS_DANGLE; if (!strcmp(s, "dangleB")) return LEGS_DANGLEB;
  if (!strcmp(s, "stepA")) return LEGS_STEPA; if (!strcmp(s, "stepB")) return LEGS_STEPB; if (!strcmp(s, "tap")) return LEGS_TAP;
  return LEGS_STAND;
}

struct ArmBox { int x, y, z, a, b, h; };
struct ArmGeom { ArmBox boxes[2]; int n; bool hasHand; float hx, hy, hz; bool front; };

static ArmGeom armGeom(ArmPose pose, int side, const Spec &S, int FWb, int x0, int zb, int Hh) {
  int L = S.armL, D = S.armD, Dp = S.D;
  int T = imax(2, R(Hh * 0.25f));
  auto out = [&](int len) { return side < 0 ? x0 - len : x0 + FWb; };
  auto inner = [&](int len) { return side < 0 ? x0 : x0 + FWb - len; };
  auto tipX = [&](int len) { return side < 0 ? x0 - len : x0 + FWb + len; };
  int yb = Dp - D;
  int zOut = zb + R(Hh * 0.36f);
  ArmGeom g = {}; g.n = 0; g.hasHand = true; g.front = false;
  switch (pose) {
    case ARM_NONE: g.hasHand = false; return g;
    case ARM_HIGH: {
      int zz = zOut + R(Hh * 0.14f);
      g.boxes[g.n++] = {out(L), yb, zz, L, D, T};
      g.hx = tipX(L); g.hy = yb + D / 2.f; g.hz = zz + T / 2.f; return g;
    }
    case ARM_UP: case ARM_WAVEA: {
      int l1 = imax(1, L - 1), z1 = zb + R(Hh * 0.5f), z2 = z1 + T - 1;
      g.boxes[g.n++] = {out(l1), yb, z1, l1, D, T};
      g.boxes[g.n++] = {side < 0 ? x0 - l1 - l1 : x0 + FWb + l1, yb, z2, l1, D, T};
      g.hx = tipX(2 * l1); g.hy = yb + D / 2.f; g.hz = z2 + T; return g;
    }
    case ARM_WAVEB: {
      int l1 = imax(1, L - 1), z1 = zb + R(Hh * 0.62f), hh = R(Hh * 0.55f);
      g.boxes[g.n++] = {out(l1), yb, z1, l1, D, hh};
      g.hx = tipX(l1) - (side * l1) / 2.f; g.hy = yb + D / 2.f; g.hz = z1 + hh; return g;
    }
    case ARM_DOWN: {
      int l1 = imax(1, L - 2), hh = R(Hh * 0.45f);
      g.boxes[g.n++] = {out(l1), yb, zb + 1, l1, D, hh};
      g.hx = tipX(l1); g.hy = yb + D / 2.f; g.hz = zb + 1; return g;
    }
    case ARM_FORWARD: case ARM_HOLD: case ARM_TYPEA: case ARM_TYPEB: {
      int zz = pose == ARM_TYPEA ? zb + R(Hh * 0.12f) : pose == ARM_TYPEB ? zb + R(Hh * 0.22f) : zb + R(Hh * 0.3f);
      int len = L, l1 = imax(1, L - 1);
      g.boxes[g.n++] = {inner(l1), Dp, zz, l1, len, T - 1};
      g.hx = inner(l1) + l1 / 2.f; g.hy = Dp + len; g.hz = zz + (T - 1) / 2.f; g.front = true; return g;
    }
    case ARM_OUT: default:
      g.boxes[g.n++] = {out(L), yb, zOut, L, D, T};
      g.hx = tipX(L); g.hy = yb + D / 2.f; g.hz = zOut + T / 2.f; return g;
  }
}

ClawdRes drawClawd(Ctx &c, const ClawdOpts &o) {
  const Spec &S = o.mini ? SPEC_MINI : SPEC_FULL;
  bool mini = o.mini;
  const Pal &pal = o.tint;
  bool flip = o.facing == RIGHT;
  int x = o.x, y = o.y, lift = o.z, squash = o.squash;
  LegsPose legs = o.legs;
  int shake = o.shake;

  int Hh = imax(4, S.H - squash);
  int k = mini ? (squash >= 3 ? 1 : 0) : (squash >= 5 ? 2 : squash >= 2 ? 1 : squash <= -3 ? -1 : 0);
  int FWb = S.FW + 2 * k, x0 = -k;
  bool sitting = legs == LEGS_SIT;
  bool dangling = legs == LEGS_DANGLE || legs == LEGS_DANGLEB;
  int zb = sitting ? 1 : dangling ? 0 : S.legH;

  int D = S.D;
  int ox = x + shake - (flip ? D - S.FW : S.FW - D), oy = y - (S.FW + D) / 2;
  auto scr = [&](float lx, float ly, float lz) { return flip ? w2sf(ox, oy, ly, lx, lz + lift) : w2sf(ox, oy, lx, ly, lz + lift); };
  auto lbox = [&](int lx, int ly, int lz, int la, int lb, int lh, const Pal &p, bool round = false) {
    Pt s = flip ? w2s(ox, oy, ly, lx, 0) : w2s(ox, oy, lx, ly, 0);
    return boxAt(c, s.x, s.y, lz + lift, flip ? lb : la, flip ? la : lb, lh, p, round);
  };

  if (o.shadow) {
    int sh = imin(lift / 6, 3);
    int sa = S.FW + 2 - 2 * sh, sb = D + 2 - sh;
    if (sa > 2 && sb > 1) {
      int lx = -1 + sh, ly = -1 + sh / 2;
      Pt ss = flip ? w2s(ox - shake, oy, ly, lx, 0) : w2s(ox - shake, oy, lx, ly, 0);
      boxAt(c, ss.x, ss.y, 0, flip ? sb : sa, flip ? sa : sb, 0, {o.shadowColor, NONE, NONE, NONE});
    }
  }

  if (!sitting) {
    int order[4] = {0, 1, 2, 3};   // legX ascending; descending when flipped
    if (flip) { order[0] = 3; order[1] = 2; order[2] = 1; order[3] = 0; }
    for (int q = 0; q < 4; q++) {
      int i = order[q], lx = S.legX[i];
      int lz = 0, lh = S.legH, ly = D - S.legD - (mini ? 0 : 1);
      if (legs == LEGS_STEPA && (i == 0 || i == 2)) { lz = 2; lh = S.legH - 2; }
      if (legs == LEGS_STEPB && (i == 1 || i == 3)) { lz = 2; lh = S.legH - 2; }
      if (legs == LEGS_TAP && i == (flip ? 0 : 3)) { lz = 2; lh = S.legH - 2; }
      if (dangling) {
        int fwd = ((legs == LEGS_DANGLEB) == (i % 2 == 0)) ? 1 : 0;
        ly = D - S.legD + fwd; lz = -S.legH - 1 + fwd; lh = S.legH + 1 - fwd;
      }
      if (lh > 0) lbox(lx + x0 + k, ly, lz, S.legW, S.legD, lh, pal);
    }
  } else {
    for (int i = 0; i < 4; i++) lbox(S.legX[i], D - S.legD - (mini ? 0 : 1), 0, S.legW, S.legD, 1, pal);
  }

  ArmGeom geomL = armGeom(o.armL, flip ? +1 : -1, S, FWb, x0, zb, Hh);
  ArmGeom geomR = armGeom(o.armR, flip ? -1 : +1, S, FWb, x0, zb, Hh);
  const ArmGeom &backG = flip ? geomR : geomL, &frontG = flip ? geomL : geomR;
  auto drawArm = [&](const ArmGeom &g) { for (int i = 0; i < g.n; i++) { const ArmBox &b = g.boxes[i]; lbox(b.x, b.y, b.z, b.a, b.b, b.h, pal); } };
  if (!backG.front) drawArm(backG);

  BoxRes body = lbox(x0, 0, zb, FWb, D, Hh, pal, true);
  const FacePainter &F = flip ? body.right : body.left;

  int cx = FWb;
  int ev = mini ? S.eyeV : (squash >= 3 ? S.eyeV - 1 : S.eyeV);
  int gx = o.eyes.gazeX * 2, gy = o.eyes.gazeY;
  Col ec = o.eyeColor;
  auto eyeU = [&](int side) { return cx + (side < 0 ? S.eyeL : S.eyeR) + gx; };
  int eyeV = ev + gy;
  auto shapeFor = [&](int side) -> EyeShape {
    EyeShape sh = o.eyes.shape;
    if (side < 0 && o.eyes.l != EYE_DEFAULT) sh = o.eyes.l;
    else if (side > 0 && o.eyes.r != EYE_DEFAULT) sh = o.eyes.r;
    else if (sh == EYE_WINK) sh = side < 0 ? EYE_OPEN : EYE_HAPPY;
    if (blinkSuppressed && sh == EYE_BLINK) sh = EYE_OPEN;
    return sh;
  };
  Col bc = o.blushColor;
  // draw one face glyph centred on column uc, row v (mirrored when facing right)
  auto glyph = [&](const char *const *spr, int n, int dv, int uc, int v, bool shine) {
    // a named array, not ISO_MAP(): that compound literal only lives to the end of its statement
    const MapEntry me[] = {{'#', ec}, {'r', C::red}, {'w', shine ? C::text : ec}, {'b', bc}, {0, 0}};
    F.spriteSheared(uc - ((int)strlen(spr[0]) >> 1), v + dv, spr, n, SpriteMap{me}, flip);
  };
  for (int side = -1; side <= 1; side += 2) {
    const Glyph &g = eyeGlyph(mini, shapeFor(side));
    // facing right mirrors the face, so the screen-right eye takes the left-eye variant and vice versa
    bool useR = g.sprR && ((side > 0) != flip);
    bool shine = o.shine ? true : o.shineOff ? false : g.shine;
    glyph(useR ? g.sprR : g.spr, useR ? g.nR : g.n, useR ? g.dvR : g.dv, eyeU(side) + (S.eyeW >> 1), eyeV, shine);
  }
  if (o.blush) {
    const Glyph &B = mini ? BLUSH_MINI : BLUSH_FULL;
    int du = mini ? BLUSH_DU_MINI : BLUSH_DU_FULL, bv = ev + S.eyeH + 1;
    glyph(B.spr, B.n, 0, cx - du, bv, false);
    glyph(B.spr, B.n, 0, cx + du, bv, false);
  }
  // the mouth follows the gaze; 'none' draws nothing. Squashed (>= 4): compact '_sq' variant when there is one.
  const Glyph *mg = mouthGlyph(mini, o.mouth, squash);
  int mBase = ev + S.eyeH + (mini ? 1 : 2) + gy;
  int mv = mg ? (mBase < Hh - mg->n - 2 ? mBase : Hh - mg->n - 2) : mBase;
  if (mg) glyph(mg->spr, mg->n, mg->dv, cx + gx, mv, false);

  if (backG.front) drawArm(backG);
  drawArm(frontG);

  ClawdRes r;
  r.hasHandL = geomL.hasHand; r.hasHandR = geomR.hasHand;
  r.handL = geomL.hasHand ? scr(geomL.hx, geomL.hy, geomL.hz) : Pt{0, 0};
  r.handR = geomR.hasHand ? scr(geomR.hx, geomR.hy, geomR.hz) : Pt{0, 0};
  auto scrf = [&](float lx, float ly, float lz, float &sx, float &sy) {
    float wx = flip ? ly : lx, wy = flip ? lx : ly;
    sx = ox + 2 * (wx - wy); sy = oy + (wx + wy) - (lz + lift);
  };
  r.handLx = r.handLy = r.handRx = r.handRy = 0;
  if (geomL.hasHand) scrf(geomL.hx, geomL.hy, geomL.hz, r.handLx, r.handLy);
  if (geomR.hasHand) scrf(geomR.hx, geomR.hy, geomR.hz, r.handRx, r.handRy);
  Pt head = scr(x0 + FWb / 2.f, D / 2.f, zb + Hh);
  r.top = head;
  r.head = {head.x, head.y - (S.FW + D) / 2 + 1};
  r.face = F.at(cx, Hh / 2);
  r.eyeL = F.at(eyeU(-1) + (S.eyeW >> 1), eyeV);
  r.eyeR = F.at(eyeU(1) + (S.eyeW >> 1), eyeV);
  r.mouth = F.at(cx + gx, mv);
  r.feet = {x, y};
  r.bx = body.x - 2 * S.armL - 2; r.by = body.y - 2; r.bw = body.w + 4 * S.armL + 4; r.bh = body.h + S.legH + 4;
  r.F = F;
  return r;
}

// ---------------------------------------------------------------- desk
static Pt deskPt(const Desk &d, float u, float v) {
  int ox = d.x - (d.a - d.b), oy = d.y - (d.a + d.b) / 2;
  return w2sf(ox, oy, u * d.a, v * d.b, 0);
}
Pt deskPoint(float u, float v, const Desk *d) { Desk def; return deskPt(d ? *d : def, u, v); }

DeskRes drawDesk(Ctx &c, const DeskOpts *o) {
  DeskOpts def;
  const DeskOpts &op = o ? *o : def;
  const Desk &d = op.d;
  const Pal &pal = op.pal ? *op.pal : SHADE::wood;
  int ox = d.x - (d.a - d.b), oy = d.y - (d.a + d.b) / 2;
  BoxRes r = boxAt(c, ox, oy, -d.thick, d.a, d.b, d.thick, pal);
  if (op.grain) {
    static const int streaks[6][3] = {{3, 6, 14}, {7, 20, 12}, {12, 2, 9}, {16, 26, 13}, {21, 10, 16}, {24, 30, 9}};
    for (auto &s : streaks) { int j = s[0], i0 = s[1], n = s[2]; for (int i = i0; i < i0 + n && i < d.a - 1; i++) if (j < d.b - 1) r.top.cell(i, j, C::grain); }
  }
  if (op.mat) for (int i = 10; i < 34 && i < d.a - 2; i++) for (int j = 6; j < 22 && j < d.b - 2; j++) r.top.cell(i, j, op.matColor);
  return {r.top, d};
}

// ---------------------------------------------------------------- props
void screenContent(const FacePainter &p, const Area &A, ScreenKind kind, int frame, Col accent) {
  auto L = [&](int row, int u0, int len, Col col) { if (row < A.h && len > 0) p.rect(A.u + u0, A.v + row, imin(len, A.w - u0), 1, col); };
  if (kind == SCR_OFF || kind == SCR_BLANK) return;
  if (kind == SCR_SEARCH) {
    L(1, 2, A.w - 4, C::paper);
    L(4, 2, 8, C::blueLight); L(6, 2, A.w - 8, C::muted); L(8, 2, 8 + (frame % 3) * 2, C::blueLight);
    return;
  }
  if (kind == SCR_PLAN) {
    for (int r = 0; r < 4; r++) { L(1 + r * 2, 1, 2, r < (frame % 5) ? C::green : C::dim); L(1 + r * 2, 4, 6 + ((r * 5) % 7), C::muted); }
    return;
  }
  static const struct { int ind, len; Col col; } rows[8] = {{0, 6, C::violet}, {2, 9, C::text}, {2, 5, C::amber}, {4, 7, C::blueLight},
                                                           {2, 10, C::text}, {0, 3, C::violet}, {2, 6, C::green}, {4, 8, C::text}};
  for (int r = 0; r * 2 + 1 < A.h; r++) {
    auto &q = rows[(r + frame) % 8];
    L(1 + r * 2, 1 + q.ind, q.len, (accent != NONE && r == 0) ? accent : q.col);
  }
}

static LaptopRes laptopLike(Ctx &c, int x, int y, const LaptopOpts &o, bool monitor) {
  LaptopRes res = {};
  if (!monitor) {
    const int A = 11, B = 7;
    Obj ob(c, x, y, A, B, o.facing, o.z);
    ob.box(0, 0, 0, A, B, 2, SHADE::metal, 0);
    ob.box(0, 0, 2, A, 1, 10, {C::muted, C::grey, C::greyDark, NONE}, 1);
    ob.draw();
    for (int i = 1; i < A - 1; i++) for (int j = 2; j < B - 1; j++) if ((i + j) % 2 == 0) { Pt p = ob.pt(i + 0.5f, j + 0.5f, 2); rect(c, p.x - 1, p.y, 2, 1, C::greyDark); }
    res.base = ob.res[0]; res.lid = ob.res[1];
  } else {
    const int A = 12, B = 5;
    Obj ob(c, x, y, A, B, o.facing, o.z);
    ob.box(4, 1, 0, 4, 3, 1, SHADE::dark, 0);
    ob.box(5, 1, 1, 2, 1, 6, SHADE::dark, 2);
    ob.box(0, 2, 5, A, 1, 14, {C::border, C::surface2, C::surface, NONE}, 1);
    ob.draw();
    res.base = ob.res[0]; res.lid = ob.res[1];
  }
  const BoxRes &lid = res.lid;
  if (lid.front()) {
    res.hasScreen = true;
    res.screen = *lid.front();
    res.area = {2, 1, res.screen.cols - 4, monitor ? 11 : 8};
    res.screen.rect(res.area.u, res.area.v, res.area.w, res.area.h, o.screen == SCR_OFF ? C::surface : C::screen);
    if (o.screen == SCR_CUSTOM && o.fn) o.fn(res.screen, res.area, o.frame, o.ud);
    else screenContent(res.screen, res.area, o.screen == SCR_CUSTOM ? SCR_CODE : o.screen, o.frame);
  } else if (lid.back()) {
    const FacePainter &b = *lid.back();
    if (!monitor) {
      int mu = (b.cols >> 1) - 2;
      b.rect(mu, 3, 4, 3, C::muted);
      b.px(mu + 1, 4, C::greyDark); b.px(mu + 2, 4, C::greyDark);
    }
    if (o.glow) { Pt t0 = b.at(0, -1), t1 = b.at(b.cols - 1, -1); line(c, t0.x, t0.y, t1.x, t1.y, C::glow); }
  }
  return res;
}
LaptopRes laptop(Ctx &c, int x, int y, const LaptopOpts &o) { return laptopLike(c, x, y, o, false); }
LaptopRes monitor(Ctx &c, int x, int y, const LaptopOpts &o) { return laptopLike(c, x, y, o, true); }

BoxRes paper(Ctx &c, int x, int y, const PaperOpts &o) {
  BoxRes r = box(c, x, y, o.z, o.a, o.b, 1, {o.color, C::paperShade, C::muted, NONE});
  if (o.lines) for (int j = 1; j < o.b - 1; j++) {
    int n = j == 1 ? o.a - 3 : o.a - 2 - ((j * 3) % 3);
    for (int i = 1; i < 1 + n; i++) if (j % 2 == 1) r.top.cell(i, j, C::muted);
  }
  return r;
}

BoxRes book(Ctx &c, int x, int y, const BookOpts &o) {
  if (o.open) {
    BoxRes r = box(c, x, y, o.z, 10, 7, 2, {C::paper, C::paperShade, C::muted, NONE});
    for (int j = 0; j < 7; j++) r.top.cell(5, j, C::paperShade);
    for (int j = 1; j < 6; j += 2) { for (int i = 1; i < 4; i++) r.top.cell(i, j, C::muted); for (int i = 6; i < 9; i++) r.top.cell(i, j, C::muted); }
    return r;
  }
  BoxRes r = box(c, x, y, o.z, 6, 8, 3, {o.color.top, C::paper, o.color.right, NONE});
  for (int i = 0; i < 6; i++) r.top.cell(i, 0, o.color.left);
  r.left.rect(0, 1, r.left.cols, 1, C::paperShade);
  return r;
}

BoxRes notepad(Ctx &c, int x, int y, bool lines, int z) {
  BoxRes r = box(c, x, y, z, 5, 7, 2, {C::paper, C::paperShade, C::muted, NONE});
  for (int i = 0; i < 5; i++) r.top.cell(i, 0, C::amber);
  if (lines) for (int j = 2; j < 6; j += 2) for (int i = 1; i < 4; i++) r.top.cell(i, j, C::paperShade);
  return r;
}

void pencil(Ctx &c, int x, int y, int len, char dir) {
  int n = len + 3, sx = dir == 'x' ? 2 : -2;
  int x0 = x - (sx * n) / 2, y0 = y - n / 2;
  for (int i = 0; i < n; i++) {
    Col col = i == 0 ? C::blush : i == n - 1 ? C::eye : i == n - 2 ? C::paperShade : C::amber;
    int X = x0 + sx * i - (sx < 0 ? 2 : 0), Y = y0 + i;
    rect(c, X, Y, 2, 1, col);
    if (i < n - 1) rect(c, X, Y + 1, 2, 1, i == 0 ? rgb(0xC9786A) : i == n - 2 ? C::muted : C::amberDark);
  }
}

BoxRes mug(Ctx &c, int x, int y, const MugOpts &o) {
  const Pal &s = o.color;
  BoxRes r = box(c, x, y, o.z, 4, 4, 8, {s.top, s.left, s.right, NONE});
  box(c, x, y + 1, o.z + 8, 2, 2, 0, {C::woodRight, NONE, NONE, NONE});
  r.right.rect(r.right.cols - 2, 1, 2, 1, s.right);
  Pt hp = r.right.at(r.right.cols - 1, 1);
  rect(c, hp.x + 1, hp.y, 1, 4, s.right); rect(c, hp.x, hp.y + 3, 1, 1, s.right);
  if (o.steam) {
    int f = o.frame % 4;
    Pt top = r.top.at(2, 0);
    static const int wisp[7][2] = {{0, 0}, {1, -1}, {1, -2}, {0, -3}, {-1, -4}, {-1, -5}, {0, -6}};
    for (int k = 0; k < 2; k++) {
      int off = (f + k * 2) % 4;
      for (int i = 0; i < 4; i++) {
        const int *w = wisp[(i + off) % 7];
        int yy = top.y - 3 - i * 2 - off, xx = top.x - 1 + k * 2 + w[0];
        if (i + off < 6) rect(c, xx, yy, 1, 1, i > 2 ? C::dim : C::muted);
      }
    }
  }
  return r;
}

void magnifier(Ctx &c, int x, int y, bool flip) {
  static const char *const rows[] = {"..rrr..", ".rgggr.", "rgwgggr", "rgggggr", "rgggggr", ".rgggr.", "..rrr.."};
  sprite(c, x - 3, y - 3, rows, ISO_MAP({'r', C::muted}, {'g', C::blueLight}, {'w', C::text}), flip);
  int hx = flip ? x - 3 : x + 3, hy = y + 3;
  for (int i = 0; i < 3; i++) rect(c, flip ? hx - 2 - 2 * i : hx + 2 * i, hy + i, 2, 2, C::woodLeft);
}

void sticky(Ctx &c, int x, int y, const StickyOpts &o) {
  if (o.plane == 't') {
    BoxRes r = box(c, x, y, o.z, 3, 3, 0, {o.color, NONE, NONE, NONE});
    if (o.mark) r.top.cell(1, 1, C::eye);
    return;
  }
  FacePainter p = plane(c, x, y, o.z, 3, 6, o.plane, o.color);
  if (o.mark) { p.rect(1, 2, 4, 1, C::dim); p.rect(1, 4, 3, 1, C::dim); }
}

static WindowRes windowPanel(Ctx &c, int x, int y, const WindowOpts &o, Col bg) {
  FacePainter p = plane(c, x, y, o.z, o.len, o.h, o.plane, bg);
  p.rect(0, 0, p.cols, 3, C::surface2);
  p.rect(0, 3, 1, o.h - 3, C::surface2); p.rect(p.cols - 1, 3, 1, o.h - 3, C::surface2); p.rect(0, o.h - 1, p.cols, 1, C::surface2);
  p.px(2, 1, C::red); p.px(4, 1, C::amber); p.px(6, 1, C::green);
  return {p, {2, 4, p.cols - 4, o.h - 6}};
}

WindowRes terminal(Ctx &c, int x, int y, const WindowOpts &o) {
  WindowRes w = windowPanel(c, x, y, o, o.bg != NONE ? o.bg : C::bg);
  const FacePainter &p = w.painter; const Area &area = w.area;
  static const TermLine def[5] = {{0, 8, C::green}, {2, 12, C::text}, {2, 9, C::muted}, {0, 6, C::green}, {2, 14, C::text}};
  const TermLine *lines = o.lines ? o.lines : def; int nl = o.lines ? o.nlines : 5;
  long budget = o.reveal < 0 ? 1000000000L : o.reveal;
  int lastRow = 0, lastU = area.u;
  for (int r = 0; r < nl && r * 2 < area.h; r++) {
    int ind = lines[r].indent, len = lines[r].len; Col col = lines[r].col;
    int v = area.v + r * 2;
    if (ind == 0) p.px(area.u, v, C::orange);
    int u0 = area.u + (ind == 0 ? 2 : ind);
    long n = imax(0, (int)imin((long)len, imin(budget, (long)(area.w - (u0 - area.u)))));
    if (n > 0) p.rect(u0, v, (int)n, 1, col);
    budget -= len;
    lastRow = v; lastU = u0 + (int)n;
    if (budget <= 0) break;
  }
  if (o.cursor && ((o.frame >> 2) % 2) == 0) p.rect(imin(lastU + 1, area.u + area.w - 1), lastRow - 1, 1, 2, C::text);
  return w;
}

WindowRes browser(Ctx &c, int x, int y, const WindowOpts &o) {
  WindowRes w = windowPanel(c, x, y, o, o.bg != NONE ? o.bg : C::surface);
  const FacePainter &p = w.painter; const Area &area = w.area;
  p.rect(area.u, area.v, area.w, 2, C::paper);
  p.px(area.u + 1, area.v, C::blue);
  int rows = o.reveal < 0 ? 99 : o.reveal;
  const struct { Col col; int w; } body[5] = {{C::blue, area.w - 6}, {C::muted, area.w - 2}, {C::muted, area.w - 5}, {C::green, 6}, {C::muted, area.w - 3}};
  for (int r = 0; r < 5 && r < rows; r++) {
    int v = area.v + 3 + r * 2;
    if (v >= area.v + area.h) break;
    p.rect(area.u, v, body[r].w, 1, body[r].col);
  }
  if (o.reveal >= 0 && o.reveal < 5) {
    static const int sp[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    int f = o.frame % 4;
    p.rect(area.u + area.w - 3 + sp[f][0], area.v + sp[f][1], 1, 1, C::orange);
  }
  return w;
}

void globe(Ctx &c, int x, int y, int frame) {
  static const char *const LAND[6] = {"..##......###...", ".####....#####..", "..###.....###.#.", "...##......#....", "....#.....##....", "...........#...."};
  box(c, x, y, 0, 3, 3, 2, SHADE::wood);
  rect(c, x, y - 6, 1, 4, C::woodLeft);
  int cx = x, cy = y - 12, f = frame % 16;
  const float r = 5.5f;
  for (int j = -5; j <= 5; j++) for (int i = -5; i <= 5; i++) {
    float d = sqrtf((float)(i * i + j * j));
    if (d > r) continue;
    float den = sqrtf(r * r - j * j); if (den < 1) den = 1;
    float a = i / den; if (a > 1) a = 1; if (a < -1) a = -1;
    int lon = (R(asinf(a) / 3.14159265f * 8) + 8 + f) % 16;
    int lat = imin(5, (j + 5) / 2);
    bool land = LAND[lat][(lon + 16) % 16] == '#';
    Col col = land ? C::green : C::blue;
    if (i >= 3 || (i >= 2 && abs(j) >= 3)) col = land ? C::greenDark : C::blueDark;
    if (i <= -3 && j <= -2 && !land) col = C::blueLight;
    rect(c, cx + i, cy + j, 1, 1, col);
  }
  rect(c, cx + 6, cy - 2, 1, 6, C::woodEdge); rect(c, cx + 5, cy + 4, 1, 2, C::woodEdge);
}

PlanBoardRes planBoard(Ctx &c, int x, int y, const PlanBoardOpts &o) {
  Obj ob(c, x, y, 12, 4, o.facing, o.z);
  ob.box(1, 1, 0, 1, 1, 14, SHADE::wood, 0);
  ob.box(10, 1, 0, 1, 1, 14, SHADE::wood, 1);
  ob.box(0, 2, 4, 12, 1, 17, {C::woodEdge, C::paper, C::woodRight, NONE}, 2);
  ob.draw();
  PlanBoardRes r = {};
  const BoxRes &board = ob.res[2];
  const FacePainter *p = board.front() ? board.front() : board.back();
  if (p) r.painter = *p;
  if (!board.front()) return r;
  r.hasFront = true;
  int n = o.items, k = o.checked;
  p->rect(0, 0, p->cols, 1, C::woodEdge);
  for (int i = 0; i < n; i++) {
    int v = 3 + i * 3;
    p->rect(2, v, 2, 2, i < k ? C::green : C::paperShade);
    p->rect(6, v, 10 + ((i * 7) % 6), 1, i < k ? C::paperShade : C::muted);
  }
  return r;
}

BoxRes clipboard(Ctx &c, int x, int y, int checked, int z) {
  BoxRes r = box(c, x, y, z, 6, 8, 1, SHADE::wood);
  for (int i = 1; i < 5; i++) for (int j = 1; j < 7; j++) r.top.cell(i, j, C::paper);
  r.top.cell(2, 0, C::muted); r.top.cell(3, 0, C::muted);
  for (int j = 2; j < 7; j += 2) { r.top.cell(1, j, j / 2 <= checked ? C::green : C::paperShade); r.top.cell(3, j, C::muted); }
  return r;
}

void lamp(Ctx &c, int x, int y, bool right, bool on) {
  if (on) {
    int lpx = x + (right ? 14 : -14), lpy = y + 6;
    for (int j = -3; j <= 3; j++) { int w = 18 - abs(j) * 4; dither(c, lpx - w / 2, lpy + j, w, 1, C::woodEdge, 0.5f); }
  }
  box(c, x, y, 0, 4, 4, 1, SHADE::dark);
  rect(c, x, y - 14, 1, 13, C::surface2);
  int s = right ? 1 : -1;
  for (int i = 0; i < 5; i++) rect(c, x + s * (1 + 2 * i) - (s < 0 ? 1 : 0), y - 14 + i, 2, 1, C::surface2);
  int hx = x + s * 11, hy = y - 9;
  static const char *const head[] = {".###.", "#####", "#####"};
  sprite(c, hx - 3, hy - 2, head, ISO_MAP({'#', C::border}), right);
  if (on) { rect(c, hx - 1, hy + 1, 3, 1, C::amber); px(c, hx, hy + 2, rgb(0xF7D885)); }
}

// ---------------------------------------------------------------- FX
namespace fx {
void sparkle(Ctx &c, int x, int y, int size, Col col) {
  static const char *const S0[] = {"#"};
  static const char *const S1[] = {".#.", "###", ".#."};
  static const char *const S2[] = {"..#..", "..#..", "#####", "..#..", "..#.."};
  static const char *const S3[] = {"...#...", "...#...", "..###..", "#######", "..###..", "...#...", "...#..."};
  size = imax(0, imin(3, size));
  const char *const *s = size == 0 ? S0 : size == 1 ? S1 : size == 2 ? S2 : S3;
  int n = size == 0 ? 1 : size == 1 ? 3 : size == 2 ? 5 : 7;
  sprite(c, x - (n >> 1), y - (n >> 1), s, n, ISO_MAP({'#', col}));
}
void bang(Ctx &c, int x, int y, bool badge, Col col) {
  if (!badge) { static const char *const p[] = {"##", "##", "##", "##", "..", "##"}; sprite(c, x - 1, y - 4, p, ISO_MAP({'#', col})); return; }
  static const char *const b[] = {".aaaaa.", "aaadaaa", "aaadaaa", "aaadaaa", "aaadaaa", "aaaaaaa", "aaadaaa", ".aaaaa.", "..aa...", "..a...."};
  sprite(c, x - 3, y - 4, b, ISO_MAP({'a', col}, {'d', C::eye}));
}
void question(Ctx &c, int x, int y, bool badge, Col col) {
  if (!badge) { static const char *const p[] = {"####.", "...##", "..##.", ".##..", ".....", ".##.."}; sprite(c, x - 2, y - 4, p, ISO_MAP({'#', col})); return; }
  static const char *const b[] = {".aaaaa.", "aadddaa", "aaaaada", "aaaddaa", "aaadaaa", "aaaaaaa", "aaadaaa", ".aaaaa.", "..aa...", "..a...."};
  sprite(c, x - 3, y - 4, b, ISO_MAP({'a', col}, {'d', C::eye}));
}
void zee(Ctx &c, int x, int y, int size, Col col) {
  static const char *const Z1[] = {"###", ".#.", "###"};
  static const char *const Z2[] = {"####", "..#.", ".#..", "####"};
  static const char *const Z3[] = {"#####", "...#.", "..#..", ".#...", "#####"};
  int k = imax(0, imin(2, size - 1));
  const char *const *s = k == 0 ? Z1 : k == 1 ? Z2 : Z3; int n = k == 0 ? 3 : k == 1 ? 4 : 5;
  sprite(c, x - (n >> 1), y - (n >> 1), s, n, ISO_MAP({'#', col}));
}
void sweat(Ctx &c, int x, int y) { static const char *const s[] = {".b.", ".b.", "bwb", "bbb", ".b."}; sprite(c, x - 1, y - 2, s, ISO_MAP({'b', C::blue}, {'w', C::blueLight})); }
void heart(Ctx &c, int x, int y, int size, Col col) {
  static const char *const H2[] = {".##.##.", "#######", "#######", ".#####.", "..###..", "...#..."};
  static const char *const H1[] = {"##.##", "#####", ".###.", "..#.."};
  if (size == 2) sprite(c, x - 3, y - 3, H2, ISO_MAP({'#', col})); else sprite(c, x - 2, y - 2, H1, ISO_MAP({'#', col}));
}
void check(Ctx &c, int x, int y, Col col) { static const char *const s[] = {".....##", "....##.", "##.##..", ".###...", "..#...."}; sprite(c, x - 3, y - 2, s, ISO_MAP({'#', col})); }
void cross(Ctx &c, int x, int y, Col col) { static const char *const s[] = {"##..##", ".####.", "..##..", ".####.", "##..##"}; sprite(c, x - 2, y - 2, s, ISO_MAP({'#', col})); }
void dots(Ctx &c, int x, int y, int n, Col col) { for (int i = 0; i < n; i++) rect(c, x - 5 + i * 4, y, 2, 2, col); }
void poof(Ctx &c, int x, int y, int frame, Col col) {
  static const char *const P0[] = {".##.", "####", ".##."};
  static const char *const P1[] = {"..##..", ".####.", "######", ".####.", "..##.."};
  static const char *const P2[] = {".##..##.", "########", ".######.", "########", ".##..##."};
  static const char *const P3[] = {"#......#", "...##...", "#......#", "..#..#..", "#......#"};
  int f = imax(0, imin(3, frame));
  const char *const *s = f == 0 ? P0 : f == 1 ? P1 : f == 2 ? P2 : P3;
  int n = f == 0 ? 3 : 5, w = f == 0 ? 4 : f == 1 ? 6 : 8;
  sprite(c, x - (w >> 1), y - (n >> 1), s, n, ISO_MAP({'#', col}));
}
void ticks(Ctx &c, int x, int y, const char *dir, Col col) {
  if (!strcmp(dir, "left")) { rect(c, x - 4, y - 4, 3, 1, col); rect(c, x - 5, y, 3, 1, col); rect(c, x - 4, y + 4, 3, 1, col); }
  else if (!strcmp(dir, "right")) { rect(c, x + 2, y - 4, 3, 1, col); rect(c, x + 3, y, 3, 1, col); rect(c, x + 2, y + 4, 3, 1, col); }
  else { rect(c, x - 4, y - 2, 1, 2, col); rect(c, x, y - 4, 1, 3, col); rect(c, x + 4, y - 2, 1, 2, col); }
}
Rect puff(Ctx &c, int x, int y, int w, int h, const PuffOpts &o) {
  rect(c, x + 1, y, w - 2, h, o.border); rect(c, x, y + 1, w, h - 2, o.border);
  rect(c, x + 1, y + 1, w - 2, h - 2, o.fill);
  if (o.tail != 'n') {
    bool left = o.tail == 'l';
    int tx = left ? x + 4 : x + w - 8;
    if (o.thought) {
      rect(c, tx + (left ? 0 : 2), y + h + 1, 3, 2, o.fill);
      rect(c, tx + (left ? -2 : 5), y + h + 4, 2, 2, o.fill);
    } else {
      static const char *const TL[] = {"####", "###.", "##..", "#..."};
      static const char *const TR[] = {"####", ".###", "..##", "...#"};
      sprite(c, tx, y + h - 1, left ? TL : TR, 4, ISO_MAP({'#', o.fill}));
      if (left) rect(c, tx - 1, y + h - 1, 1, 4, o.border); else rect(c, tx + 4, y + h - 1, 1, 4, o.border);
    }
  }
  return {x + 2, y + 2, w - 4, h - 4};
}
}  // namespace fx

int lerp(int a, int b, float t) { return R(a + (b - a) * t); }

}  // namespace iso
