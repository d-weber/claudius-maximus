"""Which USB serial port is our board?

One ESP32-S3 on the bus: no question, the daemon and the flasher take its native USB-Serial-JTAG port. Several
(another ESP32-S3 project on the desk, a second board): the native ports all look alike (same VID:PID), so the
daemon asks each one for a hello ({"t":"cmd","hello":1}) and keeps the one that answers as Claudius Maximus
firmware. A board that answers nothing (blank, or running other firmware) is never picked by itself: the user
chooses it in the tray menu (Device port) or with `claudius-maximus port <name>`, and that choice is kept in the
app's settings.json (`port` = device name, `port_serial` = its USB serial, matched first because names change:
COM6 -> COM7, a new cu.usbmodem*). The flasher uses the same choice, so "the port the daemon talks to" and
"the port we flash" are always the same one.

Pure logic here (Chooser), probing through link.probe_hello; flasher.detect() does the enumeration.
"""
from __future__ import annotations

import logging
import time
from dataclasses import dataclass, field
from typing import Callable

from . import hostos

log = logging.getLogger("companion.ports")

SETTING_PORT = "port"            # user's choice: a device name; absent/"" = automatic
SETTING_SERIAL = "port_serial"   # USB serial of that device when it had one (matched before the name)
PROBE_TIMEOUT_S = 1.5
PROBE_RETRY_S = 60.0             # a native port that did not answer is asked again after this long
AUTO = "auto"


# -- the user's choice ---------------------------------------------------------------------------------------
def selection() -> tuple[str | None, str | None]:
    """(device, serial) chosen by the user, (None, None) = automatic."""
    dev = hostos.settings_get(SETTING_PORT) or None
    return (str(dev), hostos.settings_get(SETTING_SERIAL) or None) if dev else (None, None)


def select(device: str | None, ports=None) -> str:
    """Store the choice (None / "" / "auto" = automatic). Returns a sentence for the user. Raises OSError if
    settings.json is not writable."""
    if not device or device.lower() == AUTO:
        hostos.settings_set(SETTING_PORT, None)
        hostos.settings_set(SETTING_SERIAL, None)
        return "device port: automatic"
    serial = None
    for p in ports or []:
        if p.device == device:
            serial = p.serial
            break
    hostos.settings_set(SETTING_PORT, device)
    hostos.settings_set(SETTING_SERIAL, serial)
    return f"device port: {device}" + (f" (USB serial {serial})" if serial else "")


def resolve(ports, device: str | None, serial: str | None):
    """The enumerated Port the choice points at: by serial first, then by name. None if it is not plugged in."""
    if serial:
        for p in ports:
            if p.serial == serial:
                return p
    if device:
        for p in ports:
            if p.device == device:
                return p
    return None


# -- picking a port --------------------------------------------------------------------------------------------
def _port_dict(p) -> dict:
    return {"device": p.device, "kind": p.kind, "vid": p.vid, "pid": p.pid, "serial": p.serial,
            "description": p.description}


@dataclass
class Chooser:
    """Decides which port the daemon opens. `probe(device)` returns the hello dict of a Claudius Maximus
    firmware on that port, or None (default: link.probe_hello). Remembers the board that answered (by USB
    serial) so the choice survives reconnects, replugs and renames without probing again."""
    probe: Callable[[str], dict | None] | None = None
    now: Callable[[], float] = time.time
    known_serial: str | None = None       # the board that said hello (probe, or the daemon's connection)
    known_device: str | None = None
    hellos: dict[str, dict] = field(default_factory=dict)      # device -> last hello seen on it
    failed_at: dict[str, float] = field(default_factory=dict)  # device -> when a probe got no answer
    reason: str = ""                                            # why choose() returned None
    manual_missing: bool = False

    def _probe(self, device: str) -> dict | None:
        if self.probe is None:
            from .link import probe_hello
            self.probe = probe_hello
        try:
            return self.probe(device)
        except Exception as e:   # a probe must never take the daemon down
            log.debug("probe %s failed: %s", device, e)
            return None

    def learned(self, device: str | None, hello: dict, ports=None) -> None:
        """The daemon got a hello on `device`: that is our board."""
        if not device:
            return
        self.hellos[device] = dict(hello)
        self.failed_at.pop(device, None)
        self.known_device = device
        for p in ports or []:
            if p.device == device and p.serial:
                self.known_serial = p.serial

    def choose(self, ports, manual: tuple[str | None, str | None] = (None, None), override: str | None = None):
        """-> Port or None (then .reason says why). `override` = a --port on the command line (always used,
        even if not enumerated); `manual` = selection() from the settings."""
        from .flasher import Port
        self.reason, self.manual_missing = "", False
        if override:
            for p in ports:
                if p.device == override:
                    return p
            return Port(device=override, kind="other", vid=0, pid=0, serial=None, description="")
        device, serial = manual
        if device:
            p = resolve(ports, device, serial)
            if p is None:
                self.manual_missing = True
                self.reason = f"the chosen port {device} is not connected (Device port in the menu to change it)"
            return p
        natives = [p for p in ports if p.kind == "native"]
        if not natives:
            self.reason = "no device found"
            return None
        if len(natives) == 1:
            return natives[0]
        # several ESP32-S3: the one we know, else the one that answers as our firmware
        if self.known_serial:
            for p in natives:
                if p.serial == self.known_serial:
                    return p
        if self.known_device and not any(p.serial for p in natives):   # no serials at all (some drivers)
            for p in natives:
                if p.device == self.known_device:
                    return p
        now = self.now()
        for p in natives:
            if now - self.failed_at.get(p.device, -1e9) < PROBE_RETRY_S:
                continue
            hello = self._probe(p.device)
            if hello:
                log.info("%s answers as Claudius Maximus firmware %s: using it", p.device, hello.get("fw"))
                self.learned(p.device, hello, natives)
                return p
            self.failed_at[p.device] = now
        self.reason = (f"{len(natives)} ESP32-S3 boards on USB and none runs the Claudius Maximus firmware: "
                       "choose the port (Device port in the menu, or `claudius-maximus port <name>`)")
        return None

    def describe(self, ports, connected: str | None = None, fw: str | None = None, board: str | None = None,
                 manual: tuple[str | None, str | None] = (None, None)) -> list[dict]:
        """status.json `ports`: the enumerated ports plus what we know about each (ours? firmware? chosen?)."""
        out = []
        device, serial = manual
        chosen = resolve(ports, device, serial)
        for p in ports:
            d = _port_dict(p)
            hello = self.hellos.get(p.device) or {}
            d["connected"] = bool(connected) and p.device == connected
            d["selected"] = chosen is not None and p.device == chosen.device
            d["fw"] = fw if d["connected"] and fw else hello.get("fw")
            d["board"] = board if d["connected"] and board else hello.get("board")
            d["ours"] = bool(d["fw"]) or (self.known_serial is not None and p.serial == self.known_serial)
            out.append(d)
        return out


def port_label(p: dict) -> str:
    """One line for a menu or `port` listing: "/dev/ttyACM0 · ESP32-S3 · Claudius Maximus fw 0.4.1"."""
    kind = p.get("kind")
    what = ("ESP32-S3" if kind == "native" else "USB-TO-UART (CH343)" if kind == "uart"
            else (p.get("description") or "serial port"))
    s = f"{p.get('device')} · {what}"
    if p.get("fw"):
        s += f" · Claudius Maximus fw {p['fw']}"
    elif p.get("ours"):
        s += " · Claudius Maximus"
    return s
