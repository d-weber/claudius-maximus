"""USB-serial JSON-lines link to the device (see PROTOCOL.md)."""
from __future__ import annotations

import json
import time

import serial
from serial.tools import list_ports

ESPRESSIF_VID = 0x303A


def find_port() -> str | None:
    for p in list_ports.comports():
        if p.vid == ESPRESSIF_VID:
            return p.device
    for p in list_ports.comports():
        if "usbmodem" in p.device:
            return p.device
    return None


class DeviceLink:
    def __init__(self, port: str | None = None):
        self.port_name = port
        self.ser: serial.Serial | None = None
        self._buf = b""

    def connect(self) -> bool:
        port = self.port_name or find_port()
        if not port:
            return False
        try:
            # Don't toggle DTR/RTS: on the ESP32-S3 USB-JTAG that can reset the chip.
            s = serial.Serial()
            s.port = port
            s.baudrate = 115200
            s.timeout = 0
            s.write_timeout = 2
            s.dtr = False
            s.rts = False
            s.open()
            self.ser = s
            self._buf = b""
            return True
        except (serial.SerialException, OSError):
            self.ser = None
            return False

    @property
    def connected(self) -> bool:
        return self.ser is not None

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None

    def send(self, msg: dict) -> bool:
        if not self.ser:
            return False
        try:
            self.ser.write((json.dumps(msg, separators=(",", ":"), ensure_ascii=True) + "\n").encode())
            return True
        except (serial.SerialException, OSError):
            self.close()
            return False

    def read_messages(self) -> list[dict]:
        """Non-blocking: return any complete JSON lines received (non-JSON lines ignored)."""
        if not self.ser:
            return []
        try:
            n = self.ser.in_waiting
            if n:
                self._buf += self.ser.read(n)
        except (serial.SerialException, OSError):
            self.close()
            return []
        out = []
        while b"\n" in self._buf:
            line, self._buf = self._buf.split(b"\n", 1)
            line = line.strip()
            if line.startswith(b"{"):
                try:
                    out.append(json.loads(line))
                except ValueError:
                    pass
        return out

    def screenshot(self, timeout: float = 10.0) -> tuple[int, int, bytes, int]:
        """Request a framebuffer dump. Returns (w, h, rgb565le bytes, rotation)."""
        assert self.ser
        self.ser.reset_input_buffer()
        self._buf = b""
        self.send({"t": "cmd", "shot": 1})
        deadline = time.time() + timeout
        self.ser.timeout = 0.5
        hdr = None
        while time.time() < deadline and hdr is None:
            line = self.ser.readline()
            if line.startswith(b'{"t":"shot"'):
                hdr = json.loads(line)
        if not hdr:
            raise TimeoutError("no screenshot header")
        need = hdr["len"]
        data = b""
        while len(data) < need and time.time() < deadline:
            data += self.ser.read(need - len(data))
        self.ser.timeout = 0
        if len(data) < need:
            raise TimeoutError(f"screenshot truncated {len(data)}/{need}")
        return hdr["w"], hdr["h"], data, hdr.get("rot", 0)
