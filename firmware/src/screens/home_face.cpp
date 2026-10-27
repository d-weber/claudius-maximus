// Clawd on the home screen: the isometric desk scene (iso/), full width, 3x. See home_face.h for the API.
//
// Rendering: every animation frame is drawn into a logical 240 x 121 canvas (iso library, PSRAM). The visible
// window is the middle 160 columns, upscaled 3x into a 480 x 363 RGB565 image (PSRAM) that the face object
// paints in its draw callback. Only the rows/columns that changed since the previous frame are upscaled and
// invalidated, so a blink costs a few hundred device pixels, not the whole scene. Settings.home_mirror flips
// the scene horizontally at upscale time.
#include "home_face.h"
#include "screens.h"
#include "iso/anim.h"
#include "../ui/theme.h"
#include "../core/settings.h"
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <math.h>
#include <string.h>

using namespace iso;

static const int ZOOM = 3, VIEW_X0 = 40, VIEW_W = 160;      // logical window shown: x 40..199
static const int IMG_W = VIEW_W * ZOOM, IMG_H = H * ZOOM;    // 480 x 363

static lv_obj_t *face, *bubble, *bubble_lbl, *scroller;
static void (*tap_cb)() = nullptr;
static Col *canvas, *prev_canvas;     // logical frames (current / last upscaled)
static Col *img;                      // upscaled scene
static lv_img_dsc_t img_dsc;
static bool first = true, mirrored = false;

// ---- inputs
static HomeMood mood = HOME_MOOD_OFFLINE;
static float energy_tgt = 0, energy = 0;
static int helpers = 0;
static char tool_cat = 0;

// ---- animation director
static const IsoAnim *cur = &anim_01_idle;     // the mood loop
static const IsoAnim *react_anim = nullptr;    // one-shot playing on top of the loop
static uint32_t anim_start = 0, last_ms = 0;
// Pacing of the mood loop: a full pass, then (usually) a rest in the animation's calm segment for a random
// 4-12 s, then another full pass... so the gags come at irregular, longer intervals.
static bool calm_mode = false;
static uint32_t calm_until = 0;
// Blink pacing while resting: a calm segment is short (idle: 1 s) and usually holds one blink, so looping it verbatim
// would make Clawd blink every second. Each pass of the segment rolls whether it may blink (on average one blink
// per ~3.5 s); the other passes render EYE_BLINK as open eyes (iso::blinkSuppressed).
static int calm_pass = -1;
static bool calm_blink = true;
static bool drawn_noblink = false;
static uint32_t rnd(uint32_t lo, uint32_t hi) { return lo + esp_random() % (hi - lo + 1); }
static int drawn_frame = -1;
static const IsoAnim *drawn_anim = nullptr;
static AnimEnv drawn_env;
static const IsoAnim *dbg_anim = nullptr;     // QA override (home_face_debug)
static int dbg_frame = -1, dbg_helpers = 0;
static bool dbg_grey = false;
static int dbg_mirror = -1;
static bool want_mirror() { return dbg_mirror >= 0 ? dbg_mirror != 0 : settings().home_mirror; }

// ---- bubble
static float bub_a = 0, bub_drawn = 0;
static uint32_t bub_until = 0;
static int bub_prio = 0, bub_opa_set = -1;

static const IsoAnim *mood_anim() {
  switch (mood) {
    case HOME_MOOD_OFFLINE: case HOME_MOOD_SLEEP: return &anim_02_sleep;
    case HOME_MOOD_WAITING: return &anim_03_waiting;
    case HOME_MOOD_BUSY:
      switch (tool_cat) {
        case 'r': return &anim_10_reading_doc;
        case 'e': return &anim_11_writing_code;
        case 'x': return &anim_14_running_command;
        case 'w': return &anim_15_web_search;
        case 'a': return &anim_17_delegating;
        case 'p': return &anim_12_creating_plan;
        case 'v': return &anim_13_reviewing_code;
        case 't': return &anim_16_thinking;
        default: return &anim_11_writing_code;
      }
    default: return &anim_01_idle;
  }
}

static const IsoAnim *react_to_anim(HomeReact r) {
  switch (r) {
    case HOME_REACT_HOP: return &anim_09_hop_todo;
    case HOME_REACT_SURPRISE: return &anim_06_surprise_wake;
    case HOME_REACT_WINK: return &anim_08_wink;
    case HOME_REACT_GIGGLE: return &anim_07_giggle_tap;
    case HOME_REACT_HAPPY: return &anim_04_happy_done;
    case HOME_REACT_WINCE: return &anim_05_wince_error;
    default: return nullptr;   // GLANCE: the loops glance on their own
  }
}

