# -*- mode: python -*-
# PyInstaller spec for the Claudius Maximus desktop app (onedir on every OS, so `hook` starts without unpacking).
#
#   pyinstaller packaging/claudius-maximus.spec [--distpath DIR --workpath DIR]
#
# Output (dist/):
#   Windows  ClaudiusMaximus/ClaudiusMaximus.exe     windowed (no console), packaging/icons/app.ico
#   macOS    Claudius Maximus.app                    LSUIElement (menu bar only, no Dock icon), app.icns
#   Linux    claudius-maximus/claudius-maximus       wrapped into an AppImage + tar.gz by packaging/linux/
# Bundled firmware: whatever is in host/companion/firmware/<board>/ at build time (tools/fw_bundle.py / CI).
import re
import sys
from pathlib import Path

from PyInstaller.utils.hooks import collect_data_files, collect_submodules

SPEC_DIR = Path(SPECPATH).resolve()          # noqa: F821  (SPECPATH is injected by PyInstaller)
ROOT = SPEC_DIR.parent
HOST = ROOT / "host"
ICONS = SPEC_DIR / "icons"
VERSION = re.search(r'__version__\s*=\s*"([^"]+)"', (HOST / "companion" / "version.py").read_text()).group(1)

WIN = sys.platform == "win32"
MAC = sys.platform == "darwin"

sys.path.insert(0, str(HOST))  # so collect_* can import companion even without `pip install -e host`

hiddenimports = [
    *collect_submodules("companion"),          # subcommands, tray backends and providers are imported lazily
    *collect_submodules("esptool"),            # chip targets are loaded by name
    "PIL", "PIL.Image", "PIL.ImageDraw", "PIL.ImageFont",
    "serial", "serial.tools.list_ports",
]
if WIN:
    hiddenimports += ["pystray._win32", "serial.tools.list_ports_windows", "psutil._pswindows"]
elif MAC:
    hiddenimports += ["pystray._darwin", "serial.tools.list_ports_osx", "serial.tools.list_ports_posix",
                      "AppKit", "Foundation", "objc", "PyObjCTools", "psutil._psosx"]
else:
    hiddenimports += [*collect_submodules("dbus_fast"), "serial.tools.list_ports_linux",
                      "serial.tools.list_ports_posix", "psutil._pslinux"]

datas = [
    *collect_data_files("companion.firmware", includes=["**/*.bin", "**/*.json"]),
    *collect_data_files("esptool"),            # flasher stubs (targets/stub_flasher/*.json)
    # Claude Code hook scripts (stdlib only); used when the user has a python3 to run them with.
    *[(str(p), "hooks") for p in sorted((HOST / "hooks").glob("*.py"))],
]

excludes = ["tkinter", "_tkinter", "turtle", "turtledemo", "idlelib", "test", "tests", "lib2to3", "pydoc_data",
            "PIL.ImageTk", "PIL.ImageQt", "PyQt5", "PyQt6", "PySide2", "PySide6", "matplotlib", "numpy",
            "IPython", "pytest"]
if not WIN:
    excludes += ["pystray._win32"]
if not MAC:
    excludes += ["pystray._darwin", "AppKit", "Foundation", "objc"]

a = Analysis(  # noqa: F821
    [str(SPEC_DIR / "entry.py")],
    pathex=[str(HOST)],
    binaries=[],
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    runtime_hooks=[],
    excludes=excludes,
    noarchive=False,
)
pyz = PYZ(a.pure)  # noqa: F821

exe_name = "ClaudiusMaximus" if WIN else "claudius-maximus"
icon = None
if WIN and (ICONS / "app.ico").exists():
    icon = str(ICONS / "app.ico")
elif MAC and (ICONS / "app.icns").exists():
    icon = str(ICONS / "app.icns")

exe = EXE(  # noqa: F821
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name=exe_name,
    debug=False,
    strip=False,
    upx=False,                 # UPX slows startup and trips antivirus heuristics
    console=not WIN,           # Windows: no console window for the tray app (entry.py re-attaches for the CLI)
    disable_windowed_traceback=False,
    argv_emulation=False,      # macOS: we take no file-open events; keep argv as given (hooks pass args)
    target_arch=None,          # native arch of the build machine (CI builds arm64 and x64 separately)
    codesign_identity=None,
    entitlements_file=None,
    icon=icon,
)

coll = COLLECT(  # noqa: F821
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=False,
    name=exe_name,
)

if MAC:
    app = BUNDLE(  # noqa: F821
        coll,
        name="Claudius Maximus.app",
        icon=icon,
        bundle_identifier="com.claudius-maximus.app",
        version=VERSION,
        info_plist={
            "CFBundleName": "Claudius Maximus",
            "CFBundleDisplayName": "Claudius Maximus",
            "CFBundleShortVersionString": VERSION,
            "CFBundleVersion": VERSION,
            "LSUIElement": True,               # menu bar app: no Dock icon, no app menu
            "LSMinimumSystemVersion": "11.0",
            "NSHighResolutionCapable": True,
            "NSHumanReadableCopyright": "MIT License",
        },
    )
