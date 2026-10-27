/* room/layout.js — THE scene layout: where every item of the square office sits. Single source of truth.
 * Loaded right after lib/room.js and BEFORE every other lib/room/*.js. The item files only PAINT: they read their
 * placement from here and never set it. To move something, change it here (and nowhere else).
 *
 * Reference: mocks/r_final/scene_day.png (480x480 device px = 160x160 logical at 3x). The mock is AI-made and not
 * strictly isometric, so these values match its COMPOSITION (what is where, what overlaps what), not its pixels.
 * Measurements below are quoted in DEVICE px of the mock (divide by 3 for logical px).
 *
 * World frame: origin Room.B = floor back corner; +x down-right (along the right wall), +y down-left (along the
 * left wall); 1 unit = 2 px across, 1 px down; z in px. Room.w2s(wx, wy, wz) -> logical screen point.
 * Props on the desk are anchored with z = 0 at the desk's world footprint (the desk box is drawn from z 0 up to
 * DESK_H, so a prop at w2s(x, y, 0) lands on the desk top at desk-world (x + DESK_H/2, y + DESK_H/2)).
 * LVGL chrome: logical y < 18 (clock, bars) and y >= 134 (counter strip): keep readable things above y 134.
 * Check with: host/.venv/bin/python tools/room_item.py full --light day
 */
(function () {
  'use strict';
  const Room = window.Room, W = (x, y, z) => Room.w2s(x, y, z || 0);

  // ---------------------------------------------------------------- room shell (walls.js, light.js, window.js)
  Room.B = { x: 67, y: 97 };   // floor back corner: the mock's corner seam is at device x ~201-204 (logical 67-68)
  Room.A = 56;                 // left wall / floor along +y (units): runs off the left screen edge, like the mock
  Room.A_R = 44;               // right wall / floor along +x: face ends at x B.x + 88 = 155 (mock: cut end from device x ~464)
  Room.WALL_H = 97;            // wall height px: mock ceiling band top at device y 79 at x 0 / 108 at x 450 -> inner ceiling y 0 at the corner

  // window glass in the right wall's face coords (u from the corner, v from the wall top; window.js + light.js)
  // mock glass: device x 298..432, top y ~85 at the left / ~150 at the right, bottom ~240 / ~300 -> logical x 99..144
  // -> u = x - B.x = 32..77, v = y - u/2 = 12..63. window.js centres the post and puts the rail a third down (mock).
  Room.WIN = { u0: 32, u1: 77, v0: 12, v1: 63 };

  // shelf on the left wall (shelf.js): plank top at z, from y0 (its right tip, near the corner) for len units
  // mock: plank right tip at device (170, ~150), books x 15..95 / y ~130..205, pot x 105..145 / y 125..160; the plank
  // runs off the left edge (len 34 reaches x < 0). Render: tip at device (~170, 145), books 15..95, pot 105..145.
  Room.SHELF = { z: 62, y0: 9, len: 34 };

  // ---------------------------------------------------------------- desk (desk.js, room.js pillars)
  // The mock desk is not iso (back edge (0, 295) -> (465, 375) device, slope ~0.2); an iso box cannot match both ends.
  // Chosen: back edge ~2 px below Clawd's body bottom (his dangling feet show above it, like the mock), back-left corner
  // at logical (19, 84) so its left end crosses the screen edge at y ~95 (mock: device y ~290 at x 0) under the lamp,
  // right end + front under the counter strip (x1 46 stays inside the right wall, A_R 44 + the 6-unit top shift).
  Room.DESK_W = { x0: -12, x1: 46, y0: 11, y1: 27 };   // world footprint (units), along +x in front of Clawd
  Room.DESK_Z = 0;             // base lift of the desk box (px)
  Room.DESK_H = 12;            // desk thickness (px): its top face is at z DESK_Z + DESK_H

  // ---------------------------------------------------------------- Clawd + chair (clawd.js, chair.js, pages)
  Room.SEAT_Z = 6;             // seat height px: Clawd's body bottom (legs 'dangle' 7 px below it)
  // mock body device x 165..300 (centre ~232 = logical 77-80), top y ~205, feet ~340..355 just above the desk edge.
  // (81, 112): body x 59..103, top logical 61 (device 183: the 'tall' sprite is taller on screen than the mock's Clawd,
  // so he sits a little higher to keep the desk/helper above the strip), feet 2 px above the desk's back edge.
  Room.P.clawd = W(11, 4);     // ground point under the chair (screen) = (81, 112); the chair follows it
  // mock helper device x 322..435, y 300..390. The iso desk edge drops 0.5 px per px right, so the mini helper stands
  // on the back edge as far right as the strip allows: (119, 133) -> body x 108..130, its feet/lower ~4 px under the strip.
  Room.P.helperR = W(31, 5);   // the mini helper on the desk's right end

  // ---------------------------------------------------------------- desk set (room.js deskBack / deskFront, light.js)
  Room.P.lamp = W(-13, 16);                                              // lamp base centre (9, 100): mock lamp device x ~15..70, base y ~305, head ~230 under the shelf
  Room.P.lampHead = { x: Room.P.lamp.x + 13, y: Room.P.lamp.y - 14 };   // the bulb in the lamp sprite (light source)
  Room.P.monitor = W(3, 16);                                           // monitor footprint centre (39, 116): mock device x 75..165, y 250..365 -> bbox x-15..x+17, y-35..y+6
  Room.P.screen = { x: Room.P.monitor.x + 2, y: Room.P.monitor.y - 16 }; // screen centre (light source)
  Room.P.keyboard = W(6, 10);                                           // keyboard centre (59, 113): mock device x 165..210, y 330..365, in front of Clawd,
                                                                        // tucked behind the monitor's right end (deskBack draws it before the monitor)
  Room.P.button = W(16, 12);                                            // red button (75, 125): mock device x 197..250, y 362..395, right of the keyboard
  Room.P.mug = W(22, 8);                                               // mug (95, 127): not in the mock; between the button and the helper (pages 20-22)
})();
