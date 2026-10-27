"""Linux tray: StatusNotifierItem + com.canonical.dbusmenu over D-Bus, with dbus-fast (pure Python).

Modelled on ~/projects/claude_system_indicator/claude_tray.py (Gio), which works on KDE Plasma Wayland:
 * own a unique name org.kde.StatusNotifierItem-<pid>-1 and export the item at /StatusNotifierItem and the
   menu at /MenuBar, then org.kde.StatusNotifierWatcher.RegisterStatusNotifierItem(name);
 * IconPixmap is a(iiay) of ARGB32 in network byte order, several sizes so the panel never upscales;
 * ItemIsMenu=true: a click opens the menu (GetLayout); NewIcon / NewToolTip / LayoutUpdated tell the
   host to re-read;
 * the watcher's NameOwnerChanged is watched, so a restarted plasmashell gets us again.
GNOME needs the AppIndicator extension for SNI; without a watcher the app keeps running and logs it.

Threading: run() owns an asyncio loop on the main thread. set_* can be called from any thread: they store the
new value and schedule the publish with loop.call_soon_threadsafe. Menu actions run on a worker thread so a
slow action (hooks install, flashing) never blocks D-Bus.
"""
import asyncio
import html
import logging
import os
import signal as _signal
import threading

from dbus_fast import BusType, Message, MessageType, Variant
from dbus_fast.aio import MessageBus
from dbus_fast.constants import PropertyAccess
from dbus_fast.service import ServiceInterface, dbus_property, method, signal

from .base import MenuItem, TrayBackend

log = logging.getLogger("companion.tray")

SNI_PATH = "/StatusNotifierItem"
MENU_PATH = "/MenuBar"
WATCHER = "org.kde.StatusNotifierWatcher"
PIXMAP_SIZES = (22, 32, 44, 64)


def argb_pixmap(img) -> tuple[int, int, bytes]:
    """PIL image -> (w, h, ARGB32 big-endian bytes) as SNI wants (same bytes as claude_tray's cairo byteswap)."""
    from PIL import Image
    img = img.convert("RGBA")
    r, g, b, a = img.split()
    return img.width, img.height, Image.merge("RGBA", (a, r, g, b)).tobytes()


def menu_label(text: str) -> str:
    """dbusmenu treats '_' as a mnemonic marker: double it to show it literally."""
    return text.replace("_", "__")


class MenuModel:
    """MenuItem tree -> dbusmenu ids/properties/layout. Ids are stable for an unchanged label path so a click on
    an item that was rebuilt meanwhile still lands on the same entry."""

    def __init__(self):
        self.revision = 1
        self.items: list[MenuItem] = []
        self.nodes: dict[int, MenuItem] = {}
        self.children: dict[int, list[int]] = {0: []}
        self._ids: dict[tuple, int] = {}
        self._next = 1
        self._key = None

    def set(self, items: list[MenuItem]) -> bool:
        """Returns True when the layout changed (the caller then emits LayoutUpdated)."""
        key = tuple(i.key() for i in items)
        if key == self._key:
            # same structure: keep ids, but take the new callables (they may close over fresh values)
            self._rebind(items)
            return False
        self._key = key
        self.items = items
        ids, nodes, children = {}, {}, {0: []}

        def walk(parent_id, path, lst):
            for idx, it in enumerate(lst):
                k = path + ((idx, "-") if it.separator else (it.label,),)
                iid = self._ids.get(k) or ids.get(k)
                if iid is None or iid in nodes:
                    iid, self._next = self._next, self._next + 1
                ids[k] = iid
                nodes[iid] = it
                children[parent_id].append(iid)
                if it.children is not None:
                    children[iid] = []
                    walk(iid, k, it.children)
        walk(0, (), items)
        self._ids, self.nodes, self.children = ids, nodes, children
        self.revision += 1
        return True

    def _rebind(self, items):
        def walk(parent_id, lst):
            for iid, it in zip(self.children.get(parent_id, []), lst):
                self.nodes[iid] = it
                if it.children is not None:
                    walk(iid, it.children)
        walk(0, items)
        self.items = items

    def props(self, iid: int, names=None) -> dict:
        if iid == 0:
            p = {"children-display": Variant("s", "submenu")}
        else:
            it = self.nodes.get(iid)
            if it is None:
                return {}
            if it.separator:
                p = {"type": Variant("s", "separator")}
            else:
                p = {"label": Variant("s", menu_label(it.label))}
                if not it.enabled:
                    p["enabled"] = Variant("b", False)
                if it.checked is not None:
                    p["toggle-type"] = Variant("s", "checkmark")
                    p["toggle-state"] = Variant("i", 1 if it.checked else 0)
                if it.children is not None:
                    p["children-display"] = Variant("s", "submenu")
        if names:
            p = {k: v for k, v in p.items() if k in names}
        return p

    def layout(self, parent_id: int = 0, depth: int = -1, names=None):
        """(ia{sv}av) for GetLayout: children are Variant('(ia{sv}av)', ...) as the spec's 'av' requires."""
        kids = []
        if depth != 0:
            for cid in self.children.get(parent_id, []):
                kids.append(Variant("(ia{sv}av)", self.layout(cid, depth - 1 if depth > 0 else -1, names)))
        return [parent_id, self.props(parent_id, names), kids]

    def action(self, iid: int):
        it = self.nodes.get(iid)
        if it is None or it.separator or not it.enabled:
            return None
        return it.action


