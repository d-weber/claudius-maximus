"""Windows port of the providers and the Focus action, tested on any OS with mocks.

The Windows branches are selected by each module's IS_WINDOWS flag (patched here); psutil and user32 are fakes.
Run:  host/.venv/bin/python -m unittest host/tests/test_windows_port.py -v
"""
from __future__ import annotations

import ctypes
import json
import os
import shutil
import sys
import tempfile
import time
import types
import unittest
from collections import namedtuple
from datetime import datetime, timezone
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from companion import runtime_focus, sessions_text  # noqa: E402
from companion.providers import sessions as sessions_mod  # noqa: E402
from companion.providers import system as system_mod  # noqa: E402
from companion.providers import usage as usage_mod  # noqa: E402

WHOME = "C:\\Users\\test"
WCWD = "C:\\Users\\test\\projects\\demo-proj"
SID = "abcdef01-2222-3333-4444-555555555555"


def iso(ts: float) -> str:
    return datetime.fromtimestamp(ts, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def windows_text():
    """Patch sessions_text as if it ran on Windows with HOME = C:\\Users\\test."""
    return mock.patch.multiple(sessions_text, IS_WINDOWS=True, HOME=WHOME)


# ============================================================================== text / paths
class WindowsPathTests(unittest.TestCase):
    def test_cwd_slug_matches_claude_code(self):
        self.assertEqual(sessions_mod.cwd_slug(WCWD), "C--Users-test-projects-demo-proj")
        self.assertEqual(sessions_mod.cwd_slug("D:\\work\\my.app"), "D--work-my-app")

    def test_folder_name(self):
        with windows_text():
            self.assertEqual(sessions_text.folder_name(WCWD), "demo-proj")
            self.assertEqual(sessions_text.folder_name(WCWD + "\\"), "demo-proj")
            self.assertEqual(sessions_text.folder_name("C:/Users/test/proj/"), "proj")
            self.assertEqual(sessions_text.folder_name(""), "")
        # POSIX unchanged
        self.assertEqual(sessions_text.folder_name("/Users/x/proj/"), "proj")

    def test_tilde(self):
        with windows_text():
            self.assertEqual(sessions_text.tilde(WCWD), "~\\projects\\demo-proj")
            self.assertEqual(sessions_text.tilde("c:\\users\\TEST\\a"), "~\\a")       # case-insensitive
            self.assertEqual(sessions_text.tilde(WHOME), "~")
            self.assertEqual(sessions_text.tilde("C:\\Users\\tester\\a"), "C:\\Users\\tester\\a")
            self.assertEqual(sessions_text.tilde("D:\\x"), "D:\\x")
            self.assertEqual(sessions_text.tilde(""), "")

    def test_short_path_and_tool_detail(self):
        with windows_text():
            self.assertEqual(sessions_text.short_path(WCWD + "\\src\\main.py", WCWD), "src\\main.py")
            self.assertEqual(sessions_text.short_path("C:/Users/test/projects/demo-proj/src/a.py", WCWD), "src/a.py")
            self.assertEqual(sessions_text.short_path("C:\\Users\\test\\other\\x.txt", WCWD), "~\\other\\x.txt")
            deep = "D:\\" + "\\".join(["very-long-directory-name"] * 4) + "\\file.py"
            self.assertEqual(sessions_text.short_path(deep, WCWD), "very-long-directory-name\\file.py")
            self.assertEqual(sessions_text.tool_detail("Read", {"file_path": WCWD + "\\src\\main.py"}, WCWD),
                             "src\\main.py")
            self.assertEqual(sessions_text.tool_detail("Artifact", {"file_path": WCWD + "\\out\\page.html"}),
                             "publish page.html")


# ============================================================================== pid liveness
class PidAliveTests(unittest.TestCase):
    def test_windows_uses_psutil_never_os_kill(self):
        fake = types.SimpleNamespace(pid_exists=mock.Mock(side_effect=lambda p: p == 1234))
        with mock.patch.object(sessions_mod, "IS_WINDOWS", True), \
                mock.patch.dict(sys.modules, {"psutil": fake}), \
                mock.patch("os.kill", side_effect=AssertionError("os.kill terminates processes on Windows")):
            self.assertTrue(sessions_mod.pid_alive(1234))
            self.assertTrue(sessions_mod.pid_alive("1234"))
            self.assertFalse(sessions_mod.pid_alive(999))
            self.assertFalse(sessions_mod.pid_alive(0))
            self.assertFalse(sessions_mod.pid_alive(None))

    def test_posix_unchanged(self):
        self.assertTrue(sessions_mod.pid_alive(os.getpid()))
        self.assertFalse(sessions_mod.pid_alive(-1))


# ============================================================================== sessions provider
class WindowsTreeTests(unittest.TestCase):
    """A %USERPROFILE%\\.claude tree as Claude Code writes it on Windows (Windows cwd, UTF-8, CRLF-free JSONL)."""

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="cm-win-claude-")
        os.makedirs(os.path.join(self.root, "sessions"))
        self.pdir = os.path.join(self.root, "projects", "C--Users-test-projects-demo-proj")
        os.makedirs(self.pdir)
        self.pid = 4321
        now = time.time()
        reg = {"pid": self.pid, "sessionId": SID, "cwd": WCWD, "startedAt": int((now - 60) * 1000),
               "kind": "interactive", "entrypoint": "cli", "name": "démo", "nameSource": "derived",
               "status": "busy", "updatedAt": int(now * 1000), "statusUpdatedAt": int(now * 1000)}
        with open(os.path.join(self.root, "sessions", f"{self.pid}.json"), "w", encoding="utf-8") as f:
            json.dump(reg, f, ensure_ascii=False)
        recs = [
            {"type": "user", "isSidechain": False, "promptSource": "typed", "origin": {"kind": "human"},
             "message": {"role": "user", "content": "Répare le café — vite"}, "timestamp": iso(now - 5),
             "cwd": WCWD, "sessionId": SID, "gitBranch": "main"},
            {"type": "assistant", "isSidechain": False, "timestamp": iso(now - 4), "cwd": WCWD, "sessionId": SID,
             "message": {"model": "claude-opus-5-5", "id": "m1", "type": "message", "role": "assistant",
                         "stop_reason": "tool_use",
                         "content": [{"type": "tool_use", "id": "t1", "name": "Read",
                                      "input": {"file_path": WCWD + "\\src\\café.py"}}],
                         "usage": {"input_tokens": 10, "output_tokens": 100, "cache_read_input_tokens": 0,
                                   "cache_creation_input_tokens": 0}}},
        ]
        with open(os.path.join(self.pdir, SID + ".jsonl"), "w", encoding="utf-8", newline="\n") as f:
            for r in recs:
                f.write(json.dumps(r, ensure_ascii=False) + "\n")

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def test_provider_parses_windows_tree(self):
        fake = types.SimpleNamespace(pid_exists=lambda p: p == self.pid)
        with windows_text(), mock.patch.object(sessions_mod, "IS_WINDOWS", True), \
                mock.patch.dict(sys.modules, {"psutil": fake}), \
                mock.patch("os.kill", side_effect=AssertionError("no os.kill on Windows")):
            p = sessions_mod.SessionsProvider(base=self.root)
            for _ in range(2):
                p.poll(time.time())
            st = {}
            p.contribute(st)
        self.assertEqual(len(st["sessions"]), 1)
        s = st["sessions"][0]
        self.assertEqual(s["id"], SID[:8])
        self.assertEqual(s["name"], "demo-proj")
        self.assertEqual(s["project"], "demo-proj")
        self.assertEqual(s["cwd"], "~\\projects\\demo-proj")
        self.assertEqual(s["status"], "busy")
        self.assertEqual(s["last_prompt"], "Repare le cafe - vite")
        self.assertEqual(s["activity"]["tool"], "Read")
        self.assertEqual(s["activity"]["detail"], "src\\cafe.py")
        self.assertEqual(s["model"], "opus-5.5")

    def test_dead_pid_is_not_live(self):
        fake = types.SimpleNamespace(pid_exists=lambda p: False)
        with windows_text(), mock.patch.object(sessions_mod, "IS_WINDOWS", True), \
                mock.patch.dict(sys.modules, {"psutil": fake}):
            p = sessions_mod.SessionsProvider(base=self.root)
            p.poll(time.time())
            st = {}
            p.contribute(st)
        self.assertEqual([x for x in st["sessions"] if x["status"] != "offline"], [])

    def test_claude_config_dir(self):
        with mock.patch.dict(os.environ, {"CLAUDE_CONFIG_DIR": self.root}):
            self.assertEqual(sessions_mod.claude_dir(), self.root)
            self.assertEqual(runtime_focus._claude_dir(), self.root)
            rec = runtime_focus.find_session(SID[:8])
        self.assertEqual(rec["cwd"], WCWD)
        self.assertEqual(rec["name"], "démo")   # read as UTF-8 whatever the locale


