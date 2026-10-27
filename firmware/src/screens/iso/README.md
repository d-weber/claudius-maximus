# Isometric Clawd in firmware — porting guide

`iso.h` / `iso.cpp` is a 1:1 C++ port of `iso-clawd/lib/iso.js` (same function names, same pixel output).
Each animation page `iso-clawd/anims/NN-name.html` becomes `anim_NN_name.cpp` here, exporting
`const IsoAnim anim_NN_name = {"NN-name", fps, frames, loop, drawsHelpers, draw};` (see `anim.h`).
`anim_01_idle.cpp` is the reference port: read it next to `01-idle.html` first.

## Rules
- `draw(Ctx &c, int i, const AnimEnv &env)` is a pure function of the frame index `i` (and `env`). Same fps,
  frame count and loop/one-shot as the JS page (`Iso.player({...})`).
- Keep the JS coordinates as they are (logical 240 x 121; the device shows columns 40..199 at 3x). Do not
  re-layout the scene.
- Clawd's tint: use `env.tint()` where the JS uses the default orange (grey when the host is offline). Where
  the JS passes an explicit tint (e.g. violet minis), keep it.
- `drawsHelpers` is `false` unless the animation places the mini helpers itself (17-delegating uses
  `env.helpers`); the director then draws them.
- No heap allocation in `draw`: tables are `static const`, buffers are fixed arrays.
- Integer pixels only, like the JS (`Math.round` -> `lroundf` / `iso::lerp`).
- Only touch your own `anim_NN_name.cpp`. Helpers the JS page defines inline go in your file (anonymous
  namespace). If `iso.cpp` seems wrong compared to `iso.js`, report it; do not edit it.

