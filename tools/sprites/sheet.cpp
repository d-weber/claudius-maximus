// Renders every sprite of Clawd's Studio from the firmware's own draw calls (firmware/src/screens/room/room.cpp).
// Built and driven by tools/sprites/make_sheets.py. Writes to <out>:
//   cells.jsonl  one line per sprite: sheet, group, frame, tags, crop size, anchor, byte offset into cells.bin
//   cells.bin    the cropped palette indices of every sprite, row-major, concatenated
//   palette.bin  BASE as r,g,b triplets (N_COL entries)
// Each cell is drawn on a transparent canvas (clear(T)) and cropped to its opaque bounding box. Overlay groups
// (desk screen / steam / drawer..., rack progress) are the pixels a variant changes over a documented base draw;
// their independence is checked here by recomposing combinations and comparing with the direct draw.
#include "room.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

using namespace room;

static Idx buf[W * H];
static Canvas cv{buf};
static FILE *fj, *fb;
static long boff;
static const char *sheet = "";
static int nCells;

#define COLN(c) c, #c   // a colour index and its name (for tags)

static const char *const VIEW_N[] = {"front", "back", "left", "right"};
static const char *const ARMS_N[] = {"down", "up", "up_l", "up_r", "type_a", "type_b", "hold", "wave_a", "wave_b", "none"};
static const char *const EYES_N[] = {"open", "blink", "wide", "happy", "look_l", "look_r", "wince", "wink", "sleep", "up"};
static const char *const MOUTH_N[] = {"none", "o", "smile", "flat"};
static const char *const TINT_N[] = {"coral", "grey", "violet"};
static const char *const SCREEN_N[] = {"off", "dim", "code", "web", "saver", "plug"};
static const char *const ICON_N[] = {"code", "play", "star", "look", "check", "cross", "heart", "note", "question", "bang",
                                     "leaf", "book", "plug", "web", "pen", "coffee"};
static_assert(sizeof ICON_N / sizeof *ICON_N == ICO_COUNT, "icon names out of sync with room::Icon");

// Tags as the body of a JSON object.
struct Tags {
  std::string s;
  Tags &str(const char *k, const char *v) { sep(); s += "\""; s += k; s += "\": \""; s += v; s += "\""; return *this; }
  Tags &num(const char *k, int v) { sep(); s += "\""; s += k; s += "\": " + std::to_string(v); return *this; }
  Tags &flag(const char *k, bool v) { sep(); s += "\""; s += k; s += v ? "\": true" : "\": false"; return *this; }
  void sep() { if (!s.empty()) s += ", "; }
};

static void die(const char *what, const char *group, int frame) {
  fprintf(stderr, "sheet: %s: %s frame %d\n", what, group, frame);
  exit(1);
}

// Crop px to its opaque box and write it. (ox, oy) is the position passed to the draw call; (0, 0) for things
// drawn at fixed canvas coordinates, which makes the anchor minus the crop's canvas position.
static void emit(const char *group, int frame, const Tags &tg, const Idx *px, int ox, int oy) {
  int x0 = W, y0 = H, x1 = -1, y1 = -1;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      if (px[y * W + x] != T) { x0 = x < x0 ? x : x0; x1 = x > x1 ? x : x1; y0 = y < y0 ? y : y0; y1 = y > y1 ? y : y1; }
  if (x1 < 0) die("empty sprite", group, frame);
  // a positioned sprite touching the border may have been clipped by Canvas::p
  if ((ox || oy) && (x0 == 0 || y0 == 0 || x1 == W - 1 || y1 == H - 1)) die("sprite clipped by the canvas", group, frame);
  int w = x1 - x0 + 1, h = y1 - y0 + 1;
  for (int y = y0; y <= y1; y++) fwrite(px + y * W + x0, 1, w, fb);
  fprintf(fj, "{\"sheet\": \"%s\", \"group\": \"%s\", \"frame\": %d, \"w\": %d, \"h\": %d, \"ax\": %d, \"ay\": %d, "
              "\"cx\": %d, \"cy\": %d, \"off\": %ld, \"tags\": {%s}}\n",
          sheet, group, frame, w, h, ox - x0, oy - y0, x0, y0, boff, tg.s.c_str());
  boff += (long)w * h;
  nCells++;
}