class UsageEncodingTests(unittest.TestCase):
    def test_rate_limits_read_as_utf8(self):
        d = tempfile.mkdtemp(prefix="cm-win-usage-")
        try:
            p = os.path.join(d, "statusline.json")
            with open(p, "w", encoding="utf-8") as f:
                json.dump({"note": "é", "rate_limits": {}}, f, ensure_ascii=False)
            real_open = open
            seen = []

            def spy(path, *a, **kw):
                if path == p:
                    seen.append(kw.get("encoding"))
                return real_open(path, *a, **kw)
            with mock.patch.object(usage_mod, "RATE_LIMIT_FILES", [p]), mock.patch("builtins.open", spy):
                usage_mod.UsageProvider()._rate_limits(int(time.time()))
            self.assertEqual(seen, ["utf-8"])
        finally:
            shutil.rmtree(d, ignore_errors=True)


# ============================================================================== system provider
_VM = namedtuple("vm", "total available percent used free")
_SW = namedtuple("sw", "total used free percent")
_NIC = namedtuple("nic", "bytes_sent bytes_recv")
_DU = namedtuple("du", "total used free percent")
_BATT = namedtuple("batt", "percent secsleft power_plugged")
_MI = namedtuple("mi", "rss vms")


class _FakeProc:
    def __init__(self, pid, ppid, name, cpu, rss, cmd=()):
        self.pid, self._cmd = pid, list(cmd)
        self.info = {"pid": pid, "ppid": ppid, "name": name, "memory_info": _MI(rss, rss), "cpu_percent": cpu}

    def cmdline(self):
        return self._cmd


