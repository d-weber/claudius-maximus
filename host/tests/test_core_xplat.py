"""Tests for the cross-platform core: locking, hostos (settings store, self_command), hooks IPC (unix + the
Windows UDP path, exercised for real on loopback here), hooksetup / cm_settings command forms, autostart entries,
Daemon.public_status / pause_device / resume_device / thread use, and the CLI fast paths.

Run:  host/.venv/bin/python -m unittest host/tests/test_core_xplat.py -v
No hardware, no network (loopback UDP only), never touches the real ~/.claude or ~/.claudius-maximus.
"""
from __future__ import annotations

import io
import json
import os
import plistlib
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest
from unittest import mock

HOST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, HOST)

from companion import autostart, hookclient, hooksetup, hostos, locking  # noqa: E402
from companion import daemon as daemon_mod  # noqa: E402
from companion.link import DeviceLink, read_lock_holder  # noqa: E402
from companion.providers.hooks import HooksProvider  # noqa: E402

POSIX = os.name == "posix"


class TempHome(unittest.TestCase):
    """HOME, CLAUDIUS_HOME, XDG_*, CLAUDE_CONFIG_DIR, LOCALAPPDATA all inside a temp dir (short path: AF_UNIX
    socket paths are limited to ~100 bytes)."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="cmx")
        self.home = os.path.join(self.tmp, "h")
        self.state = os.path.join(self.home, ".claudius-maximus")
        os.makedirs(self.state)
        env = {"HOME": self.home, "USERPROFILE": self.home, "CLAUDIUS_HOME": self.state,
               "XDG_CONFIG_HOME": os.path.join(self.home, ".config"),
               "XDG_STATE_HOME": os.path.join(self.home, ".local", "state"),
               "CLAUDE_CONFIG_DIR": os.path.join(self.home, ".claude"),
               "LOCALAPPDATA": os.path.join(self.home, "AppData", "Local")}
        self._env = mock.patch.dict(os.environ, env)
        self._env.start()
        os.environ.pop("CLAUDIUS_HOOKS_TRANSPORT", None)
        os.environ.pop("APPIMAGE", None)

    def tearDown(self):
        self._env.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)


# ---------------------------------------------------------------------------------------------
class LockingTest(TempHome):
    def test_exclusive_and_holder(self):
        p = os.path.join(self.tmp, ".devlock")
        a, b = locking.open_lock_file(p), locking.open_lock_file(p)
        try:
            self.assertTrue(locking.try_lock(a))
            self.assertFalse(locking.try_lock(b))
            locking.write_holder(a, "default flash pid=42\n")
            self.assertEqual(read_lock_holder(p), "default flash pid=42")
            locking.unlock(a)
            self.assertTrue(locking.try_lock(b))
        finally:
            os.close(a)
            os.close(b)

    def test_devicelink_lock(self):
        p = os.path.join(self.tmp, ".devlock")
        link = DeviceLink("/dev/null-not-a-port", lock_path=p)
        self.assertTrue(link._acquire_lock())
        self.assertEqual(link.lock_holder(), f"daemon pid={os.getpid()}")
        other = DeviceLink("/dev/x", lock_path=p)
        self.assertFalse(other._acquire_lock())
        self.assertIn("lock", other.last_error)
        link._release_lock()
        self.assertTrue(other._acquire_lock())
        other._release_lock()

    def test_old_holder_format_still_readable(self):
        p = os.path.join(self.tmp, ".devlock")
        with open(p, "w") as f:
            f.write("daemon pid=7\n")   # written by an older daemon (no leading space)
        self.assertEqual(read_lock_holder(p), "daemon pid=7")

    def test_instance_lock(self):
        fd = daemon_mod.acquire_instance_lock()
        self.assertIsNotNone(fd)
        try:
            self.assertIsNone(daemon_mod.acquire_instance_lock())
            self.assertEqual(daemon_mod.instance_lock_holder(), os.getpid())
            d = daemon_mod.Daemon.__new__(daemon_mod.Daemon)   # run() must refuse before building anything
            d.dry_run, d.error = False, ""
            self.assertEqual(daemon_mod.run(daemon=d), 1)
            self.assertIn("already running", d.error)
        finally:
            daemon_mod.release_instance_lock(fd)
        fd = daemon_mod.acquire_instance_lock()
        self.assertIsNotNone(fd)
        daemon_mod.release_instance_lock(fd)

    def test_claim_device(self):
        lock = os.path.join(self.tmp, ".devlock")
        with mock.patch.object(daemon_mod, "DEVLOCK", daemon_mod.Path(lock)):
            with daemon_mod.claim_device("flash", timeout=1):
                self.assertTrue(os.path.exists(os.path.join(self.tmp, f".devwant.{os.getpid()}")))
                self.assertTrue(daemon_mod.dev_wants_device(time.time()))
                self.assertEqual(read_lock_holder(lock), f"flash pid={os.getpid()}")
                fd = locking.open_lock_file(lock)
                self.assertFalse(locking.try_lock(fd))
                os.close(fd)
            self.assertFalse(os.path.exists(os.path.join(self.tmp, f".devwant.{os.getpid()}")))
            fd = locking.open_lock_file(lock)
            self.assertTrue(locking.try_lock(fd))
            try:
                with self.assertRaises(TimeoutError):
                    with daemon_mod.claim_device("flash", timeout=0.3):
                        pass
            finally:
                locking.unlock(fd)
                os.close(fd)

    def test_claim_device_creates_missing_dir(self):
        """#11: in a frozen app the lock lives in the state dir, which only the daemon creates; `flash` before
        the first daemon start must make it rather than crash with FileNotFoundError."""
        d = os.path.join(self.tmp, "fresh", ".claudius-maximus")
        lock = os.path.join(d, ".devlock")
        with mock.patch.object(daemon_mod, "DEVLOCK", daemon_mod.Path(lock)):
            with daemon_mod.claim_device("flash", timeout=1):
                self.assertTrue(os.path.isdir(d))
                self.assertEqual(read_lock_holder(lock), f"flash pid={os.getpid()}")
            self.assertFalse(os.path.exists(os.path.join(d, f".devwant.{os.getpid()}")))
        if POSIX and os.geteuid() != 0:
            self.assertEqual(os.stat(d).st_mode & 0o777, 0o700)

    def test_claim_device_unwritable_dir_raises_oserror(self):
        """The lock file cannot be created: OSError, not TimeoutError (so `flash` can say what is wrong)."""
        blocker = os.path.join(self.tmp, "afile")
        with open(blocker, "w"):
            pass
        lock = os.path.join(blocker, "sub", ".devlock")   # a regular file in the path: makedirs/open both fail
        with mock.patch.object(daemon_mod, "DEVLOCK", daemon_mod.Path(lock)):
            with self.assertRaises(OSError) as cm:
                with daemon_mod.claim_device("flash", timeout=1):
                    pass
        self.assertNotIsInstance(cm.exception, TimeoutError)


# ---------------------------------------------------------------------------------------------
class HostosTest(TempHome):
    def test_settings_store(self):
        self.assertEqual(hostos.settings_path(), os.path.join(self.state, "settings.json"))
        self.assertEqual(hostos.settings_get("board", "4b"), "4b")
        hostos.settings_set("board", "v4")
        hostos.settings_set("auto_flash", True)
        self.assertEqual(hostos.settings_get("board"), "v4")
        self.assertIs(hostos.settings.get("auto_flash"), True)
        with open(hostos.settings_path()) as f:
            self.assertEqual(json.load(f), {"auto_flash": True, "board": "v4"})
        hostos.settings_set("board", None)
        self.assertIsNone(hostos.settings_get("board"))
        self.assertEqual([f for f in os.listdir(self.state) if f.endswith(".tmp")], [])
        with open(hostos.settings_path(), "w") as f:
            f.write("{broken")
        self.assertEqual(hostos.settings_get("x", 1), 1)   # corrupt file = defaults, no crash
        hostos.settings_set("x", 2)
        self.assertEqual(hostos.settings_get("x"), 2)

    def test_settings_threads(self):
        def w(i):
            for j in range(20):
                hostos.settings_set(f"k{i}", j)
        ts = [threading.Thread(target=w, args=(i,)) for i in range(4)]
        for t in ts:
            t.start()
        for t in ts:
            t.join()
        self.assertEqual({k: v for k, v in hostos.settings.all().items()}, {f"k{i}": 19 for i in range(4)})

    def test_paths_per_os(self):
        with mock.patch.object(hostos, "IS_WINDOWS", True), mock.patch.object(hostos, "IS_MAC", False):
            self.assertEqual(hostos.log_dir(), os.path.join(self.home, "AppData", "Local", "claudius-maximus", "logs"))
            self.assertIsNone(hostos.service_path())
            self.assertFalse(hostos.service_installed())
        with mock.patch.object(hostos, "IS_MAC", True):
            self.assertEqual(hostos.log_path(), os.path.join(self.home, "Library", "Logs", "claudius-maximus.log"))
        with mock.patch.object(hostos, "IS_MAC", False), mock.patch.object(hostos, "IS_WINDOWS", False):
            self.assertEqual(hostos.log_dir(), os.path.join(self.home, ".local", "state", "claudius-maximus"))
        self.assertEqual(hostos.claude_dir(), os.path.join(self.home, ".claude"))
        self.assertEqual(hostos.state_dir(), self.state)

    def test_self_command(self):
        self.assertEqual(hostos.self_command(), [sys.executable, "-m", "companion"])
        self.assertEqual(hostos.self_cwd(), os.path.abspath(HOST))
        with mock.patch.object(sys, "frozen", True, create=True), mock.patch.object(sys, "executable", "/opt/cm/cm"):
            self.assertEqual(hostos.self_command(), ["/opt/cm/cm"])
            self.assertIsNone(hostos.self_cwd())
            with mock.patch.dict(os.environ, {"APPIMAGE": "/home/u/CM.AppImage"}):
                self.assertEqual(hostos.self_command(), ["/home/u/CM.AppImage"])

    def test_pid_alive(self):
        self.assertTrue(hostos.pid_alive(os.getpid()))
        self.assertFalse(hostos.pid_alive(None))
        self.assertFalse(hostos.pid_alive(-3))
        p = subprocess.Popen([sys.executable, "-c", "pass"])
        p.wait()
        self.assertFalse(hostos.pid_alive(p.pid))

    def test_open_path(self):
        with mock.patch("subprocess.Popen") as po, mock.patch.object(hostos, "IS_WINDOWS", False), \
                mock.patch.object(hostos, "IS_MAC", False):
            self.assertTrue(hostos.open_path("/tmp/x.log"))
            self.assertEqual(po.call_args[0][0], ["xdg-open", "/tmp/x.log"])
        with mock.patch("subprocess.Popen") as po, mock.patch.object(hostos, "IS_WINDOWS", False), \
                mock.patch.object(hostos, "IS_MAC", True):
            hostos.open_path("https://claude.ai/settings/usage")
            self.assertEqual(po.call_args[0][0], ["open", "https://claude.ai/settings/usage"])
        with mock.patch.object(hostos, "IS_WINDOWS", True), \
                mock.patch.object(os, "startfile", create=True) as sf:
            self.assertTrue(hostos.open_path("C:\\x.log"))
            sf.assert_called_once_with("C:\\x.log")


# ---------------------------------------------------------------------------------------------
HOOK = {"hook_event_name": "PreToolUse", "session_id": "abcdef12-0000-0000-0000-000000000000",
        "cwd": "/home/u/proj", "tool_name": "Bash", "tool_input": {"command": "ls -la", "description": "List"}}


class HooksIpcTest(TempHome):
    def _poll_until(self, hp, n, timeout=2.0):
        t0 = time.time()
        while hp.received < n and time.time() - t0 < timeout:
            hp.poll(time.time())
            time.sleep(0.01)

    def test_udp_path(self):
        """The Windows transport, for real on loopback: port file, key check, StatusLine, cleanup."""
        hp = HooksProvider(transport="udp")
        hp.poll(time.time())
        try:
            self.assertIsNotNone(hp.sock)
            pf = os.path.join(self.state, "hooks.port")
            port, key = hookclient.read_port_file(pf)
            self.assertEqual(port, hp.port)
            self.assertEqual(key, hp.key)
            self.assertEqual(len(key), 16)
            with mock.patch.object(hookclient, "IS_WINDOWS", True):
                tgt = hookclient.target()
                self.assertEqual(tgt, ("udp", ("127.0.0.1", port), key))
                hookclient.send(hookclient.compact(HOOK, key=tgt[2]), tgt)
                self._poll_until(hp, 1)
                self.assertEqual(hp.received, 1)
                self.assertEqual(hp.live()["abcdef12"]["status"], "busy")
                # a datagram without the key is dropped
                hookclient.send(hookclient.compact(dict(HOOK, session_id="ffffffff-1"), key="nope"), tgt)
                t0 = time.time()
                while hp.rejected < 1 and time.time() - t0 < 2:
                    hp.poll(time.time())
                    time.sleep(0.01)
                self.assertEqual(hp.rejected, 1)
                self.assertNotIn("ffffffff", hp.live())
                # statusLine wrapper: statusline.json + datagram with the rate limits
                rl = {"five_hour": {"used_percentage": 12.5, "resets_at": int(time.time()) + 3600}}
                hookclient.record({"session_id": HOOK["session_id"], "rate_limits": rl,
                                   "model": {"display_name": "Opus"}})
                self._poll_until(hp, 2)
                self.assertEqual(hp.rate_limits, rl)
                with open(os.path.join(self.state, "statusline.json")) as f:
                    self.assertEqual(json.load(f)["rate_limits"], rl)
            # someone deletes the port file: rebound + rewritten on the next poll
            os.unlink(pf)
            hp.poll(time.time())
            self.assertTrue(os.path.exists(pf))
        finally:
            hp.close()
        self.assertFalse(os.path.exists(os.path.join(self.state, "hooks.port")))
        self.assertIsNone(hookclient.target())

    def test_udp_via_subprocess_script(self):
        """cm_hook.py -I -S over UDP, as Claude Code runs it (sock file absent => port file)."""
        hp = HooksProvider(transport="udp")
        hp.poll(time.time())
        try:
            r = subprocess.run([sys.executable, "-I", "-S", os.path.join(HOST, "hooks", "cm_hook.py")],
                               input=json.dumps(HOOK).encode(), capture_output=True, timeout=10,
                               env=dict(os.environ))
            self.assertEqual((r.returncode, r.stdout), (0, b""))
            self._poll_until(hp, 1)
            self.assertEqual(hp.received, 1)
        finally:
            hp.close()

    @unittest.skipUnless(POSIX and hasattr(socket, "AF_UNIX"), "AF_UNIX")
    def test_unix_path_and_cli_fast_path(self):
        hp = HooksProvider()
        self.assertEqual(hp.transport, "unix")
        hp.poll(time.time())
        try:
            self.assertTrue(os.path.exists(os.path.join(self.state, "hooks.sock")))
            self.assertEqual(hookclient.target()[0], "unix")
            t0 = time.time()
            r = subprocess.run([sys.executable, "-m", "companion", "hook"], cwd=HOST,
                               input=json.dumps(HOOK).encode(), capture_output=True, timeout=10)
            dt = time.time() - t0
            self.assertEqual((r.returncode, r.stdout), (0, b""))
            self.assertLess(dt, 1.0)   # ~15 ms here; generous bound for slow CI
            self._poll_until(hp, 1)
            self.assertEqual(hp.received, 1)
        finally:
            hp.close()
        self.assertFalse(os.path.exists(os.path.join(self.state, "hooks.sock")))

    def test_hook_never_fails(self):
        for stdin in (b"", b"not json", b"[1,2]", json.dumps(HOOK).encode()):
            r = subprocess.run([sys.executable, "-I", "-S", os.path.join(HOST, "hooks", "cm_hook.py")],
                               input=stdin, capture_output=True, timeout=10)
            self.assertEqual((r.returncode, r.stdout, r.stderr), (0, b"", b""))

    def test_fast_path_imports_nothing_heavy(self):
        r = subprocess.run([sys.executable, "-X", "importtime", "-m", "companion", "hook"], cwd=HOST,
                           input=json.dumps(HOOK).encode(), capture_output=True, timeout=10)
        mods = {line.split("|")[-1].strip() for line in r.stderr.decode().splitlines() if "|" in line}
        for heavy in ("serial", "psutil", "PIL", "esptool", "companion.daemon", "logging", "argparse"):
            self.assertNotIn(heavy, mods)
        self.assertIn("companion.hookclient", mods)

    def test_statusline_chains(self):
        cmd = f'"{sys.executable}" -c "import sys; d=sys.stdin.read(); print(\'chained\', len(d))"'
        with open(os.path.join(self.state, "statusline-chain.json"), "w") as f:
            json.dump({"command": cmd}, f)
        r = subprocess.run([sys.executable, "-m", "companion", "statusline"], cwd=HOST, input=b'{"a":1}',
                           capture_output=True, timeout=20)
        self.assertEqual(r.returncode, 0)
        self.assertEqual(r.stdout.decode().strip(), "chained 7")
        self.assertTrue(os.path.exists(os.path.join(self.state, "statusline.json")))

    def test_compact_limits(self):
        big = dict(HOOK, prompt="x" * 5000, last_assistant_message="y" * 5000,
                   tool_input={k: "z" * 3000 for k in hookclient.INPUT_KEYS})
        data = hookclient.compact(big, key="k" * 16)
        self.assertLessEqual(len(data), hookclient.MAXDGRAM)
        self.assertEqual(json.loads(data)["k"], "k" * 16)


# ---------------------------------------------------------------------------------------------
class HookSetupTest(TempHome):
    def settings(self):
        with open(hooksetup.settings_path()) as f:
            return json.load(f)

    def commands(self):
        d = self.settings()
        return [h["command"] for gs in d.get("hooks", {}).values() for g in gs for h in g["hooks"]]

    def test_command_forms(self):
        with mock.patch.object(sys, "frozen", True, create=True), \
                mock.patch.object(sys, "executable", "/Applications/Claudius Maximus.app/Contents/MacOS/claudius-maximus"):
            self.assertEqual(hooksetup.hook_command("hook"),
                             '"/Applications/Claudius Maximus.app/Contents/MacOS/claudius-maximus" hook')
            self.assertEqual(hooksetup.hook_command("statusline"),
                             '"/Applications/Claudius Maximus.app/Contents/MacOS/claudius-maximus" statusline')
        cmd = hooksetup.hook_command("hook")
        self.assertEqual(cmd, f'"{sys.executable}" -I -S "{os.path.join(os.path.abspath(HOST), "hooks", "cm_hook.py")}"')
        with mock.patch.object(hooksetup, "hooks_script_dir", return_value=None):
            self.assertEqual(hooksetup.hook_command("statusline"), f'"{sys.executable}" -m companion statusline')
        ours = ['"C:\\Users\\u\\AppData\\Local\\Programs\\Claudius Maximus\\ClaudiusMaximus.exe" hook',
                '"/opt/claudius-maximus/claudius-maximus" hook', "/x/py -I -S /r/host/hooks/cm_hook.py",
                '"/usr/bin/python3" -m companion hook']
        for c in ours:
            self.assertTrue(hooksetup.is_ours_command(c), c)
            self.assertFalse(hooksetup.is_ours_command(c, "statusline"), c)
        for c in ("my-hook.sh", "claude hook", '"/bin/foo" hook', "echo claudius-maximus hook done"):
            self.assertFalse(hooksetup.is_ours_command(c), c)
        self.assertTrue(hooksetup.is_ours_command('"/a/claudius-maximus" statusline', "statusline"))

    def test_quote_special_chars(self):
        if os.name != "nt":
            self.assertEqual(hooksetup.quote("/a/$HOME/b"), "'/a/$HOME/b'")
        self.assertEqual(hooksetup.quote("/a b/c"), '"/a b/c"')

    def test_install_switch_forms_and_uninstall(self):
        os.makedirs(os.path.dirname(hooksetup.settings_path()))
        user = {"hooks": {"Stop": [{"hooks": [{"type": "command", "command": "my-stop.sh"}]}]},
                "statusLine": {"type": "command", "command": "my-status.sh"}, "theme": "dark"}
        with open(hooksetup.settings_path(), "w") as f:
            json.dump(user, f)
        out = io.StringIO()
        with mock.patch("sys.stdout", out):
            self.assertEqual(hooksetup.install("/repo", "/repo/host/.venv/bin/python", True), 0)
        cmds = self.commands()
        hook_py = os.path.join('/repo', 'host', 'hooks', 'cm_hook.py')
        self.assertIn(f'"/repo/host/.venv/bin/python" -I -S "{hook_py}"', cmds)
        self.assertEqual(len([c for c in cmds if "cm_hook.py" in c]), len(hooksetup.EVENTS))
        # the app installs its own form: replaces ours, keeps the user's, statusLine chain preserved
        exe = "/opt/Claudius Maximus/claudius-maximus"
        with mock.patch.object(sys, "frozen", True, create=True), mock.patch.object(sys, "executable", exe), \
                mock.patch("sys.stdout", out):
            self.assertEqual(hooksetup.install_for_app(True), 0)
            self.assertEqual(hooksetup.check(), 0)
        cmds = self.commands()
        self.assertEqual(cmds.count(f'"{exe}" hook'), len(hooksetup.EVENTS))
        self.assertFalse(any("cm_hook.py" in c for c in cmds))
        self.assertIn("my-stop.sh", cmds)
        d = self.settings()
        self.assertEqual(d["statusLine"]["command"], f'"{exe}" statusline')
        with open(os.path.join(self.state, "statusline-chain.json")) as f:
            self.assertEqual(json.load(f)["command"], "my-status.sh")
        inst = hooksetup.installed()
        self.assertTrue(inst["hooks"] and inst["statusline"])
        # the cm_settings.py shim exposes the same API
        sys.path.insert(0, os.path.join(HOST, "hooks"))
        try:
            import cm_settings
            self.assertIs(cm_settings.install, hooksetup.install)
            self.assertTrue(callable(cm_settings.install_for_app))
        finally:
            sys.path.remove(os.path.join(HOST, "hooks"))
        with mock.patch("sys.stdout", out):
            self.assertEqual(hooksetup.uninstall(), 0)
        d = self.settings()
        self.assertEqual(d["statusLine"]["command"], "my-status.sh")
        self.assertEqual(d["theme"], "dark")
        self.assertEqual(self.commands(), ["my-stop.sh"])
        self.assertEqual(hooksetup.check(), 1)


# ---------------------------------------------------------------------------------------------
class FakeWinreg:
    HKEY_CURRENT_USER = "HKCU"
    REG_SZ = 1
    KEY_SET_VALUE = 2

    def __init__(self):
        self.values: dict = {}

    class _Key:
        def __init__(self, reg, path):
            self.reg, self.path = reg, path

        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

    def CreateKey(self, root, path):
        return self._Key(self, path)

    def OpenKey(self, root, path, *a):
        return self._Key(self, path)

    def SetValueEx(self, k, name, _r, _t, value):
        self.values[(k.path, name)] = value

    def QueryValueEx(self, k, name):
        try:
            return self.values[(k.path, name)], 1
        except KeyError:
            raise FileNotFoundError(name) from None

    def DeleteValue(self, k, name):
        try:
            del self.values[(k.path, name)]
        except KeyError:
            raise FileNotFoundError(name) from None


class AutostartTest(TempHome):
    def os_(self, name):
        return [mock.patch.object(hostos, "IS_MAC", name == "mac"),
                mock.patch.object(hostos, "IS_LINUX", name == "linux"),
                mock.patch.object(hostos, "IS_WINDOWS", name == "win")]

    def _with(self, name):
        ps = self.os_(name)
        for p in ps:
            p.start()
            self.addCleanup(p.stop)

    def test_never_runs_service_tools_in_test_home(self):
        self.assertFalse(autostart._real_home())
        with mock.patch("subprocess.run") as run:
            self.assertEqual(autostart._run(["systemctl", "--user", "disable", "--now", "x"]), 0)
            run.assert_not_called()

    @unittest.skipIf(sys.platform == "win32", "desktop entries escape backslashes; Windows paths are meaningless here")
    def test_linux_desktop_entry(self):
        self._with("linux")
        self.assertFalse(autostart.is_enabled())
        msg = autostart.enable()
        p = os.path.join(self.home, ".config", "autostart", "claudius-maximus.desktop")
        self.assertIn(p, msg)
        self.assertTrue(autostart.is_enabled())
        with open(p) as f:
            txt = f.read()
        self.assertIn("[Desktop Entry]", txt)
        self.assertIn(f"Exec={sys.executable} -m companion app\n", txt.replace(f'"{sys.executable}"', sys.executable))
        self.assertIn(f"Path={os.path.abspath(HOST)}", txt)
        self.assertIn("X-KDE-autostart-after=panel", txt)
        autostart.disable()
        self.assertFalse(autostart.is_enabled())
        autostart.disable()  # idempotent

    def test_desktop_quoting(self):
        with mock.patch.object(sys, "frozen", True, create=True), \
                mock.patch.dict(os.environ, {"APPIMAGE": "/home/u/My Apps/CM-0.3.0.AppImage"}):
            txt = autostart.desktop_bytes().decode()
        self.assertIn('Exec="/home/u/My Apps/CM-0.3.0.AppImage" app\n', txt)
        self.assertNotIn("Path=", txt)

    def test_linux_legacy_service(self):
        self._with("linux")
        unit = os.path.join(self.home, ".config", "systemd", "user", "claudius-maximus.service")
        self.assertFalse(autostart.legacy_service_installed())
        os.makedirs(os.path.dirname(unit))
        open(unit, "w").close()
        self.assertTrue(autostart.legacy_service_installed())
        with mock.patch.object(autostart, "_run", return_value=0) as run:
            msg = autostart.enable()
        run.assert_called_once_with(["systemctl", "--user", "disable", "--now", "claudius-maximus.service"])
        self.assertIn("stopped and disabled the headless systemd user service", msg)

    def test_mac_launch_agent(self):
        self._with("mac")
        legacy = os.path.join(self.home, "Library", "LaunchAgents", "com.claudius-maximus.daemon.plist")
        os.makedirs(os.path.dirname(legacy))
        open(legacy, "w").close()
        with mock.patch.object(autostart, "_run", return_value=0) as run:
            msg = autostart.enable()
        self.assertEqual(run.call_args_list[0][0][0][:2], ["launchctl", "bootout"])
        self.assertEqual(len(run.call_args_list), 1)   # no bootstrap: would start a second app right now
        self.assertIn("LaunchAgent", msg)
        self.assertFalse(os.path.exists(legacy))
        self.assertTrue(os.path.exists(legacy + ".disabled-by-app"))
        self.assertFalse(autostart.legacy_service_installed())
        p = os.path.join(self.home, "Library", "LaunchAgents", "com.claudius-maximus.app.plist")
        with open(p, "rb") as f:
            d = plistlib.load(f)
        self.assertEqual(d["Label"], "com.claudius-maximus.app")
        self.assertEqual(d["ProgramArguments"], [sys.executable, "-m", "companion", "app"])
        self.assertTrue(d["RunAtLoad"])
        self.assertFalse(d["KeepAlive"])
        self.assertEqual(d["WorkingDirectory"], os.path.abspath(HOST))
        self.assertTrue(autostart.is_enabled())
        autostart.disable()
        self.assertFalse(autostart.is_enabled())

    def test_windows_run_key(self):
        self._with("win")
        reg = FakeWinreg()
        with mock.patch.object(autostart, "_winreg", return_value=reg), \
                mock.patch.object(sys, "frozen", True, create=True), \
                mock.patch.object(sys, "executable", r"C:\Users\u\AppData\Local\Programs\Claudius Maximus\ClaudiusMaximus.exe"):
            self.assertFalse(autostart.is_enabled())
            autostart.enable()
            self.assertTrue(autostart.is_enabled())
            self.assertEqual(reg.values[(autostart.RUN_KEY, "ClaudiusMaximus")],
                             r'"C:\Users\u\AppData\Local\Programs\Claudius Maximus\ClaudiusMaximus.exe" app')
            autostart.disable()
            self.assertFalse(autostart.is_enabled())
        self.assertFalse(autostart.legacy_service_installed())

    def test_windows_unfrozen_bootstrap(self):
        self._with("win")
        cmd = autostart.app_command()
        self.assertEqual(cmd[1], "-c")
        self.assertIn(repr(hostos.package_root()), cmd[2])
        self.assertIn("main(['app'])", cmd[2])


# ---------------------------------------------------------------------------------------------
class FakeLink:
    def __init__(self):
        self.connects = 0
        self.ser = None
        self.last_error = "no device found"
        self.port_name = None

    def connect(self):
        self.connects += 1
        self.ser = object()
        return True

    @property
    def connected(self):
        return self.ser is not None

    @property
    def port(self):
        return "/dev/fake0" if self.ser else None

    def close(self):
        self.ser = None

    def send(self, msg):
        return self.ser is not None

    def read_messages(self):
        return []

    def lock_holder(self):
        return ""

    def fileno(self):
        return None


def native(device, serial=None):
    from companion.flasher import Port
    return Port(device=device, kind="native", vid=0x303A, pid=0x1001, serial=serial, description="USB JTAG")


class DaemonApiTest(TempHome):
    def setUp(self):
        super().setUp()
        # one board on USB, whatever the machine running the tests has plugged in
        p = mock.patch("companion.flasher.detect", return_value=[native("/dev/fake0", "AA")])
        p.start()
        self.addCleanup(p.stop)

    def make(self, dry_run=True):
        d = daemon_mod.Daemon(demo=True, dry_run=dry_run, out=io.StringIO())
        return d

    def test_public_status(self):
        rl = {"five_hour": {"used_percentage": 40, "resets_at": 1}}
        with open(os.path.join(self.state, "statusline.json"), "w") as f:
            json.dump({"rate_limits": rl, "rate_limits_at": 5}, f)
        d = self.make()
        d.handle_device({"t": "hello", "fw": "0.3.0", "board": "ESP32-S3-Touch-LCD-4B", "caps": ["part"]})
        d.tick()
        st = d.public_status()
        for k in ("pid", "started", "connected", "port", "fw", "board", "last_hello", "device_error", "hooks",
                  "sessions", "rate_limits", "updated"):
            self.assertIn(k, st)
        self.assertEqual(st["fw"], "0.3.0")
        self.assertEqual(st["board"], "ESP32-S3-Touch-LCD-4B")
        self.assertEqual(st["rate_limits"], rl)
        self.assertGreater(len(st["sessions"]), 0)
        st["sessions"].clear()
        self.assertGreater(len(d.public_status()["sessions"]), 0)   # a copy
        json.dumps(st)

    def test_pause_resume(self):
        d = self.make(dry_run=False)
        fake = d.link = FakeLink()
        with mock.patch.object(daemon_mod, "dev_wants_device", return_value=False):
            d.ensure_connected(time.time())
            self.assertEqual(fake.connects, 1)
            d.fw, d.board = "0.2.0", "x"
            d.pause_device("flashing")
            self.assertFalse(fake.connected)
            st = d.public_status()
            self.assertEqual(st["device_error"], "paused: flashing")
            self.assertTrue(st["paused"])
            for _ in range(3):
                d.ensure_connected(time.time() + 100)
            self.assertEqual(fake.connects, 1)
            d.resume_device()
            self.assertIsNone(d.last_body)
            self.assertIsNone(d.fw)
            d.ensure_connected(time.time())
            self.assertEqual(fake.connects, 2)
            self.assertEqual(d.public_status()["device_error"], "")
            d.resume_device()  # no-op when not paused

    def test_yield_reconnect_is_quiet(self):
        """The periodic lock yield (YIELD_EVERY_S) closes and reopens the port on purpose: that reconnect and the
        hello it provokes must not land in the INFO log (they filled it with two lines every 22 s)."""
        d = self.make(dry_run=False)
        fake = d.link = FakeLink()
        hello = {"t": "hello", "fw": "0.4.0", "board": "ESP32-S3-Touch-LCD-4B", "caps": ["part", "screen"]}
        t = time.time()
        with mock.patch.object(daemon_mod, "dev_wants_device", return_value=False), \
                mock.patch.object(daemon_mod, "YIELD_EVERY_S", 20.0):
            with self.assertLogs("companion", level="INFO") as first:
                d.ensure_connected(t)
                d.handle_device(hello)
            self.assertEqual([r.getMessage() for r in first.records],
                             ["device connected on /dev/fake0", "device hello fw=0.4.0"])
            with self.assertLogs("companion", level="DEBUG") as cycle:
                d.ensure_connected(t + 21)            # yields: port closed
                self.assertFalse(fake.connected)
                d.ensure_connected(t + 21.5)          # still in the YIELD_FOR_S gap
                self.assertFalse(fake.connected)
                d.ensure_connected(t + 23)            # reconnects
                self.assertTrue(fake.connected)
                self.assertEqual(fake.connects, 2)
                d.handle_device(hello)                # same firmware says hello again
            self.assertEqual([r.levelname for r in cycle.records], ["DEBUG"] * len(cycle.records))
            self.assertIn("device connected on /dev/fake0", [r.getMessage() for r in cycle.records])
            self.assertIn("device hello fw=0.4.0", [r.getMessage() for r in cycle.records])
            self.assertEqual(d.fw, "0.4.0")
            # a different firmware after a yield (someone reflashed meanwhile) is news: INFO
            d.ensure_connected(t + 44)
            d.ensure_connected(t + 46)
            with self.assertLogs("companion", level="INFO") as new_fw:
                d.handle_device(dict(hello, fw="0.4.1"))
            self.assertEqual([r.getMessage() for r in new_fw.records], ["device hello fw=0.4.1"])
            # a real disconnect (device gone) then reconnect is news too
            fake.ser = None
            fake.last_error = "read: device gone"
            d._on_disconnect(fake.last_error)
            with self.assertLogs("companion", level="INFO") as back:
                d.ensure_connected(t + 60)
                d.handle_device(dict(hello, fw="0.4.1"))
            self.assertEqual([r.getMessage() for r in back.records],
                             ["device connected on /dev/fake0", "device hello fw=0.4.1"])

    def test_two_boards_connects_only_to_ours(self):
        """Two ESP32-S3 on USB: the daemon probes them for a hello and opens the one that answers; none answering
        means no connection and a status that says to choose the port; a stored choice is followed, also when it
        changes while connected (tray menu / `port` command)."""
        d = self.make(dry_run=False)
        fake = d.link = FakeLink()
        boards = [native("/dev/ttyACM0", "AA"), native("/dev/ttyACM1", "BB")]
        probes = []

        def probe(device):
            probes.append(device)
            return {"t": "hello", "fw": "0.4.1", "board": "ESP32-S3-Touch-LCD-4B"} if device == "/dev/ttyACM1" else None
        d.chooser.probe = probe
        t = time.time()
        with mock.patch.object(daemon_mod, "dev_wants_device", return_value=False), \
                mock.patch("companion.flasher.detect", return_value=boards):
            d.ensure_connected(t)
            self.assertEqual(fake.connects, 1)
            self.assertEqual(fake.port_name, "/dev/ttyACM1")
            self.assertEqual(probes, ["/dev/ttyACM0", "/dev/ttyACM1"])
            st = d.public_status()
            self.assertEqual(st["port_choice"], "auto")
            ours = [p for p in st["ports"] if p["device"] == "/dev/ttyACM1"][0]
            self.assertTrue(ours["ours"])
            self.assertEqual(ours["fw"], "0.4.1")
            self.assertFalse([p for p in st["ports"] if p["device"] == "/dev/ttyACM0"][0]["ours"])
            # replugged under another name: known by USB serial, no new probe
            fake.ser = None
            d._on_disconnect("gone")
            renamed = [native("/dev/ttyACM0", "AA"), native("/dev/ttyACM5", "BB")]
            with mock.patch("companion.flasher.detect", return_value=renamed):
                d.ensure_connected(t + 10)
            self.assertEqual(fake.port_name, "/dev/ttyACM5")
            self.assertEqual(len(probes), 2)
        # nobody answers: no connection, a reason, probes not repeated within PROBE_RETRY_S
        d2 = self.make(dry_run=False)
        fake2 = d2.link = FakeLink()
        d2.chooser.probe = lambda device: None
        with mock.patch.object(daemon_mod, "dev_wants_device", return_value=False), \
                mock.patch("companion.flasher.detect", return_value=boards):
            d2.ensure_connected(t)
            d2.ensure_connected(t + 3)
            self.assertEqual(fake2.connects, 0)
            self.assertIn("none runs the Claudius Maximus firmware", d2.public_status()["device_error"])
            self.assertEqual(len(d2.chooser.failed_at), 2)
            # the user chooses one: connect to it, flash would use the same one (settings.json)
            msg = d2.set_port("/dev/ttyACM0")
            self.assertEqual(msg, "device port: /dev/ttyACM0 (USB serial AA)")
            d2.ensure_connected(t + 4)
            self.assertEqual((fake2.connects, fake2.port_name), (1, "/dev/ttyACM0"))
            self.assertEqual(d2.public_status()["port_choice"], "/dev/ttyACM0")
            from companion import ports as portsel
            self.assertEqual(portsel.selection(), ("/dev/ttyACM0", "AA"))
            # the choice changes from outside (CLI `port`) while connected: picked up within PORTS_REFRESH_S
            portsel.select("/dev/ttyACM1", boards)
            d2.ensure_connected(t + 4 + daemon_mod.PORTS_REFRESH_S + 1)
            self.assertFalse(fake2.connected)
            d2.ensure_connected(t + 4 + daemon_mod.PORTS_REFRESH_S + 1.1)
            self.assertEqual((fake2.connects, fake2.port_name), (2, "/dev/ttyACM1"))
            # a choice that is not plugged in is honoured too: leave the board we are on, wait for the chosen one
            portsel.select("/dev/ttyACM9", boards)
            t2 = t + 4 + 2 * (daemon_mod.PORTS_REFRESH_S + 1)
            d2.ensure_connected(t2)
            self.assertFalse(fake2.connected)
            d2.ensure_connected(t2 + 0.1)
            self.assertEqual(fake2.connects, 2)
            self.assertIn("/dev/ttyACM9 is not connected", d2.public_status()["device_error"])
            # back to automatic
            self.assertEqual(d2.set_port(None), "device port: automatic")
            self.assertEqual(portsel.selection(), (None, None))

    def test_runs_in_thread(self):
        d = self.make()
        rc = []
        t = threading.Thread(target=lambda: rc.append(daemon_mod.run(daemon=d)))
        with mock.patch("signal.signal") as sig:
            t.start()
            time.sleep(0.5)
            st = d.public_status()
            d.stop = True
            t.join(5)
        self.assertFalse(t.is_alive())
        self.assertEqual(rc, [0])
        sig.assert_not_called()
        self.assertEqual(st["device_error"], "dry run")
        self.assertIn('"t": "state"', d.out.getvalue())

    def test_windows_wait(self):
        d = self.make()
        d.link = FakeLink()

        class Ser:
            in_waiting = 0
        d.link.ser = Ser()
        with mock.patch.object(sys, "platform", "win32"), mock.patch.object(daemon_mod, "TICK_S", 0.2):
            t0 = time.time()
            d.wait()
            self.assertGreaterEqual(time.time() - t0, 0.15)
            Ser.in_waiting = 3
            t0 = time.time()
            d.wait()
            self.assertLess(time.time() - t0, 0.05)


# ---------------------------------------------------------------------------------------------
class CliTest(TempHome):
    def run_cli(self, *args, timeout=60):
        return subprocess.run([sys.executable, "-m", "companion", *args], cwd=HOST, capture_output=True,
                              text=True, timeout=timeout)

    def test_parser_and_defaults(self):
        from companion.__main__ import build_parser
        a = build_parser().parse_args(["run", "--demo", "--dry-run", "--seconds", "2", "-v"])
        self.assertEqual((a.command, a.demo, a.dry_run, a.seconds, a.verbose), ("run", True, True, 2.0, True))
        a = build_parser().parse_args(["flash", "--board", "4b", "--erase"])
        self.assertEqual((a.board, a.erase, a.port), ("4b", True, None))
        a = build_parser().parse_args(["autostart"])
        self.assertEqual(a.action, "status")

    def test_legacy_flags_mean_run(self):
        r = self.run_cli("--dry-run", "--demo", "--seconds", "1")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('"t": "state"', r.stdout)

    def test_hooks_and_autostart_cli(self):
        r = self.run_cli("hooks", "check")
        self.assertEqual(r.returncode, 1)
        self.assertIn("not installed", r.stdout)
        r = self.run_cli("hooks", "install")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.run_cli("hooks", "check").returncode, 0)
        self.assertEqual(self.run_cli("hooks", "uninstall").returncode, 0)
        r = self.run_cli("autostart", "status")
        self.assertIn("disabled", r.stdout)

    def test_status_json(self):
        r = self.run_cli("status", "--json")
        rep = json.loads(r.stdout)
        self.assertFalse(rep["daemon_running"])
        self.assertIn("hooks_installed", rep)



# ---------------------------------------------------------------------------------------------
# xplat review: Windows / macOS code paths exercised on any OS
# ---------------------------------------------------------------------------------------------
class FakeMsvcrt:
    LK_NBLCK, LK_UNLCK = 2, 0

    def __init__(self):
        self.calls = []
        self.held = set()

    def locking(self, fd, mode, n):
        pos = os.lseek(fd, 0, os.SEEK_CUR)
        self.calls.append((mode, pos, n))
        if mode == self.LK_NBLCK:
            if pos in self.held:
                raise PermissionError(13, "locked")
            self.held.add(pos)
        else:
            self.held.discard(pos)


class WindowsLockingTest(TempHome):
    def _win_locking(self):
        import importlib
        fake = FakeMsvcrt()
        with mock.patch.object(sys, "platform", "win32"), mock.patch.dict(sys.modules, {"msvcrt": fake}):
            mod = importlib.reload(locking)
        self.addCleanup(importlib.reload, locking)
        return mod, fake

    def test_lock_byte_is_past_the_holder_text(self):
        mod, fake = self._win_locking()
        p = os.path.join(self.tmp, ".devlock")
        a, b = mod.open_lock_file(p), mod.open_lock_file(p)
        try:
            self.assertTrue(mod.try_lock(a))
            self.assertFalse(mod.try_lock(b))
            self.assertEqual(fake.calls[0], (fake.LK_NBLCK, mod.WIN_LOCK_OFFSET, 1))
            self.assertEqual(os.lseek(a, 0, os.SEEK_CUR), 0)   # position restored for the holder write
            mod.write_holder(a, "daemon pid=7\n")
            mod.write_holder(a, "x\n")                        # shorter: truncated after the write
            self.assertEqual(read_lock_holder(p), "x")
            with open(p, "rb") as f:
                self.assertEqual(f.read(), b" x\n")            # binary: no CRLF, nothing near the lock byte
            mod.unlock(a)
            self.assertEqual(fake.calls[-1], (fake.LK_UNLCK, mod.WIN_LOCK_OFFSET, 1))
            self.assertTrue(mod.try_lock(b))
        finally:
            os.close(a)
            os.close(b)


class FrozenDaemonImportTest(TempHome):
    def test_dev_dir_frozen_uses_state_dir(self):
        with mock.patch.object(hostos, "is_frozen", return_value=True):
            self.assertEqual(str(daemon_mod.dev_dir()), hostos.state_dir())

    def test_frozen_app_does_not_yield_the_lock(self):
        """A frozen app locks in the state dir, which tools/dev.py never looks at: yielding there is pointless.
        CLAUDIUS_YIELD_S still overrides."""
        code = "import sys; sys.frozen = True; import companion.daemon as d; print(d.YIELD_EVERY_S)"
        r = subprocess.run([sys.executable, "-c", code], cwd=HOST, capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "0.0")
        env = dict(os.environ, CLAUDIUS_YIELD_S="7")
        r = subprocess.run([sys.executable, "-c", code], cwd=HOST, capture_output=True, text=True, timeout=30,
                           env=env)
        self.assertEqual(r.stdout.strip(), "7.0")
        code = "import companion.daemon as d; print(d.YIELD_EVERY_S)"
        env = dict(os.environ); env.pop("CLAUDIUS_YIELD_S", None)
        r = subprocess.run([sys.executable, "-c", code], cwd=HOST, capture_output=True, text=True, timeout=30,
                           env=env)
        self.assertEqual(r.stdout.strip(), "20.0")   # source checkout keeps yielding

    def test_daemon_module_imports_when_frozen(self):
        """DEVLOCK is computed at import: a frozen build must not hit a NameError there."""
        code = ("import sys; sys.frozen = True; import companion.daemon as d; "
                "print(d.DEVLOCK.parent)")
        r = subprocess.run([sys.executable, "-c", code], cwd=HOST, capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(os.path.realpath(r.stdout.strip()), os.path.realpath(self.state))


class ShortSocketPathTest(TempHome):
    def _long_state(self):
        d = os.path.join(self.tmp, "x" * 60, "y" * 40, ".claudius-maximus")
        os.makedirs(d)
        os.environ["CLAUDIUS_HOME"] = d
        return d

    def test_short_paths_unchanged(self):
        self.assertEqual(hookclient.sock_path(), os.path.join(self.state, "hooks.sock"))
        from companion.providers import hooks as hooks_mod
        self.assertEqual(hooks_mod.sock_path(), hookclient.sock_path())

    @unittest.skipUnless(POSIX and hasattr(socket, "AF_UNIX"), "AF_UNIX")
    def test_long_state_dir_falls_back_to_tmp(self):
        d = self._long_state()
        sp = hookclient.sock_path()
        self.assertTrue(sp.startswith(f"/tmp/claudius-maximus-{os.getuid()}-"), sp)
        self.assertLessEqual(len(sp), hookclient.SUN_PATH_MAX)
        self.addCleanup(shutil.rmtree, os.path.dirname(sp), True)
        hp = HooksProvider()
        self.assertEqual(hp.path, sp)
        hp.poll(time.time())
        try:
            self.assertIsNotNone(hp.sock)
            self.assertEqual(os.stat(os.path.dirname(sp)).st_mode & 0o777, 0o700)
            self.assertEqual(hookclient.target(), ("unix", sp, ""))
            hookclient.send(hookclient.compact(HOOK), hookclient.target())
            for _ in range(50):
                hp.poll(time.time())
                if hp.received:
                    break
                time.sleep(0.02)
            self.assertEqual(hp.received, 1)
        finally:
            hp.close()
        self.assertTrue(d)

    @unittest.skipUnless(POSIX and hasattr(socket, "AF_UNIX"), "AF_UNIX")
    def test_foreign_tmp_dir_is_refused(self):
        self._long_state()
        sp = hookclient.sock_path()
        folder = os.path.dirname(sp)
        os.makedirs(folder, mode=0o755, exist_ok=True)
        self.addCleanup(shutil.rmtree, folder, True)
        os.chmod(folder, 0o755)
        real = os.lstat

        def lstat(p, *a, **k):   # pretend another user owns it
            st = real(p, *a, **k)
            if p == folder:
                return os.stat_result((st.st_mode, st.st_ino, st.st_dev, st.st_nlink, st.st_uid + 1, st.st_gid,
                                       st.st_size, st.st_atime, st.st_mtime, st.st_ctime))
            return st
        with mock.patch("os.lstat", lstat):
            self.assertFalse(hookclient.owned_by_me(folder))
            hp = HooksProvider()
            hp.poll(time.time())
            self.assertIsNone(hp.sock)
            open(sp, "w").close()
            self.assertIsNone(hookclient.target())   # never send to a socket in a dir we don't own


class StatuslineChainTest(TempHome):
    def test_windows_runs_chain_with_git_bash(self):
        bash = os.path.join(self.tmp, "Git", "bin", "bash.exe")
        os.makedirs(os.path.dirname(bash))
        open(bash, "w").close()
        with mock.patch.object(hookclient, "IS_WINDOWS", True), \
                mock.patch.dict(os.environ, {"CLAUDE_CODE_GIT_BASH_PATH": bash}):
            self.assertEqual(hookclient.chain_argv("~/sl.sh"), ([bash, "-c", "~/sl.sh"], False))
        os.environ.pop("CLAUDE_CODE_GIT_BASH_PATH", None)
        git = os.path.join(self.tmp, "Git", "cmd", "git.exe")
        os.makedirs(os.path.dirname(git))
        open(git, "w").close()
        with mock.patch.object(hookclient, "IS_WINDOWS", True), mock.patch("shutil.which", return_value=git):
            found = hookclient.chain_argv("x")[0][0]
            self.assertEqual(os.path.normcase(os.path.realpath(found)), os.path.normcase(os.path.realpath(bash)))
        with mock.patch.object(hookclient, "IS_WINDOWS", True), mock.patch("shutil.which", return_value=None):
            self.assertEqual(hookclient.chain_argv("x"), ("x", True))   # no Git Bash: cmd via shell=True
        with mock.patch.object(hookclient, "IS_WINDOWS", False):
            self.assertEqual(hookclient.chain_argv("x"), ("x", True))   # POSIX: /bin/sh as before

    def test_chain_subprocess_flags_on_windows(self):
        with open(os.path.join(self.state, "statusline-chain.json"), "w", encoding="utf-8") as f:
            json.dump({"command": "echo hi"}, f)
        seen = {}

        def run(args, **kw):
            seen.update(kw, args=args)
            return types_ns(stdout=b"hi\n")
        out = io.BytesIO()
        with mock.patch.object(hookclient, "IS_WINDOWS", True), mock.patch("subprocess.run", run), \
                mock.patch.object(hookclient, "_win_bash", return_value=None), \
                mock.patch.object(hookclient, "read_stdin", return_value=b"{}"), \
                mock.patch.object(hookclient, "write_stdout", out.write):
            hookclient.statusline_main()
        self.assertEqual(seen["creationflags"], 0x08000000)
        self.assertEqual(seen["stderr"], subprocess.DEVNULL)
        self.assertEqual(out.getvalue(), b"hi\n")


def types_ns(**kw):
    import types
    return types.SimpleNamespace(**kw)


class HookSetupEncodingTest(TempHome):
    def test_non_ascii_paths_and_bom(self):
        path = hooksetup.settings_path()
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:   # BOM + non-ASCII, as Notepad would save it
            f.write("\ufeff".encode("utf-8") + json.dumps({"x": "Zoë"}, ensure_ascii=False).encode("utf-8"))
        exe = "C:\\Users\\Zoë Doe\\AppData\\Local\\Programs\\ClaudiusMaximus\\ClaudiusMaximus.exe"
        with mock.patch("sys.stdout"):
            rc = hooksetup.install_commands(f'"{exe}" hook', f'"{exe}" statusline', True)
        self.assertEqual(rc, 0)
        with open(path, "rb") as f:
            raw = f.read()
        self.assertNotIn(b"\r\n", raw)
        d = json.loads(raw.decode("utf-8"))   # strict UTF-8, whatever the locale encoding is
        self.assertEqual(d["x"], "Zoë")
        self.assertEqual(d["hooks"]["Stop"][0]["hooks"][0]["command"], f'"{exe}" hook')
        self.assertTrue(hooksetup.is_ours_command(d["statusLine"]["command"], "statusline"))

    def test_installed_matches_self(self):
        self.assertFalse(hooksetup.installed_matches_self())
        with mock.patch("sys.stdout"):
            hooksetup.install_for_app(True)
        self.assertTrue(hooksetup.installed_matches_self())
        self.assertTrue(hooksetup.installed_matches_self(statusline=True))
        with mock.patch("sys.stdout"):   # the app moved: old commands are no longer ours-for-this-program
            hooksetup.install_commands('"/old/claudius-maximus" hook', '"/old/claudius-maximus" statusline', True)
        self.assertTrue(hooksetup.installed()["hooks"])
        self.assertFalse(hooksetup.installed_matches_self())


class MacAutostartLocationTest(TempHome):
    os_ = AutostartTest.os_
    _with = AutostartTest._with

    def test_unstable_locations(self):
        f = autostart.mac_unstable_location
        self.assertIn("Translocation", f("/private/var/folders/ab/T/AppTranslocation/1234/d/Claudius Maximus.app"
                                         "/Contents/MacOS/claudius-maximus"))
        self.assertIn("disk image", f("/Volumes/Claudius Maximus/Claudius Maximus.app/Contents/MacOS/x"))
        self.assertEqual(f("/Applications/Claudius Maximus.app/Contents/MacOS/claudius-maximus"), "")

    def test_enable_refuses_before_side_effects(self):
        self._with("mac")
        exe = "/Volumes/Claudius Maximus/Claudius Maximus.app/Contents/MacOS/claudius-maximus"
        with mock.patch.object(hostos, "is_frozen", return_value=True), mock.patch.object(sys, "executable", exe), \
                mock.patch.object(autostart, "disable_legacy_service") as legacy:
            with self.assertRaises(OSError) as cm:
                autostart.enable()
            legacy.assert_not_called()
        self.assertIn("Applications", str(cm.exception))
        self.assertFalse(os.path.exists(autostart.plist_path()))

    def test_bundle_plist(self):
        self._with("mac")
        exe = "/Applications/Claudius Maximus.app/Contents/MacOS/claudius-maximus"
        with mock.patch.object(hostos, "is_frozen", return_value=True), mock.patch.object(sys, "executable", exe), \
                mock.patch.object(autostart, "_run") as run:
            autostart.enable()
            run.assert_not_called()   # never launchctl bootstrap the app plist (would start a second app)
        with open(autostart.plist_path(), "rb") as f:
            d = plistlib.load(f)
        self.assertEqual(d["ProgramArguments"], [exe, "app"])
        self.assertTrue(d["RunAtLoad"])
        self.assertEqual(d["LimitLoadToSessionType"], "Aqua")
        self.assertNotIn("WorkingDirectory", d)
        self.assertTrue(autostart.is_enabled())


class AutostartStatusExitTest(TempHome):
    def test_status_query_exits_zero_when_disabled(self):
        from companion.__main__ import main
        with mock.patch("sys.stdout", new_callable=io.StringIO) as out:
            self.assertEqual(main(["autostart", "status"]), 0)
        self.assertIn("disabled", out.getvalue())


class PortsTest(TempHome):
    def test_selection_roundtrip_and_resolve(self):
        from companion import ports as portsel
        self.assertEqual(portsel.selection(), (None, None))
        boards = [native("/dev/ttyACM0", "AA"), native("/dev/ttyACM1", None)]
        self.assertEqual(portsel.select("/dev/ttyACM0", boards), "device port: /dev/ttyACM0 (USB serial AA)")
        self.assertEqual(portsel.selection(), ("/dev/ttyACM0", "AA"))
        self.assertEqual(portsel.select("/dev/ttyACM1", boards), "device port: /dev/ttyACM1")
        self.assertEqual(portsel.selection(), ("/dev/ttyACM1", None))
        self.assertEqual(portsel.select("auto"), "device port: automatic")
        self.assertEqual(portsel.selection(), (None, None))
        self.assertEqual(portsel.resolve(boards, "COM9", "AA").device, "/dev/ttyACM0")
        self.assertEqual(portsel.resolve(boards, "/dev/ttyACM1", None).device, "/dev/ttyACM1")
        self.assertIsNone(portsel.resolve(boards, "/dev/ttyACM9", "ZZ"))

    def test_chooser(self):
        from companion import ports as portsel
        from companion.flasher import Port
        clock = [1000.0]
        probes = []
        c = portsel.Chooser(probe=lambda dev: probes.append(dev) or None, now=lambda: clock[0])
        one = [native("/dev/ttyACM0", "AA")]
        uart = Port("/dev/ttyACM3", "uart", 0x1A86, 0x55D3, None, "")
        self.assertIsNone(c.choose([]))
        self.assertEqual(c.reason, "no device found")
        self.assertIsNone(c.choose([uart]))
        self.assertEqual(c.choose(one + [uart]).device, "/dev/ttyACM0")   # a single board: no probe
        self.assertEqual(probes, [])
        two = one + [native("/dev/ttyACM1", "BB")]
        self.assertIsNone(c.choose(two))
        self.assertEqual(probes, ["/dev/ttyACM0", "/dev/ttyACM1"])
        self.assertIn("2 ESP32-S3 boards", c.reason)
        clock[0] += 10
        self.assertIsNone(c.choose(two))                       # within PROBE_RETRY_S: not asked again
        self.assertEqual(len(probes), 2)
        clock[0] += portsel.PROBE_RETRY_S
        self.assertIsNone(c.choose(two))
        self.assertEqual(len(probes), 4)
        # --port on the command line wins, even if not enumerated
        self.assertEqual(c.choose(two, override="COM9").device, "COM9")
        # a manual choice: by serial, else by name; missing -> reason
        self.assertEqual(c.choose(two, manual=("/dev/ttyACM7", "BB")).device, "/dev/ttyACM1")
        self.assertIsNone(c.choose(two, manual=("/dev/ttyACM7", None)))
        self.assertTrue(c.manual_missing)
        # the daemon connected and got a hello: remembered by serial
        c.learned("/dev/ttyACM1", {"t": "hello", "fw": "0.4.1"}, two)
        self.assertEqual(c.known_serial, "BB")
        self.assertEqual(c.choose(two).device, "/dev/ttyACM1")
        self.assertEqual(len(probes), 4)
        desc = c.describe(two, connected="/dev/ttyACM1", fw="0.4.1", board="ESP32-S3-Touch-LCD-4B",
                          manual=(None, None))
        self.assertEqual([(d["device"], d["ours"], d["connected"], d["selected"], d["fw"]) for d in desc],
                         [("/dev/ttyACM0", False, False, False, None), ("/dev/ttyACM1", True, True, False, "0.4.1")])
        self.assertEqual(portsel.port_label(desc[1]), "/dev/ttyACM1 · ESP32-S3 · Claudius Maximus fw 0.4.1")
        self.assertEqual(portsel.port_label(desc[0]), "/dev/ttyACM0 · ESP32-S3")
        self.assertEqual(portsel.port_label({"device": "COM3", "kind": "uart"}), "COM3 · USB-TO-UART (CH343)")

    def test_port_cli(self):
        from companion.__main__ import main
        two = [native("/dev/ttyACM0", "AA"), native("/dev/ttyACM1", "BB")]
        with mock.patch("companion.flasher.detect", return_value=two):
            with mock.patch("sys.stdout", new_callable=io.StringIO) as out:
                self.assertEqual(main(["port"]), 0)
            self.assertIn("choice: automatic", out.getvalue())
            self.assertIn("/dev/ttyACM1 · ESP32-S3", out.getvalue())
            with mock.patch("sys.stdout", new_callable=io.StringIO) as out:
                self.assertEqual(main(["port", "/dev/ttyACM1"]), 0)
            self.assertEqual(out.getvalue().strip(), "device port: /dev/ttyACM1 (USB serial BB)")
            with mock.patch("sys.stdout", new_callable=io.StringIO) as out:
                main(["port"])
            self.assertIn("choice: /dev/ttyACM1 (USB serial BB)", out.getvalue())
            self.assertIn("[chosen]", out.getvalue())
            with mock.patch("sys.stdout", new_callable=io.StringIO) as out:
                self.assertEqual(main(["port", "auto"]), 0)
            self.assertEqual(out.getvalue().strip(), "device port: automatic")


class UpdaterTest(TempHome):
    API = {"tag_name": "v9.9.0", "html_url": "https://github.com/d-weber/claudius-maximus/releases/tag/v9.9.0",
           "body": "notes", "published_at": "2026-10-29T00:00:00Z",
           "assets": [{"name": "ClaudiusMaximus-9.9.0-linux-x64.AppImage", "browser_download_url": "https://dl/a", "size": 5},
                      {"name": "ClaudiusMaximus-9.9.0-windows-x64.exe", "browser_download_url": "https://dl/w", "size": 6}]}

    def test_asset_names_and_versions(self):
        from companion import updater
        self.assertEqual(updater.asset_name("1.2.3", "win32", "AMD64"), "ClaudiusMaximus-1.2.3-windows-x64.exe")
        self.assertEqual(updater.asset_name("1.2.3", "darwin", "arm64"), "ClaudiusMaximus-1.2.3-macos-arm64.dmg")
        self.assertEqual(updater.asset_name("1.2.3", "darwin", "x86_64"), "ClaudiusMaximus-1.2.3-macos-x64.dmg")
        self.assertEqual(updater.asset_name("1.2.3", "linux", "x86_64", appimage=True),
                         "ClaudiusMaximus-1.2.3-linux-x64.AppImage")
        self.assertIsNone(updater.asset_name("1.2.3", "linux", "x86_64", appimage=False))   # tar.gz install
        self.assertTrue(updater.is_newer("0.5.0", "0.4.1"))
        self.assertTrue(updater.is_newer("0.10.0", "0.9.9"))
        self.assertFalse(updater.is_newer("0.4.1", "0.4.1"))
        self.assertFalse(updater.is_newer("0.4.0", "0.4.1"))
        self.assertFalse(updater.is_newer("garbage", "0.4.1"))
        self.assertIn("source checkout", updater.can_self_install())

    def test_check_parses_the_release(self):
        from companion import updater
        with mock.patch.object(updater, "_get", return_value=json.dumps(self.API).encode()):
            rel = updater.check()
        self.assertEqual((rel.version, rel.tag), ("9.9.0", "v9.9.0"))
        self.assertEqual(rel.asset("ClaudiusMaximus-9.9.0-linux-x64.AppImage"), ("https://dl/a", 5))
        self.assertIsNone(rel.asset("nope"))
        import urllib.error
        with mock.patch.object(updater, "_get", side_effect=urllib.error.URLError("no network")):
            with self.assertRaises(updater.UpdateError) as cm:
                updater.check()
            self.assertIn("cannot reach", str(cm.exception))
        with mock.patch.object(updater, "_get", side_effect=urllib.error.HTTPError("u", 403, "rate", {}, None)):
            with self.assertRaises(updater.UpdateError) as cm:
                updater.check()
            self.assertIn("rate-limiting", str(cm.exception))
        with mock.patch.object(updater, "_get", return_value=b"[]"):
            with self.assertRaises(updater.UpdateError):
                updater.check()

    def test_daily_check_schedule_and_toggle(self):
        from companion import updater
        T = 1.7e9   # a real clock: never checked yet (0) means due
        self.assertTrue(updater.auto_enabled())
        self.assertTrue(updater.auto_check_due(T))
        updater.mark_checked(True, T)
        self.assertFalse(updater.auto_check_due(T + updater.CHECK_EVERY_S - 1))
        self.assertTrue(updater.auto_check_due(T + updater.CHECK_EVERY_S))
        self.assertEqual(updater.set_auto(False), "daily update check: off")
        self.assertFalse(updater.auto_check_due(1e12))
        updater.set_auto(True)
        self.assertTrue(updater.auto_check_due(1e12))

    def test_download_and_linux_install(self):
        """download: streamed to .part, size checked, renamed. Linux install: the new AppImage lands next to the
        running one, is made executable, hooks + autostart are pointed at it and it is started."""
        from companion import updater, autostart
        import io as _io
        body = b"ELF-not-really"
        resp = mock.MagicMock()
        resp.__enter__.return_value = resp
        resp.headers = {"Content-Length": str(len(body))}
        resp.read.side_effect = [body, b""]
        dest = os.path.join(self.tmp, "dl", "x.AppImage")
        got = []
        with mock.patch.object(updater.urllib.request, "urlopen", return_value=resp):
            self.assertEqual(updater.download("https://dl/a", dest, len(body), lambda p, m: got.append(p)), dest)
        self.assertEqual(open(dest, "rb").read(), body)
        self.assertFalse(os.path.exists(dest + ".part"))
        resp2 = mock.MagicMock(); resp2.__enter__.return_value = resp2; resp2.headers = {}
        resp2.read.side_effect = [b"short", b""]
        with mock.patch.object(updater.urllib.request, "urlopen", return_value=resp2):
            with self.assertRaises(updater.UpdateError):
                updater.download("https://dl/a", dest + "2", 99)
        self.assertFalse(os.path.exists(dest + "2.part"))
        # install, as if we were the AppImage at <tmp>/apps/ClaudiusMaximus-0.4.1-linux-x64.AppImage
        apps = os.path.join(self.tmp, "apps")
        os.makedirs(apps)
        current = os.path.join(apps, "ClaudiusMaximus-0.4.1-linux-x64.AppImage")
        open(current, "w").close()
        runs, pops = [], []
        with mock.patch.dict(os.environ, {"APPIMAGE": current}), \
                mock.patch.object(updater.platform, "machine", return_value="x86_64"), \
                mock.patch.object(hostos, "is_frozen", return_value=True), \
                mock.patch.object(hostos, "IS_LINUX", True), mock.patch.object(hostos, "IS_MAC", False), \
                mock.patch.object(hostos, "IS_WINDOWS", False), \
                mock.patch.object(updater.subprocess, "run", lambda cmd, **kw: runs.append(cmd)), \
                mock.patch.object(updater.subprocess, "Popen", lambda cmd, **kw: pops.append(cmd)), \
                mock.patch.object(autostart, "is_enabled", return_value=True), \
                mock.patch.object(autostart, "enable", lambda cmd=None: runs.append(("autostart", cmd))):
            msg = updater.install(dest, "9.9.0")
        new = os.path.join(apps, "ClaudiusMaximus-9.9.0-linux-x64.AppImage")
        self.assertTrue(os.path.exists(new))
        self.assertFalse(os.path.exists(dest))
        if POSIX:
            self.assertTrue(os.stat(new).st_mode & 0o111)
        self.assertEqual(runs, [[new, "hooks", "install"], ("autostart", [new, "app"])])
        self.assertEqual(pops, [[new, "app"]])
        self.assertIn("9.9.0 installed as " + new, msg)
        self.assertIn(current, msg)

    def test_update_cli(self):
        from companion import updater
        from companion.__main__ import main
        newer = updater.Release(version="9.9.0", tag="v9.9.0", url="https://x/rel")
        with mock.patch.object(updater, "check", return_value=newer), \
                mock.patch("sys.stdout", new_callable=io.StringIO) as out:
            self.assertEqual(main(["update"]), 3)
        self.assertIn("update available: 9.9.0", out.getvalue())
        self.assertIn("cannot self-install: this is a source checkout", out.getvalue())
        same = updater.Release(version=updater.current_version(), tag="v", url="")
        with mock.patch.object(updater, "check", return_value=same), \
                mock.patch("sys.stdout", new_callable=io.StringIO) as out:
            self.assertEqual(main(["update"]), 0)
        self.assertIn("up to date", out.getvalue())
        with mock.patch.object(updater, "check", side_effect=updater.UpdateError("offline")), \
                mock.patch("sys.stderr", new_callable=io.StringIO) as err:
            self.assertEqual(main(["update"]), 1)
        self.assertIn("offline", err.getvalue())
        with mock.patch("sys.stdout", new_callable=io.StringIO) as out:
            self.assertEqual(main(["update", "auto", "off"]), 0)
        self.assertFalse(updater.auto_enabled())
        with mock.patch("sys.stdout", new_callable=io.StringIO) as out:
            self.assertEqual(main(["update", "auto", "on"]), 0)
        self.assertTrue(updater.auto_enabled())
        with mock.patch("sys.stderr", new_callable=io.StringIO):
            self.assertEqual(main(["update", "auto", "maybe"]), 2)


class StatusCommandTest(TempHome):
    """#12: `status` reports the app's start-at-login entry; the headless service of a script install is only
    mentioned when it is actually installed, under a label that says what it is."""

    def status(self, *args):
        from companion.__main__ import main
        with mock.patch("sys.stdout", new_callable=io.StringIO) as out:
            rc = main(["status", *args])
        return rc, out.getvalue()

    def test_start_at_login_follows_autostart(self):
        with mock.patch.object(autostart, "is_enabled", return_value=False), \
                mock.patch("companion.hostos.service_installed", return_value=False), \
                mock.patch("companion.link.find_port", return_value=None):
            rc, out = self.status()
            self.assertEqual(rc, 1)   # no daemon running
            self.assertIn("start at login : no\n", out)
            self.assertNotIn("launch agent", out)
            self.assertNotIn("systemd unit", out)
            self.assertNotIn("headless", out)
        with mock.patch.object(autostart, "is_enabled", return_value=True), \
                mock.patch("companion.hostos.service_installed", return_value=False), \
                mock.patch("companion.link.find_port", return_value=None):
            rc, out = self.status()
            self.assertIn("start at login : yes\n", out)
            self.assertNotIn("headless", out)

    def test_headless_service_named_when_installed(self):
        with mock.patch.object(autostart, "is_enabled", return_value=False), \
                mock.patch("companion.hostos.service_installed", return_value=True), \
                mock.patch("companion.hostos.service_name", return_value="launch agent"), \
                mock.patch("companion.hostos.service_path", return_value="/x/com.claudius-maximus.daemon.plist"), \
                mock.patch("companion.link.find_port", return_value=None):
            rc, out = self.status()
            self.assertIn("start at login : no\n", out)
            self.assertIn("headless daemon: launch agent installed (/x/com.claudius-maximus.daemon.plist)\n", out)

    def test_json_keeps_old_keys_and_adds_autostart(self):
        with mock.patch.object(autostart, "is_enabled", return_value=True), \
                mock.patch("companion.hostos.service_installed", return_value=False), \
                mock.patch("companion.link.find_port", return_value=None):
            rc, out = self.status("--json")
        rep = json.loads(out)
        self.assertTrue(rep["autostart"])
        self.assertFalse(rep["launch_agent"])
        self.assertIn("service", rep)
        self.assertIn("service_path", rep)
        self.assertFalse(rep["daemon_running"])


class FlashCommandTest(TempHome):
    def test_flash_reports_unwritable_lock_dir_without_traceback(self):
        """#11: a lock file that cannot be created is a one-line `flash failed:` on stderr and exit 1."""
        from companion.__main__ import main
        blocker = os.path.join(self.tmp, "afile")
        with open(blocker, "w"):
            pass
        lock = daemon_mod.Path(blocker) / "sub" / ".devlock"
        flash = mock.Mock(side_effect=AssertionError("flash() must not run without the lock"))
        with mock.patch.object(daemon_mod, "DEVLOCK", lock), mock.patch("companion.flasher.flash", flash), \
                mock.patch("sys.stderr", new_callable=io.StringIO) as err:
            self.assertEqual(main(["flash"]), 1)
        self.assertIn("flash failed: cannot create the device lock", err.getvalue())
        self.assertIn(str(lock), err.getvalue())
        self.assertNotIn("Traceback", err.getvalue())

    def test_flash_runs_when_state_dir_missing(self):
        """#11 end to end at the CLI: fresh machine, no state dir yet, flash proceeds."""
        from companion.__main__ import main
        d = os.path.join(self.tmp, "fresh", ".claudius-maximus")
        lock = daemon_mod.Path(d) / ".devlock"
        res = types.SimpleNamespace(board="4b", fw_version="0.4.0", port="/dev/x", seconds=10.0, link_port="/dev/x")
        with mock.patch.object(daemon_mod, "DEVLOCK", lock), \
                mock.patch("companion.flasher.flash", return_value=res), \
                mock.patch("companion.flasher.detect", return_value=[]), \
                mock.patch("sys.stderr", new_callable=io.StringIO) as err:
            self.assertEqual(main(["flash"]), 0)
        self.assertTrue(os.path.isdir(d))
        self.assertIn("flashed 4b fw 0.4.0", err.getvalue())


if __name__ == "__main__":
    unittest.main()
