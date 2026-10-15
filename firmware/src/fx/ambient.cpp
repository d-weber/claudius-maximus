// Ambient behaviour: auto-dim, screensaver / night clock, wake on touch or agent activity.
// See ambient.h for the state machine. Everything runs from ambient_tick() (throttled to 10 Hz) + event hooks.
#include "ambient.h"
#include <Arduino.h>
#include <lvgl.h>
#include <time.h>
#include "../ui/theme.h"
#include "../ui/ui.h"
#include "../core/hal.h"
#include "../core/model.h"
#include "../core/settings.h"

extern "C" const lv_font_t ambient_font_clock;   // ambient_font_clock.c (digits + ':' @120 px)

enum Level { L_ACTIVE, L_DIM, L_SAVER };
static Level level = L_ACTIVE;

static uint32_t last_tick = 0;
static uint32_t seen_touch = 0;
static uint32_t last_user = 0;       // touch / navigation
static uint32_t last_activity = 0;   // any agent event / status change
static uint32_t last_urgent = 0;     // waiting event / new waiting session
static char last_screen[16] = "";
static bool forced = false;
static uint16_t ov_dim_s = 0, ov_saver_s = 0;   // dev overrides (RAM only)

// backlight
static int bri_cur = -1, bri_target = -1;
static uint8_t seen_set_bri = 0;
static bool was_night = false;

// counts from the latest state (for the saver line)
static int n_busy = 0, n_wait = 0, n_idle = 0, n_sub = 0, n_sessions = 0;
static char wait_name[40] = "";
static uint32_t status_sig = 0;
static bool have_state = false;

// widgets
static lv_obj_t *shield, *saver, *box, *clock_lbl, *date_lbl, *line_row, *line_dot, *line_lbl;
static bool shield_pressed = false;
static int shown_min = -1;
static char shown_line[64] = "";
static uint32_t drift_ms = 0;

static void logf(const char *fmt, ...) {
  char b[120];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  Serial.printf("{\"t\":\"log\",\"msg\":\"ambient: %s\"}\n", b);
}

static bool is_night() {
  const Settings &s = settings();
  if (!s.night_mode) return false;
  uint32_t now = model_now();
  if (!now) return false;
  int h = (int)(((int64_t)now + model_tz()) / 3600 % 24);
  if (h < 0) h += 24;
  if (s.night_start_h == s.night_end_h) return false;
  if (s.night_start_h > s.night_end_h) return h >= s.night_start_h || h < s.night_end_h;
  return h >= s.night_start_h && h < s.night_end_h;
}

static uint16_t dim_s() { return ov_dim_s ? ov_dim_s : settings().dim_after_s; }
static uint16_t saver_s() { return ov_saver_s ? ov_saver_s : settings().saver_after_s; }

static int level_brightness(Level l, bool night) {
  const Settings &s = settings();
  int b = s.brightness, d = s.dim_brightness;
  if (night) {
    int nb = max(d, b * 4 / 10);
    if (l == L_ACTIVE) return nb;
    return max(2, d / 2);
  }
  if (l == L_ACTIVE) return b;
  return min(d, b);
}

static void apply_brightness(bool instant) {
  int t = level_brightness(level, is_night());
  bri_target = t;
  if (instant || bri_cur < 0 || t > bri_cur) {
    bri_cur = t;
    hal_set_brightness(t);
  }
}

