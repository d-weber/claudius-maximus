// HOME — Claudius, the spark companion, plus a glanceable summary of all agents.
//   top bar:   clock + date | 5h budget mini bar
//   centre:    animated character (home_face.cpp) reflecting the aggregate mood, speech bubble
//   stats:     working / need you / idle / helpers  (tap -> agents screen)
//   card:      the most important session right now (waiting first) (tap -> detail)
#include "screens.h"
#include "../ui/theme.h"
#include "../core/model.h"
#include "home_face.h"
#include <time.h>

static lv_obj_t *lbl_clock, *lbl_date, *budget_box, *budget_bar, *budget_pct;
static lv_obj_t *stats_row, *stat_num[4], *stat_cap[4];
static lv_obj_t *card, *card_dot, *card_name, *card_meta, *card_text, *card_chev;
static lv_timer_t *tmr;
static bool visible = false, host_prev = false, greeted = false;
static char focus_sid[16];
static int n_wait_cached = 0, focus_rot = 0;
static uint32_t focus_rot_ms = 0, last_glance = 0, last_slow = 0;
static int tap_n = 0;

static const lv_color_t STAT_COL[4] = {C_BUSY, C_WAITING, C_IDLE, C_SUBAGENT};
static const char *const STAT_CAP[4] = {"working", "need you", "idle", "helpers"};

static const char *const QUIPS[] = {
    "Ship it!",
    "Small diffs make happy reviewers.",
    "Have you had some water lately?",
    "I believe in your tests.",
    "Rubber duck mode: on.",
    "Good time to stretch. I'll keep watch.",
    "Reading the docs so you don't have to.",
    "Nice code. Truly.",
    "Grab a coffee, I've got this.",
    "Tap me again, I like it.",
};
static const int N_QUIPS = sizeof(QUIPS) / sizeof(QUIPS[0]);

// ------------------------------------------------------------------ helpers
static const char *tool_verb(const char *tool) {
  static const struct { const char *t, *v; } M[] = {
      {"Read", "Reading"},     {"Edit", "Editing"},          {"MultiEdit", "Editing"},  {"Write", "Writing"},
      {"Bash", "Running"},     {"BashOutput", "Watching"},   {"Grep", "Searching"},     {"Glob", "Finding"},
      {"LS", "Listing"},       {"WebSearch", "Searching"},   {"WebFetch", "Fetching"},  {"Agent", "Delegating"},
      {"Task", "Delegating"},  {"TodoWrite", "Planning"},    {"NotebookEdit", "Editing"},
      {"thinking", "Thinking"}, {"writing", "Writing reply"},
  };
  if (!tool || !*tool) return "Working";
  for (auto &m : M) if (!strcmp(m.t, tool)) return m.v;
  return tool;
}

static char tool_category(const char *tool) {
  if (!tool || !*tool) return 't';
  if (!strcmp(tool, "Read") || !strcmp(tool, "Grep") || !strcmp(tool, "Glob") || !strcmp(tool, "LS")) return 'r';
  if (!strcmp(tool, "Edit") || !strcmp(tool, "MultiEdit") || !strcmp(tool, "Write") || !strcmp(tool, "NotebookEdit"))
    return 'e';
  if (!strncmp(tool, "Bash", 4)) return 'x';
  if (!strncmp(tool, "Web", 3)) return 'w';
  if (!strcmp(tool, "Agent") || !strcmp(tool, "Task")) return 'a';
  if (!strcmp(tool, "thinking") || !strcmp(tool, "writing")) return 't';
  return 0;
}

static bool is_file_tool(const char *tool) {
  return tool && (!strcmp(tool, "Read") || !strcmp(tool, "Edit") || !strcmp(tool, "MultiEdit") ||
                  !strcmp(tool, "Write") || !strcmp(tool, "NotebookEdit"));
}

static void activity_line(JsonObjectConst s, char *out, size_t n) {
  const char *tool = s["activity"]["tool"] | "";
  const char *det = s["activity"]["detail"] | "";
  if (is_file_tool(tool)) {
    const char *b = strrchr(det, '/');
    if (b && b[1]) det = b + 1;
  }
  if (!strcmp(tool, "thinking")) snprintf(out, n, "Thinking...");
  else if (*det) snprintf(out, n, "%s %s", tool_verb(tool), det);
  else snprintf(out, n, "%s...", tool_verb(tool));
}

