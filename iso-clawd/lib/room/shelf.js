/* room/shelf.js — the shelf high on the left wall: plank, brackets, books, the plant
 * One scene item, owned by one reviewer. Attaches to window.Room (loaded after lib/room.js).
 * Geometry helpers: Room.B (floor back corner, world origin), Room.w2s(wx, wy, wz) world -> screen, Iso.* drawing.
 * Reference: mocks/r_final/scene_<light>.png. Check with: host/.venv/bin/python tools/room_item.py shelf
 *
 * Layout (measured on the reference, logical px): a thick plank on the left wall, its bottom front edge on
 * screen y = 81.5 - x/2, right tip at x = 57, running out past the left screen edge (the mock is a bit shallower,
 * ~0.43 slope; this iso plank splits the difference). On it, from the left:
 * 4 chunky books (red, yellow, green, magenta) whose big covers face the room, each set a little further right
 * and up than the one in front (overlapping like the mock, which is not strictly isometric), then a terracotta
 * pot with a bushy plant near the right end. Two dark brackets under the plank.
 * Base colours only: room/light.js lights the frame afterwards.
 */
(function () {
  'use strict';
  const Iso = window.Iso, Room = window.Room;
  const W = Room.w2s;

  // plank: top at z = Z, TH px thick, DEP deep, from y = Y0 (right end) for LEN units
  // placement Room.SHELF = {z, y0, len} comes from room/layout.js; books, pot and brackets are placed relative to y0
  const TH = 5, DEP = 4;
  // reference (lit): top #3C2616 (dark, mostly in the books' shade), front #61432A with a lighter lip; base = lit / ~0.8
  const WOOD = { top: '#472C18', left: '#3E2616', right: '#7B5333', edge: '#8E6440', lip: '#866148' };
  const BRACKET = { top: '#2A1D15', left: '#33241A', right: '#1E1510' };

  // books, drawn back (right) to front (left): [y, thickness (units), height px, right face, left face, top border]
  // Each book is BOOK_D deep (x, out from the wall). The right face (+x, toward the room/window) is the bright one,
  // the left face (+y) the dark one, like the mock; the top shows a pale page block. Magenta shows its big magenta
  // left face above the green one and an orange right face. (y is relative to Room.SHELF.y0)
  // Screen spans (logical): red 5..15, yellow 15..19, green 21..25, magenta 21..31 (mock: 5..15, 15..20, 21..24, 20..31)
  const BOOKS = [
    [12, 2, 17, '#D3604A', '#963A5C', '#A84466'],   // magenta (left face) with the orange right face
    [15, 2, 15, '#289231', '#225D1C', '#2F7A2E'],   // green
    [18, 2, 15, '#D8B547', '#7D5D24', '#B8973C'],   // yellow
    [20, 2, 15, '#B94045', '#632320', '#8E3034'],   // red, frontmost
  ];
  const PAGES = '#D6CEBC', BOOK_D = 3;

  function book(ctx, [by, bw, bh, right, left, rim]) {
    const q = W(0, Room.SHELF.y0 + by, 0);          // against the wall; BOOK_D deep, the plank shows in front
    const b = Iso.boxAt(ctx, q.x, q.y, Room.SHELF.z, BOOK_D, bw, bh, { top: PAGES, left, right });
    for (let i = 0; i < BOOK_D; i++) b.top.cell(i, 0, rim);          // back cover's edge on top, pages in front of it
    const nr = 2 * bw, nl = 2 * BOOK_D;
    b.right.rect(0, 0, nr, 1, Room.mix(right, '#FFFFFF', 3));          // top edge catches the light
    b.right.rect(nr - 1, 1, 1, bh - 1, Room.mix(right, '#000000', 3)); // far edge
    b.right.rect(0, bh - 1, nr, 1, Room.mix(right, '#000000', 4));     // foot
    b.left.rect(0, 0, nl, 1, Room.mix(left, '#FFFFFF', 3));            // cover top edge
    b.left.rect(0, bh - 1, nl, 1, Room.mix(left, '#000000', 4));       // foot
  }

  function shelf(ctx) {
    const Z = Room.SHELF.z, Y0 = Room.SHELF.y0, LEN = Room.SHELF.len;
    // brackets first (the plank covers their tops): small dark wedges against the wall
    for (const by of [Y0 + 19, Y0 + LEN + 3]) {
      const q = W(0, by, 0);
      Iso.boxAt(ctx, q.x, q.y, Z - TH - 5, 2, 1, 5, BRACKET);
    }
    // plank: dark top, warm front face with a lighter rim along the top edge and a lighter bottom lip
    const p = W(0, Y0, 0);
    const pl = Iso.boxAt(ctx, p.x, p.y, Z - TH, DEP, LEN, TH, WOOD);
    pl.right.rect(0, 0, 2 * LEN, 1, WOOD.edge);
    pl.right.rect(0, TH - 1, 2 * LEN, 1, WOOD.lip);
    for (let i = 0; i < DEP; i++) pl.top.cell(i, LEN - 1, Room.mix(WOOD.top, WOOD.edge, 6));   // rim along the right end too

    for (const bk of BOOKS) book(ctx, bk);

    // terracotta pot near the right end (a 3x3 cube), dark soil on top, a lighter rim band
    const pp = W(0, Y0 + 4, 0);                         // pot at x ~35..47 logical, like the reference
    const POT = { top: '#3A2416', left: '#6E301E', right: '#A84B31' };
    const pot = Iso.boxAt(ctx, pp.x, pp.y, Z, 3, 3, 9, POT);
    pot.left.rect(0, 0, 6, 2, '#8A4028');
    pot.right.rect(0, 0, 6, 2, '#C2603E');
    pot.left.rect(0, 2, 6, 1, '#5A2618');               // shadow line under the rim
    pot.right.rect(0, 2, 6, 1, '#8C3C26');

    // the plant: a round, bushy clump of broad leaves over the pot (screen-space sprite, light from the upper left)
    const c = pot.top.at(1.5, 1.5);
    Iso.sprite(ctx, c.x - 7, c.y - 10, [
      '.....ll...l....',
      '..llllg.llll...',
      '.lllllgglllgg..',
      'lllllgggllggggg',
      '.lllggggdlgggdg',
      'llgggdggggggddg',
      '.gggddgggdggdd.',
      'lggdgdggddgddd.',
      '.gddggddgdddd..',
      '..dd.ddddddd...',
      '....dd..dd.....',
    ], { l: '#58BE48', g: '#329A2E', d: '#21701F' });
  }

  Room.part.shelf = shelf;
})();
