// AGENTS overview — every Claude Code session at a glance (owner: agents).
//
// Adaptive layout (cards are pooled and updated in place, never rebuilt):
//   1 session   -> one big rich card (activity, todos, helpers / last prompt, ctx, tools, cost)
//   2 sessions  -> two wide cards stacked
//   3 sessions  -> one wide card (most urgent) + two tiles
//   4 sessions  -> 2x2 tiles
//   5+          -> compact rows, vertical scroll
// Sessions are sorted: waiting (needs the human) first, then busy, idle, offline; host order within.
// Status is carried by an accent strip (busy pulses slowly, waiting pulses fast + amber frame).
#include "screens.h"
#include "../ui/theme.h"
#include "../core/model.h"

#define AG_MAX_CARDS 12
#define AG_X 12
#define AG_Y 56
#define AG_W 456
#define AG_H 402
#define AG_GAP 10
#define AG_ROW_H 80

enum Variant : uint8_t { V_NONE, V_BIG, V_WIDE, V_TILE, V_ROW };

struct Card {
  lv_obj_t *obj, *strip, *name, *title, *status, *badge, *icon, *act, *todo_lbl, *todo_bar;
  lv_obj_t *pips, *pip_lbl, *ctx_bar, *ctx_lbl, *extra;
  lv_obj_t *sub[3];
  char sid[16];
  char st[10];
  uint32_t since;
  uint8_t variant;
  int8_t pulse;          // 0 none, 1 slow (busy), 2 fast (waiting), -1 unknown
  uint8_t pips_busy, pips_done, pips_max;
  int16_t w, h, name_w;
};

static Card cards[AG_MAX_CARDS];
static int n_created = 0;
static int n_shown = -1;
static lv_obj_t *root, *list, *hdr_title, *hdr_sum, *empty_box, *empty_icon, *empty_l1, *empty_l2;
static lv_timer_t *tick_timer;
static bool visible = false;
static bool was_connected = true;
static lv_style_t st_card, st_card_pr;

// ---------------------------------------------------------------- helpers
static const lv_font_t *F12 = &lv_font_montserrat_12;
static const lv_font_t *F14 = &lv_font_montserrat_14;
static const lv_font_t *F16 = &lv_font_montserrat_16;
static const lv_font_t *F18 = &lv_font_montserrat_18;
static const lv_font_t *F20 = &lv_font_montserrat_20;
static const lv_font_t *F22 = &lv_font_montserrat_22;
static const lv_font_t *F24 = &lv_font_montserrat_24;
static const lv_font_t *F28 = &lv_font_montserrat_28;
static const lv_font_t *F32 = &lv_font_montserrat_32;

#define LINES(f, n) ((n) * lv_font_get_line_height(f))

static void set_text(lv_obj_t *l, const char *s) {
  const char *cur = lv_label_get_text(l);
  if (cur && !strcmp(cur, s)) return;
  lv_label_set_text(l, s);
}

