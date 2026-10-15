// Activity feed: live, merged timeline of what every agent is doing. (owner: detail-feed)
#include "screens.h"
#include "feed_common.h"
#include "../ui/theme.h"
#include "../core/model.h"
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------- data
#define FEED_MAX 48          // items kept
#define POOL 18              // row widgets (max rows rendered)
#define ROW_H 64
#define ROW_GAP 6
#define MERGE_WINDOW 60      // s: same session + same tool merges into "x N"
#define MERGE_LOOKBACK 3     // how many recent items we look at for a merge

enum { F_ALL = 0, F_NEEDS = 1, F_ERR = 2 };

struct Item {
  char sid[12];
  char name[28];
  char kind[16];
  char tool[20];
  char text[84];
  uint32_t ts;
  uint16_t count;
  uint16_t ver;              // bumped when a merge changes the item's content
  uint32_t uid;              // stable identity (widgets stay bound to it across shifts)
  bool sub;
};

static Item items[FEED_MAX];
static int n_items = 0;
static uint32_t next_uid = 1;

#define TICK_N 128           // timestamps of recent tool calls (tools/min)
static uint32_t tick_ts[TICK_N];
static int tick_head = 0;

// ---------------------------------------------------------------- widgets
struct Row {
  lv_obj_t *obj, *glyph, *l1, *l2, *time;
  int item;                  // index into items[] currently bound (-1 none)
  uint32_t uid;              // uid of the bound item (0 none)
  uint16_t ver;              // version of the bound item content
  char time_txt[8];
};
static Row rows[POOL];
static lv_obj_t *list_clip, *list, *empty_lbl, *rate_lbl, *spark[12], *chips[3], *chip_lbl[3];
static lv_timer_t *timer, *render_timer;
static int filter = F_ALL;
static bool visible = false, dirty = true, render_pending = false;
static uint32_t top_uid = 0;          // uid shown in the first row after the last render
static int last_needs = -1, last_errs = -1, last_filter = -1;
static int last_rate = -1, spark_h[12], spark_on[12];
#define RENDER_COALESCE_MS 250       // bursts of events → at most one rebuild per this period

static uint32_t now_s() { return model_now(); }

static bool is_needs(const Item &it) { return !strcmp(it.kind, "waiting"); }
static bool is_err(const Item &it) { return !strcmp(it.kind, "tool_error"); }
static bool passes(const Item &it) {
  if (filter == F_NEEDS) return is_needs(it) || is_err(it);
  if (filter == F_ERR) return is_err(it);
  return true;
}

// ---------------------------------------------------------------- ingest
static void ingest(JsonObjectConst ev) {
  const char *kind = ev["kind"] | "";
  if (!*kind) return;
  const char *sid = ev["sid"] | "";
  const char *tool = ev["tool"] | "";
  bool sub = !ev["agent"].isNull() && !strncmp(kind, "tool", 4);
  uint32_t ts = ev["ts"] | now_s();

  if (!strcmp(kind, "tool") || !strcmp(kind, "tool_error")) {
    tick_ts[tick_head] = ts ? ts : now_s();
    tick_head = (tick_head + 1) % TICK_N;
  }

  // merge bursts: same session, same tool, recent → bump count, move to top
  if (!strcmp(kind, "tool")) {
    for (int i = 0; i < n_items && i < MERGE_LOOKBACK; i++) {
      Item &o = items[i];
      if (!strcmp(o.kind, "tool") && !strcmp(o.sid, sid) && !strcmp(o.tool, tool) && o.sub == sub &&
          ts - o.ts <= MERGE_WINDOW) {
        Item m = o;
        m.count++;
        m.ver++;
        m.ts = ts;
        feed_sanitize(m.text, sizeof(m.text), ev["text"] | "");
        memmove(&items[1], &items[0], sizeof(Item) * i);
        items[0] = m;
        dirty = true;
        return;
      }
    }
  }

  if (n_items < FEED_MAX) n_items++;
  memmove(&items[1], &items[0], sizeof(Item) * (n_items - 1));
  Item &it = items[0];
  memset(&it, 0, sizeof(it));
  strlcpy(it.sid, sid, sizeof(it.sid));
  feed_sanitize(it.name, sizeof(it.name), ev["name"] | sid);
  strlcpy(it.kind, kind, sizeof(it.kind));
  feed_sanitize(it.tool, sizeof(it.tool), tool);
  feed_sanitize(it.text, sizeof(it.text), ev["text"] | "");
  it.ts = ts;
  it.count = 1;
  it.sub = sub;
  it.uid = next_uid++;
  dirty = true;
}

