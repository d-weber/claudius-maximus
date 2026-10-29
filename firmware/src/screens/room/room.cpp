// Clawd's Studio: palette, canvas primitives, the furniture, Clawd, helpers and FX. See room.h.
#include "room.h"
#if ROOM_SPRITES
#include "sprites_gen.h"
#endif
#include <string.h>
#include <math.h>

namespace room {

// ---------------------------------------------------------------- palette
#define RGB(h, w) {(uint8_t)((h) >> 16), (uint8_t)(((h) >> 8) & 0xFF), (uint8_t)((h) & 0xFF), (uint8_t)(w)}
const Rgb BASE[N_COL] = {
    RGB(0x000000, 255),   // T (never drawn)
    RGB(0x2b1e2a, 255),   // OUT
    RGB(0xecdcb8, 255),   // WAL
    RGB(0xdcc699, 255),   // WAL2
    RGB(0x5a3a30, 255),   // CAP
    RGB(0x8a5a3c, 255),   // W1
    RGB(0x6e4430, 255),   // W2
    RGB(0x4c2e22, 255),   // W3
    RGB(0xb07a50, 255),   // WH
    RGB(0xc98f5a, 255),   // F1
    RGB(0xb67a48, 255),   // F2
    RGB(0x9c6438, 255),   // F3
    RGB(0xddaa74, 255),   // FL   sun patch on F1
    RGB(0x9aa6c8, 60),    // MOONF moon patch (mostly emissive: moonlight)
    RGB(0xe8b86a, 30),    // LAMPF lamp pool on the floor
    RGB(0xffd890, 0),     // SPOTF alarm spotlight by the door
    RGB(0x5b8fb9, 255),   // RUG
    RGB(0x3f6d96, 255),   // RUG2
    RGB(0xf4ecdc, 255),   // CRM
    RGB(0xffffff, 255),   // WHT
    RGB(0x6a6478, 255),   // INK
    RGB(0x9fd8f0, 0),     // SKYA (overridden per time of day)
    RGB(0xd4f2fb, 0),     // SKYB
    RGB(0xfbfbff, 0),     // CLOUD
    RGB(0xb8d0e8, 0),     // RAIN
    RGB(0xffffff, 0),     // STAR
    RGB(0xf2f0d8, 0),     // MOONC
    RGB(0xc8443c, 255),   // RED
    RGB(0x5f9e4f, 255),   // GRN
    RGB(0x437a3a, 255),   // GRN2
    RGB(0xf2c14e, 255),   // YEL
    RGB(0x8c5aa8, 255),   // PUR
    RGB(0xf088b0, 255),   // PINK
    RGB(0xa9c8e6, 255),   // BLUEL
    RGB(0x9aa3b0, 255),   // MET
    RGB(0x3a3f4b, 255),   // DRK
    RGB(0x252a33, 255),   // DRK2
    RGB(0x3fe0a0, 0),     // SCR  screen text
    RGB(0x1d6e58, 40),    // SCRD screen dim
    RGB(0xe8f0f8, 0),     // SCRW web page white
    RGB(0x5ef07a, 0),     // LEDG
    RGB(0xff5a4a, 0),     // LEDR
    RGB(0xffd23f, 0),     // LEDY
    RGB(0xf7f7f2, 255),   // WB
    RGB(0xd97757, 128),   // C
    RGB(0xb85c3c, 128),   // CS
    RGB(0xf0a080, 128),   // CH
    RGB(0xf4a592, 128),   // BLUSH
    RGB(0x8a8a8a, 128),   // GC
    RGB(0x646464, 128),   // GCS
    RGB(0xaaaaaa, 128),   // GCH
    RGB(0xb392f0, 128),   // VC
    RGB(0x8e6cc9, 128),   // VCS
    RGB(0xcdb5f6, 128),   // VCH
    RGB(0xffd23f, 0),     // QY
    RGB(0xff9f1c, 0),     // QO
    RGB(0xffffff, 0),     // QW
    RGB(0xfff0a0, 0),     // LAMP
    RGB(0xfff0b0, 0),     // TORCH
    RGB(0xc0a060, 0),     // TORCH2
    RGB(0x000000, 255),   // SHD (placeholder, shadows use darker())
};

bool Light::operator==(const Light &o) const {
  for (int i = 0; i < 3; i++) if (mul[i] != o.mul[i] || add[i] != o.add[i]) return false;
  return desat == o.desat;
}

namespace LIGHTS {
const Light DAY = {{1.f, 1.f, 1.f}, {0, 0, 0}, 0};
const Light DAWN = {{1.02f, 0.9f, 0.82f}, {8, 0, 0}, 0};
const Light DUSK = {{0.98f, 0.8f, 0.68f}, {18, 4, 0}, 0};
const Light EVENING = {{0.56f, 0.56f, 0.72f}, {4, 6, 18}, 0.1f};
const Light NIGHT = {{0.30f, 0.34f, 0.55f}, {6, 8, 22}, 0.15f};
const Light OVERCAST = {{0.82f, 0.84f, 0.90f}, {0, 0, 4}, 0.15f};
const Light ALARM = {{0.62f, 0.38f, 0.18f}, {40, 22, 18}, 1.f};
const Light OFFLINE = {{0.42f, 0.42f, 0.46f}, {10, 10, 12}, 0.85f};
}

Light mixLight(const Light &a, const Light &b, float t) {
  Light o;
  for (int i = 0; i < 3; i++) { o.mul[i] = a.mul[i] + (b.mul[i] - a.mul[i]) * t; o.add[i] = a.add[i] + (b.add[i] - a.add[i]) * t; }
  o.desat = a.desat + (b.desat - a.desat) * t;
  return o;
}

uint16_t rgb565(uint32_t h) {
  return (uint16_t)((((h >> 16) & 0xFF) >> 3) << 11 | (((h >> 8) & 0xFF) >> 2) << 5 | ((h & 0xFF) >> 3));
}

static inline uint8_t clamp8(float v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)(v + 0.5f); }

void buildPalette(const Light &l, uint32_t skyA, uint32_t skyB, uint16_t out[N_COL]) {
  for (int i = 0; i < N_COL; i++) {
    Rgb c = BASE[i];
    if (i == SKYA) c = RGB(skyA, 0);
    if (i == SKYB) c = RGB(skyB, 0);
    float w = c.w / 255.f;
    float g = (c.r * 3 + c.g * 5 + c.b * 2) / 10.f;
    float ch[3] = {(float)c.r, (float)c.g, (float)c.b};
    float o[3];
    for (int k = 0; k < 3; k++) {
      float d = ch[k] + (g - ch[k]) * l.desat;
      float lit = d * l.mul[k] + l.add[k];
      o[k] = ch[k] + (lit - ch[k]) * w;
    }
    out[i] = rgb565((uint32_t)clamp8(o[0]) << 16 | (uint32_t)clamp8(o[1]) << 8 | clamp8(o[2]));
  }
}