static void show(lv_obj_t *o, bool on) {
  if (on == !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
  if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static int status_rank(const char *s) {
  if (!strcmp(s, "waiting")) return 0;
  if (!strcmp(s, "busy")) return 1;
  if (!strcmp(s, "idle")) return 2;
  return 3;
}

static uint32_t status_hex(const char *s) {
  if (!strcmp(s, "busy")) return 0xD97757;
  if (!strcmp(s, "waiting")) return 0xF2C14E;
  if (!strcmp(s, "idle")) return 0x7FB069;
  return 0x8A857D;
}

static const char *status_word(const char *s) {
  if (!strcmp(s, "busy")) return "WORKING";
  if (!strcmp(s, "waiting")) return LV_SYMBOL_BELL " NEEDS YOU";
  if (!strcmp(s, "idle")) return LV_SYMBOL_OK " DONE";
  return "OFFLINE";
}

static void fmt_dur(char *out, size_t n, int32_t sec) {
  if (sec < 0) sec = 0;
  if (sec < 60) snprintf(out, n, "%lds", (long)sec);
  else if (sec < 3600) snprintf(out, n, "%ldm", (long)(sec / 60));
  else if (sec < 86400) snprintf(out, n, "%ldh%ldm", (long)(sec / 3600), (long)((sec % 3600) / 60));
  else snprintf(out, n, "%ldd", (long)(sec / 86400));
}

static const char *basename_of(const char *p) {
  const char *b = strrchr(p, '/');
  return (b && b[1]) ? b + 1 : p;
}

// Copy user text, replacing '#' (recolor marker) and control chars.
static void sanitize(char *out, size_t n, const char *in) {
  size_t i = 0;
  for (; in && *in && i + 1 < n; in++) {
    char c = *in;
    if (c == '#' || c == '\n' || c == '\r' || c == '\t') c = ' ';
    out[i++] = c;
  }
  out[i] = 0;
}

struct ToolInfo { const char *icon; const char *verb; bool path; };

static ToolInfo tool_info(const char *tool) {
  if (!strcmp(tool, "Read")) return {LV_SYMBOL_EYE_OPEN, "Read", true};
  if (!strcmp(tool, "Edit") || !strcmp(tool, "MultiEdit")) return {LV_SYMBOL_EDIT, "Edit", true};
  if (!strcmp(tool, "NotebookEdit")) return {LV_SYMBOL_EDIT, "Edit", true};
  if (!strcmp(tool, "Write")) return {LV_SYMBOL_FILE, "Write", true};
  if (!strcmp(tool, "Bash") || !strcmp(tool, "BashOutput")) return {LV_SYMBOL_KEYBOARD, "$", false};
  if (!strcmp(tool, "Grep")) return {LV_SYMBOL_LIST, "Grep", false};
  if (!strcmp(tool, "Glob")) return {LV_SYMBOL_DIRECTORY, "Find", false};
  if (!strcmp(tool, "WebSearch")) return {LV_SYMBOL_WIFI, "Search", false};
  if (!strcmp(tool, "WebFetch")) return {LV_SYMBOL_DOWNLOAD, "Fetch", false};
  if (!strcmp(tool, "Agent") || !strcmp(tool, "Task")) return {LV_SYMBOL_SHUFFLE, "Agent", false};
  if (!strcmp(tool, "TodoWrite")) return {LV_SYMBOL_LIST, "Plan", false};
  if (!strcmp(tool, "thinking")) return {LV_SYMBOL_REFRESH, "Thinking...", false};
  if (!strcmp(tool, "writing")) return {LV_SYMBOL_EDIT, "Writing reply", false};
  return {LV_SYMBOL_SETTINGS, nullptr, false};
}

// ---------------------------------------------------------------- pulse animation
static void anim_opa_cb(void *o, int32_t v) { lv_obj_set_style_bg_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }
static void anim_text_opa_cb(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }

static void set_pulse(Card &c, int8_t mode) {
  if (c.pulse == mode) return;
  c.pulse = mode;
  lv_anim_del(c.strip, anim_opa_cb);
  lv_obj_set_style_bg_opa(c.strip, LV_OPA_COVER, 0);
  if (mode == 0) return;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, c.strip);
  lv_anim_set_exec_cb(&a, anim_opa_cb);
  lv_anim_set_values(&a, 255, mode == 2 ? 40 : 80);
  lv_anim_set_time(&a, mode == 2 ? 450 : 1100);
  lv_anim_set_playback_time(&a, mode == 2 ? 450 : 1100);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

static void flash(lv_obj_t *o, int32_t from, uint32_t ms) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_exec_cb(&a, anim_text_opa_cb);
  lv_anim_set_values(&a, from, 255);
  lv_anim_set_time(&a, ms);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

// ---------------------------------------------------------------- subagent pips (one object, custom draw)
static void pips_draw_cb(lv_event_t *e) {
  lv_obj_t *o = lv_event_get_target(e);
  Card *c = (Card *)lv_obj_get_user_data(o);
  if (!c) return;
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  int d = 10, pitch = 14;
  int y0 = a.y1 + (lv_area_get_height(&a) - d) / 2;
  int n = c->pips_busy + c->pips_done;
  if (n > c->pips_max) n = c->pips_max;
  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);
  dsc.radius = LV_RADIUS_CIRCLE;
  for (int i = 0; i < n; i++) {
    bool busy = i < c->pips_busy;
    dsc.bg_color = C_SUBAGENT;
    dsc.bg_opa = busy ? LV_OPA_COVER : LV_OPA_TRANSP;
    dsc.border_color = C_SUBAGENT;
    dsc.border_width = busy ? 0 : 2;
    dsc.border_opa = LV_OPA_70;
    lv_area_t r = {(lv_coord_t)(a.x1 + i * pitch), (lv_coord_t)y0, (lv_coord_t)(a.x1 + i * pitch + d - 1),
                   (lv_coord_t)(y0 + d - 1)};
    lv_draw_rect(ctx, &dsc, &r);
  }
}

// ---------------------------------------------------------------- card construction
static void card_click_cb(lv_event_t *e) {
  Card *c = (Card *)lv_event_get_user_data(e);
  if (c && c->sid[0]) detail_open(c->sid);
}

static lv_obj_t *mk_label(lv_obj_t *p) {
  lv_obj_t *l = lv_label_create(p);
  lv_label_set_text(l, "");
  lv_obj_set_style_text_color(l, C_TEXT, 0);
  return l;
}

static lv_obj_t *mk_plain(lv_obj_t *p) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  return o;
}

