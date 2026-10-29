"""Tests for companion.flasher (no hardware: pyserial enumeration and esptool are mocked).

Run: host/.venv/bin/python -m unittest discover -s host/tests -v
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from companion import flasher  # noqa: E402


def info(device, vid, pid, serial=None, desc=""):
    return SimpleNamespace(device=device, vid=vid, pid=pid, serial_number=serial, description=desc)


PORTS = [
    info("/dev/ttyUSB0", 0x10C4, 0xEA60, "x", "CP2102"),
    info("/dev/ttyACM1", 0x1A86, 0x55D3, "5A1", "USB Single Serial"),
    info("/dev/ttyACM0", 0x303A, 0x1001, "AA:BB", "USB JTAG/serial debug unit"),
    info("/dev/ttyS0", None, None),
]


def write_bundle(root: Path, board="4b", version="0.3.0", parts=None, skip_file=None) -> Path:
    d = root / board
    d.mkdir(parents=True)
    parts = parts or [("bootloader.bin", "0x0"), ("partitions.bin", "0x8000"), ("boot_app0.bin", "0xe000"),
                      ("firmware.bin", "0x10000")]
    for i, (f, _) in enumerate(parts):
        if f != skip_file:
            (d / f).write_bytes(b"\xe9" * (100 * (i + 1)))
    (d / "manifest.json").write_text(json.dumps({
        "board": board, "board_name": flasher.BOARDS[board], "fw_version": version, "tree": "abc",
        "chip": "esp32s3", "flash_size": "16MB", "flash_mode": "dio", "flash_freq": "80m",
        "parts": [{"file": f, "offset": o} for f, o in parts], "built": "2026-10-27T23:00:00Z"}))
    return d


class DetectTest(unittest.TestCase):
    def test_classification_and_order(self):
        with mock.patch.object(flasher.list_ports, "comports", return_value=PORTS):
            ports = flasher.detect()
            lp = flasher.link_port()
        self.assertEqual([(p.device, p.kind) for p in ports],
                         [("/dev/ttyACM0", "native"), ("/dev/ttyACM1", "uart"), ("/dev/ttyUSB0", "other")])
        self.assertEqual(ports[0].serial, "AA:BB")
        self.assertEqual(lp.device, "/dev/ttyACM0")

    def test_link_port_none_and_given_list(self):
        uart = flasher.Port("/dev/ttyACM1", "uart", 0x1A86, 0x55D3, None, "")
        self.assertIsNone(flasher.link_port([uart]))
        with mock.patch.object(flasher.list_ports, "comports", side_effect=OSError("boom")):
            self.assertEqual(flasher.detect(), [])

    def test_pick_port_prefers_native_then_uart(self):
        with mock.patch.object(flasher.list_ports, "comports", return_value=PORTS[:2]):
            self.assertEqual(flasher.pick_port(None).kind, "uart")
        with mock.patch.object(flasher.list_ports, "comports", return_value=PORTS):
            self.assertEqual(flasher.pick_port(None).kind, "native")
            self.assertEqual(flasher.pick_port("COM9").device, "COM9")
        with mock.patch.object(flasher.list_ports, "comports", return_value=PORTS[:1]):
            with self.assertRaises(flasher.FlashError):
                flasher.pick_port(None)

    def test_pick_port_several_boards_needs_a_choice(self):
        """Two ESP32-S3 boards: never guess which one to overwrite. The stored choice (companion/ports.py)
        resolves it, by USB serial first (the name may have changed)."""
        two = PORTS + [info("/dev/ttyACM2", 0x303A, 0x1001, "CC:DD", "USB JTAG/serial debug unit")]
        with mock.patch.object(flasher.list_ports, "comports", return_value=two), \
                mock.patch("companion.ports.selection", return_value=(None, None)):
            with self.assertRaises(flasher.FlashError) as cm:
                flasher.pick_port(None)
            self.assertIn("2 boards are connected", str(cm.exception))
            self.assertEqual(flasher.pick_port("/dev/ttyACM2").serial, "CC:DD")   # explicit (the daemon's port)
        with mock.patch.object(flasher.list_ports, "comports", return_value=two), \
                mock.patch("companion.ports.selection", return_value=("/dev/ttyACM9", "CC:DD")):
            self.assertEqual(flasher.pick_port(None).device, "/dev/ttyACM2")      # serial wins over the old name
        with mock.patch.object(flasher.list_ports, "comports", return_value=two), \
                mock.patch("companion.ports.selection", return_value=("/dev/ttyACM9", None)):
            with self.assertRaises(flasher.FlashError) as cm:
                flasher.pick_port(None)
            self.assertIn("not connected", str(cm.exception))


class BoardsAndVersionsTest(unittest.TestCase):
    def test_board_mapping(self):
        self.assertEqual(flasher.BOARD_IDS["ESP32-S3-Touch-LCD-4B"], "4b")
        self.assertEqual(flasher.board_id("4B"), "4b")
        self.assertIsNone(flasher.board_id("ESP32-S3-Touch-LCD-4 V4"))
        self.assertIsNone(flasher.board_id("something else"))

    def test_version_tuple(self):
        self.assertEqual(flasher.version_tuple("0.3.0"), (0, 3, 0))
        self.assertEqual(flasher.version_tuple("v1.2-dev"), (1, 2))
        self.assertGreater(flasher.version_tuple("0.10.0"), flasher.version_tuple("0.9.9"))
        self.assertEqual(flasher.version_tuple(None), ())

    def test_needs_update(self):
        with tempfile.TemporaryDirectory() as t:
            write_bundle(Path(t), "4b", "0.3.0")
            with mock.patch.dict(os.environ, {"CLAUDIUS_FIRMWARE_DIR": t}), \
                    mock.patch.object(flasher, "firmware_dirs", return_value=[Path(t)]):
                self.assertEqual(flasher.needs_update("0.2.0", "ESP32-S3-Touch-LCD-4B"), (True, "0.2.0 -> 0.3.0"))
                self.assertEqual(flasher.needs_update("0.3.0", "4b"), (False, "up to date"))
                self.assertEqual(flasher.needs_update("0.10.0", "4b"), (False, "up to date"))
                self.assertEqual(flasher.needs_update(None, None), (True, "install 0.3.0"))
                self.assertEqual(flasher.needs_update("0.2.9", None)[0], True)
                self.assertFalse(flasher.needs_update("0.1.0", "Mystery board")[0])


class ManifestTest(unittest.TestCase):
    def test_load_and_bundled(self):
        with tempfile.TemporaryDirectory() as t:
            write_bundle(Path(t), "4b", "0.3.1")
            (Path(t) / "junk").mkdir()
            with mock.patch.object(flasher, "firmware_dirs", return_value=[Path(t)]):
                b = flasher.bundled()
            self.assertEqual(sorted(b), ["4b"])
            m = b["4b"]
            self.assertEqual(m.parts[0], (0, "bootloader.bin"))
            self.assertEqual(m.files[-1], (0x10000, Path(t) / "4b" / "firmware.bin"))
            self.assertEqual(m.size, 100 + 200 + 300 + 400)
            self.assertEqual(m.fw_version, "0.3.1")

    def test_incomplete_bundle_ignored(self):
        with tempfile.TemporaryDirectory() as t:
            write_bundle(Path(t), "4b", skip_file="firmware.bin")
            with self.assertRaises(ValueError):
                flasher.Manifest.load(Path(t) / "4b")
            with mock.patch.object(flasher, "firmware_dirs", return_value=[Path(t)]):
                self.assertEqual(flasher.bundled(), {})

    def test_firmware_dirs_meipass(self):
        with mock.patch.object(sys, "_MEIPASS", "/frozen", create=True):
            dirs = flasher.firmware_dirs()
        self.assertIn(Path(flasher.__file__).resolve().parent / "firmware", dirs)
        self.assertIn(Path("/frozen") / "companion" / "firmware", dirs)


class FlashTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        write_bundle(self.root, "4b")
        p = mock.patch.object(flasher, "firmware_dirs", return_value=[self.root])
        p.start()
        self.addCleanup(p.stop)
        self.addCleanup(self.tmp.cleanup)
        p2 = mock.patch.object(flasher.time, "sleep")
        p2.start()
        self.addCleanup(p2.stop)

    def test_args_v5_and_v4(self):
        m = flasher.bundled()["4b"]
        a = flasher.esptool_args(m, "/dev/ttyACM0", erase=True, major=5)
        self.assertEqual(a[:12], ["--chip", "esp32s3", "--port", "/dev/ttyACM0", "--baud", "921600",
                                  "--before", "default-reset", "--after", "hard-reset", "write-flash", "-z"])
        self.assertIn("--erase-all", a)
        i = a.index("--flash-mode")
        self.assertEqual(a[i:i + 6], ["--flash-mode", "dio", "--flash-freq", "80m", "--flash-size", "16MB"])
        self.assertEqual(a[-8:], ["0x0", str(m.dir / "bootloader.bin"), "0x8000", str(m.dir / "partitions.bin"),
                                  "0xe000", str(m.dir / "boot_app0.bin"), "0x10000", str(m.dir / "firmware.bin")])
        legacy = flasher.esptool_args(m, "COM5", before="no-reset", major=4)
        for x in ("write_flash", "--flash_mode", "no_reset", "hard_reset"):
            self.assertIn(x, legacy)

    def _fake_esptool(self, calls, fail_first=False):
        def run(args, prog, log):
            calls.append(args)
            if fail_first and len(calls) == 1:
                raise RuntimeError("Failed to connect to ESP32-S3: No serial data received.")
            for i in range(4):          # four images, each 0 -> 100 %
                prog.image(0.0, "Writing at 0x0")
                prog.image(0.5, "Writing at 0x1")
                prog.image(1.0, "Writing at 0x2")
            log("Hard resetting via RTS pin...")
        return run

    def test_flash_native(self):
        calls, pcts, lines = [], [], []
        with mock.patch.object(flasher.list_ports, "comports", return_value=PORTS), \
                mock.patch.object(flasher, "_run_esptool", side_effect=self._fake_esptool(calls)), \
                mock.patch.object(flasher, "_wait_native",
                                  return_value=flasher.Port("/dev/ttyACM2", "native", 0x303A, 0x1001, "AA:BB", "")):
            r = flasher.flash("4b", progress=lambda p, m: pcts.append(p), log=lines.append, lock=False)
        self.assertEqual(len(calls), 1)
        self.assertIn("/dev/ttyACM0", calls[0])
        self.assertIn("default-reset", calls[0])
        self.assertEqual((r.board, r.port, r.link_port, r.method, r.fw_version),
                         ("4b", "/dev/ttyACM0", "/dev/ttyACM2", "reset", "0.3.0"))
        self.assertEqual(pcts[0], 0)
        self.assertEqual(pcts[-1], 100)
        self.assertEqual(pcts, sorted(pcts))
        self.assertIn("Hard resetting via RTS pin...", lines)

    def test_flash_uart_port_given(self):
        calls = []
        with mock.patch.object(flasher.list_ports, "comports", return_value=PORTS), \
                mock.patch.object(flasher, "_run_esptool", side_effect=self._fake_esptool(calls)), \
                mock.patch.object(flasher, "_wait_native", return_value=None):
            r = flasher.flash("4b", port="/dev/ttyACM1", lock=False)
        self.assertIn("/dev/ttyACM1", calls[0])
        self.assertEqual((r.port, r.link_port), ("/dev/ttyACM1", None))

    def test_flash_dl_fallback(self):
        calls = []
        new = flasher.Port("/dev/ttyACM3", "native", 0x303A, 0x1001, "AA:BB", "")
        with mock.patch.object(flasher.list_ports, "comports", return_value=PORTS), \
                mock.patch.object(flasher, "_run_esptool", side_effect=self._fake_esptool(calls, fail_first=True)), \
                mock.patch.object(flasher, "request_download_mode") as dl, \
                mock.patch.object(flasher, "_wait_native", return_value=new):
            r = flasher.flash("4b", lock=False)
        dl.assert_called_once_with("/dev/ttyACM0")
        self.assertEqual(len(calls), 2)
        self.assertIn("no-reset", calls[1])
        self.assertIn("watchdog-reset", calls[1])
        self.assertIn("/dev/ttyACM3", calls[1])
        self.assertEqual(r.method, "dl")

    def test_flash_errors(self):
        with self.assertRaises(flasher.FlashError):
            flasher.flash("v4", lock=False)              # the old board is not supported any more
        with self.assertRaises(flasher.FlashError):
            flasher.flash("nope", lock=False)
        with mock.patch.object(flasher.list_ports, "comports", return_value=PORTS), \
                mock.patch.object(flasher, "_run_esptool",
                                  side_effect=OSError("[Errno 13] Permission denied: '/dev/ttyACM1'")):
            with self.assertRaises(flasher.FlashError) as cm:
                flasher.flash("4b", port="/dev/ttyACM1", lock=False)
        self.assertIn("permission", str(cm.exception).lower())

    def test_run_esptool_progress_v5(self):
        """The esptool 5 logger hook: progress_bar calls become overall percentages; print goes to log."""
        try:
            import esptool
            from esptool.logger import log as eslog
        except ImportError:
            self.skipTest("esptool not installed")
        if flasher.esptool_version()[0] < 5:
            self.skipTest("esptool < 5")
        m = flasher.bundled()["4b"]
        pcts, lines = [], []
        prog = flasher._Progress(m, lambda p, _m: pcts.append(p))

        def fake_main(args):
            eslog.print("Connecting...")
            for _ in m.files:
                for i in range(1, 5):
                    eslog.progress_bar(i, 4, prefix="Writing at 0x00010000 ", suffix=" 1/4")
        before = type(__import__("esp_pylib.logger", fromlist=["EspLog"]).EspLog.instance)
        with mock.patch.object(esptool, "main", side_effect=fake_main):
            flasher._run_esptool(["x"], prog, lines.append)
        self.assertIn("Connecting...", lines)
        self.assertEqual(pcts, sorted(pcts))
        self.assertEqual(pcts[-1], 99)
        after = type(__import__("esp_pylib.logger", fromlist=["EspLog"]).EspLog.instance)
        self.assertIs(before, after)      # logger restored


class ReviewTest(unittest.TestCase):
    """Integration review: logger restore on failure, progress never backwards, fallback gating, renamed port."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        write_bundle(self.root, "4b")
        p = mock.patch.object(flasher, "firmware_dirs", return_value=[self.root])
        p.start()
        self.addCleanup(p.stop)
        self.addCleanup(self.tmp.cleanup)
        p2 = mock.patch.object(flasher.time, "sleep")
        p2.start()
        self.addCleanup(p2.stop)

    def test_ports_esptool_left_open_are_closed(self):
        """Issue #20: esptool re-raises a failed connect on a named port without closing it; in-process the
        exclusive lock would then block every later flash. _run_esptool closes what esptool left open."""
        try:
            import esptool
            import serial
        except ImportError:
            self.skipTest("esptool not installed")
        left = []
        orig = serial.serial_for_url

        def fake_main(args):
            self.assertIsNot(serial.serial_for_url, orig)                   # the hook is in place during the run
            s = serial.serial_for_url("loop://", do_not_open=True)   # what ESPLoader.__init__ does
            s.open()
            left.append(s)
            raise esptool.FatalError("device reports readiness to read but returned no data")

        prog = flasher._Progress(flasher.bundled()["4b"], None)
        with mock.patch.object(esptool, "main", side_effect=fake_main):
            with self.assertRaises(esptool.FatalError):
                flasher._run_esptool(["x"], prog, None)
        self.assertEqual(len(left), 1)
        self.assertFalse(left[0].is_open)
        self.assertIs(serial.serial_for_url, orig)                          # the hook is gone afterwards
        self.assertFalse(flasher._esptool_lock.locked())
        # and a successful run (esptool closes its own port) is left alone
        def ok_main(args):
            s = serial.serial_for_url("loop://")
            left.append(s)
            s.close()
        with mock.patch.object(esptool, "main", side_effect=ok_main):
            flasher._run_esptool(["x"], prog, None)
        self.assertFalse(left[1].is_open)

    def test_logger_restored_after_esptool_error(self):
        try:
            import esptool
            from esp_pylib.logger import EspLog
        except ImportError:
            self.skipTest("esptool not installed")
        if flasher.esptool_version()[0] < 5:
            self.skipTest("esptool < 5")
        before = EspLog.instance
        prog = flasher._Progress(flasher.bundled()["4b"], None)
        with mock.patch.object(esptool, "main", side_effect=esptool.FatalError("Failed to connect")):
            with self.assertRaises(esptool.FatalError):
                flasher._run_esptool(["x"], prog, None)
        self.assertIs(EspLog.instance, before)

    def test_progress_never_goes_backwards(self):
        pcts = []
        prog = flasher._Progress(flasher.bundled()["4b"], lambda p, _m: pcts.append(p))
        for f in (0.2, 1.0):           # bootloader
            prog.image(f, "Writing")
        for f in (0.1, 0.9):           # firmware (partitions + boot_app0 had no bar at all)
            prog.image(f, "Writing")
        for f in (0.5, 0.2, 0.8):      # a bar that restarts on the last image
            prog.image(f, "Writing")
        self.assertEqual(pcts, sorted(pcts))
        self.assertLessEqual(pcts[-1], 99)

    def test_no_dl_fallback_once_writing_started_even_without_progress(self):
        def run(args, prog, log):
            prog.image(0.0, "Writing at 0x0")   # 0 %: but the loader answered
            raise RuntimeError("serial exception: device reports readiness to read but returned no data")
        with mock.patch.object(flasher.list_ports, "comports", return_value=PORTS), \
                mock.patch.object(flasher, "_run_esptool", side_effect=run), \
                mock.patch.object(flasher, "request_download_mode") as dl:
            with self.assertRaises(flasher.FlashError):
                flasher.flash("4b", lock=False)
        dl.assert_not_called()

    def test_needs_update_dev_versions(self):
        self.assertEqual(flasher.needs_update("0.3.0-dev", "4b"), (False, "up to date"))
        self.assertEqual(flasher.needs_update("0.2.9-dev", "ESP32-S3-Touch-LCD-4B"), (True, "0.2.9-dev -> 0.3.0"))
        self.assertTrue(flasher.needs_update("dev", "4b")[0])
        self.assertEqual(flasher.needs_update(None, None), (True, "install 0.3.0"))

    def test_wait_native_follows_serial_across_rename(self):
        """Windows COM6 -> COM7 (or a new /dev/cu.usbmodem name): the board is found by its USB serial number,
        not by name, even with another ESP32-S3 plugged in."""
        seq = [
            [info("COM6", 0x303A, 0x1001, "AA:BB")],                        # still there (resetting)
            [],                                                             # gone
            [info("COM3", 0x303A, 0x1001, "CC:DD")],                        # other board only
            [info("COM3", 0x303A, 0x1001, "CC:DD"), info("COM7", 0x303A, 0x1001, "AA:BB")],
        ]
        it = iter(seq)
        last = [seq[-1]]

        def comports():
            try:
                last[0] = next(it)
            except StopIteration:
                pass
            return last[0]
        with mock.patch.object(flasher.list_ports, "comports", side_effect=comports):
            p = flasher._wait_native(5.0, "AA:BB", gone_first="COM6")
        self.assertEqual(p.device, "COM7")


if __name__ == "__main__":
    unittest.main()
