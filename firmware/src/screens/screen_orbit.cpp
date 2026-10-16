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
// CPU: only small lv_objs move. An lv_timer (~14 fps while anything is busy / animating, 4 fps when all is calm,
// 8 fps while the backlight is dimmed) moves objects only when their integer pixel position changes. Moves bypass
// LVGL's style/layout path and go through a small dirty-rect collector (dirty_*) that merges rects so LVGL's
// 32-entry invalidation buffer never overflows (overflow = full-screen redraw). The static background (stars,
// Clawd, orbit rings) is one 480x480 canvas; rings are plotted straight into its buffer (Wu circle, ~1 k px) one
// ring per frame, restoring from a pristine copy, and only 16 sector rects around the changed ring are redrawn.
// Paused when the tile is hidden, when the detail overlay is open, or when the ambient night clock covers the screen.
// Frame stats are logged every 10 s while visible: {"t":"log","msg":"orbit: ..."}.
#include "../core/link.h"
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
static const uint32_t FRAME_MS = 70;        // ~14 fps while something moves fast
static const uint32_t FRAME_WAIT_MS = 110;  // only a waiting planet's sonar ping animates (~9 fps)
static const uint32_t FRAME_DIM_MS = 125;   // backlight dimmed: nobody is looking closely
static const uint32_t FRAME_CALM_MS = 250;  // nothing busy, no fx: planets drift ~1 px / frame
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
  bool tag_clear;        // tag background dropped while the label crosses Clawd (never cut a hole in him)
  Moon m[OB_MAXM];
  lv_obj_t *body, *tag, *label;   // tag = plain box around the label (moving it doesn't re-layout text)
};

struct Spark {
  bool on;
  float x, y, vx, vy, life, max;
  int home;            // >=0: beam homing on planet index
  float sx, sy;
  uint8_t size, opa;
  int px, py;
  lv_obj_t *o;
  lv_style_t st;       // own style: opacity fades without LVGL's per-call invalidation
};

struct Ripple {
  bool on;
  int planet;
  float t, max, r0, r1;
  uint8_t opa;
  int rad;
  lv_obj_t *o;
  lv_style_t st;
};

static lv_obj_t *root, *bg, *eyes[2];
static lv_color_t *bg_buf;          // 480x480 RGB565 canvas in PSRAM: background, stars, Clawd, orbit rings
static lv_color_t *base_buf;        // same without rings (restore source when a ring changes)
struct RingSpec { int16_t r; lv_color_t c; lv_opa_t opa; };
static RingSpec ring_spec[OB_MAXP]; // wanted rings (layout_rings)
static RingSpec ring_drawn[OB_MAXP];// rings currently plotted in bg_buf (r == 0: none)
static uint32_t frame_period = 0;
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
static uint32_t dbg_flags = 0, pf_sun = 0, pf_pl = 0, pf_fx = 0, pf_bg = 0, pf_merge = 0, pf_full = 0;
static uint32_t pf_frames = 0, pf_render = 0, pf_rmax = 0, pf_px = 0, pf_calc_us = 0, pf_ticks = 0, pf_t0 = 0;

