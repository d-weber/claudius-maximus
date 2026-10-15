// Clawd — the Claude Code mascot — as the home-screen companion. See home_face.h.
//
// Clawd is drawn as crisp pixel art (plain filled rects, no anti-aliasing) on the grid of the
// Claude Code welcome banner:
//      ▐▛███▜▌        one banner quadrant = 2P wide x 4P tall, P = 7 px for the big Clawd
//     ▝▜█████▛▘       body 24P x 16P, arms 4P x 4P, legs 2P x 4P, eye cut-outs 2P x 4P
//       ▘▘ ▝▝
// In grid units (P) relative to the top-left of the 36P-wide sprite box:
//   body x 6..30, y 0..16 · eyes x 10..12 and 24..26, y 4..8 · arms x 2..6 / 30..34, y 8..12
//   legs x 8,12,22,26 (2 wide), y 16..20
//
// CPU: the simulation runs every tick, but everything visible is reduced to a small discrete
// render state (pixel offsets, poses). We only invalidate when that state changes: eye-only
// changes repaint a small face box, pose changes repaint Clawd's box, helpers/z's/bubble tail have
// their own boxes. Pixel-art motion is stepped anyway, so most ticks draw nothing.
#include "home_face.h"
#include "screens.h"
#include "../ui/theme.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define PI_F 3.14159265f

static const int P = 7;            // big Clawd pixel
static const int CX = 240;         // sprite centre x inside the face object
static const int GROUND = 236;     // feet baseline y inside the face object
static const float MINI_P = 2.f;  // helper mini-Clawds
static const int MINI_X[4] = {130, -130, 196, -196};  // offsets from CX (inner right/left, outer right/left)

#define COL_BODY   C_ACCENT
#define COL_GREY   lv_color_hex(0x77716B)
#define COL_EYE    C_BG
#define COL_BLUSH  lv_color_hex(0xF4A592)

static lv_obj_t *face, *bubble, *bubble_lbl, *scroller;
static void (*tap_cb)() = nullptr;

// ---- inputs
static HomeMood mood = HOME_MOOD_OFFLINE;
static float energy_tgt = 0, energy = 0;
static int helpers = 0;
static char tool_cat = 0;

// ---- simulation
static float t = 0, walk = 0;
static uint32_t last_ms = 0;
static uint32_t r_until[HOME_REACT_COUNT], r_start[HOME_REACT_COUNT];
static const uint16_t R_DUR[HOME_REACT_COUNT] = {900, 700, 900, 1100, 1300, 2400, 800};
static uint32_t next_blink = 1500, blink_start = 0, next_look = 0;
static int look_x = 0, look_y = 0;
static bool pressed = false;

// ---- discrete render state
enum { ARM_OUT, ARM_UP, ARM_DOWN, ARM_HIGH };              // HIGH = out, raised by 1P
enum { EYE_OPEN, EYE_CLOSED, EYE_HAPPY, EYE_WINCE, EYE_WIDE };
struct RS {
  int8_t lift, shake;
  uint8_t grey, sit, legs, arm_l, arm_r, squash;
  uint8_t eyes, wink, blink, blush, mouth;  // mouth: 0 none, 1 "o", 2 laugh
  int8_t edx, edy;
  int8_t bang, spark, z;                    // -1 = none, else animation step
  uint8_t nh, mini_phase;                   // helpers shown + their leg phases (bits)
};
static RS cur_rs, drawn_rs;   // draw_cb always renders drawn_rs
static float bub_a = 0, bub_drawn = 0;
static uint32_t bub_until = 0, bub_since = 0;
static int bub_prio = 0, bub_opa_set = -1;
static bool first = true;

static inline bool active(int r, uint32_t now) { return (int32_t)(r_until[r] - now) > 0; }
static inline float progress(int r, uint32_t now) {
  float p = (float)(now - r_start[r]) / (float)R_DUR[r];
  return p < 0 ? 0 : (p > 1 ? 1 : p);
}

// ------------------------------------------------------------------ drawing
static lv_draw_ctx_t *dc;
static float ox, oy, gp;  // sprite origin (top-left of the 36P box, top of body) + pixel size
static lv_color_t ink;

