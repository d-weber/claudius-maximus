"""Clawd icons drawn with Pillow (no cairo, no image files): the application icon and the tray icon.

Geometry is laid out on a 22-unit grid (the classic tray size) and snapped to whole target pixels, then drawn
4x supersampled and downscaled: edges stay crisp at 16/22/32 px, corners get a little antialiasing.

Tray icon, with plan limits known:      without:
    +----------+ o   <- state badge         +--------------+
    | ▌      ▌ |                            |  ▌        ▌  |
    +----------+                            |            o |  <- badge on the corner
    [=====-------]   5-hour                 +--------------+
    [==----------]   7-day
"""
from __future__ import annotations

from PIL import Image, ImageDraw

CLAWD = (217, 119, 87, 255)       # #D97757, Anthropic's orange (the device's Clawd)
EYE = (31, 26, 23, 255)
RING = (30, 30, 30, 255)          # dark ring around the badge: separates it on light and dark trays
TRACK = (153, 153, 153, 90)
TRACK_EDGE = (153, 153, 153, 205)

BADGE = {
    "none": (154, 160, 166, 255),     # grey: no device
    "ok": (63, 185, 80, 255),         # green: connected
    "update": (245, 165, 36, 255),    # amber: update available / action needed
    "busy": (245, 165, 36, 255),      # amber: flashing
    "error": (229, 72, 77, 255),      # red
}

SS = 4  # supersampling factor


def bar_color(pct: float) -> tuple:
    """Same thresholds and colours as the claude_tray usage indicator."""
    if pct >= 90:
        return (227, 69, 61, 255)
    if pct >= 70:
        return (245, 158, 38, 255)
    return (217, 120, 87, 255)


def tray_state(status: dict | None) -> str:
    """none | ok | update | busy | error. The app may precompute it as status["tray_state"]."""
    st = status or {}
    if st.get("tray_state") in BADGE:
        return st["tray_state"]
    if (st.get("flash") or {}).get("active"):
        return "busy"
    if st.get("daemon_error"):
        return "error"
    upd = st.get("update")
    if st.get("connected"):
        if isinstance(upd, (list, tuple)) and upd and upd[0]:
            return "update"
        return "ok"
    return "none"


def usage_pcts(status: dict | None) -> tuple[float | None, float | None]:
    rl = (status or {}).get("rate_limits") or {}

    def pct(key):
        v = rl.get(key)
        if isinstance(v, dict) and isinstance(v.get("used_percentage"), (int, float)):
            return max(0.0, min(100.0, float(v["used_percentage"])))
        return None
    return pct("five_hour"), pct("seven_day")


