"""Tests for packaging/ (release version guard, PyInstaller spec, icons, installer templates, frozen entry point).

Run:  host/.venv/bin/python -m unittest host/tests/test_packaging.py -v

The packaging scripts are loaded by file path: `packaging` is also the name of a common PyPI module, so a plain
import could pick up the wrong one.
"""
from __future__ import annotations

import importlib.util
import os
import sys
import tempfile
import tomllib
import unittest
from pathlib import Path
from unittest import mock

HOST = Path(__file__).resolve().parent.parent
ROOT = HOST.parent
PKG = ROOT / "packaging"
sys.path.insert(0, str(HOST))


def load(name: str):
    spec = importlib.util.spec_from_file_location(f"cm_packaging_{name}", PKG / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(PKG))   # subst.py imports check_version from its own directory
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.path.remove(str(PKG))
    return mod


check_version = load("check_version")


class VersionTest(unittest.TestCase):
    def test_app_version_matches_package(self):
        from companion.version import __version__
        self.assertEqual(check_version.app_version(), __version__)

    def test_tag_match(self):
        self.assertIsNone(check_version.check_tag("v1.2.3", "1.2.3"))
        self.assertIsNone(check_version.check_tag("refs/tags/v1.2.3", "1.2.3"))
        self.assertIsNone(check_version.check_tag(" v1.2.3\n", "1.2.3"))

    def test_tag_mismatch(self):
        for tag in ("v1.2.4", "1.2.3", "v1.2.3-rc1", "refs/heads/main", ""):
            err = check_version.check_tag(tag, "1.2.3")
            self.assertIsNotNone(err, tag)
            self.assertIn("1.2.3", err)

    def test_main_exit_codes(self):
        v = check_version.app_version()
        with mock.patch("sys.stdout"), mock.patch("sys.stderr"):
            self.assertEqual(check_version.main([]), 0)
            self.assertEqual(check_version.main([f"v{v}"]), 0)
            self.assertEqual(check_version.main(["v0.0.0-nope"]), 1)

    def test_parsers_on_files(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "version.py"
            p.write_text('"""doc"""\n__version__ = "9.8.7"\n')
            self.assertEqual(check_version.app_version(p), "9.8.7")
            h = Path(d) / "version.h"
            h.write_text('#pragma once\n// FW_VERSION in a comment "0.0.1"\n#define FW_VERSION "4.5.6"\n')
            self.assertEqual(check_version.fw_version(h), "4.5.6")
            p.write_text("VERSION = 1\n")
            with self.assertRaises(ValueError):
                check_version.app_version(p)

    @unittest.skipUnless(check_version.FW_VERSION_FILE.exists(), "firmware/src/core/version.h not there yet")
    def test_repo_fw_version(self):
        self.assertRegex(check_version.fw_version(), r"^\d+\.\d+\.\d+")


class PyprojectTest(unittest.TestCase):
    def test_pyproject(self):
        cfg = tomllib.loads((HOST / "pyproject.toml").read_text())
        st = cfg["tool"]["setuptools"]
        self.assertEqual(st["dynamic"]["version"]["attr"], "companion.version.__version__")
        self.assertIn("*/*.bin", st["package-data"]["companion.firmware"])
        self.assertIn("*/manifest.json", st["package-data"]["companion.firmware"])
        self.assertEqual(cfg["project"]["scripts"]["claudius-maximus"], "companion.__main__:main")
        # setuptools refuses files outside host/: the readme must not point at ../README.md
        self.assertNotIsInstance(cfg["project"].get("readme"), str)


class SpecTest(unittest.TestCase):
    def test_spec_compiles(self):
        src = (PKG / "claudius-maximus.spec").read_text()
        compile(src, "claudius-maximus.spec", "exec")
        for needle in ("entry.py", "companion.firmware", 'collect_data_files("esptool")', "pystray._win32",
                       "pystray._darwin", "dbus_fast", "LSUIElement", "com.claudius-maximus.app",
                       "Claudius Maximus.app", '"ClaudiusMaximus"', "tkinter"):
            self.assertIn(needle, src)

    def test_entry_compiles(self):
        compile((PKG / "entry.py").read_text(), "entry.py", "exec")

    def _run_entry(self, argv):
        entry = load("entry")
        seen = {}

        def fake_main():
            seen["argv"] = list(sys.argv)
            return 0

        fake = mock.MagicMock(main=fake_main)
        with mock.patch.object(sys, "argv", list(argv)), mock.patch.dict(sys.modules, {"companion.__main__": fake}):
            rc = entry.run()
        return rc, seen["argv"]

    def test_entry_defaults_to_app(self):
        self.assertEqual(self._run_entry(["ClaudiusMaximus"]), (0, ["ClaudiusMaximus", "app"]))
        self.assertEqual(self._run_entry(["x", "-psn_0_1234"])[1], ["x", "app"])

    def test_entry_passes_cli_through(self):
        self.assertEqual(self._run_entry(["x", "status", "--json"])[1], ["x", "status", "--json"])

    def _run_entry_hook(self, argv, hook_side_effect=None):
        """hook / statusline: straight to companion.hookclient, then os._exit(0) whatever happens."""
        entry = load("entry")
        from companion import hookclient

        class Exited(BaseException):
            pass

        calls = []

        def fake(*a):
            calls.append(a)
            if hook_side_effect:
                raise hook_side_effect

        def fake_exit(code):
            calls.append(("exit", code))
            raise Exited

        boom = mock.MagicMock(side_effect=AssertionError("companion.__main__ must not be imported"))
        with mock.patch.object(sys, "argv", list(argv)), mock.patch.object(hookclient, "run_hook", fake), \
                mock.patch.object(hookclient, "run_statusline", fake), \
                mock.patch.object(entry.os, "_exit", fake_exit), \
                mock.patch.dict(sys.modules, {"companion.__main__": mock.MagicMock(main=boom)}), \
                mock.patch.object(sys, "stdin", None):
            with self.assertRaises(Exited):
                entry.run()
            self.assertIsNone(sys.stdin)   # not replaced by os.devnull: hookclient falls back to GetStdHandle
        boom.assert_not_called()
        return calls

    def test_entry_hook_fast_path(self):
        self.assertEqual(self._run_entry_hook(["x", "hook", "Stop"]), [(["Stop"],), ("exit", 0)])
        self.assertEqual(self._run_entry_hook(["x", "statusline"]), [(), ("exit", 0)])
        self.assertEqual(self._run_entry_hook(["x", "hook"], RuntimeError("boom")), [([],), ("exit", 0)])


class IconsTest(unittest.TestCase):
    def _check(self, out: Path):
        from PIL import Image
        for name in ("app.png", "app.ico", "app.icns"):
            self.assertTrue((out / name).stat().st_size > 0, name)
        with Image.open(out / "app.png") as im:
            self.assertEqual(im.size, (1024, 1024))
        with Image.open(out / "app.ico") as im:
            ico_sizes = im.info["sizes"]
        self.assertIn((16, 16), ico_sizes)
        self.assertIn((256, 256), ico_sizes)
        self.assertEqual((out / "app.icns").read_bytes()[:4], b"icns")

    def test_make_icons(self):
        make_icons = load("make_icons")
        with tempfile.TemporaryDirectory() as d, mock.patch("sys.stdout"):
            files = make_icons.make_icons(Path(d))
            self.assertEqual([f.name for f in files], ["app.png", "app.ico", "app.icns"])
            self._check(Path(d))

    def test_placeholder_fallback(self):
        make_icons = load("make_icons")
        with mock.patch.dict(sys.modules, {"companion.tray.icon": None}):   # import -> ImportError
            fn, source = make_icons.renderer()
        self.assertEqual(source, "placeholder")
        with tempfile.TemporaryDirectory() as d, mock.patch("sys.stdout"), \
                mock.patch.object(make_icons, "renderer", return_value=(fn, source)):
            make_icons.make_icons(Path(d))
            self._check(Path(d))

    def test_committed_icons(self):
        for name in ("app.png", "app.ico", "app.icns"):
            self.assertTrue((PKG / "icons" / name).exists(), f"run packaging/make_icons.py ({name})")


class TemplatesTest(unittest.TestCase):
    def test_desktop_substitution(self):
        subst = load("subst")
        src = (PKG / "linux" / "claudius-maximus.desktop").read_text()
        self.assertIn("@VERSION@", src)
        out = subst.substitute(src, "1.2.3", "x64")
        self.assertNotIn("@", out)
        lines = out.splitlines()
        self.assertEqual(lines[0], "[Desktop Entry]")
        self.assertIn("X-AppImage-Version=1.2.3", lines)
        self.assertIn("Exec=claudius-maximus app", lines)
        self.assertIn("Icon=claudius-maximus", lines)

    def test_subst_cli(self):
        subst = load("subst")
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "in", Path(d) / "out"
            src.write_text("v=@VERSION@ a=@ARCH@\n")
            self.assertEqual(subst.main([str(src), str(dst), "--arch", "arm64"]), 0)
            self.assertEqual(dst.read_text(), f"v={check_version.app_version()} a=arm64\n")

    def test_iss_defines(self):
        """The .iss takes the version as /DAppVersion (release.yml) and names its output like the other assets."""
        iss = (PKG / "windows" / "installer.iss").read_text()
        for d in ("AppVersion", "Arch", "DistDir", "OutputDir"):
            self.assertIn(f"#ifndef {d}", iss)
        self.assertIn("OutputBaseFilename=ClaudiusMaximus-{#AppVersion}-windows-{#Arch}", iss)
        self.assertIn(r"DefaultDirName={localappdata}\Programs\ClaudiusMaximus", iss)
        self.assertIn("PrivilegesRequired=lowest", iss)
        self.assertIn('ValueName: "ClaudiusMaximus"', iss)
        self.assertIn('Parameters: "hooks uninstall"', iss)
        self.assertIn("CloseApplications=", iss)
        # Inno < 6.3 doesn't know x64compatible: chosen by compiler version
        self.assertIn("#if Ver >= EncodeVer(6, 3, 0)", iss)
        self.assertIn("ArchitecturesInstallIn64BitMode={#ArchAllowed}", iss)
        # the Run value is removed at uninstall even when autostart was enabled from the tray, not the task
        self.assertRegex(iss, r'ValueType: none; ValueName: "ClaudiusMaximus"; \\\n\s*Flags: uninsdeletevalue\n')
        self.assertIn('RunOnceId: "HooksUninstall"', iss)
        release = (ROOT / ".github" / "workflows" / "release.yml").read_text()
        self.assertIn("/DAppVersion=", release)
        self.assertIn("packaging/check_version.py", release)

    def test_scripts_are_executable(self):
        if os.name == "nt":
            self.skipTest("no exec bit on Windows")
        for p in (PKG / "linux" / "make_appimage.sh", PKG / "macos" / "make_dmg.sh"):
            self.assertTrue(os.access(p, os.X_OK), p)


class WorkflowTest(unittest.TestCase):
    """.github/workflows: parse, job graph, artifact names consistent between jobs, runner labels."""

    def load(self, name):
        try:
            import yaml
        except ImportError:
            self.skipTest("PyYAML not installed")
        return yaml.safe_load((ROOT / ".github" / "workflows" / name).read_text())

    @staticmethod
    def steps(job):
        return [st for st in job.get("steps", []) if isinstance(st, dict)]

    def test_release_graph_and_artifacts(self):
        wf = self.load("release.yml")
        on = wf.get("on", wf.get(True))            # YAML 1.1 reads a bare `on:` as True
        self.assertEqual(on["push"]["tags"], ["v*"])
        jobs = wf["jobs"]
        self.assertEqual(jobs["app"]["needs"], "firmware")
        self.assertEqual(sorted(jobs["release"]["needs"]), ["app", "firmware"])
        self.assertIn("refs/tags/v", jobs["release"]["if"])
        self.assertEqual(jobs["release"]["permissions"]["contents"], "write")

        def uploads(job):
            return [st["with"]["name"] for st in self.steps(job) if str(st.get("uses", "")).startswith(
                "actions/upload-artifact")]

        def downloads(job):
            return [st["with"] for st in self.steps(job) if str(st.get("uses", "")).startswith(
                "actions/download-artifact")]
        fw_up = uploads(jobs["firmware"])
        self.assertIn("firmware-bundle", fw_up)
        [dl] = downloads(jobs["app"])
        self.assertEqual(dl["name"], "firmware-bundle")
        # the spec bundles companion/firmware/** and fw_bundle.py writes there by default
        self.assertEqual(dl["path"], "host/companion/firmware")
        fw_bundle = (ROOT / "tools" / "fw_bundle.py").read_text()
        self.assertIn('default=ROOT / "host" / "companion" / "firmware"', fw_bundle)
        self.assertIn('collect_data_files("companion.firmware"', (PKG / "claudius-maximus.spec").read_text())
        app_up = uploads(jobs["app"])
        self.assertEqual(app_up, ["release-${{ matrix.os }}-${{ matrix.arch }}"])
        [rel] = downloads(jobs["release"])
        pattern = rel["pattern"].rstrip("*")
        self.assertTrue(rel.get("merge-multiple"))
        for name in ["release-firmware-zips", "release-windows-x64", "release-macos-arm64", "release-linux-x64"]:
            self.assertTrue(name.startswith(pattern), name)
        self.assertFalse("firmware-bundle".startswith(pattern), "raw bundle must not be attached twice")
        self.assertIn("release-firmware-zips", fw_up)

    def test_runner_labels(self):
        rel = self.load("release.yml")
        runners = [m["runner"] for m in rel["jobs"]["app"]["strategy"]["matrix"]["include"]]
        self.assertNotIn("macos-13", runners)          # retired by GitHub
        self.assertIn("macos-15-intel", runners)       # x64
        self.assertIn("macos-14", runners)             # arm64
        names = {(m["os"], m["arch"]) for m in rel["jobs"]["app"]["strategy"]["matrix"]["include"]}
        self.assertEqual(names, {("windows", "x64"), ("macos", "arm64"), ("macos", "x64"), ("linux", "x64")})

    def test_ci_smoke_shells(self):
        ci = self.load("ci.yml")
        host = ci["jobs"]["host"]
        self.assertEqual(host["defaults"]["run"]["shell"], "bash")   # the smoke test's case/esac is bash
        self.assertIn("windows-latest", host["strategy"]["matrix"]["os"])
        rel = self.load("release.yml")
        for st in self.steps(rel["jobs"]["app"]):
            if "ISCC" in str(st.get("run", "")):
                self.assertEqual(st.get("shell"), "pwsh")


if __name__ == "__main__":
    unittest.main()
