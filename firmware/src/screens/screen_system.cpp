// System monitor tile (owner: system). btop-inspired host CPU / RAM dashboard.
// Data: state.system (schema at the top of host/companion/providers/system.py).
// Widgets are created once; update() only changes values. No timers / continuous animations.
// Perf: update() only touches widgets whose *displayed* value changed (text compare, colour buckets),
// never forces a layout pass (no lv_obj_align_to), and the two charts are rendered by hand into small
// opaque canvases (cheap image blit instead of LVGL's masked gradient area fill).
#include "screens.h"
#include "../core/model.h"
#include "../ui/theme.h"
#include <math.h>
#include <stdarg.h>
#include <esp_heap_caps.h>

#define SYS_HIST_N 60
#define SYS_NET_N 40
#define SYS_MAX_CORES 16
#define SYS_PROCS 6

// ---- widgets -----------------------------------------------------------------------------------
static lv_obj_t *root, *placeholder;
static lv_obj_t *lb_host, *lb_meta;
// cpu card
static lv_obj_t *cpu_card, *lb_cpu, *lb_load, *lb_cores_info, *lb_thermal, *thermal_dot;
static lv_obj_t *lb_scale;
static int cpu_scale = 100;
static int16_t cpu_pts[SYS_HIST_N];
static int n_cores = 0;
static lv_obj_t *core_lb[SYS_MAX_CORES], *core_bar[SYS_MAX_CORES], *core_val[SYS_MAX_CORES];
// mem card
static lv_obj_t *lb_mem_used, *lb_mem_total, *lb_mem_pct, *lb_mem_right, *mem_track;
static lv_obj_t *mem_seg[4], *mem_leg[4];
// procs
static lv_obj_t *pr_dot[SYS_PROCS], *pr_name[SYS_PROCS], *pr_bar[SYS_PROCS], *pr_cpu[SYS_PROCS], *pr_mem[SYS_PROCS];
// net
static lv_obj_t *lb_rx, *lb_tx;
static int16_t rx_pts[SYS_NET_N], tx_pts[SYS_NET_N];
// claude
static lv_obj_t *lb_cl_pct, *lb_cl_sub, *lb_cl_detail;


#define C_MEM_APP   C_ACCENT
#define C_MEM_WIRED C_INFO
#define C_MEM_COMP  C_SUBAGENT
#define C_MEM_CACHE lv_color_hex(0x4A4640)
#define C_RX        C_INFO
#define C_TX        C_IDLE

// ---- helpers -----------------------------------------------------------------------------------
static lv_color_t load_color(int v) {
  // green -> amber -> orange -> red
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  if (v <= 40) return C_IDLE;
  if (v <= 65) return lv_color_mix(C_WAITING, C_IDLE, (uint8_t)((v - 40) * 255 / 25));
  if (v <= 85) return lv_color_mix(C_BUSY, C_WAITING, (uint8_t)((v - 65) * 255 / 20));
  return lv_color_mix(C_ERROR, C_BUSY, (uint8_t)((v - 85) * 255 / 15));
}

// quantised: colours only change in 5% steps, so style updates happen on bucket changes only
static lv_color_t load_color_q(int v) { return load_color(v <= 40 ? v : (v / 5) * 5); }

// ---- change-only setters (each LVGL setter invalidates / refreshes styles even if unchanged) ----
static bool set_txt(lv_obj_t *l, const char *t) {
  const char *o = lv_label_get_text(l);
  if (o && !strcmp(o, t)) return false;
  lv_label_set_text(l, t);
  return true;
}

static bool set_txtf(lv_obj_t *l, const char *fmt, ...) {
  char b[112];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  return set_txt(l, b);
}

static void set_tcolor(lv_obj_t *o, lv_color_t c) {
  if (lv_obj_get_style_text_color(o, 0).full != c.full) lv_obj_set_style_text_color(o, c, 0);
}

static void set_bgcolor(lv_obj_t *o, lv_color_t c, lv_style_selector_t sel = 0) {
  if (lv_obj_get_style_bg_color(o, sel).full != c.full) lv_obj_set_style_bg_color(o, c, sel);
}

static void show(lv_obj_t *o, bool on) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != on) return;
  if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static int txt_w(const lv_font_t *f, const char *t) {
  return lv_txt_get_width(t, strlen(t), f, 0, LV_TEXT_FLAG_NONE);
}

static lv_obj_t *mk_card(lv_obj_t *p, int x, int y, int w, int h) {
  lv_obj_t *c = lv_obj_create(p);
  lv_obj_remove_style_all(c);
  lv_obj_set_pos(c, x, y);
  lv_obj_set_size(c, w, h);
  lv_obj_set_style_bg_color(c, C_SURFACE, 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 14, 0);
  lv_obj_set_style_border_color(c, C_BORDER, 0);
  lv_obj_set_style_border_width(c, 1, 0);
  lv_obj_set_style_border_opa(c, LV_OPA_60, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
  return c;
}

static lv_obj_t *mk_label(lv_obj_t *p, const lv_font_t *f, lv_color_t col, int x, int y, const char *txt) {
  lv_obj_t *l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, col, 0);
  lv_obj_set_pos(l, x, y);
  lv_label_set_text(l, txt);
  return l;
}