static void px(float gx, float gy, float w, float h, lv_color_t c, lv_opa_t opa = LV_OPA_COVER) {
  lv_area_t a = {(lv_coord_t)lroundf(ox + gx * gp), (lv_coord_t)lroundf(oy + gy * gp),
                 (lv_coord_t)(lroundf(ox + (gx + w) * gp) - 1), (lv_coord_t)(lroundf(oy + (gy + h) * gp) - 1)};
  if (a.x2 < a.x1 || a.y2 < a.y1 || !_lv_area_is_on(&a, dc->clip_area)) return;
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = c;
  d.bg_opa = opa;
  lv_draw_rect(dc, &d, &a);
}

static void arm(bool left, uint8_t pose) {
  float x = left ? 2 : 30;
  switch (pose) {
    case ARM_UP:  // "\o/": stepped diagonal up and out
      px(left ? 3 : 30, 6, 3, 3, ink);
      px(left ? 1 : 32, 3, 3, 3, ink);
      break;
    case ARM_DOWN: px(left ? 4 : 30, 11, 2, 4, ink); break;   // hanging
    case ARM_HIGH: px(x, 7, 4, 4, ink); break;
    default: px(x, 8, 4, 4, ink); break;
  }
}

static void eye(bool left, const RS &s) {
  float ex = (left ? 10 : 24) + s.edx, ey = 4 + s.edy;
  uint8_t shape = (!left && s.wink) ? EYE_HAPPY : s.eyes;
  switch (shape) {
    case EYE_CLOSED: px(ex - 1, 6, 4, 1, COL_EYE); break;
    case EYE_HAPPY:  // ^
      px(ex - 1, ey + 2, 1, 1, COL_EYE); px(ex, ey + 1, 2, 1, COL_EYE); px(ex + 2, ey + 2, 1, 1, COL_EYE);
      break;
    case EYE_WINCE: {  // > <
      float d = left ? 1 : -1, x0 = left ? ex - 1 : ex + 2;
      px(x0, 3, 1, 1, COL_EYE); px(x0 + d, 4, 1, 1, COL_EYE); px(x0 + 2 * d, 5, 1, 1, COL_EYE);
      px(x0 + d, 6, 1, 1, COL_EYE); px(x0, 7, 1, 1, COL_EYE);
      break;
    }
    case EYE_WIDE: px(ex, ey - 1, 2, 5, COL_EYE); break;
    default:
      if (s.blink) px(ex, ey + 2, 2, 1, COL_EYE);
      else px(ex, ey, 2, 4, COL_EYE);
  }
}

// Full Clawd at (cx, ground) with pixel size p.
static void clawd(float cx, float ground, float p, lv_color_t body, const RS &s, bool mini) {
  gp = p;
  int body_drop = s.sit ? 3 : 0;
  ox = cx - 18 * p + s.shake;
  oy = ground - 20 * p - s.lift + body_drop * p;
  ink = body;
  // legs (from body bottom to the ground; lifted legs are shorter)
  static const float LX[4] = {8, 12, 22, 26};
  for (int i = 0; i < 4; i++) {
    float h = s.sit ? 1 : ((s.legs >> i) & 1 ? 2 : 4);
    if (!s.sit && s.lift >= p * 0.99f && !mini) h = 4;  // airborne: legs dangle
    px(LX[i], 16, 2, h, ink);
  }
  // body (squash: 1P shorter + 1P wider each side)
  if (s.squash) px(5, 1, 26, 15, ink);
  else px(6, 0, 24, 16, ink);
  arm(true, s.arm_l);
  arm(false, s.arm_r);
  if (mini) {  // simple eyes looking toward big Clawd
    px(10 + s.edx, 4, 2, 4, COL_EYE);
    px(24 + s.edx, 4, 2, 4, COL_EYE);
    return;
  }
  eye(true, s);
  eye(false, s);
  if (s.blush) { px(7, 9, 3, 1, COL_BLUSH); px(26, 9, 3, 1, COL_BLUSH); }
  if (s.mouth == 1) px(17, 10, 2, 2, COL_EYE);
  else if (s.mouth == 2) { px(16, 10, 4, 1, COL_EYE); px(17, 11, 2, 1, COL_EYE); }
}

