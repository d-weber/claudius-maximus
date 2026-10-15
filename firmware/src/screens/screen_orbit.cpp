// ORBIT — ambient real-time orrery of the agent swarm.
//
//  * Clawd (the Claude Code mascot, pixel art from the CLI banner) sits in the centre. Its body is baked into the
//    background canvas; only the two eyes are objects: they look toward the most recently active planet (a waiting
//    one wins), blink now and then, and close to sleepy slits when no agent is busy.
//  * Each session = a planet on its own ring (oldest session innermost). Colour = status, size = tokens (log),
//    angular speed = Kepler-ish by radius x busyness (busy & recently active = fast, idle = drift, offline = still).
//  * Subagents = violet moons circling their planet (finished ones fade until the host drops them).
//  * Every tool event emits a spark burst tinted by tool family (read/search blue, edit/write mint, bash cream,
//    web pink, agent violet, error red). Prompts fly as a beam from Clawd to the planet.
//  * Waiting planets: amber, sonar ping, amber ring. Stop = green ripple.
//  * Tap a planet (or its label) -> detail_open(sid).
// CPU: only small lv_objs move (no full-screen canvas). A 60 ms lv_timer (~16 fps) moves objects only when their
// integer pixel position changes, so LVGL redraws a handful of small dirty rects. Paused when the tile is hidden,
// when the detail overlay is open, or when the ambient night clock covers the screen.
// Frame stats are logged every 10 s while visible: {"t":"log","msg":"orbit: ..."}.
#include "screens.h"
#include <math.h>
#include "../ui/theme.h"
#include "../core/model.h"
#include "../fx/ambient.h"

#define OB_MAXP 8     // planets
#define OB_MAXM 4     // moons per planet
#define OB_MAXS 40    // sparks
#define OB_MAXR 8     // ripples

static const int CX = 240, CY = 226;
static const float R_MIN = 106, R_MAX = 186;
static const uint32_t FRAME_MS = 60;   // ~16 fps target
static const float TAU = 6.2831853f;

enum { ST_BUSY, ST_WAIT, ST_IDLE, ST_OFF };

struct Moon {
  char id[12];
  bool busy;
  float ang;
  int px, py;
  lv_obj_t *o;
};

struct Planet {
  bool used, seen;
  char sid[12];
  char name[28];
  uint8_t st;
  uint32_t started;
  float ang, omega, r, r_tgt, d, d_tgt, heat;
  int px, py, pd;        // last applied pixel centre / diameter
  int tx, ty;            // label anchor (moves with 2 px hysteresis: text is the costliest thing to redraw)
  int lw, lh;            // label size
  int rank;
  int nm;
  Moon m[OB_MAXM];
  lv_obj_t *body, *tag, *label;   // tag = plain box around the label (moving it doesn't re-layout text)
};

struct Spark {
  bool on;
  float x, y, vx, vy, life, max;
  int home;            // >=0: beam homing on planet index
  float sx, sy;
  uint8_t size;
  int px, py;
  lv_obj_t *o;
};

struct Ripple {
  bool on;
  int planet;
  float t, max, r0, r1;
  lv_obj_t *o;
};

static lv_obj_t *root, *bg, *eyes[2];
static lv_color_t *bg_buf;          // 480x480 RGB565 canvas in PSRAM: background, stars, halo, orbit rings
static bool bg_dirty = true;
static uint32_t bg_last = 0;
struct RingSpec { int16_t r; lv_color_t c; lv_opa_t opa; };
static RingSpec ring_spec[OB_MAXP];
static lv_obj_t *hdr_title, *hdr_sum, *hint, *legend_l, *legend_r;
static Planet P[OB_MAXP];
static Spark S[OB_MAXS];
static Ripple R[OB_MAXR];
static lv_timer_t *timer;
static bool visible = false;
static uint32_t last_ms = 0;
static int look_at = -1;            // planet index Clawd is watching
static int eye_dx = 99, eye_dy = 99, eye_h = 99;
static uint32_t blink_at = 0;
static int n_busy = 0, n_wait = 0, n_sub = 0;
static int ring_n = 0;

// perf
static void (*prev_monitor)(lv_disp_drv_t *, uint32_t, uint32_t) = nullptr;
static bool monitor_set = false;
static uint32_t dbg_flags = 0, pf_sun = 0, pf_pl = 0, pf_fx = 0, pf_bg = 0;
static uint32_t pf_frames = 0, pf_render = 0, pf_rmax = 0, pf_px = 0, pf_calc_us = 0, pf_ticks = 0, pf_t0 = 0;

static uint32_t hash32(const char *s) {
  uint32_t h = 2166136261u;
  while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
  return h;
}
static float frand() { return (float)(lv_rand(0, 10000)) / 10000.0f; }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
// add/clear HIDDEN always invalidate, even when nothing changes — only touch the flag on a real change
static void show(lv_obj_t *o, bool on) {
  if (on == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
}

static lv_obj_t *circle(lv_obj_t *parent, int d, lv_color_t c, lv_opa_t opa) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, d, d);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(o, c, 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  return o;
}

