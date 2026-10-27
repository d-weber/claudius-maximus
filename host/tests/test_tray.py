"""Tests for the tray: icons, the status -> menu model, the pystray and SNI backends (no display, no D-Bus).

Run:  host/.venv/bin/python -m unittest host/tests/test_tray.py -v
"""
from __future__ import annotations

import os
import sys
import threading
import time
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from companion import app as appmod  # noqa: E402
from companion.tray.base import MenuItem  # noqa: E402
from companion.tray.icon import (BADGE, CLAWD, render_app_icon, render_tray_icon,  # noqa: E402
                                 render_tray_icon_mono, usage_pcts)

try:
    import dbus_fast  # noqa: F401
    HAVE_DBUS_FAST = True
except ImportError:
    HAVE_DBUS_FAST = False

RL = {"five_hour": {"used_percentage": 42, "resets_at": time.time() + 7500},
      "seven_day": {"used_percentage": 12, "resets_at": time.time() + 4 * 86400}}
NATIVE = {"device": "/dev/ttyACM0", "kind": "native", "vid": 0x303A, "pid": 0x1001}
UART = {"device": "/dev/ttyACM1", "kind": "uart", "vid": 0x1A86, "pid": 0x55D3}

STATES = {
    "no_device": {"connected": False, "ports": [], "sessions": []},
    "uart_only": {"connected": False, "ports": [UART], "sessions": []},
    "up_to_date": {"connected": True, "port": "/dev/ttyACM0", "fw": "0.3.0", "board": "ESP32-S3-Touch-LCD-4B",
                   "ports": [NATIVE], "update": (False, "up to date"), "rate_limits": RL,
                   "sessions": [{"id": "a", "status": "busy"}, {"id": "b", "status": "waiting"},
                                {"id": "c", "status": "offline"}]},
    "update": {"connected": True, "port": "COM5", "fw": "0.2.0", "board": "ESP32-S3-Touch-LCD-4B",
               "ports": [NATIVE], "update": (True, "0.2.0 -> 0.3.0"), "sessions": []},
    "flashing": {"connected": False, "ports": [NATIVE], "flash": {"active": True, "pct": 42, "msg": "writing"},
                 "sessions": []},
    "install": {"connected": True, "port": "/dev/ttyACM0", "fw": None, "ports": [NATIVE], "install_needed": True,
                "sessions": []},
}


class FakeApp:
    def __init__(self):
        self.calls = []

    def flash(self, board=None):
        self.calls.append(("flash", board))

    def set_autostart(self, on):
        self.calls.append(("autostart", on))

    def install_hooks(self):
        self.calls.append(("hooks",))

    def open_log(self):
        self.calls.append(("log",))

    def open_usage_page(self):
        self.calls.append(("usage",))

    def quit(self):
        self.calls.append(("quit",))


def pixels(img):
    b = img.convert("RGBA").tobytes()
    return [tuple(b[i:i + 4]) for i in range(0, len(b), 4)]


def labels(items):
    return [("---" if i.separator else i.label) for i in items]


def find(items, prefix):
    return next(i for i in items if not i.separator and i.label.startswith(prefix))