class StatusNotifierItem(ServiceInterface):
    def __init__(self, tray: "SNITray"):
        super().__init__("org.kde.StatusNotifierItem")
        self.tray = tray

    @dbus_property(access=PropertyAccess.READ)
    def Category(self) -> "s":
        return "ApplicationStatus"

    @dbus_property(access=PropertyAccess.READ)
    def Id(self) -> "s":
        return "claudius-maximus"

    @dbus_property(access=PropertyAccess.READ)
    def Title(self) -> "s":
        return self.tray.title or "Claudius Maximus"

    @dbus_property(access=PropertyAccess.READ)
    def Status(self) -> "s":
        return "Active"

    @dbus_property(access=PropertyAccess.READ)
    def WindowId(self) -> "i":
        return 0

    @dbus_property(access=PropertyAccess.READ)
    def IconName(self) -> "s":
        return ""

    @dbus_property(access=PropertyAccess.READ)
    def IconPixmap(self) -> "a(iiay)":
        return [list(p) for p in self.tray.pixmaps]

    @dbus_property(access=PropertyAccess.READ)
    def ToolTip(self) -> "(sa(iiay)ss)":
        body = "<br>".join(html.escape(line, quote=False) for line in self.tray.lines)
        return ["", [], self.tray.title or "Claudius Maximus", body]

    @dbus_property(access=PropertyAccess.READ)
    def ItemIsMenu(self) -> "b":
        return True

    @dbus_property(access=PropertyAccess.READ)
    def Menu(self) -> "o":
        return MENU_PATH

    @method()
    def ContextMenu(self, x: "i", y: "i"):
        pass

    @method()
    def Activate(self, x: "i", y: "i"):
        pass

    @method()
    def SecondaryActivate(self, x: "i", y: "i"):
        pass

    @method()
    def Scroll(self, delta: "i", orientation: "s"):
        pass

    @signal()
    def NewIcon(self):
        pass

    @signal()
    def NewToolTip(self):
        pass

    @signal()
    def NewTitle(self):
        pass

    @signal()
    def NewStatus(self) -> "s":
        return "Active"


