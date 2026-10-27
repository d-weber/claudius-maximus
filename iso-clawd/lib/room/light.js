/* room/light.js — time-of-day presets and the light map (ambient, window beam, halo, corner shadow, lamp pool, monitor glow)
 * One scene item, owned by one reviewer. Attaches to window.Room (loaded after lib/room.js).
 * Geometry helpers: Room.B (floor back corner, world origin), Room.w2s(wx, wy, wz) world -> screen, Iso.* drawing.
 * Reference: mocks/r_final/scene_<light>.png. Check with: host/.venv/bin/python tools/room_item.py light
 */
(function () {
  'use strict';
  const Iso = window.Iso, Room = window.Room, C = Iso.C, SH = Iso.SHADE, R = Math.round;
  const W = Room.w2s;

  // ambient: RGB multipliers on the flat base colours (the reference is moody: daylight walls read mid-grey, not white).
  // beam: sunlight through the window {str, col}: a soft volumetric shaft down-left from the window's top-left plus the
  // sun patch it lays on the desk / Clawd's window side / the floor. halo: glow on the wall around the panes.
  // actor (optional): the ambient for the COLOURFUL things in front of the room (Clawd, the helpers, the desk), blended in
  // by the pixel's saturation (Room.applyLight). The blue night/dusk ambient alone turns Clawd mauve; the mocks keep him
  // warm orange in every light (the room's lamps and screen light him), while the walls and the grey props (chair,
  // keyboard, monitor) keep the room ambient.
  // lampOn: the bulb is lit (lamp.js; independent of LIGHT_FX). lamp: desk lamp pool strength. screen: monitor glow {str, col} (cool, toward Clawd). sky: gradient stops for the panes.
  Room.LIGHT = {
    morning: { lampOn: false, ambient: [0.74, 0.78, 0.86], beam: { str: 0.5, col: [1.0, 0.92, 0.76] }, halo: 0.28, lamp: 0, screen: { str: 0.22, col: [0.35, 0.8, 1.0] },
               sky: [[0, '#8EC2DD'], [0.45, '#C6E3EE'], [0.8, '#F2E8D2']], sun: { v: 0.72, c: '#F7D885', r: 3 }, rooftops: '#6B7686' },
    day:     { lampOn: false, ambient: [0.78, 0.8, 0.83], beam: { str: 0.55, col: [1.0, 0.9, 0.74] }, halo: 0.32, lamp: 0, screen: { str: 0.22, col: [0.35, 0.8, 1.0] },
               sky: [[0, '#A8E8F5'], [0.6, '#DDF6FA'], [1, '#F4FBF8']], clouds: true, rooftops: '#8893A0' },
    sunset:  { lampOn: true, ambient: [0.94, 0.65, 0.50], beam: { str: 0.95, col: [1.0, 0.66, 0.34] }, halo: 0.2, lamp: 0.75, screen: { str: 0.18, col: [0.35, 0.8, 1.0] },
               sky: [[0, '#6C4F8E'], [0.35, '#D9775A'], [0.7, '#F2A05A'], [0.95, '#F7D885']], sun: { v: 0.68, c: '#F7D885', r: 4 }, rooftops: '#3C2F3F' },
    dusk:    { lampOn: false, ambient: [0.5, 0.58, 0.86], actor: [0.8, 0.68, 0.66], beam: { str: 0.5, col: [0.8, 0.82, 1.0] }, halo: 0.1, lamp: 0, screen: { str: 0.75, col: [0.3, 0.8, 1.0] },
               sky: [[0, '#1E2A4A'], [0.5, '#2E4A7A'], [0.9, '#6C5A7A'], [1, '#D9775A']], windows: true, rooftops: '#1A2030' },
    night:   { lampOn: true, ambient: [0.27, 0.32, 0.42], actor: [0.68, 0.58, 0.56], beam: { str: 0.06, col: [0.7, 0.8, 1.0] }, halo: 0.05, lamp: 0, screen: { str: 1.05, rad: 480, col: [0.3, 0.8, 1.0] },
               sky: [[0, '#0B1020'], [0.7, '#141C33'], [1, '#1E2A40']], stars: true, moon: { u: 0.68, v: 0.2 }, windows: true, rooftops: '#0D111A' },
  };
  Room.lightFor = (h) => (h < 6 ? 'night' : h < 10 ? 'morning' : h < 17.5 ? 'day' : h < 19.75 ? 'sunset' : h < 21.5 ? 'dusk' : 'night');

  // Per preset, per pixel: multiplier [r,g,b] + additive glow [r,g,b]. Screen-space approximation of the room's lights.
  // Window rect in the right wall's face coords comes from Room.WIN (room/window.js); light source points from Room.P.
  const lmCache = new Map();
  const smooth = (t) => (t <= 0 ? 0 : t >= 1 ? 1 : t * t * (3 - 2 * t));
  function lightmap(L0) {
    const L = Room.LIGHT_FX === false ? Object.assign({}, L0, { beam: null, halo: 0, lamp: 0, screen: null }) : L0;   // shines off: ambient + occlusion only
    const key = JSON.stringify([L.ambient, L.beam, L.halo, L.lamp, L.screen, Room.B, Room.WIN, Room.P.lampHead, Room.P.screen]);
    let m = lmCache.get(key);
    if (m) return m;
    const Wd = Room.W, Hd = Room.H, B = Room.B;
    const mul = new Float32Array(Wd * Hd * 3), add = new Float32Array(Wd * Hd * 3);
    // occ: the room's ambient occlusion (corner, wall/floor junction) as a separate per-pixel factor. It is a property of
    // the ROOM SURFACES, so Room.applyLight applies it only to pixels still showing the room art (not to Clawd, the desk
    // set or the helpers standing in front of the walls: on them the screen-space junction line drew diagonal bands).
    const occ = new Float32Array(Wd * Hd);
    const { u0, u1, v0, v1 } = Room.WIN;
    const top = B.y - Room.WALL_H;                                                          // wall top at the corner
    const winPt = (u, v) => ({ x: B.x + u, y: top + (u >> 1) + v });                       // right-wall face -> screen
    const TL = winPt(u0, v0), BL = winPt(u0, v1), TR = winPt(u1, v0), BR = winPt(u1, v1);
    const floorY = (x) => (x >= B.x ? B.y + (x - B.x) / 2 : B.y + (B.x - x) / 2);          // wall/floor junction
    const onRightWall = (x, y) => x >= B.x && y < floorY(x);
    const onLeftWall = (x, y) => x < B.x && y < floorY(x);
    // shaft: soft wedge in the air from the window's left edge, running down-left (invariant x + y)
    const sh0 = TL.x + TL.y, sh1 = BL.x + BL.y - 0.25 * (BL.y - TL.y);
    // sun patch: steep sun, rays run (-0.3, +1) on screen (invariant x + 0.3 y); it lands on Clawd's window side and
    // the right half of the desk, just left of the panes
    const winW = BR.x - BL.x;
    const pv = (p) => p.x + 0.3 * p.y;
    const pa = pv(TL) - 0.12 * winW, pb = pv(TL) + 0.72 * winW;
    const pw = Math.max(5, (pb - pa) * 0.2);
    const sunTop = TL.y + 0.6 * (BL.y - TL.y);
    for (let y = 0; y < Hd; y++) for (let x = 0; x < Wd; x++) {
      let r = L.ambient[0], g = L.ambient[1], b = L.ambient[2];
      let ar = 0, ag = 0, ab = 0;
      const fy = floorY(x);
      // ambient occlusion (room surfaces only, see occ above): walls a touch lighter toward the ceiling, softly darker in
      // the corner and along the floor junction
      let ao;
      if (onRightWall(x, y) || onLeftWall(x, y)) {
        const up = 1 - (y - top) / Math.max(1, fy - top);
        ao = 1.0 + 0.12 * up - 0.16 * Math.exp(-Math.abs(x - B.x) / 12) - 0.16 * Math.exp(-Math.max(0, fy - y) / 9);
      } else {
        ao = 1 - 0.22 * Math.exp(-Math.max(0, y - fy) / 9);
      }
      occ[y * Wd + x] = ao;
      if (L.beam) {
        const bc = L.beam.col;
        let k = 0;
        // volumetric shaft left of the window (fades with distance from the frame)
        if (x < TL.x && (onLeftWall(x, y) || onRightWall(x, y))) {   // the shaft lives on the walls; the desk and Clawd get the sun patch below
          const s = x + y;
          const band = smooth((s - sh0 + 6) / 16) * (1 - smooth((s - sh1 + 10) / 20));
          const d = TL.x - x;
          k += 1.1 * band * smooth(d / 6) * Math.exp(-d / 70);   // the wedge on the wall is the strongest light in the reference
        }
        // sun patch on the desk, Clawd's window side and the floor; mullion shadow across the middle
        const vR = y - (top + ((x - B.x) >> 1));                                           // right-wall face row
        if (!(onRightWall(x, y) && vR < v1 + 3)) {
          const s = x + 0.3 * y;
          const band = smooth((s - pa + pw) / (2 * pw)) * (1 - smooth((s - pb + pw) / (2 * pw)));
          const gate = smooth((y - sunTop) / 14);
          const dist = Math.max(0, y - BL.y);
          const att = Math.exp(-dist / 90);
          const t = (s - pa) / Math.max(1, pb - pa);
          const post = 1 - 0.35 * Math.exp(-Math.pow((t - 0.62) * 9, 2));
          k += 0.35 * band * gate * att * post;
        }
        k *= L.beam.str;
        r += k * bc[0] * 0.85; g += k * bc[1] * 0.85; b += k * bc[2] * 0.85;
        ar += k * bc[0] * 0.08; ag += k * bc[1] * 0.08; ab += k * bc[2] * 0.08;
      }
      // halo on the right wall around the panes (daylight bleeding onto the wall), mostly under/left of the sill
      if (L.halo && onRightWall(x, y)) {
        const u = x - B.x, v = y - (top + (u >> 1));
        const du = Math.max(0, u0 - u, u - u1), dv = Math.max(0, v0 - v, v - v1);
        const d = Math.sqrt(du * du + dv * dv);
        if (d > 0) { const k = L.halo * Math.exp(-d / (u > u1 ? 4 : 8)); r += k; g += k * 0.95; b += k * 0.85; }
      }
      // desk lamp pool (warm) on the desk under the shade, plus a small bloom at the bulb
      if (L.lamp) {
        const cx = Room.P.lampHead.x - 3, cy = Room.P.lampHead.y + 11;
        const dx = (x - cx) / 1.7, dy = y - cy;
        const k = L.lamp * Math.exp(-(dx * dx + dy * dy) / 110);
        r += k * 1.0; g += k * 0.7; b += k * 0.32;
        const bx = x - Room.P.lampHead.x, by = y - Room.P.lampHead.y - 2;
        const bl = L.lamp * Math.exp(-(bx * bx + by * by) / 30);
        r += bl * 0.5; g += bl * 0.35; b += bl * 0.15;
        ar += bl * 0.35; ag += bl * 0.25; ab += bl * 0.1;
      }
      // monitor glow (cool): the screen faces Clawd, so the pool sits right of the screen, on Clawd's left side and the desk
      if (L.screen) {
        const sx = Room.P.screen.x + 15, sy = Room.P.screen.y + 6;
        const face = smooth((x - Room.P.screen.x - 3) / 8);                                  // little light on the panel's back
        const dx = (x - sx) / 1.3, dy = (y - sy) / 1.0;
        const d2 = dx * dx + dy * dy;
        const rad = L.screen.rad || 300;                                                  // glow radius (night: wide, lights Clawd)
        const k = L.screen.str * face * Math.exp(-d2 / rad);
        const c = L.screen.col;
        r += k * c[0] * 0.45; g += k * c[1] * 0.45; b += k * c[2] * 0.45;
        const bl = L.screen.str * 0.2 * face * Math.exp(-d2 / (rad * 0.55));
        ar += bl * c[0]; ag += bl * c[1]; ab += bl * c[2];
      }
      // (the window glass is excluded per pixel by Room.applyLight: window.js marks the pane pixels with alpha 254)
      const i = (y * Wd + x) * 3;
      mul[i] = r; mul[i + 1] = g; mul[i + 2] = b;
      add[i] = ar * 255; add[i + 1] = ag * 255; add[i + 2] = ab * 255;
    }
    m = { mul, add, occ };
    lmCache.set(key, m);
    return m;
  }
  Room.lightmap = lightmap;
})();