// right-aligned fixed-width label (numbers don't jitter)
static lv_obj_t *mk_rlabel(lv_obj_t *p, const lv_font_t *f, lv_color_t col, int x, int y, int w, const char *txt) {
  lv_obj_t *l = mk_label(p, f, col, x, y, txt);
  lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(l, w);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
  return l;
}

static lv_obj_t *mk_rect(lv_obj_t *p, int x, int y, int w, int h, lv_color_t c, int radius) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, c, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

static lv_obj_t *mk_bar(lv_obj_t *p, int x, int y, int w, int h) {
  lv_obj_t *b = lv_bar_create(p);
  lv_obj_remove_style_all(b);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_bar_set_range(b, 0, 100);
  lv_obj_set_style_bg_color(b, C_SURFACE2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(b, h / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(b, C_IDLE, LV_PART_INDICATOR);
  lv_obj_set_style_radius(b, h / 2, LV_PART_INDICATOR);
  lv_bar_set_value(b, 0, LV_ANIM_OFF);
  return b;
}

// solid bucketed colour, no animation (a 500 ms anim on 22 bars = ~30 extra frames every second)
static void set_bar(lv_obj_t *b, int v) {
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  set_bgcolor(b, load_color_q(v), LV_PART_INDICATOR);
  if (lv_bar_get_value(b) != v) lv_bar_set_value(b, v, LV_ANIM_OFF);
}

static void fmt_mb(char *out, size_t n, long mb) {
  if (mb >= 1024) snprintf(out, n, "%.1fG", mb / 1024.0);
  else snprintf(out, n, "%ldM", mb);
}

static void fmt_rate(char *out, size_t n, long kbps) {
  if (kbps >= 1000 * 1024) snprintf(out, n, "%.1f GB/s", kbps / 1048576.0);
  else if (kbps >= 1000) snprintf(out, n, "%.1f MB/s", kbps / 1024.0);
  else snprintf(out, n, "%ld KB/s", kbps);
}

static void fmt_uptime(char *out, size_t n, long s) {
  long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
  if (d > 0) snprintf(out, n, "up %ldd %ldh", d, h);
  else if (h > 0) snprintf(out, n, "up %ldh %ldm", h, m);
  else snprintf(out, n, "up %ldm", m);
}

// ---- canvas line charts --------------------------------------------------------------------------
// Rendered by hand into an opaque RGB565 canvas in PSRAM: background + grid + faded area + 2-px
// anti-aliased line. Costs ~1-2 ms for the 310x74 CPU chart and LVGL just blits it (no masks).
#define SYS_PT_NONE INT16_MIN
#define SYS_PLOT_MAXW 320
#define SYS_Y_NONE INT16_MIN

struct SysPlot {
  lv_obj_t *cv = nullptr;
  lv_color_t *buf = nullptr;
  int w = 0, h = 0, lw = 2;
  bool grid = false;
  uint32_t sig = 0;
};
struct SysSeries { const int16_t *v; int n; lv_color_t col; lv_opa_t fill; };

static SysPlot cpu_plot, net_plot;

static void plot_create(SysPlot &p, lv_obj_t *parent, int x, int y, int w, int h, int lw, bool grid) {
  p.w = w; p.h = h; p.lw = lw; p.grid = grid;
  p.buf = (lv_color_t *)heap_caps_malloc((size_t)w * h * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  if (!p.buf) return;
  for (int i = 0; i < w * h; i++) p.buf[i] = C_SURFACE;
  p.cv = lv_canvas_create(parent);
  lv_canvas_set_buffer(p.cv, p.buf, w, h, LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(p.cv, x, y);
  lv_obj_clear_flag(p.cv, LV_OBJ_FLAG_CLICKABLE);
}

static uint32_t fnv1a(const void *d, size_t n, uint32_t h = 2166136261u) {
  const uint8_t *s = (const uint8_t *)d;
  for (size_t i = 0; i < n; i++) h = (h ^ s[i]) * 16777619u;
  return h;
}

static void plot_render(SysPlot &p, const SysSeries *ser, int ns, int vmax) {
  if (!p.buf || vmax <= 0) return;
  // skip if nothing visible changed
  uint32_t sig = fnv1a(&vmax, sizeof vmax);
  for (int k = 0; k < ns; k++) {
    sig = fnv1a(ser[k].v, ser[k].n * sizeof(int16_t), sig);
    sig = fnv1a(&ser[k].col, sizeof(lv_color_t), sig);
  }
  if (sig == p.sig) return;
  p.sig = sig;

  const int W = p.w, H = p.h;
  if (W > SYS_PLOT_MAXW || H > 255) return;
  // per-column scratch, internal RAM (~1.9 KB): y in 8.8 fixed px (SYS_Y_NONE = no data), first filled row
  static int16_t ys[2][SYS_PLOT_MAXW];
  static uint8_t y0s[2][SYS_PLOT_MAXW];
  const int32_t top = (p.lw * 256) / 2, span = (H - 1) * 256 - top;
  for (int k = 0; k < 2; k++) {
    if (k >= ns) { memset(y0s[k], H, W); continue; }
    const SysSeries &s = ser[k];
    for (int x = 0; x < W; x++) {
      int32_t pos = s.n > 1 ? (int32_t)x * (s.n - 1) * 256 / (W - 1) : 0;
      int i = pos >> 8, f = pos & 255;
      int a = s.v[i], b = (i + 1 < s.n) ? s.v[i + 1] : a;
      y0s[k][x] = H;
      if (a == SYS_PT_NONE || (f && b == SYS_PT_NONE)) { ys[k][x] = SYS_Y_NONE; continue; }
      if (b == SYS_PT_NONE) b = a;
      int32_t v = a * 256 + (b - a) * f;
      if (v < 0) v = 0;
      if (v > vmax * 256) v = vmax * 256;
      ys[k][x] = (int16_t)(top + span - v * (span >> 4) / (vmax * 16));  // H < 128 -> fits int32
      if (s.fill) y0s[k][x] = (uint8_t)(ys[k][x] >> 8);
    }
  }
  // background + grid + faded area in one row-major pass. The fill alpha depends on the row only
  // (s.fill at the top of the plot -> 0 at the bottom), so each row needs just 4 precomputed
  // colours (none / series 0 / series 1 / both) and the per-pixel work is a table lookup.
  lv_color_t bg = C_SURFACE, gc = lv_color_mix(C_BORDER, C_SURFACE, LV_OPA_50);
  lv_opa_t f0 = ns > 0 ? ser[0].fill : 0, f1 = ns > 1 ? ser[1].fill : 0;
  lv_color_t c0 = ns > 0 ? ser[0].col : bg, c1 = ns > 1 ? ser[1].col : bg;
  for (int r = 0; r < H; r++) {
    bool g = p.grid && (r == H / 4 || r == H / 2 || r == H * 3 / 4);
    lv_color_t t[4];
    t[0] = g ? gc : bg;
    uint8_t a0 = (uint8_t)(f0 * (H - r) / H), a1 = (uint8_t)(f1 * (H - r) / H);
    t[1] = lv_color_mix(c0, t[0], a0);
    t[2] = lv_color_mix(c1, t[0], a1);
    t[3] = lv_color_mix(c1, t[1], a1);
    lv_color_t *row = p.buf + r * W;
    const uint8_t *ya = y0s[0], *yb = y0s[1];
    int x = 0;
    if (((uintptr_t)row & 3) == 0) {  // two pixels per 32-bit store
      uint32_t *row32 = (uint32_t *)row;
      for (; x + 1 < W; x += 2) {
        uint32_t lo = t[(r >= ya[x]) | ((r >= yb[x]) << 1)].full;
        uint32_t hi = t[(r >= ya[x + 1]) | ((r >= yb[x + 1]) << 1)].full;
        row32[x >> 1] = lo | (hi << 16);
      }
    }
    for (; x < W; x++) row[x] = t[(r >= ya[x]) | ((r >= yb[x]) << 1)];
  }
  // line: column x spans between the midpoints to its neighbours, thickened by lw, AA ends
  const int32_t half = p.lw * 128;
  for (int k = 0; k < ns && k < 2; k++) {
    const int16_t *y = ys[k];
    for (int x = 0; x < W; x++) {
      if (y[x] == SYS_Y_NONE) continue;
      int32_t yl = (x > 0 && y[x - 1] != SYS_Y_NONE) ? (y[x - 1] + y[x]) / 2 : y[x];
      int32_t yr = (x + 1 < W && y[x + 1] != SYS_Y_NONE) ? (y[x + 1] + y[x]) / 2 : y[x];
      int32_t lo = LV_MIN(LV_MIN(yl, yr), y[x]) - half, hi = LV_MAX(LV_MAX(yl, yr), y[x]) + half;
      int r0 = LV_MAX(0, lo >> 8), r1 = LV_MIN(H - 1, hi >> 8);
      for (int r = r0; r <= r1; r++) {
        int32_t cov = LV_MIN(hi, (r + 1) * 256) - LV_MAX(lo, r * 256);
        if (cov <= 0) continue;
        lv_color_t *px = &p.buf[r * W + x];
        *px = cov >= 255 ? ser[k].col : lv_color_mix(ser[k].col, *px, (uint8_t)cov);
      }
    }
  }
  lv_obj_invalidate(p.cv);
}

static void place_cl_sub();

// ---- create ------------------------------------------------------------------------------------
static void create_cores(int n) {
  if (n > SYS_MAX_CORES) n = SYS_MAX_CORES;
  for (int i = 0; i < n_cores; i++) {
    lv_obj_del(core_lb[i]);
    lv_obj_del(core_bar[i]);
    lv_obj_del(core_val[i]);
  }
  n_cores = n;
  if (n <= 0) return;
  int rows = (n + 1) / 2;
  int top = 114, avail = 52;
  int pitch = rows <= 4 ? 14 : avail / rows + 1;
  const lv_font_t *f = pitch >= 13 ? &lv_font_montserrat_12 : &lv_font_montserrat_10;
  int bh = pitch >= 13 ? 8 : 5;
  int colw = 210;
  for (int i = 0; i < n; i++) {
    int col = i / rows, row = i % rows;
    int x = 12 + col * (colw + 12), y = top + row * pitch;
    core_lb[i] = mk_label(cpu_card, f, C_MUTED, x, y, "");
    core_bar[i] = mk_bar(cpu_card, x + 24, y + (pitch >= 13 ? 4 : 3), colw - 24 - 34, bh);
    core_val[i] = mk_rlabel(cpu_card, f, C_MUTED, x + colw - 32, y, 32, "");
  }
}

static void create(lv_obj_t *p) {
  placeholder = mk_label(p, F_BODY, C_MUTED, 0, 0, LV_SYMBOL_REFRESH "  waiting for host metrics...");
  lv_obj_center(placeholder);

  root = lv_obj_create(p);
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, 480, 480);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);

  // header
  lv_obj_t *logo = mk_label(root, F_SMALL, C_ACCENT, 20, 11, LV_SYMBOL_SETTINGS);
  (void)logo;
  lb_host = mk_label(root, F_SMALL, C_TEXT, 40, 11, "");
  lb_meta = mk_rlabel(root, F_SMALL, C_MUTED, 200, 11, 260, "");
  lv_label_set_recolor(lb_meta, true);

  // CPU card
  cpu_card = mk_card(root, 12, 34, 456, 174);
  mk_label(cpu_card, &lv_font_montserrat_12, C_MUTED, 14, 10, "CPU");
  lb_load = mk_rlabel(cpu_card, &lv_font_montserrat_12, C_MUTED, 200, 10, 242, "");
  lb_cpu = mk_label(cpu_card, F_HUGE, C_IDLE, 12, 28, "--");
  lv_label_set_long_mode(lb_cpu, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(lb_cpu, 118);
  lb_cores_info = mk_label(cpu_card, &lv_font_montserrat_12, C_MUTED, 14, 80, "");
  thermal_dot = mk_rect(cpu_card, 15, 101, 7, 7, C_IDLE, 4);
  lb_thermal = mk_label(cpu_card, &lv_font_montserrat_12, C_MUTED, 27, 96, "");

  // chart (grid lines 25/50/75% drawn inside the canvas) + baseline
  int cx = 132, cy = 30, cw = 310, ch = 74;
  lv_obj_t *base = mk_rect(cpu_card, cx, cy + ch, cw, 1, C_BORDER, 0);
  (void)base;
  plot_create(cpu_plot, cpu_card, cx, cy, cw, ch, 2, true);
  for (auto &v : cpu_pts) v = SYS_PT_NONE;
  lb_scale = mk_label(cpu_card, &lv_font_montserrat_10, C_DIM, cx + 2, cy - 1, "100%");
  lv_obj_t *l60 = mk_label(cpu_card, &lv_font_montserrat_10, C_DIM, cx, cy + ch + 1, "-60s");
  (void)l60;
  lv_obj_t *lnow = mk_rlabel(cpu_card, &lv_font_montserrat_10, C_DIM, cx + cw - 40, cy + ch + 1, 40, "now");
  (void)lnow;

  // MEM card
  lv_obj_t *mc = mk_card(root, 12, 216, 456, 92);
  mk_label(mc, &lv_font_montserrat_12, C_MUTED, 14, 10, "MEMORY");
  lb_mem_right = mk_rlabel(mc, &lv_font_montserrat_12, C_MUTED, 180, 10, 262, "");
  lb_mem_used = mk_label(mc, F_TITLE, C_TEXT, 14, 26, "--");
  lb_mem_total = mk_label(mc, F_SMALL, C_MUTED, 80, 34, "");
  lb_mem_pct = mk_label(mc, F_SMALL, C_MUTED, 140, 34, "");
  static const lv_color_t seg_cols[4] = {C_MEM_APP, C_MEM_WIRED, C_MEM_COMP, C_MEM_CACHE};
  mem_track = mk_rect(mc, 14, 56, 428, 10, C_SURFACE2, 5);
  lv_obj_set_style_clip_corner(mem_track, true, 0);
  for (int i = 0; i < 4; i++) {
    mem_seg[i] = mk_rect(mem_track, 0, 0, 0, 10, seg_cols[i], 0);
    if (i == 3) {
      lv_obj_set_style_bg_opa(mem_seg[i], LV_OPA_COVER, 0);
    }
  }
  // legend (right of the big number)
  for (int i = 0; i < 4; i++) {
    lv_obj_t *dot = mk_rect(mc, 0, 0, 8, 8, seg_cols[i], 2);
    mem_leg[i] = mk_label(mc, &lv_font_montserrat_12, C_MUTED, 0, 0, "");
    lv_obj_set_user_data(mem_leg[i], dot);
  }

  // processes card
  lv_obj_t *pc = mk_card(root, 12, 316, 284, 140);
  mk_label(pc, &lv_font_montserrat_12, C_MUTED, 14, 9, "TOP PROCESSES");
  mk_rlabel(pc, &lv_font_montserrat_12, C_DIM, 178, 9, 44, "CPU");
  mk_rlabel(pc, &lv_font_montserrat_12, C_DIM, 224, 9, 46, "MEM");
  for (int i = 0; i < SYS_PROCS; i++) {
    int y = 27 + i * 18;
    pr_dot[i] = mk_rect(pc, 14, y + 6, 6, 6, C_ACCENT, 3);
    pr_name[i] = mk_label(pc, F_SMALL, C_TEXT, 26, y, "");
    lv_label_set_long_mode(pr_name[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(pr_name[i], 104);
    pr_bar[i] = mk_bar(pc, 134, y + 7, 44, 5);
    pr_cpu[i] = mk_rlabel(pc, F_SMALL, C_TEXT, 178, y, 44, "");
    pr_mem[i] = mk_rlabel(pc, F_SMALL, C_MUTED, 224, y, 46, "");
  }

  // network card
  lv_obj_t *nc = mk_card(root, 304, 316, 164, 66);
  mk_label(nc, &lv_font_montserrat_12, C_MUTED, 12, 8, "NETWORK");
  lb_rx = mk_label(nc, &lv_font_montserrat_12, C_RX, 12, 26, "");
  lb_tx = mk_label(nc, &lv_font_montserrat_12, C_TX, 12, 44, "");
  plot_create(net_plot, nc, 92, 10, 62, 48, 1, false);
  for (int i = 0; i < SYS_NET_N; i++) rx_pts[i] = tx_pts[i] = SYS_PT_NONE;

  // Claude footprint card
  lv_obj_t *cc = mk_card(root, 304, 390, 164, 66);
  lv_obj_set_style_border_color(cc, C_ACCENT, 0);
  lv_obj_set_style_border_opa(cc, LV_OPA_40, 0);
  lv_obj_set_style_bg_grad_color(cc, lv_color_hex(0x33261F), 0);
  lv_obj_set_style_bg_grad_dir(cc, LV_GRAD_DIR_VER, 0);
  mk_label(cc, &lv_font_montserrat_12, C_ACCENT, 12, 8, "CLAUDE CODE");
  lb_cl_pct = mk_label(cc, F_TITLE, C_ACCENT2, 12, 24, "--");
  lb_cl_sub = mk_label(cc, &lv_font_montserrat_12, C_MUTED, 0, 33, "cpu");
  lb_cl_detail = mk_label(cc, &lv_font_montserrat_12, C_MUTED, 12, 50, "");
  place_cl_sub();
  lv_label_set_long_mode(lb_cl_detail, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(lb_cl_detail, 146);
}

// ---- update ------------------------------------------------------------------------------------
static void update_cpu(JsonObjectConst sy) {
  int cpu = sy["cpu"] | 0;
  set_txtf(lb_cpu, "%d%%", cpu);
  set_tcolor(lb_cpu, load_color_q(cpu));

  JsonArrayConst load = sy["load"].as<JsonArrayConst>();
  if (load.size() >= 3)
    set_txtf(lb_load, "load  %.1f  %.1f  %.1f", (double)(load[0] | 0.0f), (double)(load[1] | 0.0f),
             (double)(load[2] | 0.0f));
  else
    set_txt(lb_load, "");

  // history (right-aligned)
  JsonArrayConst hist = sy["hist"].as<JsonArrayConst>();
  int n = hist.size();
  if (n > SYS_HIST_N) n = SYS_HIST_N;
  int skip = (int)hist.size() - n, off = SYS_HIST_N - n, i = 0;
  for (int k = 0; k < off; k++) cpu_pts[k] = SYS_PT_NONE;
  for (JsonVariantConst v : hist) {
    if (i++ < skip) continue;
    int x = v | 0;
    cpu_pts[off++] = x < 0 ? 0 : (x > 100 ? 100 : x);
  }
  // auto-scale so an idle machine still shows texture (25 / 50 / 100 %)
  int mx = 0;
  for (int k = 0; k < SYS_HIST_N; k++)
    if (cpu_pts[k] != SYS_PT_NONE && cpu_pts[k] > mx) mx = cpu_pts[k];
  int scale = mx <= 22 ? 25 : mx <= 45 ? 50 : 100;
  if (scale != cpu_scale) {
    cpu_scale = scale;
    set_txtf(lb_scale, "%d%%", scale);
  }
  SysSeries cs = {cpu_pts, SYS_HIST_N, lv_color_mix(load_color_q(cpu), C_ACCENT, 110), LV_OPA_70};
  plot_render(cpu_plot, &cs, 1, cpu_scale);

  // cores
  JsonArrayConst cores = sy["cores"].as<JsonArrayConst>();
  int nc = cores.size();
  if (nc > SYS_MAX_CORES) nc = SYS_MAX_CORES;
  if (nc != n_cores) create_cores(nc);
  const char *kinds = sy["kinds"] | "";
  bool use_kinds = (int)strlen(kinds) >= nc && nc > 0;
  int ne = 0, np = 0, ie = 0, ip = 0;
  if (use_kinds)
    for (int k = 0; k < nc; k++) (kinds[k] == 'E' ? ne : np)++;
  for (int k = 0; k < n_cores; k++) {
    int v = cores[k] | 0;
    if (use_kinds) {
      bool e = kinds[k] == 'E';
      set_txtf(core_lb[k], "%c%d", e ? 'E' : 'P', e ? ++ie : ++ip);
      set_tcolor(core_lb[k], e ? C_DIM : C_MUTED);
    } else {
      set_txtf(core_lb[k], "%d", k + 1);
    }
    set_bar(core_bar[k], v);
    set_txtf(core_val[k], "%d%%", v);
    set_tcolor(core_val[k], v >= 85 ? load_color_q(v) : C_MUTED);
  }
  if (use_kinds && ne && np)
    set_txtf(lb_cores_info, "%dP + %dE cores", np, ne);
  else
    set_txtf(lb_cores_info, "%d cores", (int)(sy["ncpu"] | nc));

  const char *th = sy["thermal"] | "";
  if (*th) {
    lv_color_t c = !strcmp(th, "nominal") ? C_IDLE
                   : !strcmp(th, "fair")  ? C_WAITING
                   : !strcmp(th, "serious") ? C_BUSY
                                            : C_ERROR;
    set_txtf(lb_thermal, "thermal %s", th);
    set_bgcolor(thermal_dot, c);
    show(thermal_dot, true);
  } else {
    set_txt(lb_thermal, "");
    show(thermal_dot, false);
  }
}

static void update_mem(JsonObjectConst sy) {
  JsonObjectConst m = sy["mem"];
  long total = m["total"] | 0L, used = m["used"] | 0L;
  char a[16], b[16];
  if (total <= 0) {
    set_txt(lb_mem_used, "--");
    set_txt(lb_mem_total, "");
    set_txt(lb_mem_pct, "");
    return;
  }
  int pct = (int)(used * 100 / total);
  snprintf(a, sizeof a, "%.1f", used / 1024.0);
  bool moved = set_txt(lb_mem_used, a);
  snprintf(a, sizeof a, "/ %.0f GB", total / 1024.0);
  moved |= set_txt(lb_mem_total, a);
  moved |= set_txtf(lb_mem_pct, "%d%%", pct);
  set_tcolor(lb_mem_pct, load_color_q(pct));
  if (moved) {  // manual "align right of" (lv_obj_align_to forces a full-screen layout pass)
    int lh_big = lv_font_get_line_height(F_TITLE), lh = lv_font_get_line_height(F_SMALL);
    int x = 14 + txt_w(F_TITLE, lv_label_get_text(lb_mem_used)) + 6, y = 26 + lh_big - lh - 3;
    lv_obj_set_pos(lb_mem_total, x, y);
    x += txt_w(F_SMALL, lv_label_get_text(lb_mem_total)) + 6;
    lv_obj_set_pos(lb_mem_pct, x, y);
  }

  long seg[4] = {m["app"] | -1L, m["wired"] | -1L, m["comp"] | -1L, m["cache"] | -1L};
  static const char *names[4] = {"app", "wired", "comp", "cache"};
  bool have = seg[0] >= 0;
  if (!have) {
    seg[0] = used;
    seg[1] = seg[2] = seg[3] = 0;
  }
  int x = 0, W = 428;
  for (int i = 0; i < 4; i++) {
    long v = seg[i] < 0 ? 0 : seg[i];
    int w = (int)(v * W / total);
    if (x + w > W) w = W - x;
    lv_obj_set_x(mem_seg[i], x);      // both are no-ops when unchanged
    lv_obj_set_width(mem_seg[i], w);
    x += w;
  }
  // legend under the bar, one quarter each
  for (int i = 0; i < 4; i++) {
    lv_obj_t *dot = (lv_obj_t *)lv_obj_get_user_data(mem_leg[i]);
    bool on = have && seg[i] >= 0;
    show(mem_leg[i], on);
    show(dot, on);
    if (!on) continue;
    fmt_mb(a, sizeof a, seg[i]);
    set_txtf(mem_leg[i], "%s %s", names[i], a);
    int lx = 14 + i * 108;
    lv_obj_set_pos(dot, lx, 74);
    lv_obj_set_pos(mem_leg[i], lx + 12, 70);
  }

  long sw = m["swap"] | 0L;
  int pres = m["pressure"] | -1;
  const char *lvl = m["level"] | "normal";
  fmt_mb(b, sizeof b, sw);
  char pr[24] = "";
  if (pres >= 0) snprintf(pr, sizeof pr, "pressure %d%%   ", pres);
  set_txtf(lb_mem_right, "%sswap %s", pr, sw > 0 ? b : "0");
  set_tcolor(lb_mem_right, !strcmp(lvl, "critical") ? C_ERROR : !strcmp(lvl, "warn") ? C_WAITING : C_MUTED);
}

static void update_procs(JsonObjectConst sy) {
  JsonArrayConst procs = sy["procs"].as<JsonArrayConst>();
  for (int i = 0; i < SYS_PROCS; i++) {
    JsonObjectConst p = procs[i];
    bool on = !p.isNull();
    bool cl = on && (p["cl"] | 0) != 0;
    show(pr_dot[i], cl);
    show(pr_name[i], on);
    show(pr_bar[i], on);
    show(pr_cpu[i], on);
    show(pr_mem[i], on);
    if (!on) continue;
    float c = p["c"] | 0.0f;
    long mb = p["m"] | 0L;
    char mbs[16];
    fmt_mb(mbs, sizeof mbs, mb);
    set_txt(pr_name[i], p["n"] | "?");
    set_tcolor(pr_name[i], cl ? C_ACCENT2 : C_TEXT);
    if (c >= 10) set_txtf(pr_cpu[i], "%d%%", (int)lroundf(c));
    else set_txtf(pr_cpu[i], "%.1f%%", (double)c);
    set_tcolor(pr_cpu[i], c >= 85 ? load_color_q((int)c) : C_TEXT);
    set_txt(pr_mem[i], mbs);
    set_bar(pr_bar[i], (int)c);
  }
}

static void update_net(JsonObjectConst sy) {
  JsonObjectConst n = sy["net"];
  char a[20];
  if (n.isNull()) {
    set_txt(lb_rx, LV_SYMBOL_DOWN " --");
    set_txt(lb_tx, LV_SYMBOL_UP " --");
    return;
  }
  fmt_rate(a, sizeof a, n["rx"] | 0L);
  set_txtf(lb_rx, LV_SYMBOL_DOWN " %s", a);
  fmt_rate(a, sizeof a, n["tx"] | 0L);
  set_txtf(lb_tx, LV_SYMBOL_UP " %s", a);
  JsonArrayConst rh = n["rx_hist"].as<JsonArrayConst>(), th = n["tx_hist"].as<JsonArrayConst>();
  long mx = 64;  // KB/s floor so idle links don't look busy
  for (JsonVariantConst v : rh) mx = max(mx, (long)(v | 0L));
  for (JsonVariantConst v : th) mx = max(mx, (long)(v | 0L));
  struct { JsonArrayConst a; int16_t *pts; } s[2] = {{rh, rx_pts}, {th, tx_pts}};
  for (auto &e : s) {
    int cnt = e.a.size();
    if (cnt > SYS_NET_N) cnt = SYS_NET_N;
    int skip = (int)e.a.size() - cnt, off = SYS_NET_N - cnt, i = 0;
    for (int k = 0; k < off; k++) e.pts[k] = SYS_PT_NONE;
    for (JsonVariantConst v : e.a) {
      if (i++ < skip) continue;
      e.pts[off++] = (int16_t)((v | 0L) * 100 / mx);
    }
  }
  SysSeries ns[2] = {{rx_pts, SYS_NET_N, C_RX, LV_OPA_50}, {tx_pts, SYS_NET_N, C_TX, LV_OPA_50}};
  plot_render(net_plot, ns, 2, 100);
}

static void place_cl_sub() {  // manual "align right-bottom of lb_cl_pct"
  int x = 12 + txt_w(F_TITLE, lv_label_get_text(lb_cl_pct)) + 5;
  int y = 24 + lv_font_get_line_height(F_TITLE) - lv_font_get_line_height(&lv_font_montserrat_12) - 3;
  lv_obj_set_pos(lb_cl_sub, x, y);
}

static void update_claude(JsonObjectConst sy) {
  JsonObjectConst c = sy["claude"];
  if (c.isNull()) {
    if (set_txt(lb_cl_pct, "--")) place_cl_sub();
    set_txt(lb_cl_detail, "no data");
    return;
  }
  int ncpu = sy["ncpu"] | 1;
  float cpu = c["cpu"] | 0.0f;
  int share = (int)lroundf(cpu / (ncpu > 0 ? ncpu : 1));
  if (share > 100) share = 100;
  if (set_txtf(lb_cl_pct, "%d%%", share)) place_cl_sub();
  char mb[16];
  fmt_mb(mb, sizeof mb, c["mem"] | 0L);
  int ns = c["n"] | 0, kids = c["kids"] | 0;
  set_txtf(lb_cl_detail, "%s  %d sess  %d procs", mb, ns, kids);
  set_txt(lb_cl_sub, "of cpu");
}

static void update_header(JsonObjectConst sy) {
  set_txt(lb_host, sy["host"] | "this computer");
  char buf[96], up[24] = "";
  long upt = sy["uptime"] | 0L;
  if (upt > 0) fmt_uptime(up, sizeof up, upt);
  char disk[32] = "";
  JsonObjectConst d = sy["disk"];
  if (!d.isNull()) {
    int dp = d["pct"] | 0;
    snprintf(disk, sizeof disk, dp >= 90 ? "   #E5534B " LV_SYMBOL_DRIVE " %d%%#" : "   " LV_SYMBOL_DRIVE " %d%%", dp);
  }
  char bat[40] = "";
  JsonObjectConst b = sy["batt"];
  if (!b.isNull()) {
    int pct = b["pct"] | 0;
    const char *sym = pct > 85 ? LV_SYMBOL_BATTERY_FULL : pct > 60 ? LV_SYMBOL_BATTERY_3 : pct > 35 ? LV_SYMBOL_BATTERY_2
                    : pct > 12 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    bool plug = (b["plug"] | 0) != 0;
    snprintf(bat, sizeof bat, (!plug && pct <= 20) ? "   #E5534B %s%s %d%%#" : "   %s%s %d%%",
             plug ? LV_SYMBOL_CHARGE " " : "", sym, pct);
  }
  snprintf(buf, sizeof buf, "%s%s%s", up, disk, bat);
  set_txt(lb_meta, buf);
}

// FNV-1a over the JSON serialisation, without a buffer (ArduinoJson custom writer)
struct SysHashWriter {
  uint32_t h = 2166136261u;
  size_t write(uint8_t c) { h = (h ^ c) * 16777619u; return 1; }
  size_t write(const uint8_t *s, size_t n) { for (size_t i = 0; i < n; i++) h = (h ^ s[i]) * 16777619u; return n; }
};

static uint32_t sec_hash(JsonObjectConst sy, const char *const *keys, int nk) {
  SysHashWriter w;
  for (int i = 0; i < nk; i++) {
    w.write((uint8_t)i);
    serializeJson(sy[keys[i]], w);
  }
  return w.h ? w.h : 1;
}

enum { SEC_HDR, SEC_CPU, SEC_MEM, SEC_PROCS, SEC_NET, SEC_CL, SEC__N };
static uint32_t sec_h[SEC__N];  // 0 = force update

static void update(JsonObjectConst s) {
  JsonObjectConst sy = s["system"];
  if (sy.isNull()) {
    show(root, false);
    show(placeholder, true);
    memset(sec_h, 0, sizeof sec_h);
    return;
  }
  // Skip states where the host did not re-send / change the system part (perf-link part versions).
  static uint32_t seen_ver = 0;
  uint32_t ver = model_part_version("system");
  if (ver && ver == seen_ver && sec_h[0]) return;
  seen_ver = ver;
  // State arrives up to 4x/s (sessions churn); system metrics change at 1 Hz and the process table
  // every 4 s -> hash each section and only rebind the ones whose data changed.
  static const char *const k_hdr[] = {"host", "uptime", "disk", "batt"};
  static const char *const k_cpu[] = {"cpu", "load", "hist", "cores", "kinds", "thermal", "ncpu"};
  static const char *const k_mem[] = {"mem"};
  static const char *const k_procs[] = {"procs"};
  static const char *const k_net[] = {"net"};
  static const char *const k_cl[] = {"claude", "ncpu"};
  static const struct { const char *const *k; int n; void (*fn)(JsonObjectConst); } secs[SEC__N] = {
      {k_hdr, 4, update_header}, {k_cpu, 7, update_cpu},  {k_mem, 1, update_mem},
      {k_procs, 1, update_procs}, {k_net, 1, update_net}, {k_cl, 2, update_claude}};
  show(placeholder, false);
  show(root, true);
  for (int i = 0; i < SEC__N; i++) {
    uint32_t h = sec_hash(sy, secs[i].k, secs[i].n);
    if (h == sec_h[i]) continue;
    sec_h[i] = h;
    secs[i].fn(sy);
  }
}

static void on_show(bool visible) {
  if (visible) memset(sec_h, 0, sizeof sec_h);  // force a full rebind when we come back
}

const ScreenDef screen_system = {"system", "System", create, update, nullptr, on_show};