static lv_color_t tool_color(const char *tool) {
  if (!tool) return C_ACCENT2;
  if (!strcmp(tool, "Read") || !strcmp(tool, "Grep") || !strcmp(tool, "Glob") || !strcmp(tool, "LS"))
    return lv_color_hex(0x6A9BCC);
  if (!strcmp(tool, "Edit") || !strcmp(tool, "Write") || !strcmp(tool, "MultiEdit") || !strcmp(tool, "NotebookEdit"))
    return lv_color_hex(0x7ED3A4);
  if (!strcmp(tool, "Bash") || !strcmp(tool, "BashOutput") || !strcmp(tool, "KillShell"))
    return lv_color_hex(0xF3E3C3);
  if (!strncmp(tool, "Web", 3)) return lv_color_hex(0xE88FB8);
  if (!strcmp(tool, "Agent") || !strcmp(tool, "Task")) return C_SUBAGENT;
  return C_ACCENT2;
}

static uint8_t parse_status(const char *s) {
  if (!strcmp(s, "busy")) return ST_BUSY;
  if (!strcmp(s, "waiting")) return ST_WAIT;
  if (!strcmp(s, "idle")) return ST_IDLE;
  return ST_OFF;
}
static lv_color_t st_color(uint8_t st) {
  return st == ST_BUSY ? C_BUSY : st == ST_WAIT ? C_WAITING : st == ST_IDLE ? C_IDLE : C_OFFLINE;
}

// ------------------------------------------------------------------------------------------------ fx pools
static Spark *spark_alloc() {
  for (auto &s : S) if (!s.on) return &s;
  // steal the oldest
  Spark *best = &S[0];
  for (auto &s : S) if (s.life > best->life) best = &s;
  return best;
}

static void spark_burst(float x, float y, lv_color_t c, int n, float speed) {
  float ox = x - CX, oy = y - CY;
  float base = atan2f(oy, ox);
  for (int i = 0; i < n; i++) {
    Spark *s = spark_alloc();
    float a = base + (frand() - 0.5f) * 2.2f;
    float v = speed * (0.6f + 0.6f * frand());
    s->on = true;
    s->home = -1;
    s->x = x; s->y = y;
    s->vx = cosf(a) * v; s->vy = sinf(a) * v;
    s->life = 0;
    s->max = 0.7f + 0.5f * frand();
    s->size = 4 + (uint8_t)lv_rand(0, 2);
    s->px = s->py = -999;
    lv_obj_set_size(s->o, s->size, s->size);
    lv_obj_set_style_bg_color(s->o, c, 0);
    lv_obj_set_style_bg_opa(s->o, LV_OPA_COVER, 0);
    lv_obj_add_flag(s->o, LV_OBJ_FLAG_HIDDEN);
  }
}

static void beam(int pi, lv_color_t c) {
  Spark *s = spark_alloc();
  s->on = true;
  s->home = pi;
  s->sx = CX; s->sy = CY;
  s->x = CX; s->y = CY;
  s->life = 0;
  s->max = 0.9f;
  s->size = 7;
  s->px = s->py = -999;
  lv_obj_set_size(s->o, 7, 7);
  lv_obj_set_style_bg_color(s->o, c, 0);
  lv_obj_set_style_bg_opa(s->o, LV_OPA_COVER, 0);
  lv_obj_add_flag(s->o, LV_OBJ_FLAG_HIDDEN);
}

static void ripple(int pi, lv_color_t c, float r1, float dur, float delay = 0) {
  Ripple *best = nullptr;
  for (auto &r : R) if (!r.on) { best = &r; break; }
  if (!best) best = &R[0];
  best->on = true;
  best->planet = pi;
  best->t = -delay;
  best->max = dur;
  best->r0 = P[pi].d * 0.5f + 2;
  best->r1 = r1;
  lv_obj_set_style_border_color(best->o, c, 0);
  lv_obj_add_flag(best->o, LV_OBJ_FLAG_HIDDEN);
}

static void fx_clear() {
  for (auto &s : S) { s.on = false; lv_obj_add_flag(s.o, LV_OBJ_FLAG_HIDDEN); }
  for (auto &r : R) { r.on = false; lv_obj_add_flag(r.o, LV_OBJ_FLAG_HIDDEN); }
}

// Move an object without touching its x/y styles: style changes mark the whole screen's layout dirty and make
// LVGL walk every object of every tile on the next refresh. Here we shift coords directly and invalidate
// old+new area. Objects moved this way never get a layout pass of their own (no layout, fixed size), and any
// size change goes through move_sized() which also syncs the style position.
static void fast_move(lv_obj_t *o, int x, int y) {
  if (dbg_flags & 128) { lv_obj_set_pos(o, x, y); return; }
  lv_coord_t nx = root->coords.x1 + x, ny = root->coords.y1 + y;
  lv_coord_t dx = nx - o->coords.x1, dy = ny - o->coords.y1;
  if (!dx && !dy) return;
  lv_obj_invalidate(o);
  lv_area_move(&o->coords, dx, dy);
  lv_obj_move_children_by(o, dx, dy, false);
  lv_obj_invalidate(o);
}
static void move_sized(lv_obj_t *o, int x, int y, int w, int h) {
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
}