// ---- mini helpers (unless the animation places them itself)
static void draw_helpers(Ctx &c, int n, const AnimEnv &env, int i) {
  static const Pt spots[3] = {SPOT_frontLeft, SPOT_frontRight, SPOT_front};
  int shown = n > 3 ? 3 : n;
  for (int k = 0; k < shown; k++) {
    ClawdOpts o;
    o.mini = true;
    o.x = spots[k].x; o.y = spots[k].y;
    o.tint = env.grey ? SHADE::grey : SHADE::violet;
    o.facing = k == 1 ? RIGHT : LEFT;
    bool ph = ((i / 3) + k) & 1;
    o.legs = ph ? LEGS_STEPA : LEGS_STEPB;
    o.z = ph ? 1 : 0;
    o.eyes.shape = ((i + k * 7) % 40) == 0 ? EYE_BLINK : EYE_OPEN;
    drawClawd(c, o);
  }
  if (n > 3) {
    char b[8];
    snprintf(b, sizeof b, "+%d", n - 3);
    text(c, SPOT_front.x + 14, SPOT_front.y - 6, b, C::violet);
  }
}

// ---- upscale the changed part of the logical canvas into the image and invalidate it
static void publish(int lx0, int ly0, int lx1, int ly1) {   // inclusive logical rect
  if (lx0 < VIEW_X0) lx0 = VIEW_X0;
  if (lx1 > VIEW_X0 + VIEW_W - 1) lx1 = VIEW_X0 + VIEW_W - 1;
  if (lx0 > lx1 || ly0 > ly1) return;
  for (int ly = ly0; ly <= ly1; ly++) {
    const Col *src = canvas + ly * W;
    for (int r = 0; r < ZOOM; r++) {
      Col *dst = img + (ly * ZOOM + r) * IMG_W;
      for (int lx = lx0; lx <= lx1; lx++) {
        Col v = src[lx];
        int dx = mirrored ? (VIEW_X0 + VIEW_W - 1 - lx) * ZOOM : (lx - VIEW_X0) * ZOOM;
        dst[dx] = v; dst[dx + 1] = v; dst[dx + 2] = v;
      }
    }
  }
  lv_area_t A;
  lv_obj_get_coords(face, &A);
  int sx0 = mirrored ? (VIEW_X0 + VIEW_W - 1 - lx1) * ZOOM : (lx0 - VIEW_X0) * ZOOM;
  int sx1 = mirrored ? (VIEW_X0 + VIEW_W - 1 - lx0) * ZOOM + ZOOM - 1 : (lx1 - VIEW_X0) * ZOOM + ZOOM - 1;
  lv_area_t d = {(lv_coord_t)(A.x1 + sx0), (lv_coord_t)(A.y1 + ly0 * ZOOM), (lv_coord_t)(A.x1 + sx1), (lv_coord_t)(A.y1 + ly1 * ZOOM + ZOOM - 1)};
  lv_obj_invalidate_area(face, &d);
}

static void render(const IsoAnim *a, int frame, const AnimEnv &env) {
  Ctx c = {canvas};
  c.clear();
  a->draw(c, frame, env);
  if (!a->drawsHelpers && env.helpers > 0) draw_helpers(c, env.helpers, env, frame);
  bool mir = want_mirror();
  if (first || mir != mirrored) {
    mirrored = mir;
    memcpy(prev_canvas, canvas, W * H * sizeof(Col));
    publish(VIEW_X0, 0, VIEW_X0 + VIEW_W - 1, H - 1);
    first = false;
    return;
  }
  // dirty bbox of the logical diff
  int x0 = W, y0 = H, x1 = -1, y1 = -1;
  for (int y = 0; y < H; y++) {
    const Col *a0 = canvas + y * W + VIEW_X0, *b0 = prev_canvas + y * W + VIEW_X0;
    if (!memcmp(a0, b0, VIEW_W * sizeof(Col))) continue;
    int lx = 0, rx = VIEW_W - 1;
    while (lx < VIEW_W && a0[lx] == b0[lx]) lx++;
    while (rx > lx && a0[rx] == b0[rx]) rx--;
    if (y < y0) y0 = y;
    if (y > y1) y1 = y;
    if (VIEW_X0 + lx < x0) x0 = VIEW_X0 + lx;
    if (VIEW_X0 + rx > x1) x1 = VIEW_X0 + rx;
  }
  if (x1 < 0) return;
  memcpy(prev_canvas, canvas, W * H * sizeof(Col));
  publish(x0, y0, x1, y1);
}

