# Clawd's Studio (home screen room)

Portable C++ (no Arduino / LVGL includes): the same two files build in the firmware and in `tools/room_sim.py`,
so every behaviour can be previewed on the host as PNG sheets / GIFs before flashing.

- `room.h/.cpp`: a 120 x 106 **palette-index** canvas, the palette with its lighting transform (`Light`), the
  furniture, Clawd (18 x 12, four views, poses), the mini helpers, props and FX (emotes, quest "!", thought
  bubble, zzz, sparkles). Lighting is a palette rebuild, not a redraw: entries carry a weight (0 = emissive, 255 =
  fully lit by the room), so the monitor, LEDs, lamp, emotes and the alarm "!" stay bright at night.
- `director.h/.cpp`: the behaviour. Inputs (`Input`): mood, current tool, helpers, plan progress, time of day.
  It derives a task (code / web / review / read / run / search / think / delegate / plan / idle / wait / sleep /
  offline), walks Clawd along the station waypoints (every station joins a free "hallway" row at feet y = 53), plays
  the task loop and schedules fidgets and errands from a weighted table with a no-repeat history. The room runs on
  its own clock: sky and light keyframes over 24 h, sun / moon patch sliding across the floor, lamp after sunset,
  clouds, rain episodes, a bird on the sill, stars and shooting stars, rack LEDs, the monitor's screensaver.
- `home_face.cpp` (one level up) is the LVGL glue: fixed 12 fps stepping, dirty-rect 4x upscale through the palette,
  speech bubble, taps. `screen_home.cpp` maps the agent state to `Input` and draws the HUD band.

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
