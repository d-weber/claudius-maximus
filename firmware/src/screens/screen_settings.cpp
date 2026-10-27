// Settings tile: display (brightness, dim, timeouts), alerts, night mode, about.
// Vertical scroll inside the tile; every control is >= 48 px tall. Changes apply live and are
// persisted with a debounced settings_save_later().
#include "../core/link.h"
#include "screens.h"
#include "../ui/theme.h"
#include "../core/settings.h"
#include "../core/hal.h"
#include "../core/model.h"
#include "../fx/alerts.h"
#include <esp_heap_caps.h>
#include <stdarg.h>
#include <Wire.h>
#include "WS_CH32_IO.h"

#ifndef FW_VERSION
#define FW_VERSION "0.1.0"
#endif

static lv_obj_t *cont;
static lv_obj_t *bri_slider, *bri_val, *dim_slider, *dim_val;
static lv_obj_t *dim_after_bm, *saver_bm;
static lv_obj_t *sw_wait, *sw_done, *sw_err, *sw_sub, *sw_night;
static lv_obj_t *night_from_val, *night_to_val;
static lv_obj_t *ab_host, *ab_sessions, *ab_heap, *ab_psram, *ab_uptime, *ab_bl;
static lv_timer_t *about_timer;

static const uint16_t DIM_AFTER[] = {0, 60, 180, 300, 600};
static const char *DIM_AFTER_MAP[] = {"Never", "1 min", "3 min", "5 min", "10 min", ""};
static const uint16_t SAVER_AFTER[] = {0, 300, 900, 1800, 3600};
static const char *SAVER_MAP[] = {"Never", "5 min", "15 min", "30 min", "1 h", ""};

// ---------------------------------------------------------------------------------------------
// building blocks
static lv_obj_t *section(const char *title) {
  lv_obj_t *l = lv_label_create(cont);
  lv_label_set_text(l, title);
  lv_obj_set_style_text_font(l, F_SMALL, 0);
  lv_obj_set_style_text_color(l, C_ACCENT, 0);
  lv_obj_set_style_text_letter_space(l, 2, 0);
  lv_obj_set_style_pad_top(l, 14, 0);
  lv_obj_set_style_pad_left(l, 6, 0);
  lv_obj_t *g = lv_obj_create(cont);
  lv_obj_remove_style_all(g);
  lv_obj_set_size(g, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(g, C_SURFACE, 0);
  lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(g, 18, 0);
  lv_obj_set_style_pad_hor(g, 18, 0);
  lv_obj_set_style_pad_ver(g, 8, 0);
  lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);
  return g;
}

static lv_obj_t *row(lv_obj_t *g, lv_coord_t h) {
  lv_obj_t *r = lv_obj_create(g);
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, LV_PCT(100), h);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
  return r;
}

static lv_obj_t *text(lv_obj_t *p, const char *t, const lv_font_t *f, lv_color_t c) {
  lv_obj_t *l = lv_label_create(p);
  lv_label_set_text(l, t);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  return l;
}

static void divider(lv_obj_t *g) {
  lv_obj_t *d = lv_obj_create(g);
  lv_obj_remove_style_all(d);
  lv_obj_set_size(d, LV_PCT(100), 1);
  lv_obj_set_style_bg_color(d, C_BORDER, 0);
  lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
}

static lv_obj_t *slider_row(lv_obj_t *g, const char *title, lv_obj_t **val, int min, int max, int v) {
  lv_obj_t *r = row(g, 84);
  lv_obj_t *t = text(r, title, F_BODY, C_TEXT);
  lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 10);
  *val = text(r, "", F_BODY, C_ACCENT2);
  lv_obj_align(*val, LV_ALIGN_TOP_RIGHT, 0, 10);
  lv_obj_t *s = lv_slider_create(r);
  lv_obj_set_size(s, LV_PCT(92), 10);
  lv_obj_align(s, LV_ALIGN_BOTTOM_MID, 0, -18);
  lv_slider_set_range(s, min, max);
  lv_slider_set_value(s, v, LV_ANIM_OFF);
  lv_obj_set_ext_click_area(s, 22);
  lv_obj_set_style_bg_color(s, C_SURFACE2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, C_ACCENT, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, C_TEXT, LV_PART_KNOB);
  lv_obj_set_style_pad_all(s, 10, LV_PART_KNOB);
  lv_obj_set_style_shadow_width(s, 0, LV_PART_KNOB);
  lv_obj_set_style_bg_color(s, C_ACCENT2, LV_PART_KNOB | LV_STATE_PRESSED);
  return s;
}