def fake_system_psutil():
    procs = [
        _FakeProc(0, 0, "System Idle Process", 750.0, 8 << 10),
        _FakeProc(4, 0, "System", 0.5, 1 << 20),
        _FakeProc(100, 4, "explorer.exe", 1.0, 120 << 20),
        _FakeProc(200, 100, "WindowsTerminal.exe", 2.0, 90 << 20),
        _FakeProc(300, 200, "pwsh.exe", 0.1, 60 << 20),
        _FakeProc(400, 300, "claude.exe", 12.5, 400 << 20),
        _FakeProc(410, 400, "node.exe", 30.0, 200 << 20, ["node", "C:\\x\\mcp-server.js"]),
        _FakeProc(500, 300, "node.exe", 5.0, 300 << 20,
                  ["C:\\Program Files\\nodejs\\node.exe",
                   "C:\\Users\\test\\AppData\\Roaming\\npm\\node_modules\\@anthropic-ai\\claude-code\\cli.js"]),
        _FakeProc(600, 100, "chrome.exe", 40.0, 800 << 20),
    ]
    by_pid = {p.pid: p for p in procs}
    m = types.SimpleNamespace()
    m.cpu_percent = mock.Mock(return_value=[10.0, 30.0, 50.0, 70.0])
    m.net_io_counters = mock.Mock(return_value={
        "Ethernet": _NIC(1000, 2000), "Loopback Pseudo-Interface 1": _NIC(10**9, 10**9),
        "vEthernet (WSL)": _NIC(10**9, 10**9)})
    m.virtual_memory = mock.Mock(return_value=_VM(16 << 30, 6 << 30, 62.5, 10 << 30, 6 << 30))
    m.swap_memory = mock.Mock(return_value=_SW(4 << 30, 1 << 30, 3 << 30, 25.0))
    m.getloadavg = mock.Mock(return_value=(1.25, 0.5, 0.0))
    m.disk_usage = mock.Mock(return_value=_DU(500 * 10**9, 200 * 10**9, 300 * 10**9, 40.0))
    m.boot_time = mock.Mock(return_value=time.time() - 3600)
    m.sensors_battery = mock.Mock(return_value=_BATT(87.4, -2, True))   # POWER_TIME_UNLIMITED
    m.process_iter = mock.Mock(side_effect=lambda attrs=None: iter(procs))
    m.Process = lambda pid: by_pid[pid]
    return m


