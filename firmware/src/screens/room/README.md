# Clawd's Studio (home screen room)

Portable C++ (no Arduino / LVGL includes): the same two files build in the firmware and in `tools/room_sim.py`,
so every behaviour can be previewed on the host as PNG sheets / GIFs before flashing.

- `room.h/.cpp`: a 120 x 106 **palette-index** canvas, the palette with its lighting transform (`Light`), the
  sprite blitter and the lighting passes. Lighting is a palette rebuild, not a redraw: entries carry a weight
  (0 = emissive, 255 = fully lit by the room), so the monitor, LEDs, lamp, emotes and the alarm "!" stay bright at
  night.
- `sprites_gen.h/.cpp` (generated, not committed): the sprite sheets of `firmware/assets/sprites/` as RLE tables,
  written by `tools/sprites.py build` (the PlatformIO pre-build hook and `tools/room_sim.py` run it).
- `director.h/.cpp`: the behaviour. Inputs (`Input`): mood, current tool, helpers, plan progress, time of day.
  It derives a task (code / web / review / read / run / search / think / delegate / plan / idle / wait / sleep /
  offline), walks Clawd along the station waypoints (every station joins a free "hallway" row at feet y = 53), plays
  the task loop and schedules fidgets and errands from a weighted table with a no-repeat history. The room runs on
  its own clock: sky and light keyframes over 24 h, sun / moon patch sliding across the floor, lamp after sunset,
  clouds, rain episodes, a bird on the sill, stars and shooting stars, rack LEDs, the monitor's screensaver.
- `home_face.cpp` (one level up) is the LVGL glue: fixed 12 fps stepping, dirty-rect 4x upscale through the palette,
  speech bubble, taps. `screen_home.cpp` maps the agent state to `Input`; `home_hud.cpp` draws the HUD band (pixel art at 2x).

## What is art and what is code

**Art (the sheets in `firmware/assets/sprites/`, the source of truth; edit them in indexed mode with
`palette.gpl`, see `tools/sprites/FORMAT.md`):** walls, floor, rug, every piece of furniture and its states
(desk screens, steam, drawer, rack LEDs and progress bar, bookshelf gaps, whiteboard, bed, nightstand lamp,
plant sway), the window (day / rain / night per moon phase and twinkle, shooting star, bird on the sill), Clawd
(walk cycles, arm poses, eyes, mouths, face combinations, tints, in bed), the mini helpers, the props (book, can,
torch, mug, paper) and the FX (emotes, quest "!", thought bubble, zzz, sparkles, sweat, dash, alarm ring). Their
colours are palette indices, so the art is lit like the rest of the room.

**Code (`director.cpp` `draw` and `room.cpp`):**
- which frame, where and in which order: the director's state picks the frame (`(t / 8) % 3`, the walk phase...),
  the position (Clawd's x, y, the emote beside or above him) and the painter's order (by feet y);
- light, not art: `drawSunPatch` (sun and moon patch), `drawLampPool`, `drawDoorSpot`, `drawUnderDeskBeam` and
  `Canvas::darken` (the shadows under Clawd and the minis) recolour the pixels already on the canvas;
- the geometry of things that join two moving points: the delegation dash (dots along the line from Clawd to each
  helper, each dot the first pixel of the `dash` sprite) and the happy-reaction ring of sparkles;
- the tint swap (`Canvas::blit` remap): a grey / violet Clawd in a pose `clawd_tint` does not have is the coral
  sprite with C / CS / CH swapped.

Where the sheet has no exact frame the director picks the closest one, deterministically: the desk screensaver
and the active rack LEDs loop their 24 / 8 frames; the shooting star shows over the full moon only (`window_shoot`);
a bird over another sky is the `bird_sit` / `bird_fly` sprite over that sky's frame; Clawd's poses follow the rules
above `clawdSprite` in `director.cpp` (a blink is dropped, walking legs win over the face, raised arms over the
legs and the eyes). A new face combination needs its frame in `clawd_expr` and the matching entry in `EXPR`.

## Behaviour

Tool-driven task changes wait for a short dwell (7-11 s) so a one-off Read in the middle of edits does not send
Clawd across the room; mood changes (needs you, idle, sleep) are immediate.

```
python3 tools/room_sim.py states                 # one frame per task + contact sheet
python3 tools/room_sim.py task idle --seconds 60 --gif --tod 20
python3 tools/room_sim.py day --seconds 1800     # scripted session: activity log + fidget variety stats
python3 tools/room_sim.py clock                  # 24 h of lighting, one frame per 20 min
```
On the device: `tools/dev.py send '{"t":"cmd","anim":"wait"}'` forces a task (`""` releases), then `dev.py shot`.

The `Director` instance is allocated in PSRAM by `home_face.cpp` (not a static in `.bss`): hot per-tick state in
internal RAM collided with the LCD scan-out DMA and made frames jump (docs/STATUS.md, "Scan-out stalls"). Keep it
that way, and check `lcd_underruns` in a `perf` window after changes to what runs every tick.
