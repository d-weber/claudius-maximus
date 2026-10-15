// Attention system on lv_layer_top() — sits above every screen and the detail overlay.
//
//  waiting  : amber edge glow (4 thin layered bars, opacity pulse) for as long as any session waits,
//             + a "<name> needs you" card (queue, newest first, tap -> detail, X -> dismiss).
//             The card collapses into a small top pill after a while; the pill stays while anyone waits.
//  stop     : green toast "<name> finished" + a small confetti burst (rate-limited, merges bursts).
//  error    : short red edge flash + red toast (rate-limited).
//  subagent : tiny violet chip.
// CPU: glow is driven by a 40 ms lv_timer that only runs while needed; confetti is 16 tiny objects.
#include "alerts.h"
#include <lvgl.h>
#include "../ui/theme.h"
#include "../screens/screens.h"
#include "../screens/detail_api.h"
#include "../core/model.h"
#include "../core/settings.h"

// ---------------------------------------------------------------------------------------------
// tunables
static const uint32_t CARD_COLLAPSE_MS = 10000;   // card -> pill after this long without news
static const uint32_t TOAST_MS = 3200;
static const uint32_t ERR_TOAST_MS = 2600;
static const uint32_t CONFETTI_COOLDOWN_MS = 20000;
static const uint32_t ERR_COOLDOWN_MS = 8000;
static const uint32_t CHIP_MS = 2200;
static const uint32_t FLASH_MS = 900;
static const int MAXQ = 6;
static const int MAXW = 12;

struct WaitEntry {
  char sid[16];
  char name[40];
  char text[128];
  uint32_t since_ms;   // when it started waiting (device ms)
  uint32_t added_ms;   // when the entry was (re)queued
  bool demo;
};

static WaitEntry q[MAXQ];
static int nq = 0;

// waiting sessions according to the latest state
static char st_wait[MAXW][16];
static int n_st_wait = 0;
static char st_first_sid[16];
static char dismissed[MAXW][16];
static int n_dismissed = 0;
static bool seen_state = false;
static uint32_t wait_hint_until = 0;   // glow before the state confirms (event-first)
static uint32_t demo_until = 0;        // demo forces visuals until then
static uint32_t card_news_ms = 0;
static bool card_collapsed = false;

// ---------------------------------------------------------------------------------------------
// objects
static lv_obj_t *layer;
static lv_obj_t *bars[4][3];               // side x layer
static const uint8_t BAR_T[3] = {3, 8, 16};
static const uint8_t BAR_OPA[3] = {255, 85, 30};
static lv_timer_t *glow_timer;
static uint32_t flash_until = 0;
static lv_color_t flash_color;

static lv_obj_t *card, *card_icon, *card_kicker, *card_title, *card_body, *card_more;
static lv_obj_t *pill, *pill_lbl;
static char pill_sid[16];
static lv_obj_t *toast, *toast_icon, *toast_icon_lbl, *toast_title, *toast_sub;
static char toast_sid[16];
static int toast_merge = 0;
static uint32_t toast_until = 0;
static bool toast_is_error = false;
static uint32_t last_confetti = 0, last_err = 0;
static lv_obj_t *chip, *chip_lbl;
static uint32_t chip_until = 0;

#define NCONF 22
static lv_obj_t *conf[NCONF];
static float cx[NCONF], cy[NCONF], cvx[NCONF], cvy[NCONF];
static int conf_ticks = 0;
static lv_timer_t *conf_timer;

static lv_timer_t *tick_timer;

// ---------------------------------------------------------------------------------------------
// helpers
static bool in_list(char (*l)[16], int n, const char *sid) {
  for (int i = 0; i < n; i++) if (!strcmp(l[i], sid)) return true;
  return false;
}
static void list_add(char (*l)[16], int &n, const char *sid) {
  if (in_list(l, n, sid) || n >= MAXW) return;
  strlcpy(l[n++], sid, 16);
}
static int q_find(const char *sid) {
  for (int i = 0; i < nq; i++) if (!strcmp(q[i].sid, sid)) return i;
  return -1;
}
static void q_remove(int i) {
  if (i < 0 || i >= nq) return;
  for (int j = i; j < nq - 1; j++) q[j] = q[j + 1];
  nq--;
}
static bool now_before(uint32_t t) { return (int32_t)(t - millis()) > 0; }
// the user is already looking at this session's detail overlay -> no card / pill for it
static bool looking_at(const char *sid) {
  const char *d = detail_sid();
  return d && *d && sid && !strcmp(d, sid);
}