// pixel "z" (5x5 cells of size b)
static void zee(float x, float y, float b, lv_opa_t o) {
  float sox = ox, soy = oy, sgp = gp;
  ox = x; oy = y; gp = b;
  px(0, 0, 5, 1, C_MUTED, o); px(3, 1, 1, 1, C_MUTED, o); px(2, 2, 1, 1, C_MUTED, o); px(1, 3, 1, 1, C_MUTED, o);
  px(0, 4, 5, 1, C_MUTED, o);
  ox = sox; oy = soy; gp = sgp;
}

static void sparkle(float x, float y, int size, lv_color_t c) {
  float sox = ox, soy = oy, sgp = gp;
  ox = x; oy = y; gp = P;
  px(-0.5f, -0.5f, 1, 1, c);
  if (size > 1) { px(-1.5f, -0.5f, 3, 1, c); px(-0.5f, -1.5f, 1, 3, c); }
  ox = sox; oy = soy; gp = sgp;
}

static void draw_cb(lv_event_t *e) {
  dc = lv_event_get_draw_ctx(e);
  lv_area_t A;
  lv_obj_get_coords(face, &A);
  const RS &s = drawn_rs;
  const float cx = A.x1 + CX, ground = A.y1 + GROUND;

  // bubble tail (under the bubble, which is our child and drawn after us)
  if (bub_drawn > 0.02f && !lv_obj_has_flag(bubble, LV_OBJ_FLAG_HIDDEN)) {
    lv_area_t B;
    lv_obj_get_coords(bubble, &B);
    lv_point_t pts[3] = {{(lv_coord_t)(B.x1 + 26), (lv_coord_t)(B.y2 - 4)},
                         {(lv_coord_t)(B.x1 + 50), (lv_coord_t)(B.y2 - 4)},
                         {(lv_coord_t)(B.x1 + 8), (lv_coord_t)(B.y2 + 16)}};
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = C_SURFACE2;
    d.bg_opa = (lv_opa_t)(bub_drawn * 255);
    lv_draw_polygon(dc, &d, pts, 3);
  }

  // helpers: mini-Clawds standing beside the big one, shuffling their feet
  for (int k = 0; k < s.nh && k < 4; k++) {
    RS m = {};
    bool ph = (s.mini_phase >> k) & 1;
    m.legs = ph ? 0x5 : 0xA;
    m.lift = ph ? 2 : 0;
    m.edx = MINI_X[k] > 0 ? -1 : 1;
    clawd(cx + MINI_X[k], ground, MINI_P, C_SUBAGENT, m, true);
  }
  if (s.nh > 4) {
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = F_SMALL;
    d.color = C_SUBAGENT;
    char b[8];
    snprintf(b, sizeof b, "+%d", s.nh - 4);
    lv_area_t a = {(lv_coord_t)(cx + MINI_X[2] - 20), (lv_coord_t)(ground - 58), (lv_coord_t)(cx + MINI_X[2] + 20),
                   (lv_coord_t)(ground - 40)};
    d.align = LV_TEXT_ALIGN_CENTER;
    lv_draw_label(dc, &d, &a, b, nullptr);
  }

  // Clawd
  clawd(cx, ground, P, s.grey ? COL_GREY : COL_BODY, s, false);
  // (ox, oy, gp) now describe the big sprite

  // waiting "!" up-left of the head
  if (s.bang >= 0) {
    float dy = s.bang ? -1 : 0;
    px(0, -10 + dy, 2, 5, C_WAITING);
    px(0, -4 + dy, 2, 2, C_WAITING);
  }

  // finish sparkles (pixel crosses popping around the head)
  if (s.spark >= 0) {
    static const float SX[6] = {-3, 39, 1, 35, -6, 42}, SY[6] = {-6, -4, -12, -11, 4, 6};
    static const lv_color_t SC[3] = {C_WAITING, C_TEXT, C_ACCENT2};
    for (int k = 0; k < 6; k++) {
      int st = s.spark - k;  // each sparkle lives 4 steps, staggered
      if (st < 0 || st > 3) continue;
      sparkle(ox + SX[k] * P, oy + SY[k] * P, (st == 1 || st == 2) ? 2 : 1, SC[k % 3]);
    }
  }

  // z's drifting up-right from the head
  if (s.z >= 0) {
    for (int k = 0; k < 3; k++) {
      int st = (s.z + k * 6) % 18;  // 0..17
      float b = 2 + st / 6;         // 2,3,4 px cells
      lv_opa_t o = st < 2 ? 120 : (st > 15 ? 110 : 220);
      zee(ox + 31 * P + st * 3, oy - 2 * P - st * 4, b, o);
    }
  }
}

