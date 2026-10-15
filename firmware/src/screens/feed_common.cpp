#include "feed_common.h"
#include <string.h>
#include <stdio.h>
#include "../ui/theme.h"

// Distinct hues that stay clear of status colors (orange/amber/green/red/violet/grey).
static const uint32_t SESSION_PALETTE[] = {
    0x4EC9B0,  // teal
    0x6CB6FF,  // sky
    0xE58FB8,  // pink
    0x9FD8E8,  // ice
    0xD7C38E,  // sand
    0x8C9EFF,  // periwinkle
    0x5FD3D3,  // cyan
    0xC9A0DC,  // mauve-pink
};

uint32_t feed_session_color_hex(const char *sid) {
  uint32_t h = 2166136261u;  // FNV-1a
  for (const char *p = sid ? sid : ""; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
  return SESSION_PALETTE[h % (sizeof(SESSION_PALETTE) / sizeof(SESSION_PALETTE[0]))];
}
lv_color_t feed_session_color(const char *sid) { return lv_color_hex(feed_session_color_hex(sid)); }

const char *feed_tool_glyph(const char *t) {
  if (!t || !*t) return LV_SYMBOL_BULLET;
  if (!strcmp(t, "Read") || !strcmp(t, "NotebookRead")) return LV_SYMBOL_FILE;
  if (!strcmp(t, "Edit") || !strcmp(t, "MultiEdit") || !strcmp(t, "NotebookEdit")) return LV_SYMBOL_EDIT;
  if (!strcmp(t, "Write")) return LV_SYMBOL_SAVE;
  if (!strcmp(t, "Bash") || !strcmp(t, "BashOutput") || !strcmp(t, "KillShell")) return ">_";
  if (!strcmp(t, "Grep")) return LV_SYMBOL_EYE_OPEN;
  if (!strcmp(t, "Glob") || !strcmp(t, "LS")) return LV_SYMBOL_DIRECTORY;
  if (!strncmp(t, "Web", 3)) return LV_SYMBOL_GPS;
  if (!strcmp(t, "Agent") || !strcmp(t, "Task")) return LV_SYMBOL_SHUFFLE;
  if (!strcmp(t, "TodoWrite")) return LV_SYMBOL_LIST;
  if (!strcmp(t, "thinking")) return LV_SYMBOL_REFRESH;
  if (!strcmp(t, "writing")) return LV_SYMBOL_KEYBOARD;
  if (!strncmp(t, "mcp", 3)) return LV_SYMBOL_USB;
  return LV_SYMBOL_SETTINGS;
}

FeedKindStyle feed_kind_style(const char *k, const char *tool, bool sub) {
  if (!k) k = "";
  if (!strcmp(k, "tool")) return {feed_tool_glyph(tool), sub ? C_SUBAGENT : C_BUSY, nullptr};
  if (!strcmp(k, "tool_error")) return {LV_SYMBOL_WARNING, C_ERROR, "failed"};
  if (!strcmp(k, "waiting")) return {LV_SYMBOL_BELL, C_WAITING, "Needs you"};
  if (!strcmp(k, "prompt")) return {LV_SYMBOL_KEYBOARD, C_INFO, "You"};
  if (!strcmp(k, "stop")) return {LV_SYMBOL_OK, C_IDLE, "Finished"};
  if (!strcmp(k, "subagent_start")) return {LV_SYMBOL_SHUFFLE, C_SUBAGENT, "Subagent started"};
  if (!strcmp(k, "subagent_stop")) return {LV_SYMBOL_OK, C_SUBAGENT, "Subagent done"};
  if (!strcmp(k, "todo_done")) return {LV_SYMBOL_OK, C_IDLE, "Todo done"};
  if (!strcmp(k, "session_start")) return {LV_SYMBOL_POWER, C_INFO, "Session started"};
  if (!strcmp(k, "session_end")) return {LV_SYMBOL_POWER, C_OFFLINE, "Session ended"};
  if (!strcmp(k, "compact")) return {LV_SYMBOL_LOOP, C_MUTED, "Context compacted"};
  return {LV_SYMBOL_BULLET, C_MUTED, k};
}

char *feed_fmt_ago(char *b, size_t n, uint32_t ts, uint32_t now) {
  if (!ts || !now || ts >= now) { snprintf(b, n, "now"); return b; }
  uint32_t d = now - ts;
  if (d < 5) snprintf(b, n, "now");
  else if (d < 60) snprintf(b, n, "%us", (unsigned)d);
  else if (d < 3600) snprintf(b, n, "%um", (unsigned)(d / 60));
  else if (d < 86400) snprintf(b, n, "%uh", (unsigned)(d / 3600));
  else snprintf(b, n, "%ud", (unsigned)(d / 86400));
  return b;
}

char *feed_fmt_dur(char *b, size_t n, uint32_t s) {
  if (s < 60) snprintf(b, n, "%us", (unsigned)s);
  else if (s < 3600) snprintf(b, n, "%um %02us", (unsigned)(s / 60), (unsigned)(s % 60));
  else if (s < 86400) snprintf(b, n, "%uh %02um", (unsigned)(s / 3600), (unsigned)((s / 60) % 60));
  else snprintf(b, n, "%ud %uh", (unsigned)(s / 86400), (unsigned)((s / 3600) % 24));
  return b;
}

char *feed_fmt_num(char *b, size_t n, double v) {
  if (v < 1000) snprintf(b, n, "%d", (int)v);
  else if (v < 100000) snprintf(b, n, "%.1fk", v / 1000.0);
  else if (v < 1000000) snprintf(b, n, "%dk", (int)(v / 1000.0));
  else if (v < 100000000) snprintf(b, n, "%.1fM", v / 1e6);
  else snprintf(b, n, "%dM", (int)(v / 1e6));
  return b;
}

void feed_sanitize(char *dst, size_t n, const char *src) {
  if (!n) return;
  size_t j = 0;
  for (const char *p = src ? src : ""; *p && j + 1 < n; p++) {
    char c = *p;
    if (c == '#') c = ' ';
    else if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    else if ((uint8_t)c < 0x20) continue;
    dst[j++] = c;
  }
  dst[j] = 0;
}