// One sprite: whatever draw() puts on a transparent canvas.
template <class F> static void cap(const char *group, int frame, const Tags &tg, int ox, int oy, F draw) {
  cv.clear(T);
  draw(cv);
  emit(group, frame, tg, buf, ox, oy);
}

// The pixels draw() changes over base(): blitting it on top of the base draw reproduces draw() exactly.
template <class B, class F> static void diff(Idx *out, B base, F draw) {
  static Idx b[W * H];
  Canvas bc{b};
  bc.clear(T); base(bc);
  cv.clear(T); draw(cv);
  for (int i = 0; i < W * H; i++) {
    if (buf[i] == T && b[i] != T) { fprintf(stderr, "sheet: variant clears a base pixel\n"); exit(1); }
    out[i] = buf[i] != b[i] ? buf[i] : T;
  }
}
template <class B, class F> static void over(const char *group, int frame, const Tags &tg, B base, F draw) {
  static Idx o[W * H];
  diff(o, base, draw);
  emit(group, frame, tg, o, 0, 0);
}
static void blitOver(Idx *dst, const Idx *ov) { for (int i = 0; i < W * H; i++) if (ov[i] != T) dst[i] = ov[i]; }

// ---------------------------------------------------------------- furniture
static const DeskState DESK_BASE = {SCREEN_OFF, 0, false, 0, true, false};

// Which t (1..S(14)) gives steam frame f: bit0 = (t/3)&1, bit1 = (t/4)&1, bit2 = (t/5)&1 (see drawDesk).
static int steamT(int f) {
  for (int t = 1; t <= 14 * FPS; t++)
    if (((t / 3) & 1) == (f & 1) && ((t / 4) & 1) == ((f >> 1) & 1) && ((t / 5) & 1) == ((f >> 2) & 1)) return t;
  return -1;
}

// base + screen + steam + drawer + mug + lit overlays must equal the direct draw for every combination.
static void verifyDesk() {
  static Idx want[W * H], got[W * H], ov[W * H];
  auto desk = [](DeskState d) { return [d](Canvas &c) { drawDesk(c, d); }; };
  int checked = 0;
  for (int m = 0; m <= SCREEN_PLUG; m++)
    for (int t = 0; t < 50; t += 7)
      for (int st = 0; st < 40; st += 13)
        for (int k = 0; k < 8; k++) {
          DeskState d = DESK_BASE;
          d.screen = (ScreenMode)m; d.t = t; d.steam = st; d.drawer = k & 1; d.mug = !(k & 2); d.lit = k & 4;
          if (!d.mug) d.steam = 0;   // steam only rises from the mug
          cv.clear(T); drawDesk(cv, d); memcpy(want, buf, sizeof want);
          cv.clear(T); drawDesk(cv, DESK_BASE); memcpy(got, buf, sizeof got);
          DeskState v;
          v = DESK_BASE; v.screen = d.screen; v.t = d.t; if (m != SCREEN_OFF) { diff(ov, desk(DESK_BASE), desk(v)); blitOver(got, ov); }
          v = DESK_BASE; v.steam = d.steam; if (d.steam) { diff(ov, desk(DESK_BASE), desk(v)); blitOver(got, ov); }
          v = DESK_BASE; v.drawer = true; if (d.drawer) { diff(ov, desk(DESK_BASE), desk(v)); blitOver(got, ov); }
          v = DESK_BASE; v.mug = false; if (!d.mug) { diff(ov, desk(DESK_BASE), desk(v)); blitOver(got, ov); }
          v = DESK_BASE; v.lit = true; if (d.lit) { diff(ov, desk(DESK_BASE), desk(v)); blitOver(got, ov); }
          if (memcmp(want, got, sizeof want)) { fprintf(stderr, "sheet: desk overlays do not recompose (screen %d t %d)\n", m, t); exit(1); }
          checked++;
        }
  fprintf(stderr, "desk overlays: %d combinations recompose exactly\n", checked);
}