// ---------------------------------------------------------------- rendering
// Change-only setters: a rebind for a merged "x N" item must not repaint the whole row.
static void bg_if(lv_obj_t *o, lv_color_t c) {
  if (lv_obj_get_style_bg_color(o, 0).full != c.full) lv_obj_set_style_bg_color(o, c, 0);
}
static void fg_if(lv_obj_t *o, lv_color_t c) {
  if (lv_obj_get_style_text_color(o, 0).full != c.full) lv_obj_set_style_text_color(o, c, 0);
}
static void label_if(lv_obj_t *l, const char *t) {
  const char *o = lv_label_get_text(l);
  if (!o || strcmp(o, t)) lv_label_set_text(l, t);
}

static void style_row(Row &r, const Item &it) {
  FeedKindStyle st = feed_kind_style(it.kind, it.tool, it.sub);
  lv_color_t sc = feed_session_color(it.sid);
  if (lv_obj_get_style_border_color(r.obj, 0).full != sc.full) lv_obj_set_style_border_color(r.obj, sc, 0);

  bool warn = is_needs(it), err = is_err(it);
  lv_opa_t opa = (warn || err) ? LV_OPA_20 : LV_OPA_COVER;
  lv_coord_t ow = (warn || err) ? 2 : 0;
  bg_if(r.obj, warn ? C_WAITING : err ? C_ERROR : C_SURFACE);
  if (lv_obj_get_style_bg_opa(r.obj, 0) != opa) lv_obj_set_style_bg_opa(r.obj, opa, 0);
  if (ow) {
    lv_color_t oc = warn ? C_WAITING : C_ERROR;
    if (lv_obj_get_style_outline_color(r.obj, 0).full != oc.full) lv_obj_set_style_outline_color(r.obj, oc, 0);
  }
  if (lv_obj_get_style_outline_width(r.obj, 0) != ow) lv_obj_set_style_outline_width(r.obj, ow, 0);

  label_if(r.glyph, st.glyph);
  fg_if(r.glyph, st.color);
  bg_if(r.glyph, st.color);

  char l1[128], cnt[12] = "";
  uint32_t sc_hex = feed_session_color_hex(it.sid);
  char cntsec[24] = "";
  if (it.count > 1) { snprintf(cnt, sizeof(cnt), "x%u", it.count); snprintf(cntsec, sizeof(cntsec), "  #9C978E %s#", cnt); }
  const char *what;
  char whatbuf[40];
  if (!strcmp(it.kind, "tool")) {
    snprintf(whatbuf, sizeof(whatbuf), "%s%s", it.sub ? LV_SYMBOL_SHUFFLE " " : "", it.tool[0] ? it.tool : "tool");
    what = whatbuf;
  } else if (!strcmp(it.kind, "tool_error")) {
    snprintf(whatbuf, sizeof(whatbuf), "%s failed", it.tool[0] ? it.tool : "Tool");
    what = whatbuf;
  } else {
    what = st.verb;
  }
  lv_color32_t wc;
  wc.full = lv_color_to32(st.color);
  bool plain_what = !strcmp(it.kind, "tool") && !it.sub;
  if (plain_what)
    snprintf(l1, sizeof(l1), "#%06lX %s#  %s%s", (unsigned long)sc_hex, it.name, what, cntsec);
  else
    snprintf(l1, sizeof(l1), "#%06lX %s#  #%02X%02X%02X %s#%s", (unsigned long)sc_hex, it.name,
             wc.ch.red, wc.ch.green, wc.ch.blue, what, cntsec);
  label_if(r.l1, l1);

  if (!strcmp(it.kind, "prompt") && it.text[0]) {
    char q[96];
    snprintf(q, sizeof(q), "\"%s\"", it.text);
    label_if(r.l2, q);
  } else {
    label_if(r.l2, it.text);
  }
  fg_if(r.l2, warn ? C_WAITING : err ? lv_color_hex(0xF09A94) : C_MUTED);
}