// ---------------------------------------------------------------- canvas
void Canvas::clear(Idx c) { memset(buf, c, W * H); }

void Canvas::r(int x0, int y0, int x1, int y1, Idx c) {
  if (c == T) return;
  if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
  if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 >= W) x1 = W - 1;
  if (y1 >= H) y1 = H - 1;
  for (int y = y0; y <= y1; y++) memset(buf + y * W + x0, c, x1 - x0 + 1 > 0 ? x1 - x0 + 1 : 0);
}

void Canvas::box(int x0, int y0, int x1, int y1, Idx fill, Idx out) {
  r(x0, y0, x1, y1, out);
  r(x0 + 1, y0 + 1, x1 - 1, y1 - 1, fill);
}

static Idx mapGet(const MapEntry *m, char ch) {
  for (; m->ch; m++) if (m->ch == ch) return m->c;
  return T;
}

void Canvas::stamp(int x, int y, const char *const *rows, int nrows, const MapEntry *map, bool flip) {
  int w = 0;
  for (int j = 0; j < nrows; j++) { int l = (int)strlen(rows[j]); if (l > w) w = l; }
  for (int j = 0; j < nrows; j++) {
    const char *row = rows[j];
    for (int i = 0; row[i]; i++) {
      char ch = row[i];
      if (ch == '.' || ch == ' ') continue;
      p(flip ? x + w - 1 - i : x + i, y + j, mapGet(map, ch));
    }
  }
}

void Canvas::ellipse(int cx, int cy, int rx, int ry, Idx c) {
  for (int y = cy - ry; y <= cy + ry; y++) {
    float dy = (y - cy) / (float)ry;
    float dx = sqrtf(1.f - dy * dy) * rx;
    r((int)(cx - dx), y, (int)(cx + dx), y, c);
  }
}

Idx darker(Idx c) {
  switch (c) {
    case F1: case FL: return F3;
    case F2: return F3;
    case F3: return W3;
    case LAMPF: case SPOTF: case MOONF: return F2;
    case RUG: return RUG2;
    case RUG2: return OUT;
    case CRM: return WAL2;
    case WHT: return CRM;
    case GRN: return GRN2;
    case GRN2: return W3;
    case YEL: return WH;
    case WH: return W2;
    case W1: return W2;
    case W2: return W3;
    case RED: return W3;
    default: return c;
  }
}
void Canvas::darken(int x0, int x1, int y) {
  for (int x = x0; x <= x1; x++) p(x, y, darker(get(x, y)));
}

#if ROOM_SPRITES
#include "sprites_gen.h"
// Decode one RLE stream (tools/sprites.py: c < 0x80 repeats the next byte c times; 0xC0|n is followed by n
// opaque literals, 0x80|n by n literals that may hold transparent pixels) of ow x oh pixels with its top-left
// at canvas (tx, ty), writing non-zero indices inside the clip rect (cx0..cx1, cy0..cy1). flip mirrors each
// row (the caller has already moved tx for it). Opaque literals and runs are block copies; only mixed
// literals and flipped rows go pixel by pixel.
static void rle_draw(Idx *buf, const uint8_t *p, int n, int ow, int oh, int tx, int ty, bool flip,
                     int cx0, int cy0, int cx1, int cy1) {
  const uint8_t *end = p + n;
  int x = 0, y = 0;
  while (p < end && y < oh) {
    uint8_t c = *p++;
    bool lit = c & 0x80, opaque = (c & 0xC0) == 0xC0;
    int len = lit ? c & 0x3F : c;
    Idx run = lit ? 0 : *p++;
    while (len > 0) {
      int seg = ow - x;
      if (seg > len) seg = len;
      int yy = ty + y;
      if (yy >= cy0 && yy <= cy1 && (lit || run)) {
        Idx *row = buf + yy * W;
        if (!flip) {
          int xx = tx + x, i0 = xx < cx0 ? cx0 - xx : 0, i1 = xx + seg > cx1 + 1 ? cx1 + 1 - xx : seg;
          if (i0 < i1) {
            if (opaque) memcpy(row + xx + i0, p + i0, i1 - i0);
            else if (lit) { for (int i = i0; i < i1; i++) { Idx v = p[i]; if (v) row[xx + i] = v; } }
            else memset(row + xx + i0, run, i1 - i0);
          }
        } else {
          for (int i = 0; i < seg; i++) {
            Idx v = lit ? p[i] : run;
            int xx = tx + ow - 1 - (x + i);
            if (v && xx >= cx0 && xx <= cx1) row[xx] = v;
          }
        }
      }
      if (lit) p += seg;
      len -= seg;
      x += seg;
      if (x == ow) { x = 0; y++; }
    }
  }
}

void Canvas::blitRect(int x, int y, const spr::Sprite &s, bool flip, int cx0, int cy0, int cx1, int cy1) {
  int x0 = x - s.ax, y0 = y - s.ay;
  if (cx0 < 0) cx0 = 0;
  if (cy0 < 0) cy0 = 0;
  if (cx1 > W - 1) cx1 = W - 1;
  if (cy1 > H - 1) cy1 = H - 1;
  if (x0 > cx1 || y0 > cy1 || x0 + s.w <= cx0 || y0 + s.h <= cy0) return;
  const spr::Sprite *b = s.base;
  if (b) rle_draw(buf, b->rle, b->n, b->ow, b->oh, x0 + (flip ? s.w - b->ox - b->ow : b->ox), y0 + b->oy, flip, cx0, cy0, cx1, cy1);
  rle_draw(buf, s.rle, s.n, s.ow, s.oh, x0 + (flip ? s.w - s.ox - s.ow : s.ox), y0 + s.oy, flip, cx0, cy0, cx1, cy1);
}

void Canvas::blit(int x, int y, const spr::Sprite &s, bool flip) { blitRect(x, y, s, flip, 0, 0, W - 1, H - 1); }
#endif

