#include "settings.h"
#include <Preferences.h>
#include <lvgl.h>
#include "hal.h"

static Settings s;
static Preferences prefs;
static lv_timer_t *save_timer;
static const uint16_t SETTINGS_MAGIC = 0xC1A2;   // bump when the struct layout changes incompatibly

Settings &settings() { return s; }

static void sanitize() {
  if (s.brightness < 5 || s.brightness > 100) s.brightness = 80;
  if (s.dim_brightness > 100) s.dim_brightness = 12;
  if (s.night_start_h > 23) s.night_start_h = 22;
  if (s.night_end_h > 23) s.night_end_h = 7;
}

void settings_load() {
  prefs.begin("companion", true);
  if (prefs.getUShort("magic", 0) == SETTINGS_MAGIC) {
    size_t n = prefs.getBytesLength("s");
    // Accept older, shorter blobs: fields appended later keep their defaults.
    if (n > 0 && n <= sizeof(Settings)) prefs.getBytes("s", &s, n);
  }
  prefs.end();
  sanitize();
  hal_set_brightness(s.brightness);
}

void settings_save() {
  if (save_timer) lv_timer_pause(save_timer);
  prefs.begin("companion", false);
  prefs.putUShort("magic", SETTINGS_MAGIC);
  prefs.putBytes("s", &s, sizeof(Settings));
  prefs.end();
}

static void save_cb(lv_timer_t *) { settings_save(); }

void settings_save_later() {
  if (!save_timer) {
    save_timer = lv_timer_create(save_cb, 1500, NULL);
  } else {
    lv_timer_reset(save_timer);
    lv_timer_resume(save_timer);
  }
}

bool settings_night_now(uint32_t epoch, int32_t tz) {
  if (!s.night_mode || !epoch) return false;
  int h = (int)(((int64_t)epoch + tz) % 86400 / 3600);
  if (h < 0) h += 24;
  uint8_t a = s.night_start_h, b = s.night_end_h;
  if (a == b) return false;
  return a < b ? (h >= a && h < b) : (h >= a || h < b);
}
