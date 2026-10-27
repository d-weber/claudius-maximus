/* room/walls.js — the floor and the two plaster/brick walls (base colours; the window is room/window.js)
 * One scene item, owned by one reviewer. Attaches to window.Room (loaded after lib/room.js).
 * Geometry helpers: Room.B (floor back corner, world origin), Room.w2s(wx, wy, wz) world -> screen, Iso.* drawing.
 * Reference: mocks/r_final/scene_<light>.png. Check with: host/.venv/bin/python tools/room_item.py walls
 *
 * Walls are boxes Room.WALL_T thick: their top faces are the reference's dark grey ceiling band (2*WALL_T rows + Room.RIM_H
 * painted rows) and the right wall (Room.A_R units) ends at x ~155 with its cut end filling the last 6 px to the screen
 * edge, like the reference. The left wall runs off screen (Room.A). Reference (device px): corner seam ~x 201-204, ceiling
 * band top y 79 / bottom 102 at the left edge, top 108 / bottom 130 at x 450, cut end from x ~464.
 * Flat base colours only: the light map (room/light.js) adds the corner/junction occlusion, beams and halos.
 */
(function () {
  'use strict';
  const Iso = window.Iso, Room = window.Room, C = Iso.C, SH = Iso.SHADE, R = Math.round;
  const W = Room.w2s, mix = Room.mix;

  // Room.A (left wall along +y), Room.A_R (right wall along +x), Room.WALL_H: set in room/layout.js
  Room.WALL_T = 3;    // wall thickness (units): the top faces make the reference's ceiling band (6 rows) and the cut end (6 px)
  Room.RIM_H = 1;     // + this many band-coloured rows painted on the face under the rim (band = 7 rows, as in the reference)

  // office palette: walls keep the cfg's warm plaster (the day ambient cools it to the reference's grey);
  // the floor is a darker, redder walnut than the cfg default
  Object.assign(Room.CFG.office.floor, { color: '#46311F', line: '#36261A' });

  function wall(ctx, side, cfg, L) {
    const A = side === 'L' ? Room.A : Room.A_R, H = Room.WALL_H, T = Room.WALL_T;
    const rim = '#7A7E7E', cut = mix(cfg.color, '#000000', 6);
    let P;
    if (side === 'L') {   // x in [-T, 0], y in [-T, A]: interior = the box's right face
      const s = W(-T, -T);
      P = Iso.boxAt(ctx, s.x, s.y, 0, T, A + T, H, { top: rim, right: cfg.color }).right;
      // (the y in [-T, 0] stub fills the corner post's top; the right wall, drawn next, covers its face)
    } else {              // x in [0, A], y in [-T, 0]: interior = the box's left face
      const s = W(0, -T);
      P = Iso.boxAt(ctx, s.x, s.y, 0, A, T, H, { top: rim, left: cfg.color, right: cut }).left;
    }
    // the ceiling band: the wall-top rim (box top) + RIM_H rows of the same grey on the face (uniform in the reference)
    P.rect(0, 0, P.cols, Room.RIM_H, rim);
    P.rect(0, Room.RIM_H, P.cols, 1, mix(cfg.color, '#000000', 5));
    if (cfg.style === 'brick') {
      for (let v = 4; v < H; v += 5) {
        P.rect(0, v, P.cols, 1, cfg.mortar);
        const off = ((v / 5) & 1) * 5;
        for (let u = off; u < P.cols; u += 10) P.rect(u, v - 4, 1, 4, cfg.mortar);
      }
    } else {
      // no skirting board: a soft darker band where the plaster meets the floor, tinted by cfg.skirting
      const sk = cfg.skirting || '#5E5A54';
      P.rect(0, H - 3, P.cols, 1, mix(cfg.color, sk, 2));
      P.rect(0, H - 2, P.cols, 2, mix(cfg.color, sk, 4));
    }
    if (cfg.window && side === 'R') Room.part.window(ctx, P, L);
    return P;
  }

  function floor(ctx, cfg) {
    const A = Room.A, AR = Room.A_R;
    const r = Iso.boxAt(ctx, Room.B.x, Room.B.y, 0, AR, A, 0, { top: cfg.color });
    if (cfg.style === 'wood') {
      // planks run along +y (the left wall's direction), 4 units wide, staggered butt joints
      const PW = 4;
      for (let i = PW - 1; i < AR; i += PW) for (let j = 0; j < A; j++) r.top.cell(i, j, cfg.line);
      const tone = mix(cfg.color, '#000000', 1);
      for (let p = 0; p * PW < AR; p++) {
        const i0 = p * PW, len = 13 + (p * 5) % 7, off = (p * 9) % len;
        if (p & 1) for (let i = i0; i < Math.min(AR, i0 + PW - 1); i++) for (let j = 0; j < A; j++) r.top.cell(i, j, tone);
        for (let j = off; j < A; j += len) for (let i = i0; i < Math.min(AR, i0 + PW - 1); i++) r.top.cell(i, j, cfg.line);
      }
    } else {
      for (let j = 6; j < A; j += 7) for (let i = 0; i < AR; i++) r.top.cell(i, j, cfg.line);
      for (let i = 6; i < AR; i += 7) for (let j = 0; j < A; j++) r.top.cell(i, j, cfg.line);
    }
  }

  Room.part.floor = floor;
  Room.part.wall = wall;
  // the dark seam where the two walls meet (1 px, just left of the corner line)
  Room.part.corner = function (ctx, cfg) {
    const c = mix(mix(cfg.wallL.color, cfg.wallR.color, 8), '#000000', 3);
    for (let v = 0; v < Room.WALL_H; v++) Iso.px(ctx, Room.B.x - 1, Room.B.y - Room.WALL_H + v, c);
  };
})();
