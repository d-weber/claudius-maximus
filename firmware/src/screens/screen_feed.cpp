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
  bool sub;
};

static Item items[FEED_MAX];
static int n_items = 0;

#define TICK_N 128           // timestamps of recent tool calls (tools/min)
static uint32_t tick_ts[TICK_N];
static int tick_head = 0;

// ---------------------------------------------------------------- widgets
struct Row {
  lv_obj_t *obj, *glyph, *l1, *l2, *time;
  int item;                  // index into items[] currently bound (-1 none)
  char time_txt[8];
};
static Row rows[POOL];
static lv_obj_t *list_clip, *list, *empty_lbl, *rate_lbl, *spark[12], *chips[3], *chip_lbl[3];
static lv_timer_t *timer;
static int filter = F_ALL;
static bool visible = false, dirty = true;
static int pending_new = 0;  // new items since last render (for slide-in)

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
        m.ts = ts;
        feed_sanitize(m.text, sizeof(m.text), ev["text"] | "");
        memmove(&items[1], &items[0], sizeof(Item) * i);
        items[0] = m;
        if (i > 0) pending_new++;
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
  pending_new++;
  dirty = true;
}

// ---------------------------------------------------------------- rendering
static void style_row(Row &r, const Item &it) {
  FeedKindStyle st = feed_kind_style(it.kind, it.tool, it.sub);
  lv_color_t sc = feed_session_color(it.sid);
  lv_obj_set_style_border_color(r.obj, sc, 0);

  bool warn = is_needs(it), err = is_err(it);
  if (warn || err) {
    lv_obj_set_style_bg_color(r.obj, warn ? C_WAITING : C_ERROR, 0);
    lv_obj_set_style_bg_opa(r.obj, LV_OPA_20, 0);
    lv_obj_set_style_outline_color(r.obj, warn ? C_WAITING : C_ERROR, 0);
    lv_obj_set_style_outline_width(r.obj, 2, 0);
  } else {
    lv_obj_set_style_bg_color(r.obj, C_SURFACE, 0);
    lv_obj_set_style_bg_opa(r.obj, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(r.obj, 0, 0);
  }

  lv_label_set_text(r.glyph, st.glyph);
  lv_obj_set_style_text_color(r.glyph, st.color, 0);
  lv_obj_set_style_bg_color(r.glyph, st.color, 0);

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
  lv_label_set_text(r.l1, l1);

  if (!strcmp(it.kind, "prompt") && it.text[0]) {
    char q[96];
    snprintf(q, sizeof(q), "\"%s\"", it.text);
    lv_label_set_text(r.l2, q);
  } else {
    lv_label_set_text(r.l2, it.text);
  }
  lv_obj_set_style_text_color(r.l2, warn ? C_WAITING : err ? lv_color_hex(0xF09A94) : C_MUTED, 0);
}

static void update_times() {
  uint32_t now = now_s();
  for (auto &r : rows) {
    if (r.item < 0) continue;
    char b[8];
    feed_fmt_ago(b, sizeof(b), items[r.item].ts, now);
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
  lv_label_set_text_fmt(rate_lbl, "%d", total);
  int mx = 1;
  for (int b : bins) if (b > mx) mx = b;
  for (int i = 0; i < 12; i++) {
    int h = 3 + bins[i] * 21 / mx;
    lv_obj_set_height(spark[i], h);
    lv_obj_set_style_bg_color(spark[i], bins[i] ? C_ACCENT : C_DIM, 0);
  }
}

static void update_chips() {
  int needs = 0, errs = 0;
  for (int i = 0; i < n_items; i++) { needs += is_needs(items[i]) || is_err(items[i]); errs += is_err(items[i]); }
  for (int c = 0; c < 3; c++) {
    bool on = c == filter;
    lv_obj_set_style_bg_color(chips[c], on ? C_ACCENT : C_SURFACE, 0);
    lv_obj_set_style_text_color(chip_lbl[c], on ? C_BG : C_TEXT, 0);
  }
  lv_label_set_text(chip_lbl[0], "All");
  if (needs) lv_label_set_text_fmt(chip_lbl[1], "Needs me  %d", needs);
  else lv_label_set_text(chip_lbl[1], "Needs me");
  if (errs) lv_label_set_text_fmt(chip_lbl[2], "Errors  %d", errs);
  else lv_label_set_text(chip_lbl[2], "Errors");
}

static void slide_anim_cb(void *o, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t *)o, v, 0); }
static void fade_anim_cb(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }

static void render() {
  dirty = false;
  int bound = 0;
  for (int i = 0; i < n_items && bound < POOL; i++) {
    if (!passes(items[i])) continue;
    Row &r = rows[bound++];
    r.item = i;
    r.time_txt[0] = 0;
    style_row(r, items[i]);
    lv_obj_clear_flag(r.obj, LV_OBJ_FLAG_HIDDEN);
  }
  for (int i = bound; i < POOL; i++) {
    rows[i].item = -1;
    lv_obj_add_flag(rows[i].obj, LV_OBJ_FLAG_HIDDEN);
  }
  if (bound) lv_obj_add_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
  else {
    lv_label_set_text(empty_lbl, filter == F_ALL ? LV_SYMBOL_LIST "\n\nNo activity yet"
                                 : filter == F_NEEDS ? LV_SYMBOL_OK "\n\nNothing needs you"
                                                     : LV_SYMBOL_OK "\n\nNo errors");
    lv_obj_clear_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
  }
  update_times();
  update_chips();

  // slide new rows in at the top (only when the user is looking at the top)
  int n_new = pending_new;
  pending_new = 0;
  if (n_new > 0 && visible && bound) {
    lv_coord_t sy = lv_obj_get_scroll_y(list);
    if (sy <= 4) {
      int dist = (n_new > 2 ? 2 : n_new) * (ROW_H + ROW_GAP);
      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a, list);
      lv_anim_set_exec_cb(&a, slide_anim_cb);
      lv_anim_set_values(&a, -dist, 0);
      lv_anim_set_time(&a, 260);
      lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
      lv_anim_start(&a);
      lv_anim_t f;
      lv_anim_init(&f);
      lv_anim_set_var(&f, rows[0].obj);
      lv_anim_set_exec_cb(&f, fade_anim_cb);
      lv_anim_set_values(&f, LV_OPA_TRANSP, LV_OPA_COVER);
      lv_anim_set_time(&f, 320);
      lv_anim_start(&f);
    } else {
      // keep the user's reading position stable
      lv_obj_update_layout(list);
      lv_obj_scroll_to_y(list, sy + n_new * (ROW_H + ROW_GAP), LV_ANIM_OFF);
    }
  }
}

static void tick_cb(lv_timer_t *) {
  if (!visible) return;
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
  pending_new = 0;
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
  render();
}

static void update(JsonObjectConst) {
  if (dirty) render();
  update_rate();
}

static void on_event(JsonObjectConst ev) {
  ingest(ev);
  if (visible) render();
}

static void on_show(bool v) {
  visible = v;
  if (v) {
    pending_new = 0;
    lv_timer_resume(timer);
    if (dirty) render();
    update_times();
    update_rate();
  } else {
    lv_timer_pause(timer);
  }
}

const ScreenDef screen_feed = {"feed", "Activity", create, update, on_event, on_show};