class DBusMenu(ServiceInterface):
    def __init__(self, tray: "SNITray"):
        super().__init__("com.canonical.dbusmenu")
        self.tray = tray
        self.model = tray.model

    @dbus_property(access=PropertyAccess.READ)
    def Version(self) -> "u":
        return 3

    @dbus_property(access=PropertyAccess.READ)
    def TextDirection(self) -> "s":
        return "ltr"

    @dbus_property(access=PropertyAccess.READ)
    def Status(self) -> "s":
        return "normal"

    @dbus_property(access=PropertyAccess.READ)
    def IconThemePath(self) -> "as":
        return []

    @method()
    def GetLayout(self, parent_id: "i", recursion_depth: "i", property_names: "as") -> "u(ia{sv}av)":
        return [self.model.revision, self.model.layout(parent_id, recursion_depth, property_names)]

    @method()
    def GetGroupProperties(self, ids: "ai", property_names: "as") -> "a(ia{sv})":
        want = ids or list(self.model.nodes)
        return [[i, self.model.props(i, property_names)] for i in want if i == 0 or i in self.model.nodes]

    @method()
    def GetProperty(self, id: "i", name: "s") -> "v":
        return self.model.props(id).get(name, Variant("s", ""))

    @method()
    def Event(self, id: "i", event_id: "s", data: "v", timestamp: "u"):
        if event_id == "clicked":
            self.tray.clicked(id)

    @method()
    def EventGroup(self, events: "a(isvu)") -> "ai":
        errors = []
        for iid, event_id, _data, _ts in events:
            if iid != 0 and iid not in self.model.nodes:
                errors.append(iid)
            elif event_id == "clicked":
                self.tray.clicked(iid)
        return errors

    @method()
    def AboutToShow(self, id: "i") -> "b":
        return False

    @method()
    def AboutToShowGroup(self, ids: "ai") -> "aiai":
        return [[], []]

    @signal()
    def ItemsPropertiesUpdated(self) -> "a(ia{sv})a(ias)":
        return [[], []]

    @signal()
    def LayoutUpdated(self) -> "ui":
        return [self.model.revision, 0]


