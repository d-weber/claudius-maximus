// Session detail overlay (owner: detail-feed). Full-screen panel above the tileview.
// Open: detail_open(sid) from anywhere. Close: back button, or pull the content down past the top.
#include "screens.h"
#include "feed_common.h"
#include "detail_api.h"
#include "../ui/theme.h"
#include "../core/model.h"
#include "../core/link.h"
#include <string.h>
#include <stdio.h>

#define SUB_POOL 5
#define EV_POOL 10
#define TODO_BOXES 12

static lv_obj_t *panel, *body;
static lv_obj_t *hdr_name, *hdr_title, *hdr_bar, *focus_btn, *focus_lbl;
static lv_obj_t *pill, *pill_lbl, *started_lbl;
static lv_obj_t *wait_card, *wait_text;
static lv_obj_t *act_cap, *act_ago, *act_glyph, *act_tool, *act_detail;
static lv_obj_t *met_val[6], *met_cap[6], *ctx_bar;
static lv_obj_t *prompt_card, *prompt_lbl;
static lv_obj_t *todo_card, *todo_cap, *todo_bar, *todo_boxes[TODO_BOXES], *todo_cur;
static lv_obj_t *sub_card, *sub_cap;
static lv_obj_t *ev_card, *ev_empty;
static lv_obj_t *gone_lbl;
struct SubRow { lv_obj_t *obj, *dot, *desc, *meta; };
static SubRow subs[SUB_POOL];
struct EvRow { lv_obj_t *obj, *glyph, *txt, *ago; uint32_t ts; };
static EvRow evs[EV_POOL];
static lv_timer_t *timer;

static char cur_sid[16];
static bool is_open = false;
static uint32_t st_since = 0, st_started = 0, act_ts = 0;
static uint32_t focus_flash_until = 0;

