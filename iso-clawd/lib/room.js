/* room.js — the "square office": a cut-away isometric cube room that fills the whole 480x480 screen,
 * the time-of-day LIGHTING (a light map applied over the finished frame) and the bevelled Clawd.
 * Design reference: mocks/r_final/scene_*.png. Plain script, loads after iso.js, hangs off window.Room.
 *
 * Scale: logical 160 x 160 shown at 3x (like the firmware home face), so Clawd is as big as in the mocks.
 *
 * Files: this core + one file per scene item in lib/room/ (light, walls, window, shelf, desk, lamp, monitor, clawd),
 * each owned by one reviewer. Pages load all of them (see anims/00-room-sheet.html).
 *
 * Pipeline per frame (Room.player does it): the page draws the scene in flat base colours into a 160x160
 * buffer (room, desk set, Clawd, helpers, FX) -> Room.applyLight multiplies every pixel by the light map of
 * the current preset (soft beams, pools, glows, corner shadows) -> nearest-neighbour upscale to the stage.
 * The LVGL chrome (clock, 5H/7D bars, counter strip) is NOT pixel art: it is drawn at device resolution with
 * a real font and real alpha on an overlay canvas (Room.chromeOverlay), like the firmware does.
 *
 * The room (floor + walls) is user-configurable later, so the lighting NEVER edits the room art: a light preset
 * is ambient colour + light sources, computed into a map once and applied over whatever the room cfg draws.
 *
 * Geometry: see lib/room/layout.js (floor back corner B = world origin, wall height, every item's placement). +x world = down-right (the right wall runs along it), +y = down-left (left wall).
 * LVGL chrome covers y 0..17 (bar) and y 134..160 (counter strip).
 */