static lv_obj_t *seg_row(lv_obj_t *g, const char *title, const char **map, int sel) {
  lv_obj_t *r = row(g, 104);
  lv_obj_t *t = text(r, title, F_BODY, C_TEXT);
  lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 10);
  lv_obj_t *bm = lv_btnmatrix_create(r);
  lv_btnmatrix_set_map(bm, map);
  lv_btnmatrix_set_btn_ctrl_all(bm, LV_BTNMATRIX_CTRL_CHECKABLE);
  lv_btnmatrix_set_one_checked(bm, true);
  lv_btnmatrix_set_btn_ctrl(bm, sel, LV_BTNMATRIX_CTRL_CHECKED);
  lv_obj_set_size(bm, LV_PCT(100), 54);
  lv_obj_align(bm, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(bm, 0, 0);
  lv_obj_set_style_pad_all(bm, 0, 0);
  lv_obj_set_style_pad_gap(bm, 6, 0);
  lv_obj_set_style_radius(bm, 0, 0);
  lv_obj_set_style_bg_color(bm, C_SURFACE2, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(bm, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_radius(bm, 12, LV_PART_ITEMS);
  lv_obj_set_style_border_width(bm, 0, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(bm, 0, LV_PART_ITEMS);
  lv_obj_set_style_text_color(bm, C_MUTED, LV_PART_ITEMS);
  lv_obj_set_style_text_font(bm, &lv_font_montserrat_16, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(bm, C_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(bm, lv_color_white(), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(bm, C_BORDER, LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_outline_width(bm, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
  lv_obj_set_style_outline_width(bm, 0, LV_PART_ITEMS | LV_STATE_FOCUSED);
  return bm;
}

static void row_toggle_cb(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_user_data(e);
  if (lv_obj_has_state(sw, LV_STATE_CHECKED)) lv_obj_clear_state(sw, LV_STATE_CHECKED);
  else lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_event_send(sw, LV_EVENT_VALUE_CHANGED, NULL);
}

static void switch_cb(lv_event_t *e) {
  bool *field = (bool *)lv_event_get_user_data(e);
  *field = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  settings_save_later();
}

static lv_obj_t *switch_row(lv_obj_t *g, const char *title, const char *sub, bool *field) {
  lv_obj_t *r = row(g, 66);
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *t = text(r, title, F_BODY, C_TEXT);
  lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, sub ? -10 : 0);
  if (sub) {
    lv_obj_t *s = text(r, sub, F_SMALL, C_MUTED);
    lv_obj_align(s, LV_ALIGN_LEFT_MID, 0, 13);
  }
  lv_obj_t *sw = lv_switch_create(r);
  lv_obj_set_size(sw, 64, 36);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_ext_click_area(sw, 8);
  lv_obj_set_style_bg_color(sw, C_SURFACE2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sw, C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(sw, C_TEXT, LV_PART_KNOB);
  lv_obj_set_style_pad_all(sw, -4, LV_PART_KNOB);
  lv_obj_set_style_outline_width(sw, 0, LV_STATE_FOCUS_KEY);
  if (*field) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, field);
  lv_obj_add_event_cb(r, row_toggle_cb, LV_EVENT_CLICKED, sw);
  return sw;
}

static lv_obj_t *round_btn(lv_obj_t *p, const char *sym, lv_event_cb_t cb, void *ud) {
  lv_obj_t *b = lv_obj_create(p);
  lv_obj_remove_style_all(b);
  lv_obj_set_size(b, 52, 48);
  lv_obj_set_style_radius(b, 14, 0);
  lv_obj_set_style_bg_color(b, C_SURFACE2, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, C_BORDER, LV_STATE_PRESSED);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t *l = text(b, sym, F_BODY, C_TEXT);
  lv_obj_center(l);
  return b;
}

static void hour_text(lv_obj_t *l, uint8_t h) { lv_label_set_text_fmt(l, "%02u:00", h); }

static void hour_step_cb(lv_event_t *e) {
  intptr_t code = (intptr_t)lv_event_get_user_data(e);  // 0/1 = from -/+, 2/3 = to -/+
  Settings &s = settings();
  uint8_t &h = code < 2 ? s.night_start_h : s.night_end_h;
  h = (h + ((code & 1) ? 1 : 23)) % 24;
  hour_text(night_from_val, s.night_start_h);
  hour_text(night_to_val, s.night_end_h);
  settings_save_later();
}

static lv_obj_t *stepper_row(lv_obj_t *g, const char *title, uint8_t v, intptr_t base) {
  lv_obj_t *r = row(g, 64);
  lv_obj_t *t = text(r, title, F_BODY, C_TEXT);
  lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *plus = round_btn(r, LV_SYMBOL_PLUS, hour_step_cb, (void *)(base + 1));
  lv_obj_align(plus, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_t *val = text(r, "", F_TITLE, C_TEXT);
  lv_obj_set_width(val, 92);
  lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(val, LV_ALIGN_RIGHT_MID, -56, 0);
  hour_text(val, v);
  lv_obj_t *minus = round_btn(r, LV_SYMBOL_MINUS, hour_step_cb, (void *)base);
  lv_obj_align(minus, LV_ALIGN_RIGHT_MID, -152, 0);
  return val;
}

static lv_obj_t *about_row(lv_obj_t *g, const char *key) {
  lv_obj_t *r = row(g, 40);
  lv_obj_t *k = text(r, key, F_BODY, C_MUTED);
  lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *v = text(r, "-", F_BODY, C_TEXT);
  lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
  return v;
}

// ---------------------------------------------------------------------------------------------
// callbacks
static void bri_cb(lv_event_t *) {
  int v = lv_slider_get_value(bri_slider);
  settings().brightness = v;
  hal_set_brightness(v);
  lv_label_set_text_fmt(bri_val, "%d%%", v);
  settings_save_later();
}

static void dim_cb(lv_event_t *e) {
  lv_event_code_t c = lv_event_get_code(e);
  int v = lv_slider_get_value(dim_slider);
  if (c == LV_EVENT_VALUE_CHANGED) {
    settings().dim_brightness = v;
    lv_label_set_text_fmt(dim_val, v ? "%d%%" : "off", v);
    hal_set_brightness(LV_MAX(v, 1));  // live preview of the dim level while dragging
    settings_save_later();
  } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
    hal_set_brightness(settings().brightness);
  }
}

static void seg_cb(lv_event_t *e) {
  lv_obj_t *bm = lv_event_get_target(e);
  uint16_t i = lv_btnmatrix_get_selected_btn(bm);
  if (i == LV_BTNMATRIX_BTN_NONE || i > 4) return;
  if (bm == dim_after_bm) settings().dim_after_s = DIM_AFTER[i];
  else settings().saver_after_s = SAVER_AFTER[i];
  settings_save_later();
}

static void preview_cb(lv_event_t *e) { alerts_demo((const char *)lv_event_get_user_data(e)); }

static int nearest(const uint16_t *opts, uint16_t v) {
  int best = 0;
  for (int i = 1; i < 5; i++)
    if (abs((int)opts[i] - (int)v) < abs((int)opts[best] - (int)v)) best = i;
  return best;
}

static void fmt_bytes(char *out, size_t n, size_t b) {
  if (b < 1024) snprintf(out, n, "%u B", (unsigned)b);
  else if (b >= 1024 * 1024) snprintf(out, n, "%u.%u MB", (unsigned)(b >> 20), (unsigned)((b & 0xFFFFF) * 10 >> 20));
  else snprintf(out, n, "%u KB", (unsigned)(b >> 10));
}

// Only touch labels whose text/colour actually changed: lv_label_set_text() and style setters
// always invalidate, and this runs every second while the tile is visible.
static void set_if(lv_obj_t *l, const char *fmt, ...) {
  char buf[64];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (strcmp(lv_label_get_text(l), buf)) lv_label_set_text(l, buf);
}

static void about_refresh(lv_timer_t *) {
  static int host_state = -1;
  bool up = model_host_connected();
  int hs = up ? 2 : model_state_version() ? 1 : 0;
  if (hs != host_state) {
    host_state = hs;
    lv_label_set_text(ab_host, hs == 2 ? LV_SYMBOL_OK " Connected" : hs == 1 ? "Lost" : "Not connected");
    lv_obj_set_style_text_color(ab_host, up ? C_IDLE : C_ERROR, 0);
  }
  JsonObjectConst st = model_state();
  JsonObjectConst t = st["totals"];
  int n = st["sessions"].size();
  int w = t["waiting"] | 0;
  if (w) set_if(ab_sessions, "%d  (%d busy, %d waiting)", n, (int)(t["busy"] | 0), w);
  else set_if(ab_sessions, "%d  (%d busy)", n, (int)(t["busy"] | 0));
  char a[24], b[24];
  // KB resolution: byte-level heap jitter would otherwise redraw these rows every second
  fmt_bytes(a, sizeof a, heap_caps_get_free_size(MALLOC_CAP_INTERNAL) & ~(size_t)1023);
  fmt_bytes(b, sizeof b, heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) & ~(size_t)1023);
  set_if(ab_heap, "%s  (max %s)", a, b);
  fmt_bytes(a, sizeof a, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  fmt_bytes(b, sizeof b, heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
  set_if(ab_psram, "%s / %s", a, b);
  uint32_t s = millis() / 1000;
  if (s < 3600) set_if(ab_uptime, "%um %02us", (unsigned)(s / 60), (unsigned)(s % 60));
  else if (s < 86400) set_if(ab_uptime, "%uh %02um", (unsigned)(s / 3600), (unsigned)(s / 60 % 60));
  else set_if(ab_uptime, "%ud %uh", (unsigned)(s / 86400), (unsigned)(s / 3600 % 24));
  set_if(ab_bl, "%u%%", hal_get_brightness());
}

// ---------------------------------------------------------------------------------------------
static void create(lv_obj_t *p) {
  Settings &s = settings();
  cont = lv_obj_create(p);
  lv_obj_remove_style_all(cont);
  lv_obj_set_size(cont, 480, 480);
  lv_obj_set_style_pad_hor(cont, 22, 0);
  lv_obj_set_style_pad_top(cont, 26, 0);
  lv_obj_set_style_pad_bottom(cont, 44, 0);
  lv_obj_set_style_pad_row(cont, 8, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(cont, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(cont, C_DIM, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(cont, 4, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_right(cont, 4, LV_PART_SCROLLBAR);

  lv_obj_t *hdr = row(cont, 44);
  lv_obj_t *h = text(hdr, "Settings", F_LARGE, C_TEXT);
  lv_obj_align(h, LV_ALIGN_LEFT_MID, 6, 0);
  lv_obj_t *gear = text(hdr, LV_SYMBOL_SETTINGS, F_TITLE, C_ACCENT);
  lv_obj_align(gear, LV_ALIGN_RIGHT_MID, -6, 0);

  // DISPLAY
  lv_obj_t *g = section("DISPLAY");
  bri_slider = slider_row(g, "Brightness", &bri_val, 5, 100, s.brightness);
  lv_label_set_text_fmt(bri_val, "%d%%", s.brightness);
  lv_obj_add_event_cb(bri_slider, bri_cb, LV_EVENT_VALUE_CHANGED, NULL);
  divider(g);
  dim_slider = slider_row(g, "Dimmed level", &dim_val, 0, 50, s.dim_brightness);
  lv_label_set_text_fmt(dim_val, s.dim_brightness ? "%d%%" : "off", s.dim_brightness);
  lv_obj_add_event_cb(dim_slider, dim_cb, LV_EVENT_ALL, NULL);
  divider(g);
  dim_after_bm = seg_row(g, "Dim after", DIM_AFTER_MAP, nearest(DIM_AFTER, s.dim_after_s));
  lv_obj_add_event_cb(dim_after_bm, seg_cb, LV_EVENT_VALUE_CHANGED, NULL);
  divider(g);
  saver_bm = seg_row(g, "Screensaver after", SAVER_MAP, nearest(SAVER_AFTER, s.saver_after_s));
  lv_obj_add_event_cb(saver_bm, seg_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // CLAWD
  g = section("CLAWD");
  switch_row(g, "Mirror the desk", "Flip the home scene left / right", &s.home_mirror);

  // ALERTS
  g = section("ALERTS");
  sw_wait = switch_row(g, "Needs you", "Card + glowing edges", &s.alert_waiting);
  divider(g);
  sw_done = switch_row(g, "Finished", "Toast + confetti when done", &s.alert_done);
  divider(g);
  sw_err = switch_row(g, "Tool errors", "Short red flash", &s.alert_errors);
  divider(g);
  sw_sub = switch_row(g, "Subagents", "Small chips on start / finish", &s.alert_subagents);
  divider(g);
  lv_obj_t *pr = row(g, 64);
  lv_obj_t *pt = text(pr, "Preview", F_BODY, C_TEXT);
  lv_obj_align(pt, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *pbox = lv_obj_create(pr);
  lv_obj_remove_style_all(pbox);
  lv_obj_set_size(pbox, LV_SIZE_CONTENT, 48);
  lv_obj_align(pbox, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_flex_flow(pbox, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(pbox, 8, 0);
  lv_obj_clear_flag(pbox, LV_OBJ_FLAG_SCROLLABLE);
  static const struct { const char *sym; const char *kind; uint32_t col; } PV[] = {
      {LV_SYMBOL_BELL, "waiting", 0xF2C14E}, {LV_SYMBOL_OK, "stop", 0x7FB069},
      {LV_SYMBOL_WARNING, "error", 0xE5534B}, {LV_SYMBOL_SHUFFLE, "subagent", 0xB392F0}};
  for (auto &v : PV) {
    lv_obj_t *b = round_btn(pbox, v.sym, preview_cb, (void *)v.kind);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_hex(v.col), 0);
  }

  // NIGHT
  g = section("NIGHT MODE");
  sw_night = switch_row(g, "Night mode", "Extra dim + clock overnight", &s.night_mode);
  divider(g);
  night_from_val = stepper_row(g, "Starts", s.night_start_h, 0);
  divider(g);
  night_to_val = stepper_row(g, "Ends", s.night_end_h, 2);

  // ABOUT
  g = section("ABOUT");
  lv_obj_t *v = about_row(g, "Firmware");
  lv_label_set_text(v, "Claudius Maximus " FW_VERSION);
  ab_host = about_row(g, "Host");
  ab_sessions = about_row(g, "Sessions");
  ab_heap = about_row(g, "Free RAM");
  ab_psram = about_row(g, "PSRAM");
  ab_uptime = about_row(g, "Uptime");
  ab_bl = about_row(g, "Backlight");

  about_timer = lv_timer_create(about_refresh, 1000, NULL);
  lv_timer_pause(about_timer);
  about_refresh(NULL);
}

static void update(JsonObjectConst) {}

// dev hook (RAM only): {"t":"event","kind":"dev_settings","scroll":600}  scrolls the tile for screenshots;
// "log":1 prints the current settings to the host as a log line.
static void on_event(JsonObjectConst ev) {
  if (strcmp(ev["kind"] | "", "dev_settings")) return;
  if (ev["scroll"].is<int>()) lv_obj_scroll_to_y(cont, ev["scroll"].as<int>(), LV_ANIM_OFF);
  if (ev["log"] | 0) {
    Settings &s = settings();
    uint8_t pwm = 0xEE;
    bool pwm_ok = WS_CH32_IO::readRegister(Wire, WS_CH32_IO::REG_PWM, &pwm);
    Link.printf("{\"t\":\"log\",\"msg\":\"ch32 pwm reg read ok=%d val=%u (expect %u)\"}\n", pwm_ok, pwm,
                  (unsigned)((100 - hal_get_brightness()) * 255 / 100));
    Link.printf("{\"t\":\"log\",\"msg\":\"settings bri=%u dim=%u dim_after=%u saver=%u wait=%d done=%d err=%d sub=%d "
                  "night=%d %u-%u hw_bl=%u heap=%u largest=%u psram=%u\"}\n",
                  s.brightness, s.dim_brightness, s.dim_after_s, s.saver_after_s, s.alert_waiting, s.alert_done,
                  s.alert_errors, s.alert_subagents, s.night_mode, s.night_start_h, s.night_end_h, hal_get_brightness(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }
}

static void on_show(bool visible) {
  if (visible) {
    about_refresh(NULL);
    lv_timer_resume(about_timer);
  } else {
    lv_timer_pause(about_timer);
  }
}

const ScreenDef screen_settings = {"settings", "Settings", create, update, on_event, on_show};
