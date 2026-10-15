// System monitor tile (owner: system). btop-inspired host CPU / RAM dashboard.
// Data: state.system (schema at the top of host/companion/providers/system.py).
// Widgets are created once; update() only changes values. No timers / continuous animations.
#include "screens.h"
#include "../ui/theme.h"
#include <math.h>

#define SYS_HIST_N 60
#define SYS_NET_N 40
#define SYS_MAX_CORES 16
#define SYS_PROCS 6

// ---- widgets -----------------------------------------------------------------------------------
static lv_obj_t *root, *placeholder;
static lv_obj_t *lb_host, *lb_meta;
// cpu card
static lv_obj_t *cpu_card, *lb_cpu, *lb_load, *lb_cores_info, *lb_thermal, *thermal_dot;
static lv_obj_t *cpu_chart, *lb_scale;
static int cpu_scale = 100;
static lv_chart_series_t *cpu_ser;
static lv_coord_t cpu_pts[SYS_HIST_N];
static int n_cores = 0;
static lv_obj_t *core_lb[SYS_MAX_CORES], *core_bar[SYS_MAX_CORES], *core_val[SYS_MAX_CORES];
// mem card
static lv_obj_t *lb_mem_used, *lb_mem_total, *lb_mem_pct, *lb_mem_right, *mem_track;
static lv_obj_t *mem_seg[4], *mem_leg[4];
// procs
static lv_obj_t *pr_dot[SYS_PROCS], *pr_name[SYS_PROCS], *pr_bar[SYS_PROCS], *pr_cpu[SYS_PROCS], *pr_mem[SYS_PROCS];
// net
static lv_obj_t *lb_rx, *lb_tx, *net_chart;
static lv_chart_series_t *rx_ser, *tx_ser;
static lv_coord_t rx_pts[SYS_NET_N], tx_pts[SYS_NET_N];
// claude
static lv_obj_t *lb_cl_pct, *lb_cl_sub, *lb_cl_detail;

static uint32_t last_hash = 0;

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
  lv_obj_set_style_radius(b, h / 2, LV_PART_INDICATOR);
  lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
  lv_obj_set_style_anim_time(b, 500, LV_PART_MAIN);
  return b;
}

