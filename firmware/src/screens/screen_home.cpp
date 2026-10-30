// HOME — Clawd's Studio (home_face.cpp: the top-down pixel-art room, 480 x 424) plus a HUD band:
//   room:  Clawd at the station of whatever the agents are doing, the room living on the real clock
//   HUD:   pixel-art band (home_hud.cpp): clock + date | the 5-hour and weekly plan limits, plus the per-model
//          weekly window (e.g. Fable) when the host knows it (tap -> usage)
#include "screens.h"
#include "../ui/theme.h"
#include "../core/model.h"
#include "home_face.h"
#include "home_hud.h"
#include <time.h>

static const int ROOM_H = 424, HUD_Y = 424, HUD_H = 56;

static lv_obj_t *hud, *limits_box;   // limits_box: transparent tap target over the bars (-> usage)
static lv_timer_t *tmr;
static bool visible = false, host_prev = false, greeted = false;
static uint32_t last_glance = 0, last_slow = 0;
static int tap_n = 0;


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
    "The plant says hi.",
    "I watered the plant. You're welcome.",
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

// What Clawd does in the room for a tool (home_face.h tool_cat)
static char tool_category(const char *tool) {
  if (!tool || !*tool) return 'e';
  if (!strcmp(tool, "Read")) return 'r';
  if (!strcmp(tool, "Grep") || !strcmp(tool, "Glob") || !strcmp(tool, "LS")) return 's';
  if (!strcmp(tool, "Edit") || !strcmp(tool, "MultiEdit") || !strcmp(tool, "Write") || !strcmp(tool, "NotebookEdit")) return 'e';
  if (!strncmp(tool, "Bash", 4)) return 'x';
  if (!strncmp(tool, "Web", 3)) return 'w';
  if (!strcmp(tool, "Agent") || !strcmp(tool, "Task")) return 'a';
  if (!strcmp(tool, "TodoWrite")) return 'p';
  if (!strcmp(tool, "thinking")) return 't';
  if (!strcmp(tool, "writing")) return 'e';
  return 'e';
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

// ------------------------------------------------------------------ change-only setters
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
static void refresh(JsonObjectConst s) {
  bool host = model_host_connected();
  int busy = 0, wait = 0, idle = 0, help = 0, alive = 0;
  int todo_done = 0, todo_total = 0;
  JsonObjectConst f_busy;
  for (JsonObjectConst x : s["sessions"].as<JsonArrayConst>()) {
    const char *st = x["status"] | "offline";
    if (!strcmp(st, "busy")) { busy++; if (f_busy.isNull()) f_busy = x; }
    else if (!strcmp(st, "waiting")) wait++;
    else if (!strcmp(st, "idle")) idle++;
    else continue;
    alive++;
    for (JsonObjectConst a : x["subagents"].as<JsonArrayConst>())
      if (!strcmp(a["status"] | "", "busy")) help++;
    todo_done += x["todos"]["done"] | 0;
    todo_total += x["todos"]["total"] | 0;
  }
  if (help == 0) help = s["totals"]["subagents_active"] | 0;
  // one Clawd for the first active (busy or waiting) session, a mini helper for every other active session and
  // every active subagent; idle sessions do not count
  int active = busy + wait;
  int minis = (active > 1 ? active - 1 : 0) + help;

  // Clawd
  char cat = 0;
  if (!f_busy.isNull()) cat = tool_category(f_busy["activity"]["tool"] | "");
  HomeMood m = !host ? HOME_MOOD_OFFLINE
               : alive == 0 ? HOME_MOOD_SLEEP
               : wait ? HOME_MOOD_WAITING
               : busy ? HOME_MOOD_BUSY
                      : HOME_MOOD_IDLE;
  float energy = busy ? 0.35f + 0.22f * (busy - 1) + 0.06f * help : 0.1f;
  if (energy > 1) energy = 1;
  home_face_set_plan(todo_done, todo_total);
  home_face_set_mood(m, energy, host ? minis : 0, cat);

  // plan limits: 5H, 7D and, when the host reports one, the first per-model weekly window (usage.seven_day_models,
  // e.g. Fable). That row reads as stale (grey) when its observation is older than 12 h: the host may only have
  // Claude Code's cached snapshot of it.
  JsonObjectConst mw = s["usage"]["seven_day_models"][0];
  int mw_pct = mw["pct"] | -1;
  static char mw_cap[8];
  HudBar bars[3] = {{"5H", s["usage"]["five_hour"]["pct"] | -1, false},
                    {"7D", s["usage"]["seven_day"]["pct"] | -1, false},
                    {mw_cap, mw_pct, false}};
  int rows = host && mw_pct >= 0 ? 3 : 2;
  if (rows == 3) {
    snprintf(mw_cap, sizeof mw_cap, "%.2s", mw["name"] | "MO");   // two letters like 5H / 7D: FA, OP, SO
    uint32_t now = model_now(), at = mw["at"] | 0u;
    bars[2].stale = now && at && now > at + 12 * 3600;
  }
  bool any = false;
  for (int i = 0; i < rows; i++) any = any || bars[i].pct >= 0;
  bool show = host && any;
  home_hud_set_limits(bars, show ? rows : 0);
  if (show) {
    lv_area_t a;
    home_hud_limits_area(&a);
    lv_obj_set_pos(limits_box, a.x1, 0);
    lv_obj_set_width(limits_box, a.x2 - a.x1 + 1);
  }
  set_hidden(limits_box, !show);
}

static void update_clock() {
  uint32_t now = model_now();
  static uint32_t shown = 1;
  if (!now) {
    if (shown) { home_hud_set_clock("--:--", ""); shown = 0; }
    return;
  }
  time_t lt = (time_t)now + model_tz();
  if ((uint32_t)lt / 60 == shown) return;
  shown = (uint32_t)lt / 60;
  struct tm tm;
  gmtime_r(&lt, &tm);
  static const char *DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *MONS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char hm[8], d[20];
  snprintf(hm, sizeof hm, "%02d:%02d", tm.tm_hour, tm.tm_min);
  snprintf(d, sizeof d, "%s %d %s", DAYS[tm.tm_wday], tm.tm_mday, MONS[tm.tm_mon]);
  home_hud_set_clock(hm, d);
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
    snprintf(lines[n++], 112, "Zzz... start the companion app on your computer and I'll wake up.");
  } else {
    int nb = 0;
    for (JsonObjectConst x : s["sessions"].as<JsonArrayConst>()) {
      if (n >= 5) break;
      const char *st = x["status"] | "";
      if (!strcmp(st, "waiting")) snprintf(lines[n++], 112, "%s is waiting for you. Swipe to Agents!", x["name"] | "?");
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

static void limits_clicked(lv_event_t *) { ui_nav("usage"); }

static void on_timer(lv_timer_t *) {
  home_face_tick(visible);
  uint32_t now = lv_tick_get();
  if (now - last_slow < 500) return;
  last_slow = now;
  if (visible) update_clock();
  bool host = model_host_connected();
  if (host != host_prev) {
    host_prev = host;
    refresh(model_state());
    if (host && !greeted) { greeted = true; if (visible) greet(); }
    if (!host) greeted = false;
  }
}

// ------------------------------------------------------------------ ScreenDef
static void create(lv_obj_t *p) {
  home_face_create(p, 0);
  home_face_on_tap(face_tapped);

  // HUD band (pixel art, drawn by home_hud.cpp) with a transparent tap target over the limit bars -> usage.
  // The tileview's page dots sit bottom-centre.
  hud = home_hud_create(p, HUD_Y, HUD_H);
  limits_box = lv_obj_create(hud);
  style_plain(limits_box);
  lv_obj_set_size(limits_box, 160, HUD_H);
  lv_obj_set_pos(limits_box, 320, 0);
  lv_obj_add_flag(limits_box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(limits_box, limits_clicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(limits_box, LV_OBJ_FLAG_HIDDEN);

  tmr = lv_timer_create(on_timer, 36, nullptr);   // keeps running while hidden: the room lives on, drawing is skipped
}

static void update(JsonObjectConst s) {
  if (model_host_connected() != host_prev) return;  // timer handles transitions (+ greeting)
  static uint32_t seen[3] = {0, 0, 0};
  uint32_t k[3] = {model_part_version("sessions"), model_part_version("totals"), model_part_version("usage")};
  if (!memcmp(k, seen, sizeof k)) return;
  memcpy(seen, k, sizeof k);
  refresh(s);
}

static void on_event(JsonObjectConst ev) {
  const char *k = ev["kind"] | "";
  const char *name = ev["name"] | "agent";
  char buf[112];
  uint32_t now = lv_tick_get();
  if (!strcmp(k, "stop")) {
    static const char *const YAY[] = {"Woohoo!", "Nailed it!", "High five!", "Another one done!", "Yay, %s!",
                                      "Look at %s go!"};
    home_face_react(HOME_REACT_HAPPY);
    if (!visible) return;
    snprintf(buf, sizeof(buf), YAY[(now / 7) % 6], name);
    home_face_say(buf, 2600, 2);
  } else if (!strcmp(k, "waiting")) {
    home_face_react(HOME_REACT_SURPRISE);
    if (!visible) return;
    snprintf(buf, sizeof(buf), "Psst! %s needs you.", name);
    home_face_say(buf, 3500, 3);
  } else if (!strcmp(k, "tool_error")) {
    home_face_react(HOME_REACT_WINCE);
    if (!visible) return;
    static const char *const OUCH[] = {"Ouch!", "Oops...", "That stung.", "Hmm, not quite."};
    home_face_say(OUCH[(now / 7) % 4], 1800, 1);
  } else if (!strcmp(k, "session_start")) {
    home_face_react(HOME_REACT_SURPRISE);
    if (!visible) return;
    snprintf(buf, sizeof(buf), "Oh hi, %s!", name);
    home_face_say(buf, 2800, 2);
  } else if (!strcmp(k, "session_end")) {
    if (!visible) return;
    snprintf(buf, sizeof(buf), "Bye, %s.", name);
    home_face_say(buf, 2500, 1);
  } else if (!strcmp(k, "todo_done")) {
    home_face_react(HOME_REACT_HOP);
    if (!visible) return;
    if (!home_face_bubble_visible()) {
      snprintf(buf, sizeof(buf), "Checked off: %s", ev["text"] | "a todo");
      home_face_say(buf, 2600, 0);
    }
  } else if (!strcmp(k, "subagent_start")) {
    if (!visible) return;
    if (!home_face_bubble_visible()) {
      snprintf(buf, sizeof(buf), "%s sent out a helper.", name);
      home_face_say(buf, 2400, 0);
    }
  } else if (!strcmp(k, "compact")) {
    if (!visible) return;
    if (!home_face_bubble_visible()) {
      snprintf(buf, sizeof(buf), "Tidying up %s's memory...", name);
      home_face_say(buf, 2600, 0);
    }
  } else if (!strcmp(k, "tool")) {
    if (visible && now - last_glance > 20000) { last_glance = now; home_face_react(HOME_REACT_GLANCE); }
  }
}

static void on_show(bool v) {
  visible = v;
  if (v) {
    home_face_resume();
    update_clock();
  }
}

const ScreenDef screen_home = {"home", "Companion", create, update, on_event, on_show};
