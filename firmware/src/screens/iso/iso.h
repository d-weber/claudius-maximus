#pragma once
// Isometric pixel-art library: C++ port of iso-clawd/lib/iso.js (same names, same semantics, same pixels).
// See iso-clawd/README.md for the conventions. Logical canvas 240 x 121; the home screen shows the middle
// 160 columns at 3x (480 x 363). Projection: classic 2:1 iso, +x -> (+2,+1), +y -> (-2,+1), +z -> (0,-1).
#include <stdint.h>
#include <stddef.h>
#include <math.h>

namespace iso {

typedef uint16_t Col;                 // RGB565, LVGL native byte order
constexpr Col NONE = 0;               // "no colour": skips the face / pixel (no palette entry is 0)
constexpr Col rgb(uint32_t h) { return (Col)((((h >> 16) & 0xFF) >> 3) << 11 | (((h >> 8) & 0xFF) >> 2) << 5 | ((h & 0xFF) >> 3)); }

constexpr int W = 240, H = 121;

// ---------------------------------------------------------------- palette (lib: Iso.C)
namespace C {
constexpr Col bg = rgb(0x1A1918), surface = rgb(0x262523), surface2 = rgb(0x33312E), border = rgb(0x3D3A36),
              text = rgb(0xECE9E4), muted = rgb(0x9C978E), dim = rgb(0x5E5A54),
              orange = rgb(0xD97757), orangeLight = rgb(0xF0A07E), orangeDark = rgb(0xB4603F),
              blush = rgb(0xF4A592), eye = rgb(0x1A1918),
              amber = rgb(0xF2C14E), amberDark = rgb(0xC99A2E), green = rgb(0x7FB069), greenDark = rgb(0x5E8C4C),
              red = rgb(0xE5534B), redDark = rgb(0xB23A33),
              violet = rgb(0xB392F0), violetLight = rgb(0xCDB5F6), violetDark = rgb(0x8E6CC9),
              blue = rgb(0x6A9BCC), blueLight = rgb(0xA9C8E6), blueDark = rgb(0x46719C),
              grey = rgb(0x77716B), greyLight = rgb(0x948E86), greyDark = rgb(0x5E5A54),
              woodTop = rgb(0x6B5140), woodLeft = rgb(0x523D30), woodRight = rgb(0x3F2F25), woodEdge = rgb(0x80624E),
              woodShadow = rgb(0x4C392D), grain = rgb(0x62493A),
              paper = rgb(0xECE9E4), paperShade = rgb(0xC9C2B6), ink = rgb(0x5E5A54),
              screen = rgb(0x1E2A36), glow = rgb(0xA9C8E6), shadow = rgb(0x121110);
}

struct Pal { Col top, left, right, edge; };   // any may be NONE
namespace SHADE {
constexpr Pal orange = {C::orangeLight, C::orange, C::orangeDark, NONE};
constexpr Pal grey = {C::greyLight, C::grey, C::greyDark, NONE};
constexpr Pal violet = {C::violetLight, C::violet, C::violetDark, NONE};
constexpr Pal wood = {C::woodTop, C::woodLeft, C::woodRight, C::woodEdge};
constexpr Pal paper = {C::paper, C::paperShade, C::muted, NONE};
constexpr Pal metal = {C::muted, C::grey, C::greyDark, NONE};
constexpr Pal dark = {C::border, C::surface2, C::surface, NONE};
constexpr Pal blue = {C::blueLight, C::blue, C::blueDark, NONE};
constexpr Pal green = {rgb(0xA3CB8F), C::green, C::greenDark, NONE};
constexpr Pal amber = {rgb(0xF7D885), C::amber, C::amberDark, NONE};
constexpr Pal red = {rgb(0xEE8680), C::red, C::redDark, NONE};
}

// ---------------------------------------------------------------- canvas + raw pixels
struct Ctx {
  Col *buf;                   // W*H pixels, row-major
  void clear(Col c = C::bg);
};
struct Pt { int x, y; };

void rect(Ctx &c, int x, int y, int w, int h, Col col);
inline void px(Ctx &c, int x, int y, Col col) { rect(c, x, y, 1, 1, col); }
void line(Ctx &c, int x0, int y0, int x1, int y1, Col col);
void isoLine(Ctx &c, int x, int y, int n, const char *dir, Col col);      // dir "dr","dl","ur","ul"
void dither(Ctx &c, int x, int y, int w, int h, Col col, float density = 0.5f);

// Sprite from strings: rows like "..##.." ('.' and ' ' transparent); map = {{'#', col}, ..., {0, 0}}.
struct MapEntry { char ch; Col col; };
struct SpriteMap { const MapEntry *e; Col get(char ch) const; };
#define ISO_MAP(...) iso::SpriteMap{(const iso::MapEntry[]){__VA_ARGS__, {0, 0}}}
struct Size { int w, h; };
Size sprite(Ctx &c, int x, int y, const char *const *rows, int nrows, const SpriteMap &map, bool flipX = false);
template <size_t N> inline Size sprite(Ctx &c, int x, int y, const char *const (&rows)[N], const SpriteMap &map, bool flipX = false) {
  return sprite(c, x, y, rows, (int)N, map, flipX);
}

int text(Ctx &c, int x, int y, const char *str, Col col = C::text);   // 3x5 font, uppercase, 4 px advance
int textWidth(const char *str);

// ---------------------------------------------------------------- projection
inline Pt w2s(int ox, int oy, int wx, int wy, int wz = 0) { return {ox + 2 * (wx - wy), oy + (wx + wy) - wz}; }
Pt w2sf(int ox, int oy, float wx, float wy, float wz = 0);

// Face painter for one rasterised vertical face (u = column from the face's screen-left edge, v = row).
struct FacePainter {
  Ctx *c; int X0, Y0, cols, rows, slope;
  int yAt(int u) const { return slope > 0 ? Y0 + (u >> 1) : Y0 + ((cols - 1 - u) >> 1); }
  Pt at(int u, int v) const { return {X0 + u, yAt(u) + v}; }
  void rect(int u, int v, int w, int h, Col col) const;
  void px(int u, int v, Col col) const { iso::rect(*c, X0 + u, yAt(u) + v, 1, 1, col); }
  Size sprite(int u, int v, const char *const *rows, int nrows, const SpriteMap &map, bool flipX = false) const;
  template <size_t N> Size sprite(int u, int v, const char *const (&rows)[N], const SpriteMap &map, bool flipX = false) const {
    return sprite(u, v, rows, (int)N, map, flipX);
  }
  // flipX mirrors the bitmap within each row's width (with the mirrored slope: the mirror image of the glyph)
  void spriteSheared(int u, int v, const char *const *rows, int nrows, const SpriteMap &map, bool flipX = false) const;
  template <size_t N> void spriteSheared(int u, int v, const char *const (&rows)[N], const SpriteMap &map, bool flipX = false) const {
    spriteSheared(u, v, rows, (int)N, map, flipX);
  }
};
struct TopPainter {
  Ctx *c; int Xback, Yback;
  void cell(int i, int j, Col col) const { iso::rect(*c, Xback + 2 * (i - j) - 1, Yback + i + j + 1, 2, 1, col); }
  Pt at(int i, int j) const { return {Xback + 2 * (i - j), Yback + i + j}; }
};
struct BoxRes {
  FacePainter left, right; TopPainter top;
  int x, y, w, h;                       // screen bounds
  int frontSel = 0, backSel = 0, sideSel = 0;   // Obj: 0 none, 1 left, 2 right
  const FacePainter *front() const { return frontSel == 1 ? &left : frontSel == 2 ? &right : nullptr; }
  const FacePainter *back() const { return backSel == 1 ? &left : backSel == 2 ? &right : nullptr; }
  const FacePainter *side() const { return sideSel == 1 ? &left : sideSel == 2 ? &right : nullptr; }
};
// Voxel block. (sx, sy) = screen position of the BACK corner at the base; a along x, b along y (units), h px.
BoxRes boxAt(Ctx &c, int sx, int sy, int z, int a, int b, int h, const Pal &pal, bool round = false);
// Centre-anchored box: (x, y) = screen point of the footprint centre, z = lift.
BoxRes box(Ctx &c, int x, int y, int z, int a, int b, int h, const Pal &pal, bool round = false);
inline BoxRes tile(Ctx &c, int x, int y, int a, int b, Col col) { return box(c, x, y, 0, a, b, 0, {col, NONE, NONE, NONE}); }
// Vertical plane: plane 'L' (runs along x) or 'R' (runs along y); (x, y) = bottom-centre, z = lift of its bottom.
FacePainter plane(Ctx &c, int x, int y, int z, int len, int h, char plane, Col fill);

// ---------------------------------------------------------------- object builder (lib: Iso.obj)
enum Facing { LEFT, RIGHT, UP_RIGHT, UP_LEFT };
Facing facingOf(const char *s);   // "left" | "right" | "upRight" | "upLeft"
struct Obj {
  Ctx *c; int x, y, A, B, z; Facing facing; bool swap, refl;
  int ox, oy;
  struct Item { int wx, wy, wz, wa, wb, lh; Pal pal; int tag; bool round; int k; };
  Item items[12]; int n = 0;
  BoxRes res[8];
  Obj(Ctx &c, int x, int y, int A, int B, Facing facing = LEFT, int z = 0);
  bool frontVisible() const { return !refl; }
  void box(int lx, int ly, int lz, int la, int lb, int lh, const Pal &pal, int tag = -1, bool round = false);
  Pt pt(float lx, float ly, float lz = 0) const;
  void draw();                                   // sorts and draws; res[tag] filled for tagged boxes
};

// ---------------------------------------------------------------- Clawd
enum EyeShape { EYE_OPEN, EYE_BLINK, EYE_CLOSED, EYE_HAPPY, EYE_WINCE, EYE_WIDE, EYE_WINK, EYE_DEFAULT = -1 };
enum MouthKind { MOUTH_NONE, MOUTH_O, MOUTH_O_SMALL, MOUTH_LAUGH, MOUTH_SMILE, MOUTH_WOBBLY, MOUTH_FLAT };
enum ArmPose { ARM_OUT, ARM_HIGH, ARM_UP, ARM_WAVEA, ARM_WAVEB, ARM_DOWN, ARM_FORWARD, ARM_HOLD, ARM_TYPEA, ARM_TYPEB, ARM_NONE };
enum LegsPose { LEGS_STAND, LEGS_SIT, LEGS_DANGLE, LEGS_DANGLEB, LEGS_STEPA, LEGS_STEPB, LEGS_TAP };
EyeShape eyeOf(const char *s); MouthKind mouthOf(const char *s); ArmPose armOf(const char *s); LegsPose legsOf(const char *s);

struct Eyes { EyeShape shape = EYE_OPEN; int gazeX = 0, gazeY = 0; EyeShape l = EYE_DEFAULT, r = EYE_DEFAULT; };
struct ClawdOpts {
  int x = 120, y = 76, z = 0;        // feet point (SPOT.clawd), lift
  bool mini = false;
  Pal tint = SHADE::orange;
  Facing facing = LEFT;              // LEFT | RIGHT only
  int squash = 0, shake = 0;
  LegsPose legs = LEGS_STAND;
  Eyes eyes;
  MouthKind mouth = MOUTH_NONE;
  ArmPose armL = ARM_OUT, armR = ARM_OUT;
  bool shadow = true; Col shadowColor = C::woodShadow;
  bool blush = false; Col blushColor = SHADE::red.top;   // JS default SH.red.top
  Col eyeColor = C::eye;
  bool shine = false;                // force the catch-light on; default: per glyph ("wide" shines, others not)
  bool shineOff = false;
};
// Set by the director while Clawd rests in a calm segment that it decided should not blink this pass:
// EYE_BLINK renders as EYE_OPEN. Off by default (QA renders match the JS reference).
extern bool blinkSuppressed;

struct ClawdRes {
  bool hasHandL, hasHandR; Pt handL, handR;
  float handLx, handLy, handRx, handRy;   // the same anchors unrounded (JS keeps half pixels: round after arithmetic)
  Pt head, top, face, eyeL, eyeR, mouth, feet;
  int bx, by, bw, bh;                // bbox
  FacePainter F;
};
ClawdRes drawClawd(Ctx &c, const ClawdOpts &o);

// ---------------------------------------------------------------- desk
struct Desk { int x = 120, y = 74, a = 44, b = 28, thick = 7; };
constexpr Pt SPOT_center = {120, 74}, SPOT_clawd = {120, 76}, SPOT_backLeft = {84, 56}, SPOT_backRight = {148, 66},
             SPOT_left = {74, 70}, SPOT_right = {166, 78}, SPOT_frontLeft = {100, 88}, SPOT_frontRight = {148, 92},
             SPOT_front = {128, 100}, SPOT_mug = {150, 90};   // SPOT_mug: the mug lives here in every animation
Pt deskPoint(float u, float v, const Desk *d = nullptr);
struct DeskOpts { Desk d; const Pal *pal = nullptr; bool grain = true; bool mat = false; Col matColor = C::surface; };
struct DeskRes { TopPainter top; Desk d; Pt pt(float u, float v) const { return deskPoint(u, v, &d); } };
DeskRes drawDesk(Ctx &c, const DeskOpts *o = nullptr);

// ---------------------------------------------------------------- props (lib: Iso.prop)
struct Area { int u, v, w, h; };
enum ScreenKind { SCR_CODE, SCR_SEARCH, SCR_PLAN, SCR_BLANK, SCR_OFF, SCR_CUSTOM };
typedef void (*ScreenFn)(const FacePainter &p, const Area &a, int frame, void *ud);
void screenContent(const FacePainter &p, const Area &a, ScreenKind kind, int frame, Col accent = NONE);
struct LaptopOpts { Facing facing = LEFT; ScreenKind screen = SCR_CODE; ScreenFn fn = nullptr; void *ud = nullptr; int frame = 0; bool glow = true; int z = 0; };
struct LaptopRes { bool hasScreen; FacePainter screen; Area area; BoxRes lid, base; };
LaptopRes laptop(Ctx &c, int x, int y, const LaptopOpts &o = LaptopOpts());
LaptopRes monitor(Ctx &c, int x, int y, const LaptopOpts &o = LaptopOpts());
struct PaperOpts { bool lines = true; int a = 6, b = 8; Col color = C::paper; int z = 0; };
BoxRes paper(Ctx &c, int x, int y, const PaperOpts &o = PaperOpts());
struct BookOpts { Pal color = SHADE::blue; bool open = false; int z = 0; };
BoxRes book(Ctx &c, int x, int y, const BookOpts &o = BookOpts());
BoxRes notepad(Ctx &c, int x, int y, bool lines = true, int z = 0);
void pencil(Ctx &c, int x, int y, int len = 5, char dir = 'x');
struct MugOpts { int frame = 0; bool steam = true; Pal color = SHADE::paper; int z = 0; };
BoxRes mug(Ctx &c, int x, int y, const MugOpts &o = MugOpts());
void magnifier(Ctx &c, int x, int y, bool handleDownLeft = false);
struct StickyOpts { Col color = C::amber; char plane = 't'; int z = 0; bool mark = true; };   // plane 't' | 'L' | 'R'
void sticky(Ctx &c, int x, int y, const StickyOpts &o = StickyOpts());
struct TermLine { int indent, len; Col col; };
struct WindowOpts { char plane = 'L'; int len = 14, h = 18, z = 10; Col bg = NONE; int reveal = -1; bool cursor = true; int frame = 0;
                    const TermLine *lines = nullptr; int nlines = 0; };
struct WindowRes { FacePainter painter; Area area; };
WindowRes terminal(Ctx &c, int x, int y, const WindowOpts &o = WindowOpts());
WindowRes browser(Ctx &c, int x, int y, const WindowOpts &o = WindowOpts());
void globe(Ctx &c, int x, int y, int frame = 0);
struct PlanBoardOpts { Facing facing = LEFT; int items = 4, checked = 0, z = 0; };
struct PlanBoardRes { bool hasFront; FacePainter painter; };
PlanBoardRes planBoard(Ctx &c, int x, int y, const PlanBoardOpts &o = PlanBoardOpts());
BoxRes clipboard(Ctx &c, int x, int y, int checked = 0, int z = 0);
void lamp(Ctx &c, int x, int y, bool right = false, bool on = true);
inline BoxRes shadow(Ctx &c, int x, int y, int a, int b, Col col = C::woodShadow) { return tile(c, x, y, a, b, col); }

// ---------------------------------------------------------------- FX (lib: Iso.fx)
namespace fx {
void sparkle(Ctx &c, int x, int y, int size = 2, Col col = C::amber);
void bang(Ctx &c, int x, int y, bool badge = true, Col col = C::amber);
void question(Ctx &c, int x, int y, bool badge = true, Col col = C::blue);
void zee(Ctx &c, int x, int y, int size = 1, Col col = C::muted);
void sweat(Ctx &c, int x, int y);
void heart(Ctx &c, int x, int y, int size = 1, Col col = C::red);
void check(Ctx &c, int x, int y, Col col = C::green);
void cross(Ctx &c, int x, int y, Col col = C::red);
void dots(Ctx &c, int x, int y, int n = 3, Col col = C::text);
void poof(Ctx &c, int x, int y, int frame, Col col = C::muted);
void ticks(Ctx &c, int x, int y, const char *dir = "up", Col col = C::text);
struct PuffOpts { bool thought = false; char tail = 'l'; Col fill = C::surface2; Col border = C::border; };  // tail 'l' | 'r' | 'n'
struct Rect { int x, y, w, h; };
Rect puff(Ctx &c, int x, int y, int w, int h, const PuffOpts &o = PuffOpts());
}

// ---------------------------------------------------------------- timing helpers (lib: Iso.seq & co)
template <class T> struct Beat { T v; int hold; };
template <class T, size_t N> int seqLen(const Beat<T> (&l)[N]) { int s = 0; for (size_t k = 0; k < N; k++) s += l[k].hold > 0 ? l[k].hold : 1; return s; }
template <class T, size_t N> const T &seq(const Beat<T> (&l)[N], int i) {
  int n = seqLen(l), f = ((i % n) + n) % n;
  for (size_t k = 0; k < N; k++) { int h = l[k].hold > 0 ? l[k].hold : 1; if (f < h) return l[k].v; f -= h; }
  return l[N - 1].v;
}
template <class T, size_t N> const T &cycle(const T (&arr)[N], int i, int hold = 1) { return arr[(i / (hold > 0 ? hold : 1)) % N]; }
inline int pingpong(int n, int i) { int p = 2 * (n - 1); if (p < 1) p = 1; int k = i % p; return k < n ? k : p - k; }
int lerp(int a, int b, float t);
inline int jsRound(float v) { return (int)floorf(v + 0.5f); }   // JS Math.round (half toward +inf)
inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }

}  // namespace iso