static void fmt_ago(uint32_t since, char *out, size_t n) {
  uint32_t now = model_now();
  if (!since || !now || since > now) { out[0] = 0; return; }
  uint32_t d = now - since;
  if (d < 60) snprintf(out, n, "%us", (unsigned)d);
  else if (d < 3600) snprintf(out, n, "%um", (unsigned)(d / 60));
  else snprintf(out, n, "%uh %um", (unsigned)(d / 3600), (unsigned)(d % 3600 / 60));
}

// Latest event text of `kind` for session `sid` (empty if none).
static const char *last_event_text(const char *sid, const char *kind) {
  for (size_t i = model_event_count(); i-- > 0;) {
    JsonObjectConst e = model_event(i);
    if (!strcmp(e["sid"] | "", sid) && !strcmp(e["kind"] | "", kind)) return e["text"] | "";
  }
  return "";
}

static void style_plain(lv_obj_t *o) {
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *mk_label(lv_obj_t *p, const lv_font_t *f, lv_color_t c) {
  lv_obj_t *l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, "");
  return l;
}

static void set_single_line(lv_obj_t *l, lv_coord_t w) {
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_width(l, w);
  lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
}

// ------------------------------------------------------------------ change-only setters
// Every lv_label_set_text / lv_obj_set_style_* / flag change invalidates (and may relayout) even when the
// value is identical, so refresh() only touches what actually changed.
static void set_txt(lv_obj_t *l, const char *t) {
  if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}
static void set_txt_fmt(lv_obj_t *l, const char *fmt, int v) {
  char b[16];
  snprintf(b, sizeof b, fmt, v);
  set_txt(l, b);
}
static void set_text_col(lv_obj_t *o, lv_color_t c) {
  if (lv_obj_get_style_text_color(o, 0).full != c.full) lv_obj_set_style_text_color(o, c, 0);
}
static void set_bg_col(lv_obj_t *o, lv_color_t c, lv_style_selector_t sel = 0) {
  if (lv_obj_get_style_bg_color(o, sel).full != c.full) lv_obj_set_style_bg_color(o, c, sel);
}
static void set_hidden(lv_obj_t *o, bool h) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == h) return;
  if (h) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// ------------------------------------------------------------------ data -> UI
static void card_set(lv_color_t col, const char *name, const char *meta, const char *txt, const lv_font_t *txt_font,
                     bool alert, bool tappable) {
  set_bg_col(card_dot, col);
  set_text_col(card_name, col);
  set_txt(card_name, name);
  set_txt(card_meta, meta);
  if (lv_obj_get_style_text_font(card_text, 0) != txt_font) {
    lv_obj_set_style_text_font(card_text, txt_font, 0);
    lv_obj_set_height(card_text, lv_font_get_line_height(txt_font));
  }
  set_txt(card_text, txt);
  if (lv_obj_get_style_border_width(card, 0) != (alert ? 2 : 1)) {
    lv_obj_set_style_border_color(card, alert ? C_WAITING : C_BORDER, 0);
    lv_obj_set_style_border_width(card, alert ? 2 : 1, 0);
    lv_obj_set_style_bg_color(card, alert ? lv_color_mix(C_WAITING, C_SURFACE, 28) : C_SURFACE, 0);
  }
  set_hidden(card_chev, !tappable);
  if (lv_obj_get_style_width(card_text, 0) != (tappable ? 370 : 412)) lv_obj_set_width(card_text, tappable ? 370 : 412);
}

