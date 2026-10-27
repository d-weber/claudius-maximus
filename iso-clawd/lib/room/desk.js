/* room/desk.js — the desk slab across the bottom of the room, the red button, the mug spot
 * One scene item, owned by one reviewer. Attaches to window.Room (loaded after lib/room.js).
 * Geometry helpers: Room.B (floor back corner, world origin), Room.w2s(wx, wy, wz) world -> screen, Iso.* drawing.
 * Reference: mocks/r_final/scene_<light>.png. Check with: host/.venv/bin/python tools/room_item.py desk
 *
 * The reference desk is not a true iso box: it is a long walnut slab across the bottom, its back edge rising gently
 * from the left wall (slope ~0.2), its front-left edge a regular iso edge (slope 0.5) running down under the counter
 * strip, its right end a small iso tip at ~(152, 127). So it is drawn as screen-space polygons (logical px).
 *
 * Clawd SITS behind the desk: pages draw desk -> lamp -> monitor -> Clawd -> helper -> deskFront, so deskFront is
 * wrapped here to repaint the desk's front band (the LIP region, in front of Clawd only) over his legs first.
 */
(function () {
  'use strict';
  const Iso = window.Iso, Room = window.Room;

  // the desk's footprint/height (Room.DESK_W, DESK_Z, DESK_H) and the button/mug anchors are set in room/layout.js
  Room.DESK = { x: 76, y: 120, a: 64, b: 30, thick: 12 };   // kept for readers (x, y = rough centre of the top)
  Room.DESK_PAL = { top: '#52321F', grain: '#5C3A24', sheen: '#6A432B', rim: '#8C6141', rimBack: '#744E33',
                    left: '#170D07', leftHi: '#2A1A0E', right: '#22150B', edge: '#7A5538' };

  // ---- the desk: an iso slab ALIGNED WITH THE ROOM, running along world +x (parallel to the right wall) in front of
  // Clawd. World footprint Room.DESK_W = {x0, x1, y0, y1} (units), box from z = DESK_Z, DESK_H px thick (room/layout.js).
  Room.deskTopAt = (wx, wy) => Room.w2s(wx, wy, Room.DESK_Z);          // screen point on the desk top for a world point
  const hash = (i, j) => { let h = (i * 374761393 + j * 668265263) | 0; h = (h ^ (h >>> 13)) * 1274126177; return (h ^ (h >>> 16)) >>> 0; };
  function paint(ctx) {
    const D = Room.DESK_W, P = Room.DESK_PAL;
    const A = D.x1 - D.x0, Bn = D.y1 - D.y0;
    // left end: a dark cabinet panel under the slab, flush with its front face, from the strip up to the desk bottom
    // (the mock's front face runs dark down to the counter strip at the left screen edge; without it the left wall
    // shows through under the slab, between the face and the left leg)
    const CAB = 12, cb = Room.w2s(D.x0, D.y1 - 3, 0);
    Iso.boxAt(ctx, cb.x, cb.y, Room.DESK_Z - Room.H, CAB, 3, Room.H, { top: P.left, left: P.left, right: P.right });
    const s = Room.w2s(D.x0, D.y0, 0);
    const r = Iso.boxAt(ctx, s.x, s.y, Room.DESK_Z, A, Bn, Room.DESK_H, { top: P.top, left: P.left, right: P.right, edge: P.edge });
    // walnut grain: long streaks running along the slab (world +x), deterministic
    for (let j = 1; j < Bn - 1; j++) {
      let i = hash(3, j) % 7;
      while (i < A) {
        const len = 3 + hash(i, j) % 6;
        if (hash(j, i) % 3 !== 0) for (let k = i; k < Math.min(A, i + len); k++) r.top.cell(k, j, P.grain);
        i += len + 2 + hash(i + 7, j) % 6;
      }
    }
    // soft sheen: a diagonal band of lighter cells across the top (a broad window reflection), dithered at its edges
    for (let i = 0; i < A; i++) for (let j = 1; j < Bn - 1; j++) {
      const d = Math.abs((i - 2 * j) - 18);
      if (d < 3 || (d < 6 && ((i + j) & 1) === 0)) r.top.cell(i, j, P.sheen);
    }
    // bevel rims: the back edge (row j 0) and the front edge (row j Bn-1), lighter
    for (let i = 0; i < A; i++) { r.top.cell(i, 0, P.rimBack); r.top.cell(i, Bn - 1, P.rim); }
    for (let j = 0; j < Bn; j++) r.top.cell(A - 1, j, P.rimBack);
    // front face: a slightly lighter strip just under the bevel, then very dark
    r.left.rect(0, 1, 2 * A, 1, P.leftHi);
    r.right.rect(0, 1, 2 * Bn, 1, P.leftHi);
    return r;
  }
  Room.part.desk = (ctx) => { const r = paint(ctx); return { box: r }; };
  Room.part.deskLip = () => {};
  Room.DESK_LIP = null;

  Room.part.button = function (ctx, x, y, pressed) {
    // dark base plate with a lighter lip, then a chunky rounded red dome with a highlight
    Iso.box(ctx, x, y, 0, 5, 5, 3, { top: '#3A3633', left: '#24211F', right: '#171514', edge: '#55504B' });
    const h = pressed ? 2 : 5;
    const d = Iso.box(ctx, x, y, 3, 4, 4, h, { top: '#E8564C', left: '#C9403A', right: '#962C28' }, { round: true });
    for (const [i, j] of [[0, 1], [1, 0], [1, 1]]) d.top.cell(i, j, '#FF9A8C');
    for (const [i, j] of [[3, 2], [2, 3], [3, 3]]) d.top.cell(i, j, '#D2463E');
    d.left.rect(1, 0, 2, 1, '#E8665C');
  };
})();
