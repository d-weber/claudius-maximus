"""Board detection and firmware flashing for the desktop app (see docs/DESKTOP.md "Flasher").

The app ships prebuilt firmware images per board in companion/firmware/<board>/ (written by
tools/fw_bundle.py from the PlatformIO build, with a manifest.json) and writes them with esptool run
in-process: a frozen app has no Python interpreter to run `esptool` as a subprocess, and in-process we get
real progress callbacks instead of scraping a terminal.

Ports. The ESP32-S3's native USB-Serial-JTAG (303a:1001) is the host link port; esptool's
default reset sequence works on it (the USB-JTAG peripheral can reset the chip into the ROM loader), and the
port re-enumerates after the final reset (sometimes under a new name on macOS/Windows), so `flash()` waits for
it to come back and returns its new name. The 4B also has a "USB TO UART" port, a CH343 (1a86:55d3) wired to
UART0 with the classic DTR/RTS auto-reset circuit: flashing works there too, but the link does not (the
firmware talks over the native port only), so the tray tells the user to move the cable after flashing.

Fallback: if the DTR/RTS reset doesn't reach the ROM loader on the native port (seen with some USB hubs /
drivers), we ask the running firmware to reboot itself into download mode with {"t":"cmd","dl":1} and connect
with --before no-reset.

esptool versions. Developed and verified with esptool 5.6.0 (pip, Python 3.13). esptool 5 renamed the CLI
(write-flash, --flash-mode, default-reset/hard-reset; the 4.x underscore spellings are deprecated aliases) and
replaced its print-based progress with a logger object (`esptool.logger.log.set_logger`), which is what we hook
for progress. esptool 4.8+ is still handled: underscore spellings, and progress parsed from the
"Writing at 0x... (NN %)" lines it prints to stdout.

Exact arguments verified on the real 4B (2026-10-27, esptool 5.6.0), same as PlatformIO's `-t upload`:
  native /dev/ttyACM0 (303a:1001):
    --chip esp32s3 --port /dev/ttyACM0 --baud 921600 --before default-reset --after hard-reset
    write-flash -z --flash-mode dio --flash-freq 80m --flash-size 16MB
    0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
  "dl" fallback: same with --before no-reset --after watchdog-reset after sending {"t":"cmd","dl":1} (the
  ROM's port came back as the same /dev/ttyACM0). --after hard-reset does NOT work there: DTR is still
  asserted from pyserial's open, the USB-Serial-JTAG ignores the RTS pulse and the stub keeps running.
  (flash_mode dio is what PlatformIO passes for board_build.flash_mode = qio: the image header says dio and
  the 2nd-stage bootloader, built for QIO, switches the flash to QIO itself.)
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from serial.tools import list_ports

# --- boards ------------------------------------------------------------------------------------------------

DEFAULT_BOARD = "4b"
BOARDS: dict[str, str] = {                    # board id -> BOARD_NAME reported in the device hello (board.h)
    "4b": "ESP32-S3-Touch-LCD-4B",
}
BOARD_IDS: dict[str, str] = {name: bid for bid, name in BOARDS.items()}   # BOARD_NAME -> board id

NATIVE_VID, NATIVE_PID = 0x303A, 0x1001       # ESP32-S3 USB-Serial-JTAG (link port; ROM loader too)
UART_VID, UART_PID = 0x1A86, 0x55D3           # WCH CH343 on the 4B's "USB TO UART" port

FLASH_BAUD = 921600                           # CH343 handles it (verified); ignored by USB-Serial-JTAG
REENUM_TIMEOUT_S = 15.0


def board_id(board: str | None) -> str | None:
    """Board id ("4b") from an id or a hello BOARD_NAME; None when unknown."""
    if not board:
        return None
    b = board.strip()
    if b.lower() in BOARDS:
        return b.lower()
    return BOARD_IDS.get(b)


# --- ports -------------------------------------------------------------------------------------------------

@dataclass
class Port:
    device: str            # "/dev/ttyACM0", "COM5", "/dev/cu.usbmodem..."
    kind: str              # "native" | "uart" | "other"
    vid: int
    pid: int
    serial: str | None
    description: str


def _kind(vid, pid) -> str:
    if vid == NATIVE_VID and pid == NATIVE_PID:
        return "native"
    if vid == UART_VID and pid == UART_PID:
        return "uart"
    return "other"


def detect() -> list[Port]:
    """USB serial ports, ours first (native, then uart, then the rest). Enumeration only, no device I/O, so it
    is safe to call every couple of seconds from the tray even while the daemon holds the port."""
    try:
        infos = list_ports.comports()
    except Exception:
        return []
    out = []
    for p in infos:
        vid, pid = getattr(p, "vid", None), getattr(p, "pid", None)
        if vid is None:          # legacy ttyS* and friends: not USB, never a board
            continue
        out.append(Port(device=p.device, kind=_kind(vid, pid), vid=vid, pid=pid or 0,
                        serial=getattr(p, "serial_number", None) or None,
                        description=getattr(p, "description", "") or ""))
    order = {"native": 0, "uart": 1, "other": 2}
    out.sort(key=lambda x: (order[x.kind], x.device))
    return out


def link_port(ports: list[Port] | None = None) -> Port | None:
    """The native USB-Serial-JTAG port (the one the daemon talks to), if any."""
    for p in detect() if ports is None else ports:
        if p.kind == "native":
            return p
    return None


# --- bundled firmware --------------------------------------------------------------------------------------

@dataclass
class Manifest:
    board: str
    board_name: str
    fw_version: str
    tree: str
    chip: str
    flash_size: str
    flash_mode: str
    flash_freq: str
    parts: list[tuple[int, str]]          # (offset, file name), as in manifest.json
    built: str
    dir: Path = field(default_factory=Path)

    @classmethod
    def load(cls, path: str | os.PathLike) -> "Manifest":
        """Read <dir>/manifest.json (path = the dir or the json file). Raises ValueError when malformed or a
        part file is missing, so a half-copied bundle is never offered."""
        p = Path(path)
        if p.is_dir():
            p = p / "manifest.json"
        try:
            d = json.loads(p.read_text())
            parts = [(int(str(x["offset"]), 0), str(x["file"])) for x in d["parts"]]
            m = cls(board=str(d["board"]), board_name=str(d.get("board_name") or BOARDS.get(d["board"], "")),
                    fw_version=str(d["fw_version"]), tree=str(d.get("tree", "")), chip=str(d.get("chip", "esp32s3")),
                    flash_size=str(d.get("flash_size", "16MB")), flash_mode=str(d.get("flash_mode", "dio")),
                    flash_freq=str(d.get("flash_freq", "80m")), parts=parts, built=str(d.get("built", "")),
                    dir=p.parent)
        except (OSError, ValueError, KeyError, TypeError) as e:
            raise ValueError(f"bad firmware manifest {p}: {e}") from e
        if not parts:
            raise ValueError(f"bad firmware manifest {p}: no parts")
        for _, f in parts:
            if not (m.dir / f).is_file():
                raise ValueError(f"firmware bundle {m.dir} is incomplete: {f} missing")
        return m

    @property
    def files(self) -> list[tuple[int, Path]]:
        return [(off, self.dir / f) for off, f in self.parts]

    @property
    def size(self) -> int:
        return sum(p.stat().st_size for _, p in self.files)


def firmware_dirs() -> list[Path]:
    """Where bundled firmware may live: $CLAUDIUS_FIRMWARE_DIR (dev override), the package's own
    companion/firmware (source tree, pip install, PyInstaller onedir: __file__ points inside the bundle), and
    <_MEIPASS>/companion/firmware for PyInstaller layouts where data files sit apart from the code."""
    dirs = []
    env = os.environ.get("CLAUDIUS_FIRMWARE_DIR")
    if env:
        dirs.append(Path(env))
    dirs.append(Path(__file__).resolve().parent / "firmware")
    mei = getattr(sys, "_MEIPASS", None)
    if mei:
        dirs.append(Path(mei) / "companion" / "firmware")
    seen, out = set(), []
    for d in dirs:
        if d not in seen:
            seen.add(d)
            out.append(d)
    return out


def bundled() -> dict[str, Manifest]:
    """board id -> manifest of the firmware shipped with this app (first directory that has it wins)."""
    out: dict[str, Manifest] = {}
    for base in firmware_dirs():
        try:
            subs = sorted(base.iterdir())
        except OSError:
            continue
        for sub in subs:
            if sub.name in out or not (sub / "manifest.json").is_file():
                continue
            try:
                m = Manifest.load(sub)
            except ValueError:
                continue
            out.setdefault(m.board, m)
    return out


def version_tuple(v: str | None) -> tuple[int, ...]:
    """"0.3.0" -> (0, 3, 0); "v1.2-dev" -> (1, 2); garbage -> (). Numeric compare, so 0.10 > 0.9."""
    if not v:
        return ()
    out = []
    for part in str(v).strip().lstrip("vV").split("."):
        m = re.match(r"\d+", part)
        if not m:
            break
        out.append(int(m.group()))
        if m.end() != len(part):
            break
    return tuple(out)


def needs_update(fw: str | None, board: str | None) -> tuple[bool, str]:
    """Should the app offer to flash? fw/board come from the device hello (fw None = no hello: nothing usable on
    the board, offer an install). board may be a hello BOARD_NAME or an id; None = the default board."""
    bid = board_id(board) if board else DEFAULT_BOARD
    if bid is None:
        return False, f"unknown board {board!r}"
    m = bundled().get(bid)
    if m is None:
        return False, f"no bundled firmware for {bid}"
    if not fw:
        return True, f"install {m.fw_version}"
    have, ship = version_tuple(fw), version_tuple(m.fw_version)
    if not have:
        return True, f"{fw} -> {m.fw_version}"
    if have < ship:
        return True, f"{fw} -> {m.fw_version}"
    return False, "up to date"


# --- flashing ----------------------------------------------------------------------------------------------

class FlashError(Exception):
    """Flashing failed; str(e) is a sentence for the user."""


@dataclass
class FlashResult:
    board: str
    fw_version: str
    port: str                  # the port that was flashed through
    link_port: str | None      # the native link port after the final reset (None: did not come back in time)
    method: str                # "reset" (DTR/RTS) | "dl" (firmware rebooted itself into download mode)
    seconds: float


Progress = Callable[[int, str], None]
Log = Callable[[str], None]


def esptool_version() -> tuple[int, ...]:
    import esptool
    return version_tuple(getattr(esptool, "__version__", "0"))


def esptool_args(m: Manifest, port: str, before: str = "default-reset", erase: bool = False,
                 major: int | None = None, after: str = "hard-reset") -> list[str]:
    """argv for esptool.main(), spelled for the installed esptool major version."""
    if major is None:
        major = (esptool_version() or (5,))[0]
    legacy = {"default-reset": "default_reset", "no-reset": "no_reset", "hard-reset": "hard_reset",
              "watchdog-reset": "watchdog_reset",
              "write-flash": "write_flash", "--flash-mode": "--flash_mode", "--flash-freq": "--flash_freq",
              "--flash-size": "--flash_size"} if major < 5 else {}   # 4.x spelled --erase-all with a dash too
    s = lambda x: legacy.get(x, x)  # noqa: E731
    args = ["--chip", m.chip, "--port", port, "--baud", str(FLASH_BAUD),
            "--before", s(before), "--after", s(after),
            s("write-flash"), "-z", s("--flash-mode"), m.flash_mode, s("--flash-freq"), m.flash_freq,
            s("--flash-size"), m.flash_size]
    if erase:
        args.append(s("--erase-all"))
    for off, path in m.files:
        args += [hex(off), str(path)]
    return args


_MARKUP = re.compile(r"\[/?[a-z0-9 _#]+\]")
_PCT4 = re.compile(r"Writing at (0x[0-9a-f]+).*?\((\d+) ?%\)")


class _Progress:
    """Turns esptool's per-image progress into one 0-100 % over all images (weighted by size)."""

    def __init__(self, m: Manifest, progress: Progress | None):
        self.cb = progress
        sizes = [p.stat().st_size for _, p in m.files]
        self.sizes, self.total = sizes, max(1, sum(sizes))
        self.idx, self.last_frac, self.last_pct = 0, 0.0, -1
        self.started = False     # esptool reported writing: it reached the loader, the reset was not the issue

    def image(self, frac: float, msg: str):
        self.started = True
        if frac + 1e-9 < self.last_frac and self.idx < len(self.sizes) - 1:   # a new image's bar started
            self.idx += 1
        self.last_frac = frac
        done = sum(self.sizes[:self.idx]) + frac * self.sizes[min(self.idx, len(self.sizes) - 1)]
        pct = min(99, int(100 * done / self.total))   # 100 only once the reset is done
        # never backwards: an image whose bar we never saw (or a re-started bar on the last image) would
        # otherwise make the tray's percentage jump down
        pct = max(pct, self.last_pct, 0)
        if pct != self.last_pct:
            self.last_pct = pct      # also without a callback: flash() reads it to tell "never connected"
            if self.cb:
                self.cb(pct, msg)
        if frac >= 1.0 and self.idx < len(self.sizes) - 1:
            self.idx += 1
            self.last_frac = 0.0