// ---------------------------------------------------------------- the room
void drawWalls(Canvas &c) {
  c.r(4, 4, 115, 31, WAL);
  for (int x = 4; x < 116; x += 6) c.r(x, 6, x, 29, WAL2);
  c.r(0, 0, 119, 3, CAP); c.r(0, 0, 3, 105, CAP); c.r(116, 0, 119, 105, CAP);
  c.r(0, 0, 119, 0, OUT); c.r(0, 0, 0, 105, OUT); c.r(119, 0, 119, 105, OUT);
  c.r(4, 4, 115, 4, W2);                            // crown shadow
  c.r(4, 29, 115, 31, W2); c.r(4, 29, 115, 29, WH); // baseboard
  c.r(3, 4, 3, 103, OUT); c.r(116, 4, 116, 103, OUT); c.r(3, 3, 116, 3, OUT);
  c.r(0, 104, 119, 105, OUT);
  c.r(52, 104, 67, 105, F3);                        // door gap
  c.box(52, 98, 67, 104, RED); c.r(54, 100, 65, 100, YEL); c.r(54, 102, 65, 102, YEL);   // mat
}

void drawFloor(Canvas &c) {
  for (int y = 32; y < 104; y++) {
    int row = (y - 32) / 6;
    for (int x = 4; x < 116; x++) {
      Idx col = F1;
      if ((y - 32) % 6 == 5) col = F3;
      else if ((x + (row % 2) * 13) % 26 == 0) col = F2;
      c.p(x, y, col);
    }
  }
}

void drawSunPatch(Canvas &c, int offset, int len, Idx col) {
  for (int y = 32; y < 32 + len; y++) {
    int off = (y - 32) / 2 + offset;
    for (int x = 30 + off; x < 48 + off; x++) {
      Idx g = c.get(x, y);
      if (g == F1) c.p(x, y, col);
      else if (g == F2) c.p(x, y, F1);
    }
  }
}

void drawWindow(Canvas &c, const WindowState &w) {
  c.box(29, 7, 48, 26, CRM);
  if (w.mode == SKY_DAY) {
    c.r(31, 9, 46, 24, SKYA);
    c.r(31, 9, 46, 12, SKYB);
    if (!w.rain) {
      // two clouds drifting right, wrapping inside the pane
      static const char *const CL[] = {".####.", "######"};
      static const char *const CL2[] = {"..###", "#####"};
      int cx = 31 + (w.cloudX % 22) - 6;
      c.stamp(cx, 14, CL, ROOM_MAP({'#', CLOUD}));
      int cx2 = 31 + ((w.cloudX + 13) % 22) - 5;
      c.stamp(cx2, 19, CL2, ROOM_MAP({'#', CLOUD}));
    } else {
      for (int k = 0; k < 7; k++) {
        int x = 31 + (k * 5 + w.rainT * 0) % 16;
        int y = 9 + ((k * 7 + w.rainT * 2) % 16);
        c.p(x, y, RAIN); c.p(x, y + 1, RAIN);
      }
    }
  } else {
    c.r(31, 9, 46, 24, SKYA);
    // moon: phase 0..7 (4 = full)
    int mx = 41, my = 12;
    c.r(mx, my, mx + 2, my + 2, MOONC); c.p(mx, my, SKYA); c.p(mx + 2, my + 2, SKYA);
    if (w.moonPhase < 3) c.r(mx, my, mx, my + 2, SKYA);
    else if (w.moonPhase > 5) c.r(mx + 2, my, mx + 2, my + 2, SKYA);
    static const int8_t STARS[][2] = {{33, 11}, {37, 18}, {44, 22}, {35, 22}, {45, 10}, {32, 17}, {40, 21}};
    for (int k = 0; k < 7; k++) {
      bool on = ((w.starT / 7 + k * 3) % 5) != 0;
      if (on) c.p(STARS[k][0], STARS[k][1], STAR);
    }
    if (w.shootingStar) {
      int t = w.shootT;
      int sx = 46 - t * 2, sy = 10 + t;
      c.p(sx, sy, STAR); c.p(sx + 1, sy - 1, STAR); c.p(sx + 2, sy - 1, MOONC);
    }
  }
  // frame + sill clear the pane edges
  c.r(38, 9, 39, 24, CRM); c.r(31, 16, 46, 17, CRM);
  if (w.rain && w.mode == SKY_DAY) {   // rain streaks on the glass over the frame
    for (int k = 0; k < 4; k++) {
      int y = 9 + ((k * 5 + w.rainT * 3) % 15);
      c.p(33 + k * 4, y, RAIN);
    }
  }
  c.r(29, 27, 48, 28, WH); c.r(29, 28, 48, 28, W2);  // sill
  if (w.bird >= 0) drawBird(c, 40, 23, w.bird);
  // curtains
  for (int k = 0; k < 2; k++) {
    int x0 = k == 0 ? 26 : 48;
    c.r(x0, 6, x0 + 3, 25, RED);
    c.r(x0 + (k == 0 ? 0 : 3), 6, x0 + (k == 0 ? 0 : 3), 25, OUT);
    c.r(x0 + 1, 7, x0 + 1, 24, CH);
  }
  c.r(25, 5, 52, 5, W3);
}

void drawWhiteboard(Canvas &c, int total, int done) {
  c.box(55, 8, 74, 22, WB);
  c.r(56, 22, 73, 23, MET); c.r(56, 23, 73, 23, DRK);
  // plan: up to 5 lines with a checkbox each; done ones get a green tick line
  int n = total > 5 ? 5 : total;
  if (n <= 0) {   // doodle
    c.r(57, 10, 62, 10, RUG2); c.r(57, 12, 65, 12, RUG2); c.r(57, 14, 61, 14, RUG2);
    c.box(66, 13, 72, 19, WB, RED); c.r(58, 17, 63, 17, GRN); c.r(58, 19, 61, 19, GRN);
  } else {
    int dn = total > 5 ? (done * 5 + total / 2) / total : done;
    for (int i = 0; i < n; i++) {
      int y = 10 + i * 2 + (i >= 3 ? 1 : 0);
      bool ok = i < dn;
      c.p(57, y, ok ? GRN : INK);
      c.r(59, y, 59 + 4 + (i * 3) % 7, y, ok ? GRN : RUG2);
    }
    if (dn >= n && n > 0) { c.p(70, 11, GRN); c.p(71, 10, GRN); c.p(69, 12, GRN); }
  }
  c.p(60, 22, RED); c.p(62, 22, RUG2);
}