static lv_obj_t *mk_bar(lv_obj_t *p) {
  lv_obj_t *b = lv_bar_create(p);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_bar_set_range(b, 0, 100);
  lv_obj_set_style_bg_color(b, C_BORDER, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(b, 4, 0);
  lv_obj_set_style_radius(b, 4, LV_PART_INDICATOR);
  lv_obj_set_style_anim_time(b, 500, 0);
  return b;
}

static void card_create(Card &c) {
  memset(&c, 0, sizeof(c));
  c.pulse = -1;
  c.obj = lv_obj_create(list);
  lv_obj_add_style(c.obj, &st_card, 0);
  lv_obj_add_style(c.obj, &st_card_pr, LV_STATE_PRESSED);
  lv_obj_clear_flag(c.obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(c.obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(c.obj, card_click_cb, LV_EVENT_CLICKED, &c);

  c.strip = mk_plain(c.obj);
  lv_obj_set_style_radius(c.strip, 3, 0);
  lv_obj_set_style_bg_opa(c.strip, LV_OPA_COVER, 0);

  c.name = mk_label(c.obj);
  lv_label_set_long_mode(c.name, LV_LABEL_LONG_DOT);
  c.title = mk_label(c.obj);
  lv_label_set_long_mode(c.title, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(c.title, C_MUTED, 0);
  c.status = mk_label(c.obj);
  lv_label_set_recolor(c.status, true);
  lv_label_set_long_mode(c.status, LV_LABEL_LONG_CLIP);

  c.badge = mk_label(c.obj);
  lv_obj_set_style_bg_color(c.badge, C_SURFACE2, 0);
  lv_obj_set_style_bg_opa(c.badge, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c.badge, 6, 0);
  lv_obj_set_style_pad_hor(c.badge, 7, 0);
  lv_obj_set_style_pad_ver(c.badge, 2, 0);
  lv_obj_set_style_text_color(c.badge, C_MUTED, 0);

  c.icon = mk_label(c.obj);
  c.act = mk_label(c.obj);
  lv_label_set_long_mode(c.act, LV_LABEL_LONG_DOT);

  c.todo_lbl = mk_label(c.obj);
  lv_label_set_long_mode(c.todo_lbl, LV_LABEL_LONG_DOT);
  lv_label_set_recolor(c.todo_lbl, true);
  lv_obj_set_style_text_color(c.todo_lbl, C_MUTED, 0);
  c.todo_bar = mk_bar(c.obj);

  c.pips = mk_plain(c.obj);
  lv_obj_set_user_data(c.pips, &c);
  lv_obj_add_event_cb(c.pips, pips_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
  c.pip_lbl = mk_label(c.obj);
  lv_obj_set_style_text_color(c.pip_lbl, C_SUBAGENT, 0);

  c.ctx_bar = mk_bar(c.obj);
  c.ctx_lbl = mk_label(c.obj);
  lv_obj_set_style_text_color(c.ctx_lbl, C_MUTED, 0);
  lv_obj_set_style_text_align(c.ctx_lbl, LV_TEXT_ALIGN_RIGHT, 0);
  c.extra = mk_label(c.obj);
  lv_obj_set_style_text_color(c.extra, C_MUTED, 0);
  for (auto &s : c.sub) {
    s = mk_label(c.obj);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_label_set_recolor(s, true);
    lv_obj_set_style_text_color(s, C_TEXT, 0);
  }
}

static void place(lv_obj_t *o, int x, int y, int w, int h, const lv_font_t *f = nullptr) {
  show(o, true);
  lv_obj_set_pos(o, x, y);
  if (h == -1) h = lv_font_get_line_height(f);  // single line (LONG_DOT/CLIP labels)
  lv_obj_set_size(o, w, h);
  if (f) lv_obj_set_style_text_font(o, f, 0);
}

// Geometry per variant. Called only when the layout changes.
static void card_layout(Card &c, Variant v, int x, int y, int w, int h) {
  lv_obj_set_pos(c.obj, x, y);
  lv_obj_set_size(c.obj, w, h);
  c.variant = v;
  c.w = w;
  c.h = h;
  const int C = LV_SIZE_CONTENT;
  for (auto s : c.sub) show(s, false);
  show(c.title, false);
  show(c.extra, false);
  lv_obj_set_style_text_align(c.status, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_set_style_pad_ver(c.badge, 2, 0);
  switch (v) {
    case V_BIG:
      lv_obj_set_style_radius(c.obj, 20, 0);
      place(c.strip, 10, 20, 6, h - 40);
      place(c.status, 30, 20, 280, -1, F18);
      place(c.badge, 0, 0, C, C, F14);
      lv_obj_align(c.badge, LV_ALIGN_TOP_RIGHT, -18, 18);
      lv_obj_set_style_pad_ver(c.badge, 3, 0);
      place(c.name, 30, 48, w - 50, -1, F32);
      place(c.title, 30, 90, w - 50, -1, F18);
      place(c.icon, 30, 134, C, C, F28);
      place(c.act, 74, 134, w - 94, LINES(F22, 2), F22);
      place(c.todo_lbl, 30, 202, w - 50, -1, F18);
      place(c.todo_bar, 30, 230, w - 50, 12);
      place(c.pips, 30, 258, 5 * 14, 20);
      place(c.pip_lbl, 30, 258, w - 50, -1, F16);
      for (int i = 0; i < 3; i++) place(c.sub[i], 30, 284 + i * 25, w - 50, -1, F16);
      place(c.extra, 30, h - 36, 220, -1, F16);
      place(c.ctx_lbl, w - 20 - 84, h - 36, 84, -1, F16);
      place(c.ctx_bar, w - 20 - 84 - 8 - 100, h - 30, 100, 8);
      c.pips_max = 5;
      break;
    case V_WIDE:
      lv_obj_set_style_radius(c.obj, 18, 0);
      place(c.strip, 10, 16, 5, h - 32);
      place(c.name, 26, 12, w - 200, -1, F24);
      place(c.status, w - 16 - 170, 16, 170, -1, F16);
      lv_obj_set_style_text_align(c.status, LV_TEXT_ALIGN_RIGHT, 0);
      place(c.title, 26, 46, w - 130, -1, F14);
      place(c.badge, 0, 0, C, C, F12);
      lv_obj_align(c.badge, LV_ALIGN_TOP_RIGHT, -16, 46);
      place(c.icon, 26, 78, C, C, F20);
      place(c.act, 56, 78, w - 72, LINES(F18, 2), F18);
      place(c.todo_lbl, 26, 128, w - 42, -1, F14);
      place(c.todo_bar, 26, 150, w - 42, 8);
      place(c.pips, 26, h - 30, 5 * 14, 16);
      place(c.pip_lbl, 26 + 5 * 14, h - 31, 60, -1, F14);
      place(c.ctx_lbl, w - 16 - 70, h - 31, 70, -1, F14);
      place(c.ctx_bar, w - 16 - 70 - 6 - 80, h - 26, 80, 6);
      c.pips_max = 5;
      break;
    case V_TILE:
      lv_obj_set_style_radius(c.obj, 18, 0);
      place(c.strip, 10, 16, 5, h - 32);
      place(c.name, 24, 12, w - 38, -1, F20);
      place(c.status, 24, 40, w - 38, -1, F14);
      place(c.badge, 0, 0, C, C, F12);
      lv_obj_align(c.badge, LV_ALIGN_TOP_RIGHT, -12, 66);
      show(c.badge, false);
      place(c.icon, 24, 68, C, C, F18);
      place(c.act, 48, 68, w - 60, LINES(F16, 2), F16);
      place(c.todo_lbl, 24, 114, w - 38, -1, F14);
      place(c.todo_bar, 24, 136, w - 38, 8);
      place(c.pips, 24, h - 31, 4 * 14, 16);
      place(c.pip_lbl, 24 + 4 * 14, h - 31, 40, -1, F14);
      place(c.ctx_lbl, w - 12 - 40, h - 31, 40, -1, F14);
      place(c.ctx_bar, w - 12 - 40 - 6 - 44, h - 26, 44, 6);
      c.pips_max = 4;
      break;
    case V_ROW:
    default:
      lv_obj_set_style_radius(c.obj, 16, 0);
      place(c.strip, 10, 14, 5, h - 28);
      place(c.name, 26, 10, w - 210, -1, F20);
      place(c.status, w - 14 - 170, 13, 170, -1, F14);
      lv_obj_set_style_text_align(c.status, LV_TEXT_ALIGN_RIGHT, 0);
      place(c.icon, 26, 46, C, C, F16);
      place(c.act, 50, 46, w - 50 - 150, -1, F16);
      place(c.pips, w - 150 + 8, 47, 4 * 14, 16);
      place(c.pip_lbl, w - 150 + 8 + 4 * 14, 46, 30, -1, F14);
      place(c.todo_lbl, w - 14 - 50, 46, 50, -1, F14);
      lv_obj_set_style_text_align(c.todo_lbl, LV_TEXT_ALIGN_RIGHT, 0);
      show(c.badge, false);
      show(c.todo_bar, false);
      show(c.ctx_bar, false);
      show(c.ctx_lbl, false);
      c.pips_max = 4;
      break;
  }
  if (v != V_ROW) lv_obj_set_style_text_align(c.todo_lbl, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_set_style_pad_top(c.name, 0, 0);
  c.name_w = lv_obj_get_style_width(c.name, 0);
}

// ---------------------------------------------------------------- card content
// Shrink the project name font (down to two steps) when it doesn't fit, before falling back to "...".
static void fit_name(Card &c, const char *txt) {
  static const lv_font_t *const steps[][3] = {
      {F32, F28, F24},  // V_BIG
      {F24, F22, F20},  // V_WIDE
      {F20, F18, F16},  // V_TILE
      {F20, F18, F16},  // V_ROW
  };
  int vi = c.variant - V_BIG;
  if (vi < 0 || vi > 3) return;
  lv_coord_t avail = c.name_w;
  const lv_font_t *f = steps[vi][2];
  for (int i = 0; i < 3; i++) {
    lv_point_t sz;
    lv_txt_get_size(&sz, txt, steps[vi][i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x <= avail) { f = steps[vi][i]; break; }
  }
  if (lv_obj_get_style_text_font(c.name, 0) != f) {
    lv_obj_set_style_text_font(c.name, f, 0);
    // keep the baseline area: vertically center the smaller font in the original line box
    lv_obj_set_height(c.name, lv_font_get_line_height(steps[vi][0]));
    lv_obj_set_style_pad_top(c.name, f == steps[vi][0] ? 0 : (lv_font_get_line_height(steps[vi][0]) - lv_font_get_line_height(f)) / 2 + 1, 0);
  }
}

static void card_status_text(Card &c) {
  char dur[16] = "";
  uint32_t now = model_now();
  if (c.since && now) fmt_dur(dur, sizeof(dur), (int32_t)(now - c.since));
  char buf[96];
  snprintf(buf, sizeof(buf), "#%06lX %s#  #9C978E %s#", (unsigned long)status_hex(c.st), status_word(c.st), dur);
  set_text(c.status, buf);
}

static void card_fill(Card &c, JsonObjectConst s) {
  const char *sid = s["id"] | "";
  const char *st = s["status"] | "offline";
  bool new_sid = strcmp(c.sid, sid) != 0;
  bool st_changed = strcmp(c.st, st) != 0;
  strlcpy(c.sid, sid, sizeof(c.sid));
  strlcpy(c.st, st, sizeof(c.st));
  c.since = s["since"] | 0;
  lv_color_t col = status_color(st);
  bool waiting = !strcmp(st, "waiting"), offline = !strcmp(st, "offline"), idle = !strcmp(st, "idle");
  Variant v = (Variant)c.variant;
  char buf[200], tmp[160];

  if (new_sid || st_changed) {
    lv_obj_set_style_bg_color(c.strip, col, 0);
    lv_obj_set_style_border_width(c.obj, waiting ? 2 : 0, 0);
    lv_obj_set_style_border_color(c.obj, C_WAITING, 0);
    lv_obj_set_style_bg_color(c.obj, waiting ? lv_color_mix(C_WAITING, C_SURFACE, 28) : C_SURFACE, 0);
    lv_obj_set_style_text_color(c.name, offline ? C_MUTED : C_TEXT, 0);
    lv_obj_set_style_text_color(c.icon, offline ? C_DIM : col, 0);
    lv_obj_set_style_text_color(c.act, (idle || offline) ? C_MUTED : C_TEXT, 0);
    lv_obj_set_style_bg_color(c.todo_bar, offline ? C_DIM : col, LV_PART_INDICATOR);
    if (!new_sid) flash(c.icon, 40, 500);
  }
  if (visible) set_pulse(c, waiting ? 2 : (!strcmp(st, "busy") ? 1 : 0));
  if (new_sid) flash(c.obj, 60, 300);

  // name / title
  const char *name = s["name"] | "";
  if (!*name) name = s["project"] | "?";
  sanitize(tmp, sizeof(tmp), name);
  set_text(c.name, tmp);
  fit_name(c, tmp);
  if (v == V_BIG || v == V_WIDE) {
    const char *t = s["title"] | "";
    if (!*t) t = s["cwd"] | "";
    sanitize(tmp, sizeof(tmp), t);
    set_text(c.title, tmp);
  }
  card_status_text(c);

  // model badge
  const char *model = s["model"] | "";
  if (v == V_BIG || v == V_WIDE) {
    show(c.badge, *model != 0);
    sanitize(tmp, 24, model);
    set_text(c.badge, tmp);
  }

  // activity
  JsonObjectConst a = s["activity"];
  const char *tool = a["tool"] | "";
  const char *detail = a["detail"] | "";
  const char *icon;
  if (offline) {
    icon = LV_SYMBOL_POWER;
    snprintf(buf, sizeof(buf), "Session ended");
  } else if (idle) {
    icon = LV_SYMBOL_OK;
    const char *t = s["title"] | "";
    if (!*t) t = s["last_prompt"] | "Ready for the next prompt";
    sanitize(buf, sizeof(buf), t);
  } else {
    if (!strncmp(tool, "mcp__", 5)) {
      const char *p = strrchr(tool, '_');
      tool = p ? p + 1 : tool;
    }
    ToolInfo ti = tool_info(tool);
    icon = ti.icon;
    const char *d = (ti.path && v != V_BIG) ? basename_of(detail) : detail;
    const char *verb = ti.verb ? ti.verb : (*tool ? tool : "Working");
    if (*d) snprintf(tmp, sizeof(tmp), "%s %s", verb, d);
    else snprintf(tmp, sizeof(tmp), "%s", verb);
    sanitize(buf, sizeof(buf), tmp);
  }
  set_text(c.icon, icon);
  set_text(c.act, buf);

  // todos
  JsonObjectConst td = s["todos"];
  int done = td["done"] | 0, total = td["total"] | 0;
  bool has_todo = total > 0;
  if (v == V_ROW) {
    show(c.todo_lbl, has_todo);
    if (has_todo) {
      snprintf(buf, sizeof(buf), "%d/%d", done, total);
      set_text(c.todo_lbl, buf);
    }
  } else {
    show(c.todo_lbl, has_todo);
    show(c.todo_bar, has_todo);
    if (has_todo) {
      sanitize(tmp, sizeof(tmp), td["current"] | "");
      if (done >= total) snprintf(buf, sizeof(buf), "#ECE9E4 %d/%d#  all done", done, total);
      else snprintf(buf, sizeof(buf), "#ECE9E4 %d/%d#  %s", done, total, tmp);
      set_text(c.todo_lbl, buf);
      lv_bar_set_value(c.todo_bar, total ? done * 100 / total : 0, new_sid ? LV_ANIM_OFF : LV_ANIM_ON);
    }
    // activity gets more lines when there is no todo list
    if (v == V_TILE) lv_obj_set_height(c.act, LINES(F16, has_todo ? 2 : 4));
    if (v == V_WIDE) lv_obj_set_height(c.act, LINES(F18, has_todo ? 2 : 3));
    if (v == V_BIG) lv_obj_set_height(c.act, LINES(F22, has_todo ? 2 : 4));
  }

  // subagents
  JsonArrayConst subs = s["subagents"];
  int nb = 0, nd = 0;
  for (JsonObjectConst sa : subs) {
    if (!strcmp(sa["status"] | "", "busy")) nb++;
    else nd++;
  }
  if (nb != c.pips_busy || nd != c.pips_done) {
    c.pips_busy = nb;
    c.pips_done = nd;
    lv_obj_invalidate(c.pips);
  }
  int npip = nb + nd;
  bool any_sub = npip > 0;
  if (v == V_BIG) {
    show(c.pips, any_sub);
    lv_obj_set_width(c.pips, (npip > 5 ? 5 : npip) * 14);
    lv_obj_set_x(c.pip_lbl, 30 + (any_sub ? (npip > 5 ? 5 : npip) * 14 + 4 : 0));
    if (any_sub) {
      lv_obj_set_style_text_color(c.pip_lbl, C_SUBAGENT, 0);
      if (nb) snprintf(buf, sizeof(buf), "%d helper%s working", nb, nb == 1 ? "" : "s");
      else snprintf(buf, sizeof(buf), "%d helper%s finished", nd, nd == 1 ? "" : "s");
      set_text(c.pip_lbl, buf);
      int i = 0;
      // busy first, then done
      for (int pass = 0; pass < 2; pass++)
        for (JsonObjectConst sa : subs) {
          bool busy = !strcmp(sa["status"] | "", "busy");
          if ((pass == 0) != busy || i >= 3) continue;
          sanitize(tmp, 120, sa["desc"] | "subagent");
          char tl[40];
          sanitize(tl, sizeof(tl), sa["tool"] | "");
          if (busy)
            snprintf(buf, sizeof(buf), "#B392F0 " LV_SYMBOL_SHUFFLE "#  %s  #9C978E %s#", tmp, tl);
          else
            snprintf(buf, sizeof(buf), "#5E5A54 " LV_SYMBOL_OK "#  #9C978E %s#", tmp);
          show(c.sub[i], true);
          lv_obj_set_style_text_color(c.sub[i], C_TEXT, 0);
          lv_label_set_long_mode(c.sub[i], LV_LABEL_LONG_DOT);
          lv_obj_set_height(c.sub[i], lv_font_get_line_height(F16));
          set_text(c.sub[i], buf);
          i++;
        }
      for (; i < 3; i++) show(c.sub[i], false);
    } else {
      // no helpers: show the last prompt instead
      lv_obj_set_style_text_color(c.pip_lbl, C_DIM, 0);
      set_text(c.pip_lbl, "LAST PROMPT");
      const char *lp = s["last_prompt"] | "";
      sanitize(tmp, sizeof(tmp), lp);
      snprintf(buf, sizeof(buf), "%s", *tmp ? tmp : "-");
      lv_obj_set_style_text_color(c.sub[0], C_MUTED, 0);  // recolor doesn't survive line wraps
      show(c.sub[0], true);
      lv_label_set_long_mode(c.sub[0], LV_LABEL_LONG_DOT);
      lv_obj_set_height(c.sub[0], LINES(F16, 3));
      set_text(c.sub[0], buf);
      show(c.sub[1], false);
      show(c.sub[2], false);
    }
    int tc = s["tool_count"] | 0;
    float cost = s["cost"] | 0.0f;
    snprintf(buf, sizeof(buf), LV_SYMBOL_SETTINGS " %d tools   $%.2f", tc, cost);
    set_text(c.extra, buf);
  } else {
    show(c.pips, any_sub);
    show(c.pip_lbl, any_sub);
    int shown = npip > c.pips_max ? c.pips_max : npip;
    lv_obj_set_width(c.pips, shown * 14);
    lv_obj_set_x(c.pip_lbl, lv_obj_get_x(c.pips) + shown * 14 + 2);
    if (any_sub) {
      if (npip > c.pips_max) snprintf(buf, sizeof(buf), "+%d", npip - c.pips_max);
      else buf[0] = 0;
      set_text(c.pip_lbl, buf);
    }
  }

  // context meter
  if (v != V_ROW) {
    int ctx = s["ctx_pct"] | -1;
    show(c.ctx_bar, ctx >= 0);
    show(c.ctx_lbl, ctx >= 0);
    if (ctx >= 0) {
      if (ctx > 100) ctx = 100;
      lv_color_t cc = ctx >= 85 ? C_ERROR : C_INFO;
      lv_obj_set_style_bg_color(c.ctx_bar, cc, LV_PART_INDICATOR);
      lv_bar_set_value(c.ctx_bar, ctx, new_sid ? LV_ANIM_OFF : LV_ANIM_ON);
      if (v == V_TILE) snprintf(buf, sizeof(buf), "%d%%", ctx);
      else snprintf(buf, sizeof(buf), "ctx %d%%", ctx);
      set_text(c.ctx_lbl, buf);
      lv_obj_set_style_text_color(c.ctx_lbl, ctx >= 85 ? C_ERROR : C_MUTED, 0);
    }
  }
}

// ---------------------------------------------------------------- layout
static void apply_layout(int n) {
  if (n == n_shown) return;
  n_shown = n;
  while (n_created < n && n_created < AG_MAX_CARDS) card_create(cards[n_created++]);
  for (int i = 0; i < n_created; i++) {
    show(cards[i].obj, i < n);
    if (i >= n) { set_pulse(cards[i], 0); cards[i].sid[0] = 0; cards[i].st[0] = 0; }
  }
  int tw = (AG_W - AG_GAP) / 2, th = (AG_H - AG_GAP) / 2;
  bool scroll = n >= 5;
  if (n == 1) card_layout(cards[0], V_BIG, 0, 0, AG_W, AG_H);
  else if (n == 2) {
    card_layout(cards[0], V_WIDE, 0, 0, AG_W, th);
    card_layout(cards[1], V_WIDE, 0, th + AG_GAP, AG_W, th);
  } else if (n == 3) {
    card_layout(cards[0], V_WIDE, 0, 0, AG_W, th);
    card_layout(cards[1], V_TILE, 0, th + AG_GAP, tw, th);
    card_layout(cards[2], V_TILE, tw + AG_GAP, th + AG_GAP, tw, th);
  } else if (n == 4) {
    for (int i = 0; i < 4; i++) card_layout(cards[i], V_TILE, (i % 2) * (tw + AG_GAP), (i / 2) * (th + AG_GAP), tw, th);
  } else {
    for (int i = 0; i < n; i++) card_layout(cards[i], V_ROW, 0, i * (AG_ROW_H + 8), AG_W, AG_ROW_H);
  }
  // Force content refresh after a re-layout (fonts/visibility changed).
  for (int i = 0; i < n; i++) { cards[i].sid[0] = 0; cards[i].st[0] = 0; }
  if (scroll) lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  else {
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  }
}

static void set_empty(const char *icon, const char *l1, const char *l2, lv_color_t col) {
  show(empty_box, true);
  set_text(empty_icon, icon);
  lv_obj_set_style_text_color(empty_icon, col, 0);
  set_text(empty_l1, l1);
  set_text(empty_l2, l2);
}

static void header_summary(JsonArrayConst ss, bool connected) {
  if (!connected) {
    set_text(hdr_sum, "#E5534B " LV_SYMBOL_USB " host offline#");
    return;
  }
  int cnt[4] = {0, 0, 0, 0}, helpers = 0;
  for (JsonObjectConst s : ss) {
    cnt[status_rank(s["status"] | "offline")]++;
    for (JsonObjectConst sa : s["subagents"].as<JsonArrayConst>())
      if (!strcmp(sa["status"] | "", "busy")) helpers++;
  }
  char buf[200];
  size_t o = 0;
  buf[0] = 0;
  if (cnt[0]) o += snprintf(buf + o, sizeof(buf) - o, "#F2C14E " LV_SYMBOL_BELL " %d#   ", cnt[0]);
  if (cnt[1]) o += snprintf(buf + o, sizeof(buf) - o, "#D97757 " LV_SYMBOL_REFRESH " %d#   ", cnt[1]);
  if (cnt[2]) o += snprintf(buf + o, sizeof(buf) - o, "#7FB069 " LV_SYMBOL_OK " %d#   ", cnt[2]);
  if (helpers) o += snprintf(buf + o, sizeof(buf) - o, "#B392F0 " LV_SYMBOL_SHUFFLE " %d#", helpers);
  while (o > 0 && buf[o - 1] == ' ') buf[--o] = 0;
  set_text(hdr_sum, buf);
}

static void update(JsonObjectConst state) {
  bool connected = model_host_connected();
  was_connected = connected;
  JsonArrayConst ss = state["sessions"];
  header_summary(ss, connected);
  lv_obj_set_style_opa(list, connected ? LV_OPA_COVER : LV_OPA_50, 0);

  // sort: waiting, busy, idle, offline (stable)
  JsonObjectConst sorted[AG_MAX_CARDS];
  int n = 0;
  for (int rank = 0; rank < 4 && n < AG_MAX_CARDS; rank++)
    for (JsonObjectConst s : ss) {
      if (n >= AG_MAX_CARDS) break;
      if (status_rank(s["status"] | "offline") == rank) sorted[n++] = s;
    }

  if (model_state_version() == 0) {
    apply_layout(0);
    set_empty(LV_SYMBOL_USB, "Waiting for host", "Run the companion on your Mac", C_DIM);
    return;
  }
  if (n == 0) {
    apply_layout(0);
    set_empty(LV_SYMBOL_EYE_CLOSE, "All quiet", "No Claude Code sessions running", C_IDLE);
    return;
  }
  show(empty_box, false);
  apply_layout(n);
  for (int i = 0; i < n; i++) {
    card_fill(cards[i], sorted[i]);
    if (!connected) set_pulse(cards[i], 0);
  }
}

static void tick_cb(lv_timer_t *) {
  bool connected = model_host_connected();
  if (connected != was_connected) {
    update(model_state());
    return;
  }
  for (int i = 0; i < n_shown && i < n_created; i++) card_status_text(cards[i]);
}

static void on_show(bool vis) {
  visible = vis;
  if (vis) {
    lv_timer_resume(tick_timer);
  } else {
    lv_timer_pause(tick_timer);
    for (int i = 0; i < n_created; i++) set_pulse(cards[i], 0);
    for (int i = 0; i < n_created; i++) cards[i].pulse = -1;
  }
}

static void on_event(JsonObjectConst ev) {
  if (!visible) return;
  const char *kind = ev["kind"] | "";
  if (strcmp(kind, "tool") && strcmp(kind, "subagent_start") && strcmp(kind, "todo_done")) return;
  const char *sid = ev["sid"] | "";
  for (int i = 0; i < n_shown && i < n_created; i++)
    if (!strcmp(cards[i].sid, sid)) {
      flash(cards[i].icon, 70, 450);
      break;
    }
}

static void create(lv_obj_t *p) {
  root = p;
  lv_style_init(&st_card);
  lv_style_set_bg_color(&st_card, C_SURFACE);
  lv_style_set_bg_opa(&st_card, LV_OPA_COVER);
  lv_style_set_radius(&st_card, 18);
  lv_style_set_border_width(&st_card, 0);
  lv_style_set_pad_all(&st_card, 0);
  lv_style_set_shadow_width(&st_card, 0);
  lv_style_init(&st_card_pr);
  lv_style_set_bg_color(&st_card_pr, C_SURFACE2);

  hdr_title = lv_label_create(p);
  lv_label_set_text(hdr_title, "Agents");
  lv_obj_set_style_text_font(hdr_title, F_TITLE, 0);
  lv_obj_set_style_text_color(hdr_title, C_TEXT, 0);
  lv_obj_set_pos(hdr_title, 22, 16);

  hdr_sum = lv_label_create(p);
  lv_label_set_recolor(hdr_sum, true);
  lv_label_set_text(hdr_sum, "");
  lv_obj_set_style_text_font(hdr_sum, F20, 0);
  lv_obj_align(hdr_sum, LV_ALIGN_TOP_RIGHT, -22, 19);

  list = lv_obj_create(p);
  lv_obj_remove_style_all(list);
  lv_obj_set_pos(list, AG_X, AG_Y);
  lv_obj_set_size(list, AG_W, AG_H);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_pad_bottom(list, 8, 0);
  lv_obj_set_style_bg_color(list, C_DIM, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(list, LV_OPA_70, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_right(list, 0, LV_PART_SCROLLBAR);
  lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

  empty_box = mk_plain(p);
  lv_obj_set_size(empty_box, 440, 260);
  lv_obj_align(empty_box, LV_ALIGN_CENTER, 0, 10);
  empty_icon = lv_label_create(empty_box);
  lv_obj_set_style_text_font(empty_icon, F_HUGE, 0);
  lv_obj_align(empty_icon, LV_ALIGN_TOP_MID, 0, 30);
  empty_l1 = lv_label_create(empty_box);
  lv_obj_set_style_text_font(empty_l1, F_TITLE, 0);
  lv_obj_set_style_text_color(empty_l1, C_TEXT, 0);
  lv_obj_align(empty_l1, LV_ALIGN_TOP_MID, 0, 110);
  empty_l2 = lv_label_create(empty_box);
  lv_obj_set_style_text_font(empty_l2, F_BODY, 0);
  lv_obj_set_style_text_color(empty_l2, C_MUTED, 0);
  lv_obj_align(empty_l2, LV_ALIGN_TOP_MID, 0, 150);
  set_empty(LV_SYMBOL_USB, "Waiting for host", "Run the companion on your Mac", C_DIM);

  tick_timer = lv_timer_create(tick_cb, 1000, NULL);
  lv_timer_pause(tick_timer);
}

const ScreenDef screen_agents = {"agents", "Agents", create, update, on_event, on_show};
