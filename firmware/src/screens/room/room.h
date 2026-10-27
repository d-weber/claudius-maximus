#pragma once
// Clawd's Studio: a top-down 3/4 pixel-art room (120 x 106 logical pixels, shown at 4x on the home screen).
//
// Portable C++ (no Arduino / LVGL): the same sources build on the host for tools/room_sim.py. The canvas holds
// palette indices, not colours: lighting (time of day, alarm, offline) is a palette transform, so a night fade
// costs a 64-entry table rebuild, not a redraw. Entries carry a "weight": emissive things (screens, LEDs, the
// quest "!", emotes) ignore the room light, Clawd is half affected, furniture fully.
#include <stdint.h>
#include <stddef.h>

namespace room {

constexpr int W = 120, H = 106;      // the room; the 14-row HUD band below it is LVGL widgets
constexpr int FPS = 12;              // director tick rate (sprite animation rate)

typedef uint8_t Idx;

// ---------------------------------------------------------------- palette
enum : Idx {
  T = 0,                             // transparent (sprites) / unchanged
  OUT, WAL, WAL2, CAP, W1, W2, W3, WH,
  F1, F2, F3, FL, MOONF, LAMPF, SPOTF,          // floor planks, sun patch, moon patch, lamp pool, alarm spot
  RUG, RUG2, CRM, WHT, INK,
  SKYA, SKYB, CLOUD, RAIN, STAR, MOONC,          // window (SKYA/SKYB are set per time of day)
  RED, GRN, GRN2, YEL, PUR, PINK, BLUEL,
  MET, DRK, DRK2, SCR, SCRD, SCRW, LEDG, LEDR, LEDY, WB,
  C, CS, CH, BLUSH,                              // Clawd
  GC, GCS, GCH,                                  // Clawd, offline grey
  VC, VCS, VCH,                                  // mini helpers, violet
  QY, QO, QW,                                    // quest yellow / amber / emissive white
  LAMP, TORCH, TORCH2, SHD,
  N_COL
};

struct Rgb { uint8_t r, g, b, w; };              // w: 0 emissive .. 255 fully lit by the room light
extern const Rgb BASE[N_COL];

// Room light as a palette transform: c' = lerp(c, desaturate(c) * mul + add, w)
struct Light {
  float mul[3], add[3], desat;
  bool operator==(const Light &o) const;
};
namespace LIGHTS {
extern const Light DAY, DAWN, DUSK, EVENING, NIGHT, OVERCAST, ALARM, OFFLINE;
}
Light mixLight(const Light &a, const Light &b, float t);
// Builds the RGB565 table (LVGL native byte order). skyA/skyB override the two window-sky entries (packed 0xRRGGBB).
void buildPalette(const Light &l, uint32_t skyA, uint32_t skyB, uint16_t out[N_COL]);
uint16_t rgb565(uint32_t rgb);

// ---------------------------------------------------------------- canvas
struct MapEntry { char ch; Idx c; };
#define ROOM_MAP(...) ((const room::MapEntry[]){__VA_ARGS__, {0, 0}})

struct Canvas {
  Idx *buf;                                       // W*H, row-major
  void clear(Idx c);
  inline void p(int x, int y, Idx c) { if (x >= 0 && x < W && y >= 0 && y < H && c != T) buf[y * W + x] = c; }
  inline Idx get(int x, int y) const { return (x >= 0 && x < W && y >= 0 && y < H) ? buf[y * W + x] : OUT; }
  void r(int x0, int y0, int x1, int y1, Idx c);                    // inclusive rect
  void box(int x0, int y0, int x1, int y1, Idx fill, Idx out = OUT); // outlined rect
  void hline(int x0, int x1, int y, Idx c) { r(x0, y, x1, y, c); }
  void vline(int x, int y0, int y1, Idx c) { r(x, y0, x, y1, c); }
  // Rows like "..##..": '.' / ' ' transparent, others looked up in map. flip mirrors each row within the sprite width.
  void stamp(int x, int y, const char *const *rows, int nrows, const MapEntry *map, bool flip = false);
  template <size_t N> void stamp(int x, int y, const char *const (&rows)[N], const MapEntry *map, bool flip = false) {
    stamp(x, y, rows, (int)N, map, flip);
  }
  void ellipse(int cx, int cy, int rx, int ry, Idx c);              // filled
  void darken(int x0, int x1, int y);                               // 1-row shadow: floor colours -> darker kin
};
Idx darker(Idx c);

// ---------------------------------------------------------------- the room
enum SkyMode { SKY_DAY, SKY_NIGHT };
struct WindowState {
  SkyMode mode = SKY_DAY;
  int cloudX = 0;            // 0..40 drift (day)
  bool rain = false; int rainT = 0;
  int starT = 0;             // twinkle phase (night)
  int moonPhase = 4;         // 0..7 (4 = full)
  int bird = -1;             // -1 none, else bird frame 0..
  bool shootingStar = false; int shootT = 0;
};
void drawWalls(Canvas &c);
void drawFloor(Canvas &c);
void drawSunPatch(Canvas &c, int offset, int len, Idx col);  // offset: horizontal shift of the light, len: height
void drawWindow(Canvas &c, const WindowState &w);
void drawWhiteboard(Canvas &c, int total, int done);
void drawBookshelf(Canvas &c, int gap);                        // gap: -1 none, else which book slot is missing (0..8)
enum ScreenMode { SCREEN_OFF, SCREEN_DIM, SCREEN_CODE, SCREEN_WEB, SCREEN_SAVER, SCREEN_PLUG };
struct DeskState { ScreenMode screen = SCREEN_DIM; int t = 0; bool drawer = false; int steam = 0; bool mug = true; bool lit = false; };
void drawDesk(Canvas &c, const DeskState &d);
void drawRack(Canvas &c, bool active, int t, int progress /* -1 none, 0..16 */, uint32_t seed);
void drawRug(Canvas &c);
void drawBed(Canvas &c, bool sleeping, int breath);
void drawNightstand(Canvas &c, bool lampOn);
void drawLampPool(Canvas &c);                                  // light pool on the floor around the lamp (before props)
void drawPlant(Canvas &c, int sway);
void drawDoorSpot(Canvas &c);                                  // alarm spotlight on the floor by the door
void drawAlarmRing(Canvas &c, int phase);
void drawUnderDeskBeam(Canvas &c, int t);                     // torch beam under the desk (searching)
void drawPaper(Canvas &c, int x, int y);                      // the lost paper under the desk

// ---------------------------------------------------------------- Clawd
enum View { FRONT, BACK, LEFTV, RIGHTV };
enum Arms { ARMS_DOWN, ARMS_UP, ARMS_UP_L, ARMS_UP_R, ARMS_TYPE_A, ARMS_TYPE_B, ARMS_HOLD, ARMS_WAVE_A, ARMS_WAVE_B, ARMS_NONE };
enum Eyes { EYES_OPEN, EYES_BLINK, EYES_WIDE, EYES_HAPPY, EYES_LOOK_L, EYES_LOOK_R, EYES_WINCE, EYES_WINK, EYES_SLEEP, EYES_UP };
enum Mouth { MOUTH_NONE, MOUTH_O, MOUTH_SMILE, MOUTH_FLAT };
enum Tint { TINT_CORAL, TINT_GREY, TINT_VIOLET };
struct ClawdPose {
  int x = 0, y = 0;          // sprite top-left (18 x 12); feet are at y + 11
  View view = FRONT;
  Arms arms = ARMS_DOWN;
  Eyes eyes = EYES_OPEN;
  Mouth mouth = MOUTH_NONE;
  int legPhase = 0;          // walk cycle 0..3 (0 = standing)
  Tint tint = TINT_CORAL;
  bool blush = false;
  bool shadow = true;
};
void drawClawd(Canvas &c, const ClawdPose &p);
void drawClawdInBed(Canvas &c, int shift, bool eyesOpen, Tint tint);   // head on the pillow
// Mini helper (11 x 9). Feet at y + 8.
void drawMini(Canvas &c, int x, int y, View v, int legPhase, Tint tint = TINT_VIOLET, bool blink = false);

// Props held / placed
void drawBook(Canvas &c, int x, int y, int page);              // open book 14 x 7
void drawCan(Canvas &c, int x, int y, bool pour, int t);       // watering can + droplets
void drawTorch(Canvas &c, int x, int y);
void drawMugHeld(Canvas &c, int x, int y);

// ---------------------------------------------------------------- FX
enum Icon { ICO_CODE, ICO_PLAY, ICO_STAR, ICO_LOOK, ICO_CHECK, ICO_CROSS, ICO_HEART, ICO_NOTE, ICO_QUESTION, ICO_BANG,
            ICO_LEAF, ICO_BOOK, ICO_PLUG, ICO_WEB, ICO_PEN, ICO_COFFEE, ICO_COUNT };
void drawEmote(Canvas &c, int x, int y, Icon icon, Idx col, bool tailRight = false);   // 13 x 11 (+ tail)
void drawQuestBang(Canvas &c, int x, int y, int lift);         // the big needs-you "!" bubble, 22 x 34
void drawThought(Canvas &c, int x, int y, int dots, int tailX, int tailY);  // thought bubble 24 x 12 + trail
void drawZzz(Canvas &c, int x, int y, int t);
void drawSparkle(Canvas &c, int x, int y, Idx col, bool big = false);
void drawSweat(Canvas &c, int x, int y);
void drawDash(Canvas &c, int x0, int y0, int x1, int y1, int t, Idx col);
void drawBird(Canvas &c, int x, int y, int frame);

// ---------------------------------------------------------------- misc
struct Rng {
  uint32_t s = 0x9E3779B9u;
  uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  int range(int lo, int hi) { return lo + (int)(next() % (uint32_t)(hi - lo + 1)); }   // inclusive
  bool chance(int pct) { return (int)(next() % 100) < pct; }
  float unit() { return (next() & 0xFFFF) / 65536.f; }
};
inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline int iabs(int a) { return a < 0 ? -a : a; }

}  // namespace room