## JS -> C++ cheat sheet
| JS | C++ |
|---|---|
| `Iso.rect(ctx,x,y,w,h,c)` / `Iso.px` / `Iso.line` / `Iso.isoLine(ctx,x,y,n,'dr',c)` / `Iso.dither` | `rect(c,...)`, `px`, `line`, `isoLine(c,x,y,n,"dr",col)`, `dither` |
| `Iso.sprite(ctx,x,y,rows,{'#':C.x},flip)` | `static const char *const rows[] = {...}; sprite(c,x,y,rows,ISO_MAP({'#',C::x}),flip)`. Pass `ISO_MAP(...)` straight into the call: it is a compound-literal array that dies at the end of the statement, so never keep one in a local (use a named `MapEntry[]` instead). |
| `Iso.text(ctx,x,y,'HI',c)` / `Iso.textWidth` | `text(c,x,y,"HI",col)` / `textWidth` |
| `Iso.w2s(ox,oy,wx,wy,wz)` | `w2s(...)` (ints) or `w2sf(...)` (floats, rounded) |
| `Iso.boxAt / box / tile / plane` | same names; `Pal{top,left,right,edge}`, `NONE` skips a face; `opts.round` -> last bool arg |
| painters `r.left.rect(u,v,w,h,c)`, `r.top.cell(i,j,c)`, `p.at(u,v)`, `p.cols` | same (`FacePainter`, `TopPainter`) |
| `Iso.obj(ctx,x,y,A,B,'left',z)` + `o.box(...,'tag')` + `r = o.draw(); r.tag.front` | `Obj o(c,x,y,A,B,LEFT,z); o.box(..., tagIndex); o.draw(); o.res[tagIndex].front()` (pointer, may be null) |
| `Iso.drawClawd(ctx,{x,y,z,eyes:{shape:'open',gazeX,gazeY,l,r},mouth,arms:{l,r} or 'out',legs,squash,shake,tint,facing,blush,shadow,size:'mini',shine,eyeColor})` | `ClawdOpts o; o.x=..; o.eyes={EYE_OPEN,gx,gy}; o.mouth=MOUTH_O; o.armL=ARM_UP; o.legs=LEGS_STEPA; o.mini=true; o.facing=RIGHT; drawClawd(c,o)` -> `ClawdRes` (`head`, `top`, `face`, `eyeL/eyeR`, `mouth`, `handL/handR` + `hasHandL/R`, `feet`, bbox `bx,by,bw,bh`, painter `F`) |
| string names `'blink' 'closed' 'happy' 'wince' 'wide' 'wink'` | `EYE_BLINK ...`; mouths `MOUTH_O, MOUTH_O_SMALL, MOUTH_LAUGH, MOUTH_SMILE, MOUTH_WOBBLY, MOUTH_FLAT`; arms `ARM_OUT, ARM_HIGH, ARM_UP, ARM_WAVEA, ARM_WAVEB, ARM_DOWN, ARM_FORWARD, ARM_HOLD, ARM_TYPEA, ARM_TYPEB, ARM_NONE`; legs `LEGS_STAND, LEGS_SIT, LEGS_DANGLE, LEGS_DANGLEB, LEGS_STEPA, LEGS_STEPB, LEGS_TAP` (`eyeOf()/mouthOf()/armOf()/legsOf()` convert strings if a table is easier to keep as strings) |
| `Iso.SPOT.clawd` etc. / `Iso.DESK` / `Iso.deskPoint(u,v)` / `Iso.drawDesk(ctx,{mat:true})` | `SPOT_clawd` ... / `Desk` / `deskPoint` / `DeskOpts d; d.mat = true; drawDesk(c,&d)` |
| `Iso.prop.laptop(ctx,x,y,{facing:'upLeft',screen:'code',frame})` | `LaptopOpts o; o.facing=UP_LEFT; o.screen=SCR_CODE; o.frame=i; laptop(c,x,y,o)` -> `LaptopRes{hasScreen,screen,area,lid,base}`; custom screen fn: `o.screen=SCR_CUSTOM; o.fn=myfn; o.ud=...` |
| `prop.monitor / paper / book / notepad / pencil / mug / magnifier / sticky / terminal / browser / globe / planBoard / clipboard / lamp / shadow` | same names, option structs in `iso.h` (`MugOpts{frame,steam,color,z}`, `WindowOpts{plane,len,h,z,bg,reveal,cursor,frame,lines,nlines}` with `TermLine{indent,len,col}`, ...) |
| `Iso.fx.sparkle / bang / question / zee / sweat / heart / check / cross / dots / poof / ticks / puff` | `fx::sparkle(c,x,y,size,col)` ... `fx::bang(c,x,y,badge,col)`, `fx::puff(c,x,y,w,h,{thought,tail('l'/'r'/'n'),fill,border})` |
| `Iso.seq([[v,hold],...], i)` / `Iso.seq.len` / `Iso.cycle(arr,i,hold)` / `Iso.pingpong(n,i)` / `Iso.lerp` | `static const Beat<Pose> TL[] = {{{...},hold},...}; seq(TL,i)` / `seqLen(TL)` / `cycle(arr,i,hold)` / `pingpong` / `lerp(a,b,t)` |
| pose objects with optional fields (`p.sq !== undefined`) | a `Pose` struct with explicit sentinel values (see `NOSQ` in anim_01_idle.cpp) |

## Pacing
Loops carry a calm segment (`calmFrom, calmTo`, the last two fields of `IsoAnim`): a short stretch that loops
on its own (breathing, blinking, typing...). The director plays the loop once, then rests in that segment for a
random 4-12 s (70 % of the time) before the next full pass, so the gags come at irregular, longer intervals on a
screen that is watched for a long time. Pick a range whose first and last frames are near the same pose.
A calm segment usually holds one blink, and looping 1 s of it verbatim would make Clawd blink every second, so the
director rolls per pass whether that pass may blink (about one blink per 3.5 s on average) and otherwise renders
`EYE_BLINK` as open eyes through `iso::blinkSuppressed` (off for QA renders). The mug sits at `SPOT_mug` in every
animation; put new props elsewhere.

## Build
From the repo root: `DEV_NAME=<you> host/.venv/bin/python tools/dev.py build` (own build dir, no device needed).
Do not flash: the device is shared and the lead flashes and reviews screenshots.
