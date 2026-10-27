"""Tray icon + menu for the desktop app. make_backend picks the implementation for this OS."""
from __future__ import annotations

import sys

from .base import MenuItem, TrayBackend

__all__ = ["MenuItem", "TrayBackend", "make_backend"]


def make_backend(on_ready=None, on_quit=None) -> TrayBackend:
    """Linux: StatusNotifierItem over D-Bus (dbus-fast). Windows / macOS: pystray."""
    if sys.platform.startswith("linux"):
        from .backend_sni import SNITray
        return SNITray(on_ready, on_quit)
    from .backend_pystray import PystrayTray
    return PystrayTray(on_ready, on_quit)