def _run_esptool(args: list[str], prog: _Progress, log: Log | None) -> None:
    """esptool.main(args) in-process with output routed to log() and progress to prog."""
    import esptool

    def emit(text: str):
        text = _MARKUP.sub("", text).strip()
        if text and log:
            for line in text.splitlines():
                if line.strip():
                    log(line.rstrip())

    if (esptool_version() or (5,))[0] >= 5:
        from esptool.logger import TemplateLogger, log as eslog
        from esp_pylib.logger import EspLog

        class Capture(TemplateLogger):
            def print(self, *a, **k):
                emit(k.get("sep", " ").join(str(x) for x in a))

            def note(self, message):
                emit(f"Note: {message}")

            def warning(self, message):
                emit(f"Warning: {message}")

            def error(self, message):
                emit(f"Error: {message}")

            def stage(self, finish=False):
                pass

            def progress_bar(self, cur_iter, total_iters, prefix="", suffix="", bar_length=30):
                if total_iters and "Writing" in prefix:
                    prog.image(cur_iter / total_iters, (prefix + suffix).strip())

            def set_verbosity(self, verbosity):
                pass

        old = EspLog.instance
        eslog.set_logger(Capture())
        try:
            esptool.main(args)
        finally:
            EspLog.instance = old
    else:   # esptool 4.x prints; progress lines end with \r
        class Tap(io.TextIOBase):
            def __init__(self):
                self.buf = ""

            def write(self, s):
                self.buf += s
                parts = re.split(r"[\r\n]", self.buf)
                self.buf = parts.pop()
                for line in parts:
                    mt = _PCT4.search(line)
                    if mt:
                        prog.image(int(mt.group(2)) / 100, line.strip())
                    else:
                        emit(line)
                return len(s)

        tap = Tap()
        with contextlib.redirect_stdout(tap):
            esptool.main(args)
        emit(tap.buf)


