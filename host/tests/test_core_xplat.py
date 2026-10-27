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
        self.assertIn('"/repo/host/.venv/bin/python" -I -S "/repo/host/hooks/cm_hook.py"', cmds)
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


class DaemonApiTest(TempHome):
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
        a = build_parser().parse_args(["flash", "--board", "v4", "--erase"])
        self.assertEqual((a.board, a.erase, a.port), ("v4", True, None))
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


if __name__ == "__main__":
    unittest.main()