static void draw_cb(lv_event_t *e) {
  lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
  lv_area_t A;
  lv_obj_get_coords(face, &A);
  lv_area_t ia = {A.x1, A.y1, (lv_coord_t)(A.x1 + IMG_W - 1), (lv_coord_t)(A.y1 + IMG_H - 1)};
  if (_lv_area_is_on(&ia, dc->clip_area)) {
    lv_draw_img_dsc_t d;
    lv_draw_img_dsc_init(&d);
    lv_draw_img(dc, &d, &ia, &img_dsc);
  }
  // bubble tail (the bubble is our child and draws after us)
  if (bub_drawn > 0.02f && !lv_obj_has_flag(bubble, LV_OBJ_FLAG_HIDDEN)) {
    lv_area_t B;
    lv_obj_get_coords(bubble, &B);
    lv_point_t pts[3] = {{(lv_coord_t)(B.x1 + 26), (lv_coord_t)(B.y2 - 4)},
                         {(lv_coord_t)(B.x1 + 50), (lv_coord_t)(B.y2 - 4)},
                         {(lv_coord_t)(B.x1 + 8), (lv_coord_t)(B.y2 + 16)}};
    lv_area_t tb = {(lv_coord_t)(B.x1 + 8), (lv_coord_t)(B.y2 - 4), (lv_coord_t)(B.x1 + 50), (lv_coord_t)(B.y2 + 16)};
    if (_lv_area_is_on(&tb, dc->clip_area)) {
      lv_draw_rect_dsc_t d;
      lv_draw_rect_dsc_init(&d);
      d.bg_color = C_SURFACE2;
      d.bg_opa = (lv_opa_t)(bub_drawn * 255);
      lv_draw_polygon(dc, &d, pts, 3);
    }
  }
}

// ---- input: a tap anywhere around Clawd (centre of the scene)
static bool hit(lv_point_t p) {
  lv_area_t A;
  lv_obj_get_coords(face, &A);
  int x = p.x - A.x1, y = p.y - A.y1;
  return x >= (80 - VIEW_X0) * ZOOM && x <= (160 - VIEW_X0) * ZOOM && y >= 10 * ZOOM && y <= 100 * ZOOM;
}

static void face_ev(lv_event_t *e) {
  lv_event_code_t c = lv_event_get_code(e);
  if (c != LV_EVENT_CLICKED) return;
  lv_point_t p = {0, 0};
  lv_indev_t *in = lv_indev_get_act();
  if (in) lv_indev_get_point(in, &p);
  if (hit(p) && tap_cb) tap_cb();
}

static void bubble_ev(lv_event_t *) {
  if (bub_until) { bub_until = lv_tick_get(); bub_prio = 0; }
}