void drawBookshelf(Canvas &c, int gap) {
  c.r(5, 6, 24, 9, WH); c.r(5, 6, 24, 6, OUT); c.r(5, 6, 5, 40, OUT); c.r(24, 6, 24, 40, OUT);
  c.r(6, 10, 23, 39, W2); c.r(5, 40, 24, 40, OUT);
  static const Idx cols[] = {RED, GRN, RUG2, YEL, PUR, RUG, RED, GRN, YEL};
  int slot = 0;
  for (int si = 0; si < 3; si++) {
    int sy = 11 + si * 8;
    c.r(7, sy, 22, sy + 6, W3);
    int x = 7, k = si * 3;
    while (x < 22) {
      int w = (x + si) % 3 ? 2 : 1;
      int hh = 6 - ((x * 7 + si) % 3 == 0 ? 1 : 0);
      Idx col = cols[k % 9]; k++;
      if (slot != gap) {
        c.r(x, sy + 6 - hh + 1, imin(x + w - 1, 22), sy + 6, col);
        if (w == 2 && col != YEL) c.p(x, sy + 6 - hh + 1, CRM);
      }
      slot++;
      x += w + (k % 4 == 0 ? 1 : 0);
    }
    c.r(6, sy + 7, 23, sy + 7, WH);
  }
  c.r(6, 35, 23, 39, W1); c.r(13, 37, 16, 37, W3);
}

static void screenCode(Canvas &c, int t) {
  static const int8_t LINES[][2] = {{0, 7}, {2, 5}, {2, 9}, {4, 4}, {2, 8}, {0, 3}, {4, 6}, {2, 5}};
  int scroll = (t / 6) % 8;
  for (int i = 0; i < 5; i++) {
    int li = (i + scroll) % 8;
    int ind = LINES[li][0], ln = LINES[li][1];
    int yy = 19 + i * 2;
    if (i == 4) ln = imax(1, (t % 6) + 1);
    c.r(82 + ind, yy, imin(82 + ind + ln, 93), yy, SCR);
  }
  if ((t / 3) % 2 == 0) c.p(imin(82 + 2 + (t % 6) + 3, 93), 27, SCRW);
}

static void screenWeb(Canvas &c, int t) {
  c.r(81, 18, 94, 27, SCRW);
  c.r(81, 18, 94, 19, BLUEL);
  c.r(82, 21, 88, 21, INK); c.r(82, 23, 92, 23, INK); c.r(82, 25, 90, 25, INK);
  int sc = (t / 8) % 3;
  c.r(82 + sc * 2, 27, 85 + sc * 2, 27, RUG2);
}

static void screenSaver(Canvas &c, int t) {
  // a bouncing dot in the dark
  int px = 82 + (t * 1) % 24, py = 19 + (t / 2) % 16;
  if (px > 93) px = 93 - (px - 93);
  if (py > 26) py = 26 - (py - 26);
  c.p(px, py, SCRD); c.p(px + 1, py, SCRD);
  for (int k = 0; k < 3; k++) c.p(83 + (t * 3 + k * 5) % 11, 20 + (t + k * 3) % 7, SCRD);
}

void drawDesk(Canvas &c, const DeskState &d) {
  c.box(79, 16, 96, 29, DRK);
  c.r(81, 18, 94, 27, d.screen == SCREEN_CODE ? SCRD : DRK2);
  switch (d.screen) {
    case SCREEN_CODE: screenCode(c, d.t); break;
    case SCREEN_WEB: screenWeb(c, d.t); break;
    case SCREEN_SAVER: screenSaver(c, d.t); break;
    case SCREEN_DIM: c.r(83, 20, 86, 20, SCR); break;
    case SCREEN_PLUG: {
      static const char *const PL[] = {".#.#.", "#####", ".###.", "..#..", "..#.."};
      c.stamp(85, 19, PL, ROOM_MAP({'#', INK}));
      break;
    }
    default: break;
  }
  c.r(86, 30, 89, 31, MET); c.r(86, 31, 89, 31, DRK);
  // desk body
  c.r(70, 29, 101, 30, OUT);
  c.r(70, 31, 101, 38, WH); c.r(70, 31, 70, 46, OUT); c.r(101, 31, 101, 46, OUT);
  c.r(71, 39, 100, 41, W1); c.r(71, 42, 100, 42, OUT);
  c.r(71, 43, 73, 46, W2); c.r(98, 43, 100, 46, W2);
  c.r(74, 43, 97, 46, W3);
  c.r(70, 47, 74, 47, OUT); c.r(97, 47, 101, 47, OUT);
  if (d.drawer) { c.r(71, 39, 78, 42, WH); c.r(71, 43, 78, 43, OUT); c.r(73, 40, 76, 40, W3); }
  else { c.r(72, 40, 75, 40, W2); c.p(73, 40, YEL); }
  // keyboard, mug, papers
  c.box(82, 33, 94, 36, MET); c.r(83, 34, 93, 34, d.lit ? CRM : WAL2);
  if (d.mug) {
    c.box(73, 32, 76, 35, CRM); c.p(77, 33, OUT);
    if (d.steam > 0) {
      int t = d.steam;
      c.p(74 + (t / 3) % 2, 30 - (t / 4) % 2, WAL2);
      c.p(75 - (t / 5) % 2, 28 - (t / 3) % 2, WAL2);
    }
  }
  c.r(97, 32, 99, 35, CRM); c.r(97, 33, 98, 33, INK);
}

void drawRack(Canvas &c, bool active, int t, int progress, uint32_t seed) {
  c.r(103, 7, 115, 9, MET); c.r(103, 6, 115, 6, OUT);
  c.r(102, 6, 102, 44, OUT);
  c.r(103, 10, 115, 43, DRK); c.r(102, 44, 115, 44, OUT);
  for (int i = 0; i < 7; i++) {
    int uy = 11 + i * 5;
    c.r(104, uy, 114, uy + 3, DRK2); c.r(104, uy + 3, 114, uy + 3, OUT);
    for (int j = 0; j < 4; j++) {
      Idx col = DRK;
      if (active) {
        uint32_t h = (seed + i * 7 + j * 13 + (uint32_t)(t / 2) * 31) * 2654435761u;
        bool on = (h >> 8) % 3 != 0;
        Idx kind = ((h >> 12) % 7 == 0) ? LEDR : ((h >> 16) % 5 == 0) ? LEDY : LEDG;
        col = on ? kind : DRK;
      } else {
        bool blink = ((t / 24) + i) % 5 == 0 && j == 0;
        col = (j == 0) ? (blink ? LEDG : SCRD) : DRK;
      }
      c.p(105 + j * 2, uy + 1, col);
    }
    c.r(110, uy + 1, 113, uy + 1, INK);
  }
  c.r(101, 45, 102, 45, DRK2);
  if (progress >= 0) {
    c.box(100, 1, 117, 5, DRK2);
    int fill = progress > 16 ? 16 : progress;
    if (fill > 0) c.r(101, 2, 100 + fill, 4, LEDG);
  }
}

