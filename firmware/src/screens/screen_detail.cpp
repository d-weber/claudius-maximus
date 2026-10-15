// STUB — to be replaced by the owning agent.
#include "screens.h"
#include "../ui/theme.h"
#include "../core/model.h"

static lv_obj_t *panel, *lbl;
static char cur_sid[16];

static void close_cb(lv_event_t *) { detail_close(); }

void detail_create(lv_obj_t *scr) {
  panel = lv_obj_create(scr);
  lv_obj_set_size(panel, 480, 480);
  lv_obj_set_style_bg_color(panel, C_BG, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_radius(panel, 0, 0);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(panel, close_cb, LV_EVENT_CLICKED, NULL);
  lbl = lv_label_create(panel);
  lv_obj_center(lbl);
}
void detail_open(const char *sid) {
  strlcpy(cur_sid, sid, sizeof(cur_sid));
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(panel);
  detail_update(model_state());
}
void detail_close() { lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN); }
bool detail_is_open() { return !lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN); }
void detail_update(JsonObjectConst) {
  if (!detail_is_open()) return;
  JsonObjectConst s = model_find_session(cur_sid);
  lv_label_set_text_fmt(lbl, "%s\n%s", s["name"] | "?", s["status"] | "?");
}
void detail_on_event(JsonObjectConst) {}