// ---- API
void home_face_create(lv_obj_t *parent, lv_coord_t y, lv_coord_t h) {
  scroller = lv_obj_get_parent(parent);  // the tileview
  canvas = (Col *)heap_caps_calloc(W * H, sizeof(Col), MALLOC_CAP_SPIRAM);
  prev_canvas = (Col *)heap_caps_calloc(W * H, sizeof(Col), MALLOC_CAP_SPIRAM);
  img = (Col *)heap_caps_malloc(IMG_W * IMG_H * sizeof(Col), MALLOC_CAP_SPIRAM);
  if (!canvas || !prev_canvas || !img) {   // no PSRAM: no scene (face stays null, every API no-ops)
    heap_caps_free(canvas); heap_caps_free(prev_canvas); heap_caps_free(img);
    canvas = prev_canvas = img = nullptr;
    return;
  }
  for (int i = 0; i < IMG_W * IMG_H; i++) img[i] = C::bg;
  img_dsc.header.always_zero = 0;
  img_dsc.header.w = IMG_W;
  img_dsc.header.h = IMG_H;
  img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  img_dsc.data_size = IMG_W * IMG_H * sizeof(Col);
  img_dsc.data = (const uint8_t *)img;

  face = lv_obj_create(parent);
  lv_obj_remove_style_all(face);
  lv_obj_set_pos(face, 0, y);
  lv_obj_set_size(face, 480, h);
  lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(face, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(face, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_add_event_cb(face, face_ev, LV_EVENT_ALL, nullptr);

  bubble = lv_obj_create(face);
  lv_obj_remove_style_all(bubble);
  lv_obj_set_style_bg_color(bubble, C_SURFACE2, 0);
  lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(bubble, 18, 0);
  lv_obj_set_style_pad_hor(bubble, 16, 0);
  lv_obj_set_style_pad_ver(bubble, 11, 0);
  lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(bubble, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(bubble, bubble_ev, LV_EVENT_CLICKED, nullptr);
  bubble_lbl = lv_label_create(bubble);
  lv_obj_set_style_text_font(bubble_lbl, F_BODY, 0);
  lv_obj_set_style_text_color(bubble_lbl, C_TEXT, 0);
  lv_obj_set_style_text_line_space(bubble_lbl, 3, 0);
  lv_label_set_long_mode(bubble_lbl, LV_LABEL_LONG_WRAP);
  lv_label_set_text(bubble_lbl, "");
  anim_start = lv_tick_get();
}

void home_face_set_mood(HomeMood m, float e, int h, char cat) {
  bool changed = m != mood || cat != tool_cat;
  mood = m; energy_tgt = e; helpers = h; tool_cat = cat;
  if (changed) {
    cur = mood_anim();
    calm_mode = false;
    if (!react_anim) anim_start = lv_tick_get();
  }
}

void home_face_react(HomeReact r) {
  const IsoAnim *a = react_to_anim(r);
  if (!a) return;
  react_anim = a;
  anim_start = lv_tick_get();
}

void home_face_say(const char *txt, uint32_t ms, int prio) {
  if (!face) return;
  uint32_t now = lv_tick_get();
  if (bub_until && prio < bub_prio && (int32_t)(bub_until - now) > 600) return;
  lv_label_set_text(bubble_lbl, txt);
  lv_obj_set_width(bubble_lbl, LV_SIZE_CONTENT);
  lv_obj_update_layout(bubble_lbl);
  if (lv_obj_get_width(bubble_lbl) > 300) lv_obj_set_width(bubble_lbl, 300);
  lv_obj_update_layout(bubble);
  // above Clawd's head, on the left so the tail points down-right to him
  lv_obj_set_pos(bubble, 14, 6);
  lv_obj_clear_flag(bubble, LV_OBJ_FLAG_HIDDEN);
  bub_until = now + ms;
  bub_prio = prio;
}

bool home_face_bubble_visible() { return bub_until != 0; }
void home_face_on_tap(void (*cb)()) { tap_cb = cb; }
void home_face_resume() { last_ms = lv_tick_get(); drawn_frame = -1; }

void home_face_tick() {
  if (!face) return;
  uint32_t now = lv_tick_get();
  float dt = (now - last_ms) / 1000.f;
  if (dt <= 0 || dt > 0.15f) dt = 0.04f;
  last_ms = now;
  energy += (energy_tgt - energy) * (1 - expf(-dt * 1.5f));

  // bubble fade
  if (bub_until && (int32_t)(now - bub_until) >= 0) { bub_until = 0; bub_prio = 0; }
  float bt = bub_until ? 1.f : 0.f;
  bub_a += (bt - bub_a) * (1 - expf(-dt * 12));
  if (!bub_until && bub_a < 0.03f) bub_a = 0;
  if (bub_until && bub_a > 0.97f) bub_a = 1;
  int o = (int)(bub_a * 255);
  if (o == 0 && !lv_obj_has_flag(bubble, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_add_flag(bubble, LV_OBJ_FLAG_HIDDEN);
    bub_opa_set = -1;
  } else if (o > 0 && (abs(o - bub_opa_set) > 24 || (o >= 250 && bub_opa_set < 250))) {
    lv_obj_set_style_opa(bubble, o >= 250 ? LV_OPA_COVER : (lv_opa_t)o, 0);
    bub_opa_set = o >= 250 ? 255 : o;
  }

  // which animation, which frame
  const IsoAnim *a = dbg_anim ? dbg_anim : react_anim ? react_anim : cur;
  int frame = (int)((uint64_t)(now - anim_start) * a->fps / 1000);
  if (!dbg_anim && react_anim && frame >= a->frames) {       // one-shot finished: back to the mood loop
    react_anim = nullptr;
    calm_mode = false;
    a = cur;
    anim_start = now;
    frame = 0;
  }
  if (a->loop && a->frames > 0) {
    bool has_calm = a->calmFrom >= 0 && a->calmTo >= a->calmFrom && a->calmTo < a->frames;
    if (!dbg_anim && has_calm) {
      if (calm_mode) {
        if ((int32_t)(now - calm_until) >= 0) { calm_mode = false; anim_start = now; frame = 0; }
        else {
          int len = a->calmTo - a->calmFrom + 1;
          int pass = frame / len;
          if (pass != calm_pass) {   // a new pass of the calm segment: may it blink this time?
            calm_pass = pass;
            calm_blink = rnd(0, 3499) < (uint32_t)(len * 1000 / a->fps);
          }
          frame = a->calmFrom + frame % len;
        }
      } else if (frame >= a->frames) {           // full pass done: rest (70 %) or go again
        anim_start = now;
        frame = 0;
        if (rnd(0, 99) < 70) { calm_mode = true; calm_until = now + rnd(4000, 12000); frame = a->calmFrom; calm_pass = -1; }
      }
    } else {
      frame %= a->frames;
    }
  } else if (frame >= a->frames) {
    frame = a->frames - 1;
  }
  if (dbg_anim && dbg_frame >= 0) frame = dbg_frame % (a->frames > 0 ? a->frames : 1);

  if (detail_is_open()) return;
  if (scroller && (lv_obj_is_scrolling(scroller) || lv_anim_get(scroller, nullptr))) return;

  if (fabsf(bub_a - bub_drawn) > 0.005f) {
    lv_area_t B;
    lv_obj_get_coords(bubble, &B);
    lv_area_t tb = {B.x1, (lv_coord_t)(B.y2 - 6), (lv_coord_t)(B.x1 + 54), (lv_coord_t)(B.y2 + 18)};
    bub_drawn = bub_a;
    lv_obj_invalidate_area(face, &tb);
  }

  AnimEnv env;
  env.grey = dbg_anim ? dbg_grey : mood == HOME_MOOD_OFFLINE;
  env.helpers = dbg_anim ? dbg_helpers : helpers;
  env.toolCat = tool_cat;
  env.energy = energy;
  bool noblink = calm_mode && !calm_blink && !dbg_anim && !react_anim;
  if (a == drawn_anim && frame == drawn_frame && env.grey == drawn_env.grey && env.helpers == drawn_env.helpers &&
      noblink == drawn_noblink && want_mirror() == mirrored && !first)
    return;
  drawn_anim = a; drawn_frame = frame; drawn_env = env; drawn_noblink = noblink;
  iso::blinkSuppressed = noblink;
  render(a, frame, env);
  iso::blinkSuppressed = false;
}

void home_face_debug(const char *name, int frame, int nhelpers, bool grey) {
  static const IsoAnim *const ALL[] = {&anim_01_idle, &anim_02_sleep, &anim_03_waiting, &anim_04_happy_done, &anim_05_wince_error,
                                       &anim_06_surprise_wake, &anim_07_giggle_tap, &anim_08_wink, &anim_09_hop_todo,
                                       &anim_10_reading_doc, &anim_11_writing_code, &anim_12_creating_plan, &anim_13_reviewing_code,
                                       &anim_14_running_command, &anim_15_web_search, &anim_16_thinking, &anim_17_delegating};
  dbg_anim = nullptr;
  if (name && *name)
    for (const IsoAnim *a : ALL) if (!strcmp(a->name, name) || !strncmp(a->name, name, 2)) { dbg_anim = a; break; }
  dbg_frame = frame; dbg_helpers = nhelpers; dbg_grey = grey;
  anim_start = lv_tick_get();
  drawn_frame = -1;
  if (dbg_anim) { bub_until = 0; bub_prio = 0; bub_a = 0; }   // no speech bubble over a QA frame
}

void home_face_debug_mirror(int m) { dbg_mirror = m; drawn_frame = -1; }

void home_face_status(char *buf, size_t n) {
  snprintf(buf, n, "{\"anim\":\"%s\",\"frame\":%d,\"mode\":\"%s\",\"react\":%d,\"dbg\":%d,\"noblink\":%d}",
           drawn_anim ? drawn_anim->name : "", drawn_frame, calm_mode ? "calm" : "full", react_anim != nullptr, dbg_anim != nullptr,
           drawn_noblink);
}