// ------------------------------------------------------------------------------------------------- saver
static void refresh_line() {
  char txt[64];
  lv_color_t c;
  if (!have_state || !model_host_connected()) {
    snprintf(txt, sizeof txt, "host offline");
    c = C_DIM;
  } else if (n_wait == 1 && wait_name[0]) {
    snprintf(txt, sizeof txt, "%s needs you", wait_name);
    c = C_WAITING;
  } else if (n_wait > 0) {
    snprintf(txt, sizeof txt, "%d agents need you", n_wait);
    c = C_WAITING;
  } else if (n_busy > 0) {
    if (n_sub > 0) snprintf(txt, sizeof txt, "%d agent%s working, %d subagent%s", n_busy, n_busy > 1 ? "s" : "", n_sub, n_sub > 1 ? "s" : "");
    else snprintf(txt, sizeof txt, "%d agent%s working", n_busy, n_busy > 1 ? "s" : "");
    c = C_BUSY;
  } else if (n_sessions > 0) {
    snprintf(txt, sizeof txt, "all quiet");
    c = C_IDLE;
  } else {
    snprintf(txt, sizeof txt, "no agents running");
    c = C_DIM;
  }
  if (strcmp(txt, shown_line)) {
    strlcpy(shown_line, txt, sizeof shown_line);
    lv_label_set_text(line_lbl, txt);
  }
  lv_obj_set_style_text_color(line_lbl, c, 0);
  lv_obj_set_style_bg_color(line_dot, c, 0);
  // waiting deserves more presence than "working"
  lv_obj_set_style_text_opa(line_lbl, n_wait ? LV_OPA_COVER : LV_OPA_80, 0);
}