void drawRug(Canvas &c) {
  c.r(41, 57, 80, 81, OUT);
  c.r(42, 58, 79, 80, CRM);
  c.r(44, 60, 77, 78, RUG);
  c.r(46, 62, 75, 76, RUG2); c.r(47, 63, 74, 75, RUG);
  for (int x = 48; x < 74; x += 4) { c.p(x, 69, CRM); c.p(x + 2, 66, YEL); c.p(x + 2, 72, YEL); }
  for (int x = 42; x < 80; x += 2) { c.p(x, 81, CRM); c.p(x, 56, CRM); }
}

void drawBed(Canvas &c, bool sleeping, int breath) {
  c.r(5, 57, 27, 97, OUT);
  c.r(6, 58, 26, 62, W1); c.r(6, 58, 26, 58, WH);
  c.r(6, 63, 26, 92, CRM);
  c.box(9, 63, 23, 69, WHT, INK);
  c.r(10, 68, 22, 68, CRM);
  c.r(6, 71, 26, 92, GRN); c.r(6, 71, 26, 72, CRM); c.r(6, 73, 26, 73, GRN2);
  for (int y = 76; y < 92; y += 5)
    for (int x = 8; x < 26; x += 5) { c.p(x, y, GRN2); c.p(x + 1, y, YEL); }
  c.r(6, 93, 26, 96, W2); c.r(6, 93, 26, 93, W1);
  if (sleeping) {
    int b = breath;   // 0 / 1: the blanket rises a pixel
    c.r(10, 75 - b, 22, 86, GRN); c.r(9, 76 - b, 9, 85, GRN2); c.r(23, 76 - b, 23, 85, GRN2);
    c.r(10, 87, 22, 87, GRN2);
    c.r(10, 75 - b, 22, 75 - b, GRN2);
  }
}

void drawNightstand(Canvas &c, bool lampOn) {
  c.box(29, 58, 37, 68, W1); c.r(30, 59, 36, 61, WH); c.r(32, 65, 34, 65, W3);
  c.r(32, 53, 34, 59, OUT); c.p(33, 57, MET); c.p(33, 58, MET);
  c.r(30, 50, 36, 54, OUT); c.r(31, 51, 35, 53, lampOn ? LAMP : W1);
  if (lampOn) c.r(31, 51, 35, 51, QW);
}

void drawLampPool(Canvas &c) {
  for (int y = 60; y <= 72; y++) {
    float dy = (y - 66) / 7.f;
    float dx = sqrtf(imax(0.f, 1.f - dy * dy)) * 12;
    for (int x = (int)(33 - dx); x <= (int)(33 + dx); x++) {
      Idx g = c.get(x, y);
      if (g == F1 || g == F2) c.p(x, y, LAMPF);
    }
  }
}

void drawPlant(Canvas &c, int sway) {
  c.box(103, 89, 112, 98, CS); c.r(104, 90, 111, 90, CH); c.r(104, 91, 111, 91, W3);
  static const char *const L[] = {
      "....g.g.....", "...gGg.G....", ".g.gGgGg.g..", "gGg.gGGg.Gg.", ".gGggGggGg..", "..gGgGgGg...", "...gGGGg....",
  };
  c.stamp(101 + sway, 82, L, ROOM_MAP({'g', GRN}, {'G', GRN2}));
}

void drawDoorSpot(Canvas &c) {
  for (int y = 78; y < 104; y++) {
    float dy = (y - 98) / 14.f;
    float dx = sqrtf(imax(0.f, 1.f - dy * dy)) * 21;
    for (int x = (int)(60.5f - dx); x <= (int)(60.5f + dx); x++) {
      Idx g = c.get(x, y);
      if (g == F1 || g == F2 || g == F3 || g == FL || g == LAMPF) c.p(x, y, SPOTF);
    }
  }
}

void drawAlarmRing(Canvas &c, int phase) {
  static const Idx A[4] = {QO, QY, QY, QO}, B[4] = {QY, QO, QO, OUT};
  const Idx *ring = phase ? B : A;
  for (int i = 0; i < 4; i++) {
    Idx col = ring[i];
    c.r(i, i, W - 1 - i, i, col); c.r(i, 105 - i, W - 1 - i, 105 - i, col);
    c.r(i, i, i, 105 - i, col); c.r(W - 1 - i, i, W - 1 - i, 105 - i, col);
  }
}

void drawUnderDeskBeam(Canvas &c, int t) {
  c.r(74, 43, 97, 46, DRK2);
  int flick = (t / 5) % 3 == 0 ? 1 : 0;
  for (int x = 70; x < 97 - flick; x++) {
    int k = (x - 70) / 4;
    for (int y = 45 - k; y < 47 + k; y++) {
      if (y < 43 || y > 50) continue;
      Idx g = c.get(x, y);
      if (g == DRK2 || g == W3) c.p(x, y, TORCH2);
      else if (g == F1 || g == F2 || g == F3 || g == W2 || g == FL) c.p(x, y, TORCH);
    }
  }
}

void drawPaper(Canvas &c, int x, int y) { c.r(x, y, x + 3, y + 2, CRM); c.r(x + 1, y + 1, x + 2, y + 1, INK); }

// ---------------------------------------------------------------- Clawd
static void tintCols(Tint t, Idx &b, Idx &s, Idx &h) {
  switch (t) {
    case TINT_GREY: b = GC; s = GCS; h = GCH; break;
    case TINT_VIOLET: b = VC; s = VCS; h = VCH; break;
    default: b = C; s = CS; h = CH; break;
  }
}

