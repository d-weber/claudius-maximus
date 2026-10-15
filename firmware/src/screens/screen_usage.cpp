// STUB — to be replaced by the owning agent.
#include "screens.h"
#include "../ui/theme.h"

static lv_obj_t *info;

static void create(lv_obj_t *p) {
  lv_obj_t *t = lv_label_create(p);
  lv_label_set_text(t, "Usage");
  lv_obj_set_style_text_font(t, F_LARGE, 0);
  lv_obj_set_style_text_color(t, C_ACCENT, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 40);
  info = lv_label_create(p);
  lv_obj_set_style_text_color(info, C_MUTED, 0);
  lv_obj_center(info);
  lv_label_set_text(info, "waiting for host...");
}

static void update(JsonObjectConst s) {
  lv_label_set_text_fmt(info, "%d sessions", (int)s["sessions"].size());
}

const ScreenDef screen_usage = {"usage", "Usage", create, update, nullptr, nullptr};
