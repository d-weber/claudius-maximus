"""USB-serial JSON-lines link to the device (see PROTOCOL.md).

Device sharing: during development several tools use the one physical device. Everything that opens the
serial port takes an exclusive flock on <root>/.devlock (see tools/dev.py `device_lock`). `DeviceLink` can
do the same non-blockingly (`lock_path=`): `connect()` fails (and is retried later by the caller) while
someone else holds the lock, and `close()` always releases it. tools/dev.py constructs DeviceLink() without
a lock_path because it already holds the lock itself.
"""
from __future__ import annotations

import fcntl
import json
import os
import time

import serial
from serial.tools import list_ports

ESPRESSIF_VID = 0x303A
MAX_LINE = 64 * 1024  # drop runaway garbage lines


def find_port() -> str | None:
    try:
        ports = list_ports.comports()
    except Exception:
        return None
    for p in ports:
        if p.vid == ESPRESSIF_VID:
            return p.device
    for p in ports:
        if "usbmodem" in p.device:
            return p.device
    return None


class DeviceLink:
    def __init__(self, port: str | None = None, lock_path: str | os.PathLike | None = None):
        self.port_name = port
        self.lock_path = lock_path
        self.ser: serial.Serial | None = None
        self._buf = b""
        self._lock_fd: int | None = None
        self.last_error = ""
        self.connected_at = 0.0

    # -- device lock ---------------------------------------------------------------------------
    def _acquire_lock(self) -> bool:
        if not self.lock_path:
            return True
        if self._lock_fd is not None:
            return True
        try:
            fd = os.open(self.lock_path, os.O_CREAT | os.O_RDWR, 0o644)
        except OSError as e:
            self.last_error = f"lock open: {e}"
            return True  # can't even create the lock file: don't let that block the product
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            os.close(fd)
            self.last_error = "device lock held by another tool"
            return False
        try:
            os.ftruncate(fd, 0)
            os.write(fd, f"daemon pid={os.getpid()}\n".encode())
        except OSError:
            pass
        self._lock_fd = fd
        return True

    def _release_lock(self):
        if self._lock_fd is not None:
            try:
                fcntl.flock(self._lock_fd, fcntl.LOCK_UN)
            except OSError:
                pass
            try:
                os.close(self._lock_fd)
            except OSError:
                pass
            self._lock_fd = None

    def lock_holder(self) -> str:
        """Best effort: what the lock file says about its current holder."""
        try:
            with open(self.lock_path or "", "r") as f:
                return f.read().strip()
        except OSError:
            return ""

    # -- port ----------------------------------------------------------------------------------
    def connect(self) -> bool:
        if self.ser:
            return True
        port = self.port_name or find_port()
        if not port:
            self.last_error = "no device found"
            return False
        if not self._acquire_lock():
            return False
        try:
            # Leave DTR/RTS as the OS sets them on open (both asserted). Setting dtr=False, rts=False makes
            # pyserial drop DTR first, which briefly gives (DTR=0, RTS=1) = the ESP32-S3 USB-Serial-JTAG
            # reset condition: the device rebooted on every open (verified on the device, 2026-10-15).
            s = serial.Serial()
            s.port = port
            s.baudrate = 115200
            s.timeout = 0
            s.write_timeout = 2
            s.open()
            self.ser = s
            self._buf = b""
            self.last_error = ""
            self.connected_at = time.time()
            return True
        except (serial.SerialException, OSError, ValueError) as e:
            self.last_error = f"open {port}: {e}"
            self.ser = None
            self._release_lock()
            return False

    @property
    def connected(self) -> bool:
        return self.ser is not None

    @property
    def port(self) -> str | None:
        return self.ser.port if self.ser else None

    def fileno(self) -> int | None:
        try:
            return self.ser.fileno() if self.ser else None
        except Exception:
            return None

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self._buf = b""
        self._release_lock()

    def send(self, msg: dict) -> bool:
        if not self.ser:
            return False
        try:
            data = (json.dumps(msg, separators=(",", ":"), ensure_ascii=True) + "\n").encode()
            self.ser.write(data)
            return True
        except (serial.SerialException, OSError, ValueError, TypeError) as e:
            self.last_error = f"write: {e}"
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
        except (serial.SerialException, OSError, ValueError, TypeError) as e:
            self.last_error = f"read: {e}"
            self.close()
            return []
        out = []
        while b"\n" in self._buf:
            line, self._buf = self._buf.split(b"\n", 1)
            line = line.strip()
            if line.startswith(b"{"):
                try:
                    m = json.loads(line)
                except ValueError:
                    continue
                if isinstance(m, dict):
                    out.append(m)
        if len(self._buf) > MAX_LINE:
            self._buf = b""
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