static lv_obj_t *plain(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}
static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, lv_color_t c) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  return l;
}

static void anim_y_cb(void *o, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t *)o, v, 0); }
static void anim_opa_cb(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, v, 0); }
static void anim_hide_ready(lv_anim_t *a) { lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN); }

static void slide_in(lv_obj_t *o, int32_t from_y, uint32_t ms) {
  lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(o);
  lv_anim_del(o, NULL);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_exec_cb(&a, anim_y_cb);
  lv_anim_set_values(&a, from_y, 0);
  lv_anim_set_time(&a, ms);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
  lv_anim_set_exec_cb(&a, anim_opa_cb);
  lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_start(&a);
}
static void fade_out(lv_obj_t *o, uint32_t ms) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
  lv_anim_del(o, NULL);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_exec_cb(&a, anim_opa_cb);
  lv_anim_set_values(&a, lv_obj_get_style_opa(o, 0), LV_OPA_TRANSP);
  lv_anim_set_time(&a, ms);
  lv_anim_set_ready_cb(&a, anim_hide_ready);
  lv_anim_start(&a);
}

// ---------------------------------------------------------------------------------------------
// edge glow
static bool waiting_active() {
  if (now_before(demo_until)) return true;
  if (!settings().alert_waiting) return false;
  return n_st_wait > 0 || nq > 0 || now_before(wait_hint_until);
}

static void bars_set(lv_color_t c, int32_t factor /*0..256*/) {
  for (int s = 0; s < 4; s++)
    for (int l = 0; l < 3; l++) {
      lv_obj_set_style_bg_color(bars[s][l], c, 0);
      lv_obj_set_style_bg_opa(bars[s][l], (lv_opa_t)(BAR_OPA[l] * factor / 256), 0);
    }
}
static void bars_show(bool on) {
  for (int s = 0; s < 4; s++)
    for (int l = 0; l < 3; l++) {
      if (on) {
        if (lv_obj_has_flag(bars[s][l], LV_OBJ_FLAG_HIDDEN)) {
          lv_obj_clear_flag(bars[s][l], LV_OBJ_FLAG_HIDDEN);
          lv_obj_move_foreground(bars[s][l]);
        }
      } else {
        lv_obj_add_flag(bars[s][l], LV_OBJ_FLAG_HIDDEN);
      }
    }
}

static void glow_cb(lv_timer_t *) {
  uint32_t now = millis();
  if (now_before(flash_until)) {
    // quick double blink then fade
    uint32_t left = flash_until - now;
    uint32_t el = FLASH_MS - left;
    int32_t f = el < 150 ? 256 : el < 260 ? 90 : el < 420 ? 256 : 256 * left / (FLASH_MS - 420);
    bars_show(true);
    bars_set(flash_color, f);
    return;
  }
  if (waiting_active()) {
    // slow breathing: 1.8 s period, 35%..100%
    int16_t ang = (int16_t)((now % 1800) * 360 / 1800);
    int32_t sn = lv_trigo_sin(ang);                 // -32767..32767
    int32_t f = 90 + (166 * (sn + 32767) / 65534);  // 90..256
    bars_show(true);
    bars_set(C_WAITING, f);
    return;
  }
  bars_show(false);
  lv_timer_pause(glow_timer);
}
static void glow_kick() { lv_timer_resume(glow_timer); }

// ---------------------------------------------------------------------------------------------
// waiting card + pill
static void pill_refresh();