static void refresh_clock(bool force) {
  uint32_t now = model_now();
  bool night = is_night();
  lv_obj_set_style_text_color(clock_lbl, night ? lv_color_hex(0x9A7462) : lv_color_hex(0xB3ACA2), 0);
  if (!now) {
    if (force || shown_min != -2) {
      lv_label_set_text(clock_lbl, "--:--");
      lv_label_set_text(date_lbl, "");
      shown_min = -2;
    }
    return;
  }
  time_t t = (time_t)((int64_t)now + model_tz());
  struct tm tm;
  gmtime_r(&t, &tm);
  int m = tm.tm_hour * 60 + tm.tm_min;
  if (!force && m == shown_min) return;
  shown_min = m;
  lv_label_set_text_fmt(clock_lbl, "%02d:%02d", tm.tm_hour, tm.tm_min);
  static const char *WD[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  static const char *MO[] = {"January", "February", "March", "April", "May", "June", "July",
                             "August", "September", "October", "November", "December"};
  lv_label_set_text_fmt(date_lbl, "%s %d %s", WD[tm.tm_wday % 7], tm.tm_mday, MO[tm.tm_mon % 12]);
}

// Slow Lissajous drift, re-positioned every 20 s (one small redraw), so no pixel stays lit for long.
static void drift(bool force) {
  uint32_t now = millis();
  if (!force && now - drift_ms < 20000) return;
  drift_ms = now;
  float k = now / 1000.0f / 60.0f;  // minutes
  int dx = (int)(36 * sinf(k * 0.71f + 0.3f));
  int dy = (int)(64 * sinf(k * 0.43f + 1.1f));
  lv_obj_align(box, LV_ALIGN_CENTER, dx, dy);
}

static void touch_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    shield_pressed = true;
    ambient_wake();
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    shield_pressed = false;
    if (level == L_ACTIVE) {
      lv_obj_add_flag(shield, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(saver, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void fade_cb(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }

static void show_saver(bool on) {
  if (on) {
    if (!lv_obj_has_flag(saver, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_move_background(saver);  // stay below alert cards / glow created by fx/alerts.cpp
    refresh_clock(true);
    refresh_line();
    drift(true);
    lv_obj_clear_flag(saver, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, saver);
    lv_anim_set_exec_cb(&a, fade_cb);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_time(&a, 700);
    lv_anim_start(&a);
  } else {
    lv_anim_del(saver, fade_cb);
    lv_obj_set_style_opa(saver, LV_OPA_COVER, 0);
    lv_obj_add_flag(saver, LV_OBJ_FLAG_HIDDEN);
  }
}

static void set_level(Level l, const char *why) {
  if (l == level) return;
  Level prev = level;
  level = l;
  bool night = is_night();
  logf("%s -> %s (%s%s)", prev == L_ACTIVE ? "active" : prev == L_DIM ? "dim" : "saver",
       l == L_ACTIVE ? "active" : l == L_DIM ? "dim" : "saver", why, night ? ", night" : "");
  if (l == L_ACTIVE) {
    // keep the layer that took the press until the finger lifts (swallows the waking touch)
    if (!shield_pressed) {
      lv_obj_add_flag(shield, LV_OBJ_FLAG_HIDDEN);
      show_saver(false);
    } else if (!lv_obj_has_flag(saver, LV_OBJ_FLAG_HIDDEN)) {
      // saver is the one being pressed: let it vanish now, an invisible shield takes over
      lv_obj_move_background(shield);
      lv_obj_clear_flag(shield, LV_OBJ_FLAG_HIDDEN);
      show_saver(false);
    }
    apply_brightness(true);
  } else {
    lv_obj_move_background(shield);
    lv_obj_clear_flag(shield, LV_OBJ_FLAG_HIDDEN);
    show_saver(l == L_SAVER);
    apply_brightness(false);
  }
}

// ------------------------------------------------------------------------------------------------- api
void ambient_init() {
  lv_obj_t *top = lv_layer_top();

  shield = lv_obj_create(top);
  lv_obj_remove_style_all(shield);
  lv_obj_set_size(shield, 480, 480);
  lv_obj_add_flag(shield, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(shield, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(shield, touch_cb, LV_EVENT_ALL, NULL);

  saver = lv_obj_create(top);
  lv_obj_remove_style_all(saver);
  lv_obj_set_size(saver, 480, 480);
  lv_obj_set_style_bg_color(saver, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(saver, LV_OPA_COVER, 0);
  lv_obj_add_flag(saver, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(saver, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(saver, touch_cb, LV_EVENT_ALL, NULL);

  box = lv_obj_create(saver);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(box, 10, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  clock_lbl = lv_label_create(box);
  lv_obj_set_style_text_font(clock_lbl, &ambient_font_clock, 0);
  lv_obj_set_style_text_letter_space(clock_lbl, -2, 0);
  lv_label_set_text(clock_lbl, "--:--");

  date_lbl = lv_label_create(box);
  lv_obj_set_style_text_font(date_lbl, F_BODY, 0);
  lv_obj_set_style_text_color(date_lbl, lv_color_hex(0x6E685F), 0);
  lv_obj_set_style_pad_top(date_lbl, 4, 0);
  lv_label_set_text(date_lbl, "");

  line_row = lv_obj_create(box);
  lv_obj_remove_style_all(line_row);
  lv_obj_set_size(line_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(line_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(line_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(line_row, 10, 0);
  lv_obj_set_style_pad_top(line_row, 18, 0);
  lv_obj_clear_flag(line_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  line_dot = lv_obj_create(line_row);
  lv_obj_remove_style_all(line_dot);
  lv_obj_set_size(line_dot, 9, 9);
  lv_obj_set_style_radius(line_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(line_dot, LV_OPA_COVER, 0);
  line_lbl = lv_label_create(line_row);
  lv_obj_set_style_text_font(line_lbl, F_BODY, 0);
  lv_label_set_text(line_lbl, "");

  // both live at the very bottom of the top layer: alerts (created before and after) sit above them
  lv_obj_move_background(saver);
  lv_obj_move_background(shield);

  uint32_t now = millis();
  last_user = last_activity = now;
  last_urgent = 0;
  seen_touch = hal_last_touch_ms();
  seen_set_bri = settings().brightness;
  was_night = is_night();
}

void ambient_wake() {
  uint32_t now = millis();
  last_user = now;
  forced = false;
  set_level(L_ACTIVE, "touch");
}

void ambient_force_saver(bool on) {
  forced = on;
  if (on) set_level(L_SAVER, "forced");
  else { last_user = millis(); set_level(L_ACTIVE, "release"); }
}

bool ambient_saver_active() { return saver && !lv_obj_has_flag(saver, LV_OBJ_FLAG_HIDDEN); }
bool ambient_dimmed() { return level != L_ACTIVE; }

static void activity(bool urgent, const char *why) {
  uint32_t now = millis();
  last_activity = now;
  if (urgent) {
    last_urgent = now;
    forced = false;
  }
  if (forced) return;
  if (level != L_ACTIVE && (urgent || !is_night())) set_level(L_ACTIVE, why);
}

void ambient_update(JsonObjectConst s) {
  int b = 0, w = 0, i = 0, sub = 0, n = 0;
  uint32_t sig = 2166136261u;
  const char *wn = "";
  for (JsonObjectConst x : s["sessions"].as<JsonArrayConst>()) {
    const char *st = x["status"] | "offline";
    n++;
    if (!strcmp(st, "busy")) b++;
    else if (!strcmp(st, "waiting")) { w++; if (!wn[0]) wn = x["name"] | (x["project"] | ""); }
    else if (!strcmp(st, "idle")) i++;
    for (JsonObjectConst a : x["subagents"].as<JsonArrayConst>())
      if (!strcmp(a["status"] | "", "busy")) sub++;
    for (const char *p = x["id"] | ""; *p; p++) sig = (sig ^ (uint8_t)*p) * 16777619u;
    for (const char *p = st; *p; p++) sig = (sig ^ (uint8_t)*p) * 16777619u;
  }
  sig ^= (uint32_t)(s["totals"]["tools_today"] | 0) * 2654435761u;
  bool new_wait = have_state && w > n_wait;
  bool changed = have_state && sig != status_sig;
  n_busy = b; n_wait = w; n_idle = i; n_sub = sub; n_sessions = n;
  strlcpy(wait_name, wn, sizeof wait_name);
  status_sig = sig;
  have_state = true;
  if (new_wait) activity(true, "waiting");
  else if (changed) activity(false, "state");
  if (ambient_saver_active()) refresh_line();
}

void ambient_on_event(JsonObjectConst ev) {
  const char *k = ev["kind"] | "";
  if (!strcmp(k, "dev_ambient")) {
    if (!ev["dim_s"].isNull()) ov_dim_s = ev["dim_s"] | 0;
    if (!ev["saver_s"].isNull()) ov_saver_s = ev["saver_s"] | 0;
    if (!ev["saver"].isNull()) ambient_force_saver((ev["saver"] | 0) != 0);
    logf("dev dim_s=%u saver_s=%u forced=%d", dim_s(), saver_s(), (int)forced);
    return;
  }
  activity(!strcmp(k, "waiting"), k);
}

void ambient_tick() {
  uint32_t now = millis();
  if (now - last_tick < 100) return;
  last_tick = now;

  // user input: touch anywhere (also on alert cards above the shield) or tile navigation (incl. host cmd)
  uint32_t t = hal_last_touch_ms();
  if (t != seen_touch) {
    seen_touch = t;
    last_user = now;
    if (level != L_ACTIVE) { forced = false; set_level(L_ACTIVE, "touch"); }
  }
  const char *cur = ui_current();
  if (cur && strcmp(cur, last_screen)) {
    bool first = !last_screen[0];
    strlcpy(last_screen, cur, sizeof last_screen);
    if (!first) {
      last_user = now;
      if (level != L_ACTIVE) { forced = false; set_level(L_ACTIVE, "nav"); }
    }
  }

  bool night = is_night();
  if (!forced) {
    uint32_t ref = night ? max(last_user, last_urgent) : max(last_user, last_activity);
    uint32_t idle = now - ref;
    uint32_t dms = (uint32_t)dim_s() * 1000, sms = (uint32_t)saver_s() * 1000;
    Level want = L_ACTIVE;
    if (sms && idle >= sms) want = L_SAVER;
    else if (dms && idle >= dms) want = night ? L_SAVER : L_DIM;
    // only ever go *down* from the timer; waking is driven by input/events above
    if (want > level) set_level(want, "idle");
  }

  // settings slider moved / night boundary crossed -> follow
  if (settings().brightness != seen_set_bri || night != was_night) {
    seen_set_bri = settings().brightness;
    was_night = night;
    apply_brightness(level == L_ACTIVE);
  }
  // smooth dim ramp
  if (bri_cur > bri_target && bri_target >= 0) {
    int step = max(1, (bri_cur - bri_target) / 5);
    bri_cur -= step;
    hal_set_brightness(bri_cur);
  }

  if (ambient_saver_active()) {
    refresh_clock(false);
    drift(false);
    static uint32_t line_ms = 0;
    if (now - line_ms > 2000) { line_ms = now; refresh_line(); }
  }
}
