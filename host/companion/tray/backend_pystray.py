"""Windows / macOS tray: pystray (0.19) with Pillow images.

pystray only has a title string for the hover text, so the tooltip is the title plus the first status line.
Menus are rebuilt from MenuItem lists: pystray.MenuItem with `checked` as a callable for check items, a nested
pystray.Menu for submenus, pystray.Menu.SEPARATOR for separators, `enabled=False` for status lines.

Threading (set_* are called from the app's refresh thread):
* macOS: AppKit must only be touched from the main thread, and pystray's darwin backend updates the
  NSStatusItem directly when .icon/.title/.menu are assigned. Updates are marshalled with
  PyObjCTools.AppHelper.callAfter. Menu updates use callLater instead: its performSelector:afterDelay: only
  fires in the default run loop mode, i.e. never while the menu is open (event tracking mode). That matters:
  pystray maps a click to callbacks[tag] of the *current* menu, so swapping the menu under an open one would
  run the wrong item (e.g. "Flash firmware" shifting under "Quit"). Notifications (pystray runs osascript)
  stay off the main thread. stop() goes through callAfter too.
* Windows: pystray's win32 backend rebuilds the HMENU in the calling thread and DestroyMenu()s the old one,
  even while TrackPopupMenuEx shows it on the UI thread. Updates are therefore posted to pystray's own
  window (a private WM_USER message registered in its _message_handlers) and run on the UI thread; menu
  updates that arrive while the popup is open (the modal menu loop still dispatches posted messages) are
  kept and applied when it closes. Without those pystray internals (other version), calls run directly.
  NOTIFYICONDATAW limits: tooltip 128, balloon title 64, balloon text 256 UTF-16 units incl. the NUL
  (pystray raises ValueError beyond): truncated here.
The colour icon is used on macOS (it reads on light and dark menu bars thanks to the orange body);
render_tray_icon_mono exists if a template image is wanted later.
"""
from __future__ import annotations

import collections
import logging
import sys
import threading

from .base import MenuItem, TrayBackend

log = logging.getLogger("companion.tray")

ICON_SIZE = 64          # pystray scales it down (Windows: 16/32 px; macOS: menu bar height)
TOOLTIP_MAX = 127       # szTip is WCHAR[128]
WIN_INFO_TITLE_MAX = 63
WIN_INFO_MAX = 255
WM_USER = 0x0400
WM_PYSTRAY_NOTIFY = WM_USER + 11    # pystray._util.win32.WM_NOTIFY (icon clicks; opens the menu)
WM_CM_UI = WM_USER + 0x60           # ours: "run the queued UI calls" (pystray uses +10 and +11)
MAC_MENU_DELAY_S = 0.05


def _safe(fn, *args):
    try:
        fn(*args)
    except Exception:
        log.debug("tray UI update failed", exc_info=True)


def cut_utf16(text: str, n: int) -> str:
    """Truncate to at most n UTF-16 code units (what the Win32 WCHAR buffers count), with an ellipsis."""
    if len(text.encode("utf-16-le")) // 2 <= n:
        return text
    out = text
    while out and len((out + "…").encode("utf-16-le")) // 2 > n:
        out = out[:-1]
    return out + "…"


def _mac_helpers():
    """(callAfter, callLater) from PyObjCTools.AppHelper, or None."""
    if sys.platform != "darwin":
        return None
    try:
        from PyObjCTools import AppHelper
        return AppHelper.callAfter, AppHelper.callLater
    except Exception:
        return None