class SNITray(TrayBackend):
    def __init__(self, on_ready=None, on_quit=None):
        super().__init__(on_ready, on_quit)
        self.title = "Claudius Maximus"
        self.lines: list[str] = []
        self.pixmaps: list[tuple[int, int, bytes]] = []
        self.model = MenuModel()
        self.loop: asyncio.AbstractEventLoop | None = None
        self.bus: MessageBus | None = None
        self.name = f"org.kde.StatusNotifierItem-{os.getpid()}-1"
        self._stop: asyncio.Event | None = None
        self._lock = threading.Lock()
        self._pending_menu: list[MenuItem] | None = None
        self.sni = StatusNotifierItem(self)
        self.menu = DBusMenu(self)
        self.registered = False

    # -- thread-safe setters ------------------------------------------------------------------
    def _soon(self, fn, *args):
        loop = self.loop
        if loop is not None and loop.is_running():
            try:
                loop.call_soon_threadsafe(fn, *args)
            except RuntimeError:   # loop closed while we were shutting down
                pass

    def set_icon(self, image) -> None:
        from PIL import Image
        img = image.convert("RGBA")
        self._set_pixmaps([argb_pixmap(img.resize((s, s), Image.LANCZOS) if img.width != s else img)
                           for s in PIXMAP_SIZES])

    def set_icon_renderer(self, render) -> None:
        self._set_pixmaps([argb_pixmap(render(s)) for s in PIXMAP_SIZES])

    def _set_pixmaps(self, pix):
        with self._lock:
            if pix == self.pixmaps:
                return
            self.pixmaps = pix
        self._soon(self._emit, self.sni.NewIcon)

    def set_tooltip(self, title: str, lines: list[str]) -> None:
        with self._lock:
            if (title, list(lines)) == (self.title, self.lines):
                return
            self.title, self.lines = title, list(lines)
        self._soon(self._emit, self.sni.NewToolTip)

    def set_menu(self, items: list[MenuItem]) -> None:
        with self._lock:
            self._pending_menu = list(items)
        if self.loop is None:
            self._apply_menu()
        else:
            self._soon(self._apply_menu)

    def _apply_menu(self):
        with self._lock:
            items, self._pending_menu = self._pending_menu, None
        if items is not None and self.model.set(items):
            self._emit(self.menu.LayoutUpdated)

    def _emit(self, sig):
        if self.bus is None:
            return
        try:
            sig()
        except Exception:
            log.debug("signal %s failed", getattr(sig, "__name__", sig), exc_info=True)

    def notify(self, title: str, body: str) -> None:
        self._soon(lambda: asyncio.ensure_future(self._notify(title, body)))

    def stop(self) -> None:
        self._soon(lambda: self._stop and self._stop.set())

    # -- menu clicks ----------------------------------------------------------------------------
    def clicked(self, iid: int):
        fn = self.model.action(iid)
        if fn is None:
            return
        threading.Thread(target=self._run_action, args=(fn,), name="tray-action", daemon=True).start()

    @staticmethod
    def _run_action(fn):
        try:
            fn()
        except Exception:
            log.exception("tray action failed")

    # -- D-Bus ----------------------------------------------------------------------------------
    async def _register(self):
        try:
            reply = await self.bus.call(Message(
                destination=WATCHER, path="/StatusNotifierWatcher", interface=WATCHER,
                member="RegisterStatusNotifierItem", signature="s", body=[self.name]))
            if reply.message_type == MessageType.ERROR:
                self.registered = False
                log.warning("tray: RegisterStatusNotifierItem failed: %s %s", reply.error_name, reply.body)
            else:
                self.registered = True
                log.info("tray: registered %s with %s", self.name, WATCHER)
        except Exception as e:
            self.registered = False
            log.warning("tray: cannot register with %s: %s", WATCHER, e)

    def _on_message(self, msg: Message):
        if (msg.message_type == MessageType.SIGNAL and msg.member == "NameOwnerChanged"
                and msg.interface == "org.freedesktop.DBus" and msg.body and msg.body[0] == WATCHER):
            new_owner = msg.body[2]
            if new_owner:
                log.info("tray: %s (re)appeared, registering again", WATCHER)
                asyncio.ensure_future(self._register())
            else:
                self.registered = False
                log.info("tray: %s went away", WATCHER)
        return None   # never consume: dbus-fast's own handlers need the messages too

    async def _notify(self, title: str, body: str):
        if self.bus is None:
            return
        hints = {}
        try:
            from .icon import render_app_icon
            img = render_app_icon(64).convert("RGBA")
            hints["image-data"] = Variant("(iiibiiay)", [img.width, img.height, img.width * 4, True, 8, 4,
                                                         img.tobytes()])
        except Exception:
            pass
        try:
            reply = await self.bus.call(Message(
                destination="org.freedesktop.Notifications", path="/org/freedesktop/Notifications",
                interface="org.freedesktop.Notifications", member="Notify", signature="susssasa{sv}i",
                body=["Claudius Maximus", 0, "", title, html.escape(body, quote=False), [], hints, 8000]))
            if reply.message_type == MessageType.ERROR:
                log.info("notification failed: %s", reply.error_name)
        except Exception as e:
            log.info("notification failed: %s", e)

    async def _main(self):
        self._stop = asyncio.Event()
        self.bus = await MessageBus(bus_type=BusType.SESSION).connect()
        self.bus.export(SNI_PATH, self.sni)
        self.bus.export(MENU_PATH, self.menu)
        await self.bus.request_name(self.name)
        self.bus.add_message_handler(self._on_message)
        await self.bus.call(Message(
            destination="org.freedesktop.DBus", path="/org/freedesktop/DBus", interface="org.freedesktop.DBus",
            member="AddMatch", signature="s",
            body=[f"type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                  f"member='NameOwnerChanged',arg0='{WATCHER}'"]))
        self._apply_menu()
        await self._register()
        if not self.registered:
            log.warning("tray: no StatusNotifierWatcher yet (GNOME needs the AppIndicator extension); "
                        "will register when one appears")
        for sig in (_signal.SIGINT, _signal.SIGTERM):
            try:
                self.loop.add_signal_handler(sig, self._quit_requested)
            except (NotImplementedError, RuntimeError, ValueError):
                pass
        if self.on_ready:
            try:
                self.on_ready()
            except Exception:
                log.exception("tray on_ready failed")
        await self._stop.wait()
        try:
            self.bus.disconnect()
        except Exception:
            pass

    def _quit_requested(self):
        if self.on_quit:
            threading.Thread(target=self._run_action, args=(self.on_quit,), daemon=True).start()
        else:
            self._stop.set()

    def run(self) -> None:
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        try:
            self.loop.run_until_complete(self._main())
        finally:
            self.loop.close()
            self.loop = None
            self.bus = None