class SystemWindowsTests(unittest.TestCase):
    def test_sample_without_subprocess(self):
        fake = fake_system_psutil()
        with mock.patch.multiple(system_mod, IS_WINDOWS=True, IS_MAC=False, IS_LINUX=False, psutil=fake), \
                mock.patch.object(system_mod.subprocess, "run", side_effect=AssertionError("spawned a process")), \
                mock.patch("os.getloadavg", side_effect=AssertionError("no os.getloadavg on Windows"), create=True), \
                mock.patch("socket.gethostname", return_value="Damien-PC.local"), \
                mock.patch.dict(os.environ, {"SystemDrive": "D:"}):
            sp = system_mod.SystemProvider()
            sp.poll(time.time())
            d = sp.data
        self.assertEqual(d["host"], "Damien-PC")
        self.assertEqual(d["cores"], [10, 30, 50, 70])
        self.assertEqual(d["cpu"], 40)
        self.assertEqual(d["kinds"], "")
        self.assertEqual(d["load"], [1.2, 0.5, 0.0])
        self.assertEqual(d["mem"]["total"], 16384)
        self.assertEqual(d["mem"]["used"], 10240)
        self.assertEqual(d["mem"]["swap"], 1024)
        self.assertEqual(d["mem"]["pressure"], 62)
        self.assertEqual(d["mem"]["level"], "normal")
        fake.disk_usage.assert_called_with("D:\\")
        self.assertEqual(d["disk"], {"used": 200, "total": 500, "pct": 40})
        self.assertEqual(d["batt"], {"pct": 87, "plug": 1, "mins": -1})
        self.assertNotIn("thermal", d)
        names = [p["n"] for p in d["procs"]]
        self.assertNotIn("System Idle Process", names)
        self.assertEqual(names[:3], ["chrome", "node", "claude"])
        cl = {p["n"]: p.get("cl") for p in d["procs"]}
        self.assertEqual(cl["claude"], 1)
        self.assertIsNone(cl["chrome"])
        # claude.exe + npm-installed claude (node cli.js) are two sessions; the MCP node is a descendant
        self.assertEqual(d["claude"], {"n": 2, "cpu": 47.5, "mem": 900, "kids": 1})

    def test_net_skips_virtual_adapters(self):
        with mock.patch.object(system_mod, "IS_WINDOWS", True):
            self.assertTrue(system_mod._skip_nic("Loopback Pseudo-Interface 1"))
            self.assertTrue(system_mod._skip_nic("vEthernet (Default Switch)"))
            self.assertTrue(system_mod._skip_nic("Tailscale"))
            self.assertFalse(system_mod._skip_nic("Ethernet"))
            self.assertFalse(system_mod._skip_nic("Wi-Fi"))
        self.assertTrue(system_mod._skip_nic("lo"))
        self.assertFalse(system_mod._skip_nic("enp3s0"))

    def test_no_psutil_degrades(self):
        with mock.patch.multiple(system_mod, IS_WINDOWS=True, IS_MAC=False, IS_LINUX=False, psutil=None), \
                mock.patch.object(system_mod.subprocess, "run", side_effect=AssertionError("spawned a process")), \
                self.assertLogs("companion.system", "WARNING"):
            sp = system_mod.SystemProvider()
            sp.poll(time.time())
            st = {}
            sp.contribute(st)
        self.assertEqual(st, {})


