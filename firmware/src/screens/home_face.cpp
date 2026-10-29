// Clawd's Studio on the home screen: LVGL glue around screens/room/ (the director and the pixel engine).
//
// Rendering: the director draws every tick (12 fps) into a 120 x 106 palette-index canvas (internal RAM). The
// face object's draw callback upscales the canvas 4x through the current palette straight into LVGL's draw buffer
// for whatever area LVGL is refreshing, so the room takes the same paced band-by-band path to the PSRAM
// framebuffer as every other screen (no intermediate 480 x 424 image, no PSRAM write bursts that would starve the
// panel's scan-out). After each tick only the row bands that changed are invalidated; a palette change (the light
// fading at dusk, the alarm) invalidates the whole room. Settings.home_mirror flips the scene horizontally.
#include "home_face.h"
#include "screens.h"
#include "room/director.h"
#include "../ui/theme.h"
#include "../core/settings.h"
#include "../core/model.h"
#include <esp_heap_caps.h>
#include "../core/hal.h"
#include "../core/perf.h"
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
static bool first = true, mirrored = false;
static uint32_t n_full = 0, n_band = 0, n_face_inv = 0;   // publish statistics (perf report)
// The director's state lives in PSRAM, on purpose. In internal RAM (.bss) it sat in a region the LCD scan-out DMA
// also depends on, and the CPU hammering it every tick starved the DMA: 10-40 missed frames per minute
// (perf lcd_underruns, a sideways jump on the panel), zero with the state in PSRAM. Measured 2026-10-28, see
// docs/STATUS.md "Scan-out stalls". Keep any other hot per-frame state out of .bss too.
// PROBE: the Director can live in PSRAM (e340ec9) or internal RAM; dev_probe {"dirloc":1} switches at runtime
static Director *dir_ptr = (Director *)heap_caps_calloc(1, sizeof(Director), MALLOC_CAP_SPIRAM);
static Director dir_internal;
#define dir (*dir_ptr)
void home_face_probe_dirloc(bool internal) {
  Director *to = internal ? &dir_internal : (Director *)heap_caps_calloc(1, sizeof(Director), MALLOC_CAP_SPIRAM);
  if (to == dir_ptr) return;
  memcpy(to, dir_ptr, sizeof(Director));
  dir_ptr = to;
}
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

// ---- invalidate the device area of a logical rect (inclusive); the draw callback repaints it from the canvas
static void publish(int lx0, int ly0, int lx1, int ly1) {
  if (lx0 < 0) lx0 = 0;
  if (lx1 > W - 1) lx1 = W - 1;
  if (lx0 > lx1 || ly0 > ly1) return;
  lv_area_t A;
  lv_obj_get_coords(face, &A);
  int sx0 = mirrored ? (W - 1 - lx1) * ZOOM : lx0 * ZOOM;
  int sx1 = mirrored ? (W - 1 - lx0) * ZOOM + ZOOM - 1 : lx1 * ZOOM + ZOOM - 1;
  lv_area_t d = {(lv_coord_t)(A.x1 + sx0), (lv_coord_t)(A.y1 + ly0 * ZOOM), (lv_coord_t)(A.x1 + sx1), (lv_coord_t)(A.y1 + ly1 * ZOOM + ZOOM - 1)};
  lv_obj_invalidate_area(face, &d);
}

static void render() {
  Canvas c = {canvas};
  uint32_t t0 = perf_now_us();
  dir.draw(c);
#if ROOM_SPRITES
  perf_room_draw(perf_now_us() - t0, dir.sprFallbacks, dir.sprFallbackFrames);
#else
  perf_room_draw(perf_now_us() - t0, 0, 0);
#endif
  bool pdirty = false;
  dir.palette(&pdirty);
  bool mir = want_mirror();
  if (first || pdirty || mir != mirrored) {
    n_full++;
    mirrored = mir;
    memcpy(prev_canvas, canvas, W * H);
    publish(0, 0, W - 1, H - 1);
    first = false;
    return;
  }
  // Dirty rects per band of 8 logical rows (32 device px): two small changes far apart (a star twinkling in the
  // window, Clawd walking by the door) stay two small publishes instead of one frame-sized bounding box.
  const int BAND = 8;
  bool any = false;
  for (int b0 = 0; b0 < H; b0 += BAND) {
    int b1 = b0 + BAND > H ? H : b0 + BAND;
    int x0 = W, x1 = -1;
    for (int y = b0; y < b1; y++) {
      const Idx *a0 = canvas + y * W, *p0 = prev_canvas + y * W;
      if (!memcmp(a0, p0, W)) continue;
      int lx = 0, rx = W - 1;
      while (lx < W && a0[lx] == p0[lx]) lx++;
      while (rx > lx && a0[rx] == p0[rx]) rx--;
      if (lx < x0) x0 = lx;
      if (rx > x1) x1 = rx;
    }
    if (x1 < 0) continue;
    any = true;
    n_band++;
    publish(x0, b0, x1, b1 - 1);
  }
  if (any) memcpy(prev_canvas, canvas, W * H);
}

