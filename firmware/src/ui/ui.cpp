#include "ui.h"
#include "theme.h"
#include "../core/model.h"
#include "../screens/screens.h"
#include "../fx/alerts.h"
#include "../fx/ambient.h"
#include "../core/hal.h"
#include "../screens/home_face.h"

static const ScreenDef *SCREENS[] = {
    &screen_home, &screen_agents, &screen_feed, &screen_usage, &screen_system, &screen_orbit, &screen_settings,
};
static const int N = sizeof(SCREENS) / sizeof(SCREENS[0]);
static lv_obj_t *tv;
static lv_obj_t *tiles[N];
static lv_obj_t *dots[N];
static int cur = 0;
static uint32_t seen_version = 0;

static void update_dots() {
  for (int i = 0; i < N; i++) {
    lv_obj_set_style_bg_color(dots[i], i == cur ? C_ACCENT : C_DIM, 0);
    lv_obj_set_width(dots[i], i == cur ? 18 : 6);
  }
}

static void on_tile_changed(lv_event_t *) {
  lv_obj_t *t = lv_tileview_get_tile_act(tv);
  int prev = cur;
  for (int i = 0; i < N; i++) if (tiles[i] == t) cur = i;
  if (prev == cur) return;
  if (SCREENS[prev]->on_show) SCREENS[prev]->on_show(false);
  if (SCREENS[cur]->on_show) SCREENS[cur]->on_show(true);
  if (SCREENS[cur]->update) SCREENS[cur]->update(model_state());
  update_dots();
}

void ui_init() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, C_BG, 0);
  lv_obj_set_style_text_color(scr, C_TEXT, 0);
  lv_obj_set_style_text_font(scr, F_BODY, 0);

  tv = lv_tileview_create(scr);
  lv_obj_set_size(tv, 480, 480);
  lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
  lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
  for (int i = 0; i < N; i++) {
    uint8_t dir = (i > 0 ? LV_DIR_LEFT : 0) | (i < N - 1 ? LV_DIR_RIGHT : 0);
    tiles[i] = lv_tileview_add_tile(tv, i, 0, (lv_dir_t)dir);
    lv_obj_set_style_pad_all(tiles[i], 0, 0);
    SCREENS[i]->create(tiles[i]);
  }
  lv_obj_add_event_cb(tv, on_tile_changed, LV_EVENT_VALUE_CHANGED, NULL);

  // page dots
  lv_obj_t *bar = lv_obj_create(scr);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, 200, 12);
  lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(bar, 6, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
  for (int i = 0; i < N; i++) {
    dots[i] = lv_obj_create(bar);
    lv_obj_remove_style_all(dots[i]);
    lv_obj_set_size(dots[i], 6, 6);
    lv_obj_set_style_radius(dots[i], 3, 0);
    lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
  }
  update_dots();

  detail_create(scr);
  alerts_init();
  ambient_init();
  if (SCREENS[0]->on_show) SCREENS[0]->on_show(true);
}

void ui_tick() {
  uint32_t v = model_state_version();
  if (v != seen_version) {
    seen_version = v;
    JsonObjectConst s = model_state();
    if (SCREENS[cur]->update) SCREENS[cur]->update(s);
    detail_update(s);
    alerts_update(s);
    ambient_update(s);
  }
  ambient_tick();
}

void ui_nav(const char *id) {
  for (int i = 0; i < N; i++)
    if (!strcmp(SCREENS[i]->id, id)) {
      detail_close();
      lv_obj_set_tile(tv, tiles[i], LV_ANIM_OFF);
      lv_event_send(tv, LV_EVENT_VALUE_CHANGED, NULL);
      return;
    }
}

const char *ui_current() { return detail_is_open() ? "detail" : SCREENS[cur]->id; }

void ui_open(const char *what, const char *sid) {
  if (!strcmp(what, "detail")) detail_open(sid);
}

void ui_dispatch_event(JsonObjectConst ev) {
  for (int i = 0; i < N; i++) if (SCREENS[i]->on_event) SCREENS[i]->on_event(ev);
  detail_on_event(ev);
  alerts_on_event(ev);
  ambient_on_event(ev);
  if (!strcmp(ev["kind"] | "", "dev_touch")) hal_touch_set_poll((ev["poll"] | 1) != 0);   // PROBE
  if (!strcmp(ev["kind"] | "", "dev_probe")) {                                              // PROBE
    hal_probe_set(ev["mode"] | 0);
    if (!ev["dirloc"].isNull()) home_face_probe_dirloc((ev["dirloc"] | 0) != 0);
    if (!ev["poll_ms"].isNull()) hal_probe_poll_ms(ev["poll_ms"] | 10);
    if (!ev["i2c_hz"].isNull()) hal_probe_i2c_hz(ev["i2c_hz"] | 400000);
    if (!ev["pclk"].isNull()) hal_probe_pclk(ev["pclk"] | 0);
  }
}

void ui_demo_alert(const char *kind) { alerts_demo(kind); }
