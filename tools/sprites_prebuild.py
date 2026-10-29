"""PlatformIO pre-build hook (firmware/platformio.ini `extra_scripts`): regenerates the room's sprite tables.

Runs `tools/sprites.py build` before every build, so `firmware/src/screens/room/sprites_gen.h/.cpp` always match
the sheets in `firmware/assets/sprites/` (the masters; the generated pair is gitignored). `build` rewrites the
files only when their content changes, so an unchanged tree costs ~0.2 s and recompiles nothing. An invalid sheet
fails the build with the converter's `error:` line.

It runs here, at script load, because the IDF builder globs `src/` when CMake configures, right after the pre
scripts: a sprites_gen.cpp generated later would miss the first build.
"""
import subprocess
import sys
from pathlib import Path

Import("env")  # noqa: F821 (SCons builtin)

ROOT = Path(env["PROJECT_DIR"]).resolve().parent  # noqa: F821
TOOL = ROOT / "tools" / "sprites.py"
OUT = ROOT / "firmware" / "src" / "screens" / "room"


def has_pillow(py: str) -> bool:
    # Always a real probe: a venv's python is a symlink to its base interpreter, so comparing resolved paths
    # (or trusting find_spec in this process) mistakes PlatformIO's penv for the Python that has Pillow.
    return subprocess.call([py, "-c", "import PIL"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0


def python_with_pillow() -> str:
    """The first Python with Pillow among: the one running PlatformIO, the one PlatformIO builds with
    ($PYTHONEXE: on CI the runner's, where the workflow installed Pillow), the repo venv's; else install Pillow
    into PlatformIO's and use that."""
    pio_py = env.subst("$PYTHONEXE") or sys.executable  # noqa: F821
    candidates = [sys.executable, pio_py, str(ROOT / "host" / ".venv" / "bin" / "python"),
                  str(ROOT / "host" / ".venv" / "Scripts" / "python.exe")]
    for py in dict.fromkeys(candidates):   # in order, without repeats
        if Path(py).exists() and has_pillow(py):
            return py
    print("sprites: Pillow missing from PlatformIO's Python, installing it (one time)")
    subprocess.call([sys.executable, "-m", "pip", "install", "--quiet", "pillow"])
    if has_pillow(sys.executable):
        return sys.executable
    sys.stderr.write(f"error: tools/sprites.py needs Pillow: run `{sys.executable} -m pip install pillow`\n")
    env.Exit(1)  # noqa: F821


rc = subprocess.call([python_with_pillow(), str(TOOL), "build", "--out", str(OUT)])
if rc:
    sys.stderr.write("error: sprite sheets invalid (tools/sprites.py build failed, see above)\n")
    env.Exit(rc)  # noqa: F821
