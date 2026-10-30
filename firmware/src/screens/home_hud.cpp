// Pixel-art HUD for the home screen, see home_hud.h. Same scheme as the room (home_face.cpp): a 240 x 28 canvas
// of palette indices in internal RAM, upscaled 2x (half the room's zoom: finer text and gauges) straight into
// LVGL's draw buffer by the object's draw callback. The bitmap fonts are hand-made: bold 6x12 digits for the clock,
// a 5x7 upper-case set for the date and the captions.
#include "home_hud.h"
#include <esp_heap_caps.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

namespace {

const int W = 240, H = 28, ZOOM = 2;

enum : uint8_t { BG, TEXT, SHADOW, MUTED, DIM, TRACK, TRACK_LO, OK, OK_LO, WARN, WARN_LO, ERR, ERR_LO, GREY, GREY_LO, N_PAL };
// the HUD's own colours (theme tones); every bar has a lighter top row and a darker bottom row, pixel-art style
const uint32_t PAL_RGB[N_PAL] = {
    0x1D1A24, 0xECE9E4, 0x0E0C12, 0x9C978E, 0x5E5A54, 0x3A3546, 0x2A2633,
    0x7FB069, 0x5A8549, 0xF2C14E, 0xC29636, 0xE5534B, 0xAE3C37, 0x6E6963, 0x4A4642};
uint16_t pal[N_PAL];

uint8_t *px = nullptr;
lv_obj_t *obj = nullptr;
int rows = H;                       // logical rows in use (h / ZOOM)
char clk[8] = "", date[20] = "";
HudBar bars[3];
int n_bars = 0;
lv_area_t bars_area = {0, 0, 0, 0};

// ---- fonts: one string per row, '#' = pixel
struct Glyph { char c; const char *rows[12]; };

const Glyph CLOCK12[] = {   // 6 x 12, 2 px strokes (the thin 5x11 set thickened down-right)
    {'0', {" #### ", "######", "##  ##", "##  ##", "##  ##", "##  ##", "##  ##", "##  ##", "##  ##", "##  ##", "######", " #### "}},
    {'1', {"  ##  ", " ###  ", " ###  ", "  ##  ", "  ##  ", "  ##  ", "  ##  ", "  ##  ", "  ##  ", "  ##  ", " #### ", " #### "}},
    {'2', {" #### ", "######", "##  ##", "    ##", "    ##", "   ###", "  ### ", " ###  ", "###   ", "##    ", "######", "######"}},
    {'3', {" #### ", "######", "##  ##", "    ##", "    ##", " #####", " #####", "    ##", "    ##", "##  ##", "######", " #### "}},
    {'4', {"   ## ", "  ### ", "  ### ", " #### ", " #### ", "##### ", "## ## ", "######", "######", "   ## ", "   ## ", "   ## "}},
    {'5', {"######", "######", "##    ", "##    ", "##### ", "######", "    ##", "    ##", "    ##", "##  ##", "######", " #### "}},
    {'6', {" #### ", "######", "##  ##", "##    ", "##### ", "######", "##  ##", "##  ##", "##  ##", "##  ##", "######", " #### "}},
    {'7', {"######", "######", "    ##", "   ###", "   ## ", "  ### ", "  ##  ", "  ##  ", " ###  ", " ##   ", " ##   ", " ##   "}},
    {'8', {" #### ", "######", "##  ##", "##  ##", "##  ##", "######", "######", "##  ##", "##  ##", "##  ##", "######", " #### "}},
    {'9', {" #### ", "######", "##  ##", "##  ##", "##  ##", "######", " #####", "    ##", "    ##", "##  ##", "######", " #### "}},
    {':', {"  ", "  ", "  ", "##", "##", "##", "  ", "##", "##", "##", "  ", "  "}},
    {'-', {"      ", "      ", "      ", "      ", "      ", "######", "######", "      ", "      ", "      ", "      ", "      "}},
    {' ', {"   ", "   ", "   ", "   ", "   ", "   ", "   ", "   ", "   ", "   ", "   ", "   "}},
};

const Glyph SMALL7[] = {    // 5 x 7 (space 3 wide)
    {'A', {" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
    {'B', {"#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "}},
    {'C', {" ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "}},
    {'D', {"#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "}},
    {'E', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"}},
    {'F', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "}},
    {'G', {" ### ", "#   #", "#    ", "# ###", "#   #", "#   #", " ####"}},
    {'H', {"#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
    {'I', {" ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}},
    {'J', {"  ###", "   # ", "   # ", "   # ", "   # ", "#  # ", " ##  "}},
    {'K', {"#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"}},
    {'L', {"#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"}},
    {'M', {"#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #"}},
    {'N', {"#   #", "#   #", "##  #", "# # #", "#  ##", "#   #", "#   #"}},
    {'O', {" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
    {'P', {"#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "}},
    {'Q', {" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"}},
    {'R', {"#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"}},
    {'S', {" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "}},
    {'T', {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "}},
    {'U', {"#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
    {'V', {"#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "}},
    {'W', {"#   #", "#   #", "#   #", "# # #", "# # #", "# # #", " # # "}},
    {'X', {"#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"}},
    {'Y', {"#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "}},
    {'Z', {"#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"}},
    {'0', {" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "}},
    {'1', {"  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}},
    {'2', {" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"}},
    {'3', {"#####", "   # ", "  #  ", "   # ", "    #", "#   #", " ### "}},
    {'4', {"   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "}},
    {'5', {"#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "}},
    {'6', {"  ## ", " #   ", "#    ", "#### ", "#   #", "#   #", " ### "}},
    {'7', {"#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "}},
    {'8', {" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "}},
    {'9', {" ### ", "#   #", "#   #", " ####", "    #", "   # ", " ##  "}},
    {'-', {"     ", "     ", "     ", " ### ", "     ", "     ", "     "}},
    {'%', {"##   ", "##  #", "   # ", "  #  ", " #   ", "#  ##", "   ##"}},
    {'.', {"     ", "     ", "     ", "     ", "     ", " ##  ", " ##  "}},
    {' ', {"   ", "   ", "   ", "   ", "   ", "   ", "   "}},
};

struct Font { const Glyph *g; int n, h; };
const Font F_CLOCK = {CLOCK12, sizeof(CLOCK12) / sizeof(CLOCK12[0]), 12};
const Font F_SMALL7 = {SMALL7, sizeof(SMALL7) / sizeof(SMALL7[0]), 7};

const Glyph *find(const Font &f, char c) {
  c = (char)toupper((unsigned char)c);
  for (int i = 0; i < f.n; i++) if (f.g[i].c == c) return &f.g[i];
  return nullptr;
}

inline void put(int x, int y, uint8_t c) {
  if (x >= 0 && x < W && y >= 0 && y < rows) px[y * W + x] = c;
}

int text_w(const Font &f, const char *s) {
  int w = 0;
  for (; *s; s++) { const Glyph *g = find(f, *s); if (g) w += (int)strlen(g->rows[0]) + 1; }
  return w ? w - 1 : 0;
}

// draws s at (x, y); shadow: a copy one pixel down-right first (the clock)
void text(const Font &f, int x, int y, const char *s, uint8_t col, bool shadow = false) {
  for (; *s; s++) {
    const Glyph *g = find(f, *s);
    if (!g) continue;
    int gw = (int)strlen(g->rows[0]);
    for (int r = 0; r < f.h; r++)
      for (int c = 0; c < gw; c++)
        if (g->rows[r][c] == '#') {
          if (shadow) put(x + c + 1, y + r + 1, SHADOW);
          put(x + c, y + r, col);
        }
    x += gw + 1;
  }
}

// gauge: 5 rows, a dark 1 px frame around a 3-row track; the fill has a light top row and a dark bottom row
void hbar(int x, int y, int w, int pct, bool stale, bool known) {
  for (int i = 0; i < w; i++)
    for (int r = 0; r < 5; r++)
      put(x + i, y + r, (r == 0 || r == 4 || i == 0 || i == w - 1) ? TRACK_LO : TRACK);
  if (!known || pct <= 0) return;
  int inner = w - 2;
  int fill = (pct * inner + 50) / 100;
  if (fill < 1) fill = 1;
  if (fill > inner) fill = inner;
  uint8_t hi = stale ? GREY : pct >= 85 ? ERR : pct >= 60 ? WARN : OK;
  uint8_t lo = (uint8_t)(hi + 1);
  for (int i = 0; i < fill; i++) { put(x + 1 + i, y + 1, hi); put(x + 1 + i, y + 2, hi); put(x + 1 + i, y + 3, lo); }
}

void rebuild() {
  if (!px) return;
  memset(px, BG, W * H);
  // left: clock rows 2..13 with its shadow, date rows 19..25 (level with the third gauge's caption)
  if (clk[0]) text(F_CLOCK, 7, 2, clk, TEXT, true);
  if (date[0]) text(F_SMALL7, 7, 19, date, MUTED);
  // right: gauges 52 px wide ending 5 px from the edge, captions right-aligned 4 px before them
  const int BW = 52, bx = W - 5 - BW, cap_r = bx - 4;
  int y0 = n_bars == 3 ? 1 : 4, pitch = n_bars == 3 ? 9 : 11;
  int left = W;
  for (int i = 0; i < n_bars; i++) {
    int y = y0 + i * pitch;
    char cap[8];
    snprintf(cap, sizeof cap, "%.6s", bars[i].caption ? bars[i].caption : "");
    bool known = bars[i].pct >= 0;
    int cw = text_w(F_SMALL7, cap);
    text(F_SMALL7, cap_r - cw, y, cap, known && !bars[i].stale ? MUTED : DIM);
    hbar(bx, y + 1, BW, bars[i].pct, bars[i].stale, known);
    if (cap_r - cw < left) left = cap_r - cw;
  }
  if (n_bars) bars_area = {(lv_coord_t)((left - 4) * ZOOM), 0, (lv_coord_t)(W * ZOOM - 1), (lv_coord_t)(rows * ZOOM - 1)};
  lv_obj_invalidate(obj);
}

void draw_cb(lv_event_t *e) {
  lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
  lv_area_t A;
  lv_obj_get_coords(obj, &A);
  lv_area_t clip;
  if (!px || !_lv_area_intersect(&clip, &A, dc->clip_area)) return;
  lv_color_t *buf = (lv_color_t *)dc->buf;
  int stride = lv_area_get_width(dc->buf_area);
  for (int y = clip.y1; y <= clip.y2; y++) {
    int ly = (y - A.y1) / ZOOM;
    if (ly >= rows) break;
    const uint8_t *src = px + ly * W;
    lv_color_t *dst = buf + (y - dc->buf_area->y1) * stride + (clip.x1 - dc->buf_area->x1);
    int x = clip.x1;
    while (x <= clip.x2) {
      int ox = x - A.x1;
      int run = ZOOM - ox % ZOOM;
      if (x + run - 1 > clip.x2) run = clip.x2 - x + 1;
      lv_color_t v;
      v.full = pal[src[ox / ZOOM]];
      for (int k = 0; k < run; k++) dst[k] = v;
      dst += run; x += run;
    }
  }
}

}  // namespace

lv_obj_t *home_hud_create(lv_obj_t *parent, lv_coord_t y, lv_coord_t h) {
  rows = h / ZOOM;
  if (rows > H) rows = H;
  for (int i = 0; i < N_PAL; i++) pal[i] = lv_color_hex(PAL_RGB[i]).full;
  // PSRAM: unlike the room's canvas this one is touched only when a value changes and on repaints, so it does
  // not compete with the panel's scan-out DMA for internal RAM bandwidth
  px = (uint8_t *)heap_caps_calloc(W * H, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!px) px = (uint8_t *)heap_caps_calloc(W * H, 1, MALLOC_CAP_8BIT);
  obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_set_pos(obj, 0, y);
  lv_obj_set_size(obj, W * ZOOM, rows * ZOOM);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(obj, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
  rebuild();
  return obj;
}

void home_hud_set_clock(const char *hhmm, const char *d) {
  if (!strcmp(clk, hhmm) && !strcmp(date, d)) return;
  snprintf(clk, sizeof clk, "%s", hhmm);
  snprintf(date, sizeof date, "%s", d);
  rebuild();
}

void home_hud_set_limits(const HudBar *b, int n) {
  if (n > 3) n = 3;
  bool same = n == n_bars;
  for (int i = 0; same && i < n; i++)
    same = bars[i].pct == b[i].pct && bars[i].stale == b[i].stale &&
           !strcmp(bars[i].caption ? bars[i].caption : "", b[i].caption ? b[i].caption : "");
  if (same) return;
  n_bars = n;
  for (int i = 0; i < n; i++) bars[i] = b[i];
  rebuild();
}

void home_hud_limits_area(lv_area_t *out) { *out = bars_area; }