static void verifyRack() {
  static Idx want[W * H], got[W * H], ov[W * H];
  for (int a = 0; a < 2; a++)
    for (int t = 0; t < 40; t += 5)
      for (int p = 0; p <= 16; p++) {
        cv.clear(T); drawRack(cv, a, t, p, 7); memcpy(want, buf, sizeof want);
        cv.clear(T); drawRack(cv, a, t, -1, 7); memcpy(got, buf, sizeof got);
        diff(ov, [](Canvas &c) { drawRack(c, false, 0, -1, 1); }, [p](Canvas &c) { drawRack(c, false, 0, p, 1); });
        blitOver(got, ov);
        if (memcmp(want, got, sizeof want)) { fprintf(stderr, "sheet: rack progress does not recompose\n"); exit(1); }
      }
}

static void furniture() {
  sheet = "furniture";
  for (int g = -1; g <= 8; g++) cap("bookshelf", g + 1, Tags().num("gap", g), 0, 0, [g](Canvas &c) { drawBookshelf(c, g); });

  // the desk: one full draw per screen mode, then overlays over DESK_BASE (screen off, mug, no steam, drawer shut, unlit)
  for (int m = 0; m <= SCREEN_PLUG; m++) {
    DeskState d = DESK_BASE; d.screen = (ScreenMode)m;
    cap("desk", m, Tags().str("screen", SCREEN_N[m]), 0, 0, [d](Canvas &c) { drawDesk(c, d); });
  }
  auto base = [](Canvas &c) { drawDesk(c, DESK_BASE); };
  auto screen = [&](const char *group, ScreenMode m, int frames, int step) {
    for (int f = 0; f < frames; f++) {
      DeskState d = DESK_BASE; d.screen = m; d.t = f * step;
      over(group, f, Tags().str("screen", SCREEN_N[m]).num("t", d.t).flag("overlay", true), base, [d](Canvas &c) { drawDesk(c, d); });
    }
  };
  screen("desk_scr_dim", SCREEN_DIM, 1, 1);
  screen("desk_scr_plug", SCREEN_PLUG, 1, 1);
  screen("desk_scr_code", SCREEN_CODE, 48, 1);    // full period: scroll (t/6)%8, cursor line t%6, caret (t/3)%2
  screen("desk_scr_web", SCREEN_WEB, 3, 8);       // (t/8)%3
  screen("desk_scr_saver", SCREEN_SAVER, 24, 1);  // one x bounce; the real period is thousands of ticks
  for (int f = 0; f < 8; f++) {
    DeskState d = DESK_BASE; d.steam = steamT(f);
    if (d.steam < 0) die("no t for steam frame", "desk_steam", f);
    over("desk_steam", f, Tags().num("t", d.steam).flag("overlay", true), base, [d](Canvas &c) { drawDesk(c, d); });
  }
  { DeskState d = DESK_BASE; d.drawer = true; over("desk_drawer", 0, Tags().str("drawer", "open").flag("overlay", true), base, [d](Canvas &c) { drawDesk(c, d); }); }
  { DeskState d = DESK_BASE; d.mug = false; over("desk_nomug", 0, Tags().flag("mug", false).flag("overlay", true), base, [d](Canvas &c) { drawDesk(c, d); }); }
  { DeskState d = DESK_BASE; d.lit = true; over("desk_lit", 0, Tags().flag("lit", true).flag("overlay", true), base, [d](Canvas &c) { drawDesk(c, d); }); }

  // the rack: idle LEDs blink on (t/24)%5; active LEDs hash (seed, t/2), shown for seed 1; progress is an overlay
  for (int f = 0; f < 5; f++) cap("rack_idle", f, Tags().num("t", f * 24), 0, 0, [f](Canvas &c) { drawRack(c, false, f * 24, -1, 1); });
  for (int f = 0; f < 8; f++) cap("rack_active", f, Tags().num("t", f * 2).num("seed", 1), 0, 0, [f](Canvas &c) { drawRack(c, true, f * 2, -1, 1); });
  for (int p = 0; p <= 16; p++)
    over("rack_progress", p, Tags().num("progress", p).flag("overlay", true), [](Canvas &c) { drawRack(c, false, 0, -1, 1); },
         [p](Canvas &c) { drawRack(c, false, 0, p, 1); });

  cap("bed", 0, Tags().flag("sleeping", false), 0, 0, [](Canvas &c) { drawBed(c, false, 0); });
  for (int b = 0; b < 2; b++) cap("bed_sleep", b, Tags().flag("sleeping", true).num("breath", b), 0, 0, [b](Canvas &c) { drawBed(c, true, b); });
  for (int on = 0; on < 2; on++) cap("nightstand", on, Tags().flag("lampOn", on), 0, 0, [on](Canvas &c) { drawNightstand(c, on); });
  for (int s = -1; s <= 1; s++) cap("plant", s + 1, Tags().num("sway", s), 0, 0, [s](Canvas &c) { drawPlant(c, s); });
  // whiteboard: the doodle (no plan), then total 1..5 x done 0..total; a plan over 5 items draws as total 5 with
  // done scaled to (done * 5 + total / 2) / total
  cap("whiteboard_empty", 0, Tags().num("total", 0).num("done", 0), 0, 0, [](Canvas &c) { drawWhiteboard(c, 0, 0); });
  for (int n = 1; n <= 5; n++) {
    char g[24];
    snprintf(g, sizeof g, "whiteboard_%d", n);
    for (int d = 0; d <= n; d++) cap(g, d, Tags().num("total", n).num("done", d), 0, 0, [n, d](Canvas &c) { drawWhiteboard(c, n, d); });
  }
  cap("paper", 0, Tags(), 88, 44, [](Canvas &c) { drawPaper(c, 88, 44); });
}