static void draw_cb(lv_event_t *e) {
  lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
  lv_area_t A;
  lv_obj_get_coords(face, &A);
  lv_area_t ia = {A.x1, A.y1, (lv_coord_t)(A.x1 + IMG_W - 1), (lv_coord_t)(A.y1 + IMG_H - 1)};
  lv_area_t clip;
  if (canvas && _lv_area_intersect(&clip, &ia, dc->clip_area)) {
    const uint16_t *pal = dir.palette(nullptr);
    lv_color_t *buf = (lv_color_t *)dc->buf;
    int stride = lv_area_get_width(dc->buf_area);
    for (int y = clip.y1; y <= clip.y2; y++) {
      const Idx *src = canvas + ((y - A.y1) / ZOOM) * W;
      lv_color_t *dst = buf + (y - dc->buf_area->y1) * stride + (clip.x1 - dc->buf_area->x1);
      int x = clip.x1;
      while (x <= clip.x2) {
        int ox = x - A.x1;
        int lx = ox / ZOOM;
        int run = ZOOM - ox % ZOOM;                 // device pixels left in this logical pixel
        if (x + run - 1 > clip.x2) run = clip.x2 - x + 1;
        lv_color_t v;
        v.full = pal[src[mirrored ? W - 1 - lx : lx]];
        for (int k = 0; k < run; k++) dst[k] = v;
        dst += run; x += run;
      }
    }
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
  // the tail moved: invalidate the old and the new tail strips only (not the whole room)
  lv_area_t B;
  lv_obj_get_coords(bubble, &B);
  lv_area_t old_tail = {(lv_coord_t)(B.x1 - 2), (lv_coord_t)(B.y2 - 6), (lv_coord_t)(B.x2 + 2), (lv_coord_t)(B.y2 + 18)};
  lv_obj_invalidate_area(face, &old_tail);
  lv_obj_set_pos(bubble, x, y);
  lv_obj_get_coords(bubble, &B);
  lv_area_t new_tail = {(lv_coord_t)(B.x1 - 2), (lv_coord_t)(B.y2 - 6), (lv_coord_t)(B.x2 + 2), (lv_coord_t)(B.y2 + 18)};
  lv_obj_invalidate_area(face, &new_tail);
  n_face_inv++;
}

// ---- API
void home_face_create(lv_obj_t *parent, lv_coord_t y) {
  scroller = lv_obj_get_parent(parent);  // the tileview
  // Internal RAM: the director writes the canvas pixel by pixel every tick and the draw callback reads it per
  // device pixel; CPU traffic to PSRAM would compete with the panel's scan-out DMA (lcd underruns, a flicker).
  canvas = (Idx *)heap_caps_calloc(W * H, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  prev_canvas = (Idx *)heap_caps_calloc(W * H, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!canvas || !prev_canvas) {   // no RAM: no scene (face stays null, every API no-ops)
    heap_caps_free(canvas); heap_caps_free(prev_canvas);
    canvas = prev_canvas = nullptr;
    return;
  }

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
  if (hal_probe_mode() & 512) return;   // PROBE: animation paused
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
  bub_a += (bt - bub_a) * (1 - expf(-(float)dt / 1000.f * 12));
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
  snprintf(buf, n, "{\"anim\":\"%s\",\"task\":\"%s\",\"frame\":%u,\"forced\":%d,\"x\":%d,\"y\":%d,\"full\":%u,\"bands\":%u,\"face_inv\":%u}",
           face ? dir.activity() : "", face ? Director::TASK_NAMES[dir.task] : "", (unsigned)dir.tick, dbg_forced, dir.cx, dir.cy,
           (unsigned)n_full, (unsigned)n_band, (unsigned)n_face_inv);
}