static uint32_t hash32(const char *s) {
  uint32_t h = 2166136261u;
  while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
  return h;
}
static float frand() { return (float)(lv_rand(0, 10000)) / 10000.0f; }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
// ------------------------------------------------------------------------------------------------ dirty rects
// LVGL keeps 32 invalidated areas per refresh and falls back to a FULL-SCREEN redraw when they overflow. A busy
// orbit moves ~60 small objects (old + new rect each), so animation invalidations are collected here, merged
// (overlapping/adjacent first, then the cheapest union) and handed to LVGL once per frame as <= DZ_MAX rects.
#define DZ_MAX 10   // leaves room for alerts glow (~12 rects) + LVGL-internal style invalidations
static lv_area_t dz[DZ_MAX];
static int dz_n = 0;
static inline int32_t area_px(const lv_area_t &a) { return (int32_t)(a.x2 - a.x1 + 1) * (a.y2 - a.y1 + 1); }
static inline lv_area_t area_union(const lv_area_t &a, const lv_area_t &b) {
  lv_area_t u = {LV_MIN(a.x1, b.x1), LV_MIN(a.y1, b.y1), LV_MAX(a.x2, b.x2), LV_MAX(a.y2, b.y2)};
  return u;
}
static void dirty_add(lv_area_t a) {
  if (a.x2 < a.x1 || a.y2 < a.y1) return;
  for (;;) {
    bool merged = false;
    for (int i = 0; i < dz_n; i++) {
      if (_lv_area_is_in(&a, &dz[i], 0)) return;
      lv_area_t u = area_union(a, dz[i]);
      // cheap merge: the union costs (almost) nothing more than both rects drawn separately
      if (area_px(u) <= area_px(a) + area_px(dz[i]) + 64) {
        a = u;
        dz[i] = dz[--dz_n];
        merged = true;
        break;
      }
    }
    if (!merged) break;
  }
  if (dz_n == DZ_MAX) {   // full: grow the rect for which the union wastes the fewest pixels
    int best = 0;
    int32_t bw = INT32_MAX;
    for (int i = 0; i < dz_n; i++) {
      int32_t w = area_px(area_union(a, dz[i])) - area_px(dz[i]) - area_px(a);
      if (w < bw) { bw = w; best = i; }
    }
    a = area_union(a, dz[best]);
    dz[best] = dz[--dz_n];
    pf_merge++;
    dirty_add(a);   // the grown rect may now swallow others
    return;
  }
  dz[dz_n++] = a;
}
static void dirty_obj(lv_obj_t *o) {
  lv_area_t a = o->coords;
  lv_coord_t e = _lv_obj_get_ext_draw_size(o);
  if (e) lv_area_increase(&a, e, e);
  dirty_add(a);
}
// _lv_inv_area directly: lv_obj_invalidate_area() grows every rect by 5 px per side (transformed-area slack in
// LVGL 8.4), which turns a 5 px spark into 15x15. We clip to the visible part of the tile ourselves.
static void dirty_flush() {
  lv_disp_t *d = lv_obj_get_disp(root);
  lv_area_t vis_a, scr = {0, 0, (lv_coord_t)(lv_disp_get_hor_res(d) - 1), (lv_coord_t)(lv_disp_get_ver_res(d) - 1)};
  bool any = !lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN) && _lv_area_intersect(&vis_a, &root->coords, &scr);
  for (int i = 0; i < dz_n && any; i++) {
    lv_area_t a;
    if (_lv_area_intersect(&a, &dz[i], &vis_a)) _lv_inv_area(d, &a);
  }
  dz_n = 0;
}
// Show/hide through the collector (lv_obj_add/clear_flag(HIDDEN) invalidate on their own; our pooled fx objects
// sit directly on `root` which has no layout, so flipping the flag is all LVGL would do besides that).
static void vis(lv_obj_t *o, bool on) {
  bool hidden = o->flags & LV_OBJ_FLAG_HIDDEN;
  if (on != hidden) return;
  dirty_obj(o);
  if (on) o->flags &= ~LV_OBJ_FLAG_HIDDEN;
  else o->flags |= LV_OBJ_FLAG_HIDDEN;
}
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

// (Re)arm a pooled spark: hidden, given size + colour without going through LVGL's style/layout path (a set_size
// would mark the object's layout dirty and snap it back to its style position on the next refresh).
static void spark_prep(Spark *s, uint8_t size, lv_color_t c) {
  vis(s->o, false);
  s->size = size;
  s->px = s->py = -999;
  s->o->coords.x2 = s->o->coords.x1 + size - 1;
  s->o->coords.y2 = s->o->coords.y1 + size - 1;
  lv_style_set_bg_color(&s->st, c);
  lv_style_set_bg_opa(&s->st, LV_OPA_COVER);
  s->opa = LV_OPA_COVER;
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
    spark_prep(s, 4 + (uint8_t)lv_rand(0, 2), c);
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
  spark_prep(s, 7, c);
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
  best->rad = -1;
  vis(best->o, false);
  lv_style_set_border_color(&best->st, c);
}

