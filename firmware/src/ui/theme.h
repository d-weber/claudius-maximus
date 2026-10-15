#pragma once
// Shared palette & typography. Warm-dark "Claude" look. Use these instead of hard-coded colors.
#include <lvgl.h>

#define C_BG        lv_color_hex(0x1A1918)   // screen background
#define C_SURFACE   lv_color_hex(0x262523)   // cards
#define C_SURFACE2  lv_color_hex(0x33312E)   // raised / pressed
#define C_BORDER    lv_color_hex(0x3D3A36)
#define C_TEXT      lv_color_hex(0xECE9E4)
#define C_MUTED     lv_color_hex(0x9C978E)
#define C_DIM       lv_color_hex(0x5E5A54)
#define C_ACCENT    lv_color_hex(0xD97757)   // Claude orange
#define C_ACCENT2   lv_color_hex(0xF0A07E)

// Status colors (keep consistent across all screens)
#define C_BUSY      lv_color_hex(0xD97757)   // working — orange
#define C_WAITING   lv_color_hex(0xF2C14E)   // needs the human — amber
#define C_IDLE      lv_color_hex(0x7FB069)   // done / ready — green
#define C_OFFLINE   lv_color_hex(0x5E5A54)   // gone — grey
#define C_ERROR     lv_color_hex(0xE5534B)
#define C_SUBAGENT  lv_color_hex(0xB392F0)   // subagents — violet
#define C_INFO      lv_color_hex(0x6A9BCC)   // blue

#define F_SMALL   (&lv_font_montserrat_14)
#define F_BODY    (&lv_font_montserrat_18)
#define F_TITLE   (&lv_font_montserrat_24)
#define F_LARGE   (&lv_font_montserrat_32)
#define F_HUGE    (&lv_font_montserrat_48)

static inline lv_color_t status_color(const char *s) {
  if (!s) return C_OFFLINE;
  if (!strcmp(s, "busy")) return C_BUSY;
  if (!strcmp(s, "waiting")) return C_WAITING;
  if (!strcmp(s, "idle")) return C_IDLE;
  return C_OFFLINE;
}