// ------------------------------------------------------------------ boxes
static lv_area_t sprite_box(const RS &s, bool extras) {
  lv_area_t A;
  lv_obj_get_coords(face, &A);
  int top = GROUND - 20 * P - s.lift + (s.sit ? 3 * P : 0) - (s.arm_l == ARM_UP || s.arm_r == ARM_UP ? 0 : 0);
  if (extras && (s.bang >= 0 || s.spark >= 0)) top -= 14 * P;
  int l = CX - 18 * P - 8 * P, r = CX + 18 * P + 8 * P;  // sparkles reach out ~6P
  if (!(extras && s.spark >= 0)) { l = CX - 17 * P - 7; r = CX + 17 * P + 7; }
  if (extras && s.bang >= 0 && l > CX - 18 * P - 2) l = CX - 18 * P - 2;
  return {(lv_coord_t)(A.x1 + l + s.shake), (lv_coord_t)(A.y1 + top - 2), (lv_coord_t)(A.x1 + r + s.shake),
          (lv_coord_t)(A.y1 + GROUND + 1)};
}

static lv_area_t face_box(const RS &s) {
  lv_area_t A;
  lv_obj_get_coords(face, &A);
  int x0 = A.x1 + CX - 18 * P + s.shake, y0 = A.y1 + GROUND - 20 * P - s.lift + (s.sit ? 3 * P : 0);
  return {(lv_coord_t)(x0 + 6 * P), (lv_coord_t)(y0 + 2 * P), (lv_coord_t)(x0 + 30 * P - 1), (lv_coord_t)(y0 + 13 * P)};
}

static lv_area_t z_box(const RS &s) {
  lv_area_t A;
  lv_obj_get_coords(face, &A);
  int x0 = A.x1 + CX - 18 * P + 31 * P, y0 = A.y1 + GROUND - 20 * P - s.lift + (s.sit ? 3 * P : 0);
  return {(lv_coord_t)x0, (lv_coord_t)(y0 - 2 * P - 17 * 4 - 2), (lv_coord_t)(x0 + 17 * 3 + 22),
          (lv_coord_t)(y0 - 2 * P + 22)};
}

static void minis_inval() {
  lv_area_t A;
  lv_obj_get_coords(face, &A);
  lv_area_t l = {(lv_coord_t)(A.x1 + CX - 196 - 36), (lv_coord_t)(A.y1 + GROUND - 60), (lv_coord_t)(A.x1 + CX - 130 + 36),
                 (lv_coord_t)(A.y1 + GROUND + 1)};
  lv_area_t r = {(lv_coord_t)(A.x1 + CX + 130 - 36), (lv_coord_t)(A.y1 + GROUND - 60), (lv_coord_t)(A.x1 + CX + 196 + 36),
                 (lv_coord_t)(A.y1 + GROUND + 1)};
  lv_obj_invalidate_area(face, &l);
  lv_obj_invalidate_area(face, &r);
}

// ------------------------------------------------------------------ input
static bool hit(lv_point_t p) {
  lv_area_t b = sprite_box(drawn_rs, false);
  return p.x >= b.x1 - 16 && p.x <= b.x2 + 16 && p.y >= b.y1 - 24 && p.y <= b.y2 + 10;
}

