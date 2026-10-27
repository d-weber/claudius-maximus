/* room/window.js — the 2x2 window in the right wall: panes (sky), frame, sill
 * One scene item, owned by one reviewer. Attaches to window.Room (loaded after lib/room.js).
 * Geometry helpers: Room.B (floor back corner, world origin), Room.w2s(wx, wy, wz) world -> screen, Iso.* drawing.
 * Reference: mocks/r_final/scene_<light>.png. Check with: host/.venv/bin/python tools/room_item.py window
 *
 * Layout (face coords of the right wall, u from the corner, v from the wall top), measured on the reference:
 * the window is set INTO the wall: a pale left reveal (the jamb's side, catching the light), a thick bevelled wood
 * frame (left, top, bottom), the glass Room.WIN (bottom 4 rows = pale lip + bottom frame), a centre post a bit right
 * of the middle and a centre rail ~1/4 down (upper panes shorter), and a cream sill box sticking out of the wall.
 * The sky is painted at its FINAL colours (the pane pixels are marked alpha 254 and Room.applyLight skips them).
 */
(function () {
  'use strict';
  const Iso = window.Iso, Room = window.Room, R = Math.round;

  // glass rect Room.WIN = {u0, u1, v0, v1} in the right wall's face coords: set in room/layout.js (light.js reads it too).
  // The glass proper ends 4 rows above v1 (pale lip + bottom frame live in those rows, like the mock).
  const GB = () => Room.WIN.v1 - 4;
  // centre post (3 cols) and centre rail (3 rows), measured on the mock: post at u ~56..58, rail at v ~24..26
  const POST_W = 3, RAIL_H = 3;
  const postAt = () => R(Room.WIN.u0 + (Room.WIN.u1 - Room.WIN.u0) * 0.53);
  const railAt = () => Room.WIN.v0 + R((GB() - Room.WIN.v0) * 0.26);

  // final sky colours per reference preset (the light.js stops are for the light map; the panes show these);
  // other presets (morning, dusk) use L.sky with a light cyan push
  const SKY = {
    day:    { stops: [[0, '#6FCDEA'], [0.5, '#93E2F4'], [1, '#C4F1F2']] },
    sunset: { stops: [[0, '#EE9257'], [0.35, '#F6AC66'], [0.6, '#F7B470'], [1, '#EE8C64']] },
    night:  { stops: [[0, '#08141E'], [0.6, '#0E2534'], [1, '#14303F']] },
  };
  const which = (L) => Object.keys(SKY).find((k) => Room.LIGHT[k] === L);

  // soft puffy clouds drawn in SCREEN space (round lobes, flat bottom following the wall slope), clipped to the glass:
  // anchor (du, dv) = left end of the flat bottom relative to the glass top-left; lobes [dx, dy, r] in screen px
  const CLOUDS = [
    { du: 2,  dv: 15, l: [[2, -2, 3], [7, -4, 5], [13, -6, 6], [19, -5, 5], [23, -2, 3]] },
    { du: 1,  dv: 31, l: [[2, -2, 3], [6, -4, 4], [10, -2, 3]] },
    { du: 27, dv: 11, l: [[2, -2, 3], [7, -4, 5], [12, -5, 5], [16, -2, 3]] },
    { du: 28, dv: 25, l: [[2, -2, 3], [7, -4, 5], [12, -3, 4], [16, -1, 3]] },
  ];
  // distant city along the bottom of the lower panes: [u from, u to (rel. to u0), top (rows above the glass bottom),
  // roof shape, tone b = blue-grey, g = sage, t = tan]; later entries are nearer (drawn over)
  const ROOFS = [
    [0, 7, 18, 'step', 'b'], [5, 23, 16, 'hip', 'b'], [22, 31, 11, 'flat', 'b'], [30, 40, 13, 'hip', 'b'],
    [38, 45, 16, 'flat', 'b'], [9, 26, 10, 'flat', 'g'], [25, 33, 9, 'gable', 't'], [32, 40, 12, 'flat', 't'],
    [38, 45, 7, 'flat', 'g'],
  ];
  // lit windows at night: clusters of dots [du, rows above the glass bottom]
  const LIT = [[2, 13], [3, 11], [4, 13], [3, 9], [5, 10], [6, 12], [7, 9], [8, 11], [9, 8], [11, 10], [12, 8],
    [30, 4], [32, 6], [34, 4], [35, 7], [36, 5], [38, 3], [39, 6], [40, 4], [41, 7], [42, 5], [43, 3], [37, 8]];
  const STARS = [[6, 3], [14, 1], [25, 7], [3, 15], [11, 20], [20, 24], [8, 31], [41, 6], [37, 17], [43, 27], [28, 30], [18, 12]];

  const hexRGB = (h) => { const n = parseInt(h.slice(1), 16); return [n >> 16, (n >> 8) & 255, n & 255]; };
  function skyCol(stops, f) {
    let k = 0;
    while (k < stops.length - 2 && f > stops[k + 1][0]) k++;
    const a = stops[k], b = stops[Math.min(k + 1, stops.length - 1)];
    const t = b[0] === a[0] ? 0 : Math.max(0, Math.min(1, (f - a[0]) / (b[0] - a[0])));
    return Room.mix(a[1], b[1], R(t * 16));
  }
  // generic presets: push the stops toward a clear sky (cool skies only)
  const tint = (hex) => {
    const [r, g, b] = hexRGB(hex);
    if (r > b) return hex;
    return '#' + [r * 0.9, g * 1.2 + 6, b * 1.1 + 6].map((x) => Math.min(255, R(x)).toString(16).padStart(2, '0')).join('');
  };

  function sky(ctx, P, L) {
    const { u0, u1, v0 } = Room.WIN, gb = GB(), H = gb - v0, Wd = u1 - u0, key = which(L);
    const stops = key ? SKY[key].stops : L.sky.map(([f, c]) => [f, tint(c)]);
    // gradient down the panes, banded in 3-row steps like pixel art
    for (let v = v0; v < gb; v++) P.rect(u0, v, Wd, 1, skyCol(stops, (((v - v0) / 3 | 0) * 3) / H));
    const inGlass = (x, y) => { const u = x - P.at(0, 0).x, v = y - P.at(u, 0).y; return u >= u0 && u < u1 && v >= v0 && v < gb; };
    if (L.stars) for (const [du, dv] of STARS) P.px(u0 + du, v0 + dv, (du + dv) % 3 ? '#E8F0F2' : '#9FB8C4');
    if (L.moon) {
      const p = P.at(u0 + R(L.moon.u * Wd), v0 + R(L.moon.v * H));
      const spr = ['...####..', '..###....', '.###.....', '###......', '###......', '###......', '###......', '.###.....', '..####...', '...####..'];
      spr.forEach((row, j) => [...row].forEach((ch, i) => {
        const x = p.x + i - 3, y = p.y - 6 + j;
        if (ch === '#' && inGlass(x, y)) Iso.px(ctx, x, y, (i === 0 || j === 0 || j === 9) ? '#D8DCD0' : '#F4F2E6');
      }));
    }
    if (L.sun) {
      // the low sun: a big round disc (screen space) behind the city, pale yellow with a warm rim at sunset
      const warm = key === 'sunset', r = warm ? 11 : L.sun.r;
      const p = warm ? P.at(u0 + R(Wd * 0.64), v0 + R(0.56 * H)) : P.at(u0 + R(Wd * 0.7), v0 + R(L.sun.v * H));
      const core = warm ? '#FBF2B4' : L.sun.c, rim = warm ? '#F9DE96' : L.sun.c, glow = null;
      for (let dy = -r - 2; dy <= r + 2; dy++) for (let dx = -r - 2; dx <= r + 2; dx++) {
        const d = dx * dx + dy * dy, x = p.x + dx, y = p.y + dy;
        if (!inGlass(x, y)) continue;
        if (d <= (r - 1) * (r - 1) + r) Iso.px(ctx, x, y, core);
        else if (d <= r * r + r) Iso.px(ctx, x, y, rim);
        else if (glow && d <= (r + 2) * (r + 2) + r && ((x + y) & 1)) Iso.px(ctx, x, y, glow);
      }
    }
    if (L.clouds) {
      const hi = '#FBFAF2', mid = '#EAF3F0', lo = '#D2E9EE';
      for (const c of CLOUDS) {
        const o = P.at(u0 + c.du, v0 + c.dv);
        for (let dx = -4; dx < 28; dx++) {
          const sl = dx >> 1;                                  // the flat bottom follows the wall slope
          for (let dy = -12; dy <= 0; dy++) {
            let hit = false, top = false;
            for (const [lx, ly, r] of c.l) {
              const x = dx - lx, y = dy - ly;
              if (x * x + y * y <= r * r + r) { hit = true; if (y < -r * 0.15 || (y < r * 0.3 && x < 0)) top = true; }
            }
            if (!hit) continue;
            const x = o.x + dx, y = o.y + sl + dy;
            if (inGlass(x, y)) Iso.px(ctx, x, y, dy >= 0 ? lo : top && dy < -1 ? hi : mid);
          }
        }
      }
    }
    if (L.rooftops || L.windows) {
      const night = !!L.stars;
      let tone, far, edge = null;
      if (key === 'day') {
        tone = { b: '#9AADBC', g: '#B6CBAE', t: '#D3BEA2' }; far = '#ACD6E2'; edge = true;
      } else if (key === 'sunset') {
        tone = { b: '#DD7650', g: '#CC6442', t: '#E5845A' }; far = '#EE9A6A';
      } else if (key === 'night') {
        tone = { b: '#122634', g: '#0E1E28', t: '#10212C' }; far = '#11293A';
      } else {
        const skyLo = tint(L.sky[L.sky.length - 1][1]), blue = Room.mix(L.rooftops, skyLo, night ? 2 : 5);
        tone = { b: blue, g: Room.mix(blue, night ? '#000000' : '#D4E2C0', night ? 3 : 9), t: Room.mix(blue, night ? '#000000' : '#D2B494', night ? 2 : 9) };
        far = Room.mix(L.rooftops, skyLo, night ? 5 : 10);
      }
      for (let u = u0; u < u1; u++) P.rect(u, gb - 7, 1, 7, far);
      for (const [a, b, t, kind, k] of ROOFS) {
        for (let u = a; u < b; u++) {
          let top = t;
          const e = Math.min(u - a, b - 1 - u);
          if (kind === 'gable') top = t + Math.min(0, e - 3);
          if (kind === 'hip') top = t - Math.max(0, 4 - e);
          if (kind === 'step') top = u - a < 3 ? t : t - 3;
          P.rect(u0 + u, gb - top, 1, top, tone[k]);
          if (edge) P.px(u0 + u, gb - top, Room.mix(tone[k], '#FFFFFF', 5));   // lit roof edge
        }
      }
      if (L.windows) for (const [du, h] of LIT) P.px(u0 + du, gb - h, (du % 4) ? '#EEF2F0' : '#F7D885');
    }
  }

  // P = the right wall's face painter (Iso.plane), L = light preset
  Room.part.window = function (ctx, P, L) {
    const { u0, u1, v0, v1 } = Room.WIN, Wd = u1 - u0, gb = GB();
    sky(ctx, P, L);
    const wood = '#A57552', woodL = '#C29A74', woodD = '#7A5238', woodDD = '#553826';
    const revealL = '#FFF0D0', revealD = '#E6D6B4', revealT = '#C9CEC8', lip = '#E4F2EE';
    // left reveal: the side of the opening, lit (a darker outer edge where it meets the wall face)
    P.rect(u0 - 8, v0 - 4, 4, v1 - v0 + 4, revealL); P.rect(u0 - 8, v0 - 4, 1, v1 - v0 + 4, revealD);
    // top reveal: the lintel's underside, a soft grey band above the top frame
    P.rect(u0 - 4, v0 - 4, Wd + 4, 2, revealT); P.rect(u0 - 8, v0 - 5, Wd + 9, 1, Room.mix(revealT, '#000000', 2));
    // frame: left stile (dark outer line, wood, light bevel), top rail (light top edge, dark inner lip), right edge
    P.rect(u0 - 4, v0 - 2, 4, v1 - v0 + 2, wood);
    P.rect(u0 - 4, v0 - 2, 1, v1 - v0 + 2, woodDD); P.rect(u0 - 3, v0 - 2, 1, v1 - v0 + 2, woodD);
    P.rect(u0 - 1, v0 - 1, 1, v1 - v0 + 1, woodL);
    P.rect(u0 - 4, v0 - 2, Wd + 5, 1, woodL); P.rect(u0 - 1, v0 - 1, Wd + 2, 1, woodD);
    P.rect(u1, v0 - 2, 1, v1 - v0 + 2, woodDD);
    // pale glass lip along the left and bottom edge of the panes
    P.rect(u0, v0, 1, gb - v0, lip); P.rect(u0, gb, Wd, 1, lip);
    // bottom frame: 3 rows (light top bevel, wood, dark underside)
    P.rect(u0 - 4, gb + 1, Wd + 5, 1, woodL); P.rect(u0 - 4, gb + 2, Wd + 5, 1, wood); P.rect(u0 - 4, gb + 3, Wd + 5, 1, woodD);
    // centre rail (light top, wood, dark bottom) and centre post (light left, wood, dark right) over it
    const POST = postAt(), RAIL = railAt();
    P.rect(u0 + 1, RAIL, Wd - 1, 1, woodL); P.rect(u0 + 1, RAIL + 1, Wd - 1, RAIL_H - 2, wood); P.rect(u0 + 1, RAIL + RAIL_H - 1, Wd - 1, 1, woodD);
    P.rect(POST, v0, 1, gb - v0 + 1, woodL); P.rect(POST + 1, v0, POST_W - 2, gb - v0 + 1, wood); P.rect(POST + POST_W - 1, v0, 1, gb - v0 + 1, woodD);
    // the sill: a cream box sticking out of the wall under the bottom frame (top face, front face, cut end), shadow under it
    const ztop = Room.WALL_H - v1, sx0 = (u0 - 9) / 2, len = (Wd + 10) / 2, D = 2, TH = 2;
    P.rect(u0 - 8, v1 + TH, Wd + 9, 2, Room.mix('#8C877E', '#E4E9E3', 6));
    P.rect(u0 - 7, v1 + TH + 2, Wd + 7, 1, Room.mix('#8C877E', '#E4E9E3', 11));
    const s = Room.w2s(sx0, 0, 0);
    Iso.boxAt(ctx, s.x, s.y, ztop - TH, len, D, TH, { top: '#F6EED6', left: '#CFC2A2', right: '#B9AC8E', edge: '#FFF8E6' });
    // mark the glass: alpha 254 on the pane pixels (not the post/rail/lip). Room.applyLight skips those pixels, so the sky keeps
    // its painted colours, while anything drawn over the glass later (the chair, a helper) has alpha 255 and gets lit.
    const img = ctx.getImageData(0, 0, ctx.canvas.width, ctx.canvas.height), d = img.data, W = ctx.canvas.width;
    for (let u = u0 + 1; u < u1; u++) {
      if (u >= POST && u < POST + POST_W) continue;
      for (let v = v0; v < gb; v++) {
        if (v >= RAIL && v < RAIL + RAIL_H) continue;
        const p = P.at(u, v);
        if (p.x >= 0 && p.x < W && p.y >= 0 && p.y < ctx.canvas.height) d[(p.y * W + p.x) * 4 + 3] = 254;
      }
    }
    ctx.putImageData(img, 0, 0);
  };

  // (Room.draw turns these marks into a glass mask when it caches the room, so they survive the composite into the frame)
})();
