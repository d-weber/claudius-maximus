// Usage & stats screen (owner: usage). Data: state.usage (host/companion/providers/usage.py) + state.totals.
//
//  +--------------------------------------------+
//  | USAGE                         ^ 6-day streak|
//  |   .-------.          .-------.              |
//  |  / 5-HOUR  \        / WEEKLY  \             |   two big 270deg rings: plan limits when known (the Fable
//  | |   42%     |      |   18%     |            |   week on a plan without a 5-hour window), otherwise "this
//  |  \ 2h 15m  /        \ 4d 2h   /             |   5h block" (estimate) and cache hit
//  |  $10.92      1.2M        $39                |   today cost / today tokens / 7-day cost
//  |  [opus=========|fable==|h]  legend          |   model mix (14 days)
//  | +----------------------------------------+  |
//  | | LAST HOUR                    151k/min  |  |   tap card: last hour -> today -> 14 days
//  | | ||| |  |||||||||| ||  |||||||||||||||| |  |
//  | | -60m          -30m               now   |  |
//  | +----------------------------------------+  |
//  +--------------------------------------------+
#include "screens.h"
#include "../ui/theme.h"
#include "../core/model.h"
#include <math.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#define C_FABLE   lv_color_hex(0xC9A0DC)
#define C_SONNET  lv_color_hex(0x6A9BCC)
#define C_HAIKU   lv_color_hex(0x5FB3A3)
#define C_CACHE   lv_color_hex(0x5FB3A3)

// ---------------------------------------------------------------- formatting helpers
static void fmt_tok(char *b, size_t n, double v) {
  if (v < 1000) snprintf(b, n, "%d", (int)v);
  else if (v < 10e3) snprintf(b, n, "%.1fk", v / 1e3);
  else if (v < 1e6) snprintf(b, n, "%dk", (int)(v / 1e3 + 0.5));
  else if (v < 10e6) snprintf(b, n, "%.2fM", v / 1e6);
  else if (v < 1e9) snprintf(b, n, "%.1fM", v / 1e6);
  else snprintf(b, n, "%.2fB", v / 1e9);
}
static void fmt_usd(char *b, size_t n, double v) {
  if (v < 0.005) snprintf(b, n, "$0");
  else if (v < 100) snprintf(b, n, "$%.2f", v);
  else if (v < 10000) snprintf(b, n, "$%d", (int)(v + 0.5));
  else snprintf(b, n, "$%.1fk", v / 1e3);
}
static void fmt_dur(char *b, size_t n, long s) {
  if (s < 0) s = 0;
  long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
  if (d > 0) snprintf(b, n, "%ldd %ldh", d, h);
  else if (h > 0) snprintf(b, n, "%ldh %02ldm", h, m);
  else snprintf(b, n, "%ldm", m < 1 ? 1 : m);
}
// Perf: lv_label_set_text() always invalidates (even with identical text), so only touch a label
// when its displayed string actually changes. Same for style colors (refresh_style invalidates the
// whole object -- 40k px for a 200 px ring).
static void set_txt(lv_obj_t *l, const char *t) {
  if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}