void drawClawd(Canvas &c, const ClawdPose &p) {
  Idx B, S, Hh;
  tintCols(p.tint, B, S, Hh);
  int x = p.x, y = p.y;
  if (p.shadow) c.darken(x + 3, x + 14, y + 12);
  // legs: two pairs of stubby 2 px feet (like the mascot's four little feet); walk phases lift alternate pairs
  for (int i = 0; i < 4; i++) {
    static const int8_t LX[4] = {3, 6, 10, 13};
    int lx = x + LX[i];
    int up = 0;
    if (p.legPhase == 1 && (i % 2 == 0)) up = 1;
    if (p.legPhase == 3 && (i % 2 == 1)) up = 1;
    c.r(lx, y + 9, lx + 1, y + 10 - up, S);
    c.r(lx, y + 11 - up, lx + 1, y + 11 - up, OUT);
  }
  // arms
  bool lu = p.arms == ARMS_UP || p.arms == ARMS_UP_L || p.arms == ARMS_WAVE_A || p.arms == ARMS_WAVE_B;
  bool ru = p.arms == ARMS_UP || p.arms == ARMS_UP_R;
  bool typing = p.arms == ARMS_TYPE_A || p.arms == ARMS_TYPE_B;
  bool none = p.arms == ARMS_NONE;
  auto arm = [&](int side, bool up) {
    int ax = side < 0 ? x : x + 15;
    Idx col = side < 0 ? B : S;
    if (typing) {   // arms forward (toward the top of the sprite), alternating
      int lift = (p.arms == ARMS_TYPE_A) == (side < 0) ? 1 : 0;
      int ax2 = ax + (side < 0 ? 1 : -1);
      c.r(ax2, y + 1 - lift, ax2 + 2, y + 4 - lift, OUT); c.r(ax2 + 1, y + 2 - lift, ax2 + 1, y + 3 - lift, col);
    } else if (!up) {
      c.r(ax, y + 2, ax + 2, y + 5, OUT); c.r(ax + (side < 0 ? 1 : 0), y + 3, ax + (side < 0 ? 2 : 1), y + 4, col);
    } else {
      int ax2 = ax + (side < 0 ? 1 : -1);
      int tilt = (p.arms == ARMS_WAVE_B && side < 0) ? 1 : 0;
      c.r(ax2 - tilt, y - 2, ax2 + 2 - tilt, y + 3, OUT); c.r(ax2 + 1 - tilt, y - 1, ax2 + 1 - tilt, y + 2, col);
    }
  };
  if (!none) {
    if (p.view != RIGHTV) arm(-1, lu);
    if (p.view != LEFTV) arm(1, ru);
  }
  // body
  c.r(x + 3, y, x + 14, y, OUT); c.r(x + 3, y + 8, x + 14, y + 8, OUT);
  c.r(x + 2, y + 1, x + 2, y + 7, OUT); c.r(x + 15, y + 1, x + 15, y + 7, OUT);
  c.r(x + 3, y + 1, x + 14, y + 7, B);
  c.r(x + 3, y + 7, x + 14, y + 7, S); c.r(x + 14, y + 1, x + 14, y + 7, S);
  c.r(x + 3, y + 1, x + 13, y + 1, Hh); c.r(x + 3, y + 2, x + 3, y + 3, Hh);
  // arm joints over the outline so the nubs connect
  if (!none) {
    if (p.view != RIGHTV) { if (typing) c.r(x + 2, y + 1, x + 2, y + 2, B); else if (lu) c.r(x + 2, y + 1, x + 2, y + 2, B); else c.r(x + 2, y + 3, x + 2, y + 4, B); }
    if (p.view != LEFTV) { if (typing) c.r(x + 15, y + 1, x + 15, y + 2, S); else if (ru) c.r(x + 15, y + 1, x + 15, y + 2, S); else c.r(x + 15, y + 3, x + 15, y + 4, S); }
  }
  // face
  if (p.view == FRONT) {
    int L = x + 5, R = x + 11;
    switch (p.eyes) {
      case EYES_OPEN: c.r(L, y + 3, L + 1, y + 5, OUT); c.r(R, y + 3, R + 1, y + 5, OUT); break;
      case EYES_LOOK_L: c.r(L - 1, y + 3, L, y + 5, OUT); c.r(R - 1, y + 3, R, y + 5, OUT); break;
      case EYES_LOOK_R: c.r(L + 1, y + 3, L + 2, y + 5, OUT); c.r(R + 1, y + 3, R + 2, y + 5, OUT); break;
      case EYES_UP: c.r(L, y + 2, L + 1, y + 4, OUT); c.r(R, y + 2, R + 1, y + 4, OUT); break;
      case EYES_WIDE:
        c.r(L - 1, y + 2, L + 1, y + 5, OUT); c.r(R, y + 2, R + 2, y + 5, OUT);
        c.p(L - 1, y + 2, WHT); c.p(R, y + 2, WHT);
        break;
      case EYES_HAPPY:
        for (int k = 0; k < 2; k++) {
          int ex = k ? R : L - 1;
          c.r(ex, y + 4, ex, y + 5, OUT); c.r(ex + 1, y + 3, ex + 1, y + 4, OUT); c.r(ex + 2, y + 4, ex + 2, y + 5, OUT);
        }
        break;
      case EYES_WINCE:
        for (int k = 0; k < 2; k++) {
          int ex = k ? R : L - 1;
          c.p(ex, y + 3, OUT); c.p(ex + 1, y + 4, OUT); c.p(ex + 2, y + 5, OUT);
          c.p(ex, y + 5, OUT); c.p(ex + 2, y + 3, OUT);
        }
        break;
      case EYES_WINK:
        c.r(L, y + 3, L + 1, y + 5, OUT);
        c.r(R, y + 4, R + 1, y + 4, OUT);
        break;
      case EYES_SLEEP:
      case EYES_BLINK:
      default: c.r(L, y + 5, L + 1, y + 5, OUT); c.r(R, y + 5, R + 1, y + 5, OUT); break;
    }
    switch (p.mouth) {
      case MOUTH_O: c.r(x + 8, y + 6, x + 9, y + 7, OUT); break;
      case MOUTH_SMILE: c.p(x + 7, y + 6, OUT); c.r(x + 8, y + 7, x + 9, y + 7, OUT); c.p(x + 10, y + 6, OUT); break;
      case MOUTH_FLAT: c.r(x + 7, y + 7, x + 10, y + 7, OUT); break;
      default: break;
    }
    if (p.blush) { c.p(x + 4, y + 6, BLUSH); c.p(x + 13, y + 6, BLUSH); }
  } else if (p.view == RIGHTV) {
    if (p.eyes == EYES_BLINK || p.eyes == EYES_SLEEP) c.r(x + 12, y + 5, x + 13, y + 5, OUT);
    else c.r(x + 12, y + 3, x + 13, y + 5, OUT);
  } else if (p.view == LEFTV) {
    if (p.eyes == EYES_BLINK || p.eyes == EYES_SLEEP) c.r(x + 4, y + 5, x + 5, y + 5, OUT);
    else c.r(x + 4, y + 3, x + 5, y + 5, OUT);
  } else {   // back
    c.r(x + 3, y + 6, x + 14, y + 7, S);
  }
}