# ============================================================================== focus
class FakeUser32:
    """Just enough of user32 for runtime_focus: windows = [(hwnd, owner pid, title, iconic)] in z-order."""

    def __init__(self, windows, fg_ok=True):
        self.windows = windows
        self.fg_ok = list(fg_ok) if isinstance(fg_ok, (list, tuple)) else None
        self.default_ok = fg_ok if not isinstance(fg_ok, (list, tuple)) else True
        self.calls = []

    def _w(self, hwnd):
        return next(w for w in self.windows if w[0] == hwnd)

    def EnumWindows(self, cb, lparam):
        for w in self.windows:
            if not cb(w[0], lparam):
                break
        return 1

    def IsWindowVisible(self, hwnd):
        return 1

    def GetWindowTextLengthW(self, hwnd):
        return len(self._w(hwnd)[2])

    def GetWindowTextW(self, hwnd, buf, n):
        buf.value = self._w(hwnd)[2][: n - 1]
        return len(buf.value)

    def GetWindowThreadProcessId(self, hwnd, ptr):
        w = next((w for w in self.windows if w[0] == hwnd), None)
        ptr.contents.value = w[1] if w else 0
        return 7000 + (hwnd or 0)

    def IsIconic(self, hwnd):
        return int(self._w(hwnd)[3])

    def ShowWindow(self, hwnd, cmd):
        self.calls.append(("ShowWindow", hwnd, cmd))
        return 1

    def SetForegroundWindow(self, hwnd):
        self.calls.append(("SetForegroundWindow", hwnd))
        if self.fg_ok is not None:
            return int(self.fg_ok.pop(0)) if self.fg_ok else 1
        return int(self.default_ok)

    def __getattr__(self, name):   # AllowSetForegroundWindow, keybd_event, BringWindowToTop, AttachThreadInput...
        def f(*a):
            self.calls.append((name,) + a)
            return 1
        return f

    def raised(self):
        return [c[1] for c in self.calls if c[0] == "SetForegroundWindow"]


class _FProc:
    def __init__(self, table, pid):
        if pid not in table:
            raise ProcessLookupError(pid)
        self.table, self.pid = table, pid

    def name(self):
        return self.table[self.pid][1]

    def parents(self):
        out, p = [], self.table[self.pid][0]
        while p in self.table:
            out.append(_FProc(self.table, p))
            p = self.table[p][0]
        return out


def fake_focus_psutil(table):
    """table: pid -> (ppid, exe name)."""
    return types.SimpleNamespace(Process=lambda pid: _FProc(table, pid), pid_exists=lambda pid: pid in table)


class FocusWindowsTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="cm-win-focus-")
        os.makedirs(os.path.join(self.root, "sessions"))
        with open(os.path.join(self.root, "sessions", "400.json"), "w", encoding="utf-8") as f:
            json.dump({"pid": 400, "sessionId": SID, "cwd": WCWD, "updatedAt": 1}, f)

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def focus(self, table, u32, sid=SID[:8]):
        with mock.patch.object(runtime_focus, "IS_WINDOWS", True), \
                mock.patch.object(runtime_focus, "_user32", return_value=u32), \
                mock.patch.object(runtime_focus, "_win_thread_id", return_value=1), \
                mock.patch.object(runtime_focus, "_ps", side_effect=AssertionError("no ps on Windows")), \
                mock.patch.object(runtime_focus.subprocess, "run", side_effect=AssertionError("spawned")), \
                mock.patch.dict(sys.modules, {"psutil": fake_focus_psutil(table)}), \
                mock.patch.dict(os.environ, {"CLAUDE_CONFIG_DIR": self.root}):
            return runtime_focus.focus_session(sid)

    def test_windows_terminal_fallback(self):
        # WindowsTerminal.exe is not an ancestor of the shell (default-terminal handoff); title is Claude's
        table = {400: (300, "claude.exe"), 300: (100, "pwsh.exe"), 100: (4, "explorer.exe"),
                 900: (100, "WindowsTerminal.exe"), 500: (100, "notepad.exe")}
        u = FakeUser32([(11, 500, "notes.txt - Notepad", False), (22, 900, "\u2733 Claude Code", False),
                        (33, 900, "other WT window", False)])
        r = self.focus(table, u)
        self.assertEqual(u.raised(), [22])
        self.assertIn("focused windowsterminal", r)
        self.assertIn(SID[:8], r)
        self.assertIn(("ShowWindow", 22, 5), u.calls)               # not minimized: no SW_RESTORE
        self.assertIn(("keybd_event", 0x12, 0, 0, 0), u.calls)       # Alt press to unlock the foreground
        self.assertIn(("keybd_event", 0x12, 0, 2, 0), u.calls)

    def test_ancestor_pid_match_git_bash(self):
        table = {400: (300, "claude.exe"), 300: (200, "bash.exe"), 200: (100, "mintty.exe"),
                 100: (4, "explorer.exe"), 900: (100, "WindowsTerminal.exe")}
        u = FakeUser32([(22, 900, "WT", False), (44, 200, "MINGW64:/c/Users/test", True)])
        r = self.focus(table, u)
        self.assertEqual(u.raised(), [44])
        self.assertIn(("ShowWindow", 44, 9), u.calls)               # minimized: SW_RESTORE
        self.assertIn("window of mintty", r)

    def test_title_match_beats_wt_fallback(self):
        table = {400: (300, "claude.exe"), 300: (100, "pwsh.exe"), 100: (4, "explorer.exe"),
                 900: (100, "WindowsTerminal.exe")}
        u = FakeUser32([(22, 900, "pwsh in other", False), (23, 900, "demo-proj - pwsh", False)])
        r = self.focus(table, u)
        self.assertEqual(u.raised(), [23])
        self.assertIn("'demo-proj'", r)

    def test_vscode_title_match_before_pid(self):
        table = {400: (300, "claude.exe"), 300: (70, "pwsh.exe"), 70: (60, "Code.exe"), 60: (4, "Code.exe")}
        u = FakeUser32([(5, 60, "main.py - other - Visual Studio Code", False),
                        (6, 60, "app.py - Demo-Proj - Visual Studio Code", False)])
        r = self.focus(table, u)
        self.assertEqual(u.raised(), [6])
        self.assertIn("focused code", r)
        self.assertIn("exact", r)

    def test_foreground_lock_attach_thread_input(self):
        table = {400: (300, "claude.exe"), 300: (200, "cmd.exe"), 200: (4, "explorer.exe")}
        u = FakeUser32([(77, 300, "C:\\Windows\\system32\\cmd.exe - claude", False)], fg_ok=[False, True])
        r = self.focus(table, u)
        self.assertEqual(u.raised(), [77, 77])
        names = [c[0] for c in u.calls]
        self.assertIn("AttachThreadInput", names)
        attach = [c for c in u.calls if c[0] == "AttachThreadInput"]
        self.assertEqual([c[3] for c in attach], [1, 0])              # attached, then detached
        self.assertIn("exact", r)

    def test_refused_is_reported(self):
        table = {400: (300, "claude.exe"), 300: (4, "cmd.exe")}
        u = FakeUser32([(77, 300, "cmd", False)], fg_ok=False)
        r = self.focus(table, u)
        self.assertIn("foreground refused", r)

    def test_not_found_and_gone(self):
        u = FakeUser32([])
        self.assertEqual(self.focus({}, u, sid="ffffffff"), "session ffffffff not found")
        self.assertIn("gone", self.focus({}, u))
        r = self.focus({400: (4, "claude.exe")}, FakeUser32([(1, 999, "x", False)]))
        self.assertIn("no window found", r)

    def test_real_enum_callback_type(self):
        """_win_windows builds a ctypes callback (WINFUNCTYPE on Windows, CFUNCTYPE elsewhere)."""
        u = FakeUser32([(1, 10, "a", False), (2, 20, "", False), (3, 30, "c", False)])
        self.assertEqual(runtime_focus._win_windows(u), [(1, 10, "a"), (3, 30, "c")])
        self.assertTrue(callable(getattr(ctypes, "WINFUNCTYPE", ctypes.CFUNCTYPE)))


if __name__ == "__main__":
    unittest.main()
