/* room/clawd.js — Clawd's placement at the desk (behind it, the desk in front of him), his bevel finish, the helper's spot
 * One scene item, owned by one reviewer. Attaches to window.Room (loaded after lib/room.js).
 * Geometry helpers: Room.B (floor back corner, world origin), Room.w2s(wx, wy, wz) world -> screen, Iso.* drawing.
 * Reference: mocks/r_final/scene_<light>.png. Check with: host/.venv/bin/python tools/room_item.py clawd
 */
(function () {
  'use strict';
  const Iso = window.Iso, Room = window.Room, C = Iso.C, SH = Iso.SHADE, R = Math.round;
  const W = Room.w2s;

  // Room.P.clawd (ground point under the chair; he sits ON it at z = Room.SEAT_Z, legs 'dangle'), Room.SEAT_Z and
  // Room.P.helperR (the helper on the desk's right end): set in room/layout.js
  // The reference Clawd is taller than wide; the stock sprite is wider than tall. A negative squash stretches the body
  // (H + 12) and narrows it by one unit, which matches the mock's proportions. Pages add it to their beat squash.
  Room.SEAT_SQUASH = 0;                    // the form factor now comes from the 'tall' sprite size (iso.js), not from squash
  // Seated behind the desk his floor shadow would be hidden by the desk; drawn, it leaks onto the wall right of him.
  // The reference Clawd: cream top face, warm orange front, cream lit side toward the window, a gradient from a
  // bright top to a deeper bottom, a white rim on the top edges, glossy eyes with a glint, a soft shadow on the desk.
  // Base colours are pre-ambient (the day light map multiplies by ~0.78): saturated salmon front, peach-cream top,
  // a very light cream lit side (window side) so the three faces read apart like the mock.
  Room.CLAWD_TINT = { top: '#FFD3A6', left: '#FFA27A', right: '#FFE2B6' };
  Room.HELPER_TINT = { top: '#C4AEF7', left: '#8E72D6', right: '#5E46A8' };
  // The mock's eyes: tall dark slabs (6 x 13 texels, 4 rows taller than the stock 'open' glyph, which they fully cover)
  // with a 2 x 3 glint; the mini helper gets a 2 x 5 eye with a 1 px glint. Painted on the sheared face plane.
  const EYE_TALL = ['..####', '######', '##ww##', '##ww##', '##ww##', '######', '######', '######', '######', '######', 'dddddd', 'dddddd', 'dddd..'];
  const EYE_MINI = ['w#', '##', '##', '##', '##'];
  const EYE_COL = { '#': '#10191C', d: '#1A3135', w: '#EAF6F4' };
  function tallEyes(o, a) {
    const F = a && a.facePainter;
    if (!F) return;
    const mini = o.size === 'mini', S = Iso.CLAWD_SPEC[mini ? 'mini' : 'full'];
    const eyes = Object.assign({ shape: 'open', gazeX: 0, gazeY: 0 }, typeof o.eyes === 'string' ? { shape: o.eyes } : o.eyes || {});
    const sq = R(o.squash || 0), flip = o.facing === 'right';
    const k = mini ? (sq >= 3 ? 1 : 0) : (sq >= 5 ? 2 : sq >= 2 ? 1 : sq <= -3 ? -1 : 0);
    const cx = S.FW + 2 * k, ev = mini ? S.eyeV : (sq >= 3 ? S.eyeV - 1 : S.eyeV);
    const gx = R(eyes.gazeX || 0) * 2, gy = R(eyes.gazeY || 0);
    const col = Object.assign({}, EYE_COL, o.eyeColor ? { '#': o.eyeColor, d: o.eyeColor } : {});
    if (o.shine === false) col.w = col['#'];
    for (const side of [-1, 1]) {
      const shape = side < 0 && eyes.l ? eyes.l : side > 0 && eyes.r ? eyes.r : eyes.shape === 'wink' ? (side < 0 ? 'open' : 'happy') : eyes.shape;
      if (shape !== 'open') continue;
      const u = cx + (side < 0 ? S.eyeL : S.eyeR) + gx, v = ev + gy;
      if (mini) F.spriteSheared(u, v - 1, EYE_MINI, col, flip);
      else F.spriteSheared(u, v - 4, EYE_TALL, col, flip);
    }
  }
  const baseClawd = Room.clawd;
  Room.clawd = function (ctx, o) {
    o = o || {};
    const seat = Room.P.clawd, mini = o.size === 'mini';
    const seated = !mini && o.x != null && Math.abs(o.x - seat.x) <= 12 && Math.abs(o.y - seat.y) <= 8;
    const tint = o.tint === undefined || o.tint === 'orange' ? Room.CLAWD_TINT : o.tint === 'violet' ? Room.HELPER_TINT : o.tint;
    const opts = Object.assign({ shine: true }, o, { tint, shadow: false, size: mini ? 'mini' : 'tall' });
    // contact shadow (seated: a soft pool on the desk in front of the body; others: under the feet)
    if (o.shadow !== false) {
      if (seated) { /* on the chair: the chair draws the shadow under the seat */ }
      else Room.shadow(ctx, o.x, o.y + 1, mini ? 11 : 20, mini ? 3 : 5, 0.3);
    }
    const H = mini ? 14 : 40 - (opts.squash || 0);
    const y0 = (o.y || 0) - (o.z || 0) - H, y1 = (o.y || 0) - (o.z || 0);
    const res = Room.LIGHT_FX === false ? baseClawd.call(this, ctx, opts) : Room.shaded(ctx, (t) => baseClawd.call(this, t, opts), [
      { type: 'multiply', grad: [0, y0, 0, y1], stops: [[0, 'rgba(0,0,0,0)'], [0.5, 'rgba(0,0,0,0)'], [0.5, 'rgba(80,25,10,0.14)'], [0.78, 'rgba(80,25,10,0.14)'], [0.78, 'rgba(80,25,10,0.28)'], [1, 'rgba(80,25,10,0.28)']] },   // deeper toward the bottom
      { type: 'screen', grad: [(o.x || 0) - 20, y0, (o.x || 0) + 24, y0 + 10], stops: [[0, 'rgba(255,255,255,0)'], [0.5, 'rgba(255,255,255,0)'], [0.5, 'rgba(255,240,220,0.14)'], [1, 'rgba(255,240,220,0.14)']] },  // lit from the window side
    ]);
    tallEyes(opts, res);
    return res;
  };
  Room.part.bevel = function (ctx, a, o) {
    o = o || {};
    const S = Iso.CLAWD_SPEC[o.size === 'mini' ? 'mini' : 'full'];
    const flip = o.facing === 'right';
    const sq = R(o.squash || 0);
    const k = S === Iso.CLAWD_SPEC.mini ? (sq >= 3 ? 1 : 0) : (sq >= 5 ? 2 : sq >= 2 ? 1 : sq <= -3 ? -1 : 0);
    const fw = S.FW + 2 * k, d = S.D;
    const A = flip ? d : fw, B = flip ? fw : d;
    const cx = a.top.x, cy = a.top.y;
    const bx = cx - (A - B), by = cy - Math.floor((A + B) / 2);
    const tint = o.tint && typeof o.tint === 'object' ? o.tint : Iso.SHADE[o.tint || 'orange'] || Iso.SHADE.orange;
    const hi = '#FFF6EC';   // the reference has a near-white rim on the top edges
    Iso.isoLine(ctx, bx - 1, by + 1, B - 1, 'dl', hi);
    Iso.isoLine(ctx, bx + 1, by + 1, A - 1, 'dr', hi);
    const g = Iso.w2s(bx, by, Math.floor(A * 0.3), Math.floor(B * 0.35), 0);
    Iso.rect(ctx, g.x - 1, g.y, 2, 1, '#FFFFFF');
    Iso.rect(ctx, g.x + 1, g.y + 1, 2, 1, hi);
  };
})();