def _humanize(e: BaseException, port: str, kind: str) -> str:
    s = str(e)
    low = s.lower()
    if "permission denied" in low or "access is denied" in low:
        hint = " On Linux add yourself to the 'dialout' group (or install the udev rule) and log in again." \
            if sys.platform.startswith("linux") else ""
        return f"No permission to open {port}.{hint}"
    if "busy" in low or "resource temporarily unavailable" in low or "could not exclusively lock" in low:
        return f"{port} is in use by another program (serial monitor, Arduino IDE...). Close it and retry."
    if "failed to connect" in low or "no serial data received" in low or "wrong boot mode" in low:
        tip = ("hold the BOOT button, press and release RESET, release BOOT, then try again"
               if kind != "other" else "check the cable and the board")
        return f"The board on {port} did not enter the bootloader: {tip}."
    if "could not open port" in low or "no such file" in low or "filenotfound" in low:
        return f"{port} disappeared: check the USB cable and try again."
    return f"Flashing failed on {port}: {s or type(e).__name__}"


def _wait_native(timeout: float, serial_no: str | None, gone_first: str | None = None) -> Port | None:
    """Wait for the native port to (re)appear. With gone_first, first give that device a moment to vanish so
    we don't return the stale node of the board that is still resetting."""
    t0 = time.time()
    if gone_first:
        while time.time() - t0 < 2.0:
            if not any(p.device == gone_first for p in detect()):
                break
            time.sleep(0.1)
    while time.time() - t0 < timeout:
        ports = [p for p in detect() if p.kind == "native"]
        if serial_no and any(p.serial for p in ports):
            # by USB serial (the chip MAC), not by name: the node may come back renamed (COM6 -> COM7, a new
            # cu.usbmodem*), and another ESP32-S3 on the bus must not be taken for ours while it is still resetting.
            # Only an enumeration without serial numbers at all (some drivers) falls back to "any native port".
            ports = [p for p in ports if p.serial == serial_no]
        if ports:
            return ports[0]
        time.sleep(0.2)
    return None


