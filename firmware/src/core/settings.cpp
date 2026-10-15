#include "settings.h"
#include <Preferences.h>
#include "hal.h"

static Settings s;
static Preferences prefs;

Settings &settings() { return s; }

void settings_load() {
  prefs.begin("companion", true);
  if (prefs.getBytesLength("s") == sizeof(Settings)) prefs.getBytes("s", &s, sizeof(Settings));
  prefs.end();
  hal_set_brightness(s.brightness);
}

void settings_save() {
  prefs.begin("companion", false);
  prefs.putBytes("s", &s, sizeof(Settings));
  prefs.end();
}
