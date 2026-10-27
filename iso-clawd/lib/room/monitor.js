/* room/monitor.js — the big monitor left of Clawd: screen toward Clawd, we see its back and the lit edge
 * One scene item, owned by one reviewer. Attaches to window.Room (loaded after lib/room.js).
 * Geometry helpers: Room.B (floor back corner, world origin), Room.w2s(wx, wy, wz) world -> screen, Iso.* drawing.
 * Reference: mocks/r_final/scene_<light>.png. Check with: host/.venv/bin/python tools/room_item.py monitor
 *
 * Seen from its back-left ('upRight' in Iso terms: the screen faces -y, toward Clawd): a big thin dark slate panel
 * (~32 x 38 px on screen) with a slightly lighter bevelled back, a dark end face, and a 1 px cyan-white bezel line
 * along its top edge and down its far right edge (the screen light leaking round the bezel: a thin line, not a face),
 * a short neck on its back and a flat rounded foot.
 * The rim is self-lit: its pixels are marked alpha 254 (like the window glass) so Room.applyLight leaves them at full
 * brightness and at night it is the brightest thing on the desk. Built in a local world frame around (x, y), back to front.
 */
(function () {
  'use strict';
  const Iso = window.Iso, Room = window.Room;

  // panel: wx 1..15 (14 units = 28 px), wy 2..4 (2 units thick), lifted PZ, PH tall; screen = its wy 2 face
  const X0 = 1, A = 14, Y0 = 2, T = 2, PZ = 3, PH = 24;
  const COL = {
    back: '#2E3C43', bevel: '#37464D', bevelHi: '#425158', backDark: '#1F282C', top: '#28333A', end: '#242E33', endHi: '#2E3A40',
    rimHi: '#A6E6F6', rim: '#5DB4D8', rimDim: '#4590B0',
    neck: { top: '#46555A', left: '#2E393D', right: '#222A2D' }, plate: { top: '#4C5C62', left: '#35434A', right: '#273034' },
    foot: { top: '#36434A', left: '#262F33', right: '#1C2326' }, footHi: '#4A5960',
  };

  // Room.P.monitor (foot centre on the desk) and Room.P.screen (screen centre): set in room/layout.js

  Room.part.monitor = function (ctx, x, y) {
    const ox = x - 9, oy = y - 10;
    const box = (wx, wy, wz, a, b, h, pal, o) => {
      const s = Iso.w2s(ox, oy, wx, wy, 0);
      return Iso.boxAt(ctx, s.x, s.y, wz, a, b, h, pal, o);
    };
    const lit = [];                                            // self-lit pixels (marked alpha 254 at the end)
    const glow = (sx, sy, c) => { ctx.fillStyle = c; ctx.fillRect(sx, sy, 1, 1); lit.push(sx, sy); };

    // flat rounded foot on the desk (its back slips under the panel)
    box(5, 3, 0, 6, 4, 1, Object.assign({}, COL.foot, { edge: COL.footHi }), { round: true });
    // panel (back faces the viewer, +y); the screen is its -y face, toward Clawd
    const p = box(X0, Y0, PZ, A, T, PH, { top: COL.top, left: COL.back, right: COL.end });
    const L = p.left, n = L.cols;
    // bevel: a lighter frame inset round the back, a soft highlight under the top edge, a darker bottom lip
    L.rect(0, 0, n, 2, COL.bevel);
    L.rect(0, 2, n, 1, COL.bevelHi);
    L.rect(0, 3, 2, PH - 5, COL.bevel);
    L.rect(2, PH - 3, n - 2, 1, COL.bevel);
    L.rect(0, PH - 2, n, 2, COL.backDark);
    for (let i = 0; i < A; i++) { const a = L.at(2 * i, 0), b = L.at(2 * i + 1, 0); ctx.fillStyle = COL.bevelHi; ctx.fillRect(a.x, a.y, 1, 1); ctx.fillRect(b.x, b.y, 1, 1); }  // top lip
    // the end face: dark, with a slightly lighter column next to the back so the panel still reads as a slab
    const R = p.right;
    R.rect(0, 0, 1, PH, COL.endHi);
    // bezel glow: ONE pixel line along the panel's far top edge (the top face's outer edge) and down the far right edge
    // of the end face; the screen is on the other side, so this is all that leaks round. Brightest at the corner.
    {                                                         // top face outer edge, column by column (same raster as Iso.boxAt)
      const W = 2 * (A + T), Yb = p.y - 1, Yl = Yb + T, Yr = Yb + A;
      for (let c = 0; c < W; c++) {
        const top = c < 2 * T ? Yl - (c >> 1) : Yr - ((W - 1 - c) >> 1);
        glow(p.x + c, top, c < 2 * T ? COL.rim : COL.rimHi);  // the short edge over the back is a step dimmer
      }
    }
    for (let v = 0; v < PH; v++) { const q = R.at(R.cols - 1, v); glow(q.x, q.y, v >= PH - 2 ? COL.rimDim : COL.rimHi); }
    { const q = R.at(R.cols - 2, 0); glow(q.x, q.y, COL.rim); }   // the corner pixel joining the two lines

    // stand: a neck up the back and a mounting plate on the panel
    box(6, 4, 1, 3, 1, 12, COL.neck);
    box(5, 4, 12, 5, 1, 5, COL.plate);

    // mark the rim self-lit (Room.applyLight skips alpha-254 pixels); anything drawn over it later resets alpha to 255
    if (lit.length) {
      let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
      for (let k = 0; k < lit.length; k += 2) { x0 = Math.min(x0, lit[k]); x1 = Math.max(x1, lit[k]); y0 = Math.min(y0, lit[k + 1]); y1 = Math.max(y1, lit[k + 1]); }
      x0 = Math.max(0, x0); y0 = Math.max(0, y0); x1 = Math.min(ctx.canvas.width - 1, x1); y1 = Math.min(ctx.canvas.height - 1, y1);
      if (x1 >= x0 && y1 >= y0) {
        const w = x1 - x0 + 1, img = ctx.getImageData(x0, y0, w, y1 - y0 + 1), d = img.data;
        for (let k = 0; k < lit.length; k += 2) {
          const px = lit[k] - x0, py = lit[k + 1] - y0;
          if (px >= 0 && py >= 0 && px < w && py <= y1 - y0) d[(py * w + px) * 4 + 3] = 254;
        }
        ctx.putImageData(img, x0, y0);
      }
    }
    return { screen: null, area: null };
  };
})();