static void face_ev(lv_event_t *e) {
  lv_event_code_t c = lv_event_get_code(e);
  if (c != LV_EVENT_PRESSED && c != LV_EVENT_RELEASED && c != LV_EVENT_PRESS_LOST && c != LV_EVENT_CLICKED) return;
  lv_point_t p = {0, 0};
  lv_indev_t *in = lv_indev_get_act();
  if (in) lv_indev_get_point(in, &p);
  if (c == LV_EVENT_PRESSED) pressed = hit(p);
  else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) pressed = false;
  else if (c == LV_EVENT_CLICKED && hit(p) && tap_cb) tap_cb();
}

static void bubble_ev(lv_event_t *) {
  if (bub_until) { bub_until = lv_tick_get(); bub_prio = 0; }
}

// ------------------------------------------------------------------ API
void home_face_create(lv_obj_t *parent, lv_coord_t y, lv_coord_t h) {
  scroller = lv_obj_get_parent(parent);  // the tileview
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

  memset(&cur_rs, 0, sizeof cur_rs);
  cur_rs.grey = 1; cur_rs.sit = 1; cur_rs.eyes = EYE_CLOSED; cur_rs.arm_l = cur_rs.arm_r = ARM_DOWN;
  cur_rs.bang = cur_rs.spark = -1; cur_rs.z = 0;
  drawn_rs = cur_rs;
}

void home_face_set_mood(HomeMood m, float e, int h, char tc) {
  if (m != mood && m >= HOME_MOOD_IDLE && mood <= HOME_MOOD_SLEEP && !first) home_face_react(HOME_REACT_SURPRISE);
  mood = m;
  energy_tgt = e < 0 ? 0 : (e > 1 ? 1 : e);
  helpers = h < 0 ? 0 : h;
  tool_cat = tc;
}

void home_face_react(HomeReact r) {
  uint32_t now = lv_tick_get();
  if (!active(r, now)) r_start[r] = now;
  r_until[r] = now + R_DUR[r];
  if (r == HOME_REACT_GIGGLE || r == HOME_REACT_WINK || r == HOME_REACT_HAPPY) r_until[HOME_REACT_WINCE] = now;
}

