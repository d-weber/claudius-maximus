/* iso.js \u2014 shared library for the isometric Clawd animation prototypes.
 *
 * Plain script (no modules, no fetch) so pages work when opened from file://.
 * Everything hangs off window.Iso. See ../README.md for the full API & conventions.
 *
 * Logical canvas: 240 x 121 px, shown at 2x = 480 x 242 device px (the home face area).
 * Projection: classic 2:1 isometric. World axes:
 *   +x  -> screen (+2, +1)   (down-right)
 *   +y  -> screen (-2, +1)   (down-left)
 *   +z  -> screen ( 0, -1)   (up, 1 world z unit = 1 px)
 * 1 world x/y "unit" = 2 px across, 1 px down. Shading: top = light, left face = mid, right face = dark.
 */
(function () {
  'use strict';
  const Iso = {};
  window.Iso = Iso;

  Iso.W = 240;      // logical width
  Iso.H = 121;      // logical height
  Iso.SCALE = 2;    // device scale (240x121 @2x = 480x242)

  // ------------------------------------------------------------------ palette
  const C = Iso.C = {
    // UI / theme (firmware/src/ui/theme.h)
    bg: '#1A1918', surface: '#262523', surface2: '#33312E', border: '#3D3A36',
    text: '#ECE9E4', muted: '#9C978E', dim: '#5E5A54',
    // Clawd
    orange: '#D97757', orangeLight: '#F0A07E', orangeDark: '#B4603F',
    blush: '#F4A592', eye: '#1A1918',
    // status
    amber: '#F2C14E', amberDark: '#C99A2E', green: '#7FB069', greenDark: '#5E8C4C',
    red: '#E5534B', redDark: '#B23A33',
    violet: '#B392F0', violetLight: '#CDB5F6', violetDark: '#8E6CC9',
    blue: '#6A9BCC', blueLight: '#A9C8E6', blueDark: '#46719C',
    grey: '#77716B', greyLight: '#948E86', greyDark: '#5E5A54',
    // desk (dark walnut) + materials
    woodTop: '#6B5140', woodLeft: '#523D30', woodRight: '#3F2F25', woodEdge: '#80624E',
    woodShadow: '#4C392D', grain: '#62493A',
    paper: '#ECE9E4', paperShade: '#C9C2B6', ink: '#5E5A54',
    screen: '#1E2A36', glow: '#A9C8E6',
    shadow: '#121110', // generic shadow on the bare background
  };

  // 3-tone shade sets used by Iso.box and drawClawd: {top, left, right[, edge]}
  const SH = Iso.SHADE = {
    orange: { top: C.orangeLight, left: C.orange, right: C.orangeDark },
    grey:   { top: C.greyLight, left: C.grey, right: C.greyDark },
    violet: { top: C.violetLight, left: C.violet, right: C.violetDark },
    wood:   { top: C.woodTop, left: C.woodLeft, right: C.woodRight, edge: C.woodEdge },
    paper:  { top: C.paper, left: C.paperShade, right: C.muted },
    metal:  { top: C.muted, left: C.grey, right: C.greyDark },
    dark:   { top: C.border, left: C.surface2, right: C.surface },
    blue:   { top: C.blueLight, left: C.blue, right: C.blueDark },
    green:  { top: '#A3CB8F', left: C.green, right: C.greenDark },
    amber:  { top: '#F7D885', left: C.amber, right: C.amberDark },
    red:    { top: '#EE8680', left: C.red, right: C.redDark },
  };

  // ------------------------------------------------------------------ raw pixels
  const R = Math.round;
  function rect(ctx, x, y, w, h, c) {
    if (!c || w <= 0 || h <= 0) return;
    ctx.fillStyle = c;
    ctx.fillRect(R(x), R(y), R(w), R(h));
  }
  Iso.rect = rect;
  Iso.px = (ctx, x, y, c) => rect(ctx, x, y, 1, 1, c);
  Iso.clear = (ctx, c) => { ctx.fillStyle = c || C.bg; ctx.fillRect(0, 0, ctx.canvas.width, ctx.canvas.height); };

  // Bresenham line, 1px.
  Iso.line = function (ctx, x0, y0, x1, y1, c) {
    x0 = R(x0); y0 = R(y0); x1 = R(x1); y1 = R(y1);
    const dx = Math.abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -Math.abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    let e = dx + dy;
    ctx.fillStyle = c;
    for (;;) {
      ctx.fillRect(x0, y0, 1, 1);
      if (x0 === x1 && y0 === y1) break;
      const e2 = 2 * e;
      if (e2 >= dy) { e += dy; x0 += sx; }
      if (e2 <= dx) { e += dx; y0 += sy; }
    }
  };
  // Clean 2:1 iso line: `n` steps of 2px across / 1px vertical. dir: 'dr','dl','ur','ul'.
  Iso.isoLine = function (ctx, x, y, n, dir, c) {
    const sx = dir[1] === 'r' ? 1 : -1, sy = dir[0] === 'd' ? 1 : -1;
    for (let i = 0; i < n; i++) rect(ctx, sx > 0 ? x + 2 * i : x - 2 * i - 1, y + sy * i, 2, 1, c);
  };
  // Ordered dither fill. density: 0.25 | 0.5 | 0.75 (fraction of pixels painted).
  Iso.dither = function (ctx, x, y, w, h, c, density) {
    density = density == null ? 0.5 : density;
    ctx.fillStyle = c;
    x = R(x); y = R(y);
    for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) {
      const X = x + i, Y = y + j;
      const p = density >= 0.75 ? !(X % 2 === 0 && Y % 2 === 0)
        : density >= 0.5 ? (X + Y) % 2 === 0
        : (X % 2 === 0 && Y % 2 === 0);
      if (p) ctx.fillRect(X, Y, 1, 1);
    }
  };
  // Sprite from strings. rows: ['.#.', '###'], map: {'#': color}. '.' and ' ' are transparent.
  // flipX mirrors horizontally. Returns {w, h}.
  Iso.sprite = function (ctx, x, y, rows, map, flipX) {
    x = R(x); y = R(y);
    const w = rows.reduce((m, r) => Math.max(m, r.length), 0);
    for (let j = 0; j < rows.length; j++) {
      const r = rows[j];
      for (let i = 0; i < r.length; i++) {
        const ch = r[i];
        if (ch === '.' || ch === ' ') continue;
        const col = map[ch];
        if (!col) continue;
        ctx.fillStyle = col;
        ctx.fillRect(flipX ? x + w - 1 - i : x + i, y + j, 1, 1);
      }
    }
    return { w, h: rows.length };
  };

  // ------------------------------------------------------------------ 3x5 pixel font
  const FONT = {
    '0': '75557', '1': '26227', '2': '71747', '3': '71317', '4': '55711', '5': '74717', '6': '74757',
    '7': '71122', '8': '75757', '9': '75717',
    A: '25755', B: '65656', C: '34443', D: '65556', E: '74647', F: '74644', G: '34553', H: '55755',
    I: '72227', J: '11152', K: '55655', L: '44447', M: '57755', N: '65555', O: '25552', P: '65644',
    Q: '25563', R: '65655', S: '34216', T: '72222', U: '55557', V: '55552', W: '55775', X: '55255',
    Y: '55222', Z: '71247',
    '!': '22202', '?': '61202', '.': '00002', ',': '00024', ':': '02020', '-': '00700', '+': '02720',
    '/': '11244', '>': '42124', '<': '12421', '_': '00007', '$': '36236', '#': '57575', '%': '51245',
    '(': '12221', ')': '42224', '[': '32223', ']': '62226', '=': '07070', '*': '05250', "'": '22000',
    '"': '55000', '~': '00360', ' ': '00000',
  };
  // Draw text in the 3x5 font (uppercase; 4px advance). Returns width in px.
  Iso.text = function (ctx, x, y, str, c) {
    x = R(x); y = R(y);
    ctx.fillStyle = c || C.text;
    str = String(str).toUpperCase();
    for (let k = 0; k < str.length; k++) {
      const g = FONT[str[k]] || FONT['?'];
      for (let r = 0; r < 5; r++) {
        const bits = +g[r];
        for (let b = 0; b < 3; b++) if (bits & (4 >> b)) ctx.fillRect(x + k * 4 + b, y + r, 1, 1);
      }
    }
    return str.length * 4 - 1;
  };
  Iso.textWidth = (str) => String(str).length * 4 - 1;

  // ------------------------------------------------------------------ iso projection
  // Screen position of world point (wx, wy, wz) given the screen position (ox, oy) of world (0,0,0).
  Iso.w2s = (ox, oy, wx, wy, wz) => ({ x: ox + 2 * (wx - wy), y: oy + (wx + wy) - (wz || 0) });

  // Face painter for one rasterised face. u = column from the face's screen-left edge,
  // v = row from the face's top edge (rows follow the 2:1 slope = "sheared").
  function facePainter(ctx, X0, Y0, cols, rows, slope) {
    // slope +1: face descends to the right (left face); -1: rises to the right (right face)
    const yAt = slope > 0 ? (u) => Y0 + (u >> 1) : (u) => Y0 + ((cols - 1 - u) >> 1);
    const P = {
      cols, rows,
      // screen position of face texel (u, v)
      at: (u, v) => ({ x: X0 + u, y: yAt(u) + v }),
      // sheared rect: each column follows the face slope
      rect(u, v, w, h, c) {
        for (let i = 0; i < w; i++) rect(ctx, X0 + u + i, yAt(u + i) + v, 1, h, c);
      },
      px(u, v, c) { rect(ctx, X0 + u, yAt(u) + v, 1, 1, c); },
      // unsheared sprite anchored at texel (u, v): handy for symbols that must read upright
      sprite(u, v, rowsArr, map, flipX) { return Iso.sprite(ctx, X0 + u, yAt(u) + v, rowsArr, map, flipX); },
      // sheared sprite: each sprite column follows the slope
      spriteSheared(u, v, rowsArr, map) {
        for (let j = 0; j < rowsArr.length; j++) for (let i = 0; i < rowsArr[j].length; i++) {
          const col = map[rowsArr[j][i]];
          if (col) rect(ctx, X0 + u + i, yAt(u + i) + v + j, 1, 1, col);
        }
      },
    };
    return P;
  }

  // Iso box (voxel block). (sx, sy) = screen position of the box's world-min corner (the BACK
  // corner) at the box's base; z lifts it. a = size along x (units), b = along y (units),
  // h = height in px. pal = {top, left, right, edge?} (any may be null to skip that face).
  // opts.round: knock off the outer silhouette corner pixels (soft, chunky look).
  // Returns {left, right, top} painters (see facePainter / topPainter) + bounds.
  Iso.boxAt = function (ctx, sx, sy, z, a, b, h, pal, opts) {
    opts = opts || {};
    sx = R(sx); sy = R(sy); z = R(z || 0);
    const Yback = sy - z - h, XL = sx - 2 * b;
    const Yleft = Yback + b, Yright = Yback + a, Yfront = Yback + a + b;
    const W = 2 * (a + b);
    for (let c = 0; c < W; c++) {
      const X = XL + c;
      const top = c < 2 * b ? Yleft - (c >> 1) : Yright - ((W - 1 - c) >> 1);
      const ridge = c < 2 * a ? Yleft + (c >> 1) : Yright + ((W - 1 - c) >> 1);
      let t0 = top, s1 = ridge + h;
      if (opts.round) {
        const edgeCol = c === 0 || c === W - 1;
        if (edgeCol) { t0 = top + 1; s1 -= 1; }
      }
      if (pal.top && ridge > t0) rect(ctx, X, t0, 1, ridge - t0, pal.top);
      if (pal.edge && ridge > top && h > 0) rect(ctx, X, ridge - 1, 1, 1, pal.edge);
      const side = c < 2 * a ? pal.left : pal.right;
      let s0 = ridge;
      if (opts.round && (c === 0 || c === W - 1)) s0 = ridge + 1;
      if (side && s1 > s0) rect(ctx, X, s0, 1, s1 - s0, side);
    }
    const Xback = XL + 2 * b;
    return {
      left: facePainter(ctx, XL, Yleft, 2 * a, h, +1),
      right: facePainter(ctx, XL + 2 * a, Yright, 2 * b, h, -1),
      top: {
        // unit cell (i, j) of the top face -> a 2x1 pixel pair
        cell(i, j, c) { rect(ctx, Xback + 2 * (i - j) - 1, Yback + i + j + 1, 2, 1, c); },
        at: (i, j) => ({ x: Xback + 2 * (i - j), y: Yback + i + j }),
      },
      x: XL, y: Yback + 1, w: W, h: a + b - 1 + h,
    };
  };
  // Center-anchored box: (x, y) = screen point of the footprint CENTER on the ground, z = lift.
  Iso.box = function (ctx, x, y, z, a, b, h, pal, opts) {
    return Iso.boxAt(ctx, x - (a - b), y - Math.floor((a + b) / 2), z, a, b, h, pal, opts);
  };
  // Flat diamond on the ground (e.g. a shadow), centered at (x, y), a x b units.
  Iso.tile = function (ctx, x, y, a, b, c) {
    return Iso.box(ctx, x, y, 0, a, b, 0, { top: c });
  };
  // Vertical plane (zero thickness). plane: 'L' (runs along x, faces down-left) or 'R' (runs along y,
  // faces down-right). (x, y) = screen point of the plane's bottom-center, z = lift of its bottom edge,
  // len = width in units (2px per unit), h = height px, fill = color. Returns the face painter.
  Iso.plane = function (ctx, x, y, z, len, h, plane, fill) {
    if (plane === 'R') {
      const p = Iso.boxAt(ctx, x + len, y - Math.floor(len / 2), z, 0, len, h, { right: fill });
      return p.right;
    }
    const p = Iso.boxAt(ctx, x - len, y - Math.floor(len / 2), z, len, 0, h, { left: fill });
    return p.left;
  };

  // ------------------------------------------------------------------ object builder
  // Collects axis-aligned boxes in a LOCAL frame (A x B footprint, front = local +y, i.e. "faces
  // left" = toward the viewer's lower-left), mirrors/rotates them for `facing`, sorts and draws.
  // facing: 'left' (+y, down-left), 'right' (+x, down-right), 'upRight' (-y), 'upLeft' (-x).
  function Obj(ctx, x, y, A, B, facing, z) {
    facing = facing || 'left';
    const swap = facing === 'right' || facing === 'upLeft';
    const refl = facing === 'upRight' || facing === 'upLeft';
    const WA = swap ? B : A, WB = swap ? A : B;  // world footprint
    const ox = x - (WA - WB), oy = y - Math.floor((WA + WB) / 2);
    const list = [];
    return {
      facing, frontVisible: !refl,
      box(lx, ly, lz, la, lb, lh, pal, tag, opts) {
        if (refl) ly = B - ly - lb;
        let wx = lx, wy = ly, wa = la, wb = lb;
        if (swap) { wx = ly; wy = lx; wa = lb; wb = la; }
        list.push({ wx, wy, wz: lz + (z || 0), wa, wb, lh, pal, tag, opts, k: list.length });
      },
      // world point of a local point (for anchors)
      pt(lx, ly, lz) {
        if (refl) ly = B - ly;
        let wx = lx, wy = ly;
        if (swap) { wx = ly; wy = lx; }
        return Iso.w2s(ox, oy, wx, wy, lz + (z || 0));
      },
      draw() {
        list.sort((p, q) => (p.wz - q.wz) || ((p.wx + p.wa / 2 + p.wy + p.wb / 2) - (q.wx + q.wa / 2 + q.wy + q.wb / 2)) || (p.k - q.k));
        const out = {};
        for (const it of list) {
          const s = Iso.w2s(ox, oy, it.wx, it.wy, 0);
          const r = Iso.boxAt(ctx, s.x, s.y, it.wz, it.wa, it.wb, it.lh, it.pal, it.opts);
          // the face showing the local FRONT (+y local) / BACK of this box, if visible
          r.front = refl ? null : (swap ? r.right : r.left);
          r.back = refl ? (swap ? r.right : r.left) : null;
          r.side = swap ? r.left : r.right; // the other visible vertical face
          if (it.tag) out[it.tag] = r;
        }
        return out;
      },
    };
  }
  Iso.obj = (ctx, x, y, A, B, facing, z) => Obj(ctx, x, y, A, B, facing, z);

  // ------------------------------------------------------------------ Clawd
  // Size specs. FW = face width (units, 2px each), D = depth (units), H = body height px.
  const SPEC = {
    full: { FW: 16, D: 6, H: 28, legH: 6, legW: 2, legD: 2, legX: [1, 4, 10, 13],
            eyeW: 6, eyeH: 12, eyeL: -12, eyeR: 6, eyeV: 5, armL: 4, armD: 2 },
    mini: { FW: 8, D: 3, H: 12, legH: 3, legW: 1, legD: 1, legX: [1, 2, 5, 6],
            eyeW: 2, eyeH: 5, eyeL: -6, eyeR: 4, eyeV: 3, armL: 2, armD: 1 },
  };
  Iso.CLAWD_SPEC = SPEC;

  const EYE_SPR = {
    happy: ['...####...', '..######..', '.###..###.', '###....###', '##......##'],
    winceL: ['##.....', '####...', '..####.', '....###', '..####.', '####...', '##.....'],
    winceR: ['.....##', '...####', '.####..', '###....', '.####..', '...####', '.....##'],
    miniHappy: ['.##.', '#..#'],
  };
  const MOUTH_SPR = {
    o: ['.##.', '####', '####', '.##.'],
    o_small: ['##', '##'],
    laugh: ['######', '######', '.####.', '..##..'],
    smile: ['#......#', '##....##', '.######.'],
    wobbly: ['.##...##..', '##.###.##.', '.....#..##'],
    flat: ['####'],
  };
  const MOUTH_FILL = { laugh: ['......', '..##..', '......', '......'] }; // tongue pixels

  function resolveTint(t) {
    if (!t) return SH.orange;
    if (typeof t === 'object') return t;
    return SH[t] || SH.orange;
  }

  // Arm boxes for a pose, in Clawd's local frame (face on local +y side, x along the face).
  // side: -1 = arm at the x<0 end, +1 = arm at the x>FW end. zb = body bottom z.
  // Returns [{x, y, z, a, b, h}], plus hand point {x, y, z} (local).
  function armGeom(pose, side, S, FWb, x0, zb, H) {
    const L = S.armL, D = S.armD, Dp = S.D;
    const T = Math.max(2, Math.round(H * 0.25));
    const out = (len) => (side < 0 ? x0 - len : x0 + FWb);
    const inner = (len) => (side < 0 ? x0 : x0 + FWb - len);
    const tipX = (len) => (side < 0 ? x0 - len : x0 + FWb + len);
    const yb = Dp - D;
    const zOut = zb + Math.round(H * 0.36);
    switch (pose) {
      case 'none': return { boxes: [], hand: null };
      case 'high': return { boxes: [{ x: out(L), y: yb, z: zOut + Math.round(H * 0.14), a: L, b: D, h: T }],
        hand: { x: tipX(L), y: yb + D / 2, z: zOut + Math.round(H * 0.14) + T / 2 } };
      case 'up': case 'waveA': {
        const l1 = Math.max(1, L - 1), z1 = zb + Math.round(H * 0.5), z2 = z1 + T - 1;
        return { boxes: [
          { x: out(l1), y: yb, z: z1, a: l1, b: D, h: T },
          { x: side < 0 ? x0 - l1 - l1 : x0 + FWb + l1, y: yb, z: z2, a: l1, b: D, h: T },
        ], hand: { x: tipX(2 * l1), y: yb + D / 2, z: z2 + T } };
      }
      case 'waveB': { // straight up beside the head
        const l1 = Math.max(1, L - 1), z1 = zb + Math.round(H * 0.62);
        return { boxes: [{ x: out(l1), y: yb, z: z1, a: l1, b: D, h: Math.round(H * 0.55) }],
          hand: { x: tipX(l1) - (side * l1) / 2, y: yb + D / 2, z: z1 + Math.round(H * 0.55) } };
      }
      case 'down': {
        const l1 = Math.max(1, L - 2), hh = Math.round(H * 0.45);
        return { boxes: [{ x: out(l1), y: yb, z: zb + 1, a: l1, b: D, h: hh }],
          hand: { x: tipX(l1), y: yb + D / 2, z: zb + 1 } };
      }
      case 'forward': case 'hold': case 'typeA': case 'typeB': {
        const zz = pose === 'typeA' ? zb + Math.round(H * 0.12) : pose === 'typeB' ? zb + Math.round(H * 0.22) : zb + Math.round(H * 0.3);
        const len = L, l1 = Math.max(1, L - 1);
        return { boxes: [{ x: inner(l1 + 0), y: Dp, z: zz, a: l1, b: len, h: T - 1 }],
          hand: { x: inner(l1) + l1 / 2, y: Dp + len, z: zz + (T - 1) / 2 }, front: true };
      }
      case 'out': default:
        return { boxes: [{ x: out(L), y: yb, z: zOut, a: L, b: D, h: T }],
          hand: { x: tipX(L), y: yb + D / 2, z: zOut + T / 2 } };
    }
  }

  // Draw Clawd. Returns anchors (screen coords): {hand: {l, r}, head, face, eyes: {l, r}, mouth, bbox, feet}.
  // See README for every option.
  Iso.drawClawd = function (ctx, o) {
    o = o || {};
    const S = SPEC[o.size === 'mini' ? 'mini' : 'full'];
    const mini = S === SPEC.mini;
    const pal = resolveTint(o.tint);
    const facing = o.facing === 'right' ? 'right' : 'left';
    const flip = facing === 'right';
    const x = R(o.x == null ? Iso.SPOT.clawd.x : o.x), y = R(o.y == null ? Iso.SPOT.clawd.y : o.y);
    const lift = R(o.z || 0);
    const squash = R(o.squash || 0);
    const legs = o.legs || 'stand';
    const eyes = Object.assign({ shape: 'open', gazeX: 0, gazeY: 0 }, typeof o.eyes === 'string' ? { shape: o.eyes } : o.eyes || {});
    const mouth = o.mouth || 'none';
    const arms = Object.assign({ l: 'out', r: 'out' }, typeof o.arms === 'string' ? { l: o.arms, r: o.arms } : o.arms || {});
    const shake = R(o.shake || 0);

    // body dims (squash: shorter + wider; stretch: taller + narrower)
    const H = Math.max(4, S.H - squash);
    const k = mini ? (squash >= 3 ? 1 : 0) : (squash >= 5 ? 2 : squash >= 2 ? 1 : squash <= -3 ? -1 : 0);
    const FWb = S.FW + 2 * k, x0 = -k;
    const sitting = legs === 'sit';
    const dangling = legs === 'dangle' || legs === 'dangleB';
    const zb = sitting ? 1 : dangling ? 0 : S.legH;    // body bottom (local z)

    // local -> world (flip swaps axes = horizontal mirror, keeps light from the left)
    const D = S.D;
    const ox = x + shake - (flip ? D - S.FW : S.FW - D), oy = y - Math.floor((S.FW + D) / 2);
    const toW = (lx, ly) => (flip ? [ly, lx] : [lx, ly]);
    const scr = (lx, ly, lz) => { const w = toW(lx, ly); return Iso.w2s(ox, oy, w[0], w[1], lz + lift); };
    function lbox(lx, ly, lz, la, lb, lh, p, opt) {
      const w = toW(lx, ly), s = Iso.w2s(ox, oy, w[0], w[1], 0);
      return Iso.boxAt(ctx, s.x, s.y, lz + lift, flip ? lb : la, flip ? la : lb, lh, p, opt);
    }

    // shadow on the ground (shrinks with lift)
    if (o.shadow !== false) {
      const sc = o.shadow || C.woodShadow;
      const sh = Math.min(Math.floor(lift / 6), 3);
      const sa = S.FW + 2 - 2 * sh, sb = D + 2 - sh;
      if (sa > 2 && sb > 1) {
        const sw = toW(-1 + sh, -1 + Math.floor(sh / 2)), ss = Iso.w2s(ox - shake, oy, sw[0], sw[1], 0);
        Iso.boxAt(ctx, ss.x, ss.y, 0, flip ? sb : sa, flip ? sa : sb, 0, { top: sc });
      }
    }

    // legs
    if (!sitting) {
      const order = S.legX.map((lx, i) => ({ lx, i })).sort((p, q) => (flip ? q.lx - p.lx : p.lx - q.lx));
      for (const { lx, i } of order) {
        let lz = 0, lh = S.legH, ly = D - S.legD - (mini ? 0 : 1);
        if (legs === 'stepA' && (i === 0 || i === 2)) { lz = 2; lh = S.legH - 2; }
        if (legs === 'stepB' && (i === 1 || i === 3)) { lz = 2; lh = S.legH - 2; }
        if (legs === 'tap' && i === (flip ? 0 : 3)) { lz = 2; lh = S.legH - 2; }
        if (dangling) {
          const fwd = (legs === 'dangleB') === (i % 2 === 0) ? 1 : 0;
          ly = D - S.legD + fwd; lz = -S.legH - 1 + fwd; lh = S.legH + 1 - fwd;
        }
        if (lh > 0) lbox(lx + x0 + k, ly, lz, S.legW, S.legD, lh, pal);
      }
    } else {
      for (const lx of S.legX) lbox(lx, D - S.legD - (mini ? 0 : 1), 0, S.legW, S.legD, 1, pal);
    }

    // arms. Screen-left arm = local x<0 end when facing left, local x>FW end when facing right.
    const geomL = armGeom(flip ? arms.l : arms.l, flip ? +1 : -1, S, FWb, x0, zb, H);
    const geomR = armGeom(arms.r, flip ? -1 : +1, S, FWb, x0, zb, H);
    // the arm at local x<0 is BEHIND the body plane; the one at x>FW is in front of the side face
    const backG = flip ? geomR : geomL, frontG = flip ? geomL : geomR;
    const drawArm = (g) => { for (const b of g.boxes) lbox(b.x, b.y, b.z, b.a, b.b, b.h, pal); };
    if (!backG.front) drawArm(backG);

    // body
    const body = lbox(x0, 0, zb, FWb, D, H, pal, { round: true });
    const F = flip ? body.right : body.left;

    // face
    const cx = FWb; // centre column of the face (between cx-1 and cx)
    const ev = mini ? S.eyeV : (squash >= 3 ? S.eyeV - 1 : S.eyeV);
    const gx = R(eyes.gazeX || 0) * 2, gy = R(eyes.gazeY || 0);
    const ec = o.eyeColor || C.eye;
    const eyeAt = (side) => ({ u: cx + (side < 0 ? S.eyeL : S.eyeR) + gx, v: ev + gy });
    const shapeFor = (side) => {
      if (side < 0 && eyes.l) return eyes.l;
      if (side > 0 && eyes.r) return eyes.r;
      if (eyes.shape === 'wink') return side < 0 ? 'open' : 'happy';
      return eyes.shape;
    };
    function eye(side) {
      const e = eyeAt(side), sh = shapeFor(side), W = S.eyeW, Hh = S.eyeH;
      if (mini) {
        switch (sh) {
          case 'blink': F.rect(e.u, e.v + Hh - 2, W, 1, ec); break;
          case 'closed': case 'wince': F.rect(e.u - 1, e.v + 3, W + 2, 1, ec); break;
          case 'happy': F.sprite(e.u - 1, e.v + 2, EYE_SPR.miniHappy, { '#': ec }); break;
          case 'wide': F.rect(e.u, e.v - 1, W, Hh + 1, ec); break;
          default: F.rect(e.u, e.v, W, Hh, ec);
        }
        return;
      }
      switch (sh) {
        case 'blink': F.rect(e.u, e.v + (Hh >> 1), W, 2, ec); break;
        case 'closed': F.rect(e.u - 1, e.v + (Hh >> 1), W + 2, 2, ec); break;
        case 'happy': F.sprite(e.u - 2, e.v + ((Hh - 4) >> 1), EYE_SPR.happy, { '#': ec }); break;
        case 'wince': F.sprite(side < 0 ? e.u : e.u - 1, e.v + ((Hh - 6) >> 1), side < 0 ? EYE_SPR.winceL : EYE_SPR.winceR, { '#': ec }); break;
        case 'wide':
          F.rect(e.u, e.v - 2, W, Hh + 3, ec);
          if (o.shine !== false) { F.px(e.u + 1, e.v - 1, C.text); F.px(e.u + 1, e.v, C.text); }
          break;
        case 'open': default:
          F.rect(e.u, e.v, W, Hh, ec);
          if (o.shine) F.px(e.u + 1, e.v + 1, C.text);
      }
    }
    eye(-1); eye(+1);
    if (o.blush) {
      const bc = o.blushColor || C.blush;
      if (mini) { F.rect(cx + S.eyeL - 2, ev + S.eyeH, 2, 1, bc); F.rect(cx + S.eyeR + S.eyeW, ev + S.eyeH, 2, 1, bc); }
      else { F.rect(cx + S.eyeL - 3, ev + S.eyeH + 1, 4, 2, bc); F.rect(cx + S.eyeR + S.eyeW - 1, ev + S.eyeH + 1, 4, 2, bc); }
    }
    let mouthPt = null;
    if (mouth !== 'none' && !mini) {
      const spr = MOUTH_SPR[mouth] || MOUTH_SPR.o;
      const mw = spr[0].length, mv = ev + S.eyeH + 1;
      const mu = cx - (mw >> 1);
      F.sprite(mu, mv, spr, { '#': ec });
      if (MOUTH_FILL[mouth]) F.sprite(mu, mv, MOUTH_FILL[mouth], { '#': C.red });
      mouthPt = F.at(mu + (mw >> 1), mv);
    } else if (mouth !== 'none' && mini) {
      F.rect(cx - 1, ev + S.eyeH + 1, 2, mouth === 'o' || mouth === 'laugh' ? 2 : 1, ec);
    }

    if (backG.front) drawArm(backG);
    drawArm(frontG);

    // anchors
    const handPt = (g) => (g.hand ? scr(g.hand.x, g.hand.y, g.hand.z) : null);
    const head = scr(x0 + FWb / 2, D / 2, zb + H);
    const res = {
      hand: { l: handPt(geomL), r: handPt(geomR) },
      head: { x: head.x, y: head.y - Math.floor((S.FW + D) / 2) + 1 },   // top of the sprite, centred
      top: head,                                                         // centre of the top face
      face: F.at(cx, Math.floor(H / 2)),
      eyes: { l: F.at(eyeAt(-1).u + (S.eyeW >> 1), eyeAt(-1).v), r: F.at(eyeAt(1).u + (S.eyeW >> 1), eyeAt(1).v) },
      mouth: mouthPt || F.at(cx, ev + S.eyeH + 1),
      feet: { x, y },
      bbox: { x: body.x - 2 * S.armL - 2, y: body.y - 2, w: body.w + 4 * S.armL + 4, h: body.h + S.legH + 4 },
      facePainter: F,
    };
    return res;
  };

  // ------------------------------------------------------------------ desk
  Iso.DESK = { x: 120, y: 74, a: 44, b: 28, thick: 7 };
  function deskPt(d, u, v) {
    const ox = d.x - (d.a - d.b), oy = d.y - Math.floor((d.a + d.b) / 2);
    return Iso.w2s(ox, oy, u * d.a, v * d.b, 0);
  }
  // Named spots on the default desk (screen coords of points on the desk TOP surface).
  Iso.SPOT = {};
  function computeSpots() {
    // Hand-picked free areas of the default desk (screen coords, on the desk top surface).
    Object.assign(Iso.SPOT, {
      center: { x: 120, y: 74 },     // desk top centre
      clawd: { x: 120, y: 76 },      // default Clawd feet point
      backLeft: { x: 84, y: 56 },    // behind-left of Clawd  -> draw BEFORE Clawd
      backRight: { x: 148, y: 66 },  // behind-right of Clawd -> draw BEFORE Clawd
      left: { x: 74, y: 70 },        // left wing of the desk (beside Clawd)
      right: { x: 166, y: 78 },      // right wing of the desk (beside Clawd)
      frontLeft: { x: 100, y: 88 },  // in front-left  -> draw AFTER Clawd
      frontRight: { x: 148, y: 92 }, // in front-right -> draw AFTER Clawd
      front: { x: 128, y: 100 },     // front corner area -> draw AFTER Clawd
    });
  }
  computeSpots();
  Iso.deskPoint = (u, v, d) => { const p = deskPt(d || Iso.DESK, u, v); return { x: R(p.x), y: R(p.y) }; };

  // Draw the desk. opts: {x, y, a, b, thick, pal, grain:true, mat:false|color}. (x, y) = screen point
  // of the CENTER of the desk's TOP surface. Returns {pt(u,v), top painter, spots}.
  Iso.drawDesk = function (ctx, opts) {
    const d = Object.assign({}, Iso.DESK, opts || {});
    const pal = d.pal || SH.wood;
    const ox = d.x - (d.a - d.b), oy = d.y - Math.floor((d.a + d.b) / 2);
    const r = Iso.boxAt(ctx, ox, oy, -d.thick, d.a, d.b, d.thick, pal);
    if (d.grain !== false) {
      const g = C.grain;
      // a few long grain streaks along x
      const streaks = [[3, 6, 14], [7, 20, 12], [12, 2, 9], [16, 26, 13], [21, 10, 16], [24, 30, 9]];
      for (const [j, i0, n] of streaks) for (let i = i0; i < i0 + n && i < d.a - 1; i++) if (j < d.b - 1) r.top.cell(i, j, g);
      // lighter front-edge highlight on the side faces' top row is pal.edge (drawn by boxAt)
    }
    if (d.mat) {
      const mc = typeof d.mat === 'string' ? d.mat : C.surface;
      for (let i = 10; i < 34 && i < d.a - 2; i++) for (let j = 6; j < 22 && j < d.b - 2; j++) r.top.cell(i, j, mc);
    }
    return { pt: (u, v) => deskPt(d, u, v), top: r.top, desk: d };
  };

  // ------------------------------------------------------------------ props
  const prop = Iso.prop = {};

  // Screen content painters for laptop/monitor screens. p = face painter, area = {u, v, w, h}.
  function screenContent(p, A, kind, frame, accent) {
    frame = frame || 0;
    const L = (row, u0, len, c) => { if (row < A.h && len > 0) p.rect(A.u + u0, A.v + row, Math.min(len, A.w - u0), 1, c); };
    if (kind === 'off' || kind === 'blank') return;
    if (kind === 'search') {
      L(1, 2, A.w - 4, C.paper);
      L(4, 2, 8, C.blueLight); L(6, 2, A.w - 8, C.muted); L(8, 2, 8 + (frame % 3) * 2, C.blueLight);
      return;
    }
    if (kind === 'plan') {
      for (let r = 0; r < 4; r++) { L(1 + r * 2, 1, 2, r < (frame % 5) ? C.green : C.dim); L(1 + r * 2, 4, 6 + ((r * 5) % 7), C.muted); }
      return;
    }
    // 'code' (default): scrolling indented lines
    const rows = [[0, 6, C.violet], [2, 9, C.text], [2, 5, C.amber], [4, 7, C.blueLight], [2, 10, C.text], [0, 3, C.violet], [2, 6, C.green], [4, 8, C.text]];
    for (let r = 0; r * 2 + 1 < A.h; r++) {
      const q = rows[(r + frame) % rows.length];
      L(1 + r * 2, 1 + q[0], q[1], accent && r === 0 ? accent : q[2]);
    }
  }
  Iso.screenContent = screenContent;

  // Laptop. (x, y) = footprint centre on the desk. opts: {facing:'left'|'right'|'upLeft'|'upRight',
  // screen:'code'|'search'|'plan'|'blank'|'off'|function(painter, area, frame), frame, glow:true, z}
  // facing = direction the SCREEN faces. 'upLeft'/'upRight' show the lid's back (Clawd sits behind it).
  // Returns {screen: painter|null, area, lid, base}.
  prop.laptop = function (ctx, x, y, opts) {
    opts = opts || {};
    const A = 11, B = 7, o = Obj(ctx, x, y, A, B, opts.facing || 'left', opts.z);
    o.box(0, 0, 0, A, B, 2, SH.metal, 'base');
    o.box(0, 0, 2, A, 1, 10, { top: C.muted, left: C.grey, right: C.greyDark }, 'lid');
    const r = o.draw();
    // keyboard on the base
    for (let i = 1; i < A - 1; i++) for (let j = 2; j < B - 1; j++) if ((i + j) % 2 === 0) {
      const p = o.pt(i + 0.5, j + 0.5, 2);
      rect(ctx, p.x - 1, p.y, 2, 1, C.greyDark);
    }
    let area = null, scr = null;
    const lid = r.lid;
    if (lid.front) {
      scr = lid.front;
      area = { u: 2, v: 1, w: scr.cols - 4, h: 8 };
      scr.rect(area.u, area.v, area.w, area.h, opts.screen === 'off' ? C.surface : C.screen);
      if (typeof opts.screen === 'function') opts.screen(scr, area, opts.frame || 0);
      else screenContent(scr, area, opts.screen || 'code', opts.frame || 0);
    } else if (lid.back) {
      // lid back: small glowing logo (a tiny Clawd mark)
      const b = lid.back, mu = (b.cols >> 1) - 2;
      b.rect(mu, 3, 4, 3, C.muted);
      b.px(mu + 1, 4, C.greyDark); b.px(mu + 2, 4, C.greyDark);
      if (opts.glow !== false) {
        // glow spilling over the lid top edge
        const t0 = b.at(0, -1), t1 = b.at(b.cols - 1, -1);
        Iso.line(ctx, t0.x, t0.y, t1.x, t1.y, C.glow);
      }
    }
    return { screen: scr, area, lid, base: r.base };
  };

  // Desktop monitor on a stand. Same opts/return as laptop (screen area is bigger).
  prop.monitor = function (ctx, x, y, opts) {
    opts = opts || {};
    const A = 12, B = 5, o = Obj(ctx, x, y, A, B, opts.facing || 'left', opts.z);
    o.box(4, 1, 0, 4, 3, 1, SH.dark, 'foot');
    o.box(5, 1, 1, 2, 1, 6, SH.dark, 'neck');
    o.box(0, 2, 5, A, 1, 14, { top: C.border, left: C.surface2, right: C.surface }, 'panel');
    const r = o.draw();
    let area = null, scr = null;
    if (r.panel.front) {
      scr = r.panel.front;
      area = { u: 2, v: 1, w: scr.cols - 4, h: 11 };
      scr.rect(area.u, area.v, area.w, area.h, opts.screen === 'off' ? C.surface : C.screen);
      if (typeof opts.screen === 'function') opts.screen(scr, area, opts.frame || 0);
      else screenContent(scr, area, opts.screen || 'code', opts.frame || 0);
    } else if (r.panel.back && opts.glow !== false) {
      const b = r.panel.back, t0 = b.at(0, -1), t1 = b.at(b.cols - 1, -1);
      Iso.line(ctx, t0.x, t0.y, t1.x, t1.y, C.glow);
    }
    return { screen: scr, area, panel: r.panel };
  };

  // Sheet of paper lying flat. opts: {lines:true, a:6, b:8, color}
  prop.paper = function (ctx, x, y, opts) {
    opts = opts || {};
    const a = opts.a || 6, b = opts.b || 8;
    const r = Iso.box(ctx, x, y, opts.z || 0, a, b, 1, { top: opts.color || C.paper, left: C.paperShade, right: C.muted });
    if (opts.lines !== false) for (let j = 1; j < b - 1; j += 1) {
      const n = j === 1 ? a - 3 : a - 2 - ((j * 3) % 3);
      for (let i = 1; i < 1 + n; i++) if (j % 2 === 1) r.top.cell(i, j, C.muted);
    }
    return r;
  };
  prop.doc = prop.paper;

  // Closed book. opts: {color:'blue'|'violet'|... shade name or {top,left,right}, open:false}
  prop.book = function (ctx, x, y, opts) {
    opts = opts || {};
    if (opts.open) {
      const r = Iso.box(ctx, x, y, opts.z || 0, 10, 7, 2, { top: C.paper, left: C.paperShade, right: C.muted });
      for (let j = 0; j < 7; j++) r.top.cell(5, j, C.paperShade);
      for (let j = 1; j < 6; j += 2) { for (let i = 1; i < 4; i++) r.top.cell(i, j, C.muted); for (let i = 6; i < 9; i++) r.top.cell(i, j, C.muted); }
      return r;
    }
    const s = typeof opts.color === 'object' ? opts.color : SH[opts.color || 'blue'];
    const r = Iso.box(ctx, x, y, opts.z || 0, 6, 8, 3, { top: s.top, left: C.paper, right: s.right });
    for (let i = 0; i < 6; i++) r.top.cell(i, 0, s.left);  // spine
    r.left.rect(0, 1, r.left.cols, 1, C.paperShade);
    return r;
  };

  // Notepad (paper with an amber header). opts: {lines:true}
  prop.notepad = function (ctx, x, y, opts) {
    opts = opts || {};
    const r = Iso.box(ctx, x, y, opts.z || 0, 5, 7, 2, { top: C.paper, left: C.paperShade, right: C.muted });
    for (let i = 0; i < 5; i++) r.top.cell(i, 0, C.amber);
    if (opts.lines !== false) for (let j = 2; j < 6; j += 2) for (let i = 1; i < 4; i++) r.top.cell(i, j, C.paperShade);
    return r;
  };

  // Pencil lying flat (or held). dir: 'x' (runs down-right) | 'y' (runs down-left). Tip at the far end.
  // len: body length in cells (default 5). Anchor = centre.
  prop.pencil = function (ctx, x, y, opts) {
    opts = opts || {};
    const len = opts.len || 5, dir = opts.dir || 'x', n = len + 3;
    const sx = dir === 'x' ? 2 : -2;
    const x0 = x - (sx * n) / 2, y0 = y - n / 2;
    for (let i = 0; i < n; i++) {
      const c = i === 0 ? C.blush : i === n - 1 ? C.eye : i === n - 2 ? C.paperShade : C.amber;
      const X = x0 + sx * i - (sx < 0 ? 2 : 0), Y = y0 + i;
      rect(ctx, X, Y, 2, 1, c);
      if (i < n - 1) rect(ctx, X, Y + 1, 2, 1, i === 0 ? '#C9786A' : i === n - 2 ? C.muted : C.amberDark);
    }
  };

  // Coffee mug with optional steam. opts: {frame, steam:true, color: shade name|obj}
  prop.mug = function (ctx, x, y, opts) {
    opts = opts || {};
    const s = typeof opts.color === 'object' ? opts.color : SH[opts.color || 'paper'];
    const r = Iso.box(ctx, x, y, opts.z || 0, 4, 4, 8, { top: s.top, left: s.left, right: s.right });
    // coffee
    Iso.box(ctx, x, y + 1, (opts.z || 0) + 8, 2, 2, 0, { top: C.woodRight });
    // handle on the right face
    r.right.rect(r.right.cols - 2, 1, 2, 1, s.right);
    const hp = r.right.at(r.right.cols - 1, 1);
    rect(ctx, hp.x + 1, hp.y, 1, 4, s.right); rect(ctx, hp.x, hp.y + 3, 1, 1, s.right);
    // steam (4-frame cycle)
    if (opts.steam !== false) {
      const f = (opts.frame || 0) % 4;
      const top = r.top.at(2, 0);
      const wisp = [[0, 0], [1, -1], [1, -2], [0, -3], [-1, -4], [-1, -5], [0, -6]];
      for (let k = 0; k < 2; k++) {
        const off = (f + k * 2) % 4;
        for (let i = 0; i < 4; i++) {
          const w = wisp[(i + off) % wisp.length];
          const yy = top.y - 3 - i * 2 - off, xx = top.x - 1 + k * 2 + w[0];
          if (i + off < 6) rect(ctx, xx, yy, 1, 1, i > 2 ? C.dim : C.muted);
        }
      }
    }
    return r;
  };

  // Magnifying glass (2D sprite, for holding). (x, y) = lens centre. opts: {handle:'dr'|'dl', glint:frame}
  prop.magnifier = function (ctx, x, y, opts) {
    opts = opts || {};
    const rows = [
      '..rrr..',
      '.rgggr.',
      'rgwgggr',
      'rgggggr',
      'rgggggr',
      '.rgggr.',
      '..rrr..',
    ];
    const flip = opts.handle === 'dl';
    Iso.sprite(ctx, x - 3, y - 3, rows, { r: C.muted, g: C.blueLight, w: C.text }, flip);
    // handle: 2:1 diagonal
    const hx = flip ? x - 3 : x + 3, hy = y + 3;
    for (let i = 0; i < 3; i++) rect(ctx, flip ? hx - 2 - 2 * i : hx + 2 * i, hy + i, 2, 2, C.woodLeft);
  };

  // Sticky note. opts: {color: 'amber'|'green'|'blush'|'blue'|hex, plane:'top'|'L'|'R', z, mark:true}
  prop.sticky = function (ctx, x, y, opts) {
    opts = opts || {};
    const col = (opts.color && C[opts.color]) || opts.color || C.amber;
    const plane = opts.plane || 'top';
    if (plane === 'top') {
      const r = Iso.box(ctx, x, y, opts.z || 0, 3, 3, 0, { top: col });
      if (opts.mark !== false) { r.top.cell(1, 1, C.eye); }
      return r;
    }
    const p = Iso.plane(ctx, x, y, opts.z || 0, 3, 6, plane, col);
    if (opts.mark !== false) { p.rect(1, 2, 4, 1, C.dim); p.rect(1, 4, 3, 1, C.dim); }
    return p;
  };

  // Floating window panel (terminal / browser). (x, y) = screen point under the window's bottom-centre,
  // z = height of its bottom edge above that point. opts.plane: 'L' | 'R'. Returns painter + body area.
  function windowPanel(ctx, x, y, opts) {
    const len = opts.len || 14, h = opts.h || 18, plane = opts.plane || 'L';
    const p = Iso.plane(ctx, x, y, opts.z == null ? 10 : opts.z, len, h, plane, opts.bg || C.bg);
    // frame
    p.rect(0, 0, p.cols, 3, C.surface2);
    p.rect(0, 3, 1, h - 3, C.surface2); p.rect(p.cols - 1, 3, 1, h - 3, C.surface2); p.rect(0, h - 1, p.cols, 1, C.surface2);
    p.px(2, 1, C.red); p.px(4, 1, C.amber); p.px(6, 1, C.green);
    return { p, area: { u: 2, v: 4, w: p.cols - 4, h: h - 6 } };
  }
  // Terminal window. opts: {plane:'L'|'R', len:14, h:18, z:10, lines:[[indent, length, color]...],
  // reveal: number of columns typed so far (undefined = all), cursor: true|false, frame}
  prop.terminal = function (ctx, x, y, opts) {
    opts = opts || {};
    const { p, area } = windowPanel(ctx, x, y, opts);
    const lines = opts.lines || [[0, 8, C.green], [2, 12, C.text], [2, 9, C.muted], [0, 6, C.green], [2, 14, C.text]];
    let budget = opts.reveal == null ? 1e9 : opts.reveal;
    let lastRow = 0, lastU = area.u;
    for (let r = 0; r < lines.length && r * 2 < area.h; r++) {
      const [ind, len, col] = lines[r];
      const v = area.v + r * 2;
      // prompt chevron on lines with indent 0
      if (ind === 0) p.px(area.u, v, C.orange);
      const u0 = area.u + (ind === 0 ? 2 : ind);
      const n = Math.max(0, Math.min(len, budget, area.w - (u0 - area.u)));
      if (n > 0) p.rect(u0, v, n, 1, col);
      budget -= len;
      lastRow = v; lastU = u0 + n;
      if (budget <= 0) break;
    }
    if (opts.cursor !== false && ((opts.frame || 0) >> 2) % 2 === 0) p.rect(Math.min(lastU + 1, area.u + area.w - 1), lastRow - 1, 1, 2, C.text);
    return { painter: p, area };
  };
  // Browser window. opts as terminal + {reveal (rows loaded), frame (spinner)}
  prop.browser = function (ctx, x, y, opts) {
    opts = Object.assign({ bg: C.surface }, opts || {});
    const { p, area } = windowPanel(ctx, x, y, opts);
    p.rect(area.u, area.v - 0, area.w, 2, C.paper);          // address bar
    p.px(area.u + 1, area.v, C.blue);
    const rows = opts.reveal == null ? 99 : opts.reveal;
    const body = [[C.blue, area.w - 6], [C.muted, area.w - 2], [C.muted, area.w - 5], [C.green, 6], [C.muted, area.w - 3]];
    for (let r = 0; r < body.length && r < rows; r++) {
      const v = area.v + 3 + r * 2;
      if (v >= area.v + area.h) break;
      p.rect(area.u, v, body[r][1], 1, body[r][0]);
    }
    if (opts.reveal != null && opts.reveal < body.length) {
      const f = (opts.frame || 0) % 4, sp = [[0, 0], [1, 0], [1, 1], [0, 1]][f];
      p.rect(area.u + area.w - 3 + sp[0], area.v + sp[1], 1, 1, C.orange);
    }
    return { painter: p, area };
  };

  // Desk globe. (x, y) = base centre on the desk. opts: {frame} (8-frame spin)
  const LAND = [
    '..##......###...',
    '.####....#####..',
    '..###.....###.#.',
    '...##......#....',
    '....#.....##....',
    '...........#....',
  ];
  prop.globe = function (ctx, x, y, opts) {
    opts = opts || {};
    Iso.box(ctx, x, y, 0, 3, 3, 2, SH.wood);
    rect(ctx, x, y - 6, 1, 4, C.woodLeft);
    const cx = x, cy = y - 12, r = 5.5, f = (opts.frame || 0) % 16;
    for (let j = -5; j <= 5; j++) for (let i = -5; i <= 5; i++) {
      const d = Math.hypot(i + 0.5 - 0.5, j);
      if (d > r) continue;
      const lon = Math.round(((Math.asin(Math.max(-1, Math.min(1, i / Math.max(1, Math.sqrt(r * r - j * j))))) / Math.PI) * 8) + 8 + f) % 16;
      const lat = Math.min(5, Math.floor((j + 5) / 2));
      const land = LAND[lat][(lon + 16) % 16] === '#';
      let c = land ? C.green : C.blue;
      if (i >= 3 || (i >= 2 && Math.abs(j) >= 3)) c = land ? C.greenDark : C.blueDark;
      if (i <= -3 && j <= -2 && !land) c = C.blueLight;
      rect(ctx, cx + i, cy + j, 1, 1, c);
    }
    // meridian ring
    rect(ctx, cx + 6, cy - 2, 1, 6, C.woodEdge); rect(ctx, cx + 5, cy + 4, 1, 2, C.woodEdge);
  };

  // Plan board on an easel (standing). opts: {facing:'left'|'right', items:4, checked:0..items, frame}
  prop.planBoard = function (ctx, x, y, opts) {
    opts = opts || {};
    const o = Obj(ctx, x, y, 12, 4, opts.facing || 'left', opts.z);
    o.box(1, 1, 0, 1, 1, 14, SH.wood, 'legL');
    o.box(10, 1, 0, 1, 1, 14, SH.wood, 'legR');
    o.box(0, 2, 4, 12, 1, 17, { top: C.woodEdge, left: C.paper, right: C.woodRight }, 'board');
    const r = o.draw();
    const p = r.board.front || r.board.back;
    if (!r.board.front) return { painter: p };
    const n = opts.items || 4, k = opts.checked || 0;
    p.rect(0, 0, p.cols, 1, C.woodEdge);
    for (let i = 0; i < n; i++) {
      const v = 3 + i * 3;
      p.rect(2, v, 2, 2, i < k ? C.green : C.paperShade);
      p.rect(6, v, 10 + ((i * 7) % 6), 1, i < k ? C.paperShade : C.muted);
    }
    return { painter: p };
  };

  // Clipboard lying flat. opts: {checked: n}
  prop.clipboard = function (ctx, x, y, opts) {
    opts = opts || {};
    const r = Iso.box(ctx, x, y, opts.z || 0, 6, 8, 1, SH.wood);
    for (let i = 1; i < 5; i++) for (let j = 1; j < 7; j++) r.top.cell(i, j, C.paper);
    r.top.cell(2, 0, C.muted); r.top.cell(3, 0, C.muted);
    for (let j = 2; j < 7; j += 2) { r.top.cell(1, j, j / 2 <= (opts.checked || 0) ? C.green : C.paperShade); r.top.cell(3, j, C.muted); }
    return r;
  };

  // Desk lamp. opts: {facing:'left'|'right', on:true}
  prop.lamp = function (ctx, x, y, opts) {
    opts = opts || {};
    const right = opts.facing === 'right';
    const on = opts.on !== false;
    if (on) {
      // light pool on the desk in front of the lamp (dithered)
      const lp = { x: x + (right ? 14 : -14), y: y + 6 };
      for (let j = -3; j <= 3; j++) {
        const w = 18 - Math.abs(j) * 4;
        Iso.dither(ctx, lp.x - w / 2, lp.y + j, w, 1, C.woodEdge, 0.5);
      }
    }
    Iso.box(ctx, x, y, 0, 4, 4, 1, SH.dark);
    rect(ctx, x, y - 14, 1, 13, C.surface2);                 // post
    const s = right ? 1 : -1;
    for (let i = 0; i < 5; i++) rect(ctx, x + s * (1 + 2 * i) - (s < 0 ? 1 : 0), y - 14 + i, 2, 1, C.surface2); // arm
    const hx = x + s * 11, hy = y - 9;
    Iso.sprite(ctx, hx - 3, hy - 2, ['.###.', '#####', '#####'], { '#': C.border }, right);
    if (on) { rect(ctx, hx - 1, hy + 1, 3, 1, C.amber); Iso.px(ctx, hx, hy + 2, '#F7D885'); }
  };

  // Ground shadow helper (flat diamond).
  prop.shadow = (ctx, x, y, a, b, c) => Iso.tile(ctx, x, y, a, b, c || C.woodShadow);

  // ------------------------------------------------------------------ FX (screen-space sprites)
  const fx = Iso.fx = {};
  const SPARK = [
    ['#'],
    ['.#.', '###', '.#.'],
    ['..#..', '..#..', '#####', '..#..', '..#..'],
    ['...#...', '...#...', '..###..', '#######', '..###..', '...#...', '...#...'],
  ];
  // Sparkle centred at (x, y). size 0..3.
  fx.sparkle = (ctx, x, y, size, c) => {
    const s = SPARK[Math.max(0, Math.min(3, size == null ? 2 : size))];
    Iso.sprite(ctx, x - (s[0].length >> 1), y - (s.length >> 1), s, { '#': c || C.amber });
  };
  // "!" \u2014 badge:true draws an amber rounded badge with a dark "!"; else a plain amber "!". Centered.
  fx.bang = (ctx, x, y, opts) => {
    opts = opts || {};
    const c = opts.color || C.amber;
    if (opts.badge === false) return Iso.sprite(ctx, x - 1, y - 4, ['##', '##', '##', '##', '..', '##'], { '#': c });
    Iso.sprite(ctx, x - 3, y - 4, ['.aaaaa.', 'aaadaaa', 'aaadaaa', 'aaadaaa', 'aaadaaa', 'aaaaaaa', 'aaadaaa', '.aaaaa.', '..aa...', '..a....'],
      { a: c, d: C.eye });
  };
  fx.question = (ctx, x, y, opts) => {
    opts = opts || {};
    const c = opts.color || C.blue;
    if (opts.badge === false) return Iso.sprite(ctx, x - 2, y - 4, ['####.', '...##', '..##.', '.##..', '.....', '.##..'], { '#': c });
    Iso.sprite(ctx, x - 3, y - 4, ['.aaaaa.', 'aadddaa', 'aaaaada', 'aaaddaa', 'aaadaaa', 'aaaaaaa', 'aaadaaa', '.aaaaa.', '..aa...', '..a....'],
      { a: c, d: C.eye });
  };
  const ZEES = [['###', '.#.', '###'], ['####', '..#.', '.#..', '####'], ['#####', '...#.', '..#..', '.#...', '#####']];
  fx.zee = (ctx, x, y, size, c) => { const s = ZEES[Math.max(0, Math.min(2, (size || 1) - 1))]; Iso.sprite(ctx, x - (s[0].length >> 1), y - (s.length >> 1), s, { '#': c || C.muted }); };
  fx.sweat = (ctx, x, y) => Iso.sprite(ctx, x - 1, y - 2, ['.b.', '.b.', 'bwb', 'bbb', '.b.'], { b: C.blue, w: C.blueLight });
  fx.heart = (ctx, x, y, size, c) => {
    const s = size === 2 ? ['.##.##.', '#######', '#######', '.#####.', '..###..', '...#...'] : ['##.##', '#####', '.###.', '..#..'];
    Iso.sprite(ctx, x - (s[0].length >> 1), y - (s.length >> 1), s, { '#': c || C.red });
  };
  fx.check = (ctx, x, y, c) => Iso.sprite(ctx, x - 3, y - 2, ['.....##', '....##.', '##.##..', '.###...', '..#....'], { '#': c || C.green });
  fx.cross = (ctx, x, y, c) => Iso.sprite(ctx, x - 2, y - 2, ['##..##', '.####.', '..##..', '.####.', '##..##'], { '#': c || C.red });
  // "..." dots: n = number shown (0..3), centred.
  fx.dots = (ctx, x, y, n, c) => { for (let i = 0; i < (n == null ? 3 : n); i++) rect(ctx, x - 5 + i * 4, y, 2, 2, c || C.text); };
  // Smoke poof (spawn/vanish). frame 0..3.
  const POOF = [
    ['.##.', '####', '.##.'],
    ['..##..', '.####.', '######', '.####.', '..##..'],
    ['.##..##.', '########', '.######.', '########', '.##..##.'],
    ['#......#', '...##...', '#......#', '..#..#..', '#......#'],
  ];
  fx.poof = (ctx, x, y, frame, c) => { const s = POOF[Math.max(0, Math.min(3, frame | 0))]; Iso.sprite(ctx, x - (s[0].length >> 1), y - (s.length >> 1), s, { '#': c || C.muted }); };
  // Motion/emphasis lines around a point: 3 short ticks. dir: 'up' | 'left' | 'right'
  fx.ticks = (ctx, x, y, dir, c) => {
    c = c || C.text;
    if (dir === 'left') { rect(ctx, x - 4, y - 4, 3, 1, c); rect(ctx, x - 5, y, 3, 1, c); rect(ctx, x - 4, y + 4, 3, 1, c); }
    else if (dir === 'right') { rect(ctx, x + 2, y - 4, 3, 1, c); rect(ctx, x + 3, y, 3, 1, c); rect(ctx, x + 2, y + 4, 3, 1, c); }
    else { rect(ctx, x - 4, y - 2, 1, 2, c); rect(ctx, x, y - 4, 1, 3, c); rect(ctx, x + 4, y - 2, 1, 2, c); }
  };
  // Speech / thought puff. (x, y) = top-left, w x h outer size. opts: {kind:'speech'|'thought',
  // tail:'left'|'right'|'none' (bottom-left/right corner), fill, border}. Returns inner area {x, y, w, h}.
  fx.puff = (ctx, x, y, w, h, opts) => {
    opts = opts || {};
    const fill = opts.fill || C.surface2, bd = opts.border || C.border;
    x = R(x); y = R(y);
    rect(ctx, x + 1, y, w - 2, h, bd); rect(ctx, x, y + 1, w, h - 2, bd);
    rect(ctx, x + 1, y + 1, w - 2, h - 2, fill);
    const tail = opts.tail || 'left';
    if (tail !== 'none') {
      const tx = tail === 'left' ? x + 4 : x + w - 8;
      if (opts.kind === 'thought') {
        rect(ctx, tx + (tail === 'left' ? 0 : 2), y + h + 1, 3, 2, fill);
        rect(ctx, tx + (tail === 'left' ? -2 : 5), y + h + 4, 2, 2, fill);
      } else {
        const rows = tail === 'left' ? ['####', '###.', '##..', '#...'] : ['####', '.###', '..##', '...#'];
        Iso.sprite(ctx, tx, y + h - 1, rows, { '#': fill });
        if (tail === 'left') rect(ctx, tx - 1, y + h - 1, 1, 4, bd); else rect(ctx, tx + 4, y + h - 1, 1, 4, bd);
      }
    }
    return { x: x + 2, y: y + 2, w: w - 4, h: h - 4 };
  };

  // ------------------------------------------------------------------ timing helpers
  // seq([[value, holdFrames], ...], i) -> value active at frame i (wraps). seq.len(list) = total frames.
  Iso.seq = function (list, i) {
    const n = Iso.seq.len(list);
    let f = ((i % n) + n) % n;
    for (const [v, hold] of list) { if (f < (hold || 1)) return v; f -= hold || 1; }
    return list[list.length - 1][0];
  };
  Iso.seq.len = (list) => list.reduce((s, it) => s + (it[1] || 1), 0);
  Iso.cycle = (arr, i, hold) => arr[Math.floor(i / (hold || 1)) % arr.length];
  Iso.pingpong = (n, i) => { const p = 2 * (n - 1) || 1; const k = i % p; return k < n ? k : p - k; };
  Iso.lerp = (a, b, t) => R(a + (b - a) * t);

  // ------------------------------------------------------------------ player harness
  // Iso.player({name, fps, frames, draw(ctx, i, t), loop, notes, width, height, bg})
  // Builds the whole page UI. URL params: ?embed=1 (stage only, for the gallery), ?frame=N (paused at
  // frame N), ?zoom=1|2|4 (1 = logical, 2 = device, 4 = 2x device), ?play=0.
  Iso.player = function (cfg) {
    const W = cfg.width || Iso.W, H = cfg.height || Iso.H;
    const fps = cfg.fps || 10, N = cfg.frames || 1;
    const loop = cfg.loop !== false;
    const bg = cfg.bg || C.bg;
    const q = new URLSearchParams(location.search);
    const embed = q.has('embed');
    let zoom = +(q.get('zoom') || (embed ? 2 : 2));
    let frame = q.has('frame') ? (+q.get('frame') % N) : 0;
    let playing = !q.has('frame') && q.get('play') !== '0';
    let speed = 1;
    const name = cfg.name || document.title || 'anim';

    const canvas = document.createElement('canvas');
    canvas.width = W; canvas.height = H;
    const ctx = canvas.getContext('2d');
    ctx.imageSmoothingEnabled = false;
    const render = (i, c) => {
      c = c || ctx;
      c.save();
      c.fillStyle = bg; c.fillRect(0, 0, W, H);
      cfg.draw(c, i, i / fps);
      c.restore();
    };

    const css = document.createElement('style');
    css.textContent = `
      html,body{margin:0;background:#0F0E0D;color:${C.text};font:13px/1.45 ui-monospace,Menlo,Consolas,monospace}
      .iso-wrap{max-width:1000px;margin:0 auto;padding:16px}
      .iso-h{display:flex;align-items:baseline;gap:12px;flex-wrap:wrap;margin:0 0 10px}
      .iso-h h1{font-size:18px;margin:0;color:${C.text};font-weight:600}
      .iso-badge{color:${C.muted};border:1px solid ${C.border};border-radius:6px;padding:1px 7px;font-size:12px}
      .iso-stage{background:${bg};display:inline-block;line-height:0;outline:1px solid ${C.border};border-radius:4px;overflow:hidden;max-width:100%}
      .iso-stage canvas{image-rendering:pixelated;image-rendering:crisp-edges;display:block;max-width:100%}
      .iso-ctl{display:flex;gap:6px;flex-wrap:wrap;align-items:center;margin:10px 0}
      .iso-ctl button{background:${C.surface};color:${C.text};border:1px solid ${C.border};border-radius:6px;padding:5px 10px;font:inherit;cursor:pointer}
      .iso-ctl button:hover{background:${C.surface2}}
      .iso-ctl button.on{border-color:${C.orange};color:${C.orangeLight}}
      .iso-ctl .sep{width:10px}
      .iso-ctl .fc{color:${C.muted};min-width:110px}
      .iso-notes{color:${C.muted};max-width:760px;white-space:pre-wrap}
      .iso-notes b{color:${C.text}}
      body.embed{background:${bg};overflow:hidden}
      body.embed .iso-stage{outline:none;border-radius:0}
    `;
    document.head.appendChild(css);

    const stage = document.createElement('div');
    stage.className = 'iso-stage';
    stage.appendChild(canvas);
    const setZoom = (z) => {
      zoom = z;
      canvas.style.width = W * z + 'px';
      canvas.style.height = H * z + 'px';
      for (const b of zoomBtns) b.classList.toggle('on', +b.dataset.z === z);
    };
    const zoomBtns = [];

    const tick = { last: 0, acc: 0 };
    let fcEl = null, playBtn = null, loopEl = null;
    const update = () => {
      render(frame);
      if (fcEl) fcEl.textContent = `frame ${String(frame + 1).padStart(String(N).length, ' ')}/${N}`;
      if (playBtn) playBtn.textContent = playing ? '\u275a\u275a pause' : '\u25b6 play';
    };
    function loopFn(ts) {
      if (playing) {
        if (!tick.last) tick.last = ts;
        tick.acc += (ts - tick.last) * speed;
        const dt = 1000 / fps;
        while (tick.acc >= dt) {
          tick.acc -= dt;
          if (frame + 1 >= N) {
            if (loop) frame = 0;
            else { // one-shot: hold the last frame ~1s, then restart (so reviewers see it again)
              tick.hold = (tick.hold || 0) + 1;
              if (tick.hold > fps) { frame = 0; tick.hold = 0; }
            }
          } else frame++;
          update();
        }
      }
      tick.last = ts;
      requestAnimationFrame(loopFn);
    }

    function exportSheet() {
      const sheet = document.createElement('canvas');
      sheet.width = W * N; sheet.height = H;
      const sc = sheet.getContext('2d');
      sc.imageSmoothingEnabled = false;
      const tmp = document.createElement('canvas');
      tmp.width = W; tmp.height = H;
      const tc = tmp.getContext('2d');
      for (let i = 0; i < N; i++) { render(i, tc); sc.drawImage(tmp, i * W, 0); }
      const a = document.createElement('a');
      a.href = sheet.toDataURL('image/png');
      a.download = `${name.replace(/[^a-z0-9_-]+/gi, '-').toLowerCase()}_${N}f_${fps}fps_${W}x${H}.png`;
      document.body.appendChild(a); a.click(); a.remove();
    }

    document.title = document.title || name;
    if (embed) {
      document.body.classList.add('embed');
      document.body.appendChild(stage);
      setZoom(+(q.get('zoom') || 2));
      stage.addEventListener('click', () => { playing = !playing; update(); });
      try { window.parent.postMessage({ iso: 'ready', name, frames: N, fps, loop, href: location.href }, '*'); } catch (e) { /* ignore */ }
    } else {
      const wrap = document.createElement('div');
      wrap.className = 'iso-wrap';
      const h = document.createElement('div');
      h.className = 'iso-h';
      h.innerHTML = `<h1></h1><span class="iso-badge">${N} frames</span><span class="iso-badge">${fps} fps</span>` +
        `<span class="iso-badge">${(N / fps).toFixed(2)} s</span><span class="iso-badge" data-loop>${loop ? '\u27f3 loop' : '\u2192 one-shot'}</span>` +
        `<span class="iso-badge">${W}\u00d7${H} logical</span>`;
      h.querySelector('h1').textContent = name;
      loopEl = h.querySelector('[data-loop]');
      wrap.appendChild(h);
      wrap.appendChild(stage);
      const ctl = document.createElement('div');
      ctl.className = 'iso-ctl';
      const btn = (label, fn, title) => { const b = document.createElement('button'); b.textContent = label; if (title) b.title = title; b.onclick = fn; ctl.appendChild(b); return b; };
      const sep = () => { const s = document.createElement('span'); s.className = 'sep'; ctl.appendChild(s); };
      playBtn = btn('\u25b6 play', () => { playing = !playing; tick.acc = 0; update(); }, 'space');
      btn('\u25c0', () => { playing = false; frame = (frame - 1 + N) % N; update(); }, 'left arrow');
      btn('\u25b6', () => { playing = false; frame = (frame + 1) % N; update(); }, 'right arrow');
      fcEl = document.createElement('span'); fcEl.className = 'fc'; ctl.appendChild(fcEl);
      sep();
      const spBtns = [];
      for (const s of [0.5, 1, 2]) {
        const b = btn(s + 'x', () => { speed = s; spBtns.forEach((x) => x.classList.toggle('on', x === b)); }, 'speed');
        b.classList.toggle('on', s === 1); spBtns.push(b);
      }
      sep();
      for (const [z, label] of [[1, '1x logical'], [2, 'device 480\u00d7242'], [4, '2x device']]) {
        const b = btn(label, () => setZoom(z)); b.dataset.z = z; zoomBtns.push(b);
      }
      sep();
      btn('\u2b07 Export spritesheet PNG', exportSheet);
      wrap.appendChild(ctl);
      if (cfg.notes) {
        const n = document.createElement('p');
        n.className = 'iso-notes';
        n.innerHTML = cfg.notes;
        wrap.appendChild(n);
      }
      const back = document.createElement('p');
      back.innerHTML = `<a href="../index.html" style="color:${C.muted}">\u2190 gallery</a> \u00b7 keys: space play/pause, \u2190/\u2192 step, 1/2/3 zoom`;
      back.style.color = C.dim;
      wrap.appendChild(back);
      document.body.appendChild(wrap);
      setZoom(zoom);
      window.addEventListener('keydown', (e) => {
        if (e.key === ' ') { playing = !playing; e.preventDefault(); update(); }
        else if (e.key === 'ArrowLeft') { playing = false; frame = (frame - 1 + N) % N; update(); }
        else if (e.key === 'ArrowRight') { playing = false; frame = (frame + 1) % N; update(); }
        else if (e.key === '1') setZoom(1); else if (e.key === '2') setZoom(2); else if (e.key === '3') setZoom(4);
      });
    }
    update();
    requestAnimationFrame(loopFn);
    const api = { canvas, ctx, render, exportSheet, get frame() { return frame; }, set frame(v) { frame = ((v % N) + N) % N; update(); },
      play() { playing = true; update(); }, pause() { playing = false; update(); } };
    void loopEl;
    return api;
  };
})();