// ---------------------------------------------------------------- window
static void window() {
  sheet = "window";
  for (int x = 0; x < 22; x++) {
    WindowState w; w.cloudX = x;
    cap("window_day", x, Tags().str("sky", "day").num("cloudX", x), 0, 0, [w](Canvas &c) { drawWindow(c, w); });
  }
  for (int t = 0; t < 40; t++) {   // drops (rainT*2)%16 and streaks (rainT*3)%15: period 40
    WindowState w; w.rain = true; w.rainT = t;
    cap("window_rain", t, Tags().str("sky", "rain").num("rainT", t), 0, 0, [w](Canvas &c) { drawWindow(c, w); });
  }
  for (int m = 0; m < 8; m++) {
    char g[24];
    snprintf(g, sizeof g, "window_night_m%d", m);
    for (int s = 0; s < 5; s++) {   // twinkle: (starT/7 + k*3) % 5
      WindowState w; w.mode = SKY_NIGHT; w.moonPhase = m; w.starT = s * 7;
      cap(g, s, Tags().str("sky", "night").num("moonPhase", m).num("starT", s * 7), 0, 0, [w](Canvas &c) { drawWindow(c, w); });
    }
  }
  for (int t = 0; t <= 6; t++) {
    WindowState w; w.mode = SKY_NIGHT; w.shootingStar = true; w.shootT = t;
    cap("window_shoot", t, Tags().str("sky", "night").num("moonPhase", 4).num("starT", 0).num("shootT", t), 0, 0,
        [w](Canvas &c) { drawWindow(c, w); });
  }
  static const int BIRD[] = {0, 1, 8, 9, 10, 11, 12, 13};   // sitting, head turn, then flying off
  for (int f = 0; f < 8; f++) {
    WindowState w; w.bird = BIRD[f];
    cap("window_bird", f, Tags().str("sky", "day").num("cloudX", 0).num("bird", BIRD[f]), 0, 0, [w](Canvas &c) { drawWindow(c, w); });
  }
}

// ---------------------------------------------------------------- Clawd + minis
static const int PX = 40, PY = 40;   // where positioned sprites are drawn (room for arms, tails and ticks)

static void clawdCap(const char *group, int frame, ClawdPose p, Tags tg = Tags()) {
  p.x = PX; p.y = PY; p.shadow = false;   // the shadow darkens floor pixels: lighting, not sprite
  tg.str("view", VIEW_N[p.view]).num("legPhase", p.legPhase).str("arms", ARMS_N[p.arms]).str("eyes", EYES_N[p.eyes])
      .str("mouth", MOUTH_N[p.mouth]).str("tint", TINT_N[p.tint]).flag("blush", p.blush);
  cap(group, frame, tg, PX, PY, [p](Canvas &c) { drawClawd(c, p); });
}