// ------------------------------------------------------------ small builders
static lv_obj_t *mk_label(lv_obj_t *p, const lv_font_t *f, lv_color_t c) {
  lv_obj_t *l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  return l;
}
static lv_obj_t *mk_box(lv_obj_t *p) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
  return o;
}
static lv_obj_t *mk_card(lv_obj_t *p) {
  lv_obj_t *c = mk_box(p);
  lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(c, C_SURFACE, 0);
  lv_obj_set_style_radius(c, 14, 0);
  lv_obj_set_style_pad_all(c, 14, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(c, 8, 0);
  return c;
}
static lv_obj_t *mk_caption(lv_obj_t *p, const char *t) {
  lv_obj_t *l = mk_label(p, F_SMALL, C_MUTED);
  lv_obj_set_style_text_letter_space(l, 1, 0);
  lv_label_set_text(l, t);
  return l;
}
static void set_text_if(lv_obj_t *l, const char *t) {
  const char *o = lv_label_get_text(l);
  if (!o || strcmp(o, t)) lv_label_set_text(l, t);
}
static void show(lv_obj_t *o, bool v) {
  if (v) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}
static const char *status_word(const char *s) {
  if (!strcmp(s, "busy")) return "WORKING";
  if (!strcmp(s, "waiting")) return "NEEDS YOU";
  if (!strcmp(s, "idle")) return "IDLE";
  if (!strcmp(s, "offline")) return "OFFLINE";
  return "UNKNOWN";
}

// ------------------------------------------------------------ callbacks
static void back_cb(lv_event_t *) { detail_close(); }
static void focus_cb(lv_event_t *) {
  if (!cur_sid[0]) return;
  link_action("focus", cur_sid);
  focus_flash_until = lv_tick_get() + 1500;
  lv_label_set_text(focus_lbl, LV_SYMBOL_OK " Sent");
}
static void body_release_cb(lv_event_t *) {
  // pull-down-to-close: elastic overscroll past the top
  if (lv_obj_get_scroll_y(body) < -70) detail_close();
}
static void panel_gesture_cb(lv_event_t *) {
  lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (d == LV_DIR_BOTTOM) detail_close();
}

// ------------------------------------------------------------ time-based refresh
static void refresh_times() {
  uint32_t now = model_now();
  char b[32], d[24];
  if (st_since && now >= st_since) feed_fmt_dur(d, sizeof(d), now - st_since);
  else strcpy(d, "-");
  const char *txt = lv_label_get_text(pill_lbl);
  // pill text = "<WORD>  <dur>"; rebuild from stored word in user data
  const char *word = (const char *)lv_obj_get_user_data(pill_lbl);
  snprintf(b, sizeof(b), "%s  %s", word ? word : "", d);
  if (!txt || strcmp(txt, b)) lv_label_set_text(pill_lbl, b);

  if (st_started && now >= st_started) {
    feed_fmt_dur(d, sizeof(d), now - st_started);
    set_text_if(met_val[5], d);
  }
  feed_fmt_ago(b, sizeof(b), act_ts, now);
  if (strcmp(b, "now")) { char x[24]; snprintf(x, sizeof(x), "%s ago", b); set_text_if(act_ago, x); }
  else set_text_if(act_ago, "just now");
  for (auto &e : evs) {
    if (lv_obj_has_flag(e.obj, LV_OBJ_FLAG_HIDDEN)) continue;
    feed_fmt_ago(b, sizeof(b), e.ts, now);
    set_text_if(e.ago, b);
  }
  if (focus_flash_until && (int32_t)(lv_tick_get() - focus_flash_until) > 0) {
    focus_flash_until = 0;
    lv_label_set_text(focus_lbl, LV_SYMBOL_EYE_OPEN " Focus");
  }
}
static void timer_cb(lv_timer_t *) { if (is_open) refresh_times(); }

// ------------------------------------------------------------ recent events of this session
static void refresh_events() {
  int shown = 0;
  const char *wait_txt = nullptr;
  uint32_t wait_ts = 0;
  for (int i = (int)model_event_count() - 1; i >= 0; i--) {
    JsonObjectConst ev = model_event(i);
    if (strcmp(ev["sid"] | "", cur_sid)) continue;
    const char *kind = ev["kind"] | "";
    if (!wait_txt && !strcmp(kind, "waiting")) { wait_txt = ev["text"] | ""; wait_ts = ev["ts"] | 0; }
    if (shown >= EV_POOL) { if (wait_txt) break; continue; }
    EvRow &r = evs[shown++];
    const char *tool = ev["tool"] | "";
    bool sub = !ev["agent"].isNull() && !strncmp(kind, "tool", 4);
    FeedKindStyle st = feed_kind_style(kind, tool, sub);
    set_text_if(r.glyph, st.glyph);
    lv_obj_set_style_text_color(r.glyph, st.color, 0);
    char t[96], clean[84];
    feed_sanitize(clean, sizeof(clean), ev["text"] | "");
    const char *what = st.verb ? st.verb : tool;
    if (!strcmp(kind, "tool_error")) snprintf(t, sizeof(t), "%s failed  #9C978E %s#", tool, clean);
    else snprintf(t, sizeof(t), "%s%s  #9C978E %s#", sub ? "sub " : "", what, clean);
    set_text_if(r.txt, t);
    lv_obj_set_style_text_color(r.txt, !strcmp(kind, "waiting") ? C_WAITING
                                       : !strcmp(kind, "tool_error") ? C_ERROR : C_TEXT, 0);
    r.ts = ev["ts"] | 0;
    show(r.obj, true);
  }
  for (int i = shown; i < EV_POOL; i++) show(evs[i].obj, false);
  show(ev_empty, shown == 0);
  // waiting banner text: latest waiting event (if newer than status change), else activity detail
  if (wait_txt && wait_ts + 2 >= st_since) {
    char clean[120];
    feed_sanitize(clean, sizeof(clean), wait_txt);
    set_text_if(wait_text, clean[0] ? clean : "Claude is waiting for you in the terminal.");
  }
}

// ------------------------------------------------------------ create
void detail_create(lv_obj_t *scr) {
  panel = lv_obj_create(scr);
  lv_obj_remove_style_all(panel);
  lv_obj_set_size(panel, 480, 480);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(panel, C_BG, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(panel, panel_gesture_cb, LV_EVENT_GESTURE, NULL);

  // ---- header (fixed)
  lv_obj_t *back = lv_btn_create(panel);
  lv_obj_remove_style_all(back);
  lv_obj_set_size(back, 60, 60);
  lv_obj_set_pos(back, 12, 12);
  lv_obj_set_style_radius(back, 30, 0);
  lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(back, C_SURFACE, 0);
  lv_obj_set_style_bg_color(back, C_SURFACE2, LV_STATE_PRESSED);
  lv_obj_set_ext_click_area(back, 10);
  lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = mk_label(back, F_TITLE, C_TEXT);
  lv_label_set_text(bl, LV_SYMBOL_LEFT);
  lv_obj_center(bl);

  hdr_bar = mk_box(panel);  // session identity stripe
  lv_obj_set_size(hdr_bar, 4, 50);
  lv_obj_set_pos(hdr_bar, 84, 17);
  lv_obj_set_style_radius(hdr_bar, 2, 0);
  lv_obj_set_style_bg_opa(hdr_bar, LV_OPA_COVER, 0);

  hdr_name = mk_label(panel, F_TITLE, C_TEXT);
  lv_label_set_long_mode(hdr_name, LV_LABEL_LONG_DOT);
  lv_obj_set_width(hdr_name, 238);
  lv_obj_set_pos(hdr_name, 96, 14);
  hdr_title = mk_label(panel, &lv_font_montserrat_16, C_MUTED);
  lv_label_set_long_mode(hdr_title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(hdr_title, 238);
  lv_obj_set_pos(hdr_title, 96, 46);

  focus_btn = lv_btn_create(panel);
  lv_obj_remove_style_all(focus_btn);
  lv_obj_set_size(focus_btn, 120, 60);
  lv_obj_align(focus_btn, LV_ALIGN_TOP_RIGHT, -12, 12);
  lv_obj_set_style_radius(focus_btn, 30, 0);
  lv_obj_set_style_bg_opa(focus_btn, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(focus_btn, C_ACCENT, 0);
  lv_obj_set_style_bg_color(focus_btn, C_ACCENT2, LV_STATE_PRESSED);
  lv_obj_add_event_cb(focus_btn, focus_cb, LV_EVENT_CLICKED, NULL);
  focus_lbl = mk_label(focus_btn, F_BODY, C_BG);
  lv_label_set_text(focus_lbl, LV_SYMBOL_EYE_OPEN " Focus");
  lv_obj_center(focus_lbl);

  // ---- scrolling body
  body = lv_obj_create(panel);
  lv_obj_remove_style_all(body);
  lv_obj_set_size(body, 480, 480 - 84);
  lv_obj_set_pos(body, 0, 84);
  lv_obj_set_style_pad_hor(body, 14, 0);
  lv_obj_set_style_pad_top(body, 2, 0);
  lv_obj_set_style_pad_bottom(body, 30, 0);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(body, 10, 0);
  lv_obj_set_scroll_dir(body, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(body, C_DIM, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(body, 4, LV_PART_SCROLLBAR);
  lv_obj_add_event_cb(body, body_release_cb, LV_EVENT_RELEASED, NULL);

  // status row
  lv_obj_t *srow = mk_box(body);
  lv_obj_set_size(srow, LV_PCT(100), 44);
  pill = mk_box(srow);
  lv_obj_set_size(pill, LV_SIZE_CONTENT, 40);
  lv_obj_set_style_radius(pill, 20, 0);
  lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(pill, 16, 0);
  lv_obj_align(pill, LV_ALIGN_LEFT_MID, 0, 0);
  pill_lbl = mk_label(pill, &lv_font_montserrat_20, C_BG);
  lv_obj_align(pill_lbl, LV_ALIGN_LEFT_MID, 0, 0);
  lv_label_set_text(pill_lbl, "");
  started_lbl = mk_label(srow, &lv_font_montserrat_16, C_MUTED);
  lv_obj_align(started_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

  // waiting banner
  wait_card = mk_card(body);
  lv_obj_set_style_bg_color(wait_card, C_WAITING, 0);
  lv_obj_t *wt = mk_label(wait_card, F_TITLE, C_BG);
  lv_label_set_text(wt, LV_SYMBOL_BELL "  Needs your input");
  wait_text = mk_label(wait_card, F_BODY, C_BG);
  lv_label_set_long_mode(wait_text, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(wait_text, LV_PCT(100));
  lv_label_set_text(wait_text, "");

  // current activity
  lv_obj_t *ac = mk_card(body);
  lv_obj_t *acrow = mk_box(ac);
  lv_obj_set_size(acrow, LV_PCT(100), LV_SIZE_CONTENT);
  act_cap = mk_caption(acrow, "NOW");
  act_ago = mk_label(acrow, F_SMALL, C_MUTED);
  lv_obj_align(act_ago, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_t *arow = mk_box(ac);
  lv_obj_set_size(arow, LV_PCT(100), 56);
  act_glyph = mk_label(arow, F_TITLE, C_BUSY);
  lv_obj_set_size(act_glyph, 56, 56);
  lv_obj_set_style_radius(act_glyph, 28, 0);
  lv_obj_set_style_bg_opa(act_glyph, LV_OPA_20, 0);
  lv_obj_set_style_text_align(act_glyph, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(act_glyph, 14, 0);
  act_tool = mk_label(arow, F_LARGE, C_BUSY);
  lv_label_set_long_mode(act_tool, LV_LABEL_LONG_DOT);
  lv_obj_set_width(act_tool, 340);
  lv_obj_align(act_tool, LV_ALIGN_LEFT_MID, 72, 0);
  act_detail = mk_label(ac, F_BODY, C_TEXT);
  lv_label_set_long_mode(act_detail, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(act_detail, LV_PCT(100));

  // metrics grid (3 x 2)
  lv_obj_t *grid = mk_box(body);
  lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(grid, 8, 0);
  lv_obj_set_style_pad_column(grid, 8, 0);
  static const char *caps[6] = {"TOKENS", "COST", "CONTEXT", "TOOLS", "MODEL", "UPTIME"};
  for (int i = 0; i < 6; i++) {
    lv_obj_t *t = mk_box(grid);
    lv_obj_set_size(t, 145, 76);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t, C_SURFACE, 0);
    lv_obj_set_style_radius(t, 12, 0);
    lv_obj_set_style_pad_all(t, 10, 0);
    met_val[i] = mk_label(t, i == 4 ? F_BODY : F_TITLE, C_TEXT);
    lv_label_set_long_mode(met_val[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(met_val[i], 125);
    lv_obj_align(met_val[i], LV_ALIGN_TOP_LEFT, 0, i == 4 ? 4 : 0);
    lv_label_set_text(met_val[i], "-");
    met_cap[i] = mk_caption(t, caps[i]);
    lv_obj_align(met_cap[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    if (i == 2) {
      ctx_bar = lv_bar_create(t);
      lv_obj_set_size(ctx_bar, 125, 6);
      lv_obj_align(ctx_bar, LV_ALIGN_TOP_LEFT, 0, 32);
      lv_obj_set_style_bg_color(ctx_bar, C_SURFACE2, 0);
      lv_obj_set_style_bg_opa(ctx_bar, LV_OPA_COVER, 0);
      lv_bar_set_range(ctx_bar, 0, 100);
    }
  }

  // last prompt
  prompt_card = mk_card(body);
  mk_caption(prompt_card, "LAST PROMPT");
  prompt_lbl = mk_label(prompt_card, F_BODY, C_TEXT);
  lv_label_set_long_mode(prompt_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(prompt_lbl, LV_PCT(100));
  lv_obj_set_style_border_side(prompt_lbl, LV_BORDER_SIDE_LEFT, 0);
  lv_obj_set_style_border_width(prompt_lbl, 3, 0);
  lv_obj_set_style_border_color(prompt_lbl, C_INFO, 0);
  lv_obj_set_style_pad_left(prompt_lbl, 10, 0);

  // todos
  todo_card = mk_card(body);
  todo_cap = mk_caption(todo_card, "TODOS");
  lv_obj_t *boxes = mk_box(todo_card);
  lv_obj_set_size(boxes, LV_PCT(100), 26);
  lv_obj_set_flex_flow(boxes, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(boxes, 6, 0);
  for (auto &b : todo_boxes) {
    b = mk_box(boxes);
    lv_obj_set_size(b, 26, 26);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_t *ck = mk_label(b, F_SMALL, C_BG);
    lv_label_set_text(ck, LV_SYMBOL_OK);
    lv_obj_center(ck);
  }
  todo_bar = lv_bar_create(todo_card);  // used when there are too many todos for boxes
  lv_obj_set_size(todo_bar, LV_PCT(100), 10);
  lv_obj_set_style_bg_color(todo_bar, C_SURFACE2, 0);
  lv_obj_set_style_bg_opa(todo_bar, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(todo_bar, C_IDLE, LV_PART_INDICATOR);
  todo_cur = mk_label(todo_card, F_BODY, C_TEXT);
  lv_label_set_long_mode(todo_cur, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(todo_cur, LV_PCT(100));

  // subagents
  sub_card = mk_card(body);
  sub_cap = mk_caption(sub_card, "SUBAGENTS");
  for (auto &s : subs) {
    s.obj = mk_box(sub_card);
    lv_obj_set_size(s.obj, LV_PCT(100), 50);
    s.dot = mk_box(s.obj);
    lv_obj_set_size(s.dot, 12, 12);
    lv_obj_set_style_radius(s.dot, 6, 0);
    lv_obj_set_style_bg_opa(s.dot, LV_OPA_COVER, 0);
    lv_obj_align(s.dot, LV_ALIGN_TOP_LEFT, 2, 6);
    s.desc = mk_label(s.obj, F_BODY, C_TEXT);
    lv_label_set_long_mode(s.desc, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s.desc, 400);
    lv_obj_set_pos(s.desc, 24, 0);
    s.meta = mk_label(s.obj, F_SMALL, C_MUTED);
    lv_label_set_long_mode(s.meta, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s.meta, 400);
    lv_obj_set_pos(s.meta, 24, 26);
  }

  // recent events
  ev_card = mk_card(body);
  mk_caption(ev_card, "RECENT");
  lv_obj_set_style_pad_row(ev_card, 4, 0);
  for (auto &e : evs) {
    e.obj = mk_box(ev_card);
    lv_obj_set_size(e.obj, LV_PCT(100), 32);
    e.glyph = mk_label(e.obj, F_BODY, C_BUSY);
    lv_obj_set_width(e.glyph, 28);
    lv_obj_set_style_text_align(e.glyph, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(e.glyph, LV_ALIGN_LEFT_MID, 0, 0);
    e.txt = mk_label(e.obj, &lv_font_montserrat_16, C_TEXT);
    lv_label_set_recolor(e.txt, true);
    lv_label_set_long_mode(e.txt, LV_LABEL_LONG_DOT);
    lv_obj_set_width(e.txt, 330);
    lv_obj_align(e.txt, LV_ALIGN_LEFT_MID, 36, 0);
    e.ago = mk_label(e.obj, F_SMALL, C_MUTED);
    lv_obj_align(e.ago, LV_ALIGN_RIGHT_MID, 0, 0);
    e.ts = 0;
  }
  ev_empty = mk_label(ev_card, &lv_font_montserrat_16, C_DIM);
  lv_label_set_text(ev_empty, "No events yet");

  gone_lbl = mk_label(body, F_BODY, C_MUTED);
  lv_label_set_text(gone_lbl, LV_SYMBOL_POWER "  Session is no longer reported by the host.");
  lv_label_set_long_mode(gone_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(gone_lbl, LV_PCT(100));

  timer = lv_timer_create(timer_cb, 1000, NULL);
  lv_timer_pause(timer);
}

// ------------------------------------------------------------ open / close
static void slide_cb(void *o, int32_t v) { lv_obj_set_y((lv_obj_t *)o, v); }

void detail_open(const char *sid) {
  if (!sid || !*sid) return;
  bool was_open = is_open;
  if (strcmp(cur_sid, sid)) lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
  strlcpy(cur_sid, sid, sizeof(cur_sid));
  focus_flash_until = 0;
  lv_label_set_text(focus_lbl, LV_SYMBOL_EYE_OPEN " Focus");
  is_open = true;
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(panel);
  detail_update(model_state());
  refresh_events();
  lv_timer_resume(timer);
  if (!was_open) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, panel);
    lv_anim_set_exec_cb(&a, slide_cb);
    lv_anim_set_values(&a, 120, 0);
    lv_anim_set_time(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
  }
}

void detail_close() {
  if (!panel) return;
  is_open = false;
  lv_anim_del(panel, slide_cb);
  lv_obj_set_y(panel, 0);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
  if (timer) lv_timer_pause(timer);
}

bool detail_is_open() { return is_open; }

// ------------------------------------------------------------ data → widgets
void detail_update(JsonObjectConst) {
  if (!is_open) return;
  JsonObjectConst s = model_find_session(cur_sid);
  bool gone = s.isNull();
  show(gone_lbl, gone);
  if (gone) {
    lv_obj_set_user_data(pill_lbl, (void *)"GONE");
    lv_obj_set_style_bg_color(pill, C_OFFLINE, 0);
    set_text_if(hdr_name, cur_sid);
    show(wait_card, false);
    refresh_times();
    return;
  }

  char buf[160];
  lv_obj_set_style_bg_color(hdr_bar, feed_session_color(cur_sid), 0);
  feed_sanitize(buf, sizeof(buf), s["project"] | (s["name"] | cur_sid));
  set_text_if(hdr_name, buf);
  feed_sanitize(buf, sizeof(buf), s["title"] | (s["cwd"] | ""));
  set_text_if(hdr_title, buf);

  const char *status = s["status"] | "offline";
  lv_color_t sc = status_color(status);
  lv_obj_set_user_data(pill_lbl, (void *)status_word(status));  // static strings
  lv_obj_set_style_bg_color(pill, sc, 0);
  st_since = s["since"] | 0;
  st_started = s["started"] | 0;
  if (st_started) {
    time_t t = (time_t)(st_started + model_tz());
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(buf, sizeof(buf), "started %02d:%02d", tmv.tm_hour, tmv.tm_min);
    set_text_if(started_lbl, buf);
  } else set_text_if(started_lbl, "");

  // waiting banner
  bool waiting = !strcmp(status, "waiting");
  show(wait_card, waiting);
  if (waiting && !lv_label_get_text(wait_text)[0]) {
    feed_sanitize(buf, sizeof(buf), s["activity"]["detail"] | "Claude is waiting for you in the terminal.");
    lv_label_set_text(wait_text, buf);
  }
  if (!waiting) lv_label_set_text(wait_text, "");

  // activity
  JsonObjectConst act = s["activity"];
  const char *tool = act["tool"] | "";
  act_ts = act["ts"] | 0;
  bool busy = !strcmp(status, "busy");
  lv_color_t ac = busy ? C_BUSY : waiting ? C_WAITING : C_MUTED;
  set_text_if(act_cap, busy ? "NOW" : waiting ? "BLOCKED ON" : "LAST ACTIVITY");
  if (!strcmp(status, "idle")) {
    lv_label_set_text(act_glyph, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(act_glyph, C_IDLE, 0);
    lv_obj_set_style_bg_color(act_glyph, C_IDLE, 0);
    set_text_if(act_tool, "Ready");
    lv_obj_set_style_text_color(act_tool, C_IDLE, 0);
    feed_sanitize(buf, sizeof(buf), *tool ? (act["detail"] | "") : "");
    char x[200];
    snprintf(x, sizeof(x), "Waiting for your next prompt.%s%s%s", *tool ? "\nLast: " : "", tool, *buf ? "  " : "");
    strlcat(x, buf, sizeof(x));
    set_text_if(act_detail, x);
  } else {
    lv_label_set_text(act_glyph, feed_tool_glyph(tool));
    lv_obj_set_style_text_color(act_glyph, ac, 0);
    lv_obj_set_style_bg_color(act_glyph, ac, 0);
    set_text_if(act_tool, *tool ? tool : "Thinking");
    lv_obj_set_style_text_color(act_tool, ac, 0);
    feed_sanitize(buf, sizeof(buf), act["detail"] | "");
    set_text_if(act_detail, buf);
  }
  show(act_detail, lv_label_get_text(act_detail)[0] != 0);

  // metrics
  JsonObjectConst tk = s["tokens"];
  double tokens = (double)(tk["in"] | 0) + (double)(tk["out"] | 0) + (double)(tk["cache_read"] | 0) +
                  (double)(tk["cache_write"] | 0);
  set_text_if(met_val[0], feed_fmt_num(buf, sizeof(buf), tokens));
  snprintf(buf, sizeof(buf), "$%.2f", (double)(s["cost"] | 0.0));
  set_text_if(met_val[1], buf);
  int ctx = s["ctx_pct"] | 0;
  snprintf(buf, sizeof(buf), "%d%%", ctx);
  set_text_if(met_val[2], buf);
  lv_color_t cc = ctx >= 85 ? C_ERROR : ctx >= 70 ? C_WAITING : C_INFO;
  lv_obj_set_style_text_color(met_val[2], ctx >= 70 ? cc : C_TEXT, 0);
  lv_bar_set_value(ctx_bar, ctx, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(ctx_bar, cc, LV_PART_INDICATOR);
  snprintf(buf, sizeof(buf), "%d", (int)(s["tool_count"] | 0));
  set_text_if(met_val[3], buf);
  feed_sanitize(buf, sizeof(buf), s["model"] | "-");
  set_text_if(met_val[4], buf);

  // last prompt
  const char *lp = s["last_prompt"] | "";
  show(prompt_card, *lp != 0);
  if (*lp) {
    char clean[140];
    feed_sanitize(clean, sizeof(clean), lp);
    snprintf(buf, sizeof(buf), "%s", clean);
    set_text_if(prompt_lbl, buf);
  }

  // todos
  JsonObjectConst td = s["todos"];
  int total = td["total"] | 0, done = td["done"] | 0;
  show(todo_card, total > 0);
  if (total > 0) {
    snprintf(buf, sizeof(buf), "TODOS  %d / %d", done, total);
    set_text_if(todo_cap, buf);
    bool use_boxes = total <= TODO_BOXES;
    show(lv_obj_get_parent(todo_boxes[0]), use_boxes);
    show(todo_bar, !use_boxes);
    for (int i = 0; i < TODO_BOXES; i++) {
      lv_obj_t *b = todo_boxes[i];
      show(b, i < total);
      bool d = i < done, cur = i == done;
      lv_obj_set_style_bg_color(b, d ? C_IDLE : C_SURFACE, 0);
      lv_obj_set_style_border_color(b, d ? C_IDLE : cur ? C_ACCENT : C_BORDER, 0);
      show(lv_obj_get_child(b, 0), d);
    }
    lv_bar_set_range(todo_bar, 0, total);
    lv_bar_set_value(todo_bar, done, LV_ANIM_OFF);
    const char *cur = td["current"] | "";
    if (done >= total) set_text_if(todo_cur, LV_SYMBOL_OK "  All done");
    else {
      char clean[120];
      feed_sanitize(clean, sizeof(clean), cur);
      snprintf(buf, sizeof(buf), LV_SYMBOL_RIGHT "  %s", clean);
      set_text_if(todo_cur, buf);
    }
  }

  // subagents
  JsonArrayConst sa = s["subagents"];
  int n = 0, active = 0;
  uint32_t now = model_now();
  for (JsonObjectConst a : sa) {
    bool b = !strcmp(a["status"] | "", "busy");
    active += b;
    if (n >= SUB_POOL) continue;
    SubRow &r = subs[n++];
    lv_obj_set_style_bg_color(r.dot, b ? C_SUBAGENT : C_DIM, 0);
    feed_sanitize(buf, sizeof(buf), a["desc"] | (a["type"] | "subagent"));
    set_text_if(r.desc, buf);
    lv_obj_set_style_text_color(r.desc, b ? C_TEXT : C_MUTED, 0);
    char tok[16], dur[16], det[64];
    feed_fmt_num(tok, sizeof(tok), (double)(a["tokens"] | 0));
    uint32_t st = a["started"] | 0;
    feed_fmt_dur(dur, sizeof(dur), st && now > st ? now - st : 0);
    feed_sanitize(det, sizeof(det), a["detail"] | "");
    if (b)
      snprintf(buf, sizeof(buf), "#B392F0 %s#  %s  " LV_SYMBOL_BULLET "  %s tok  " LV_SYMBOL_BULLET "  %s", a["tool"] | "...", det, tok, dur);
    else
      snprintf(buf, sizeof(buf), LV_SYMBOL_OK " done  " LV_SYMBOL_BULLET "  %s tok", tok);
    lv_label_set_recolor(r.meta, true);
    set_text_if(r.meta, buf);
    show(r.obj, true);
  }
  for (int i = n; i < SUB_POOL; i++) show(subs[i].obj, false);
  show(sub_card, n > 0);
  if (n) {
    snprintf(buf, sizeof(buf), "SUBAGENTS  %d active / %d", active, (int)sa.size());
    set_text_if(sub_cap, buf);
  }

  refresh_events();
  refresh_times();
#ifdef DETAIL_DEBUG_SCROLL
  lv_obj_update_layout(body);
  lv_obj_scroll_to_y(body, DETAIL_DEBUG_SCROLL, LV_ANIM_OFF);
#endif
}

const char *detail_sid() { return is_open ? cur_sid : ""; }

void detail_on_event(JsonObjectConst ev) {
  if (!is_open) return;
  if (strcmp(ev["sid"] | "", cur_sid)) return;
  refresh_events();
}