// Age with a coarser cadence for older rows: 1 s steps under 15 s, then 5 s steps, then minutes...
// (each label only changes — and is only invalidated — when its displayed string changes)
static void fmt_age(char *b, size_t n, uint32_t ts, uint32_t now) {
  if (!ts || !now || ts >= now || now - ts < 5) { snprintf(b, n, "now"); return; }
  uint32_t d = now - ts;
  if (d < 15) snprintf(b, n, "%us", (unsigned)d);
  else if (d < 60) snprintf(b, n, "%us", (unsigned)(d / 5 * 5));
  else feed_fmt_ago(b, n, ts, now);
}

static void update_times() {
  uint32_t now = now_s();
  for (auto &r : rows) {
    if (r.item < 0) continue;
    char b[8];
    fmt_age(b, sizeof(b), items[r.item].ts, now);
    if (strcmp(b, r.time_txt)) {
      strlcpy(r.time_txt, b, sizeof(r.time_txt));
      lv_label_set_text(r.time, b);
    }
  }
}

static void update_rate() {
  uint32_t now = now_s();
  int total = 0, bins[12] = {0};
  for (int i = 0; i < TICK_N; i++) {
    uint32_t t = tick_ts[i];
    if (!t || t > now + 2) continue;
    uint32_t age = now >= t ? now - t : 0;
    if (age < 60) { total++; bins[11 - age / 5]++; }
  }
  if (total != last_rate) {
    last_rate = total;
    lv_label_set_text_fmt(rate_lbl, "%d", total);
  }
  int mx = 1;
  for (int b : bins) if (b > mx) mx = b;
  for (int i = 0; i < 12; i++) {
    int h = 3 + bins[i] * 21 / mx;
    int on = bins[i] ? 1 : 0;
    if (h != spark_h[i]) { spark_h[i] = h; lv_obj_set_height(spark[i], h); }
    if (on != spark_on[i]) { spark_on[i] = on; lv_obj_set_style_bg_color(spark[i], on ? C_ACCENT : C_DIM, 0); }
  }
}

static void update_chips() {
  int needs = 0, errs = 0;
  for (int i = 0; i < n_items; i++) { needs += is_needs(items[i]) || is_err(items[i]); errs += is_err(items[i]); }
  if (filter != last_filter) {
    last_filter = filter;
    for (int c = 0; c < 3; c++) {
      bool on = c == filter;
      lv_obj_set_style_bg_color(chips[c], on ? C_ACCENT : C_SURFACE, 0);
      lv_obj_set_style_text_color(chip_lbl[c], on ? C_BG : C_TEXT, 0);
    }
  }
  if (needs != last_needs) {
    last_needs = needs;
    if (needs) lv_label_set_text_fmt(chip_lbl[1], "Needs me  %d", needs);
    else lv_label_set_text(chip_lbl[1], "Needs me");
  }
  if (errs != last_errs) {
    last_errs = errs;
    if (errs) lv_label_set_text_fmt(chip_lbl[2], "Errors  %d", errs);
    else lv_label_set_text(chip_lbl[2], "Errors");
  }
}

