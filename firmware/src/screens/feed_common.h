#pragma once
// Shared helpers for the activity feed + session detail (owner: detail-feed).
// Other screens may use these read-only helpers (e.g. feed_session_color for a stable per-session hue).
#include <lvgl.h>
#include <stdint.h>
#include <stddef.h>

// Stable identity color for a session id (hash → palette that avoids the status colors).
lv_color_t feed_session_color(const char *sid);
uint32_t feed_session_color_hex(const char *sid);

// LVGL symbol (or short ASCII) for a tool name ("Read", "Bash", ...).
const char *feed_tool_glyph(const char *tool);

// Visual style for an event kind (+ tool for kind=="tool"). is_sub: event came from a subagent.
struct FeedKindStyle {
  const char *glyph;
  lv_color_t color;
  const char *verb;   // "Finished", "Needs you", ... or nullptr → use tool name
};
FeedKindStyle feed_kind_style(const char *kind, const char *tool, bool is_sub);

// Formatting helpers (write into buf, return buf).
char *feed_fmt_ago(char *buf, size_t n, uint32_t ts, uint32_t now);    // "now", "12s", "4m", "2h"
char *feed_fmt_dur(char *buf, size_t n, uint32_t secs);                // "45s", "2m 13s", "1h 04m"
char *feed_fmt_num(char *buf, size_t n, double v);                     // "980", "45.2k", "1.2M"
void feed_sanitize(char *dst, size_t n, const char *src);             // copy, drop '#' (recolor) + control chars