static void card_render() {
  if (nq == 0) return;
  WaitEntry &e = q[0];
  uint32_t secs = (millis() - e.since_ms) / 1000;
  if (secs < 3600)
    lv_label_set_text_fmt(card_kicker, "NEEDS YOU  #9C978E " LV_SYMBOL_BULLET " %u:%02u#", (unsigned)(secs / 60),
                          (unsigned)(secs % 60));
  else
    lv_label_set_text_fmt(card_kicker, "NEEDS YOU  #9C978E " LV_SYMBOL_BULLET " %uh%02u#", (unsigned)(secs / 3600),
                          (unsigned)(secs / 60 % 60));
  lv_label_set_text(card_title, e.name);
  lv_label_set_text(card_body, e.text[0] ? e.text : "Waiting for your input");
  if (nq > 1) {
    lv_label_set_text_fmt(card_more, "+%d more", nq - 1);
    lv_obj_clear_flag(card_more, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(card_more, LV_OBJ_FLAG_HIDDEN);
  }
}

static bool card_visible() { return !lv_obj_has_flag(card, LV_OBJ_FLAG_HIDDEN); }

static void card_show(bool bump) {
  if (nq == 0) return;
  if (looking_at(q[0].sid)) {  // already on screen in the detail overlay: glow only
    if (card_visible()) card_hide();
    glow_kick();
    pill_refresh();
    return;
  }
  card_render();
  card_collapsed = false;
  card_news_ms = millis();
  if (!card_visible()) slide_in(card, 150, 380);
  else if (bump) slide_in(card, 18, 220);
  else lv_obj_move_foreground(card);
  pill_refresh();
  glow_kick();
}

static void card_hide() {
  if (card_visible()) fade_out(card, 200);
  pill_refresh();
}

static void card_next() {
  if (nq == 0) card_hide();
  else card_render();
  pill_refresh();
}

// pill: compact persistent reminder when the card is not shown
static void pill_refresh() {
  bool card_on = card_visible() && nq > 0;
  // count waiting sessions (state + queue), minus the one whose detail is open
  int n = 0;
  const char *first = "";
  const char *first_name = "";
  for (int i = 0; i < n_st_wait; i++)
    if (!looking_at(st_wait[i])) { if (!n) first = st_wait[i]; n++; }
  for (int i = 0; i < nq; i++)
    if (!in_list(st_wait, n_st_wait, q[i].sid) && !looking_at(q[i].sid)) {
      if (!n) { first = q[i].sid; first_name = q[i].name; }
      n++;
    }
  bool enabled = settings().alert_waiting || now_before(demo_until);
  bool show = !card_on && enabled && n > 0;
  if (!show) {
    if (!lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN)) fade_out(pill, 150);
    return;
  }
  strlcpy(pill_sid, first, sizeof(pill_sid));
  if (n == 1) {
    int qi = q_find(first);
    const char *nm = qi >= 0 ? q[qi].name : first_name;
    if (!*nm) nm = model_find_session(first)["name"] | "agent";
    lv_label_set_text_fmt(pill_lbl, LV_SYMBOL_BELL "  %s needs you", nm);
  } else {
    lv_label_set_text_fmt(pill_lbl, LV_SYMBOL_BELL "  %d agents need you", n);
  }
  if (lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN) || lv_anim_get(pill, NULL)) slide_in(pill, 40, 260);
  else lv_obj_move_foreground(pill);
}

static void enqueue(const char *sid, const char *name, const char *text, bool demo) {
  int i = q_find(sid);
  WaitEntry e;
  if (i >= 0) {
    e = q[i];
    q_remove(i);
  } else {
    memset(&e, 0, sizeof(e));
    strlcpy(e.sid, sid, sizeof(e.sid));
    e.since_ms = millis();
  }
  if (name && *name) strlcpy(e.name, name, sizeof(e.name));
  if (!e.name[0]) strlcpy(e.name, "An agent", sizeof(e.name));
  if (text && *text) strlcpy(e.text, text, sizeof(e.text));
  e.added_ms = millis();
  e.demo = demo;
  if (nq == MAXQ) nq--;  // drop the oldest
  for (int j = nq; j > 0; j--) q[j] = q[j - 1];
  q[0] = e;
  nq++;
}

static void dismiss_sid(const char *sid) {
  list_add(dismissed, n_dismissed, sid);
  q_remove(q_find(sid));
}