// ------------------------------------------------------------------------------------------------ planets
static void click_cb(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i >= 0 && i < OB_MAXP && P[i].used) detail_open(P[i].sid);
}

static void planet_style(Planet &p) {
  lv_color_t c = st_color(p.st);
  lv_obj_set_style_bg_color(p.body, lv_color_lighten(c, 70), 0);
  lv_obj_set_style_bg_grad_color(p.body, lv_color_darken(c, 90), 0);
  lv_obj_set_style_outline_color(p.body, c, 0);
  bool off = p.st == ST_OFF;
  lv_obj_set_style_opa(p.body, off ? LV_OPA_40 : LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(p.label, p.st == ST_WAIT ? C_WAITING : off ? C_DIM : C_TEXT, 0);
  lv_obj_set_style_text_opa(p.label, p.st == ST_IDLE ? LV_OPA_70 : LV_OPA_COVER, 0);
  if (p.st != ST_WAIT && p.st != ST_BUSY) lv_obj_set_style_outline_opa(p.body, LV_OPA_TRANSP, 0);
}

static void planet_hide(Planet &p) {
  p.used = false;
  lv_obj_add_flag(p.body, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(p.tag, LV_OBJ_FLAG_HIDDEN);
  for (auto &m : p.m) lv_obj_add_flag(m.o, LV_OBJ_FLAG_HIDDEN);
  p.nm = 0;
}

static int planet_find(const char *sid) {
  if (!sid || !*sid) return -1;
  for (int i = 0; i < OB_MAXP; i++) if (P[i].used && !strcmp(P[i].sid, sid)) return i;
  return -1;
}

static void layout_rings() {
  int idx[OB_MAXP], n = 0;
  for (int i = 0; i < OB_MAXP; i++) if (P[i].used) idx[n++] = i;
  // sort by start time (oldest innermost), id as tiebreak — stable while sessions come and go
  for (int a = 1; a < n; a++)
    for (int b = a; b > 0; b--) {
      Planet &x = P[idx[b - 1]], &y = P[idx[b]];
      if (x.started > y.started || (x.started == y.started && strcmp(x.sid, y.sid) > 0)) {
        int t = idx[b]; idx[b] = idx[b - 1]; idx[b - 1] = t;
      } else break;
    }
  float step = n > 1 ? fminf(52.f, (R_MAX - R_MIN) / (n - 1)) : 0;
  float r0 = (R_MIN + R_MAX) * 0.5f - step * (n - 1) * 0.5f;
  for (int k = 0; k < OB_MAXP; k++) {
    if (k < n) {
      Planet &p = P[idx[k]];
      p.rank = k;
      p.r_tgt = r0 + step * k;
      lv_color_t rc = p.st == ST_WAIT ? C_WAITING : p.st == ST_BUSY ? C_ACCENT : C_BORDER;
      lv_opa_t ro = p.st == ST_WAIT ? 120 : p.st == ST_BUSY ? 80 : p.st == ST_OFF ? 90 : 190;
      RingSpec rs = {(int16_t)lroundf(p.r_tgt), rc, ro};
      if (k >= ring_n || rs.r != ring_spec[k].r || rs.c.full != ring_spec[k].c.full || rs.opa != ring_spec[k].opa) { ring_spec[k] = rs; bg_dirty = true; }
    }
  }
  if (n != ring_n) bg_dirty = true;
  ring_n = n;
}

static float planet_size(JsonObjectConst s) {
  JsonObjectConst t = s["tokens"];
  double tok = (double)(t["in"] | 0) + (double)(t["out"] | 0) + (double)(t["cache_write"] | 0) +
               (double)(t["cache_read"] | 0) / 10.0;
  float d = 18 + 7.0f * log10f((float)fmax(1.0, tok / 10000.0));
  return clampf(d, 18, 38);
}

static void set_text_if(lv_obj_t *l, const char *t) {
  if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static void update(JsonObjectConst s) {
  for (auto &p : P) p.seen = false;
  n_busy = n_wait = n_sub = 0;
  int count = 0;
  for (JsonObjectConst x : s["sessions"].as<JsonArrayConst>()) {
    if (count >= OB_MAXP) break;
    const char *sid = x["id"] | "";
    if (!*sid) continue;
    count++;
    int i = planet_find(sid);
    bool fresh = false;
    if (i < 0) {
      for (int k = 0; k < OB_MAXP; k++) if (!P[k].used) { i = k; break; }
      if (i < 0) break;
      Planet &p = P[i];
      memset(p.sid, 0, sizeof p.sid);
      strlcpy(p.sid, sid, sizeof p.sid);
      p.used = true;
      p.ang = fmodf(i * 2.39996f + (hash32(sid) % 1000) / 1000.0f, TAU);   // golden-angle spread
      p.omega = 0;
      p.r = R_MAX + 60;   // fly in from outside
      p.d = 4;
      p.heat = 0;
      p.name[0] = 0;
      p.px = p.py = p.pd = -999;
      p.st = 255;
      p.nm = 0;
      fresh = true;
      lv_obj_clear_flag(p.body, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(p.tag, LV_OBJ_FLAG_HIDDEN);
    }
    Planet &p = P[i];
    p.seen = true;
    p.started = x["started"] | 0;
    const char *nm = x["project"] | (x["name"] | "?");
    if (strncmp(p.name, nm, sizeof p.name - 1)) {
      strlcpy(p.name, nm, sizeof p.name);
      char shown[20];
      strlcpy(shown, nm, sizeof shown);
      if (strlen(nm) > 16) strcpy(shown + 15, "..");
      lv_label_set_text(p.label, shown);
      lv_point_t sz;
      lv_txt_get_size(&sz, shown, F_SMALL, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      p.lw = sz.x + 8;
      p.lh = sz.y + 2;
      lv_obj_set_size(p.tag, p.lw, p.lh);
      p.px = -999;
    }
    uint8_t st = parse_status(x["status"] | "offline");
    if (st != p.st) {
      p.st = st;
      planet_style(p);
    }
    if (st == ST_BUSY) n_busy++;
    else if (st == ST_WAIT) n_wait++;
    p.d_tgt = planet_size(x);
    if (st == ST_WAIT) p.d_tgt = fmaxf(p.d_tgt, 24);
    (void)fresh;

    // moons
    int nm2 = 0;
    for (JsonObjectConst a : x["subagents"].as<JsonArrayConst>()) {
      if (nm2 >= OB_MAXM) break;
      const char *aid = a["id"] | "";
      bool busy = !strcmp(a["status"] | "", "busy");
      if (busy) n_sub++;
      Moon &m = p.m[nm2];
      if (strcmp(m.id, aid) || nm2 >= p.nm) {
        strlcpy(m.id, aid, sizeof m.id);
        m.ang = nm2 * TAU / OB_MAXM + (hash32(aid) % 100) / 100.0f;
        m.px = m.py = -999;
      }
      if (m.busy != busy || nm2 >= p.nm) {
        m.busy = busy;
        int md = busy ? 8 : 5;
        lv_obj_set_size(m.o, md, md);
        lv_obj_set_style_bg_opa(m.o, busy ? LV_OPA_COVER : LV_OPA_50, 0);
        m.px = -999;
      }
      show(m.o, true);
      nm2++;
    }
    for (int k = nm2; k < OB_MAXM; k++) { show(p.m[k].o, false); p.m[k].id[0] = 0; }
    p.nm = nm2;
  }
  for (auto &p : P) if (p.used && !p.seen) planet_hide(p);
  layout_rings();

  // header + empty hint
  char b[80];
  if (n_wait) snprintf(b, sizeof b, "#F2C14E %d waiting#   #D97757 %d busy#", n_wait, n_busy);
  else if (n_busy) snprintf(b, sizeof b, "#D97757 %d busy#   #9C978E %d total#", n_busy, count);
  else snprintf(b, sizeof b, "#9C978E %d sessions#", count);
  set_text_if(hdr_sum, b);
  if (n_sub) snprintf(b, sizeof b, "ORBIT   #B392F0 %d sub#", n_sub);
  else snprintf(b, sizeof b, "ORBIT");
  set_text_if(hdr_title, b);
  if (count == 0) {
    set_text_if(hint, model_host_connected() ? "no agents in orbit" : "waiting for host...");
    show(hint, true);
  } else {
    show(hint, false);
  }
}

// ------------------------------------------------------------------------------------------------ clawd
// From the Claude Code banner:  ▐▛███▜▌ / ▝▜█████▛▘ /   ▘▘ ▝▝   (each block char = 2x2 pixels).
// '#' body, 'o' eye socket (drawn as body; the eye objects sit on top), '.' empty.
// Terminal quadrant blocks are twice as tall as wide, so Clawd's pixels are 7 x 14.
static const int CLAWD_W = 18, CLAWD_H = 5, PX = 7, PY = 14;
static const char *const CLAWD[CLAWD_H] = {
    "...############...",
    "...##o######o##...",
    ".################.",
    "...############...",
    "....#.#....#.#....",
};
static const int CLAWD_X = CX - CLAWD_W * PX / 2, CLAWD_Y = CY - CLAWD_H * PY / 2;

// eyes: dx/dy in px toward the watched planet; h = eye height (PY open, small = sleepy/blink)
static void eyes_set(int dx, int dy, int h) {
  if (dx == eye_dx && dy == eye_dy && h == eye_h) return;
  eye_dx = dx; eye_dy = dy; eye_h = h;
  static const int col[2] = {5, 12};
  for (int i = 0; i < 2; i++) {
    int x = CLAWD_X + col[i] * PX + dx, y = CLAWD_Y + 1 * PY + dy + (PY - h);
    lv_obj_set_pos(eyes[i], x, y);
    lv_obj_set_size(eyes[i], PX, h);
  }
}

static void clawd_frame() {
  uint32_t now = millis();
  int target = look_at;
  for (int i = 0; i < OB_MAXP; i++) if (P[i].used && P[i].st == ST_WAIT) { target = i; break; }
  int dx = 0, dy = 0;
  if (target >= 0 && target < OB_MAXP && P[target].used && P[target].px > -100) {
    float vx = P[target].px - CX, vy = P[target].py - CY;
    float l = sqrtf(vx * vx + vy * vy);
    if (l > 1) { dx = (int)lroundf(2.4f * vx / l); dy = (int)lroundf(3.4f * vy / l); }
  }
  int h = PY;
  if (!n_busy && !n_wait) { h = 4; dx = 0; dy = 0; }          // nobody working: sleepy
  if (!blink_at) blink_at = now + 3000 + lv_rand(0, 4000);
  if (now >= blink_at) {
    h = 3;
    if (now >= blink_at + 140) blink_at = now + 3000 + lv_rand(0, 5000);
  }
  eyes_set(dx, dy, h);
}

// ------------------------------------------------------------------------------------------------ frame
// Static layers are baked into one opaque canvas: redrawing a dirty rect then is a plain blit instead of
// re-evaluating big anti-aliased circle masks (8 ring borders cost ~1 us/px as objects).
static void bg_redraw() {
  bg_dirty = false;
  bg_last = millis();
  if (!bg) return;
  lv_canvas_fill_bg(bg, C_BG, LV_OPA_COVER);
  lv_draw_rect_dsc_t rd;
  lv_draw_rect_dsc_init(&rd);
  rd.radius = LV_RADIUS_CIRCLE;
  uint32_t seed = 0x2545F491;
  for (int i = 0; i < 52; i++) {
    seed = seed * 1664525u + 1013904223u;
    int x = 10 + (seed >> 8) % 460;
    seed = seed * 1664525u + 1013904223u;
    int y = 10 + (seed >> 8) % 440;
    int dx = x - CX, dy = y - CY;
    if (dx * dx + dy * dy < 64 * 64) continue;
    int sz = (seed >> 4) % 5 == 0 ? 3 : 2;
    rd.bg_color = (seed & 64) ? C_TEXT : C_ACCENT2;
    rd.bg_opa = (lv_opa_t)(25 + (seed >> 20) % 60);
    lv_canvas_draw_rect(bg, x, y, sz, sz, &rd);
  }
  // Clawd body, crisp pixel grid (see CLAWD[] below)
  rd.radius = 0;
  rd.bg_opa = LV_OPA_COVER;
  rd.bg_color = C_ACCENT;
  for (int r = 0; r < CLAWD_H; r++)
    for (int c = 0; c < CLAWD_W; c++)
      if (CLAWD[r][c] == '#' || CLAWD[r][c] == 'o')
        lv_canvas_draw_rect(bg, CLAWD_X + c * PX, CLAWD_Y + r * PY, PX, PY, &rd);
  lv_draw_arc_dsc_t ad;
  lv_draw_arc_dsc_init(&ad);
  ad.width = 1;
  for (int k = 0; k < ring_n; k++) {
    ad.color = ring_spec[k].c;
    ad.opa = ring_spec[k].opa;
    lv_canvas_draw_arc(bg, CX, CY, ring_spec[k].r, 0, 360, &ad);
  }
}

static void frame_cb(lv_timer_t *) {
  uint32_t now = millis();
  float dt = clampf((now - last_ms) / 1000.f, 0, 0.2f);
  last_ms = now;
  if (!visible || detail_is_open() || ambient_saver_active()) return;
  uint32_t t0 = micros();
  if (bg_dirty && now - bg_last > 800) { bg_redraw(); pf_bg++; }

  uint32_t ts0 = micros();
  if (!(dbg_flags & 1)) clawd_frame();
  uint32_t ts1 = micros();
  pf_sun += ts1 - ts0;

  float ping = fmodf(now / 1600.f, 1.f);   // sonar phase for waiting planets
  for (int i = 0; i < OB_MAXP; i++) {
    Planet &p = P[i];
    if (!p.used || (dbg_flags & 2)) continue;
    float kepler = TAU / (46.f * powf(p.r_tgt / 100.f, 1.5f));
    float mul = p.st == ST_BUSY ? 1.0f + 1.6f * fminf(p.heat, 1.5f)
              : p.st == ST_IDLE ? 0.22f : p.st == ST_WAIT ? 0.08f : 0.03f;
    p.omega += (kepler * mul - p.omega) * fminf(1, dt * 1.2f);
    p.ang = fmodf(p.ang + p.omega * dt, TAU);
    p.r += (p.r_tgt - p.r) * fminf(1, dt * 2.5f);
    p.d += (p.d_tgt - p.d) * fminf(1, dt * 3.f);
    p.heat *= expf(-dt / 6.f);

    float x = CX + p.r * cosf(p.ang), y = CY + p.r * sinf(p.ang);
    int ix = (int)lroundf(x), iy = (int)lroundf(y), id = (int)lroundf(p.d);
    if (ix != p.px || iy != p.py || id != p.pd) {
      if (id != p.pd) {
        move_sized(p.body, ix - id / 2, iy - id / 2, id, id);
        lv_obj_set_ext_click_area(p.body, id < 44 ? (44 - id) / 2 : 0);
      } else {
        fast_move(p.body, ix - id / 2, iy - id / 2);
      }
      if (abs(ix - p.tx) >= 2 || abs(iy - p.ty) >= 2 || id != p.pd || p.px < -100) {
        int lx = (int)clampf(ix - p.lw / 2, 14, 466 - p.lw);
        int ly = (int)fminf(iy + id / 2 + 6, 450 - p.lh);
        fast_move(p.tag, lx, ly);
        p.tx = ix; p.ty = iy;
      }
      p.px = ix; p.py = iy; p.pd = id;
    }
    // glow
    if (p.st == ST_WAIT) {
      lv_obj_set_style_outline_width(p.body, 2, 0);
      lv_obj_set_style_outline_pad(p.body, (lv_coord_t)(2 + ping * 14), 0);
      lv_obj_set_style_outline_opa(p.body, (lv_opa_t)(220 * (1 - ping)), 0);
    } else if (p.st == ST_BUSY) {
      lv_opa_t o = (lv_opa_t)(50 + 110 * fminf(p.heat, 1.f));
      if (abs((int)lv_obj_get_style_outline_opa(p.body, 0) - o) > 6) {
        lv_obj_set_style_outline_width(p.body, 3, 0);
        lv_obj_set_style_outline_pad(p.body, 3, 0);
        lv_obj_set_style_outline_opa(p.body, o, 0);
      }
    }
    // moons
    float mr = p.d * 0.5f + 9;
    for (int k = 0; k < p.nm; k++) {
      Moon &m = p.m[k];
      if (m.busy) m.ang = fmodf(m.ang + dt * TAU / 3.2f, TAU);
      int mx = (int)lroundf(x + mr * cosf(m.ang)), my = (int)lroundf(y + mr * sinf(m.ang));
      if (mx != m.px || my != m.py) {
        int md = m.busy ? 8 : 5;
        fast_move(m.o, mx - md / 2, my - md / 2);
        m.px = mx; m.py = my;
      }
    }
  }
  uint32_t ts2 = micros();
  pf_pl += ts2 - ts1;
  if (dbg_flags & 8) {
    static uint32_t dump_ms = 0;
    lv_disp_t *dd = lv_disp_get_default();
    if (now - dump_ms > 2000 && dd) {
      dump_ms = now;
      char b[400];
      int n = 0;
      for (int k = 0; k < dd->inv_p && n < 380; k++) {
        const lv_area_t &ar = dd->inv_areas[k];
        n += snprintf(b + n, sizeof b - n, "%s%d,%d %dx%d", dd->inv_area_joined[k] ? "j" : "", ar.x1, ar.y1,
                      lv_area_get_width(&ar), lv_area_get_height(&ar));
        if (n < 380) b[n++] = ';';
      }
      b[n] = 0;
      Serial.printf("{\"t\":\"log\",\"msg\":\"inv %d: %s\"}\n", dd->inv_p, b);
    }
  }

  // sparks
  for (auto &s : S) {
    if (!s.on) continue;
    s.life += dt;
    if (s.life >= s.max) { s.on = false; lv_obj_add_flag(s.o, LV_OBJ_FLAG_HIDDEN); continue; }
    float k = s.life / s.max;
    if (s.home >= 0) {
      if (!P[s.home].used) { s.on = false; lv_obj_add_flag(s.o, LV_OBJ_FLAG_HIDDEN); continue; }
      float e = k * k * (3 - 2 * k);
      s.x = s.sx + (P[s.home].px - s.sx) * e;
      s.y = s.sy + (P[s.home].py - s.sy) * e;
      lv_obj_set_style_bg_opa(s.o, (lv_opa_t)(255 * (k < 0.85f ? 1 : (1 - k) / 0.15f)), 0);
    } else {
      s.x += s.vx * dt;
      s.y += s.vy * dt;
      s.vx *= 0.96f; s.vy *= 0.96f;
      lv_obj_set_style_bg_opa(s.o, (lv_opa_t)(255 * (1 - k * k)), 0);
    }
    int ix = (int)s.x - s.size / 2, iy = (int)s.y - s.size / 2;
    if (ix != s.px || iy != s.py) { fast_move(s.o, ix, iy); s.px = ix; s.py = iy; }
    show(s.o, true);
  }
  // ripples
  for (auto &r : R) {
    if (!r.on) continue;
    r.t += dt;
    if (r.t < 0) continue;
    if (r.t >= r.max || !P[r.planet].used) { r.on = false; lv_obj_add_flag(r.o, LV_OBJ_FLAG_HIDDEN); continue; }
    float k = r.t / r.max;
    float e = 1 - (1 - k) * (1 - k);
    int rad = (int)(r.r0 + (r.r1 - r.r0) * e);
    Planet &p = P[r.planet];
    move_sized(r.o, p.px - rad, p.py - rad, rad * 2, rad * 2);
    lv_obj_set_style_border_opa(r.o, (lv_opa_t)(230 * (1 - k)), 0);
    show(r.o, true);
  }

  pf_fx += micros() - ts2;
  pf_calc_us += micros() - t0;
  pf_ticks++;
  if (now - pf_t0 >= 5000) {
    if (pf_t0 && pf_frames)
      Serial.printf("{\"t\":\"log\",\"msg\":\"orbit[%x]: %.1f fps, render avg %u ms max %u ms, %u px/frame, "
                    "calc %u us (clawd %u pl %u fx %u) bg %u\"}\n", (unsigned)dbg_flags,
                    pf_frames * 1000.f / (now - pf_t0), (unsigned)(pf_render / pf_frames), (unsigned)pf_rmax,
                    (unsigned)(pf_px / pf_frames), (unsigned)(pf_ticks ? pf_calc_us / pf_ticks : 0),
                    (unsigned)(pf_sun / pf_ticks), (unsigned)(pf_pl / pf_ticks), (unsigned)(pf_fx / pf_ticks),
                    (unsigned)pf_bg);
    pf_t0 = now;
    pf_frames = pf_render = pf_rmax = pf_px = pf_calc_us = pf_ticks = 0;
    pf_sun = pf_pl = pf_fx = pf_bg = 0;
  }
}

static void monitor_cb(lv_disp_drv_t *d, uint32_t ms, uint32_t px) {
  pf_frames++;
  pf_render += ms;
  if (ms > pf_rmax) pf_rmax = ms;
  pf_px += px;
  if (prev_monitor) prev_monitor(d, ms, px);
}

// ------------------------------------------------------------------------------------------------ events
static void on_event(JsonObjectConst ev) {
  const char *k = ev["kind"] | "";
  if (!strcmp(k, "dev_orbit")) {   // dev: {"t":"event","kind":"dev_orbit","flags":N} 1=no clawd 2=freeze planets 4=no fx
    dbg_flags = ev["flags"] | 0;
    pf_t0 = millis();
    pf_frames = pf_render = pf_rmax = pf_px = pf_calc_us = pf_ticks = 0;
    pf_sun = pf_pl = pf_fx = pf_bg = 0;
    return;
  }
  if (!visible || (dbg_flags & 4)) return;
  int i = planet_find(ev["sid"] | "");
  if (i < 0) return;
  Planet &p = P[i];
  if (p.px < -100) return;
  float x = p.px, y = p.py;
  // event from a subagent: spark from its moon
  const char *agent = ev["agent"] | "";
  if (*agent)
    for (int m = 0; m < p.nm; m++)
      if (!strcmp(p.m[m].id, agent) && p.m[m].px > -100) { x = p.m[m].px; y = p.m[m].py; }

  if (strcmp(k, "subagent_stop") && strcmp(k, "todo_done")) look_at = i;
  if (!strcmp(k, "tool")) {
    p.heat = fminf(p.heat + 0.35f, 1.6f);
    spark_burst(x, y, tool_color(ev["tool"] | ""), 3, 55);
  } else if (!strcmp(k, "tool_error")) {
    spark_burst(x, y, C_ERROR, 6, 70);
    ripple(i, C_ERROR, p.d * 0.5f + 16, 0.6f);
  } else if (!strcmp(k, "waiting")) {
    ripple(i, C_WAITING, p.d * 0.5f + 34, 1.4f);
    ripple(i, C_WAITING, p.d * 0.5f + 34, 1.4f, 0.35f);
  } else if (!strcmp(k, "stop")) {
    ripple(i, C_IDLE, p.d * 0.5f + 30, 1.2f);
    spark_burst(x, y, C_IDLE, 4, 40);
  } else if (!strcmp(k, "prompt")) {
    beam(i, C_ACCENT2);
    p.heat = fminf(p.heat + 0.5f, 1.6f);
  } else if (!strcmp(k, "subagent_start")) {
    ripple(i, C_SUBAGENT, p.d * 0.5f + 20, 0.9f);
  } else if (!strcmp(k, "subagent_stop")) {
    spark_burst(x, y, C_SUBAGENT, 3, 35);
  } else if (!strcmp(k, "todo_done")) {
    spark_burst(x, y, C_IDLE, 3, 45);
  } else if (!strcmp(k, "session_start")) {
    ripple(i, C_ACCENT, p.d * 0.5f + 26, 1.0f);
  }
}

static void on_show(bool v) {
  visible = v;
  lv_disp_t *disp = lv_disp_get_default();
  if (v) {
    last_ms = millis();
    pf_t0 = 0;
    lv_timer_resume(timer);
    if (disp && !monitor_set) {
      prev_monitor = disp->driver->monitor_cb;
      disp->driver->monitor_cb = monitor_cb;
      monitor_set = true;
    }
  } else {
    lv_timer_pause(timer);
    fx_clear();
    if (disp && monitor_set && disp->driver->monitor_cb == monitor_cb) {
      disp->driver->monitor_cb = prev_monitor;
      monitor_set = false;
    }
  }
}

// ------------------------------------------------------------------------------------------------ create
static void create(lv_obj_t *parent) {
  root = lv_obj_create(parent);
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, 480, 480);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  bg_buf = (lv_color_t *)heap_caps_malloc(480 * 480 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  if (bg_buf) {
    bg = lv_canvas_create(root);
    lv_canvas_set_buffer(bg, bg_buf, 480, 480, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);
  } else {
    bg = nullptr;
  }
  bg_redraw();

  for (auto &e : eyes) {
    e = lv_obj_create(root);
    lv_obj_remove_style_all(e);
    lv_obj_set_style_bg_color(e, lv_color_hex(0x141312), 0);
    lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
    lv_obj_clear_flag(e, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  }
  eyes_set(0, 0, PY);

  // planets (pool)
  for (int i = 0; i < OB_MAXP; i++) {
    Planet &p = P[i];
    memset(&p, 0, sizeof p);
    for (int k = 0; k < OB_MAXM; k++) {
      p.m[k].o = circle(root, 8, C_SUBAGENT, LV_OPA_COVER);
      lv_obj_add_flag(p.m[k].o, LV_OBJ_FLAG_HIDDEN);
    }
    p.body = circle(root, 16, C_IDLE, LV_OPA_COVER);
    lv_obj_set_style_bg_grad_dir(p.body, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_outline_width(p.body, 3, 0);
    lv_obj_set_style_outline_pad(p.body, 3, 0);
    lv_obj_set_style_outline_opa(p.body, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(p.body, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(p.body, click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    p.px = p.py = p.pd = -999;
  }
  // labels above every planet body so a neighbour never hides a name
  for (int i = 0; i < OB_MAXP; i++) {
    Planet &p = P[i];
    p.tag = lv_obj_create(root);
    lv_obj_remove_style_all(p.tag);
    lv_obj_set_style_bg_color(p.tag, C_BG, 0);
    lv_obj_set_style_bg_opa(p.tag, LV_OPA_70, 0);
    lv_obj_set_style_radius(p.tag, 4, 0);
    lv_obj_add_flag(p.tag, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(p.tag, LV_OBJ_FLAG_SCROLL_ON_FOCUS | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(p.tag, click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    p.label = lv_label_create(p.tag);
    lv_obj_set_style_text_font(p.label, F_SMALL, 0);
    lv_obj_set_pos(p.label, 4, 1);
    lv_obj_clear_flag(p.label, LV_OBJ_FLAG_CLICKABLE);
  }

  for (auto &r : R) {
    r.o = lv_obj_create(root);
    lv_obj_remove_style_all(r.o);
    lv_obj_set_style_radius(r.o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(r.o, 2, 0);
    lv_obj_clear_flag(r.o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r.o, LV_OBJ_FLAG_HIDDEN);
    r.on = false;
  }
  for (auto &s : S) {
    s.o = circle(root, 5, C_ACCENT2, LV_OPA_COVER);
    lv_obj_add_flag(s.o, LV_OBJ_FLAG_HIDDEN);
    s.on = false;
  }

  hdr_title = lv_label_create(root);
  lv_label_set_recolor(hdr_title, true);
  lv_obj_set_style_text_font(hdr_title, F_SMALL, 0);
  lv_obj_set_style_text_color(hdr_title, C_MUTED, 0);
  lv_obj_set_style_text_letter_space(hdr_title, 2, 0);
  lv_label_set_text(hdr_title, "ORBIT");
  lv_obj_set_pos(hdr_title, 22, 16);

  hdr_sum = lv_label_create(root);
  lv_label_set_recolor(hdr_sum, true);
  lv_obj_set_style_text_font(hdr_sum, F_SMALL, 0);
  lv_label_set_text(hdr_sum, "");
  lv_obj_align(hdr_sum, LV_ALIGN_TOP_RIGHT, -22, 16);

  legend_l = lv_label_create(root);
  lv_label_set_recolor(legend_l, true);
  lv_obj_set_style_text_font(legend_l, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(legend_l, C_DIM, 0);
  lv_label_set_text(legend_l, "#6A9BCC " LV_SYMBOL_STOP "# read  #7ED3A4 " LV_SYMBOL_STOP "# edit");
  lv_obj_align(legend_l, LV_ALIGN_BOTTOM_LEFT, 22, -22);
  legend_r = lv_label_create(root);
  lv_label_set_recolor(legend_r, true);
  lv_obj_set_style_text_font(legend_r, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(legend_r, C_DIM, 0);
  lv_label_set_text(legend_r, "#F3E3C3 " LV_SYMBOL_STOP "# bash  #E88FB8 " LV_SYMBOL_STOP "# web");
  lv_obj_align(legend_r, LV_ALIGN_BOTTOM_RIGHT, -22, -22);

  hint = lv_label_create(root);
  lv_obj_set_style_text_color(hint, C_MUTED, 0);
  lv_obj_set_style_text_font(hint, F_SMALL, 0);
  lv_label_set_text(hint, "waiting for host...");
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, CY + 70);

  timer = lv_timer_create(frame_cb, FRAME_MS, NULL);
  lv_timer_pause(timer);
}

const ScreenDef screen_orbit = {"orbit", "Orbit", create, update, on_event, on_show};