void drawClawdInBed(Canvas &c, int shift, bool eyesOpen, Tint tint) {
  Idx B, S, Hh;
  tintCols(tint, B, S, Hh);
  int hx = 10 + shift, hy = 64;
  c.r(hx + 1, hy, hx + 10, hy, OUT); c.r(hx, hy + 1, hx, hy + 7, OUT); c.r(hx + 11, hy + 1, hx + 11, hy + 7, OUT);
  c.r(hx + 1, hy + 1, hx + 10, hy + 7, S); c.r(hx + 1, hy + 1, hx + 9, hy + 1, B);
  if (eyesOpen) { c.r(hx + 3, hy + 3, hx + 4, hy + 4, OUT); c.r(hx + 7, hy + 3, hx + 8, hy + 4, OUT); }
  else { c.r(hx + 3, hy + 4, hx + 4, hy + 4, OUT); c.r(hx + 7, hy + 4, hx + 8, hy + 4, OUT); }
  c.r(6, 71, 26, 72, CRM);   // blanket fold over the chin
  c.r(6, 73, 26, 73, GRN2);
}

void drawMini(Canvas &c, int x, int y, View v, int legPhase, Tint tint, bool blink) {
  Idx B, S, Hh;
  tintCols(tint, B, S, Hh);
  c.darken(x + 2, x + 9, y + 9);
  c.r(x + 2, y, x + 9, y, OUT); c.r(x + 2, y + 6, x + 9, y + 6, OUT);
  c.r(x + 1, y + 1, x + 1, y + 5, OUT); c.r(x + 10, y + 1, x + 10, y + 5, OUT);
  c.r(x + 2, y + 1, x + 9, y + 5, B); c.r(x + 2, y + 1, x + 8, y + 1, Hh);
  c.r(x + 9, y + 1, x + 9, y + 5, S); c.r(x + 2, y + 5, x + 9, y + 5, S);
  int up = legPhase % 2;
  c.r(x - 1, y + 2 - up, x, y + 3 - up, OUT); c.r(x + 11, y + 1 + up, x + 12, y + 2 + up, OUT);
  c.p(x, y + 3 - up, B); c.p(x + 11, y + 2 + up, S);
  for (int i = 0; i < 2; i++) {
    int lx = x + 2 + i * 5;
    int lift = (legPhase == 1 && i == 0) || (legPhase == 3 && i == 1) ? 1 : 0;
    c.r(lx, y + 7 - lift, lx + 1, y + 7 - lift, S); c.r(lx, y + 8 - lift, lx + 1, y + 8 - lift, OUT);
  }
  if (v == FRONT) {
    if (blink) { c.p(x + 3, y + 3, OUT); c.p(x + 7, y + 3, OUT); }
    else { c.r(x + 3, y + 2, x + 3, y + 3, OUT); c.r(x + 7, y + 2, x + 7, y + 3, OUT); }
  } else if (v == BACK) {
    c.r(x + 2, y + 4, x + 9, y + 5, S);
  } else if (v == RIGHTV) {
    c.r(x + 8, y + 2, x + 8, y + 3, OUT);
  } else {
    c.r(x + 3, y + 2, x + 3, y + 3, OUT);
  }
}

// ---------------------------------------------------------------- props
void drawBook(Canvas &c, int x, int y, int page) {
  c.r(x, y, x + 13, y + 6, OUT);
  c.r(x + 1, y + 1, x + 6, y + 5, CRM); c.r(x + 7, y + 1, x + 12, y + 5, WHT);
  c.r(x + 6, y + 1, x + 7, y + 6, RED);
  for (int i = 0; i < 2; i++) {
    int yy = y + 2 + i * 2;
    c.r(x + 2, yy, x + 5 - (i == 1 ? 1 : 0), yy, INK);
    c.r(x + 8, yy, x + 11 - ((i == 1 && (page % 2)) ? 2 : 0) - ((page % 3) == 2 ? 1 : 0), yy, INK);
  }
}

void drawCan(Canvas &c, int x, int y, bool pour, int t) {
  static const char *const CAN[] = {".....#.", "#...##.", "####.#.", "#####..", "####...", ".###..."};
  c.stamp(x, y, CAN, ROOM_MAP({'#', MET}));
  if (pour) {
    for (int k = 0; k < 3; k++) {
      int dy = (t + k * 2) % 5;
      c.p(x + 7 + k, y + 1 + dy, BLUEL);
    }
  }
}

void drawTorch(Canvas &c, int x, int y) {
  c.r(x, y, x + 3, y + 3, OUT); c.r(x + 1, y + 1, x + 2, y + 2, MET); c.r(x + 3, y + 1, x + 3, y + 2, QY);
}

void drawMugHeld(Canvas &c, int x, int y) { c.box(x, y, x + 3, y + 3, CRM); c.p(x + 4, y + 1, OUT); }

// ---------------------------------------------------------------- FX
static const char *const ICONS[ICO_COUNT][5] = {
    {"..#...#..", ".#.....#.", "#..#.#..#", ".#.#.#.#.", "..#...#.."},   // code
    {"..#......", "..##.....", "..###....", "..##.....", "..#......"},   // play
    {"....#....", "...###...", "#########", "..#####..", ".##...##."},   // star
    {"..###....", ".#...#...", ".#...#...", "..###....", ".....##.."},   // look
    {".......#.", "......#..", "#....#...", ".#..#....", "..##....."},   // check
    {".#.....#.", "..#...#..", "...#.#...", "..#...#..", ".#.....#."},   // cross
    {".##..##..", "#######..", "#######..", ".#####...", "...#....."},   // heart
    {"....##...", "....#.#..", "....#....", "..###....", "..###...."},   // note
    {"..###....", ".#...#...", "....#....", "...#.....", "...#....."},   // question
    {"....#....", "....#....", "....#....", ".........", "....#...."},   // bang
    {"......#..", "...####..", "..#####..", "..####...", ".#......."},   // leaf
    {"#######..", "#..#..#..", "#..#..#..", "#..#..#..", "#######.."},   // book
    {".#...#...", ".#...#...", "#######..", ".#####...", "...#....."},   // plug
    {"..###....", ".#.#.#...", "#######..", ".#.#.#...", "..###...."},   // web
    {"......#..", ".....##..", "....##...", "...##....", "..#......"},   // pen
    {".#######.", ".#.....##", ".#.....##", ".#.....#.", "..#####.."},   // coffee
};

