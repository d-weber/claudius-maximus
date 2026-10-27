"""Common tray interface: the app builds a list of MenuItem and an icon, a backend shows them.

Backends: backend_sni.py (Linux, StatusNotifierItem over D-Bus) and backend_pystray.py (Windows, macOS).
`set_*` may be called from any thread; backends marshal to their UI loop themselves.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable


@dataclass
class MenuItem:
    label: str = ""
    action: Callable[[], None] | None = None
    enabled: bool = True
    checked: bool | None = None          # None = not a check item
    children: list["MenuItem"] | None = None
    separator: bool = False

    @classmethod
    def sep(cls) -> "MenuItem":
        return cls(separator=True)

    def key(self):
        """Structural identity (no callables): used by backends to skip re-publishing an unchanged menu."""
        return (self.label, self.action is not None, self.enabled, self.checked, self.separator,
                tuple(c.key() for c in self.children) if self.children is not None else None)


class TrayBackend:
    """on_ready() is called once the icon is up (from the backend's thread); on_quit() when the user or the
    system asks the tray to go away (the backend stops by itself afterwards)."""

    def __init__(self, on_ready: Callable[[], None] | None = None, on_quit: Callable[[], None] | None = None):
        self.on_ready = on_ready
        self.on_quit = on_quit

    def run(self) -> None:
        raise NotImplementedError

    def set_icon(self, image) -> None:
        """image: a PIL.Image (RGBA); backends derive the sizes they need from it."""
        raise NotImplementedError

    def set_icon_renderer(self, render: Callable[[int], object]) -> None:
        """Optional: render(size) -> PIL.Image, so a backend that publishes several sizes can draw each one
        natively (crisper bars at 22 px than a downscaled 64 px image). Default: one 64 px image."""
        self.set_icon(render(64))

    def set_tooltip(self, title: str, lines: list[str]) -> None:
        raise NotImplementedError

    def set_menu(self, items: list[MenuItem]) -> None:
        raise NotImplementedError

    def notify(self, title: str, body: str) -> None:
        raise NotImplementedError

    def stop(self) -> None:
        raise NotImplementedError