static void clawd() {
  sheet = "clawd";
  for (int v = 0; v < 4; v++) {
    char g[24];
    snprintf(g, sizeof g, "clawd_%s_walk", VIEW_N[v]);
    for (int l = 0; l < 4; l++) { ClawdPose p; p.view = (View)v; p.legPhase = l; clawdCap(g, l, p); }
  }
  for (int v = 0; v < 4; v++) {
    char g[24];
    snprintf(g, sizeof g, "clawd_%s_arms", VIEW_N[v]);
    for (int a = 0; a <= ARMS_NONE; a++) { ClawdPose p; p.view = (View)v; p.arms = (Arms)a; clawdCap(g, a, p); }
  }
  for (int e = 0; e <= EYES_UP; e++) { ClawdPose p; p.eyes = (Eyes)e; clawdCap("clawd_eyes", e, p); }
  for (int m = 0; m <= MOUTH_FLAT; m++) { ClawdPose p; p.mouth = (Mouth)m; clawdCap("clawd_mouth", m, p); }
  { ClawdPose p; p.view = LEFTV; p.eyes = EYES_BLINK; clawdCap("clawd_side_blink", 0, p); }
  { ClawdPose p; p.view = RIGHTV; p.eyes = EYES_BLINK; clawdCap("clawd_side_blink", 1, p); }
  for (int t = 0; t < 3; t++) { ClawdPose p; p.tint = (Tint)t; clawdCap("clawd_tint", t, p); }
  // the front-view faces the director composes (Director::composePose)
  struct Ex { const char *name; Arms a; Eyes e; Mouth m; int leg; bool blush; };
  static const Ex EX[] = {
      {"wait", ARMS_UP, EYES_WIDE, MOUTH_O, 0, false},           // also react surprise
      {"wait_look_l", ARMS_UP, EYES_LOOK_L, MOUTH_O, 0, false},
      {"wait_look_r", ARMS_UP, EYES_LOOK_R, MOUTH_O, 0, false},
      {"wait_wave_a", ARMS_WAVE_A, EYES_WIDE, MOUTH_O, 0, false},
      {"wait_wave_b", ARMS_WAVE_B, EYES_WIDE, MOUTH_O, 0, false},
      {"wait_tap", ARMS_UP, EYES_WIDE, MOUTH_O, 1, false},
      {"wink", ARMS_DOWN, EYES_WINK, MOUTH_SMILE, 0, false},
      {"giggle", ARMS_DOWN, EYES_HAPPY, MOUTH_SMILE, 0, true},
      {"happy", ARMS_UP, EYES_HAPPY, MOUTH_SMILE, 0, false},
      {"wince", ARMS_DOWN, EYES_WINCE, MOUTH_FLAT, 0, false},
      {"stretch", ARMS_UP, EYES_OPEN, MOUTH_O, 0, false},
      {"yawn", ARMS_DOWN, EYES_BLINK, MOUTH_O, 0, false},
      {"yawn_up", ARMS_UP, EYES_BLINK, MOUTH_O, 0, false},
      {"wave_a", ARMS_WAVE_A, EYES_OPEN, MOUTH_SMILE, 0, false},
      {"wave_b", ARMS_WAVE_B, EYES_OPEN, MOUTH_SMILE, 0, false},
      {"sip", ARMS_UP_R, EYES_BLINK, MOUTH_NONE, 0, false},
      {"doze", ARMS_DOWN, EYES_SLEEP, MOUTH_NONE, 0, false},
      {"doze_swing", ARMS_DOWN, EYES_SLEEP, MOUTH_NONE, 1, false},
  };
  for (int i = 0; i < (int)(sizeof EX / sizeof *EX); i++) {
    ClawdPose p; p.arms = EX[i].a; p.eyes = EX[i].e; p.mouth = EX[i].m; p.legPhase = EX[i].leg; p.blush = EX[i].blush;
    clawdCap("clawd_expr", i, p, Tags().str("expr", EX[i].name));
  }
  // head on the pillow: drawn at fixed canvas coordinates (head at x 10 + shift, y 64)
  for (int t = 0; t < 2; t++) {
    Tint tint = t ? TINT_GREY : TINT_CORAL;
    const char *g = t ? "clawd_bed_grey" : "clawd_bed";
    int f = 0;
    for (int s = -1; s <= 1; s++)
      for (int e = 0; e < 2; e++)
        cap(g, f++, Tags().num("shift", s).flag("eyesOpen", e).str("tint", TINT_N[tint]), 0, 0,
            [s, e, tint](Canvas &c) { drawClawdInBed(c, s, e, tint); });
  }
}