class Win32Ui:
    """Run callables on pystray's win32 UI thread (see the module docstring). install() needs the icon's
    window, so it is called from the setup callback (pystray creates the window before calling it)."""

    def __init__(self, icon, post=None):
        self.icon = icon
        self._post = post
        self._q: collections.deque = collections.deque()
        self.menu_open = False
        self._pending_menu = None
        self.installed = False

    def install(self) -> bool:
        handlers = getattr(self.icon, "_message_handlers", None)
        hwnd = getattr(self.icon, "_hwnd", None)
        if not isinstance(handlers, dict) or not hwnd:
            return False
        if self._post is None:
            try:
                import ctypes
                pm = ctypes.WinDLL("user32").PostMessageW
                pm.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
                pm.restype = ctypes.c_int
                self._post = lambda: bool(pm(hwnd, WM_CM_UI, 0, 0))
            except Exception:
                return False
        orig = handlers.get(WM_PYSTRAY_NOTIFY)

        def on_notify(wparam, lparam):
            self.menu_open = True
            try:
                return orig(wparam, lparam) if orig else 0
            finally:
                self.menu_open = False
                fn, self._pending_menu = self._pending_menu, None
                if fn:
                    _safe(fn)

        handlers[WM_PYSTRAY_NOTIFY] = on_notify
        handlers[WM_CM_UI] = self.drain
        self.installed = True
        return True

    def call(self, fn, menu: bool = False) -> None:
        if not self.installed:
            _safe(fn)
            return
        self._q.append((fn, menu))
        try:
            ok = self._post()
        except Exception:
            ok = False
        if not ok:   # window gone (quitting): nothing left to race with
            self.drain()

    def drain(self, _wparam=0, _lparam=0) -> int:
        while True:
            try:
                fn, menu = self._q.popleft()
            except IndexError:
                return 0
            if menu and self.menu_open:
                self._pending_menu = fn      # latest wins
            else:
                _safe(fn)


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
    return cut_utf16(text, TOOLTIP_MAX)


class PystrayTray(TrayBackend):
    def __init__(self, on_ready=None, on_quit=None, pystray_module=None):
        super().__init__(on_ready, on_quit)
        if pystray_module is None:
            import pystray as pystray_module
        self.pystray = pystray_module
        from .icon import render_app_icon
        self._mac = _mac_helpers()
        self._menu_key = None
        self._icon_bytes = None
        if sys.platform == "darwin":
            self._accessory_app()
        self.icon = self.pystray.Icon("claudius-maximus", icon=render_app_icon(ICON_SIZE),
                                      title="Claudius Maximus", menu=self.pystray.Menu())
        self._win = Win32Ui(self.icon) if sys.platform == "win32" else None
        self._running = False

    @staticmethod
    def _accessory_app():
        """No Dock icon / app menu when not started from the .app bundle (LSUIElement covers the bundle)."""
        try:
            import AppKit
            AppKit.NSApplication.sharedApplication().setActivationPolicy_(1)  # ...Accessory
        except Exception:
            pass

    def _ui(self, fn, *args, menu: bool = False):
        """Run fn(*args) on the UI thread (directly before run(), when there is no loop yet)."""
        call = (lambda: fn(*args)) if args else fn
        if not self._running:
            _safe(call)
            return
        try:
            if self._mac:
                after, later = self._mac
                if menu:
                    later(MAC_MENU_DELAY_S, _safe, call)
                else:
                    after(_safe, call)
            elif self._win is not None:
                self._win.call(call, menu=menu)
            else:
                _safe(call)
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
        menu (update_menu). Callables of an unchanged structure are equivalent (labels/checks encode what
        they act on)."""
        key = tuple(i.key() for i in items)
        if key == self._menu_key:
            return
        self._menu_key = key
        menu = build_menu(self.pystray, items, self._run_action)

        def apply():
            self.icon.menu = menu      # pystray's setter calls update_menu()
        self._ui(apply, menu=True)

    def notify(self, title: str, body: str) -> None:
        if getattr(self.icon, "HAS_NOTIFICATION", True) is False:
            log.info("notification not supported: %s: %s", title, body)
            return
        if sys.platform == "win32":
            title, body = cut_utf16(title, WIN_INFO_TITLE_MAX), cut_utf16(body, WIN_INFO_MAX)

        def go():
            try:
                self.icon.notify(body, title)
            except Exception as e:     # not supported on every backend / OS setting
                log.info("notification failed: %s", e)
        if self._mac:   # pystray's darwin notify is a blocking osascript call: keep it off the main thread
            threading.Thread(target=go, name="tray-notify", daemon=True).start()
        else:
            self._ui(go)

    def stop(self) -> None:
        def go():
            try:
                self.icon.stop()
            except Exception:
                log.debug("pystray stop failed", exc_info=True)
        if self._mac and self._running:
            try:
                self._mac[0](go)
                return
            except Exception:
                pass
        go()   # Windows: pystray's stop just posts WM_STOP, fine from any thread

    def run(self) -> None:
        def setup(icon):   # pystray runs this in its own thread
            if self._mac:   # visible=True creates the NSStatusItem button image: main thread only
                self._ui(setattr, icon, "visible", True)
            else:
                icon.visible = True
            if self._win is not None and not self._win.install():
                log.info("pystray internals not as expected: tray updates run on the calling thread")
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
