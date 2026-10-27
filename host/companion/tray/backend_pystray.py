"""Windows / macOS tray: pystray (0.19) with Pillow images.

pystray only has a title string for the hover text, so the tooltip is the title plus the first status line
(Windows truncates tooltips at 127 characters). Menus are rebuilt from MenuItem lists: pystray.MenuItem with
`checked` as a callable for check items, a nested pystray.Menu for submenus, pystray.Menu.SEPARATOR for
separators, `enabled=False` for status lines.

macOS: AppKit must be touched from the main thread only, and pystray's darwin backend updates the NSStatusItem
directly when .icon/.title/.menu are assigned. Updates from the refresh thread are therefore marshalled with
PyObjCTools.AppHelper.callAfter when it is available. The colour icon is used (it reads on light and dark menu
bars thanks to the orange body); render_tray_icon_mono exists if a template image is wanted later.
"""
from __future__ import annotations

import logging
import sys
import threading

from .base import MenuItem, TrayBackend

log = logging.getLogger("companion.tray")

ICON_SIZE = 64          # pystray scales it down (Windows: 16/32 px; macOS: menu bar height)
TOOLTIP_MAX = 127


def _main_thread_caller():
    """fn(callable) that runs it on the UI thread: callAfter on macOS, direct call elsewhere."""
    if sys.platform == "darwin":
        try:
            from PyObjCTools import AppHelper
            return AppHelper.callAfter
        except Exception:
            pass
    return lambda fn, *a: fn(*a)


def build_menu(pystray, items: list[MenuItem], run_action):
    """MenuItem list -> pystray.Menu. run_action(fn) runs a menu action (off the UI thread)."""
    entries = []
    for it in items:
        if it.separator:
            entries.append(pystray.Menu.SEPARATOR)
            continue
        if it.children is not None:
            entries.append(pystray.MenuItem(it.label, build_menu(pystray, it.children, run_action),
                                            enabled=it.enabled))
            continue
        kw = {"enabled": it.enabled}
        if it.checked is not None:
            kw["checked"] = (lambda v: (lambda _item: v))(bool(it.checked))
        fn = it.action
        # pystray inspects the arity of the action: give it (icon, item)
        action = (lambda f: (lambda _icon, _item: run_action(f)))(fn) if fn else None
        entries.append(pystray.MenuItem(it.label, action, **kw))
    return pystray.Menu(*entries)


def tooltip_text(title: str, lines: list[str]) -> str:
    text = title if not lines else f"{title}\n{lines[0]}" if sys.platform == "win32" else f"{title} - {lines[0]}"
    return text if len(text) <= TOOLTIP_MAX else text[:TOOLTIP_MAX - 1] + "…"


class PystrayTray(TrayBackend):
    def __init__(self, on_ready=None, on_quit=None, pystray_module=None):
        super().__init__(on_ready, on_quit)
        if pystray_module is None:
            import pystray as pystray_module
        self.pystray = pystray_module
        from .icon import render_app_icon
        self._call = _main_thread_caller()
        self._menu_key = None
        self._icon_bytes = None
        self.icon = self.pystray.Icon("claudius-maximus", icon=render_app_icon(ICON_SIZE),
                                      title="Claudius Maximus", menu=self.pystray.Menu())
        self._running = False

    def _ui(self, fn, *args):
        if not self._running:
            fn(*args)
            return
        try:
            self._call(fn, *args)
        except Exception:
            log.debug("tray UI update failed", exc_info=True)

    def _run_action(self, fn):
        def go():
            try:
                fn()
            except Exception:
                log.exception("tray action failed")
        threading.Thread(target=go, name="tray-action", daemon=True).start()

    def set_icon(self, image) -> None:
        img = image.convert("RGBA")
        if img.width != ICON_SIZE:
            from PIL import Image
            img = img.resize((ICON_SIZE, ICON_SIZE), Image.LANCZOS)
        data = img.tobytes()
        if data == self._icon_bytes:
            return
        self._icon_bytes = data
        self._ui(setattr, self.icon, "icon", img)

    def set_icon_renderer(self, render) -> None:
        self.set_icon(render(ICON_SIZE))

    def set_tooltip(self, title: str, lines: list[str]) -> None:
        text = tooltip_text(title, lines)
        if text != getattr(self.icon, "title", None):
            self._ui(setattr, self.icon, "title", text)

    def set_menu(self, items: list[MenuItem]) -> None:
        """Only re-publish when the structure changed: assigning Icon.menu makes pystray rebuild the native
        menu (update_menu), which would close an open menu on some platforms every refresh. Callables of an
        unchanged structure are equivalent (labels/checks encode what they act on)."""
        key = tuple(i.key() for i in items)
        if key == self._menu_key:
            return
        self._menu_key = key
        menu = build_menu(self.pystray, items, self._run_action)

        def apply():
            self.icon.menu = menu      # pystray's setter calls update_menu()
        self._ui(apply)

    def notify(self, title: str, body: str) -> None:
        def go():
            try:
                self.icon.notify(body, title)
            except Exception as e:     # not supported on every backend / OS setting
                log.info("notification failed: %s", e)
        self._ui(go)

    def stop(self) -> None:
        try:
            self.icon.stop()
        except Exception:
            log.debug("pystray stop failed", exc_info=True)

    def run(self) -> None:
        def setup(icon):
            icon.visible = True
            if self.on_ready:
                try:
                    self.on_ready()
                except Exception:
                    log.exception("tray on_ready failed")
        self._running = True
        try:
            self.icon.run(setup=setup)
        finally:
            self._running = False