static void minis() {
  sheet = "minis";
  for (int v = 0; v < 4; v++) {
    char g[24];
    snprintf(g, sizeof g, "mini_%s_walk", VIEW_N[v]);
    for (int l = 0; l < 4; l++)
      cap(g, l, Tags().str("view", VIEW_N[v]).num("legPhase", l).str("tint", "violet"), PX, PY,
          [v, l](Canvas &c) { drawMini(c, PX, PY, (View)v, l); });
  }
  for (int l = 0; l < 2; l++)   // at a station the arms bob on legPhase 0/1
    cap("mini_front_blink", l, Tags().str("view", "front").num("legPhase", l).flag("blink", true), PX, PY,
        [l](Canvas &c) { drawMini(c, PX, PY, FRONT, l, TINT_VIOLET, true); });
  for (int t = 0; t < 3; t++)
    cap("mini_tint", t, Tags().str("view", "front").str("tint", TINT_N[t]), PX, PY, [t](Canvas &c) { drawMini(c, PX, PY, FRONT, 0, (Tint)t); });
}

// ---------------------------------------------------------------- props + FX
static void props() {
  sheet = "props";
  for (int pg = 0; pg < 6; pg++) cap("book", pg, Tags().num("page", pg), PX, PY, [pg](Canvas &c) { drawBook(c, PX, PY, pg); });   // page%2, page%3
  cap("can", 0, Tags().flag("pour", false), PX, PY, [](Canvas &c) { drawCan(c, PX, PY, false, 0); });
  for (int t = 0; t < 5; t++) cap("can_pour", t, Tags().flag("pour", true).num("t", t), PX, PY, [t](Canvas &c) { drawCan(c, PX, PY, true, t); });
  cap("torch", 0, Tags(), PX, PY, [](Canvas &c) { drawTorch(c, PX, PY); });
  cap("mug_held", 0, Tags(), PX, PY, [](Canvas &c) { drawMugHeld(c, PX, PY); });
}