static void refresh(JsonObjectConst s) {
  bool host = model_host_connected();
  int busy = 0, wait = 0, idle = 0, help = 0, alive = 0;
  JsonObjectConst f_wait[8];
  JsonObjectConst f_busy, f_idle;
  for (JsonObjectConst x : s["sessions"].as<JsonArrayConst>()) {
    const char *st = x["status"] | "offline";
    if (!strcmp(st, "busy")) { busy++; if (f_busy.isNull()) f_busy = x; }
    else if (!strcmp(st, "waiting")) { if (wait < 8) f_wait[wait] = x; wait++; }
    else if (!strcmp(st, "idle")) { idle++; if (f_idle.isNull()) f_idle = x; }
    else continue;
    alive++;
    for (JsonObjectConst a : x["subagents"].as<JsonArrayConst>())
      if (!strcmp(a["status"] | "", "busy")) help++;
  }
  if (help == 0) help = s["totals"]["subagents_active"] | 0;
  n_wait_cached = wait;

  // character mood
  char cat = 0;
  if (!f_busy.isNull()) cat = tool_category(f_busy["activity"]["tool"] | "");
  HomeMood m = !host ? HOME_MOOD_OFFLINE
               : alive == 0 ? HOME_MOOD_SLEEP
               : wait ? HOME_MOOD_WAITING
               : busy ? HOME_MOOD_BUSY
                      : HOME_MOOD_IDLE;
  float energy = busy ? 0.35f + 0.22f * (busy - 1) + 0.06f * help : 0.1f;
  home_face_set_mood(m, energy, host ? help : 0, cat);

  // stats
  if (host) {
    set_hidden(stats_row, false);
    int v[4] = {busy, wait, idle, help};
    for (int i = 0; i < 4; i++) {
      set_txt_fmt(stat_num[i], "%d", v[i]);
      set_text_col(stat_num[i], v[i] ? STAT_COL[i] : C_DIM);
      set_text_col(stat_cap[i], v[i] ? C_MUTED : C_DIM);
    }
  } else {
    set_hidden(stats_row, true);
  }

  // budget
  int pct = s["usage"]["five_hour"]["pct"] | -1;
  if (host && pct >= 0) {
    set_hidden(budget_box, false);
    if (lv_bar_get_value(budget_bar) != (pct > 100 ? 100 : pct)) lv_bar_set_value(budget_bar, pct > 100 ? 100 : pct, LV_ANIM_OFF);
    lv_color_t bc = pct >= 85 ? C_ERROR : (pct >= 60 ? C_WAITING : C_IDLE);
    set_bg_col(budget_bar, bc, LV_PART_INDICATOR);
    set_txt_fmt(budget_pct, "%d%%", pct);
  } else {
    set_hidden(budget_box, true);
  }

  // focus card
  char meta[24], line[112], name[64];
  if (!host) {
    focus_sid[0] = 0;
    card_set(C_OFFLINE, LV_SYMBOL_USB "  Not connected", "", "Start the companion daemon on your Mac", F_BODY,
             false, false);
    return;
  }
  if (alive == 0) {
    focus_sid[0] = 0;
    card_set(C_DIM, "All quiet", "", "Start claude in a terminal to wake me", F_BODY, false, false);
    return;
  }
  JsonObjectConst f;
  if (wait) {
    f = f_wait[focus_rot % (wait > 8 ? 8 : wait)];
    snprintf(name, sizeof(name), "%s needs you", f["name"] | "?");
    const char *q = last_event_text(f["id"] | "", "waiting");
    snprintf(line, sizeof(line), "%s", *q ? q : "Waiting for your input");
    if (wait > 1) snprintf(meta, sizeof(meta), "1 of %d", wait);
    else fmt_ago(f["since"] | 0, meta, sizeof(meta));
    strlcpy(focus_sid, f["id"] | "", sizeof(focus_sid));
    card_set(C_WAITING, name, meta, line, F_TITLE, true, true);
    return;
  }
  f = !f_busy.isNull() ? f_busy : f_idle;
  strlcpy(focus_sid, f["id"] | "", sizeof(focus_sid));
  strlcpy(name, f["name"] | "?", sizeof(name));
  fmt_ago(f["since"] | 0, meta, sizeof(meta));
  if (!f_busy.isNull()) {
    activity_line(f, line, sizeof(line));
    card_set(C_BUSY, name, meta, line, F_TITLE, false, true);
  } else {
    const char *done = last_event_text(f["id"] | "", "stop");
    if (*done) snprintf(line, sizeof(line), "%s", done);
    else snprintf(line, sizeof(line), "Ready for your next prompt");
    card_set(C_IDLE, name, meta, line, F_TITLE, false, true);
  }
}