# ---------------------------------------------------------------------------------------------
class IconTest(unittest.TestCase):
    def test_sizes_and_alpha(self):
        for size in (16, 22, 32, 44, 64, 256):
            for img in (render_app_icon(size), render_tray_icon(size, STATES["up_to_date"]),
                        render_tray_icon(size, {}), render_tray_icon_mono(size, STATES["update"])):
                self.assertEqual(img.size, (size, size))
                self.assertEqual(img.mode, "RGBA")
                lo, hi = img.getchannel("A").getextrema()
                self.assertEqual(lo, 0, "icon must have a transparent background")
                self.assertEqual(hi, 255)

    def test_app_icon_is_orange_with_eyes(self):
        img = render_app_icon(64)
        self.assertEqual(img.getpixel((0, 0))[3], 0)
        r, g, b, a = img.getpixel((32, 52))
        self.assertEqual(a, 255)
        self.assertLess(abs(r - CLAWD[0]) + abs(g - CLAWD[1]) + abs(b - CLAWD[2]), 12)
        dark = [p for p in pixels(img.crop((16, 18, 48, 36))) if p[3] > 200 and sum(p[:3]) < 150]
        self.assertGreater(len(dark), 20, "two dark eyes")

    def _has_color(self, img, color, tol=40):
        return any(p[3] > 200 and sum(abs(p[i] - color[i]) for i in range(3)) < tol for p in pixels(img))

    def test_badge_colors(self):
        for name, st, key in (("none", STATES["no_device"], "none"), ("ok", STATES["up_to_date"], "ok"),
                              ("update", STATES["update"], "update"), ("busy", STATES["flashing"], "busy"),
                              ("error", {"daemon_error": "boom"}, "error")):
            st = dict(st, tray_state=appmod.tray_state(st))
            self.assertEqual(st["tray_state"], key, name)
            for size in (22, 32):
                self.assertTrue(self._has_color(render_tray_icon(size, st), BADGE[key]), f"{name} @{size}")

    def test_usage_bars_drawn(self):
        plain = render_tray_icon(22, {"connected": True})
        bars = render_tray_icon(22, {"connected": True, "rate_limits": RL})
        self.assertNotEqual(plain.tobytes(), bars.tobytes())
        self.assertEqual(usage_pcts({"rate_limits": RL}), (42.0, 12.0))
        self.assertEqual(usage_pcts({"rate_limits": {"five_hour": {"used_percentage": 150}}}), (100.0, None))
        # 5h bar (42 %) is longer than the 7d bar (12 %): count filled orange pixels per bar row
        img = render_tray_icon(64, {"rate_limits": RL})
        def filled(y):
            return sum(1 for x in range(64) if img.getpixel((x, y))[3] > 200 and img.getpixel((x, y))[0] > 180
                       and img.getpixel((x, y))[1] < 140)
        top = max(filled(y) for y in range(40, 51))
        bottom = max(filled(y) for y in range(52, 64))
        self.assertGreater(top, bottom)
        self.assertGreater(bottom, 0)

    def test_mono_is_black_only(self):
        for st in STATES.values():
            for size in (16, 22, 32):
                for p in pixels(render_tray_icon_mono(size, dict(st, rate_limits=RL))):
                    if p[3]:
                        self.assertEqual(p[:3], (0, 0, 0))


# ---------------------------------------------------------------------------------------------
class MenuModelTest(unittest.TestCase):
    def test_no_device(self):
        items = appmod.build_menu(STATES["no_device"], FakeApp())
        self.assertEqual(labels(items), ["Device: not found", "Sessions: none", "---", "Flash firmware",
                                         "Start at login", "Install Claude Code hooks", "Open log",
                                         "Open claude.ai usage", "---", "Quit"])
        self.assertFalse(items[0].enabled)
        sub = find(items, "Flash firmware").children
        self.assertEqual([s.label for s in sub], ["4B", "V4"])
        self.assertEqual([s.checked for s in sub], [True, False])

    def test_uart_only(self):
        st = STATES["uart_only"]
        self.assertEqual(appmod.device_line(st), "Board on USB-TO-UART port: move the cable to USB-OTG")
        self.assertEqual(appmod.tray_state(st), "update")

    def test_connected_up_to_date(self):
        app = FakeApp()
        st = dict(STATES["up_to_date"], hooks_installed=True, autostart=True)
        items = appmod.build_menu(st, app)
        self.assertEqual(items[0].label, "Device: connected on /dev/ttyACM0 · fw 0.3.0 (4B)")
        self.assertEqual(items[1].label, "Sessions: 2 (1 working, 1 needs you)")
        self.assertEqual(items[2].label, "Plan: 5h 42% · 7d 12%")
        self.assertIn("Flash firmware", labels(items))
        self.assertTrue(find(items, "Start at login").checked)
        self.assertFalse(find(items, "Claude Code hooks: installed").enabled)
        find(items, "Start at login").action()
        find(items, "Quit").action()
        find(items, "Open claude.ai usage").action()
        self.assertEqual(app.calls, [("autostart", False), ("quit",), ("usage",)])
        tips = appmod.tooltip_lines(st)
        self.assertTrue(tips[2].startswith("5-hour: 42%  ·  resets in 2h"))
        self.assertTrue(tips[3].startswith("Weekly: 12%  ·  resets "))
        self.assertEqual(appmod.tray_state(st), "ok")

    def test_update_available(self):
        app = FakeApp()
        items = appmod.build_menu(STATES["update"], app)
        upd = find(items, "Update firmware")
        self.assertEqual(upd.label, "Update firmware (0.2.0 → 0.3.0)…")
        self.assertNotIn("Flash firmware", labels(items))
        upd.action()
        self.assertEqual(app.calls, [("flash", "4b")])
        self.assertEqual(appmod.tray_state(STATES["update"]), "update")

    def test_install_needed(self):
        app = FakeApp()
        items = appmod.build_menu(dict(STATES["install"], board_choice="v4"), app)
        find(items, "Install firmware…").action()
        self.assertEqual(app.calls, [("flash", "v4")])

    def test_flashing(self):
        st = STATES["flashing"]
        items = appmod.build_menu(st, FakeApp())
        self.assertEqual(items[0].label, "Flashing firmware: 42% (writing)")
        fl = find(items, "Flashing firmware…")
        self.assertFalse(fl.enabled)
        self.assertEqual(appmod.tray_state(st), "busy")

    def test_observer_cannot_flash(self):
        st = dict(STATES["update"], observer=True)
        self.assertFalse(find(appmod.build_menu(st, FakeApp()), "Update firmware").enabled)
        self.assertIn("Another daemon owns the device (status only)", appmod.tooltip_lines(st))

    def test_board_id(self):
        self.assertEqual(appmod.board_id("ESP32-S3-Touch-LCD-4B"), "4b")
        self.assertEqual(appmod.board_id("ESP32-S3-Touch-LCD-4"), "v4")
        self.assertEqual(appmod.board_id("v4"), "v4")
        self.assertIsNone(appmod.board_id(None))