static void card_click_cb(lv_event_t *) {
  if (nq == 0) { card_hide(); return; }
  char sid[16];
  strlcpy(sid, q[0].sid, sizeof(sid));
  bool demo = q[0].demo;
  dismiss_sid(sid);
  card_next();
  if (!demo || model_find_session(sid)["id"].is<const char *>()) detail_open(sid);
}
static void card_close_cb(lv_event_t *e) {
  lv_event_stop_bubbling(e);
  if (nq) dismiss_sid(q[0].sid);
  card_next();
}
static void pill_click_cb(lv_event_t *) {
  // one tap = act: open the waiting session (the card's job is the heads-up, the pill's is the shortcut)
  if (!pill_sid[0]) return;
  int i = q_find(pill_sid);
  bool demo = i >= 0 && q[i].demo;
  if (i >= 0) dismiss_sid(pill_sid);
  if (!demo || model_find_session(pill_sid)["id"].is<const char *>()) detail_open(pill_sid);
  pill_refresh();
}

// ---------------------------------------------------------------------------------------------
// toast (stop / error) + confetti
static void conf_cb(lv_timer_t *) {
  conf_ticks++;
  bool fade = conf_ticks > 26;
  for (int i = 0; i < NCONF; i++) {
    cvy[i] += 0.55f;
    cvx[i] *= 0.985f;
    cx[i] += cvx[i];
    cy[i] += cvy[i];
    lv_obj_set_pos(conf[i], (lv_coord_t)cx[i], (lv_coord_t)cy[i]);
    if (fade) lv_obj_set_style_bg_opa(conf[i], (lv_opa_t)LV_MAX(0, 255 - (conf_ticks - 26) * 26), 0);
  }
  if (conf_ticks > 36) {
    for (int i = 0; i < NCONF; i++) lv_obj_add_flag(conf[i], LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(conf_timer);
  }
}

static void confetti_burst(int x0, int y0) {
  static const uint32_t cols[] = {0x7FB069, 0xD97757, 0xF2C14E, 0xB392F0, 0x6A9BCC, 0xF0A07E};
  for (int i = 0; i < NCONF; i++) {
    cx[i] = x0 + (int)lv_rand(0, 160) - 80;
    cy[i] = y0;
    cvx[i] = ((int)lv_rand(0, 200) - 100) / 10.0f;
    cvy[i] = -6.5f - lv_rand(0, 70) / 10.0f;
    bool round = i & 1;
    lv_obj_set_size(conf[i], round ? 8 : 6, round ? 8 : 12);
    lv_obj_set_style_radius(conf[i], round ? 4 : 1, 0);
    lv_obj_set_style_bg_color(conf[i], lv_color_hex(cols[i % 6]), 0);
    lv_obj_set_style_bg_opa(conf[i], LV_OPA_COVER, 0);
    lv_obj_set_pos(conf[i], (lv_coord_t)cx[i], (lv_coord_t)cy[i]);
    lv_obj_clear_flag(conf[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(conf[i]);
  }
  conf_ticks = 0;
  lv_timer_resume(conf_timer);
}

// bottom stack: [chip] [toast] [card | pill] [page dots]
static lv_coord_t stack_y() {
  if (card_visible() && nq > 0) return -(26 + 128 + 10);
  if (!lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN)) return -(26 + 40 + 10);
  return -30;
}

static void toast_show(bool error, const char *sid, const char *title, const char *sub) {
  lv_color_t c = error ? C_ERROR : C_IDLE;
  toast_is_error = error;
  strlcpy(toast_sid, sid ? sid : "", sizeof(toast_sid));
  lv_obj_set_style_border_color(toast, c, 0);
  lv_obj_set_style_bg_color(toast_icon, c, 0);
  lv_label_set_text(toast_icon_lbl, error ? LV_SYMBOL_WARNING : LV_SYMBOL_OK);
  lv_label_set_text(toast_title, title);
  lv_label_set_text(toast_sub, sub && *sub ? sub : "");
  if (sub && *sub) lv_obj_clear_flag(toast_sub, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(toast_sub, LV_OBJ_FLAG_HIDDEN);
  bool was = !lv_obj_has_flag(toast, LV_OBJ_FLAG_HIDDEN);
  if (!was) lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, stack_y());
  slide_in(toast, was ? 8 : 40, was ? 160 : 280);
  toast_until = millis() + (error ? ERR_TOAST_MS : TOAST_MS);
}

static void toast_click_cb(lv_event_t *) {
  if (toast_sid[0] && model_find_session(toast_sid)["id"].is<const char *>()) detail_open(toast_sid);
  toast_until = millis();
  fade_out(toast, 150);
}

static void celebrate(const char *sid, const char *name, const char *text, bool force_confetti) {
  char title[64];
  bool merging = !lv_obj_has_flag(toast, LV_OBJ_FLAG_HIDDEN) && !toast_is_error && now_before(toast_until);
  if (merging) {
    toast_merge++;
    snprintf(title, sizeof(title), "%d agents finished", toast_merge + 1);
    toast_show(false, sid, title, text);
  } else {
    toast_merge = 0;
    snprintf(title, sizeof(title), "%s finished", name && *name ? name : "Agent");
    toast_show(false, sid, title, text);
  }
  uint32_t now = millis();
  if (force_confetti || !last_confetti || now - last_confetti > CONFETTI_COOLDOWN_MS) {
    last_confetti = now;
    confetti_burst(240 - 4, 480 + stack_y() - 64);
  }
}

static void error_flash(const char *sid, const char *name, const char *tool, const char *text, bool force) {
  uint32_t now = millis();
  if (!force && last_err && now - last_err < ERR_COOLDOWN_MS) return;
  last_err = now;
  flash_color = C_ERROR;
  flash_until = now + FLASH_MS;
  glow_kick();
  char title[72];
  snprintf(title, sizeof(title), "%s failed in %s", tool && *tool ? tool : "Tool", name && *name ? name : "agent");
  // don't stomp a fresh celebration
  if (!lv_obj_has_flag(toast, LV_OBJ_FLAG_HIDDEN) && !toast_is_error && now_before(toast_until) && !force) return;
  toast_show(true, sid, title, text);
}

// ---------------------------------------------------------------------------------------------
// chip (subagents)
static void chip_show(const char *name, const char *text, bool start) {
  lv_label_set_text_fmt(chip_lbl, "%s  %s", start ? LV_SYMBOL_PLUS : LV_SYMBOL_OK, text && *text ? text : "subagent");
  (void)name;
  bool toast_on = !lv_obj_has_flag(toast, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(chip, LV_ALIGN_BOTTOM_MID, 0, toast_on ? lv_obj_get_y_aligned(toast) - 76 : stack_y());
  bool was = !lv_obj_has_flag(chip, LV_OBJ_FLAG_HIDDEN);
  slide_in(chip, was ? 4 : 16, 200);
  chip_until = millis() + CHIP_MS;
}

// ---------------------------------------------------------------------------------------------
// 1 Hz housekeeping
static void tick_cb(lv_timer_t *) {
  uint32_t now = millis();
  // drop demo entries when the demo expires
  if (!now_before(demo_until))
    for (int i = nq - 1; i >= 0; i--)
      if (q[i].demo) q_remove(i);
  if (card_visible()) {
    if (nq == 0 || looking_at(q[0].sid)) card_hide();
    else {
      card_render();
      if (now - card_news_ms > CARD_COLLAPSE_MS && !now_before(demo_until)) {
        card_collapsed = true;
        card_hide();
      }
    }
  }
  pill_refresh();
  if (!lv_obj_has_flag(toast, LV_OBJ_FLAG_HIDDEN) && !now_before(toast_until)) fade_out(toast, 300);
  if (!lv_obj_has_flag(chip, LV_OBJ_FLAG_HIDDEN) && !now_before(chip_until)) fade_out(chip, 250);
  if (waiting_active()) glow_kick();
}

// ---------------------------------------------------------------------------------------------
// public API
void alerts_init() {
  layer = lv_layer_top();

  // edge glow bars: 0=top 1=bottom 2=left 3=right, 3 layers each (thin & bright -> wide & faint)
  for (int s = 0; s < 4; s++)
    for (int l = 0; l < 3; l++) {
      lv_obj_t *b = plain(layer);
      lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(b, C_WAITING, 0);
      int t = BAR_T[l];
      switch (s) {
        case 0: lv_obj_set_size(b, 480, t); lv_obj_set_pos(b, 0, 0); break;
        case 1: lv_obj_set_size(b, 480, t); lv_obj_set_pos(b, 0, 480 - t); break;
        case 2: lv_obj_set_size(b, t, 480 - 2 * BAR_T[2]); lv_obj_set_pos(b, 0, BAR_T[2]); break;
        case 3: lv_obj_set_size(b, t, 480 - 2 * BAR_T[2]); lv_obj_set_pos(b, 480 - t, BAR_T[2]); break;
      }
      lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
      bars[s][l] = b;
    }
  glow_timer = lv_timer_create(glow_cb, 40, NULL);
  lv_timer_pause(glow_timer);

  // --- waiting card
  card = lv_obj_create(layer);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, 444, 128);
  lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -26);  // bottom, above the page dots: never hides headers
  lv_obj_set_style_bg_color(card, C_SURFACE2, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(card, 22, 0);
  lv_obj_set_style_border_width(card, 2, 0);
  lv_obj_set_style_border_color(card, C_WAITING, 0);
  lv_obj_set_style_shadow_width(card, 24, 0);
  lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
  lv_obj_set_style_shadow_opa(card, LV_OPA_60, 0);
  lv_obj_set_style_shadow_ofs_y(card, 6, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x3D3A36), LV_STATE_PRESSED);
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_CLICKED, NULL);

  card_icon = plain(card);
  lv_obj_set_size(card_icon, 56, 56);
  lv_obj_set_pos(card_icon, 14, 16);
  lv_obj_set_style_radius(card_icon, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(card_icon, C_WAITING, 0);
  lv_obj_set_style_bg_opa(card_icon, LV_OPA_COVER, 0);
  lv_obj_t *il = label(card_icon, F_TITLE, C_BG);
  lv_label_set_text(il, LV_SYMBOL_BELL);
  lv_obj_center(il);

  card_kicker = label(card, F_SMALL, C_WAITING);
  lv_label_set_recolor(card_kicker, true);
  lv_obj_set_style_text_letter_space(card_kicker, 1, 0);
  lv_obj_set_pos(card_kicker, 84, 12);

  card_title = label(card, F_TITLE, C_TEXT);
  lv_label_set_long_mode(card_title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(card_title, 300);
  lv_obj_set_pos(card_title, 84, 32);

  card_body = label(card, F_BODY, C_MUTED);
  lv_label_set_long_mode(card_body, LV_LABEL_LONG_DOT);
  lv_obj_set_size(card_body, 344, 46);
  lv_obj_set_pos(card_body, 84, 66);

  card_more = label(card, F_SMALL, C_BG);
  lv_obj_set_style_bg_color(card_more, C_WAITING, 0);
  lv_obj_set_style_bg_opa(card_more, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(card_more, 10, 0);
  lv_obj_set_style_pad_hor(card_more, 9, 0);
  lv_obj_set_style_pad_ver(card_more, 3, 0);
  lv_obj_align(card_more, LV_ALIGN_TOP_RIGHT, -58, 10);

  lv_obj_t *x = lv_obj_create(card);
  lv_obj_remove_style_all(x);
  lv_obj_set_size(x, 52, 52);
  lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -4, 4);
  lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(x, C_BORDER, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(x, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_add_flag(x, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(x, card_close_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *xl = label(x, F_BODY, C_MUTED);
  lv_label_set_text(xl, LV_SYMBOL_CLOSE);
  lv_obj_center(xl);
  lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);

  // --- pill
  pill = lv_obj_create(layer);
  lv_obj_remove_style_all(pill);
  lv_obj_set_size(pill, LV_SIZE_CONTENT, 40);
  lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -26);
  lv_obj_set_style_bg_color(pill, C_WAITING, 0);
  lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(pill, 20, 0);
  lv_obj_set_style_pad_hor(pill, 18, 0);
  lv_obj_set_style_bg_color(pill, C_ACCENT2, LV_STATE_PRESSED);
  lv_obj_set_ext_click_area(pill, 10);
  lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(pill, pill_click_cb, LV_EVENT_CLICKED, NULL);
  pill_lbl = label(pill, &lv_font_montserrat_16, C_BG);
  lv_label_set_long_mode(pill_lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_style_max_width(pill_lbl, 300, 0);
  lv_obj_center(pill_lbl);
  lv_obj_add_flag(pill, LV_OBJ_FLAG_HIDDEN);

  // --- toast
  toast = lv_obj_create(layer);
  lv_obj_remove_style_all(toast);
  lv_obj_set_size(toast, 400, 68);
  lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -30);
  lv_obj_set_style_bg_color(toast, C_SURFACE2, 0);
  lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(toast, 34, 0);
  lv_obj_set_style_border_width(toast, 2, 0);
  lv_obj_set_style_border_color(toast, C_IDLE, 0);
  lv_obj_add_flag(toast, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(toast, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(toast, toast_click_cb, LV_EVENT_CLICKED, NULL);
  toast_icon = plain(toast);
  lv_obj_set_size(toast_icon, 44, 44);
  lv_obj_align(toast_icon, LV_ALIGN_LEFT_MID, 10, 0);
  lv_obj_set_style_radius(toast_icon, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(toast_icon, LV_OPA_COVER, 0);
  toast_icon_lbl = label(toast_icon, F_BODY, C_BG);
  lv_obj_center(toast_icon_lbl);
  lv_obj_t *col = plain(toast);
  lv_obj_set_size(col, 320, LV_SIZE_CONTENT);
  lv_obj_align(col, LV_ALIGN_LEFT_MID, 66, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 1, 0);
  toast_title = label(col, F_BODY, C_TEXT);
  lv_label_set_long_mode(toast_title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(toast_title, 316);
  toast_sub = label(col, F_SMALL, C_MUTED);
  lv_label_set_long_mode(toast_sub, LV_LABEL_LONG_DOT);
  lv_obj_set_width(toast_sub, 316);
  lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);

  // --- chip
  chip = plain(layer);
  lv_obj_set_size(chip, LV_SIZE_CONTENT, 34);
  lv_obj_set_style_bg_color(chip, C_SURFACE2, 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(chip, 17, 0);
  lv_obj_set_style_border_width(chip, 1, 0);
  lv_obj_set_style_border_color(chip, C_SUBAGENT, 0);
  lv_obj_set_style_pad_hor(chip, 14, 0);
  chip_lbl = label(chip, F_SMALL, C_SUBAGENT);
  lv_label_set_long_mode(chip_lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_style_max_width(chip_lbl, 320, 0);
  lv_obj_center(chip_lbl);
  lv_obj_align(chip, LV_ALIGN_BOTTOM_MID, 0, -34);
  lv_obj_add_flag(chip, LV_OBJ_FLAG_HIDDEN);

  // --- confetti
  for (int i = 0; i < NCONF; i++) {
    conf[i] = plain(layer);
    lv_obj_add_flag(conf[i], LV_OBJ_FLAG_HIDDEN);
  }
  conf_timer = lv_timer_create(conf_cb, 33, NULL);
  lv_timer_pause(conf_timer);

  tick_timer = lv_timer_create(tick_cb, 1000, NULL);
}

void alerts_update(JsonObjectConst state) {
  char cur[MAXW][16];
  int n = 0;
  st_first_sid[0] = 0;
  for (JsonObjectConst s : state["sessions"].as<JsonArrayConst>()) {
    const char *st = s["status"] | "";
    const char *sid = s["id"] | "";
    if (strcmp(st, "waiting") || !*sid || n >= MAXW) continue;
    strlcpy(cur[n++], sid, 16);
    if (!st_first_sid[0]) strlcpy(st_first_sid, sid, sizeof(st_first_sid));
    bool was = in_list(st_wait, n_st_wait, sid);
    // newly waiting according to state (event may have been missed): card it — except on the first
    // snapshot after boot, where the pill + glow are enough.
    if (!was && seen_state && settings().alert_waiting && !in_list(dismissed, n_dismissed, sid) && q_find(sid) < 0) {
      char text[128];
      const char *tool = s["activity"]["tool"] | "";
      const char *det = s["activity"]["detail"] | "";
      if (*tool && *det) snprintf(text, sizeof(text), "%s: %s", tool, det);
      else strlcpy(text, *det ? det : "Waiting for your input", sizeof(text));
      enqueue(sid, s["name"] | "agent", text, false);
      card_show(true);
    }
  }
  // prune queue entries that are no longer waiting (grace period for event-before-state)
  uint32_t now = millis();
  bool changed = false;
  for (int i = nq - 1; i >= 0; i--) {
    if (q[i].demo || now - q[i].added_ms < 4000) continue;
    if (!in_list(cur, n, q[i].sid)) { q_remove(i); changed = true; }
  }
  // forget dismissals for sessions that stopped waiting
  for (int i = n_dismissed - 1; i >= 0; i--)
    if (!in_list(cur, n, dismissed[i])) {
      for (int j = i; j < n_dismissed - 1; j++) strlcpy(dismissed[j], dismissed[j + 1], 16);
      n_dismissed--;
    }
  memcpy(st_wait, cur, sizeof(cur));
  n_st_wait = n;
  seen_state = true;
  if (changed) card_next();
  pill_refresh();
  if (waiting_active()) glow_kick();
}

void alerts_on_event(JsonObjectConst ev) {
  const char *kind = ev["kind"] | "";
  const char *sid = ev["sid"] | "";
  const char *name = ev["name"] | "";
  const char *text = ev["text"] | "";
  Settings &cfg = settings();

  if (!strcmp(kind, "waiting")) {
    if (!cfg.alert_waiting) return;
    // a new prompt for the same session re-alerts even if a previous one was dismissed
    for (int i = n_dismissed - 1; i >= 0; i--)
      if (!strcmp(dismissed[i], sid)) {
        for (int j = i; j < n_dismissed - 1; j++) strlcpy(dismissed[j], dismissed[j + 1], 16);
        n_dismissed--;
      }
    enqueue(sid, name, text, false);
    wait_hint_until = millis() + 5000;
    card_show(true);
  } else if (!strcmp(kind, "stop") || !strcmp(kind, "session_end") || !strcmp(kind, "prompt") ||
             !strcmp(kind, "tool")) {
    // the human answered / the agent moved on
    int i = q_find(sid);
    if (i >= 0 && !q[i].demo && millis() - q[i].added_ms > 800) {
      q_remove(i);
      card_next();
    }
    if (!strcmp(kind, "stop") && cfg.alert_done) celebrate(sid, name, text, false);
  } else if (!strcmp(kind, "tool_error")) {
    if (cfg.alert_errors) error_flash(sid, name, ev["tool"] | "", text, false);
  } else if (!strcmp(kind, "subagent_start") || !strcmp(kind, "subagent_stop")) {
    if (cfg.alert_subagents) chip_show(name, text, !strcmp(kind, "subagent_start"));
  }
}

void alerts_demo(const char *kind) {
  if (!kind) return;
  JsonArrayConst ss = model_state()["sessions"].as<JsonArrayConst>();
  const char *sid0 = ss[0]["id"] | "demo0001";
  const char *name0 = ss[0]["name"] | "api-gateway";
  if (!strcmp(kind, "waiting")) {
    demo_until = millis() + 30000;
    const char *sid1 = ss[1]["id"] | "demo0002";
    const char *name1 = ss[1]["name"] | "web-dashboard";
    enqueue(sid1, name1, "Do you want to proceed with the migration?", true);
    enqueue(sid0, name0, "Allow Bash: npm test -- --coverage --runInBand?", true);
    q[0].since_ms = millis() - 12000;
    card_show(true);
  } else if (!strcmp(kind, "stop") || !strcmp(kind, "done")) {
    celebrate(sid0, name0, "Done - all tests passing.", true);
  } else if (!strcmp(kind, "error")) {
    error_flash(sid0, name0, "Bash", "npm test exited with code 1", true);
  } else if (!strcmp(kind, "subagent")) {
    chip_show(name0, "Explore auth module", true);
  } else if (!strcmp(kind, "pill")) {
    demo_until = millis() + 30000;
    enqueue(sid0, name0, "Allow Bash: npm test?", true);
    card_collapsed = true;
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    pill_refresh();
    glow_kick();
  }
}
