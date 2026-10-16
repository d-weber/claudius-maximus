# Animator instructions (shared by all animation agents)

You build ONE isometric pixel-art animation page of Clawd, a friendly, funny desk companion.
Work ONLY on your own file `iso-clawd/anims/NN-name.html`, plus screenshots in `iso-clawd/shots/NN-*.png`.
Do NOT edit lib/iso.js, index.html, README.md, other animations, or anything outside iso-clawd/.
If you need helpers, define them inside your own page.

## Read first
1. iso-clawd/BRIEF.md: tone, palette, constraints.
2. iso-clawd/README.md: full lib API, coordinates and gotchas. Follow the gotchas strictly.
3. iso-clawd/anims/_template.html: copy this structure.
4. iso-clawd/anims/00-model-sheet.html and iso-clawd/shots/model-sheet.png: the canonical look of Clawd and the props.
5. For context, firmware/src/screens/home_face.cpp shows how the flat 2D version animates today (read only).

## Key facts
- The logical canvas is 240x121, shown at 2x (480x242).
- The desk-top centre is at (120,74). Clawd's default feet are at Iso.SPOT.clawd (120,76).
- There is no depth sort. Draw in this order: desk, back props, Clawd, front props, FX.
- `draw(ctx, i, t)` must be a pure function of the frame index i.
- Use lib primitives only: integer pixels, no arc, alpha or transforms.
- Use 8-12 fps, with holds via Iso.seq.
- Loops run 1-4 s; one-shots run 0.7-2.5 s and end on the neutral pose.
- `Iso.player({name, fps, frames, draw, loop, notes})` builds the review UI. Put a short design note in `notes`: what happens, the gag, and the loop/one-shot length.

## Quality bar
- Friendly, funny and alive: anticipation, squash/stretch, secondary motion, and at least one small visual gag.
- It must read at a glance from across a desk at 480x242. Personality beats detail.
- Use a tight palette (Iso.C).
- Stay consistent with the model sheet: same Clawd, same desk.
- Verify visually. Render frames with headless Chrome (the foundation agent used it; check what exists, e.g.
  `"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless --screenshot=... --window-size=...`
  with `?embed=1&frame=N&zoom=2`). Look at the PNGs with the Read tool, and iterate at least twice on
  timing, readability and charm.
- Keep any installs and temp files out of the repo.

## Report back
Keep it short:
- the file path
- frame count, fps, and loop or one-shot
- a 2-3 sentence description of the beat and the gag
- the props and lib features used
- anything you wished the lib had