# ---------------------------------------------------------------------------------------------
class AppTest(unittest.TestCase):
    def setUp(self):
        self.tmp = __import__("tempfile").mkdtemp()
        self.env = mock.patch.dict(os.environ, {"CLAUDIUS_HOME": self.tmp})
        self.env.start()
        stale = mock.patch.object(appmod.App, "_refresh_stale_hooks")   # reads the real Claude settings
        stale.start()
        self.addCleanup(stale.stop)

    def tearDown(self):
        self.env.stop()
        __import__("shutil").rmtree(self.tmp, ignore_errors=True)

    def test_flash_worker_pauses_and_resumes(self):
        events = []

        class FakeDaemon:
            def pause_device(self, why):
                events.append(("pause", why))

            def resume_device(self):
                events.append(("resume",))

        fake_flasher = types.ModuleType("companion.flasher")

        def flash(board, progress=None, log=None, **kw):
            events.append(("flash", board))
            progress(50, "writing")
            progress(100, "done")
            return types.SimpleNamespace(fw_version="0.3.0")
        fake_flasher.flash = flash
        backend = mock.Mock()
        app = appmod.App(types.SimpleNamespace(no_setup=True), backend=backend, start_daemon=False)
        app.daemon = FakeDaemon()
        app.status = lambda: {"connected": True, "ports": []}
        import companion
        with mock.patch.dict(sys.modules, {"companion.flasher": fake_flasher}), \
                mock.patch.object(companion, "flasher", fake_flasher, create=True):
            app._flash_worker("4b")
        self.assertEqual(events, [("pause", "flashing"), ("flash", "4b"), ("resume",)])
        self.assertFalse(app.flash_state["active"])
        titles = [c.args[0] for c in backend.notify.call_args_list]
        self.assertEqual(titles, ["Flashing firmware", "Firmware installed"])

    def test_flash_failure_notifies_and_resumes(self):
        events = []
        d = mock.Mock()
        d.resume_device.side_effect = lambda: events.append("resume")
        fake_flasher = types.ModuleType("companion.flasher")

        def flash(board, **kw):
            raise RuntimeError("No board on USB")
        fake_flasher.flash = flash
        backend = mock.Mock()
        app = appmod.App(types.SimpleNamespace(no_setup=True), backend=backend, start_daemon=False)
        app.daemon = d
        app.status = lambda: {}
        import companion
        with mock.patch.dict(sys.modules, {"companion.flasher": fake_flasher}), \
                mock.patch.object(companion, "flasher", fake_flasher, create=True):
            with self.assertLogs("companion.app", "ERROR"):
                app._flash_worker("v4")
        self.assertEqual(events, ["resume"])
        self.assertEqual(backend.notify.call_args_list[-1].args, ("Flashing failed", "No board on USB"))
        self.assertEqual(appmod.tray_state({"flash": app.flash_state}), "error")

    def test_flash_without_daemon_refuses(self):
        backend = mock.Mock()
        app = appmod.App(types.SimpleNamespace(no_setup=True), backend=backend, start_daemon=False)
        app._flash_worker("4b")
        self.assertEqual(backend.notify.call_args_list[-1].args[0], "Cannot flash")

    def test_first_run_once(self):
        backend = mock.Mock()
        app = appmod.App(types.SimpleNamespace(no_setup=True), backend=backend, start_daemon=False)
        app.install_hooks = mock.Mock(return_value=True)
        app.set_autostart = mock.Mock(return_value="Disabled the old systemd service.")
        store = {}
        with mock.patch.object(appmod, "settings_get", lambda k, d=None: store.get(k, d)), \
                mock.patch.object(appmod, "settings_set", store.__setitem__), \
                mock.patch("companion.hostos.service_installed", return_value=False, create=True):
            app._first_run()
            app._first_run()
        app.install_hooks.assert_called_once_with(notify=False)
        app.set_autostart.assert_called_once_with(True, notify=False)
        self.assertEqual(backend.notify.call_count, 1)
        self.assertIn("Disabled the old systemd service. It lives", backend.notify.call_args.args[1])
        self.assertTrue(store["first_run_done"])

    def test_first_run_respects_windows_installer(self):
        d = __import__("tempfile").mkdtemp()
        open(os.path.join(d, "unins000.exe"), "w").close()
        exe = os.path.join(d, "ClaudiusMaximus.exe")
        with mock.patch.object(sys, "frozen", True, create=True), mock.patch.object(sys, "executable", exe):
            self.assertTrue(appmod.installer_handles_autostart())
            backend = mock.Mock()
            app = appmod.App(types.SimpleNamespace(no_setup=True), backend=backend, start_daemon=False)
            app.install_hooks = mock.Mock(return_value=True)
            app.set_autostart = mock.Mock()
            with mock.patch.object(appmod, "settings_get", lambda k, d=None: d), \
                    mock.patch.object(appmod, "settings_set", lambda k, v: None):
                app._first_run()
            app.set_autostart.assert_not_called()
        self.assertFalse(appmod.installer_handles_autostart())
        __import__("shutil").rmtree(d, ignore_errors=True)

    def test_daemon_lock_held_means_observer(self):
        """daemon.run(daemon=d) returning 1 (lock held) => observer mode, no crash, flashing disabled."""
        fake = types.ModuleType("companion.daemon")

        class D:
            def __init__(self, **kw):
                self.error = ""
        def run(daemon=None):
            daemon.error = "another Claudius Maximus daemon is already running (pid 1)"
            return 1
        fake.Daemon, fake.run = D, run
        fake.instance_lock_holder = lambda: None
        fake.status_path = lambda: os.path.join(self.tmp, "status.json")
        import companion
        with mock.patch.dict(sys.modules, {"companion.daemon": fake}), \
                mock.patch.object(companion, "daemon", fake, create=True):
            with self.assertLogs("companion.app", "WARNING"):
                app = appmod.App(types.SimpleNamespace(no_setup=True), backend=None, start_daemon=True)
                app.daemon_thread.join(2)
            self.assertIsNone(app.daemon)
            st = app._daemon_status()
        self.assertTrue(st["observer"])
        self.assertIn("already running", st["observer_msg"])
        self.assertFalse(find(appmod.build_menu(dict(st, ports=[]), FakeApp()), "Flash firmware").enabled)

    def test_settings_fallback_roundtrip(self):
        with mock.patch.dict(sys.modules, {"companion.hostos": None}):
            appmod.settings_set("board", "v4")
            self.assertEqual(appmod.settings_get("board"), "v4")