static void fx_clear() {
  for (auto &s : S) { s.on = false; vis(s.o, false); }
  for (auto &r : R) { r.on = false; vis(r.o, false); }
  dirty_flush();
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
  bool on = !(o->flags & LV_OBJ_FLAG_HIDDEN);
  if (on) dirty_obj(o);
  lv_area_move(&o->coords, dx, dy);
  if (lv_obj_get_child_cnt(o)) lv_obj_move_children_by(o, dx, dy, false);
  if (on) dirty_obj(o);
}
// Same for a size change of a childless fx object (ripples): set coords directly, invalidate old + new.
static void fast_place(lv_obj_t *o, int x, int y, int w, int h) {
  lv_area_t a = {(lv_coord_t)(root->coords.x1 + x), (lv_coord_t)(root->coords.y1 + y),
                 (lv_coord_t)(root->coords.x1 + x + w - 1), (lv_coord_t)(root->coords.y1 + y + h - 1)};
  if (_lv_area_is_equal(&a, &o->coords)) return;
  bool on = !(o->flags & LV_OBJ_FLAG_HIDDEN);
  if (on) dirty_obj(o);
  o->coords = a;
  if (on) dirty_obj(o);
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
      ring_spec[k] = {(int16_t)lroundf(p.r_tgt), rc, ro};
    } else {
      ring_spec[k].r = 0;   // none
    }
  }
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
  // only the sessions part (and the host link, for the empty hint) matter here; heartbeats / usage / system
  // updates leave it unchanged (the host only re-sends a part when its content changed)
  static uint32_t seen_v = 0xFFFFFFFF;
  static bool seen_host = false;
  uint32_t v = model_part_version("sessions");
  bool host = model_host_connected();
  if (v == seen_v && host == seen_host) return;
  seen_v = v; seen_host = host;
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
      // keep the style position in sync with where fast_move put the tag (the resize triggers a layout pass)
      lv_obj_set_pos(p.tag, p.tag->coords.x1 - root->coords.x1, p.tag->coords.y1 - root->coords.y1);
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
        lv_obj_set_pos(m.o, m.o->coords.x1 - root->coords.x1, m.o->coords.y1 - root->coords.y1);
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
// Stars + Clawd are drawn once (base_buf); rings are plotted on top by ring_plot() and re-synced one per frame.
static void bg_base_draw() {
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
  if (base_buf) memcpy(base_buf, bg_buf, 480 * 480 * sizeof(lv_color_t));
}

// Anti-aliased 1 px circle (Wu), 8-way symmetric, blended over base_buf so re-plotting is idempotent.
// erase=true restores the base pixels the same ring covered. ~1.2 k px for r=186: a few hundred us.
static inline void ring_px(int x, int y, lv_color_t c, uint32_t a, bool erase) {
  if ((unsigned)x >= 480 || (unsigned)y >= 480) return;
  int i = y * 480 + x;
  bg_buf[i] = erase ? base_buf[i] : lv_color_mix(c, base_buf[i], (lv_opa_t)a);
}
static void ring_plot(const RingSpec &rs, bool erase) {
  int r = rs.r;
  if (r <= 0 || !base_buf) return;
  for (int x = 0;; x++) {
    float yf = sqrtf((float)(r * r - x * x));
    if (x > yf + 1) break;
    int y0 = (int)yf;
    float f = yf - y0;
    uint32_t a0 = (uint32_t)(rs.opa * (1.f - f)), a1 = (uint32_t)(rs.opa * f);
    const int pts[2][2] = {{x, y0}, {x, y0 + 1}};
    const uint32_t as[2] = {a0, a1};
    for (int k = 0; k < 2; k++) {
      int u = pts[k][0], v = pts[k][1];
      uint32_t a = as[k];
      ring_px(CX + u, CY + v, rs.c, a, erase); ring_px(CX - u, CY + v, rs.c, a, erase);
      ring_px(CX + u, CY - v, rs.c, a, erase); ring_px(CX - u, CY - v, rs.c, a, erase);
      ring_px(CX + v, CY + u, rs.c, a, erase); ring_px(CX - v, CY + u, rs.c, a, erase);
      ring_px(CX + v, CY - u, rs.c, a, erase); ring_px(CX - v, CY - u, rs.c, a, erase);
    }
  }
}
// Invalidate the annulus [r0, r1] as 16 sector rects (~25-35 k px for a ring) instead of the whole canvas.
static void ring_dirty(int r0, int r1) {
  r0 -= 2; r1 += 2;
  if (r0 < 0) r0 = 0;
  for (int i = 0; i < 16; i++) {
    float a0 = i * TAU / 16, a1 = (i + 1) * TAU / 16;
    float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
    float xs[4] = {r0 * c0, r1 * c0, r0 * c1, r1 * c1}, ys[4] = {r0 * s0, r1 * s0, r0 * s1, r1 * s1};
    float x1 = xs[0], x2 = xs[0], y1 = ys[0], y2 = ys[0];
    for (int k = 1; k < 4; k++) {
      x1 = fminf(x1, xs[k]); x2 = fmaxf(x2, xs[k]);
      y1 = fminf(y1, ys[k]); y2 = fmaxf(y2, ys[k]);
    }
    lv_area_t a = {(lv_coord_t)(bg->coords.x1 + CX + (int)floorf(x1) - 1), (lv_coord_t)(bg->coords.y1 + CY + (int)floorf(y1) - 1),
                   (lv_coord_t)(bg->coords.x1 + CX + (int)ceilf(x2) + 1), (lv_coord_t)(bg->coords.y1 + CY + (int)ceilf(y2) + 1)};
    dirty_add(a);
  }
}
static bool ring_same(const RingSpec &a, const RingSpec &b) {
  return a.r == b.r && (a.r == 0 || (a.c.full == b.c.full && a.opa == b.opa));
}
// Bring at most one ring of bg_buf in line with ring_spec[]. Returns true if something changed.
static bool rings_sync() {
  if (!bg || !base_buf) return false;
  for (int k = 0; k < OB_MAXP; k++) {
    if (ring_same(ring_drawn[k], ring_spec[k])) continue;
    RingSpec old = ring_drawn[k];
    if (old.r) ring_plot(old, true);
    ring_drawn[k] = ring_spec[k];
    // re-plot neighbours whose pixels the erase may have touched, then the new ring
    for (int j = 0; j < OB_MAXP; j++)
      if (j != k && ring_drawn[j].r && old.r && abs(ring_drawn[j].r - old.r) <= 2) ring_plot(ring_drawn[j], false);
    if (ring_spec[k].r) ring_plot(ring_spec[k], false);
    int rn = ring_spec[k].r;
    if (!old.r) ring_dirty(rn, rn);
    else if (!rn) ring_dirty(old.r, old.r);
    else if (abs(rn - old.r) <= 6) ring_dirty(LV_MIN(rn, old.r), LV_MAX(rn, old.r));
    else { ring_dirty(old.r, old.r); ring_dirty(rn, rn); }
    return true;
  }
  return false;
}