(function () {
  'use strict';
  const Iso = window.Iso, C = Iso.C, SH = Iso.SHADE, R = Math.round;
  const Room = window.Room = {};

  Room.W = 160; Room.H = 160; Room.SCALE = 3;
  // Room.B (world origin), Room.A / A_R / WALL_H, Room.WIN, Room.SHELF, Room.DESK_*, Room.SEAT_Z and every Room.P anchor
  // are set by lib/room/layout.js (loaded right after this file): the single source of the scene layout.
  Room.CHROME = { top: 18, strip: 134 };
  Room.LIGHT_FX = false;                  // the light 'shines' (window beam, halo, lamp pool, monitor glow) are off for now; ambient + occlusion only
  const W = (wx, wy, wz) => Iso.w2s(Room.B.x, Room.B.y, wx, wy, wz || 0);
  Room.w2s = W;

  Room.P = {};                         // scene anchor points, filled by lib/room/layout.js
  Room.part = {};                      // item painters, filled by the part files (lib/room/*.js)

  // ------------------------------------------------------------------ colour helpers
  const hex2 = (h) => [parseInt(h.slice(1, 3), 16), parseInt(h.slice(3, 5), 16), parseInt(h.slice(5, 7), 16)];
  const rgb2 = (r, g, b) => '#' + [r, g, b].map((v) => Math.max(0, Math.min(255, R(v))).toString(16).padStart(2, '0')).join('');
  Room.mix = (a, b, k16) => { const p = hex2(a), q = hex2(b); return rgb2(...p.map((v, i) => (v * (16 - k16) + q[i] * k16) >> 4)); };

  // ------------------------------------------------------------------ room configs (user-facing later)
  Room.CFG = {
    office: { wallL: { style: 'plaster', color: '#ACB3AF', skirting: '#8A847B' }, wallR: { style: 'plaster', color: '#E4E9E3', skirting: '#7A746C', window: true },
              floor: { style: 'wood', color: '#4A3729', line: '#3A2A1F' }, shelf: true },
    brick:  { wallL: { style: 'brick', color: '#56606E', mortar: '#3C4350' }, wallR: { style: 'brick', color: '#4C5563', mortar: '#353C47', window: true },
              floor: { style: 'wood', color: '#4A3729', line: '#3A2A1F' }, shelf: false },
    bare:   { wallL: { style: 'plaster', color: '#D2CBC1', skirting: '#8E8A84' }, wallR: { style: 'plaster', color: '#BFB8AE', skirting: '#7A746C' },
              floor: { style: 'tile', color: '#6B6560', line: '#565049' }, shelf: false },
  };

  const preset = (l) => (typeof l === 'string' ? Room.LIGHT[l] : l || Room.LIGHT.day);

  // ------------------------------------------------------------------ shading toolkit
  // The reference is painted pixel art: soft contact shadows, gradients on big faces, rim highlights. These helpers
  // draw with alpha onto the 160x160 base buffer (the firmware port will pre-bake the static ones and use LVGL alpha
  // blending for the moving ones).
  // Soft elliptical shadow centred at (x, y), radii rx, ry (px), peak alpha a.
  // Pixel-art shadow: a solid core and a dithered ring (two bands, no anti-aliasing), like the reference.
  Room.shadow = function (ctx, x, y, rx, ry, a, col) {
    col = col || '0,0,0';
    x = R(x); y = R(y);
    const core = `rgba(${col},${a.toFixed(3)})`, ring = `rgba(${col},${(a * 0.8).toFixed(3)})`;
    for (let j = -ry; j <= ry; j++) for (let i = -rx; i <= rx; i++) {
      const d = (i * i) / (rx * rx) + (j * j) / (ry * ry);
      if (d <= 0.5) { ctx.fillStyle = core; ctx.fillRect(x + i, y + j, 1, 1); }
      else if (d <= 1 && ((x + i + y + j) & 1) === 0) { ctx.fillStyle = ring; ctx.fillRect(x + i, y + j, 1, 1); }
    }
  };
  // Draw a sprite through `draw(tmpCtx)` and shade it: `shade` = [{ type:'multiply'|'screen', grad:[x0,y0,x1,y1], stops:[[t, 'rgba()'],...] }]
  // The gradients are applied only where the sprite has pixels (source-atop), so the silhouette stays pixel-crisp.
  const tmp = document.createElement('canvas'); tmp.width = Room.W; tmp.height = Room.H;
  Room.shaded = function (ctx, draw, shade) {
    const t = tmp.getContext('2d');
    t.clearRect(0, 0, Room.W, Room.H);
    const out = draw(t);
    for (const sh of shade || []) {
      const g = t.createLinearGradient(...sh.grad);
      for (const [k, c] of sh.stops) g.addColorStop(k, c);
      t.globalCompositeOperation = 'source-atop';
      t.fillStyle = g; t.fillRect(0, 0, Room.W, Room.H);
      t.globalCompositeOperation = 'source-over';
    }
    ctx.drawImage(tmp, 0, 0);
    return out;
  };

  // ------------------------------------------------------------------ cached room (base colours)
  const cache = new Map();
  Room.draw = function (ctx, cfg, light) {
    cfg = typeof cfg === 'string' ? Room.CFG[cfg] : (cfg || Room.CFG.office);
    const L = preset(light);
    const key = JSON.stringify([cfg, L.sky, L.stars, L.moon, L.sun, L.clouds, L.windows, L.rooftops]);
    let cv = cache.get(key);
    if (!cv) {
      cv = document.createElement('canvas'); cv.width = Room.W; cv.height = Room.H;
      const c = cv.getContext('2d');
      c.fillStyle = C.bg; c.fillRect(0, 0, Room.W, Room.H);
      Room.part.floor(c, cfg.floor);
      Room.part.wall(c, 'L', cfg.wallL, L);
      Room.part.wall(c, 'R', cfg.wallR, L);
      Room.part.corner(c, cfg);
      if (cfg.shelf) {
        // soft shadows the shelf and the plant throw on the wall (the reference has them), then the shelf itself
        const S = Room.SHELF, sh = Room.w2s(0, S.y0 + 17, S.z - 26), se = Room.w2s(0, S.y0, S.z - 26);
        c.save(); c.globalAlpha = 0.18; c.fillStyle = '#000';
        c.beginPath(); c.moveTo(sh.x - 10, sh.y + 6); c.lineTo(se.x + 6, se.y + 6); c.lineTo(se.x + 6, se.y + 14); c.lineTo(sh.x - 10, sh.y + 14); c.closePath(); c.fill();
        c.restore();
        // the plant's shadow up-left of it on the wall (the mock has one): a SOLID soft blob (core + fainter rim, no
        // dither: a dithered ring read as a grey checker patch on the flat plaster), mostly hidden behind the plant
        for (let j = -7; j <= 7; j++) for (let i = -9; i <= 9; i++) {
          const dd = (i * i) / 81 + (j * j) / 49;
          if (dd <= 1) { c.fillStyle = `rgba(0,0,0,${dd <= 0.55 ? 0.13 : 0.07})`; c.fillRect(se.x - 17 + i, se.y - 27 + j, 1, 1); }
        }
        Room.part.shelf(c);
      }
      // window.js marks the glass pixels alpha 254 (painted sky, not lit). drawImage() of the cache over the opaque frame
      // would blend those marks away (and nudge their RGB), so keep them in a mask instead and make the cache opaque:
      // the copy into the frame is then exact, and Room.applyLight finds both the glass and the untouched room pixels by
      // comparing the frame with cv.room (the occlusion only shades the room art, never what stands in front of it).
      const img = c.getImageData(0, 0, Room.W, Room.H), d = img.data, glass = new Uint8Array(Room.W * Room.H);
      for (let p = 3, q = 0; p < d.length; p += 4, q++) if (d[p] === 254) { glass[q] = 1; d[p] = 255; }
      c.putImageData(img, 0, 0);
      cv.room = { data: c.getImageData(0, 0, Room.W, Room.H).data, glass };
      cache.set(key, cv);
    }
    ctx.drawImage(cv, 0, 0);
    if (ctx.canvas.width === Room.W && ctx.canvas.height === Room.H) Room.lastRoom = cv.room;   // read by Room.applyLight
    return L;
  };

  // Self-lit drawing: FX (sparkles, zees, ticks, clacks, flashes...) are signs, not objects in the room, so the flat
  // ambient must not dim them (at night they vanished into the dark). Room.unlit(ctx, fn) runs fn and marks every pixel
  // it changed alpha 254, which Room.applyLight leaves at its drawn colour (like the monitor rim). While Room.player
  // draws a frame, every Iso.fx.* call is wrapped this way automatically; pages wrap their own FX sprites explicitly.
  Room.unlit = function (ctx, fn) {
    if (ctx.canvas.width !== Room.W || ctx.canvas.height !== Room.H) return fn();
    const a = ctx.getImageData(0, 0, Room.W, Room.H).data;
    const out = fn();
    const img = ctx.getImageData(0, 0, Room.W, Room.H), d = img.data;
    let hit = false;
    for (let p = 0; p < d.length; p += 4) {
      if (d[p] !== a[p] || d[p + 1] !== a[p + 1] || d[p + 2] !== a[p + 2] || d[p + 3] !== a[p + 3]) { d[p + 3] = 254; hit = true; }
    }
    if (hit) ctx.putImageData(img, 0, 0);
    return out;
  };
  let inFrame = false;
  for (const k of Object.keys(Iso.fx || {})) {
    const f = Iso.fx[k];
    if (typeof f !== 'function') continue;
    Iso.fx[k] = function (ctx) { return inFrame && ctx && ctx.canvas ? Room.unlit(ctx, () => f.apply(this, arguments)) : f.apply(this, arguments); };
  }

  Room.applyLight = function (ctx, light) {
    const L = preset(light), m = Room.lightmap(L);
    const img = ctx.getImageData(0, 0, Room.W, Room.H), d = img.data;
    const room = Room.lastRoom, rd = room && room.data, act = L.actor;   // the room as Room.draw left it this frame (null: no room, occlude all)
    // soft knee above 200 so bright walls under the beam never clip to white; the light is posterized into bands
    // (multipliers in steps of 1/8, glow in steps of 24) so beams and pools read as pixel-art shading, not blur
    const knee = (v) => (v <= 200 ? v : v >= 420 ? 255 : 200 + (v - 200) * 0.25);
    const QA = (v) => Math.round(v / 8) * 8;
    for (let p = 0, q = 0; p < d.length; p += 4, q += 3) {
      if (d[p + 3] === 254) { d[p + 3] = 255; continue; }           // self-lit pixels (the monitor rim): not lit
      // still the room art here (nothing drawn over it)? then it gets the occlusion, and the glass keeps its painted sky
      const isRoom = !rd || (d[p] === rd[p] && d[p + 1] === rd[p + 1] && d[p + 2] === rd[p + 2]);
      if (isRoom && room && room.glass[p >> 2]) continue;
      const o = isRoom ? m.occ[p >> 2] : 1;
      let mr = m.mul[q] * o, mg = m.mul[q + 1] * o, mb = m.mul[q + 2] * o;
      if (!isRoom && act) {   // in front of the room: saturated pixels (Clawd, helpers) take the warm actor ambient (light.js)
        const hi = Math.max(d[p], d[p + 1], d[p + 2]), t = hi ? Math.min(1, 2.5 * (hi - Math.min(d[p], d[p + 1], d[p + 2])) / hi) * Math.min(1, hi / 160) : 0;   // dark wood stays dark
        mr += t * (act[0] - L.ambient[0]); mg += t * (act[1] - L.ambient[1]); mb += t * (act[2] - L.ambient[2]);
      }
      const k = (mr + mg + mb) / 3, kq = Math.max(0.05, Math.round(k * 16) / 16), f = kq / Math.max(1e-3, k);   // band the brightness, keep the hue
      d[p] = knee(d[p] * mr * f + QA(m.add[q]));
      d[p + 1] = knee(d[p + 1] * mg * f + QA(m.add[q + 1]));
      d[p + 2] = knee(d[p + 2] * mb * f + QA(m.add[q + 2]));
    }
    ctx.putImageData(img, 0, 0);
  };

  // ------------------------------------------------------------------ the desk set
  // Draw order in a page (back to front): Room.draw (room + shelf) -> Room.chair -> Clawd (Room.clawd) -> Room.deskBack
  // (legs, desk, lamp, keyboard, monitor: all in front of him; the keyboard sits half behind the monitor's right end)
  // -> helpers on the desk -> Room.deskFront (button, mug) -> FX.
  Room.drawDesk = (ctx, opts) => Room.part.desk(ctx, opts);
  Room.lamp = (ctx, x, y, on) => Room.part.lamp(ctx, x, y, on);
  Room.monitor = (ctx, x, y, opts) => Room.part.monitor(ctx, x, y, opts);
  Room.button = (ctx, x, y, pressed) => Room.part.button(ctx, x, y, pressed);
  Room.deskBack = function (ctx, L, opts) {
    opts = opts || {};
    L = preset(L);
    Room.pillars(ctx);
    Room.part.desk(ctx);
    Room.shadow(ctx, Room.P.lamp.x + 1, Room.P.lamp.y + 1, 9, 3, 0.35);
    Room.part.lamp(ctx, Room.P.lamp.x, Room.P.lamp.y, L.lampOn != null ? !!L.lampOn : !!L.lamp);
    // the keyboard BEFORE the monitor: its left part slips behind the panel's right end (like the mock).
    // opts.keyboard: false = none, a function (ctx) = the page draws its own (21-room-writing's hopping one)
    if (typeof opts.keyboard === 'function') opts.keyboard(ctx);
    else if (opts.keyboard !== false) Room.keyboard(ctx, Room.P.keyboard.x, Room.P.keyboard.y);
    if (opts.monitor !== false) {
      Room.shadow(ctx, Room.P.monitor.x + 2, Room.P.monitor.y + 2, 20, 5, 0.4);
      Room.part.monitor(ctx, Room.P.monitor.x, Room.P.monitor.y, { screen: opts.monitor || 'code', frame: opts.frame || 0 });
    }
  };
  // the chair: lib/room/chair.js (Room.part.chair); drawn after the desk set and BEFORE Clawd
  Room.chair = (ctx) => Room.part.chair && Room.part.chair(ctx);
  // the desk legs: two dark pillars under the front edge (mostly under the counter strip, their tops show)
  Room.pillars = function (ctx) {
    const D = Room.DESK_W;                                       // two thick legs under the front edge, near each end
    for (const wx of [D.x0 + 3, D.x1 - 8]) {
      const p = Room.w2s(wx, D.y1 - 4, 0);
      const r = Iso.boxAt(ctx, p.x, p.y, -(Room.H), 4, 3, Room.H - Room.DESK_H + 2, { left: '#24160C', right: '#160D07' });
      r.left.rect(0, 0, 1, r.left.rows, '#33200F');              // a faint highlight on the leg's front-left corner
    }
  };
  // small light-grey keyboard in front of Clawd (the reference has one), centre-anchored on the desk; drawn by deskBack
  Room.keyboard = function (ctx, x, y) {
    Room.shadow(ctx, x, y + 1, 10, 3, 0.35);
    const r = Iso.box(ctx, x, y, 0, 9, 4, 2, { top: '#AEB8BF', left: '#7C8890', right: '#5F6A72', edge: '#CDD5DA' });
    for (let i = 1; i < 8; i++) for (let j = 1; j < 3; j++) if ((i + j) % 2 === 0) r.top.cell(i, j, '#6E7C87');
  };
  Room.deskFront = function (ctx, frame, opts) {
    opts = opts || {};
    if (opts.button !== false) { Room.shadow(ctx, Room.P.button.x + 1, Room.P.button.y + 2, 8, 3, 0.4); Room.part.button(ctx, Room.P.button.x, Room.P.button.y, !!opts.pressed); }
    if (opts.mug !== false) { Room.shadow(ctx, Room.P.mug.x + 1, Room.P.mug.y + 2, 6, 2, 0.35); Iso.prop.mug(ctx, Room.P.mug.x, Room.P.mug.y, { frame }); }
  };

  Room.bevel = (ctx, a, o) => Room.part.bevel(ctx, a, o);
  Room.clawd = function (ctx, o) { const a = Iso.drawClawd(ctx, o); Room.part.bevel(ctx, a, o); return a; };
  Room.faceGlow = function () { /* the monitor glow now comes from the light map */ };

  // ------------------------------------------------------------------ chrome overlay (device resolution, real font, real alpha)
  // What LVGL draws over the scene: clock + date top-left, 5H / 7D bars top-right, translucent strip with the counters.
  Room.chromeOverlay = function (ctx, opts) {
    opts = opts || {};
    const S = Room.SCALE, Wd = Room.W * S, Hd = Room.H * S;
    const font = (px, bold) => `${bold ? '600 ' : ''}${px}px Montserrat, "DejaVu Sans", Arial, sans-serif`;
    ctx.clearRect(0, 0, Wd, Hd);
    ctx.fillStyle = C.text; ctx.font = font(26, true); ctx.textBaseline = 'top'; ctx.textAlign = 'left';
    ctx.fillText(opts.clock || '12:15', 16, 8);
    ctx.fillStyle = C.muted; ctx.font = font(13); ctx.fillText(opts.date || 'Sun 18 Oct', 17, 40);
    const bar = (y, label, frac, col) => {
      const x1 = 464, x0 = x1 - 96;
      ctx.fillStyle = C.muted; ctx.font = font(12); ctx.textAlign = 'right'; ctx.fillText(label, x0 - 8, y - 2);
      ctx.fillStyle = C.surface2; rr(ctx, x0, y, 96, 7, 3); ctx.fillStyle = col; rr(ctx, x0, y, Math.max(7, R(96 * frac)), 7, 3);
    };
    bar(14, '5H', opts.h5 == null ? 0.42 : opts.h5, C.green);
    bar(30, '7D', opts.d7 == null ? 0.18 : opts.d7, C.violet);
    const st = Room.CHROME.strip * S;
    ctx.fillStyle = 'rgba(26,25,24,0.78)'; ctx.fillRect(0, st - 2, Wd, Hd - st + 2);
    const items = opts.counters || [['4', 'working', C.orange], ['0', 'need you', C.dim], ['0', 'idle', C.dim], ['2', 'helpers', C.violet]];
    ctx.textAlign = 'center';
    items.forEach(([v, l, col], i) => {
      const x = 72 + i * 112;
      ctx.fillStyle = col; ctx.font = font(26, true); ctx.fillText(v, x, st + 8);
      ctx.fillStyle = v === '0' ? C.dim : C.muted; ctx.font = font(12); ctx.fillText(l, x, st + 40);
    });
    for (let i = 0; i < 7; i++) { ctx.fillStyle = i === 0 ? C.orange : C.dim; ctx.beginPath(); ctx.arc(216 + i * 8, 472, 2, 0, Math.PI * 2); ctx.fill(); }
  };
  function rr(ctx, x, y, w, h, r) { ctx.beginPath(); ctx.roundRect(x, y, w, h, r); ctx.fill(); }
  Room.chrome = function () { /* legacy no-op: the chrome is an overlay now (Room.chromeOverlay) */ };

  // ------------------------------------------------------------------ page helpers
  Room.params = function () {
    const q = new URLSearchParams(location.search);
    const h = q.has('t') ? parseFloat(q.get('t')) : new Date().getHours() + new Date().getMinutes() / 60;
    const light = q.get('light') || Room.lightFor(h);
    return { light, hour: h, room: q.get('room') || 'office', chrome: q.get('chrome') !== '0', q };
  };
  const CLOCKS = { morning: '07:40', day: '12:15', sunset: '19:05', dusk: '20:40', night: '00:12' };
  // Iso.player at 160x160 shown at 3x, with the lighting pass and the chrome overlay.
  // ?grid=COLS: every LIT frame in one canvas (no chrome; tools/iso_gif.py adds it at device resolution).
  Room.player = function (cfg) {
    const P = Room.params();
    const base = document.createElement('canvas'); base.width = Room.W; base.height = Room.H;
    const bctx = base.getContext('2d', { willReadFrequently: true });
    const lit = (i, t) => {
      bctx.fillStyle = C.bg; bctx.fillRect(0, 0, Room.W, Room.H);
      inFrame = true;
      try { cfg.draw(bctx, i, t); } finally { inFrame = false; }
      Room.applyLight(bctx, P.light);
      return base;
    };
    const cols = parseInt(P.q.get('grid'), 10);
    if (cols > 0) {
      const N = cfg.frames || 1, rows = Math.ceil(N / cols);
      const cv = document.createElement('canvas'); cv.width = Room.W * cols; cv.height = Room.H * rows;
      const c = cv.getContext('2d');
      c.fillStyle = '#000000'; c.fillRect(0, 0, cv.width, cv.height);
      for (let i = 0; i < N; i++) c.drawImage(lit(i, i / (cfg.fps || 10)), (i % cols) * Room.W, Math.floor(i / cols) * Room.H);
      document.body.style.cssText = 'margin:0;background:#000';
      cv.style.display = 'block';
      document.body.appendChild(cv);
      return { canvas: cv, frames: N, fps: cfg.fps, cols, rows };
    }
    const p = Iso.player(Object.assign({ width: Room.W, height: Room.H }, cfg, {
      draw(ctx, i, t) { ctx.drawImage(lit(i, t), 0, 0); },
    }));
    const stage = p.canvas.parentElement;
    const z = P.q.has('zoom') ? +P.q.get('zoom') : Room.SCALE;
    p.canvas.style.width = Room.W * z + 'px'; p.canvas.style.height = Room.H * z + 'px';
    if (P.chrome) {
      const ov = document.createElement('canvas'); ov.width = Room.W * Room.SCALE; ov.height = Room.H * Room.SCALE;
      ov.style.cssText = `position:absolute;left:0;top:0;width:${Room.W * z}px;height:${Room.H * z}px;pointer-events:none`;
      stage.style.position = 'relative';
      stage.appendChild(ov);
      const draw = () => Room.chromeOverlay(ov.getContext('2d'), { clock: CLOCKS[P.light] });
      draw();
      if (document.fonts && document.fonts.ready) document.fonts.ready.then(draw);
    }
    return p;
  };
})();