def request_download_mode(port: str) -> None:
    """Ask the running firmware ({"t":"cmd","dl":1}, fw >= 0.3.0) to reboot into the ROM download mode.
    Opens the port the way link.py does (DTR/RTS left as the OS sets them) so the open itself doesn't reset."""
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout, s.write_timeout = port, 115200, 0, 2
    s.open()
    try:
        s.write(b'{"t":"cmd","dl":1}\n')
        s.flush()
        time.sleep(0.2)
    finally:
        try:
            s.close()
        except Exception:
            pass


@contextlib.contextmanager
def device_lock(timeout: float = 30.0):
    """The shared device lock for a process that is not the daemon (tools/dev.py protocol: <root>/.devwant.<pid>
    marker so a running daemon closes the port within ~1 s, then flock <root>/.devlock). Delegates to
    daemon.claim_device, which `claudius-maximus flash` uses too; flash(lock=True) takes it itself."""
    from .daemon import claim_device
    try:
        with claim_device("flasher", timeout=timeout):
            time.sleep(0.3)   # let the daemon's close finish before esptool opens the port
            yield
    except TimeoutError as e:
        raise FlashError(f"The device is busy ({e}); try again in a minute.") from e


def pick_port(port: str | None) -> Port:
    ports = detect()
    if port:
        for p in ports:
            if p.device == port:
                return p
        return Port(device=port, kind="other", vid=0, pid=0, serial=None, description="")
    for kind in ("native", "uart"):
        for p in ports:
            if p.kind == kind:
                return p
    raise FlashError("No board found: plug it in with a USB data cable (on the 4B use the USB-OTG port).")


