/* room/lamp.js — the chunky desk lamp at the far left of the desk
 * One scene item, owned by one reviewer. Attaches to window.Room (loaded after lib/room.js).
 * Geometry helpers: Room.B (floor back corner, world origin), Room.w2s(wx, wy, wz) world -> screen, Iso.* drawing.
 * Reference: mocks/r_final/scene_<light>.png. Check with: host/.venv/bin/python tools/room_item.py lamp
 *
 * Shape (logical px, relative to the base centre x, y): round iso base 15x9, thick 4 px post up to y-14, an elbow
 * arm bending up-right, and a wide conical shade whose mouth faces down-right toward the desk (bulb ~ x+12, y-13).
 * Near-black with warm brown highlights; the bulb is self-lit when `on` (L.lampOn in room/light.js; Room.LIGHT_FX
 * adds the pool + bloom at Room.P.lampHead later).
 */
(function () {
  'use strict';
  const Iso = window.Iso, Room = window.Room;

  // Room.P.lamp (base centre) and Room.P.lampHead (the bulb: base + (13, -14) in this sprite): set in room/layout.js

  const K = { r: '#120B07', d: '#1C130D', m: '#2D1F16', h: '#4D3424', l: '#6A4930' };
  Room.part.lamp = function (ctx, x, y, on) {
    // base: a wide weighted iso puck (lit brown top, thick near-black rim) with a collar under the post
    Iso.sprite(ctx, x - 7, y - 4, [
      '....mmhhhmm....',
      '..mmhhllllhmm..',
      '.mhhhlllllhhmd.',
      'dmmhhhhhhhhmmmd',
      'rdmmmmmmmmmmmdr',
      'rrddmmmmmmmddrr',
      'rrrdddddddddrrr',
      '.rrrrdddddrrrr.',
      '...rrrrrrrrr...',
    ], K);
    Iso.sprite(ctx, x - 2, y - 3, ['lhmdr', '.hmd.'], K);
    // post: thick (4 px), lit left edge
    Iso.sprite(ctx, x - 2, y - 14, Array(11).fill('lhdr'), K);
    // elbow knuckle + arm bending up-right into the back of the shade
    Iso.sprite(ctx, x - 2, y - 21, [
      '......lhd',
      '.....lhdr',
      '....lhdr.',
      '...lhdr..',
      '.mlhdr...',
      'mlhhdr...',
      'lhhddr...',
      'lhmdrr...',
    ], K);
    // shade: wide cone, knob on top, mouth down-right toward the desk
    Iso.sprite(ctx, x + 2, y - 25, [
      '.....rmr......',
      '.....mhmd.....',
      '...rmhlhmdr...',
      '..rhhllmmddr..',
      '.rhhlmmmmdddr.',
      '.hhlmmmmmddddr',
      'rhhmmmmmmddddr',
      'rhmmmmmmdddddr',
      'rmmmmmdddddddr',
      '.rmmmddddddrrr',
      '..rrdddddrr...',
      '...rrrrr......',
    ], K);
    // the bulb in the mouth of the shade: a round pale bulb, self-lit when on so the night ambient does not dim it
    const B = on ? { o: '#FFD878', w: '#FFFBEA', y: '#F4A848' } : { o: '#C8A060', w: '#E0C890', y: '#9A7238' };
    const bulb = () => Iso.sprite(ctx, x + 10, y - 16, [
      '.yoo.',
      'yowwo',
      'yowwo',
      '.yoy.',
    ], B);
    if (on) Room.unlit(ctx, bulb); else bulb();
  };
})();