static void fx() {
  sheet = "fx";
  // emote colours as the director picks them; leaf / plug / coffee are never shown, so they get a neutral one
  struct E { Idx col; const char *name; bool used; };
  static const E EC[ICO_COUNT] = {
      {COLN(SCRD), true}, {COLN(GRN2), true}, {COLN(QO), true},  {COLN(RUG2), true}, {COLN(GRN), true}, {COLN(RED), true},
      {COLN(RED), true},  {COLN(PUR), true},  {COLN(RUG2), true}, {COLN(QO), true},  {COLN(GRN), false}, {COLN(RED), true},
      {COLN(INK), false}, {COLN(RUG2), true}, {COLN(PUR), true}, {COLN(W2), false},
  };
  for (int side = 0; side < 2; side++)
    for (int i = 0; i < ICO_COUNT; i++)
      cap(side ? "emote_r" : "emote_l", i, Tags().str("icon", ICON_N[i]).str("col", EC[i].name).flag("tailRight", side).flag("used", EC[i].used),
          PX, PY, [side, i](Canvas &c) { drawEmote(c, PX, PY, (Icon)i, EC[i].col, side); });
  for (int l = 0; l < 2; l++) cap("quest_bang", l, Tags().num("lift", l), PX, PY, [l](Canvas &c) { drawQuestBang(c, PX, PY, l); });
  // the director draws the trail at (bubble x + 6, bubble y + 20)
  for (int d = 0; d < 4; d++)
    cap("thought", d, Tags().num("dots", d).num("tailDx", 6).num("tailDy", 20), PX, PY, [d](Canvas &c) { drawThought(c, PX, PY, d, PX + 6, PY + 20); });
  for (int f = 0; f < 3; f++) cap("zzz", f, Tags().num("t", f * 6), PX, PY + 20, [f](Canvas &c) { drawZzz(c, PX, PY + 20, f * 6); });
  cap("sparkle", 0, Tags().str("col", "QW").flag("big", false), PX, PY, [](Canvas &c) { drawSparkle(c, PX, PY, QW); });
  struct S { Idx col; const char *name; };
  static const S SB[] = {{COLN(QY)}, {COLN(QW)}, {COLN(SCR)}};
  for (int i = 0; i < 3; i++)
    cap("sparkle_big", i, Tags().str("col", SB[i].name).flag("big", true), PX, PY, [i](Canvas &c) { drawSparkle(c, PX, PY, SB[i].col, true); });
  cap("sweat", 0, Tags(), PX, PY, [](Canvas &c) { drawSweat(c, PX, PY); });
  // a sample delegation line (the real one runs from Clawd to each helper): the dashes march on (i + t/4) % 3
  for (int f = 0; f < 3; f++)
    cap("dash", f, Tags().num("t", f * 4).num("dx", 32).num("dy", 24).str("col", "QY"), PX, PY,
        [f](Canvas &c) { drawDash(c, PX, PY, PX + 32, PY + 24, f * 4, QY); });
  for (int f = 0; f < 2; f++) cap("bird_sit", f, Tags().num("frame", f), PX, PY, [f](Canvas &c) { drawBird(c, PX, PY, f); });
  for (int f = 0; f < 6; f++) cap("bird_fly", f, Tags().num("frame", 8 + f), PX, PY, [f](Canvas &c) { drawBird(c, PX, PY, 8 + f); });
}

// ---------------------------------------------------------------- room
static void roomSheet() {
  sheet = "room";
  cap("walls", 0, Tags(), 0, 0, [](Canvas &c) { drawWalls(c); });
  cap("floor", 0, Tags(), 0, 0, [](Canvas &c) { drawFloor(c); });
  cap("rug", 0, Tags(), 0, 0, [](Canvas &c) { drawRug(c); });
  // everything at rest, in Director::draw order (no light patches, Clawd, helpers or FX); the plant comes from
  // drawSprites' painter pass, after the furniture
  cap("background", 0, Tags().str("screen", "dim").str("sky", "day"), 0, 0, [](Canvas &c) {
    c.clear(OUT);
    drawWalls(c); drawFloor(c);
    drawWindow(c, WindowState());
    drawWhiteboard(c, 0, 0);
    drawBookshelf(c, -1);
    drawDesk(c, DeskState());
    drawRack(c, false, 0, -1, 1);
    drawRug(c); drawBed(c, false, 0); drawNightstand(c, false);
    drawPlant(c, 0);
  });
  for (int ph = 0; ph < 2; ph++) cap("alarm_ring", ph, Tags().num("phase", ph), 0, 0, [ph](Canvas &c) { drawAlarmRing(c, ph); });
}

int main(int argc, char **argv) {
  const char *out = ".";
  for (int i = 1; i < argc; i++)
    if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
  std::string o = out;
  fj = fopen((o + "/cells.jsonl").c_str(), "w");
  fb = fopen((o + "/cells.bin").c_str(), "wb");
  FILE *fp = fopen((o + "/palette.bin").c_str(), "wb");
  if (!fj || !fb || !fp) { perror("sheet: open output"); return 1; }
  for (int i = 0; i < N_COL; i++) { uint8_t rgb[3] = {BASE[i].r, BASE[i].g, BASE[i].b}; fwrite(rgb, 1, 3, fp); }
  fclose(fp);
  verifyDesk();
  verifyRack();
  roomSheet();
  furniture();
  window();
  clawd();
  minis();
  props();
  fx();
  fclose(fj); fclose(fb);
  fprintf(stderr, "%d sprites\n", nCells);
  return 0;
}