def flash(board: str, port: str | None = None, progress: Progress | None = None, log: Log | None = None,
          erase: bool = False, *, lock: bool = False) -> FlashResult:
    """Write the bundled firmware for `board` ("4b"). port None = auto (native first, else the CH343).
    progress(pct, msg) is called with 0..100; log(line) gets esptool's output. The caller makes the port free
    (App: daemon.pause_device(); CLI: daemon.claim_device()); lock=True takes the shared device lock here
    instead (scripts). Raises FlashError with a sentence for the user."""
    bid = board_id(board) or ""
    if bid not in BOARDS:
        raise FlashError(f"Unknown board {board!r} (expected one of: {', '.join(BOARDS)}).")
    m = bundled().get(bid)
    if m is None:
        raise FlashError(f"This app has no firmware for the {BOARDS[bid]} bundled.")
    try:
        import esptool  # noqa: F401
    except ImportError as e:
        raise FlashError("esptool is not installed (pip install esptool).") from e

    say = log or (lambda _s: None)
    t0 = time.time()
    ctx = device_lock() if lock else contextlib.nullcontext()
    with ctx:
        p = pick_port(port)
        if progress:
            progress(0, f"Connecting to {p.device}")
        say(f"flashing {m.board_name} fw {m.fw_version} ({m.tree}) through {p.device} ({p.kind})")
        prog = _Progress(m, progress)
        method, used = "reset", p.device
        try:
            _run_esptool(esptool_args(m, p.device, "default-reset", erase), prog, say)
        except BaseException as e:   # esptool raises FatalError/SerialException, SystemExit on CLI errors
            if isinstance(e, KeyboardInterrupt):
                raise
            say(f"esptool: {e}")
            if p.kind != "native" or prog.started:
                raise FlashError(_humanize(e, p.device, p.kind)) from e
            # DTR/RTS reset didn't reach the ROM loader: ask the firmware to reboot into it
            say("reset into the bootloader failed; asking the firmware to reboot into download mode")
            try:
                request_download_mode(p.device)
            except Exception as e2:
                raise FlashError(_humanize(e, p.device, p.kind)) from e2
            np = _wait_native(8.0, p.serial, gone_first=p.device)
            if np is None:
                raise FlashError(_humanize(e, p.device, p.kind)) from e
            method, used = "dl", np.device
            prog = _Progress(m, progress)
            try:
                # watchdog-reset, not hard-reset: with --before no-reset DTR stays asserted (pyserial's open
                # default) and the USB-Serial-JTAG ignores the RTS pulse, leaving the stub running (seen on the 4B)
                _run_esptool(esptool_args(m, np.device, "no-reset", erase, after="watchdog-reset"), prog, say)
            except BaseException as e3:
                if isinstance(e3, KeyboardInterrupt):
                    raise
                raise FlashError(_humanize(e3, np.device, "native")) from e3
        if progress:
            progress(99, "Restarting the board")
        # after hard-reset the native port re-enumerates (also when we flashed via the CH343: the chip resets)
        back = _wait_native(REENUM_TIMEOUT_S, p.serial if p.kind == "native" else None,
                            gone_first=used if p.kind == "native" else None)
    if progress:
        progress(100, "Done" if back else "Done (board not seen on USB-OTG yet)")
    return FlashResult(board=bid, fw_version=m.fw_version, port=used, link_port=back.device if back else None,
                       method=method, seconds=round(time.time() - t0, 1))
