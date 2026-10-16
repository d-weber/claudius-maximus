# Isometric Clawd — design brief (prototype, review only, NOT firmware)

## Goal
Re-imagine the home-screen Clawd (today: flat 2D pixel sprite in firmware/src/screens/home_face.cpp)
as **isometric pixel art**: a friendly, funny little desk companion living on/at a tiny isometric desk.
Each animation is a standalone HTML page the user opens in a browser to review.

## Source character (from home_face.cpp — keep its identity)
Clawd = Claude Code mascot. Flat version on a grid P: body 24P x 16P block, two eye cut-outs (2P x 4P,
at x 10..12 and 24..26, y 4..8), stubby arms 4P x 4P on each side at mid height, 4 short legs (2P wide,
4P tall) at x 8,12,22,26. Expressions in use: open eyes (with gaze dx/dy), blink, closed "—", happy "^ ^",
wince "> <", wide eyes, "o" mouth, laugh mouth, blush cheeks, wink. Body "squash" when landing/pressed.
Extras: amber "!" badge, sparkles, drifting z's, mini violet helper Clawds (subagents).
In iso: the body becomes a chunky rounded-ish block (a voxel brick) seen from the classic 2:1 iso angle;
the face lives on the front-right (or front-left) face of the block. Keep it cute: big readable eyes.

## Palette (from firmware/src/ui/theme.h — use these; derive iso shading as lighter top / mid left / darker right)
bg 1A1918 · surface 262523 · surface2 33312E · border 3D3A36 · text ECE9E4 · muted 9C978E · dim 5E5A54
Clawd orange D97757 (light F0A07E) · waiting amber F2C14E · done green 7FB069 · error E5534B
subagent violet B392F0 · info blue 6A9BCC · offline grey 77716B · blush F4A592
Small extra hues for props (paper, wood, screen glow) are OK but keep the palette tight (~24-32 colors total).

## Technical constraints (so it can later be ported to the ESP32-S3 / LVGL firmware)
- Target stage: **480 x 242 device pixels** (the home face area, dark bg 1A1918). Render to a low-res
  logical canvas (e.g. 160x80 or 120x60 logical px) and scale up with nearest-neighbour (integer scale,
  `image-rendering: pixelated`). Crisp pixels only, no anti-aliasing, no blur, no gradients except dithering.
- Animations must be **frame-based** (discrete frames at 8-12 fps; holds allowed), short loops (1-4 s)
  or one-shot reactions (0.7-2.5 s) that return to the neutral pose. Exact frame lists, no physics.
- Each page must offer: play/pause, step frame, speed (0.5x/1x/2x), 1x vs device scale preview,
  and **"Export spritesheet PNG"** (all frames in a row, logical resolution) + frame count / fps shown.
- Pure static HTML + vanilla JS, no build, no external network deps. Shared code lives in ../lib/.

## Tone
Friendly, funny, alive. Think tamagotchi meets desk toy: anticipation & squash/stretch, little secondary
motion (feet tapping, steam from a mug, a paper flutter), a visual gag or two per animation, readable at
a glance from across the desk. Never creepy, never busy-noisy. Personality > detail.