void drawEmote(Canvas &c, int x, int y, Icon icon, Idx col, bool tailRight) {
  c.r(x + 1, y, x + 11, y + 8, OUT); c.r(x, y + 1, x + 12, y + 7, OUT);
  c.r(x + 1, y + 1, x + 11, y + 7, QW);
  if (!tailRight) { c.r(x + 2, y + 9, x + 4, y + 9, OUT); c.p(x + 3, y + 8, QW); c.p(x + 2, y + 10, OUT); }
  else { c.r(x + 8, y + 9, x + 10, y + 9, OUT); c.p(x + 9, y + 8, QW); c.p(x + 10, y + 10, OUT); }
  c.stamp(x + 2, y + 2, ICONS[icon], 5, ROOM_MAP({'#', col}));
}

void drawQuestBang(Canvas &c, int bx0, int by0, int lift) {
  by0 -= lift;
  int x1 = bx0 + 21, y1 = by0 + 33;
  // rounded bubble
  c.r(bx0 + 1, by0, x1 - 1, y1, OUT); c.r(bx0, by0 + 1, x1, y1 - 1, OUT);
  c.r(bx0 + 1, by0 + 1, x1 - 1, y1 - 1, QY);
  c.r(bx0 + 2, by0 + 1, x1 - 3, by0 + 1, QY);
  int tailX = bx0 + 11;
  c.r(tailX - 1, y1, tailX + 1, y1, QY);
  c.r(tailX - 2, y1, tailX - 2, y1 + 1, OUT); c.r(tailX + 2, y1, tailX + 2, y1, OUT);
  c.p(tailX - 1, y1 + 1, QY); c.p(tailX + 1, y1 + 1, OUT); c.p(tailX, y1 + 1, QY);
  c.p(tailX - 1, y1 + 2, OUT); c.p(tailX, y1 + 2, OUT);
  c.r(bx0 + 2, by0 + 2, bx0 + 19, by0 + 2, QW);
  c.r(bx0 + 19, by0 + 3, bx0 + 20, by0 + 31, QO); c.r(bx0 + 2, by0 + 31, bx0 + 19, by0 + 32, QO);
  // the "!"
  c.r(bx0 + 8, by0 + 5, bx0 + 13, by0 + 18, OUT); c.r(bx0 + 9, by0 + 4, bx0 + 12, by0 + 4, OUT);
  c.r(bx0 + 9, by0 + 19, bx0 + 12, by0 + 20, OUT);
  c.r(bx0 + 9, by0 + 24, bx0 + 12, by0 + 28, OUT); c.r(bx0 + 8, by0 + 25, bx0 + 13, by0 + 27, OUT);
  // alert ticks
  static const int8_t TK[6][4] = {{-4, 3, -1, -1}, {26, 3, 1, -1}, {-6, 16, -1, 0}, {28, 16, 1, 0}, {-4, 29, -1, 1}, {26, 29, 1, 1}};
  for (int k = 0; k < 6; k++)
    for (int j = 0; j < 3; j++) c.p(bx0 + TK[k][0] + TK[k][2] * j - lift * TK[k][2], by0 + TK[k][1] + TK[k][3] * j, QY);
}

void drawThought(Canvas &c, int bx0, int by0, int dots, int tailX, int tailY) {
  // trail of two small bubbles from (tailX, tailY) up toward the cloud
  c.r(tailX, tailY, tailX + 2, tailY + 2, OUT); c.p(tailX + 1, tailY + 1, QW);
  c.r(tailX + 3, tailY - 5, tailX + 6, tailY - 2, OUT); c.r(tailX + 4, tailY - 4, tailX + 5, tailY - 3, QW);
  c.r(bx0 + 2, by0, bx0 + 21, by0 + 11, OUT); c.r(bx0, by0 + 2, bx0 + 23, by0 + 9, OUT);
  c.r(bx0 + 1, by0 + 2, bx0 + 22, by0 + 9, QW); c.r(bx0 + 2, by0 + 1, bx0 + 21, by0 + 10, QW);
  for (int i = 0; i < 3; i++) {
    Idx col = i < dots ? OUT : INK;
    c.r(bx0 + 5 + i * 6, by0 + 5, bx0 + 6 + i * 6, by0 + 6, col);
  }
}

static void zee(Canvas &c, int x, int y, int s, Idx col) {
  c.r(x, y, x + s, y, col); c.r(x, y + s, x + s, y + s, col);
  for (int k = 1; k < s; k++) c.p(x + s - k, y + k, col);
}
void drawZzz(Canvas &c, int x, int y, int t) {
  int o = (t / 6) % 3;
  zee(c, x, y - o, 3, BLUEL); zee(c, x + 6, y - 8 - o, 4, BLUEL); zee(c, x + 13, y - 18 - o, 5, QW);
}

void drawSparkle(Canvas &c, int x, int y, Idx col, bool big) {
  c.p(x, y, col); c.p(x - 1, y, col); c.p(x + 1, y, col); c.p(x, y - 1, col); c.p(x, y + 1, col);
  if (big) { c.p(x - 2, y, col); c.p(x + 2, y, col); c.p(x, y - 2, col); c.p(x, y + 2, col); }
}

void drawSweat(Canvas &c, int x, int y) { c.p(x, y, BLUEL); c.r(x - 1, y + 1, x + 1, y + 2, BLUEL); c.p(x, y + 3, BLUEL); }

void drawDash(Canvas &c, int x0, int y0, int x1, int y1, int t, Idx col) {
  int steps = 16;
  for (int i = 2; i < steps - 1; i++) {
    if ((i + t / 4) % 3 == 0) continue;
    c.p(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, col);
  }
}

void drawBird(Canvas &c, int x, int y, int frame) {
  static const char *const B0[] = {"..#..", ".##b.", "####."};
  static const char *const B1[] = {".#...", ".##b.", "####."};
  static const char *const BF[] = {"#...#", ".###b", "..#.."};   // wings up (flying)
  const char *const *rows = frame >= 8 ? BF : (frame % 4 == 1 ? B1 : B0);
  int fy = frame >= 8 ? y - (frame - 8) * 2 : y;
  int fx = frame >= 8 ? x + (frame - 8) * 2 : x;
  c.stamp(fx, fy, rows, 3, ROOM_MAP({'#', DRK}, {'b', QO}));
}

}  // namespace room
