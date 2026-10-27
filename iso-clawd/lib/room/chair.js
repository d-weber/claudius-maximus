/* room/chair.js — Clawd's chair: a seat under him and a backrest centred behind his body (no armrests, no base)
 * One scene item, owned by one reviewer. Attaches to window.Room (loaded after lib/room.js).
 * Called by Room.deskBack after the desk, lamp and monitor, and BEFORE Clawd is drawn (so Clawd covers most of it).
 * Check with: host/.venv/bin/python tools/room_item.py clawd --light day
 */
(function () {
  'use strict';
  const Iso = window.Iso, Room = window.Room;
  // Geometry in Clawd's own world frame (the same one iso.js uses for the seated 'tall' sprite facing left):
  // origin (ox, oy) = (P.clawd.x - 10, P.clawd.y - 11); his body is wx 0..16, wy 0..6 (face on the +y side),
  // z SEAT_Z .. SEAT_Z + 40. +y is toward the viewer (down-left), -y is behind him.
  //  - seat: wx 0..17, wy -1..7, a 2 px slab whose top is the seat height: its front edge shows 1 unit in front of his
  //    face (under his dangling legs) and its +x end shows under his lit side. It does not reach left of his body, or it
  //    would cover the keyboard (the desk set is drawn before the chair).
  //  - backrest: wx -2..15, wy -3..0 (3 units thick), from the seat up to 3 px above his top (slack for breathing): its top edge shows
  //    above his head and its sides show a few px either side of him on screen (centred on his body).
  const PAL_BACK = { top: '#A9A796', left: '#8C8A7C', right: '#6A685F' };   // matte warm grey (pre-ambient; day ~#85837A top, stays dark at night)
  const PAL_SEAT = { top: '#959384', left: '#7A786C', right: '#5C5A52' };
  const BODY_H = 34, OVER = 1, SEAT_T = 2, BACK_T = 3;                     // backrest 3 units thick, 1 px above his top
  Room.part.chair = function (ctx) {
    const c = Room.P.clawd, sz = Room.SEAT_Z == null ? 16 : Room.SEAT_Z;
    const ox = c.x - 10, oy = c.y - 11;
    const at = (wx, wy) => Iso.w2s(ox, oy, wx, wy, 0);
    // contact shadow of the seat on the floor (mostly behind the desk; only what shows above its back edge matters)
    const g = Iso.w2s(ox, oy, 8, 3.5, 0);
    Room.shadow(ctx, g.x, g.y, 16, 3, 0.22);
    // backrest (behind him)
    const b = at(2, -BACK_T);                                              // narrower than his body (wx 2..14): he overflows it on both sides
    Iso.boxAt(ctx, b.x, b.y, sz - SEAT_T, 12, BACK_T, BODY_H + OVER + SEAT_T, PAL_BACK, { round: true });
    // seat
    const s = at(2, -1);
    Iso.boxAt(ctx, s.x, s.y, sz - SEAT_T, 13, 8, SEAT_T, PAL_SEAT, { round: true });
  };
})();
