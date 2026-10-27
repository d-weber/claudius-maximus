// Clawd's Studio on the home screen: LVGL glue around screens/room/ (the director and the pixel engine).
//
// Rendering: the director draws every tick (12 fps) into a 120 x 106 palette-index canvas (PSRAM). The rows and
// columns that differ from the previous tick are upscaled 4x through the current palette into a 480 x 424 RGB565
// image (PSRAM) that the face object paints in its draw callback, and only that area is invalidated. A palette
// change (the light fading at dusk, the alarm) republishes the whole room. Settings.home_mirror flips the scene
// horizontally at upscale time.
#include "home_face.h"
#include "screens.h"
#include "room/director.h"
#include "../ui/theme.h"
#include "../core/settings.h"
#include "../core/model.h"
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <math.h>
#include <string.h>
#include <time.h>

using namespace room;

static const int ZOOM = 4;
static const int IMG_W = W * ZOOM, IMG_H = H * ZOOM;    // 480 x 424

static lv_obj_t *face, *bubble, *bubble_lbl, *scroller;
static void (*tap_cb)() = nullptr;
static Idx *canvas, *prev_canvas;
static uint16_t *img;
static lv_img_dsc_t img_dsc;
static bool first = true, mirrored = false;
static Director dir;
static uint32_t last_ms = 0, acc_ms = 0;
static bool dbg_forced = false;
static int dbg_helpers = -1;
static int dbg_mirror = -1;
static bool want_mirror() { return dbg_mirror >= 0 ? dbg_mirror != 0 : settings().home_mirror; }

// ---- inputs
static Input in;
static float energy_tgt = 0;

// ---- bubble
static float bub_a = 0, bub_drawn = 0;
static uint32_t bub_until = 0;
static int bub_prio = 0, bub_opa_set = -1;
static int bub_hx = -1000, bub_hy = -1000;   // head position the bubble was placed for (device px)
static bool bub_tail_right = false;

static float local_tod() {
  uint32_t now = model_now();
  if (!now) return 12.f;
  time_t lt = (time_t)now + model_tz();
  struct tm tm;
  gmtime_r(&lt, &tm);
  in.yday = tm.tm_yday;
  return tm.tm_hour + tm.tm_min / 60.f + tm.tm_sec / 3600.f;
}

// ---- upscale the changed part of the canvas into the image and invalidate it
static void publish(int lx0, int ly0, int lx1, int ly1) {   // inclusive logical rect
  if (lx0 < 0) lx0 = 0;
  if (lx1 > W - 1) lx1 = W - 1;
  if (lx0 > lx1 || ly0 > ly1) return;
  const uint16_t *pal = dir.palette(nullptr);
  for (int ly = ly0; ly <= ly1; ly++) {
    const Idx *src = canvas + ly * W;
    uint16_t *row0 = img + (ly * ZOOM) * IMG_W;
    for (int lx = lx0; lx <= lx1; lx++) {
      uint16_t v = pal[src[lx]];
      int dx = mirrored ? (W - 1 - lx) * ZOOM : lx * ZOOM;
      uint16_t *d = row0 + dx;
      d[0] = v; d[1] = v; d[2] = v; d[3] = v;
    }
    for (int r = 1; r < ZOOM; r++) {
      int dx0 = mirrored ? (W - 1 - lx1) * ZOOM : lx0 * ZOOM;
      memcpy(row0 + r * IMG_W + dx0, row0 + dx0, (lx1 - lx0 + 1) * ZOOM * sizeof(uint16_t));
    }
  }
  lv_area_t A;
  lv_obj_get_coords(face, &A);
  int sx0 = mirrored ? (W - 1 - lx1) * ZOOM : lx0 * ZOOM;
  int sx1 = mirrored ? (W - 1 - lx0) * ZOOM + ZOOM - 1 : lx1 * ZOOM + ZOOM - 1;
  lv_area_t d = {(lv_coord_t)(A.x1 + sx0), (lv_coord_t)(A.y1 + ly0 * ZOOM), (lv_coord_t)(A.x1 + sx1), (lv_coord_t)(A.y1 + ly1 * ZOOM + ZOOM - 1)};
  lv_obj_invalidate_area(face, &d);
}