// New-arrival cue: a short glow on the row's 40x40 glyph disc (tiny invalidated area), instead of
// sliding/fading the whole list (which repainted ~150k px per animation frame).
static void glow_anim_cb(void *o, int32_t v) { lv_obj_set_style_bg_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }
static void glow(lv_obj_t *glyph) {
  lv_anim_del(glyph, glow_anim_cb);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, glyph);
  lv_anim_set_exec_cb(&a, glow_anim_cb);
  lv_anim_set_values(&a, LV_OPA_70, LV_OPA_20);
  lv_anim_set_time(&a, 700);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

// Bind the filtered items to the row pool. Widgets stay bound to their item (by uid): a new event
// recycles one widget to the top (one list re-layout) instead of rebinding the text of all 18 rows;
// only rows whose item content changed (new / merged "x N") get their labels touched.
static void render() {
  dirty = false;
  int want[POOL], m = 0;
  for (int i = 0; i < n_items && m < POOL; i++)
    if (passes(items[i])) want[m++] = i;

  Row *slot[POOL] = {nullptr};
  bool used[POOL] = {false};
  for (int r = 0; r < POOL; r++) {
    if (!rows[r].uid) continue;
    for (int k = 0; k < m; k++)
      if (!slot[k] && items[want[k]].uid == rows[r].uid) { slot[k] = &rows[r]; used[r] = true; break; }
  }
  int free_r = 0;
  for (int k = 0; k < m; k++) {
    if (slot[k]) continue;
    while (used[free_r]) free_r++;
    used[free_r] = true;
    slot[k] = &rows[free_r];
    slot[k]->uid = 0;  // force rebind
  }

  // how many rows appeared above the previous top row (for the arrival cue / scroll compensation)
  int shift = 0;
  if (top_uid) {
    shift = m;
    for (int k = 0; k < m; k++) if (items[want[k]].uid == top_uid) { shift = k; break; }
  }

  for (int k = 0; k < m; k++) {
    Row &r = *slot[k];
    const Item &it = items[want[k]];
    r.item = want[k];
    if (r.uid != it.uid || r.ver != it.ver) {
      style_row(r, it);
      r.uid = it.uid;
      r.ver = it.ver;
      r.time_txt[0] = 0;
    }
    if (lv_obj_has_flag(r.obj, LV_OBJ_FLAG_HIDDEN)) lv_obj_clear_flag(r.obj, LV_OBJ_FLAG_HIDDEN);
    if ((int)lv_obj_get_index(r.obj) != k) lv_obj_move_to_index(r.obj, k);
  }
  for (int r = 0; r < POOL; r++) {
    if (used[r]) continue;
    rows[r].item = -1;
    rows[r].uid = 0;
    if (!lv_obj_has_flag(rows[r].obj, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(rows[r].obj, LV_OBJ_FLAG_HIDDEN);
  }
  uint32_t prev_top = top_uid;
  top_uid = m ? items[want[0]].uid : 0;

  bool empty_shown = !lv_obj_has_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
  if (m) {
    if (empty_shown) lv_obj_add_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
  } else {
    label_if(empty_lbl, filter == F_ALL ? LV_SYMBOL_LIST "\n\nNo activity yet"
                           : filter == F_NEEDS ? LV_SYMBOL_OK "\n\nNothing needs you"
                                               : LV_SYMBOL_OK "\n\nNo errors");
    if (!empty_shown) lv_obj_clear_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
  }
  update_times();
  update_chips();

  if (shift > 0 && prev_top && visible && m) {
    lv_coord_t sy = lv_obj_get_scroll_y(list);
    if (sy <= 4) {
      for (int k = 0; k < shift && k < 2; k++) glow(slot[k]->glyph);
    } else {
      // keep the user's reading position stable
      lv_obj_update_layout(list);
      lv_obj_scroll_to_y(list, sy + shift * (ROW_H + ROW_GAP), LV_ANIM_OFF);
    }
  }
}

// Render now if the user can actually see the feed; otherwise leave it dirty for later.
static bool can_render() { return visible && !detail_is_open(); }

static void render_timer_cb(lv_timer_t *) {
  render_pending = false;
  lv_timer_pause(render_timer);
  if (dirty && can_render()) render();
}

static void schedule_render() {
  if (render_pending || !visible) return;
  render_pending = true;
  lv_timer_reset(render_timer);
  lv_timer_resume(render_timer);
}

static void tick_cb(lv_timer_t *) {
  if (!can_render()) return;
  if (dirty) render();
  else update_times();
  update_rate();
}

// ---------------------------------------------------------------- events
static void row_click_cb(lv_event_t *e) {
  Row *r = (Row *)lv_event_get_user_data(e);
  if (r->item >= 0 && r->item < n_items) detail_open(items[r->item].sid);
}

static void chip_cb(lv_event_t *e) {
  int c = (int)(intptr_t)lv_event_get_user_data(e);
  if (c == filter) return;
  filter = c;
  top_uid = 0;  // no arrival cue for a filter switch
  lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
  render();
}

// ---------------------------------------------------------------- create
static lv_obj_t *mk_label(lv_obj_t *p, const lv_font_t *f, lv_color_t c) {
  lv_obj_t *l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  return l;
}

static void create(lv_obj_t *p) {
  // ---- header: title + live tools/min ticker
  lv_obj_t *t = mk_label(p, F_TITLE, C_TEXT);
  lv_label_set_text(t, "Activity");
  lv_obj_set_pos(t, 22, 18);

  lv_obj_t *sp = lv_obj_create(p);
  lv_obj_remove_style_all(sp);
  lv_obj_set_size(sp, 12 * 6, 24);
  lv_obj_set_flex_flow(sp, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(sp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(sp, 2, 0);
  lv_obj_align(sp, LV_ALIGN_TOP_RIGHT, -126, 20);
  lv_obj_clear_flag(sp, LV_OBJ_FLAG_CLICKABLE);
  for (auto &b : spark) {
    b = lv_obj_create(sp);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 4, 3);
    lv_obj_set_style_radius(b, 2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, C_DIM, 0);
  }
  rate_lbl = mk_label(p, F_TITLE, C_ACCENT);
  lv_label_set_text(rate_lbl, "0");
  lv_obj_set_width(rate_lbl, 44);
  lv_obj_set_style_text_align(rate_lbl, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(rate_lbl, LV_ALIGN_TOP_RIGHT, -72, 16);
  lv_obj_t *u = mk_label(p, F_SMALL, C_MUTED);
  lv_label_set_text(u, "tools\n/min");
  lv_obj_set_style_text_line_space(u, -2, 0);
  lv_obj_align(u, LV_ALIGN_TOP_RIGHT, -22, 16);

  // ---- filter chips
  static const int cw[3] = {88, 162, 140};
  int x = 16;
  for (int c = 0; c < 3; c++) {
    chips[c] = lv_obj_create(p);
    lv_obj_remove_style_all(chips[c]);
    lv_obj_set_size(chips[c], cw[c], 46);
    lv_obj_set_pos(chips[c], x, 58);
    x += cw[c] + 8;
    lv_obj_set_style_radius(chips[c], 23, 0);
    lv_obj_set_style_bg_opa(chips[c], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(chips[c], C_SURFACE, 0);
    lv_obj_add_flag(chips[c], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(chips[c], 4);
    lv_obj_add_event_cb(chips[c], chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)c);
    chip_lbl[c] = mk_label(chips[c], F_BODY, C_TEXT);
    lv_obj_center(chip_lbl[c]);
  }

  // ---- list (clip wrapper so the slide-in animation stays under the header)
  list_clip = lv_obj_create(p);
  lv_obj_remove_style_all(list_clip);
  lv_obj_set_size(list_clip, 480, 480 - 114 - 26);
  lv_obj_set_pos(list_clip, 0, 114);
  lv_obj_clear_flag(list_clip, LV_OBJ_FLAG_SCROLLABLE);

  list = lv_obj_create(list_clip);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, 480, LV_PCT(100));
  lv_obj_set_style_pad_hor(list, 16, 0);
  lv_obj_set_style_pad_bottom(list, 8, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, ROW_GAP, 0);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(list, C_DIM, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);

  for (auto &r : rows) {
    r.item = -1;
    r.uid = 0;
    r.ver = 0;
    r.obj = lv_obj_create(list);
    lv_obj_remove_style_all(r.obj);
    lv_obj_set_size(r.obj, LV_PCT(100), ROW_H);
    lv_obj_set_style_radius(r.obj, 12, 0);
    lv_obj_set_style_bg_opa(r.obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r.obj, C_SURFACE, 0);
    lv_obj_set_style_border_side(r.obj, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(r.obj, 6, 0);
    lv_obj_set_style_outline_pad(r.obj, -2, 0);
    lv_obj_set_style_bg_color(r.obj, C_SURFACE2, LV_STATE_PRESSED);
    lv_obj_add_flag(r.obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(r.obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r.obj, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(r.obj, row_click_cb, LV_EVENT_CLICKED, &r);
    lv_obj_add_flag(r.obj, LV_OBJ_FLAG_HIDDEN);

    r.glyph = mk_label(r.obj, &lv_font_montserrat_20, C_BUSY);
    lv_obj_set_size(r.glyph, 40, 40);
    lv_obj_set_style_radius(r.glyph, 20, 0);
    lv_obj_set_style_bg_opa(r.glyph, LV_OPA_20, 0);
    lv_obj_set_style_text_align(r.glyph, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(r.glyph, 9, 0);
    lv_obj_align(r.glyph, LV_ALIGN_LEFT_MID, 10, 0);

    r.l1 = mk_label(r.obj, &lv_font_montserrat_20, C_TEXT);
    lv_label_set_recolor(r.l1, true);
    lv_label_set_long_mode(r.l1, LV_LABEL_LONG_DOT);
    lv_obj_set_width(r.l1, 300);
    lv_obj_set_pos(r.l1, 62, 8);

    r.l2 = mk_label(r.obj, &lv_font_montserrat_16, C_MUTED);
    lv_label_set_long_mode(r.l2, LV_LABEL_LONG_DOT);
    lv_obj_set_width(r.l2, 360);
    lv_obj_set_pos(r.l2, 62, 36);

    r.time = mk_label(r.obj, F_SMALL, C_MUTED);
    lv_obj_align(r.time, LV_ALIGN_TOP_RIGHT, -12, 10);
    r.time_txt[0] = 0;
  }

  empty_lbl = mk_label(list_clip, F_BODY, C_DIM);
  lv_obj_set_style_text_align(empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(empty_lbl, LV_ALIGN_CENTER, 0, -20);

  timer = lv_timer_create(tick_cb, 1000, NULL);
  lv_timer_pause(timer);
  render_timer = lv_timer_create(render_timer_cb, RENDER_COALESCE_MS, NULL);
  lv_timer_pause(render_timer);
  for (int i = 0; i < 12; i++) { spark_h[i] = 3; spark_on[i] = 0; }
  lv_label_set_text(chip_lbl[0], "All");
  render();
}

static void update(JsonObjectConst) {
  if (dirty && can_render()) render();
}

static void on_event(JsonObjectConst ev) {
  ingest(ev);  // hidden tile: just record
  if (dirty) schedule_render();
}

static void on_show(bool v) {
  visible = v;
  if (v) {
    top_uid = 0;  // no arrival cue for what piled up while hidden
    lv_timer_resume(timer);
    if (dirty && can_render()) render();
    update_times();
    update_rate();
  } else {
    lv_timer_pause(timer);
    lv_timer_pause(render_timer);
    render_pending = false;
  }
}

const ScreenDef screen_feed = {"feed", "Activity", create, update, on_event, on_show};