static void set_txt_fmt(lv_obj_t *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_txt_fmt(lv_obj_t *l, const char *fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  set_txt(l, buf);
}
static uint32_t now_epoch(JsonObjectConst s) {
  uint32_t t = model_now();
  return t ? t : (uint32_t)(s["ts"] | 0);
}

// ---------------------------------------------------------------- rings
struct Ring {
  lv_obj_t *arc, *title, *value, *sub;
  int shown;  // value currently displayed on the arc (0..1000), for animation
  uint32_t col;  // indicator color currently applied (lv_color_to32), 0 = none yet
};
static Ring rings[2];

static void arc_anim_cb(void *arc, int32_t v) { lv_arc_set_value((lv_obj_t *)arc, (int16_t)v); }

static void ring_create(lv_obj_t *p, Ring &r, int x, int y, int d) {
  r.arc = lv_arc_create(p);
  lv_obj_set_size(r.arc, d, d);
  lv_obj_set_pos(r.arc, x, y);
  lv_arc_set_rotation(r.arc, 135);
  lv_arc_set_bg_angles(r.arc, 0, 270);
  lv_arc_set_range(r.arc, 0, 1000);
  lv_arc_set_value(r.arc, 0);
  lv_obj_remove_style(r.arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(r.arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(r.arc, 18, LV_PART_MAIN);
  lv_obj_set_style_arc_width(r.arc, 18, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(r.arc, C_SURFACE2, LV_PART_MAIN);
  lv_obj_set_style_arc_color(r.arc, C_ACCENT, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(r.arc, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(r.arc, true, LV_PART_INDICATOR);
  lv_obj_set_style_pad_all(r.arc, 0, 0);
  r.shown = 0;
  r.col = 0;

  r.title = lv_label_create(r.arc);
  lv_obj_set_style_text_font(r.title, F_SMALL, 0);
  lv_obj_set_style_text_color(r.title, C_MUTED, 0);
  lv_obj_set_style_text_letter_space(r.title, 1, 0);
  lv_obj_align(r.title, LV_ALIGN_CENTER, 0, -42);
  lv_label_set_text(r.title, "");

  r.value = lv_label_create(r.arc);
  lv_obj_set_style_text_font(r.value, F_HUGE, 0);
  lv_obj_set_style_text_color(r.value, C_TEXT, 0);
  lv_obj_align(r.value, LV_ALIGN_CENTER, 0, 2);
  lv_label_set_text(r.value, "--");

  r.sub = lv_label_create(r.arc);
  lv_obj_set_style_text_font(r.sub, F_BODY, 0);
  lv_obj_set_style_text_color(r.sub, C_MUTED, 0);
  lv_obj_align(r.sub, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_label_set_text(r.sub, "");
}

static void ring_set(Ring &r, const char *title, const char *value, const char *sub, int permille, lv_color_t col) {
  set_txt(r.title, title);
  set_txt(r.value, value);
  set_txt(r.sub, sub);
  uint32_t c32 = lv_color_to32(col) | 0xFF000000u;
  if (c32 != r.col) {
    lv_obj_set_style_arc_color(r.arc, col, LV_PART_INDICATOR);
    r.col = c32;
  }
  if (permille < 0) permille = 0;
  if (permille > 1000) permille = 1000;
  if (permille != r.shown) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, r.arc);
    lv_anim_set_exec_cb(&a, arc_anim_cb);
    lv_anim_set_values(&a, lv_arc_get_value(r.arc), permille);
    lv_anim_set_time(&a, 700);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    r.shown = permille;
  }
}

static lv_color_t limit_color(int pct) {
  if (pct >= 90) return C_ERROR;
  if (pct >= 70) return C_WAITING;
  return C_ACCENT;
}

// ---------------------------------------------------------------- stats row
struct Stat { lv_obj_t *value, *label; };
static Stat stats[3];
static lv_obj_t *streak_lbl;

static void stat_create(lv_obj_t *p, Stat &s, int x, int w) {
  s.value = lv_label_create(p);
  lv_obj_set_style_text_font(s.value, F_LARGE, 0);
  lv_obj_set_style_text_color(s.value, C_TEXT, 0);
  lv_obj_set_style_text_align(s.value, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(s.value, w);
  lv_obj_set_pos(s.value, x, 236);
  lv_label_set_text(s.value, "--");
  s.label = lv_label_create(p);
  lv_obj_set_style_text_font(s.label, F_SMALL, 0);
  lv_obj_set_style_text_color(s.label, C_MUTED, 0);
  lv_obj_set_style_text_align(s.label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_letter_space(s.label, 1, 0);
  lv_obj_set_width(s.label, w);
  lv_obj_set_pos(s.label, x, 274);
  lv_label_set_text(s.label, "");
}

// ---------------------------------------------------------------- model mix
#define MIX_N 5
static const char *MIX_KEYS[MIX_N] = {"opus", "fable", "sonnet", "haiku", "other"};
static lv_obj_t *mix_bar, *mix_seg[MIX_N], *mix_legend;
static int mix_w[MIX_N];  // width currently applied to each segment (0 = hidden)
static lv_color_t mix_color(int i) {
  switch (i) {
    case 0: return C_ACCENT;
    case 1: return C_FABLE;
    case 2: return C_SONNET;
    case 3: return C_HAIKU;
    default: return C_DIM;
  }
}

// ---------------------------------------------------------------- chart card
enum { MODE_HOUR, MODE_TODAY, MODE_DAYS, MODE_N };
static int mode = MODE_HOUR;
static lv_obj_t *card, *chart, *card_title, *card_value, *axis[3], *mode_dots[MODE_N];
static lv_chart_series_t *ser;
static lv_coord_t pts[60];
static int npts = 0, hilite = -1, peak_idx = -1;

static void chart_draw_cb(lv_event_t *e) {
  lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(e);
  if (d->part != LV_PART_ITEMS || !d->rect_dsc || d->type != LV_CHART_DRAW_PART_BAR) return;
  int i = (int)d->id;
  lv_area_t *a = (lv_area_t *)d->draw_area;
  lv_coord_t bottom = chart->coords.y2 - lv_obj_get_style_pad_bottom(chart, 0);
  if (pts[i] <= 0) {
    // empty slot: tiny baseline tick so the time axis stays readable
    if (a->y1 > bottom - 3) a->y1 = bottom - 3;
    d->rect_dsc->bg_color = C_SURFACE2;
    return;
  }
  if (a->y1 > bottom - 4) a->y1 = bottom - 4;
  if (i == hilite) d->rect_dsc->bg_color = C_ACCENT2;
  else if (i == peak_idx) d->rect_dsc->bg_color = C_ACCENT;
  else d->rect_dsc->bg_color = lv_color_mix(C_ACCENT, C_SURFACE, 170);
}

static void set_mode_dots() {
  for (int i = 0; i < MODE_N; i++) {
    lv_obj_set_style_bg_color(mode_dots[i], i == mode ? C_ACCENT : C_DIM, 0);
    lv_obj_set_width(mode_dots[i], i == mode ? 14 : 6);
  }
}

static void card_clicked(lv_event_t *) {
  mode = (mode + 1) % MODE_N;
  set_mode_dots();
  screen_usage.update(model_state());
}

// ---------------------------------------------------------------- create
static void create(lv_obj_t *p) {
  lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *t = lv_label_create(p);
  lv_label_set_text(t, "USAGE");
  lv_obj_set_style_text_font(t, F_SMALL, 0);
  lv_obj_set_style_text_color(t, C_MUTED, 0);
  lv_obj_set_style_text_letter_space(t, 3, 0);
  lv_obj_set_pos(t, 20, 14);

  streak_lbl = lv_label_create(p);
  lv_obj_set_style_text_font(streak_lbl, F_SMALL, 0);
  lv_obj_set_style_text_color(streak_lbl, C_ACCENT2, 0);
  lv_obj_align(streak_lbl, LV_ALIGN_TOP_RIGHT, -20, 14);
  lv_label_set_text(streak_lbl, "");

  ring_create(p, rings[0], 22, 32, 200);
  ring_create(p, rings[1], 258, 32, 200);

  stat_create(p, stats[0], 12, 152);
  stat_create(p, stats[1], 164, 152);
  stat_create(p, stats[2], 316, 152);

  // model mix bar + legend
  mix_bar = lv_obj_create(p);
  lv_obj_remove_style_all(mix_bar);
  lv_obj_set_size(mix_bar, 440, 8);
  lv_obj_set_pos(mix_bar, 20, 300);
  lv_obj_set_style_radius(mix_bar, 4, 0);
  lv_obj_set_style_clip_corner(mix_bar, true, 0);
  lv_obj_set_style_bg_color(mix_bar, C_SURFACE2, 0);
  lv_obj_set_style_bg_opa(mix_bar, LV_OPA_COVER, 0);
  lv_obj_set_flex_flow(mix_bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(mix_bar, 2, 0);
  for (int i = 0; i < MIX_N; i++) {
    mix_seg[i] = lv_obj_create(mix_bar);
    lv_obj_remove_style_all(mix_seg[i]);
    lv_obj_set_size(mix_seg[i], 0, 8);
    lv_obj_set_style_bg_color(mix_seg[i], mix_color(i), 0);
    lv_obj_set_style_bg_opa(mix_seg[i], LV_OPA_COVER, 0);
    lv_obj_add_flag(mix_seg[i], LV_OBJ_FLAG_HIDDEN);
  }
  mix_legend = lv_label_create(p);
  lv_label_set_recolor(mix_legend, true);
  lv_obj_set_style_text_font(mix_legend, F_SMALL, 0);
  lv_obj_set_style_text_color(mix_legend, C_MUTED, 0);
  lv_obj_set_width(mix_legend, 440);
  lv_obj_set_style_text_align(mix_legend, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(mix_legend, 20, 312);
  lv_label_set_text(mix_legend, "");

  // chart card
  card = lv_obj_create(p);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, 456, 120);
  lv_obj_set_pos(card, 12, 336);
  lv_obj_set_style_bg_color(card, C_SURFACE, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(card, C_SURFACE2, LV_STATE_PRESSED);
  lv_obj_set_style_radius(card, 16, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(card, card_clicked, LV_EVENT_CLICKED, NULL);

  card_title = lv_label_create(card);
  lv_obj_set_style_text_font(card_title, F_SMALL, 0);
  lv_obj_set_style_text_color(card_title, C_MUTED, 0);
  lv_obj_set_style_text_letter_space(card_title, 2, 0);
  lv_obj_set_pos(card_title, 16, 10);
  lv_label_set_text(card_title, "");

  card_value = lv_label_create(card);
  lv_obj_set_style_text_font(card_value, F_BODY, 0);
  lv_obj_set_style_text_color(card_value, C_TEXT, 0);
  lv_obj_align(card_value, LV_ALIGN_TOP_RIGHT, -16, 6);
  lv_label_set_text(card_value, "");

  lv_obj_t *dots = lv_obj_create(card);
  lv_obj_remove_style_all(dots);
  lv_obj_set_size(dots, 50, 8);
  lv_obj_align(dots, LV_ALIGN_TOP_MID, 0, 14);
  lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(dots, 4, 0);
  lv_obj_clear_flag(dots, LV_OBJ_FLAG_CLICKABLE);
  for (int i = 0; i < MODE_N; i++) {
    mode_dots[i] = lv_obj_create(dots);
    lv_obj_remove_style_all(mode_dots[i]);
    lv_obj_set_size(mode_dots[i], 6, 4);
    lv_obj_set_style_radius(mode_dots[i], 2, 0);
    lv_obj_set_style_bg_opa(mode_dots[i], LV_OPA_COVER, 0);
  }
  set_mode_dots();

  chart = lv_chart_create(card);
  lv_obj_set_size(chart, 424, 62);
  lv_obj_set_pos(chart, 16, 34);
  lv_chart_set_type(chart, LV_CHART_TYPE_BAR);
  lv_chart_set_div_line_count(chart, 0, 0);
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
  lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_pad_all(chart, 0, 0);
  lv_obj_set_style_radius(chart, 2, LV_PART_ITEMS);
  lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(chart, LV_OBJ_FLAG_EVENT_BUBBLE);
  ser = lv_chart_add_series(chart, C_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
  lv_obj_add_event_cb(chart, chart_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

  static const lv_align_t al[3] = {LV_ALIGN_BOTTOM_LEFT, LV_ALIGN_BOTTOM_MID, LV_ALIGN_BOTTOM_RIGHT};
  for (int i = 0; i < 3; i++) {
    axis[i] = lv_label_create(card);
    lv_obj_set_style_text_font(axis[i], F_SMALL, 0);
    lv_obj_set_style_text_color(axis[i], C_DIM, 0);
    lv_obj_align(axis[i], al[i], i == 0 ? 16 : (i == 2 ? -16 : 0), -6);
    lv_label_set_text(axis[i], "");
  }
}

// ---------------------------------------------------------------- update
static void load_series(JsonArrayConst arr, int n, int hl) {
  double mx = 0;
  double vals[60];
  for (int i = 0; i < n; i++) vals[i] = 0;
  // right-align: if the host sends fewer values, the newest still sits at the right edge
  int sz = (int)arr.size();
  for (int i = 0; i < n && i < sz; i++) vals[n - 1 - i] = arr[sz - 1 - i] | 0.0;
  int pk = -1;
  for (int i = 0; i < n; i++)
    if (vals[i] > mx) { mx = vals[i]; pk = i; }
  lv_coord_t np[60];
  for (int i = 0; i < n; i++) {
    // sqrt-ish compression keeps small minutes visible next to big bursts
    double r = mx > 0 ? pow(vals[i] / mx, 0.6) : 0;
    np[i] = vals[i] > 0 ? (lv_coord_t)(r * 1000) : 0;
  }
  if (npts != n) {
    // mode switch: new point count / spacing -> full redraw
    memcpy(pts, np, n * sizeof(lv_coord_t));
    peak_idx = pk;
    hilite = hl;
    npts = n;
    lv_chart_set_point_count(chart, n);
    lv_chart_set_ext_y_array(chart, ser, pts);
    lv_obj_set_style_pad_column(chart, n >= 60 ? 2 : (n >= 24 ? 5 : 10), 0);
    lv_obj_invalidate(chart);
    return;
  }
  // Perf: only invalidate the bar columns whose height or color class changed. A shifted
  // series (tpm rolls once a minute) changes nearly every bar -> one whole-chart invalidate.
  int old_pk = peak_idx, old_hl = hilite;
  peak_idx = pk;
  hilite = hl;
  bool dirty[60];
  int nd = 0;
  for (int i = 0; i < n; i++) {
    dirty[i] = np[i] != pts[i] || ((i == old_pk) != (i == pk)) || ((i == old_hl) != (i == hl));
    if (dirty[i]) { pts[i] = np[i]; nd++; }
  }
  if (!nd) return;
  if (nd > n / 2) { lv_obj_invalidate(chart); return; }
  for (int i = 0; i < n; i++)
    if (dirty[i]) lv_chart_set_value_by_id(chart, ser, i, pts[i]);
}

static double sum_last(JsonArrayConst a, int k) {
  double s = 0;
  int n = a.size();
  for (int i = n - k < 0 ? 0 : n - k; i < n; i++) s += a[i] | 0.0;
  return s;
}

static void update(JsonObjectConst s) {
  JsonObjectConst u = s["usage"];
  JsonObjectConst tot = s["totals"];
  JsonObjectConst today = u["today"];
  JsonArrayConst tpm = u["tpm"], hourly = u["hourly"], daily = u["daily"], dcost = u["daily_cost"];
  uint32_t now = now_epoch(s);
  char a[32], b[48], c[64];

  // derived fallbacks for hosts/demo that only send the base protocol fields
  double tok_today = today["tokens"] | (daily.size() ? (daily[daily.size() - 1] | 0.0) : (tot["tokens_today"] | 0.0));
  double cost_today = today["cost"] | (tot["cost_today"] | 0.0);
  double tpm_now = u["tpm_now"] | (sum_last(tpm, 5) / 5.0);
  int cache_pct = today["cache_pct"] | -1;
  double saved = today["saved"] | -1.0;
  if (cache_pct < 0) {
    double cr = 0, fresh = 0;
    for (JsonObjectConst ss : s["sessions"].as<JsonArrayConst>()) {
      cr += ss["tokens"]["cache_read"] | 0.0;
      fresh += (ss["tokens"]["in"] | 0.0) + (ss["tokens"]["cache_write"] | 0.0);
    }
    if (cr + fresh > 0) cache_pct = (int)(100 * cr / (cr + fresh) + 0.5);
  }

  // ---- streak
  int streak = u["streak_days"] | 0;
  if (streak > 1) set_txt_fmt(streak_lbl, LV_SYMBOL_CHARGE " %d-day streak", streak);
  else if (streak == 1) set_txt(streak_lbl, LV_SYMBOL_CHARGE " active today");
  else set_txt(streak_lbl, "");

  // ---- left ring: 5-hour plan limit; on a plan without one (five_hour.off) the per-model weekly window
  // (Fable) the HUD also shows; else estimated 5h block, else today's pace
  JsonObjectConst fh = u["five_hour"], win = u["window"], mw = u["seven_day_models"][0];
  if (!fh.isNull() && !fh["pct"].isNull()) {
    int pct = fh["pct"] | 0;
    uint32_t ra = fh["resets_at"] | 0;
    snprintf(a, sizeof a, "%d%%", pct);
    if (ra > now) { fmt_dur(c, sizeof c, ra - now); snprintf(b, sizeof b, LV_SYMBOL_REFRESH " %s", c); }
    else snprintf(b, sizeof b, "5h window");
    ring_set(rings[0], "5-HOUR LIMIT", a, b, pct * 10, limit_color(pct));
  } else if ((fh["off"] | 0) && !mw["pct"].isNull()) {
    int pct = mw["pct"] | 0;
    uint32_t ra = mw["resets_at"] | 0, at = mw["at"] | 0u;
    char title[24];
    snprintf(title, sizeof title, "%.8s WEEKLY", mw["name"] | "MODEL");
    for (char *p = title; *p; p++) *p = (char)toupper((unsigned char)*p);
    snprintf(a, sizeof a, "%d%%", pct);
    if (ra > now) { fmt_dur(c, sizeof c, ra - now); snprintf(b, sizeof b, LV_SYMBOL_REFRESH " %s", c); }
    else snprintf(b, sizeof b, "7d window");
    // grey when the host only has an old observation of it (same 12 h rule as the HUD)
    bool stale = now && at && now > at + 12 * 3600;
    ring_set(rings[0], title, a, b, pct * 10, stale ? C_DIM : limit_color(pct));
  } else if (!win.isNull()) {
    uint32_t ra = win["resets_at"] | 0;
    fmt_usd(a, sizeof a, win["cost"] | 0.0);
    if (ra > now) { fmt_dur(c, sizeof c, ra - now); snprintf(b, sizeof b, LV_SYMBOL_REFRESH " %s", c); }
    else snprintf(b, sizeof b, "block ended");
    ring_set(rings[0], "THIS 5H BLOCK", a, b, (int)(win["pct"] | 0) * 10, C_ACCENT2);
  } else {
    fmt_tok(a, sizeof a, tpm_now);
    double mx = 1;
    for (JsonVariantConst v : tpm) mx = fmax(mx, v | 0.0);
    ring_set(rings[0], "TOKENS / MIN", a, tpm_now > 0 ? "right now" : "quiet", (int)(1000 * tpm_now / mx), C_ACCENT2);
  }

  // ---- right ring: weekly limit, else prompt-cache hit ratio
  JsonObjectConst sd = u["seven_day"];
  if (!sd.isNull() && !sd["pct"].isNull()) {
    int pct = sd["pct"] | 0;
    uint32_t ra = sd["resets_at"] | 0;
    snprintf(a, sizeof a, "%d%%", pct);
    if (ra > now) { fmt_dur(c, sizeof c, ra - now); snprintf(b, sizeof b, LV_SYMBOL_REFRESH " %s", c); }
    else snprintf(b, sizeof b, "7d window");
    ring_set(rings[1], "WEEKLY LIMIT", a, b, pct * 10, limit_color(pct));
  } else if (cache_pct >= 0) {
    snprintf(a, sizeof a, "%d%%", cache_pct);
    if (saved >= 0) { fmt_usd(c, sizeof c, saved); snprintf(b, sizeof b, "saved %s", c); }
    else snprintf(b, sizeof b, "prompt cache");
    ring_set(rings[1], "CACHE HIT", a, b, cache_pct * 10, C_CACHE);
  } else {
    ring_set(rings[1], "CACHE HIT", "--", "no data", 0, C_CACHE);
  }

  // ---- stats row
  fmt_usd(a, sizeof a, cost_today);
  set_txt(stats[0].value, a);
  double burn = u["burn"] | -1.0;
  if (burn > 0.005) { fmt_usd(c, sizeof c, burn); set_txt_fmt(stats[0].label, "TODAY  %s/h", c); }
  else set_txt(stats[0].label, "SPENT TODAY");
  fmt_tok(a, sizeof a, tok_today);
  set_txt(stats[1].value, a);
  int msgs = today["msgs"] | -1;
  if (msgs > 0) set_txt_fmt(stats[1].label, "TOKENS  %d msgs", msgs);
  else set_txt(stats[1].label, "TOKENS TODAY");
  JsonObjectConst wk = u["week"];
  double wcost = wk["cost"] | (dcost.size() ? sum_last(dcost, 7) : -1.0);
  if (wcost >= 0) {
    fmt_usd(a, sizeof a, wcost);
    set_txt(stats[2].value, a);
    set_txt(stats[2].label, "LAST 7 DAYS");
  } else {
    fmt_tok(a, sizeof a, sum_last(daily, 7));
    set_txt(stats[2].value, a);
    set_txt(stats[2].label, "TOKENS 7 DAYS");
  }

  // ---- model mix
  JsonObjectConst bm = u["by_model"];
  double mv[MIX_N] = {0}, mt = 0;
  for (JsonPairConst kv : bm) {
    int k = MIX_N - 1;
    for (int i = 0; i < MIX_N - 1; i++)
      if (strstr(kv.key().c_str(), MIX_KEYS[i])) { k = i; break; }
    mv[k] += kv.value() | 0.0;
  }
  for (int i = 0; i < MIX_N; i++) mt += mv[i];
  static char leg[200];
  leg[0] = 0;
  int shown = 0;
  for (int i = 0; i < MIX_N; i++) {
    int w = mt > 0 ? (int)(440.0 * mv[i] / mt + 0.5) : 0;
    if (w < 3 && mv[i] > 0) w = 3;
    if (w != mix_w[i]) {
      if (w > 0) { lv_obj_clear_flag(mix_seg[i], LV_OBJ_FLAG_HIDDEN); lv_obj_set_width(mix_seg[i], w); }
      else lv_obj_add_flag(mix_seg[i], LV_OBJ_FLAG_HIDDEN);
      mix_w[i] = w;
    }
    if (mv[i] > 0 && shown < 4) {
      int pct = (int)(100 * mv[i] / mt + 0.5);
      lv_color32_t c32;
      c32.full = lv_color_to32(mix_color(i));
      size_t l = strlen(leg);
      snprintf(leg + l, sizeof(leg) - l, "%s#%02X%02X%02X " LV_SYMBOL_STOP "# %s %s%d%%", shown ? "     " : "",
               c32.ch.red, c32.ch.green, c32.ch.blue, MIX_KEYS[i], pct < 1 ? "<" : "", pct < 1 ? 1 : pct);
      shown++;
    }
  }
  if (!shown) snprintf(leg, sizeof leg, "model mix: no data yet");
  set_txt(mix_legend, leg);

  // ---- chart card
  int32_t tz = model_tz();
  long local = (long)now + tz;
  if (mode == MODE_HOUR) {
    load_series(tpm, 60, 59);
    set_txt(card_title, "LAST HOUR");
    fmt_tok(a, sizeof a, tpm_now);
    set_txt_fmt(card_value, "%s/min", a);
    set_txt(axis[0], "-60m");
    set_txt(axis[1], "-30m");
    set_txt(axis[2], "now");
  } else if (mode == MODE_TODAY) {
    int hr = (int)((local % 86400 + 86400) % 86400 / 3600);
    load_series(hourly, 24, hr);
    set_txt(card_title, "TODAY");
    int ph = u["peak_hour"] | peak_idx;
    if (ph >= 0 && peak_idx >= 0) {
      fmt_tok(a, sizeof a, hourly[ph] | 0.0);
      set_txt_fmt(card_value, "peak %02d:00  %s", ph, a);
    } else set_txt(card_value, "");
    set_txt(axis[0], "00:00");
    set_txt(axis[1], "12:00");
    set_txt(axis[2], "23:00");
  } else {
    load_series(daily, 14, 13);
    set_txt(card_title, "14 DAYS");
    if (dcost.size()) { fmt_usd(a, sizeof a, sum_last(dcost, 14)); set_txt_fmt(card_value, "%s total", a); }
    else { fmt_tok(a, sizeof a, sum_last(daily, 14)); set_txt_fmt(card_value, "%s tokens", a); }
    static const char *WD[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    for (int k = 0; k < 2; k++) {
      time_t t = (time_t)(local - (k == 0 ? 13 : 6) * 86400L);
      struct tm tmv;
      gmtime_r(&t, &tmv);
      set_txt_fmt(axis[k], "%s %d", WD[tmv.tm_wday], tmv.tm_mday);
    }
    set_txt(axis[2], "today");
  }
}

static void on_show(bool visible) {
  if (visible) lv_chart_refresh(chart);
}

// dev hook: {"t":"event","kind":"usage_mode","mode":0..2} selects the chart card mode (for screenshots)
static void on_event(JsonObjectConst ev) {
  if (strcmp(ev["kind"] | "", "usage_mode")) return;
  mode = ((int)(ev["mode"] | 0) % MODE_N + MODE_N) % MODE_N;
  set_mode_dots();
  if (!strcmp(ui_current(), "usage")) update(model_state());
}

const ScreenDef screen_usage = {"usage", "Usage", create, update, on_event, on_show};