static void render() {
  Canvas c = {canvas};
  dir.draw(c);
  bool pdirty = false;
  dir.palette(&pdirty);
  bool mir = want_mirror();
  if (first || pdirty || mir != mirrored) {
    mirrored = mir;
    memcpy(prev_canvas, canvas, W * H);
    publish(0, 0, W - 1, H - 1);
    first = false;
    return;
  }
  int x0 = W, y0 = H, x1 = -1, y1 = -1;
  for (int y = 0; y < H; y++) {
    const Idx *a0 = canvas + y * W, *b0 = prev_canvas + y * W;
    if (!memcmp(a0, b0, W)) continue;
    int lx = 0, rx = W - 1;
    while (lx < W && a0[lx] == b0[lx]) lx++;
    while (rx > lx && a0[rx] == b0[rx]) rx--;
    if (y < y0) y0 = y;
    if (y > y1) y1 = y;
    if (lx < x0) x0 = lx;
    if (rx > x1) x1 = rx;
  }
  if (x1 < 0) return;
  memcpy(prev_canvas, canvas, W * H);
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
  // bubble tail toward Clawd's head (the bubble is our child and draws after us)
  if (bub_drawn > 0.02f && !lv_obj_has_flag(bubble, LV_OBJ_FLAG_HIDDEN)) {
    lv_area_t B;
    lv_obj_get_coords(bubble, &B);
    lv_coord_t bx = bub_tail_right ? (lv_coord_t)(B.x2 - 50) : (lv_coord_t)(B.x1 + 26);
    lv_coord_t tipx = bub_tail_right ? (lv_coord_t)(B.x2 - 8) : (lv_coord_t)(B.x1 + 8);
    lv_point_t pts[3] = {{bx, (lv_coord_t)(B.y2 - 4)}, {(lv_coord_t)(bx + 24), (lv_coord_t)(B.y2 - 4)}, {tipx, (lv_coord_t)(B.y2 + 16)}};
    lv_area_t tb = {(lv_coord_t)(bx < tipx ? bx : tipx), (lv_coord_t)(B.y2 - 4), (lv_coord_t)((bx + 24) > tipx ? bx + 24 : tipx), (lv_coord_t)(B.y2 + 16)};
    if (_lv_area_is_on(&tb, dc->clip_area)) {
      lv_draw_rect_dsc_t d;
      lv_draw_rect_dsc_init(&d);
      d.bg_color = C_SURFACE2;
      d.bg_opa = (lv_opa_t)(bub_drawn * 255);
      lv_draw_polygon(dc, &d, pts, 3);
    }
  }
}

static void face_ev(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (tap_cb) tap_cb();
}

static void bubble_ev(lv_event_t *) {
  if (bub_until) { bub_until = lv_tick_get(); bub_prio = 0; }
}

static void place_bubble() {
  int hx = dir.headX() * ZOOM, hy = dir.headY() * ZOOM;
  if (abs(hx - bub_hx) < 12 && abs(hy - bub_hy) < 12) return;
  bub_hx = hx; bub_hy = hy;
  lv_obj_update_layout(bubble);
  int bw = lv_obj_get_width(bubble), bh = lv_obj_get_height(bubble);
  bub_tail_right = hx > 300;
  int x = bub_tail_right ? hx + 20 - bw : hx - 20;
  if (x < 6) x = 6;
  if (x + bw > 474) x = 474 - bw;
  int y = hy - bh - 24;
  if (y < 4) y = hy + 60;
  lv_obj_set_pos(bubble, x, y);
  lv_obj_invalidate(face);   // the tail moved
}