static void update_clock() {
  uint32_t now = model_now();
  static uint32_t shown = 1;
  if (!now) {
    if (shown) { lv_label_set_text(lbl_clock, "--:--"); lv_label_set_text(lbl_date, ""); shown = 0; }
    return;
  }
  time_t lt = (time_t)now + model_tz();
  if ((uint32_t)lt / 60 == shown) return;
  shown = (uint32_t)lt / 60;
  struct tm tm;
  gmtime_r(&lt, &tm);
  static const char *DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *MONS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  lv_label_set_text_fmt(lbl_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
  lv_label_set_text_fmt(lbl_date, "%s %d %s", DAYS[tm.tm_wday], tm.tm_mday, MONS[tm.tm_mon]);
}

static int local_hour() {
  uint32_t now = model_now();
  if (!now) return 12;
  time_t lt = (time_t)now + model_tz();
  struct tm tm;
  gmtime_r(&lt, &tm);
  return tm.tm_hour;
}

static void greet() {
  int h = local_hour();
  const char *g = h < 5 ? "Burning the midnight oil?" : h < 12 ? "Good morning!" : h < 18 ? "Good afternoon!" : "Good evening!";
  JsonObjectConst s = model_state();
  int busy = s["totals"]["busy"] | 0;
  char buf[96];
  if (busy) snprintf(buf, sizeof(buf), "%s %d agent%s at work.", g, busy, busy > 1 ? "s" : "");
  else snprintf(buf, sizeof(buf), "%s", g);
  home_face_react(HOME_REACT_SURPRISE);
  home_face_say(buf, 3500, 1);
}

// ------------------------------------------------------------------ interaction
static void face_tapped() {
  home_face_react((tap_n & 1) ? HOME_REACT_WINK : HOME_REACT_GIGGLE);
  char lines[9][112];
  int n = 0;
  JsonObjectConst s = model_state();
  if (!model_host_connected()) {
    snprintf(lines[n++], 112, "Zzz... start the companion daemon on your Mac and I'll wake up.");
  } else {
    int nb = 0;
    for (JsonObjectConst x : s["sessions"].as<JsonArrayConst>()) {
      if (n >= 5) break;
      const char *st = x["status"] | "";
      if (!strcmp(st, "waiting")) snprintf(lines[n++], 112, "%s is waiting for you. Tap the card below!", x["name"] | "?");
      else if (!strcmp(st, "busy") && nb < 3) {
        char act[96];
        activity_line(x, act, sizeof(act));
        snprintf(lines[n++], 112, "%s: %s", x["name"] | "?", act);
        nb++;
      }
    }
    int help = s["totals"]["subagents_active"] | 0;
    if (help) snprintf(lines[n++], 112, "%d little helper%s out exploring.", help, help > 1 ? "s" : "");
    int pct = s["usage"]["five_hour"]["pct"] | -1;
    if (pct >= 0) snprintf(lines[n++], 112, "%d%% of your 5-hour budget used.", pct);
    int tools = s["totals"]["tools_today"] | 0;
    if (tools) snprintf(lines[n++], 112, "%d tool calls today. Busy busy!", tools);
    int streak = s["usage"]["streak_days"] | 0;
    if (streak > 1 && n < 9) snprintf(lines[n++], 112, "%d-day streak. Keep it going!", streak);
  }
  int k = tap_n % (n + 1);
  home_face_say(k < n ? lines[k] : QUIPS[(tap_n / (n + 1)) % N_QUIPS], 4200, 2);
  tap_n++;
}

static void card_clicked(lv_event_t *) {
  if (focus_sid[0]) detail_open(focus_sid);
}
static void stats_clicked(lv_event_t *) { ui_nav("agents"); }

static void on_timer(lv_timer_t *) {
  home_face_tick();
  uint32_t now = lv_tick_get();
  if (now - last_slow < 500) return;
  last_slow = now;
  update_clock();
  bool host = model_host_connected();
  if (host != host_prev) {
    host_prev = host;
    refresh(model_state());
    if (host && !greeted) { greeted = true; greet(); }
    if (!host) greeted = false;
  }
  if (n_wait_cached > 1 && now - focus_rot_ms > 4000) {
    focus_rot_ms = now;
    focus_rot++;
    refresh(model_state());
  }
}

// ------------------------------------------------------------------ ScreenDef
static void create(lv_obj_t *p) {
  home_face_create(p, 44, 242);
  home_face_on_tap(face_tapped);

  // top bar
  lv_obj_t *row = lv_obj_create(p);
  style_plain(row);
  lv_obj_set_size(row, 240, 32);
  lv_obj_set_pos(row, 26, 12);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(row, 10, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lbl_clock = mk_label(row, F_TITLE, C_TEXT);
  lv_label_set_text(lbl_clock, "--:--");
  lbl_date = mk_label(row, F_SMALL, C_MUTED);
  lv_obj_set_style_pad_bottom(lbl_date, 3, 0);

  budget_box = lv_obj_create(p);
  style_plain(budget_box);
  lv_obj_set_size(budget_box, 160, 28);
  lv_obj_align(budget_box, LV_ALIGN_TOP_RIGHT, -24, 14);
  lv_obj_clear_flag(budget_box, LV_OBJ_FLAG_CLICKABLE);
  budget_pct = mk_label(budget_box, F_SMALL, C_TEXT);
  lv_obj_align(budget_pct, LV_ALIGN_RIGHT_MID, 0, 0);
  budget_bar = lv_bar_create(budget_box);
  lv_obj_set_size(budget_bar, 64, 8);
  lv_obj_align(budget_bar, LV_ALIGN_RIGHT_MID, -46, 0);
  lv_bar_set_range(budget_bar, 0, 100);
  lv_obj_set_style_bg_color(budget_bar, C_SURFACE2, 0);
  lv_obj_set_style_bg_opa(budget_bar, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(budget_bar, 4, 0);
  lv_obj_set_style_radius(budget_bar, 4, LV_PART_INDICATOR);
  lv_obj_t *l5 = mk_label(budget_box, F_SMALL, C_MUTED);
  lv_label_set_text(l5, "5h");
  lv_obj_align(l5, LV_ALIGN_RIGHT_MID, -118, 0);
  lv_obj_add_flag(budget_box, LV_OBJ_FLAG_HIDDEN);

  // stats row
  stats_row = lv_obj_create(p);
  style_plain(stats_row);
  lv_obj_set_size(stats_row, 448, 66);
  lv_obj_set_pos(stats_row, 16, 288);
  lv_obj_set_flex_flow(stats_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(stats_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(stats_row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(stats_row, stats_clicked, LV_EVENT_CLICKED, nullptr);
  for (int i = 0; i < 4; i++) {
    lv_obj_t *c = lv_obj_create(stats_row);
    style_plain(c);
    lv_obj_set_size(c, 104, 66);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
    stat_num[i] = mk_label(c, &lv_font_montserrat_36, C_DIM);
    lv_label_set_text(stat_num[i], "0");
    lv_obj_align(stat_num[i], LV_ALIGN_TOP_MID, 0, 0);
    stat_cap[i] = mk_label(c, F_SMALL, C_DIM);
    lv_label_set_text(stat_cap[i], STAT_CAP[i]);
    lv_obj_align(stat_cap[i], LV_ALIGN_BOTTOM_MID, 0, -2);
  }
  lv_obj_add_flag(stats_row, LV_OBJ_FLAG_HIDDEN);

  // focus card
  card = lv_obj_create(p);
  style_plain(card);
  lv_obj_set_size(card, 448, 86);
  lv_obj_set_pos(card, 16, 362);
  lv_obj_set_style_radius(card, 18, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(card, C_SURFACE, 0);
  lv_obj_set_style_border_color(card, C_BORDER, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_bg_color(card, C_SURFACE2, LV_STATE_PRESSED);
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(card, card_clicked, LV_EVENT_CLICKED, nullptr);

  card_dot = lv_obj_create(card);
  style_plain(card_dot);
  lv_obj_set_size(card_dot, 10, 10);
  lv_obj_set_style_radius(card_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(card_dot, LV_OPA_COVER, 0);
  lv_obj_set_pos(card_dot, 20, 20);
  card_name = mk_label(card, F_BODY, C_MUTED);
  set_single_line(card_name, 290);
  lv_obj_set_pos(card_name, 38, 13);
  card_meta = mk_label(card, F_SMALL, C_MUTED);
  lv_obj_align(card_meta, LV_ALIGN_TOP_RIGHT, -44, 16);
  card_chev = mk_label(card, F_BODY, C_DIM);
  lv_label_set_text(card_chev, LV_SYMBOL_RIGHT);
  lv_obj_align(card_chev, LV_ALIGN_RIGHT_MID, -16, 0);
  card_text = mk_label(card, F_TITLE, C_TEXT);
  set_single_line(card_text, 370);
  lv_obj_set_pos(card_text, 20, 44);

  card_set(C_OFFLINE, LV_SYMBOL_USB "  Not connected", "", "Start the companion daemon on your Mac", F_BODY, false,
           false);

  tmr = lv_timer_create(on_timer, 36, nullptr);
  lv_timer_pause(tmr);
}

static void update(JsonObjectConst s) {
  if (model_host_connected() != host_prev) return;  // timer handles transitions (+ greeting)
  // Skip when nothing home shows changed: sessions/totals/usage parts, the event log (card texts) and the
  // second ("12s ago" meta). Unchanged values are filtered again per widget inside refresh().
  static uint32_t seen[5] = {0, 0, 0, 0, 0};
  uint32_t k[5] = {model_part_version("sessions"), model_part_version("totals"), model_part_version("usage"),
                   (uint32_t)model_event_count(), model_now()};
  if (!memcmp(k, seen, sizeof k)) return;
  memcpy(seen, k, sizeof k);
  refresh(s);
}

static void on_event(JsonObjectConst ev) {
  if (!visible) return;
  const char *k = ev["kind"] | "";
  const char *name = ev["name"] | "agent";
  char buf[112];
  uint32_t now = lv_tick_get();
  if (!strcmp(k, "stop")) {
    static const char *const YAY[] = {"Woohoo!", "Nailed it!", "High five!", "Another one done!", "Yay, %s!",
                                      "Look at %s go!"};
    home_face_react(HOME_REACT_HAPPY);
    snprintf(buf, sizeof(buf), YAY[(now / 7) % 6], name);
    home_face_say(buf, 2600, 2);
  } else if (!strcmp(k, "waiting")) {
    home_face_react(HOME_REACT_SURPRISE);
    snprintf(buf, sizeof(buf), "Psst! %s needs you.", name);
    home_face_say(buf, 3500, 3);
  } else if (!strcmp(k, "tool_error")) {
    home_face_react(HOME_REACT_WINCE);
    static const char *const OUCH[] = {"Ouch!", "Oops...", "That stung.", "Hmm, not quite."};
    home_face_say(OUCH[(now / 7) % 4], 1800, 1);
  } else if (!strcmp(k, "session_start")) {
    home_face_react(HOME_REACT_SURPRISE);
    snprintf(buf, sizeof(buf), "Oh hi, %s!", name);
    home_face_say(buf, 2800, 2);
  } else if (!strcmp(k, "session_end")) {
    snprintf(buf, sizeof(buf), "Bye, %s.", name);
    home_face_say(buf, 2500, 1);
  } else if (!strcmp(k, "todo_done")) {
    home_face_react(HOME_REACT_HOP);
    if (!home_face_bubble_visible()) {
      snprintf(buf, sizeof(buf), "Checked off: %s", ev["text"] | "a todo");
      home_face_say(buf, 2600, 0);
    }
  } else if (!strcmp(k, "subagent_start")) {
    if (!home_face_bubble_visible()) {
      snprintf(buf, sizeof(buf), "%s sent out a helper.", name);
      home_face_say(buf, 2400, 0);
    }
  } else if (!strcmp(k, "compact")) {
    if (!home_face_bubble_visible()) {
      snprintf(buf, sizeof(buf), "Tidying up %s's memory...", name);
      home_face_say(buf, 2600, 0);
    }
  } else if (!strcmp(k, "tool")) {
    if (now - last_glance > 5000) { last_glance = now; home_face_react(HOME_REACT_GLANCE); }
  }
}

static void on_show(bool v) {
  visible = v;
  if (v) {
    home_face_resume();
    update_clock();
    lv_timer_resume(tmr);
  } else {
    lv_timer_pause(tmr);
  }
}

const ScreenDef screen_home = {"home", "Companion", create, update, on_event, on_show};