# ---------------------------------------------------------------------------------------------
def fake_pystray():
    """Enough of pystray 0.19's API to build menus: Icon, Menu(*items) with SEPARATOR, MenuItem."""
    m = types.ModuleType("pystray")

    class MenuItem:
        def __init__(self, text, action=None, checked=None, radio=False, default=False, visible=True,
                     enabled=True):
            self.text, self.action, self.checked, self.enabled = text, action, checked, enabled
            self.submenu = action if type(action).__name__ == "Menu" else None

    class Menu:
        SEPARATOR = MenuItem("- - - -")

        def __init__(self, *items):
            self.items = items

    class Icon:
        def __init__(self, name, icon=None, title=None, menu=None):
            self.name, self.icon, self.title, self.menu = name, icon, title, menu
            self.notified, self.updated, self.stopped, self.visible = [], 0, False, False

        def notify(self, message, title=None):
            self.notified.append((title, message))

        def update_menu(self):
            self.updated += 1

        def stop(self):
            self.stopped = True

        def run(self, setup=None):
            if setup:
                setup(self)

    m.MenuItem, m.Menu, m.Icon = MenuItem, Menu, Icon
    return m


class PystrayBackendTest(unittest.TestCase):
    def setUp(self):
        self.mod = fake_pystray()
        sys.modules["pystray"] = self.mod
        from companion.tray import backend_pystray
        self.bp = backend_pystray

    def tearDown(self):
        sys.modules.pop("pystray", None)

    def test_menu_construction(self):
        app = FakeApp()
        items = appmod.build_menu(dict(STATES["up_to_date"], autostart=True), app)
        tray = self.bp.PystrayTray(pystray_module=self.mod)
        tray.set_menu(items)
        menu = tray.icon.menu
        entries = menu.items
        self.assertEqual(len(entries), len(items))
        self.assertIs(entries[3], self.mod.Menu.SEPARATOR)
        self.assertEqual(entries[0].text, items[0].label)
        self.assertFalse(entries[0].enabled)
        flash = next(e for e in entries if e.text == "Flash firmware")
        self.assertIsInstance(flash.submenu, self.mod.Menu)
        self.assertEqual([e.text for e in flash.submenu.items], ["4B", "V4"])
        self.assertTrue(flash.submenu.items[0].checked(None))
        self.assertFalse(flash.submenu.items[1].checked(None))
        login = next(e for e in entries if e.text == "Start at login")
        self.assertTrue(login.checked(login))
        quit_ = entries[-1]
        self.assertIsNone(quit_.checked)
        done = threading.Event()
        app.quit = lambda: done.set()
        tray.set_menu(appmod.build_menu(STATES["up_to_date"], app))
        tray.icon.menu.items[-1].action(tray.icon, None)    # pystray calls action(icon, item)
        self.assertTrue(done.wait(2))

    def test_tooltip_notify_icon_stop(self):
        ready = []
        tray = self.bp.PystrayTray(on_ready=lambda: ready.append(1), pystray_module=self.mod)
        tray._mac = None   # no AppKit run loop in a unit test: run UI calls directly (also on macOS)
        tray.set_tooltip("Claudius Maximus", ["Device: not found", "Sessions: none"])
        self.assertIn("Device: not found", tray.icon.title)
        self.assertLessEqual(len(self.bp.tooltip_text("x", ["y" * 400])), 127)
        tray.set_icon(render_tray_icon(22, {}))
        self.assertEqual(tray.icon.icon.size, (64, 64))
        tray.notify("Title", "Body")
        self.assertEqual(tray.icon.notified, [("Title", "Body")])
        tray.run()
        self.assertEqual(ready, [1])
        self.assertTrue(tray.icon.visible)
        tray.stop()
        self.assertTrue(tray.icon.stopped)

    def test_win32_updates_run_on_ui_thread_and_wait_for_closed_menu(self):
        """Windows: set_* post to pystray's window; a menu swap while the popup is open waits for it to close."""
        posted = []
        with mock.patch("sys.platform", "win32"):
            tray = self.bp.PystrayTray(pystray_module=self.mod)
        icon = tray.icon
        icon._hwnd = 1234
        opened = []

        def pystray_on_notify(w, l):   # pystray's handler: TrackPopupMenuEx runs a modal loop in here
            opened.append(icon.menu)
            ui.drain()                  # the modal loop dispatches our posted message meanwhile
            opened.append(icon.menu)
            return 0
        icon._message_handlers = {self.bp.WM_PYSTRAY_NOTIFY: pystray_on_notify}
        ui = tray._win
        ui._post = lambda: posted.append(1) or True
        tray._running = True
        self.assertTrue(ui.install())
        app = FakeApp()
        tray.set_menu(appmod.build_menu(STATES["up_to_date"], app))
        self.assertEqual(posted, [1])
        icon.menu = "old"   # nothing ran yet on the "UI thread"
        tray.set_tooltip("T", ["x"])
        self.assertNotEqual(icon.title, self.bp.tooltip_text("T", ["x"]))
        icon._message_handlers[self.bp.WM_PYSTRAY_NOTIFY](0, 0x0205)   # right click: menu opens, then closes
        self.assertEqual(opened[0], "old")
        self.assertEqual(opened[1], "old")           # not swapped under the open popup
        self.assertIsInstance(icon.menu, self.mod.Menu)   # applied once it closed
        self.assertEqual(icon.title, self.bp.tooltip_text("T", ["x"]))   # non-menu updates ran during the modal loop

    def test_win32_without_internals_runs_directly(self):
        with mock.patch("sys.platform", "win32"):
            tray = self.bp.PystrayTray(pystray_module=self.mod)
        self.assertFalse(tray._win.install())   # fake Icon has no _message_handlers/_hwnd
        tray._running = True
        tray.set_tooltip("T", [])
        self.assertEqual(tray.icon.title, "T")

    def test_win32_notification_limits(self):
        with mock.patch("sys.platform", "win32"):
            tray = self.bp.PystrayTray(pystray_module=self.mod)
            tray.notify("t" * 100, "b" * 400)
        (title, body), = tray.icon.notified
        self.assertEqual((len(title), len(body)), (63, 255))
        self.assertEqual(len(self.bp.cut_utf16("\U0001F600" * 100, 127).encode("utf-16-le")) // 2, 127)
        self.assertEqual(self.bp.cut_utf16("short", 127), "short")

    def test_darwin_marshalling(self):
        after, later = [], []
        helpers = (lambda f, *a: after.append((f, a)), lambda d, f, *a: later.append((d, f, a)))
        with mock.patch("sys.platform", "darwin"), mock.patch.object(self.bp, "_mac_helpers", return_value=helpers):
            tray = self.bp.PystrayTray(pystray_module=self.mod)
            tray._running = True
            tray.set_tooltip("T", ["x"])
            tray.set_menu(appmod.build_menu(STATES["up_to_date"], FakeApp()))
            done = threading.Event()
            tray.icon.notify = lambda m, t=None: done.set()
            tray.notify("a", "b")
            tray.stop()
        self.assertTrue(done.wait(2))            # notify: own thread (osascript), not the main thread
        self.assertNotEqual(tray.icon.title, "T - x")
        self.assertEqual(len(later), 1)          # menu: callLater (default run loop mode: not while open)
        self.assertGreater(later[0][0], 0)       # a zero delay would run it immediately, even mid-tracking
        self.assertEqual(len(after), 2)          # tooltip + stop on the main thread
        for f, a in after:
            f(*a)
        later[0][1](*later[0][2])
        self.assertEqual(tray.icon.title, "T - x")
        self.assertIsInstance(tray.icon.menu, self.mod.Menu)
        self.assertTrue(tray.icon.stopped)

    def test_pystray_not_imported_on_linux(self):
        sys.modules.pop("pystray", None)
        import importlib
        import companion.tray.backend_pystray as bp
        importlib.reload(bp)
        self.assertNotIn("pystray", sys.modules)

    def test_make_backend_by_platform(self):
        import companion.tray as tray_pkg
        with mock.patch("sys.platform", "win32"):
            b = tray_pkg.make_backend(None, None)
        self.assertEqual(type(b).__name__, "PystrayTray")


# ---------------------------------------------------------------------------------------------
@unittest.skipUnless(HAVE_DBUS_FAST, "dbus-fast not installed (Linux only)")
class SNITest(unittest.TestCase):
    def setUp(self):
        from companion.tray import backend_sni
        self.sni = backend_sni

    def _menu(self, st=None, app=None):
        return appmod.build_menu(st or STATES["up_to_date"], app or FakeApp())

    def test_layout_variant_structure(self):
        from dbus_fast import Message, Variant
        tray = self.sni.SNITray()
        tray.set_menu(self._menu())
        call = getattr(self.sni.DBusMenu.GetLayout, "__wrapped__", self.sni.DBusMenu.GetLayout)
        rev, root = call(tray.menu, 0, -1, [])
        self.assertIsInstance(rev, int)
        rid, rprops, kids = root
        self.assertEqual(rid, 0)
        self.assertEqual(rprops["children-display"], Variant("s", "submenu"))
        self.assertTrue(all(isinstance(k, Variant) and k.signature == "(ia{sv}av)" for k in kids))
        first = kids[0].value
        self.assertEqual(first[1]["label"].value, "Device: connected on /dev/ttyACM0 · fw 0.3.0 (4B)")
        self.assertEqual(first[1]["enabled"], Variant("b", False))
        sep = kids[3].value
        self.assertEqual(sep[1], {"type": Variant("s", "separator")})
        flash = next(k.value for k in kids if k.value[1].get("label", Variant("s", "")).value == "Flash firmware")
        self.assertEqual(flash[1]["children-display"].value, "submenu")
        self.assertEqual([c.value[1]["toggle-state"].value for c in flash[2]], [1, 0])
        self.assertEqual(flash[2][0].value[1]["toggle-type"].value, "checkmark")
        # depth 0: no children; the whole reply marshals with the method's signature
        self.assertEqual(call(tray.menu, 0, 0, [])[1][2], [])
        Message(path="/MenuBar", member="GetLayout", signature="u(ia{sv}av)", body=[rev, root])._marshall(False)
        # property filter
        _, (_, props, _) = call(tray.menu, first[0], -1, ["label"])
        self.assertEqual(list(props), ["label"])

    def test_ids_stable_and_revision(self):
        m = self.sni.MenuModel()
        self.assertTrue(m.set(self._menu()))
        rev = m.revision
        quit_id = next(i for i, it in m.nodes.items() if it.label == "Quit")
        self.assertFalse(m.set(self._menu()), "unchanged menu must not bump the revision")
        self.assertEqual(m.revision, rev)
        self.assertTrue(m.set(self._menu(STATES["update"])))
        self.assertGreater(m.revision, rev)
        self.assertEqual(next(i for i, it in m.nodes.items() if it.label == "Quit"), quit_id)
        self.assertEqual(len(set(m.nodes)), len(m.nodes))

    def test_click_runs_action_and_group_props(self):
        done = threading.Event()
        app = FakeApp()
        app.open_log = lambda: done.set()
        tray = self.sni.SNITray()
        tray.set_menu(self._menu(app=app))
        iid = next(i for i, it in tray.model.nodes.items() if it.label == "Open log")
        ev = getattr(self.sni.DBusMenu.Event, "__wrapped__", self.sni.DBusMenu.Event)
        from dbus_fast import Variant
        ev(tray.menu, iid, "clicked", Variant("s", ""), 0)
        self.assertTrue(done.wait(2))
        disabled = next(i for i, it in tray.model.nodes.items() if not it.enabled and not it.separator)
        tray.clicked(disabled)   # no action, no crash
        gp = getattr(self.sni.DBusMenu.GetGroupProperties, "__wrapped__", self.sni.DBusMenu.GetGroupProperties)
        rows = gp(tray.menu, [iid], [])
        self.assertEqual(rows, [[iid, {"label": Variant("s", "Open log")}]])

    def test_pixmap_argb_network_order(self):
        from PIL import Image
        img = Image.new("RGBA", (2, 1), (10, 20, 30, 40))
        w, h, data = self.sni.argb_pixmap(img)
        self.assertEqual((w, h), (2, 1))
        self.assertEqual(data[:4], bytes([40, 10, 20, 30]))
        tray = self.sni.SNITray()
        tray.set_icon_renderer(lambda s: render_tray_icon(s, STATES["up_to_date"]))
        self.assertEqual([p[0] for p in tray.pixmaps], list(self.sni.PIXMAP_SIZES))
        self.assertTrue(all(len(p[2]) == p[0] * p[1] * 4 for p in tray.pixmaps))
        prop = getattr(self.sni.StatusNotifierItem.IconPixmap, "fget", None)
        if prop is not None:
            self.assertEqual(len(prop(tray.sni)), 4)
        tray.set_tooltip("Claudius Maximus", ["a < b", "c"])
        tip = getattr(self.sni.StatusNotifierItem.ToolTip, "fget")(tray.sni)
        self.assertEqual(tip, ["", [], "Claudius Maximus", "a &lt; b<br>c"])

    def test_underscore_escaped(self):
        self.assertEqual(self.sni.menu_label("cu.usbmodem_1"), "cu.usbmodem__1")


# ---------------------------------------------------------------------------------------------
class AppIntegrationTest(unittest.TestCase):
    """Review fixes: install-needed heuristic, first-run retry, quit during a flash, --seconds timer, stale hooks."""

    def setUp(self):
        self.tmp = __import__("tempfile").mkdtemp()
        self.env = mock.patch.dict(os.environ, {"CLAUDIUS_HOME": self.tmp})
        self.env.start()

    def tearDown(self):
        self.env.stop()
        __import__("shutil").rmtree(self.tmp, ignore_errors=True)

    def _app(self, daemon_status, ports):
        fake_flasher = types.ModuleType("companion.flasher")
        fake_flasher.detect = lambda: ports
        fake_flasher.needs_update = lambda fw, board: (False, "up to date")
        fake_flasher.bundled = lambda: {"4b": object()}
        app = appmod.App(types.SimpleNamespace(no_setup=True), backend=None, start_daemon=False)
        app.daemon = types.SimpleNamespace(public_status=lambda: dict(daemon_status))
        app.autostart = lambda: False
        app.hooks_installed = lambda: True
        import companion
        patches = (mock.patch.dict(sys.modules, {"companion.flasher": fake_flasher}),
                   mock.patch.object(companion, "flasher", fake_flasher, create=True))
        return app, patches

    def _install_needed_after(self, daemon_status, ports, seconds=11):
        app, (p1, p2) = self._app(daemon_status, ports)
        t = [1000.0]
        with p1, p2, mock.patch.object(appmod.time, "time", lambda: t[0]):
            app.status()
            t[0] += seconds
            return app.status()["install_needed"]

    def test_install_needed_only_when_port_open_without_hello(self):
        self.assertTrue(self._install_needed_after({"connected": True, "fw": None}, [NATIVE]))
        self.assertFalse(self._install_needed_after({"connected": True, "fw": None}, [NATIVE], seconds=5))
        # a dev tool holds the device lock: the port is not ours, no conclusion about the firmware
        self.assertFalse(self._install_needed_after(
            {"connected": False, "fw": None, "device_error": "device lock held by another tool"}, [NATIVE]))
        # only the CH343 port: the link can't work there, the Flash submenu stays
        self.assertFalse(self._install_needed_after({"connected": False, "fw": None}, [UART]))
        self.assertFalse(self._install_needed_after({"connected": True, "fw": "0.3.0"}, [NATIVE]))

    def test_first_run_retried_when_hooks_fail_autostart_once(self):
        backend = mock.Mock()
        app = appmod.App(types.SimpleNamespace(no_setup=True), backend=backend, start_daemon=False)
        app.install_hooks = mock.Mock(side_effect=[False, True])
        app._refresh_stale_hooks = mock.Mock()
        app.set_autostart = mock.Mock(return_value="will start at login")
        store = {}
        with mock.patch.object(appmod, "settings_get", lambda k, d=None: store.get(k, d)), \
                mock.patch.object(appmod, "settings_set", store.__setitem__), \
                mock.patch.object(appmod, "installer_handles_autostart", return_value=False):
            app._first_run()
            self.assertNotIn("first_run_done", store)
            self.assertIn("Could not install", backend.notify.call_args.args[1])
            app._first_run()
            self.assertTrue(store["first_run_done"])
            app._first_run()
        self.assertEqual(app.install_hooks.call_count, 2)
        app.set_autostart.assert_called_once_with(True, notify=False)

    def test_stale_hooks_reinstalled_on_later_starts(self):
        app = appmod.App(types.SimpleNamespace(no_setup=True), backend=None, start_daemon=False)
        app.install_hooks = mock.Mock(return_value=True)
        cm = types.SimpleNamespace(installed=lambda: {"hooks": True}, installed_matches_self=lambda: False)
        with mock.patch.object(appmod, "settings_get", lambda k, d=None: True), \
                mock.patch.object(appmod, "load_cm_settings", return_value=cm):
            app._first_run()
            app.install_hooks.assert_called_once_with(notify=False)
            cm.installed_matches_self = lambda: True
            app._first_run()
            cm.installed, cm.installed_matches_self = (lambda: {"hooks": False}), (lambda: False)
            app._first_run()       # the user removed the hooks: leave them removed
        self.assertEqual(app.install_hooks.call_count, 1)

    def test_quit_waits_for_flash_then_stops_daemon(self):
        order = []
        release = threading.Event()
        backend = mock.Mock()
        backend.stop.side_effect = lambda: order.append("tray stop")
        app = appmod.App(types.SimpleNamespace(no_setup=True), backend=backend, start_daemon=False)

        def worker():
            release.wait(2)
            order.append("flash done")
        app._flash_thread = threading.Thread(target=worker)
        app._flash_thread.start()
        d = types.SimpleNamespace(stop=False)
        app.daemon = d
        threading.Timer(0.2, release.set).start()
        app.quit()
        self.assertEqual(order, ["flash done", "tray stop"])
        self.assertTrue(d.stop)

    def test_seconds_timer_is_daemon(self):
        timers = []
        real_timer = threading.Timer

        def timer(*a, **kw):
            t = real_timer(*a, **kw)
            timers.append(t)
            return t

        class FakeBackend:
            def __init__(self, on_ready, on_quit):
                self.stopped = threading.Event()

            def run(self):
                self.stopped.wait(5)

            def stop(self):
                self.stopped.set()

            def __getattr__(self, name):
                return lambda *a, **kw: None

        class FakeApp2:
            def __init__(self, opts, backend):
                self.backend = backend

            def refresh(self):
                pass

            def quit(self):
                self.backend.stop()
        with mock.patch("companion.tray.make_backend", FakeBackend), mock.patch.object(appmod, "App", FakeApp2), \
                mock.patch.object(appmod, "setup_logging"), mock.patch.object(appmod.threading, "Timer", timer):
            t0 = time.time()
            self.assertEqual(appmod.main(["app", "--no-setup", "--seconds", "0.1"]), 0)
        self.assertLess(time.time() - t0, 3)
        self.assertTrue(timers and all(t.daemon for t in timers))


@unittest.skipUnless(HAVE_DBUS_FAST, "dbus-fast not installed")
class SNILifecycleTest(unittest.TestCase):
    def setUp(self):
        from companion.tray import backend_sni
        self.sni = backend_sni

    def test_stop_before_run_returns(self):
        tray = self.sni.SNITray()
        tray.stop()                       # e.g. --seconds elapsed / Quit before the loop started
        done = threading.Event()
        with mock.patch.object(self.sni, "MessageBus") as mb:
            th = threading.Thread(target=lambda: (tray.run(), done.set()), daemon=True)
            th.start()
            self.assertTrue(done.wait(3))
        mb.assert_not_called()

    def test_register_retry_until_watcher_accepts(self):
        import asyncio
        tray = self.sni.SNITray()
        calls = []

        async def register():
            calls.append(1)
            tray.registered = len(calls) >= 3

        tray._register = register

        async def go():
            with mock.patch.object(self.sni, "REGISTER_RETRY_S", 0.01):
                t = asyncio.ensure_future(tray._register_retry())
                await asyncio.sleep(0.3)
                t.cancel()
        asyncio.run(go())
        self.assertEqual(len(calls), 3, "keeps retrying while unregistered, stops once registered")

    def test_bus_loss_quits(self):
        import asyncio
        quit_called = threading.Event()
        tray = self.sni.SNITray(on_quit=quit_called.set)

        class Bus:
            async def wait_for_disconnect(self):
                return None

        async def go():
            tray._stop = asyncio.Event()
            tray.bus = Bus()
            await tray._bus_gone()
        with self.assertLogs("companion.tray", "WARNING"):
            asyncio.run(go())
        self.assertTrue(quit_called.wait(2))


if __name__ == "__main__":
    unittest.main()