// ---- API
void home_face_create(lv_obj_t *parent, lv_coord_t y) {
  scroller = lv_obj_get_parent(parent);  // the tileview
  canvas = (Idx *)heap_caps_calloc(W * H, 1, MALLOC_CAP_SPIRAM);
  prev_canvas = (Idx *)heap_caps_calloc(W * H, 1, MALLOC_CAP_SPIRAM);
  img = (uint16_t *)heap_caps_malloc(IMG_W * IMG_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  if (!canvas || !prev_canvas || !img) {   // no PSRAM: no scene (face stays null, every API no-ops)
    heap_caps_free(canvas); heap_caps_free(prev_canvas); heap_caps_free(img);
    canvas = prev_canvas = nullptr; img = nullptr;
    return;
  }
  uint16_t bg = rgb565(0x2b1e2a);
  for (int i = 0; i < IMG_W * IMG_H; i++) img[i] = bg;
  img_dsc.header.always_zero = 0;
  img_dsc.header.w = IMG_W;
  img_dsc.header.h = IMG_H;
  img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  img_dsc.data_size = IMG_W * IMG_H * sizeof(uint16_t);
  img_dsc.data = (const uint8_t *)img;

  dir.init(esp_random(), local_tod());

  face = lv_obj_create(parent);
  lv_obj_remove_style_all(face);
  lv_obj_set_pos(face, 0, y);
  lv_obj_set_size(face, IMG_W, IMG_H);
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
  last_ms = lv_tick_get();
}

static void push_input() {
  Input i = in;
  i.tod = local_tod();
  i.yday = in.yday;
  if (dbg_helpers >= 0) i.helpers = dbg_helpers;
  dir.setInput(i);
}

void home_face_set_mood(HomeMood m, float e, int h, char cat) {
  static const Mood MM[] = {MOOD_OFFLINE, MOOD_SLEEP, MOOD_IDLE, MOOD_BUSY, MOOD_WAITING};
  in.mood = MM[m];
  in.tool = cat;
  in.helpers = h;
  energy_tgt = e;
  in.energy = e;
  if (face) push_input();
}

void home_face_set_plan(int done, int total) {
  in.todoDone = done; in.todoTotal = total;
  if (face) push_input();
}

void home_face_react(HomeReact r) {
  if (!face) return;
  dir.react((React)r);
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
  bub_hx = -1000;
  place_bubble();
  lv_obj_clear_flag(bubble, LV_OBJ_FLAG_HIDDEN);
  bub_until = now + ms;
  bub_prio = prio;
}

bool home_face_bubble_visible() { return bub_until != 0; }
void home_face_on_tap(void (*cb)()) { tap_cb = cb; }
void home_face_resume() { last_ms = lv_tick_get(); acc_ms = 0; }

void home_face_tick(bool visible) {
  if (!face) return;
  uint32_t now = lv_tick_get();
  uint32_t dt = now - last_ms;
  last_ms = now;
  if (dt > 1000) dt = 1000;
  acc_ms += dt;
  // fixed-step director at FPS; catch up at most a few ticks after a stall
  const uint32_t STEP = 1000 / FPS;
  int steps = 0;
  while (acc_ms >= STEP && steps < 4) { dir.step(); acc_ms -= STEP; steps++; }
  if (acc_ms >= STEP) acc_ms = 0;
  // every second: refresh the time of day (tod also advances inside the director between inputs)
  static uint32_t last_tod = 0;
  if (now - last_tod > 10000) { last_tod = now; push_input(); }

  // bubble fade
  if (bub_until && (int32_t)(now - bub_until) >= 0) { bub_until = 0; bub_prio = 0; }
  float bt = bub_until ? 1.f : 0.f;
  bub_a += (bt - bub_a) * (1 - expf(-dt / 1000.f * 12));
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

  if (!visible || !steps) return;
  if (detail_is_open()) return;
  if (scroller && (lv_obj_is_scrolling(scroller) || lv_anim_get(scroller, nullptr))) return;

  if (bub_until) place_bubble();
  if (fabsf(bub_a - bub_drawn) > 0.005f) {
    lv_area_t B;
    lv_obj_get_coords(bubble, &B);
    lv_area_t tb = {(lv_coord_t)(B.x1 - 2), (lv_coord_t)(B.y2 - 6), (lv_coord_t)(B.x2 + 2), (lv_coord_t)(B.y2 + 18)};
    bub_drawn = bub_a;
    lv_obj_invalidate_area(face, &tb);
  }
  render();
}

void home_face_debug(const char *name, int frame, int nhelpers, bool grey) {
  (void)frame; (void)grey;
  if (!face) return;
  dbg_forced = name && *name;
  dbg_helpers = dbg_forced ? nhelpers : -1;
  dir.force(dbg_forced ? name : "");
  push_input();
  if (dbg_forced) { bub_until = 0; bub_prio = 0; bub_a = 0; }   // no speech bubble over a QA frame
}

void home_face_debug_mirror(int m) { dbg_mirror = m; first = true; }

void home_face_status(char *buf, size_t n) {
  snprintf(buf, n, "{\"anim\":\"%s\",\"task\":\"%s\",\"frame\":%u,\"forced\":%d,\"x\":%d,\"y\":%d}",
           face ? dir.activity() : "", face ? Director::TASK_NAMES[dir.task] : "", (unsigned)dir.tick, dbg_forced, dir.cx, dir.cy);
}