class _Canvas:
    """Draws in 22-unit coordinates snapped to the target pixel grid, on a SS-times larger image."""

    def __init__(self, size: int):
        self.size = size
        self.f = size / 22.0
        self.img = Image.new("RGBA", (size * SS, size * SS), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.img)

    def px(self, v: float) -> int:
        return round(v * self.f) * SS

    def box(self, x0, y0, x1, y1, min_px=1):
        a, b, c, e = self.px(x0), self.px(y0), self.px(x1), self.px(y1)
        c = max(c, a + min_px * SS)
        e = max(e, b + min_px * SS)
        return [a, b, c - 1, e - 1]

    def rrect(self, box, r_units, fill, outline=None, width=0):
        r = max(0, round(r_units * self.f * SS))
        w = max(box[2] - box[0], box[3] - box[1])
        self.d.rounded_rectangle(box, radius=min(r, w // 2), fill=fill, outline=outline, width=width)

    def done(self) -> Image.Image:
        return self.img.resize((self.size, self.size), Image.LANCZOS)


def _face(c: _Canvas, x0, y0, x1, y1, body=CLAWD, eye=EYE, eye_cut=False):
    """Clawd's head: an orange rounded block with two tall dark eyes in its upper half."""
    c.rrect(c.box(x0, y0, x1, y1), 2.5, body)
    w, h = x1 - x0, y1 - y0
    ew, eh = max(w * 0.13, 2.0), max(h * 0.36, 3.0)
    ey = y0 + h * 0.24
    for cx in (x0 + w * 0.30, x0 + w * 0.70):
        box = c.box(cx - ew / 2, ey, cx + ew / 2, ey + eh)
        if eye_cut:   # mono: punch the eyes out so the template image reads on any background
            c.d.rectangle(box, fill=(0, 0, 0, 0))
        else:
            c.d.rectangle(box, fill=eye)


def _badge(c: _Canvas, cx, cy, d, fill, ring=RING, hollow=False):
    rw = max(1, round(0.9 * c.f)) * SS
    box = c.box(cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2, min_px=3)
    if hollow:
        c.d.ellipse(box, fill=(0, 0, 0, 0), outline=fill, width=max(rw, round(1.4 * c.f) * SS))
    else:
        c.d.ellipse(box, fill=fill, outline=ring, width=rw)


def _bars(c: _Canvas, pcts, top, mono=False):
    edge_w = max(1, round(0.7 * c.f * SS))
    h, gap = 3.6, 0.8
    for i, pct in enumerate(pcts):
        y0 = top + i * (h + gap)
        box = c.box(0.5, y0, 21.5, y0 + h, min_px=2)
        track = (0, 0, 0, 80) if mono else TRACK
        edge = (0, 0, 0, 200) if mono else TRACK_EDGE
        c.rrect(box, 1.2, track, outline=edge, width=edge_w)
        if pct is None or pct <= 0:
            continue
        x1 = box[0] + max(SS, round((box[2] - box[0]) * pct / 100.0))
        fill = (0, 0, 0, 255) if mono else bar_color(pct)
        c.rrect([box[0], box[1], x1, box[3]], 1.2, fill)


def render_app_icon(size: int = 256) -> Image.Image:
    """The application icon (packaging, notifications): Clawd's face filling a rounded square."""
    c = _Canvas(size)
    c.rrect(c.box(1, 1, 21, 21), 4.5, CLAWD)
    ew, eh = 2.6, 6.2
    for cx in (7.4, 14.6):
        c.d.rectangle(c.box(cx - ew / 2, 6.2, cx + ew / 2, 6.2 + eh), fill=EYE)
    return c.done()


def render_tray_icon(size: int, status: dict | None = None) -> Image.Image:
    return _render_tray(size, status, mono=False)


def render_tray_icon_mono(size: int, status: dict | None = None) -> Image.Image:
    """Black + alpha only (macOS template image): eyes are cut out, the badge is a hollow ring when no device
    is connected, absent when all is well, and a filled dot when something needs attention."""
    return _render_tray(size, status, mono=True)


def _render_tray(size: int, status: dict | None, mono: bool) -> Image.Image:
    c = _Canvas(size)
    state = tray_state(status)
    five, week = usage_pcts(status)
    body = (0, 0, 0, 255) if mono else CLAWD
    color = BADGE[state]
    if five is not None or week is not None:
        _face(c, 0.5, 0.5, 16.0, 12.0, body=body, eye_cut=mono)
        bx, by, bd = 19.0, 6.25, 5.6
        _bars(c, (five, week), 13.6, mono=mono)
    else:
        _face(c, 0.5, 2.0, 21.5, 19.0, body=body, eye_cut=mono)
        bx, by, bd = 17.6, 16.6, 7.4
    if mono:
        if state == "none":
            _badge(c, bx, by, bd, (0, 0, 0, 255), hollow=True)
        elif state != "ok":
            if five is None and week is None:   # on the face: cut a gap so the dot stays distinct
                _badge(c, bx, by, bd + 2.2, (0, 0, 0, 0), ring=(0, 0, 0, 0))
            _badge(c, bx, by, bd, (0, 0, 0, 255), ring=(0, 0, 0, 255))
    else:
        _badge(c, bx, by, bd, color)
    return c.done()