static void frame_cb(lv_timer_t *) {
  uint32_t now = millis();
  float dt = clampf((now - last_ms) / 1000.f, 0, 0.35f);
  last_ms = now;
  if (!visible || detail_is_open() || ambient_saver_active()) return;
  uint32_t t0 = micros();
  if (rings_sync()) pf_bg++;

  uint32_t ts0 = micros();
  if (!(dbg_flags & 1)) clawd_frame();
  uint32_t ts1 = micros();
  pf_sun += ts1 - ts0;

  bool settling = false;
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
    if (p.heat > 0.01f) p.heat *= expf(-dt / 6.f);
    if (fabsf(p.r - p.r_tgt) > 0.5f || fabsf(p.d - p.d_tgt) > 0.5f) settling = true;

    float ca = cosf(p.ang), sa = sinf(p.ang);
    float x = CX + p.r * ca, y = CY + p.r * sa;
    int ix = (int)lroundf(x), iy = (int)lroundf(y), id = (int)lroundf(p.d);
    if (ix != p.px || iy != p.py || id != p.pd) {
      if (id != p.pd) {
        dirty_obj(p.body);
        move_sized(p.body, ix - id / 2, iy - id / 2, id, id);
        lv_obj_set_ext_click_area(p.body, id < 44 ? (44 - id) / 2 : 0);
      } else {
        fast_move(p.body, ix - id / 2, iy - id / 2);
      }
      // labels are the costliest thing to redraw (text + the canvas under the old rect): 3 px hysteresis
      if (abs(ix - p.tx) >= 3 || abs(iy - p.ty) >= 3 || id != p.pd || p.px < -100) {
        int lx = (int)clampf(ix - p.lw / 2, 14, 466 - p.lw);
        int ly = (int)fminf(iy + id / 2 + 6, 450 - p.lh);
        fast_move(p.tag, lx, ly);
        p.tx = ix; p.ty = iy;
        bool over = lx < CLAWD_X + CLAWD_W * PX && lx + p.lw > CLAWD_X && ly < CLAWD_Y + CLAWD_H * PY && ly + p.lh > CLAWD_Y;
        if (over != p.tag_clear) {
          p.tag_clear = over;
          lv_obj_set_style_bg_opa(p.tag, over ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        }
      }
      p.px = ix; p.py = iy; p.pd = id;
    }
    // glow
    if (p.st == ST_WAIT) {
      lv_coord_t pad = (lv_coord_t)(2 + ping * 14);
      if (pad != lv_obj_get_style_outline_pad(p.body, 0)) {
        lv_obj_set_style_outline_width(p.body, 2, 0);
        lv_obj_set_style_outline_pad(p.body, pad, 0);
        lv_obj_set_style_outline_opa(p.body, (lv_opa_t)(220 * (1 - ping)), 0);
      }
    } else if (p.st == ST_BUSY) {
      lv_opa_t o = (lv_opa_t)(50 + 110 * fminf(p.heat, 1.f));
      if (abs((int)lv_obj_get_style_outline_opa(p.body, 0) - o) > 12) {
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
    if (now - dump_ms > 2000) {
      dump_ms = now;
      char b[400];
      int n = 0;
      for (int k = 0; k < dz_n && n < 380; k++) {
        const lv_area_t &ar = dz[k];
        n += snprintf(b + n, sizeof b - n, "%d,%d %dx%d", ar.x1, ar.y1, lv_area_get_width(&ar), lv_area_get_height(&ar));
        if (n < 380) b[n++] = ';';
      }
      b[n] = 0;
      Link.printf("{\"t\":\"log\",\"msg\":\"dz %d: %s\"}\n", dz_n, b);
    }
  }

  // sparks
  bool fx_live = false;
  for (auto &s : S) {
    if (!s.on) continue;
    s.life += dt;
    if (s.life >= s.max) { s.on = false; vis(s.o, false); continue; }
    fx_live = true;
    float k = s.life / s.max;
    float o;
    if (s.home >= 0) {
      if (!P[s.home].used) { s.on = false; vis(s.o, false); continue; }
      float e = k * k * (3 - 2 * k);
      s.x = s.sx + (P[s.home].px - s.sx) * e;
      s.y = s.sy + (P[s.home].py - s.sy) * e;
      o = 255 * (k < 0.85f ? 1 : (1 - k) / 0.15f);
    } else {
      float damp = powf(0.96f, dt * 16.f);   // was 0.96 per 60 ms tick
      s.x += s.vx * dt;
      s.y += s.vy * dt;
      s.vx *= damp; s.vy *= damp;
      o = 255 * (1 - k * k);
    }
    uint8_t q = (uint8_t)(((int)o + 8) & ~15);   // 16 opacity steps: fewer style-only invalidations
    if (q != s.opa) {
      s.opa = q;
      lv_style_set_bg_opa(&s.st, q);
      if (!(s.o->flags & LV_OBJ_FLAG_HIDDEN)) dirty_obj(s.o);
    }
    int ix = (int)s.x - s.size / 2, iy = (int)s.y - s.size / 2;
    if (ix != s.px || iy != s.py) { fast_move(s.o, ix, iy); s.px = ix; s.py = iy; }
    vis(s.o, true);
  }
  // ripples
  for (auto &r : R) {
    if (!r.on) continue;
    r.t += dt;
    fx_live = true;
    if (r.t < 0) continue;
    if (r.t >= r.max || !P[r.planet].used) { r.on = false; vis(r.o, false); continue; }
    float k = r.t / r.max;
    float e = 1 - (1 - k) * (1 - k);
    int rad = (int)(r.r0 + (r.r1 - r.r0) * e);
    Planet &p = P[r.planet];
    uint8_t q = (uint8_t)(230 * (1 - k)) & ~7;
    if (q != r.opa) {
      r.opa = q;
      lv_style_set_border_opa(&r.st, q);
      if (!(r.o->flags & LV_OBJ_FLAG_HIDDEN)) dirty_obj(r.o);
    }
    fast_place(r.o, p.px - rad, p.py - rad, rad * 2, rad * 2);
    vis(r.o, true);
  }
  dirty_flush();

  // frame rate: fast only while something moves fast or animates
  uint32_t want = FRAME_CALM_MS;
  if (n_busy || fx_live || settling || n_sub) want = ambient_dimmed() ? FRAME_DIM_MS : FRAME_MS;
  else if (n_wait) want = FRAME_WAIT_MS;
  if (want != frame_period) { frame_period = want; lv_timer_set_period(timer, want); }

  pf_fx += micros() - ts2;
  pf_calc_us += micros() - t0;
  pf_ticks++;
  if (now - pf_t0 >= 5000) {
    if (pf_t0 && pf_frames)
      Link.printf("{\"t\":\"log\",\"msg\":\"orbit[%x]: %.1f fps, render avg %u ms max %u ms, %u px/frame, "
                    "calc %u us (clawd %u pl %u fx %u) rings %u merges %u full %u period %u\"}\n", (unsigned)dbg_flags,
                    pf_frames * 1000.f / (now - pf_t0), (unsigned)(pf_render / pf_frames), (unsigned)pf_rmax,
                    (unsigned)(pf_px / pf_frames), (unsigned)(pf_ticks ? pf_calc_us / pf_ticks : 0),
                    (unsigned)(pf_sun / pf_ticks), (unsigned)(pf_pl / pf_ticks), (unsigned)(pf_fx / pf_ticks),
                    (unsigned)pf_bg, (unsigned)pf_merge, (unsigned)pf_full, (unsigned)frame_period);
    pf_t0 = now;
    pf_frames = pf_render = pf_rmax = pf_px = pf_calc_us = pf_ticks = 0;
    pf_sun = pf_pl = pf_fx = pf_bg = pf_merge = pf_full = 0;
  }
}

static void monitor_cb(lv_disp_drv_t *d, uint32_t ms, uint32_t px) {
  pf_frames++;
  pf_render += ms;
  if (ms > pf_rmax) pf_rmax = ms;
  pf_px += px;
  if (px >= 480 * 470) pf_full++;
  if (prev_monitor) prev_monitor(d, ms, px);
}

// ------------------------------------------------------------------------------------------------ events
static void on_event(JsonObjectConst ev) {
  const char *k = ev["kind"] | "";
  if (!strcmp(k, "dev_orbit")) {   // dev: {"t":"event","kind":"dev_orbit","flags":N} 1=no clawd 2=freeze planets 4=no fx
    dbg_flags = ev["flags"] | 0;
    pf_t0 = millis();
    pf_frames = pf_render = pf_rmax = pf_px = pf_calc_us = pf_ticks = 0;
    pf_sun = pf_pl = pf_fx = pf_bg = pf_merge = 0;
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
    frame_period = FRAME_MS;
    lv_timer_set_period(timer, FRAME_MS);
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
  base_buf = bg_buf ? (lv_color_t *)heap_caps_malloc(480 * 480 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM) : nullptr;
  if (bg_buf) {
    bg = lv_canvas_create(root);
    lv_canvas_set_buffer(bg, bg_buf, 480, 480, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);
  } else {
    bg = nullptr;
  }
  if (bg) bg_base_draw();   // once, at boot (~100 ms); rings are added incrementally by rings_sync()

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
    // opaque square C_BG box (was 70 % + radius 4): looks the same on the C_BG canvas, keeps the top name legible
    // when two planets line up, and is a plain fill instead of a blended, corner-masked rect on every move
    lv_obj_set_style_bg_color(p.tag, C_BG, 0);
    lv_obj_set_style_bg_opa(p.tag, LV_OPA_COVER, 0);
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
    lv_style_init(&r.st);
    lv_style_set_radius(&r.st, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&r.st, 2);
    lv_style_set_border_opa(&r.st, LV_OPA_COVER);
    lv_style_set_border_color(&r.st, C_ACCENT);
    lv_obj_add_style(r.o, &r.st, 0);
    lv_obj_clear_flag(r.o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r.o, LV_OBJ_FLAG_HIDDEN);
    r.on = false;
  }
  for (auto &s : S) {
    s.o = circle(root, 5, C_ACCENT2, LV_OPA_COVER);
    lv_style_init(&s.st);
    lv_style_set_bg_color(&s.st, C_ACCENT2);
    lv_style_set_bg_opa(&s.st, LV_OPA_COVER);
    lv_obj_add_style(s.o, &s.st, 0);   // local props would win over it: drop the ones circle() set
    lv_obj_remove_local_style_prop(s.o, LV_STYLE_BG_COLOR, 0);
    lv_obj_remove_local_style_prop(s.o, LV_STYLE_BG_OPA, 0);
    s.opa = LV_OPA_COVER;
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