static void set_bar(lv_obj_t *b, int v) {
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  lv_obj_set_style_bg_color(b, C_IDLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_grad_color(b, load_color(v), LV_PART_INDICATOR);
  lv_bar_set_value(b, v, LV_ANIM_ON);
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

// Faded area under chart lines (LVGL 8 "faded area" pattern).
static void chart_area_cb(lv_event_t *e) {
  lv_obj_t *obj = lv_event_get_target(e);
  lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
  if (dsc->part != LV_PART_ITEMS || !dsc->p1 || !dsc->p2 || !dsc->line_dsc) return;
  lv_opa_t top_opa = (lv_opa_t)(uintptr_t)lv_event_get_user_data(e);
  lv_draw_mask_line_param_t line_mask;
  lv_draw_mask_line_points_init(&line_mask, dsc->p1->x, dsc->p1->y, dsc->p2->x, dsc->p2->y,
                                LV_DRAW_MASK_LINE_SIDE_BOTTOM);
  int16_t line_id = lv_draw_mask_add(&line_mask, NULL);
  lv_draw_mask_fade_param_t fade_mask;
  lv_coord_t ytop = LV_MIN(dsc->p1->y, dsc->p2->y);
  lv_draw_mask_fade_init(&fade_mask, &obj->coords, top_opa, ytop, LV_OPA_TRANSP, obj->coords.y2);
  int16_t fade_id = lv_draw_mask_add(&fade_mask, NULL);
  lv_draw_rect_dsc_t rd;
  lv_draw_rect_dsc_init(&rd);
  rd.bg_opa = LV_OPA_COVER;
  rd.bg_color = dsc->line_dsc->color;
  lv_area_t a;
  a.x1 = dsc->p1->x;
  a.x2 = dsc->p2->x - 1;
  a.y1 = LV_MIN(dsc->p1->y, dsc->p2->y);
  a.y2 = obj->coords.y2;
  lv_draw_rect(dsc->draw_ctx, &rd, &a);
  lv_draw_mask_free_param(&line_mask);
  lv_draw_mask_free_param(&fade_mask);
  lv_draw_mask_remove_id(line_id);
  lv_draw_mask_remove_id(fade_id);
}

static lv_obj_t *mk_chart(lv_obj_t *p, int x, int y, int w, int h, int points, lv_opa_t fill_opa) {
  lv_obj_t *c = lv_chart_create(p);
  lv_obj_remove_style_all(c);
  lv_obj_set_pos(c, x, y);
  lv_obj_set_size(c, w, h);
  lv_chart_set_type(c, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(c, points);
  lv_chart_set_update_mode(c, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_range(c, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
  lv_chart_set_div_line_count(c, 0, 0);
  lv_obj_set_style_line_width(c, 2, LV_PART_ITEMS);
  lv_obj_set_style_line_rounded(c, true, LV_PART_ITEMS);
  lv_obj_set_style_size(c, 0, LV_PART_INDICATOR);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(c, chart_area_cb, LV_EVENT_DRAW_PART_BEGIN, (void *)(uintptr_t)fill_opa);
  return c;
}

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
  lb_cores_info = mk_label(cpu_card, &lv_font_montserrat_12, C_MUTED, 14, 80, "");
  thermal_dot = mk_rect(cpu_card, 15, 101, 7, 7, C_IDLE, 4);
  lb_thermal = mk_label(cpu_card, &lv_font_montserrat_12, C_MUTED, 27, 96, "");

  // chart frame: subtle grid lines (25/50/75%)
  int cx = 132, cy = 30, cw = 310, ch = 74;
  for (int i = 1; i <= 3; i++) {
    lv_obj_t *g = mk_rect(cpu_card, cx, cy + ch * i / 4, cw, 1, C_BORDER, 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_50, 0);
  }
  lv_obj_t *base = mk_rect(cpu_card, cx, cy + ch, cw, 1, C_BORDER, 0);
  (void)base;
  cpu_chart = mk_chart(cpu_card, cx, cy, cw, ch, SYS_HIST_N, LV_OPA_70);
  cpu_ser = lv_chart_add_series(cpu_chart, C_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
  for (auto &v : cpu_pts) v = LV_CHART_POINT_NONE;
  lv_chart_set_ext_y_array(cpu_chart, cpu_ser, cpu_pts);
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
  net_chart = mk_chart(nc, 92, 10, 62, 48, SYS_NET_N, LV_OPA_50);
  lv_obj_set_style_line_width(net_chart, 1, LV_PART_ITEMS);
  rx_ser = lv_chart_add_series(net_chart, C_RX, LV_CHART_AXIS_PRIMARY_Y);
  tx_ser = lv_chart_add_series(net_chart, C_TX, LV_CHART_AXIS_PRIMARY_Y);
  for (int i = 0; i < SYS_NET_N; i++) rx_pts[i] = tx_pts[i] = LV_CHART_POINT_NONE;
  lv_chart_set_ext_y_array(net_chart, rx_ser, rx_pts);
  lv_chart_set_ext_y_array(net_chart, tx_ser, tx_pts);

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
  lv_label_set_long_mode(lb_cl_detail, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(lb_cl_detail, 146);
}

// ---- update ------------------------------------------------------------------------------------
static uint32_t fnv1a(const char *s, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
  return h;
}

static void update_cpu(JsonObjectConst sy) {
  int cpu = sy["cpu"] | 0;
  lv_label_set_text_fmt(lb_cpu, "%d%%", cpu);
  lv_obj_set_style_text_color(lb_cpu, load_color(cpu), 0);

  JsonArrayConst load = sy["load"].as<JsonArrayConst>();
  if (load.size() >= 3)
  {
    char lb[48];
    snprintf(lb, sizeof lb, "load  %.1f  %.1f  %.1f", (double)(load[0] | 0.0f), (double)(load[1] | 0.0f),
             (double)(load[2] | 0.0f));
    lv_label_set_text(lb_load, lb);
  }
  else
    lv_label_set_text(lb_load, "");

  // history (right-aligned)
  JsonArrayConst hist = sy["hist"].as<JsonArrayConst>();
  int n = hist.size();
  if (n > SYS_HIST_N) n = SYS_HIST_N;
  int skip = (int)hist.size() - n, off = SYS_HIST_N - n, i = 0;
  for (int k = 0; k < off; k++) cpu_pts[k] = LV_CHART_POINT_NONE;
  for (JsonVariantConst v : hist) {
    if (i++ < skip) continue;
    int x = v | 0;
    cpu_pts[off++] = x < 0 ? 0 : (x > 100 ? 100 : x);
  }
  // auto-scale so an idle machine still shows texture (25 / 50 / 100 %)
  int mx = 0;
  for (int k = 0; k < SYS_HIST_N; k++)
    if (cpu_pts[k] != LV_CHART_POINT_NONE && cpu_pts[k] > mx) mx = cpu_pts[k];
  int scale = mx <= 22 ? 25 : mx <= 45 ? 50 : 100;
  if (scale != cpu_scale) {
    cpu_scale = scale;
    lv_chart_set_range(cpu_chart, LV_CHART_AXIS_PRIMARY_Y, 0, scale);
    lv_label_set_text_fmt(lb_scale, "%d%%", scale);
  }
  lv_chart_refresh(cpu_chart);
  lv_chart_set_series_color(cpu_chart, cpu_ser, lv_color_mix(load_color(cpu), C_ACCENT, 110));

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
      lv_label_set_text_fmt(core_lb[k], "%c%d", e ? 'E' : 'P', e ? ++ie : ++ip);
      lv_obj_set_style_text_color(core_lb[k], e ? C_DIM : C_MUTED, 0);
    } else {
      lv_label_set_text_fmt(core_lb[k], "%d", k + 1);
    }
    set_bar(core_bar[k], v);
    lv_label_set_text_fmt(core_val[k], "%d%%", v);
    lv_obj_set_style_text_color(core_val[k], v >= 85 ? load_color(v) : C_MUTED, 0);
  }
  if (use_kinds && ne && np)
    lv_label_set_text_fmt(lb_cores_info, "%dP + %dE cores", np, ne);
  else
    lv_label_set_text_fmt(lb_cores_info, "%d cores", (int)(sy["ncpu"] | nc));

  const char *th = sy["thermal"] | "";
  if (*th) {
    lv_color_t c = !strcmp(th, "nominal") ? C_IDLE
                   : !strcmp(th, "fair")  ? C_WAITING
                   : !strcmp(th, "serious") ? C_BUSY
                                            : C_ERROR;
    lv_label_set_text_fmt(lb_thermal, "thermal %s", th);
    lv_obj_set_style_bg_color(thermal_dot, c, 0);
    lv_obj_clear_flag(thermal_dot, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_label_set_text(lb_thermal, "");
    lv_obj_add_flag(thermal_dot, LV_OBJ_FLAG_HIDDEN);
  }
}

static void update_mem(JsonObjectConst sy) {
  JsonObjectConst m = sy["mem"];
  long total = m["total"] | 0L, used = m["used"] | 0L;
  char a[16], b[16];
  if (total <= 0) {
    lv_label_set_text(lb_mem_used, "--");
    lv_label_set_text(lb_mem_total, "");
    lv_label_set_text(lb_mem_pct, "");
    return;
  }
  int pct = (int)(used * 100 / total);
  snprintf(a, sizeof a, "%.1f", used / 1024.0);
  lv_label_set_text(lb_mem_used, a);
  snprintf(a, sizeof a, "/ %.0f GB", total / 1024.0);
  lv_label_set_text(lb_mem_total, a);
  lv_obj_align_to(lb_mem_total, lb_mem_used, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -3);
  lv_label_set_text_fmt(lb_mem_pct, "%d%%", pct);
  lv_obj_set_style_text_color(lb_mem_pct, load_color(pct), 0);
  lv_obj_align_to(lb_mem_pct, lb_mem_total, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

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
    lv_obj_set_x(mem_seg[i], x);
    lv_obj_set_width(mem_seg[i], w);
    x += w;
  }
  // legend under the bar, one quarter each
  for (int i = 0; i < 4; i++) {
    lv_obj_t *dot = (lv_obj_t *)lv_obj_get_user_data(mem_leg[i]);
    if (!have || seg[i] < 0) {
      lv_obj_add_flag(mem_leg[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(mem_leg[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
    fmt_mb(a, sizeof a, seg[i]);
    lv_label_set_text_fmt(mem_leg[i], "%s %s", names[i], a);
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
  lv_label_set_text_fmt(lb_mem_right, "%sswap %s", pr, sw > 0 ? b : "0");
  lv_obj_set_style_text_color(lb_mem_right, !strcmp(lvl, "critical") ? C_ERROR : !strcmp(lvl, "warn") ? C_WAITING : C_MUTED, 0);
}

static void update_procs(JsonObjectConst sy) {
  JsonArrayConst procs = sy["procs"].as<JsonArrayConst>();
  for (int i = 0; i < SYS_PROCS; i++) {
    JsonObjectConst p = procs[i];
    bool on = !p.isNull();
    lv_obj_t *objs[5] = {pr_dot[i], pr_name[i], pr_bar[i], pr_cpu[i], pr_mem[i]};
    for (auto o : objs) {
      if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    if (!on) continue;
    bool cl = (p["cl"] | 0) != 0;
    float c = p["c"] | 0.0f;
    long mb = p["m"] | 0L;
    char mbs[16];
    fmt_mb(mbs, sizeof mbs, mb);
    lv_label_set_text(pr_name[i], p["n"] | "?");
    lv_obj_set_style_text_color(pr_name[i], cl ? C_ACCENT2 : C_TEXT, 0);
    if (cl) lv_obj_clear_flag(pr_dot[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(pr_dot[i], LV_OBJ_FLAG_HIDDEN);
    if (c >= 10) lv_label_set_text_fmt(pr_cpu[i], "%d%%", (int)lroundf(c));
    else {
      char cb[12];
      snprintf(cb, sizeof cb, "%.1f%%", (double)c);
      lv_label_set_text(pr_cpu[i], cb);
    }
    lv_obj_set_style_text_color(pr_cpu[i], c >= 85 ? load_color((int)c) : C_TEXT, 0);
    lv_label_set_text(pr_mem[i], mbs);
    set_bar(pr_bar[i], (int)c);
  }
}

static void update_net(JsonObjectConst sy) {
  JsonObjectConst n = sy["net"];
  char a[20];
  if (n.isNull()) {
    lv_label_set_text(lb_rx, LV_SYMBOL_DOWN " --");
    lv_label_set_text(lb_tx, LV_SYMBOL_UP " --");
    return;
  }
  fmt_rate(a, sizeof a, n["rx"] | 0L);
  lv_label_set_text_fmt(lb_rx, LV_SYMBOL_DOWN " %s", a);
  fmt_rate(a, sizeof a, n["tx"] | 0L);
  lv_label_set_text_fmt(lb_tx, LV_SYMBOL_UP " %s", a);
  JsonArrayConst rh = n["rx_hist"].as<JsonArrayConst>(), th = n["tx_hist"].as<JsonArrayConst>();
  long mx = 64;  // KB/s floor so idle links don't look busy
  for (JsonVariantConst v : rh) mx = max(mx, (long)(v | 0L));
  for (JsonVariantConst v : th) mx = max(mx, (long)(v | 0L));
  struct { JsonArrayConst a; lv_coord_t *pts; } s[2] = {{rh, rx_pts}, {th, tx_pts}};
  for (auto &e : s) {
    int cnt = e.a.size();
    if (cnt > SYS_NET_N) cnt = SYS_NET_N;
    int skip = (int)e.a.size() - cnt, off = SYS_NET_N - cnt, i = 0;
    for (int k = 0; k < off; k++) e.pts[k] = LV_CHART_POINT_NONE;
    for (JsonVariantConst v : e.a) {
      if (i++ < skip) continue;
      e.pts[off++] = (lv_coord_t)((v | 0L) * 100 / mx);
    }
  }
  lv_chart_refresh(net_chart);
}

static void update_claude(JsonObjectConst sy) {
  JsonObjectConst c = sy["claude"];
  if (c.isNull()) {
    lv_label_set_text(lb_cl_pct, "--");
    lv_label_set_text(lb_cl_detail, "no data");
    lv_obj_align_to(lb_cl_sub, lb_cl_pct, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, -3);
    return;
  }
  int ncpu = sy["ncpu"] | 1;
  float cpu = c["cpu"] | 0.0f;
  int share = (int)lroundf(cpu / (ncpu > 0 ? ncpu : 1));
  if (share > 100) share = 100;
  lv_label_set_text_fmt(lb_cl_pct, "%d%%", share);
  lv_obj_align_to(lb_cl_sub, lb_cl_pct, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, -3);
  char mb[16];
  fmt_mb(mb, sizeof mb, c["mem"] | 0L);
  int ns = c["n"] | 0, kids = c["kids"] | 0;
  lv_label_set_text_fmt(lb_cl_detail, "%s  %d sess  %d procs", mb, ns, kids);
  lv_label_set_text(lb_cl_sub, "of cpu");
}

static void update_header(JsonObjectConst sy) {
  lv_label_set_text(lb_host, sy["host"] | "this computer");
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
  lv_label_set_text(lb_meta, buf);
}

static void update(JsonObjectConst s) {
  JsonObjectConst sy = s["system"];
  if (sy.isNull()) {
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
    last_hash = 0;
    return;
  }
  // state arrives up to 4x/s; system metrics change at 1 Hz -> skip identical snapshots
  static char buf[3072];
  size_t n = serializeJson(sy, buf, sizeof buf);
  uint32_t h = fnv1a(buf, n);
  if (h == last_hash) return;
  last_hash = h;

  lv_obj_add_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
  update_header(sy);
  update_cpu(sy);
  update_mem(sy);
  update_procs(sy);
  update_net(sy);
  update_claude(sy);
}

static void on_show(bool visible) {
  if (visible) last_hash = 0;  // force a full refresh when we come back
}

const ScreenDef screen_system = {"system", "System", create, update, nullptr, on_show};
