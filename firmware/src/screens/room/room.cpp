// Clawd's Studio: palette, canvas primitives, the sprite blitter and the lighting passes. See room.h; the art is
// in firmware/assets/sprites (README.md next to this file).
#include "room.h"
#include "sprites_gen.h"
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

// Decode one RLE stream (tools/sprites.py: c < 0x80 repeats the next byte c times; 0xC0|n is followed by n
// opaque literals, 0x80|n by n literals that may hold transparent pixels) of ow x oh pixels with its top-left
// at canvas (tx, ty), writing non-zero indices inside the clip rect (cx0..cx1, cy0..cy1). flip mirrors each
// row (the caller has already moved tx for it); remap maps each index written. Opaque literals and runs are
// block copies; mixed literals, flipped or remapped rows go pixel by pixel.
static void rle_draw(Idx *buf, const uint8_t *p, int n, int ow, int oh, int tx, int ty, bool flip, const Idx *remap,
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
        if (!flip && !remap) {
          int xx = tx + x, i0 = xx < cx0 ? cx0 - xx : 0, i1 = xx + seg > cx1 + 1 ? cx1 + 1 - xx : seg;
          if (i0 < i1) {
            if (opaque) memcpy(row + xx + i0, p + i0, i1 - i0);
            else if (lit) { for (int i = i0; i < i1; i++) { Idx v = p[i]; if (v) row[xx + i] = v; } }
            else memset(row + xx + i0, run, i1 - i0);
          }
        } else {
          for (int i = 0; i < seg; i++) {
            Idx v = lit ? p[i] : run;
            int xx = flip ? tx + ow - 1 - (x + i) : tx + x + i;
            if (v && xx >= cx0 && xx <= cx1) row[xx] = remap ? remap[v] : v;
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

void Canvas::blitRect(int x, int y, const spr::Sprite &s, bool flip, int cx0, int cy0, int cx1, int cy1, const Idx *remap) {
  int x0 = x - s.ax, y0 = y - s.ay;
  if (cx0 < 0) cx0 = 0;
  if (cy0 < 0) cy0 = 0;
  if (cx1 > W - 1) cx1 = W - 1;
  if (cy1 > H - 1) cy1 = H - 1;
  if (x0 > cx1 || y0 > cy1 || x0 + s.w <= cx0 || y0 + s.h <= cy0) return;
  const spr::Sprite *b = s.base;
  if (b) rle_draw(buf, b->rle, b->n, b->ow, b->oh, x0 + (flip ? s.w - b->ox - b->ow : b->ox), y0 + b->oy, flip, remap, cx0, cy0, cx1, cy1);
  rle_draw(buf, s.rle, s.n, s.ow, s.oh, x0 + (flip ? s.w - s.ox - s.ow : s.ox), y0 + s.oy, flip, remap, cx0, cy0, cx1, cy1);
}

void Canvas::blit(int x, int y, const spr::Sprite &s, bool flip, const Idx *remap) { blitRect(x, y, s, flip, 0, 0, W - 1, H - 1, remap); }

// ---------------------------------------------------------------- lighting
// Not art but light: they recolour what is already on the canvas (floor planks to their sunlit / moonlit / lamp /
// spot kin, the shadow under the desk to the torch's), so they follow whatever the sheets draw there.
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

}  // namespace room