void home_face_say(const char *txt, uint32_t ms, int prio) {
  uint32_t now = lv_tick_get();
  if (bub_until && (int32_t)(bub_until - now) > 0 && prio < bub_prio && now - bub_since < 2500) return;
  lv_point_t sz;
  lv_txt_get_size(&sz, txt, F_BODY, 0, 3, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  lv_coord_t w = sz.x > 226 ? 226 : sz.x + 1;
  lv_obj_set_width(bubble_lbl, w);
  lv_label_set_text(bubble_lbl, txt);
  lv_obj_align(bubble, LV_ALIGN_TOP_RIGHT, -16, 0);
  lv_obj_clear_flag(bubble, LV_OBJ_FLAG_HIDDEN);
  bub_until = now + ms;
  bub_since = now;
  bub_prio = prio;
}

bool home_face_bubble_visible() { return bub_until != 0; }
void home_face_on_tap(void (*cb)()) { tap_cb = cb; }
void home_face_resume() {
  last_ms = lv_tick_get();
  lv_obj_invalidate(face);
}

// ------------------------------------------------------------------ simulation -> render state
static RS compute(uint32_t now) {
  RS s;
  memset(&s, 0, sizeof s);
  s.bang = s.spark = s.z = -1;
  s.arm_l = s.arm_r = ARM_OUT;
  s.legs = 0;

  if ((int32_t)(now - next_look) >= 0) {  // gaze wandering
    next_look = now + 1600 + rand() % 3000;
    look_x = (rand() % 3) - 1;
    look_y = (rand() % 3) - 1;
    if (rand() % 3 == 0) look_x = look_y = 0;  // look at the human now and then
  }
  if ((int32_t)(now - next_blink) >= 0) {
    blink_start = now;
    next_blink = now + 2400 + rand() % 4000;
    if (rand() % 5 == 0) next_blink = now + 300;  // double blink
  }
  bool blink = now - blink_start < 140;
  int step2 = (int)(walk) & 1;  // walking phase

  switch (mood) {
    case HOME_MOOD_OFFLINE:
    case HOME_MOOD_SLEEP:
      s.grey = mood == HOME_MOOD_OFFLINE;
      s.sit = 1;
      s.eyes = EYE_CLOSED;
      s.squash = fmodf(t, 4.4f) < 2.2f;  // slow breathing
      s.z = (int)(t * 4) % 18;
      break;
    case HOME_MOOD_IDLE:
      s.edx = look_x; s.edy = look_y; s.blink = blink;
      s.lift = fmodf(t, 2.4f) < 1.2f ? 0 : 2;  // gentle idle bob
      s.blush = 1;
      break;
    case HOME_MOOD_BUSY:
      s.blink = blink;
      s.legs = step2 ? 0x5 : 0xA;  // marching in place
      s.lift = step2 ? P / 2 : 0;
      s.arm_l = step2 ? ARM_HIGH : ARM_OUT;
      s.arm_r = step2 ? ARM_OUT : ARM_HIGH;
      switch (tool_cat) {
        case 'r': { int ph = (int)(t * 1.6f) % 4; s.edx = ph == 0 ? -1 : (ph == 2 ? 1 : 0); s.edy = 0; break; }  // scanning
        case 'e': s.edx = (int)(t * 3) & 1 ? 1 : 0; s.edy = 1; break;  // typing, eyes down
        case 'x': s.edx = 1; s.edy = 0; break;                          // watching the terminal
        case 'w': s.edx = 1; s.edy = -1; break;
        case 't': s.edx = -1; s.edy = -1; break;                        // pondering
        default: s.edx = look_x; s.edy = look_y;
      }
      break;
    case HOME_MOOD_WAITING: {
      s.eyes = EYE_WIDE; s.mouth = 1;
      s.bang = (int)(t * 3) & 1;
      s.arm_r = (int)(t * 3) & 1 ? ARM_UP : ARM_HIGH;  // waving at you
      float ph = fmodf(t, 2.6f);
      s.lift = (ph < 0.25f || (ph > 0.4f && ph < 0.65f)) ? 2 * P : 0;  // "hey! hey!"
      break;
    }
  }

  // transients, increasing priority
  if (active(HOME_REACT_GLANCE, now)) { s.edx = 0; s.edy = 1; }
  if (active(HOME_REACT_HOP, now)) {
    float p = progress(HOME_REACT_HOP, now);
    if (p > 0.15f && p < 0.75f) s.lift = 2 * P;
    s.arm_l = s.arm_r = ARM_HIGH;
  }
  if (active(HOME_REACT_SURPRISE, now)) {
    float p = progress(HOME_REACT_SURPRISE, now);
    s.sit = 0; s.grey = s.grey && mood == HOME_MOOD_OFFLINE;
    s.eyes = EYE_WIDE; s.mouth = 1; s.edx = s.edy = 0; s.z = -1;
    s.lift = p < 0.3f ? P : 0;
    s.arm_l = s.arm_r = ARM_UP;
  }
  if (active(HOME_REACT_WINK, now)) {
    s.eyes = EYE_OPEN; s.wink = 1; s.blink = 0; s.blush = 1; s.edx = s.edy = 0; s.mouth = 0;
    s.arm_r = ARM_UP;
  }
  if (active(HOME_REACT_GIGGLE, now)) {
    int f = (int)(t * 8) & 1;
    s.eyes = EYE_HAPPY; s.mouth = 2; s.blush = 1; s.edx = s.edy = 0;
    s.lift = f ? P : 0;
    s.arm_l = f ? ARM_UP : ARM_OUT;
    s.arm_r = f ? ARM_OUT : ARM_UP;
  }
  if (active(HOME_REACT_HAPPY, now)) {
    float p = progress(HOME_REACT_HAPPY, now);
    s.eyes = EYE_HAPPY; s.mouth = p < 0.7f ? 2 : 0; s.blush = 1; s.edx = s.edy = 0; s.bang = -1;
    s.arm_l = s.arm_r = ARM_UP;
    float hop = fabsf(sinf(p * PI_F * 3)) * (1 - p);
    s.lift = hop > 0.45f ? 3 * P : (hop > 0.15f ? 2 * P : 0);
    s.squash = hop < 0.08f && p < 0.9f;
    s.spark = (int8_t)(p * 12);
  }
  if (active(HOME_REACT_WINCE, now)) {
    s.eyes = EYE_WINCE; s.mouth = 0; s.blush = 0; s.edx = s.edy = 0; s.blink = 0; s.wink = 0;
    s.arm_l = s.arm_r = ARM_DOWN;
    s.shake = ((int)(t * 16) & 1) ? P : -P;
    s.lift = 0;
  }
  if (pressed) { s.squash = 1; s.lift = 0; }

  // helpers
  s.nh = helpers > 99 ? 99 : helpers;
  for (int k = 0; k < 4 && k < s.nh; k++)
    if ((int)(t * 2.5f + k * 0.5f) & 1) s.mini_phase |= 1 << k;
  return s;
}

static bool only_face_changed(const RS &a, const RS &b) {
  RS x = a, y = b;
  x.eyes = y.eyes; x.wink = y.wink; x.blink = y.blink; x.blush = y.blush; x.mouth = y.mouth;
  x.edx = y.edx; x.edy = y.edy;
  x.nh = y.nh; x.mini_phase = y.mini_phase; x.z = y.z;
  return memcmp(&x, &y, sizeof x) == 0;
}

void home_face_tick() {
  if (!face) return;
  uint32_t now = lv_tick_get();
  float dt = (now - last_ms) / 1000.f;
  if (dt <= 0 || dt > 0.15f) dt = 0.04f;
  last_ms = now;
  t += dt;
  energy += (energy_tgt - energy) * (1 - expf(-dt * 1.5f));
  walk += dt * (2.5f + 4.0f * energy);  // steps per second

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
  } else if (o > 0 && abs(o - bub_opa_set) > 4) {
    lv_obj_set_style_opa(bubble, o >= 250 ? LV_OPA_COVER : (lv_opa_t)o, 0);
    bub_opa_set = o;
  }

  cur_rs = compute(now);

  // publish + invalidate (skip while the tile is moving or covered: the snapshot stays consistent)
  if (detail_is_open()) return;
  if (scroller && (lv_obj_is_scrolling(scroller) || lv_anim_get(scroller, nullptr))) return;

  if (fabsf(bub_a - bub_drawn) > 0.005f) {
    lv_area_t B;
    lv_obj_get_coords(bubble, &B);
    lv_area_t tb = {B.x1, (lv_coord_t)(B.y2 - 6), (lv_coord_t)(B.x1 + 54), (lv_coord_t)(B.y2 + 18)};
    bub_drawn = bub_a;
    lv_obj_invalidate_area(face, &tb);
  }
  if (first || memcmp(&cur_rs, &drawn_rs, sizeof(RS)) != 0) {
    if (first) {
      lv_obj_invalidate(face);
    } else if (only_face_changed(cur_rs, drawn_rs)) {
      if (memcmp(&cur_rs, &drawn_rs, sizeof(RS))) {
        RS tmp = drawn_rs;  // anything face-ish changed?
        tmp.nh = cur_rs.nh; tmp.mini_phase = cur_rs.mini_phase; tmp.z = cur_rs.z;
        if (memcmp(&tmp, &cur_rs, sizeof(RS))) { lv_area_t fb = face_box(cur_rs); lv_obj_invalidate_area(face, &fb); }
      }
    } else {
      lv_area_t a = sprite_box(drawn_rs, true), b = sprite_box(cur_rs, true);
      lv_obj_invalidate_area(face, &a);
      lv_obj_invalidate_area(face, &b);
    }
    if (cur_rs.nh != drawn_rs.nh || cur_rs.mini_phase != drawn_rs.mini_phase) minis_inval();
    if (cur_rs.z != drawn_rs.z || cur_rs.lift != drawn_rs.lift || cur_rs.sit != drawn_rs.sit) {
      lv_area_t a = z_box(drawn_rs), b = z_box(cur_rs);
      lv_obj_invalidate_area(face, &a);
      lv_obj_invalidate_area(face, &b);
    }
    drawn_rs = cur_rs;
    first = false;
  }
}
