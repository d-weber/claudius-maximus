"""Self-update from the GitHub Releases page.

The app checks https://api.github.com/repos/d-weber/claudius-maximus/releases/latest once a day (setting
`update_check`, default on; `claudius-maximus update auto off` or the menu turn it off), and on request. That is
the only network access the program makes: one unauthenticated GET of public release metadata, nothing about
the user goes out. A newer version shows up as an orange badge, a notification once per version, and an
"Update app to X…" menu entry; nothing is downloaded or installed without that click (or `update --install`).

Installing replaces the program the way each OS installs it:
  Linux   the running AppImage ($APPIMAGE): the new .AppImage is put next to it, made executable, the Claude
          Code hooks and the autostart entry are pointed at it (both contain the path), it is started and the
          old one quits. The old file is left in place.
  Windows the Inno Setup installer runs silently (/VERYSILENT /CLOSEAPPLICATIONS, keeping the "start at login"
          task as it is) and starts the new version; the running app quits first.
  macOS   the .dmg is attached, the .app copied over /Applications/Claudius Maximus.app (rename swap), the
          image detached, the app relaunched. LaunchAgent and hooks point at the bundle path: unchanged.
A source checkout or a .tar.gz install cannot self-install: the Releases page is opened instead.
"""
from __future__ import annotations

import json
import logging
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Callable

from . import hostos

log = logging.getLogger("companion.updater")

REPO = "d-weber/claudius-maximus"
RELEASES_URL = f"https://github.com/{REPO}/releases"
API_URL = f"https://api.github.com/repos/{REPO}/releases/latest"
CHECK_EVERY_S = 24 * 3600.0
RETRY_AFTER_ERROR_S = 3600.0
SETTING_AUTO = "update_check"            # bool, default True
SETTING_CHECKED_AT = "update_checked_at"  # unix time of the last check (also a failed one)
SETTING_NOTIFIED = "update_notified"      # version the user was already told about
TIMEOUT_S = 15.0
Progress = Callable[[int, str], None]


class UpdateError(Exception):
    """A sentence for the user."""


@dataclass
class Release:
    version: str                       # "0.5.0"
    tag: str                           # "v0.5.0"
    url: str                           # release page
    notes: str = ""
    published: str = ""
    assets: dict[str, tuple[str, int]] = field(default_factory=dict)   # name -> (download url, size)

    def asset(self, name: str | None) -> tuple[str, int] | None:
        return self.assets.get(name or "")


# -- versions and platform -------------------------------------------------------------------------------------
def current_version() -> str:
    from .version import __version__
    return __version__


def is_newer(candidate: str | None, current: str | None = None) -> bool:
    from .flasher import version_tuple
    a, b = version_tuple(candidate), version_tuple(current or current_version())
    return bool(a) and bool(b) and a > b


def _arch(machine: str | None = None) -> str:
    m = (machine or platform.machine() or "").lower()
    return "arm64" if m in ("arm64", "aarch64") else "x64"


def asset_name(version: str, system: str | None = None, machine: str | None = None,
               appimage: bool | None = None) -> str | None:
    """The release asset for this program, None when there is none we could install (source, .tar.gz)."""
    system = system or sys.platform
    if system == "win32":
        return f"ClaudiusMaximus-{version}-windows-x64.exe"
    if system == "darwin":
        return f"ClaudiusMaximus-{version}-macos-{_arch(machine)}.dmg"
    if system.startswith("linux"):
        appimage = bool(os.environ.get("APPIMAGE")) if appimage is None else appimage
        return f"ClaudiusMaximus-{version}-linux-{_arch(machine)}.AppImage" if appimage else None
    return None


def can_self_install() -> str:
    """"" when this program can replace itself, else why not (a sentence; the Releases page is the way then)."""
    if not hostos.is_frozen():
        return "this is a source checkout: update it with git pull"
    if hostos.IS_LINUX and not os.environ.get("APPIMAGE"):
        return "this copy was installed from the .tar.gz: download the new one from the Releases page"
    if hostos.IS_MAC:
        from .autostart import mac_unstable_location
        why = mac_unstable_location()
        if why:
            return f"move {hostos.APP_NAME} to the Applications folder first ({why})"
    return ""


# -- the check -------------------------------------------------------------------------------------------------
def _get(url: str, timeout: float) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": f"claudius-maximus/{current_version()}",
                                               "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:   # noqa: S310 (fixed https URL)
        return r.read()


def check(timeout: float = TIMEOUT_S, url: str | None = None) -> Release:
    """The latest release on GitHub. Raises UpdateError (offline, rate limited, odd answer)."""
    url = url or os.environ.get("CLAUDIUS_UPDATE_API") or API_URL
    try:
        data = json.loads(_get(url, timeout))
    except urllib.error.HTTPError as e:
        if e.code in (403, 429):
            raise UpdateError("GitHub is rate-limiting update checks from this address; try again later") from e
        raise UpdateError(f"the Releases page answered {e.code}") from e
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise UpdateError(f"cannot reach the Releases page ({getattr(e, 'reason', None) or e})") from e
    if not isinstance(data, dict) or not data.get("tag_name"):
        raise UpdateError("the Releases page gave an unexpected answer")
    tag = str(data["tag_name"])
    assets = {}
    for a in data.get("assets") or []:
        if isinstance(a, dict) and a.get("name") and a.get("browser_download_url"):
            assets[str(a["name"])] = (str(a["browser_download_url"]), int(a.get("size") or 0))
    rel = Release(version=tag.lstrip("vV"), tag=tag, url=str(data.get("html_url") or RELEASES_URL),
                  notes=str(data.get("body") or ""), published=str(data.get("published_at") or ""), assets=assets)
    log.info("latest release: %s (running %s)", rel.version, current_version())
    return rel


def auto_enabled() -> bool:
    return bool(hostos.settings_get(SETTING_AUTO, True))


def set_auto(on: bool) -> str:
    hostos.settings_set(SETTING_AUTO, bool(on))
    return "daily update check: " + ("on" if on else "off")


def auto_check_due(now: float | None = None) -> bool:
    if not auto_enabled():
        return False
    now = time.time() if now is None else now
    last = hostos.settings_get(SETTING_CHECKED_AT, 0) or 0
    return now - float(last) >= CHECK_EVERY_S


def mark_checked(ok: bool, now: float | None = None) -> None:
    now = time.time() if now is None else now
    # a failed check is retried after RETRY_AFTER_ERROR_S instead of a day
    hostos.settings_set(SETTING_CHECKED_AT, int(now if ok else now - CHECK_EVERY_S + RETRY_AFTER_ERROR_S))


# -- download and install -------------------------------------------------------------------------------------
def download(url: str, dest: str, size: int = 0, progress: Progress | None = None, timeout: float = TIMEOUT_S) -> str:
    """Stream `url` to `dest` (through dest.part). Raises UpdateError."""
    part = dest + ".part"
    req = urllib.request.Request(url, headers={"User-Agent": f"claudius-maximus/{current_version()}"})
    try:
        os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
        with urllib.request.urlopen(req, timeout=timeout) as r, open(part, "wb") as f:   # noqa: S310
            total = size or int(r.headers.get("Content-Length") or 0)
            got, last = 0, -1
            while True:
                chunk = r.read(256 * 1024)
                if not chunk:
                    break
                f.write(chunk)
                got += len(chunk)
                if progress and total:
                    pct = min(99, int(got * 100 / total))
                    if pct != last:
                        last = pct
                        progress(pct, f"Downloading {os.path.basename(dest)}")
        if size and os.path.getsize(part) != size:
            raise UpdateError(f"download incomplete ({os.path.getsize(part)} of {size} bytes)")
        os.replace(part, dest)
        return dest
    except (urllib.error.URLError, OSError, UpdateError) as e:
        try:
            os.unlink(part)
        except OSError:
            pass
        if isinstance(e, UpdateError):
            raise
        raise UpdateError(f"download failed: {getattr(e, 'reason', None) or e}") from e


def _popen_detached(cmd: list[str], **kw) -> None:
    """Start something that must outlive this process (the new app, the installer)."""
    kw.setdefault("stdin", subprocess.DEVNULL)
    kw.setdefault("stdout", subprocess.DEVNULL)
    kw.setdefault("stderr", subprocess.DEVNULL)
    if hostos.IS_WINDOWS:
        kw["creationflags"] = getattr(subprocess, "DETACHED_PROCESS", 0) | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
        kw.setdefault("close_fds", True)
    else:
        kw.setdefault("start_new_session", True)
    subprocess.Popen(cmd, **kw)   # noqa: S603


def _install_linux(path: str, version: str) -> str:
    current = os.environ.get("APPIMAGE")
    if not current:
        raise UpdateError("not running from an AppImage")
    folder = os.path.dirname(os.path.abspath(current))
    dest = os.path.join(folder, asset_name(version) or os.path.basename(path))
    try:
        if os.path.abspath(dest) == os.path.abspath(path):
            pass
        else:
            shutil.move(path, dest)
        os.chmod(dest, 0o755)
    except OSError as e:
        raise UpdateError(f"cannot put the new AppImage in {folder}: {e}") from e
    # the Claude Code hooks and the autostart entry carry the program's path: point them at the new file
    try:
        subprocess.run([dest, "hooks", "install"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=120)
    except (OSError, subprocess.SubprocessError) as e:
        log.warning("hooks install with the new version failed: %s", e)
    try:
        from . import autostart
        if autostart.is_enabled():
            autostart.enable(cmd=[dest, "app"])
    except Exception as e:
        log.warning("autostart entry not updated: %s", e)
    _popen_detached([dest, "app"])
    return (f"{hostos.APP_NAME} {version} installed as {dest} and starting; the previous version stays at "
            f"{current} (delete it when you like)")


def _install_windows(path: str, version: str) -> str:
    from . import autostart
    exe = sys.executable
    tasks = "autostart" if autostart.is_enabled() else "!autostart"
    cmd = (f'""{path}" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /MERGETASKS="{tasks}" '
           f'& start "" "{exe}" app"')
    _popen_detached(["cmd", "/c", cmd])
    return f"installing {hostos.APP_NAME} {version}; it starts again by itself"


def _install_macos(path: str, version: str) -> str:
    bundle = os.path.abspath(os.path.join(os.path.dirname(sys.executable), "..", ".."))
    if not bundle.endswith(".app"):
        raise UpdateError(f"not running from an app bundle ({sys.executable})")
    mount = tempfile.mkdtemp(prefix="cm-dmg-")
    staged, old = bundle + ".update", bundle + ".old"
    try:
        subprocess.run(["hdiutil", "attach", "-nobrowse", "-readonly", "-noverify", "-mountpoint", mount, path],
                       check=True, capture_output=True, timeout=120)
        src = next((os.path.join(mount, n) for n in os.listdir(mount) if n.endswith(".app")), None)
        if not src:
            raise UpdateError("no app bundle in the disk image")
        shutil.rmtree(staged, ignore_errors=True)
        subprocess.run(["ditto", src, staged], check=True, capture_output=True, timeout=300)
        shutil.rmtree(old, ignore_errors=True)
        os.rename(bundle, old)
        os.rename(staged, bundle)
        shutil.rmtree(old, ignore_errors=True)
        subprocess.run(["xattr", "-dr", "com.apple.quarantine", bundle], capture_output=True, timeout=60)
    except (OSError, subprocess.SubprocessError) as e:
        raise UpdateError(f"installing the new version failed: {e}") from e
    finally:
        subprocess.run(["hdiutil", "detach", mount, "-quiet"], capture_output=True, timeout=60)
        shutil.rmtree(mount, ignore_errors=True)
    _popen_detached(["/bin/sh", "-c", f'sleep 2; open -n "{bundle}" --args app'])
    return f"{hostos.APP_NAME} {version} installed in {os.path.dirname(bundle)}; it starts again in a moment"


def install(path: str, version: str) -> str:
    """Install the downloaded asset and start the new version. Returns a sentence; the caller then quits this
    process (the new one takes the daemon lock over once it is free). Raises UpdateError."""
    why = can_self_install()
    if why:
        raise UpdateError(why)
    if hostos.IS_WINDOWS:
        msg = _install_windows(path, version)
    elif hostos.IS_MAC:
        msg = _install_macos(path, version)
    else:
        msg = _install_linux(path, version)
    log.info("%s", msg)
    return msg


def download_dir() -> str:
    d = os.path.join(hostos.state_dir(), "updates")
    os.makedirs(d, mode=0o700, exist_ok=True)
    return d


def fetch(rel: Release, progress: Progress | None = None) -> str:
    """Download the asset for this platform into the state dir. Raises UpdateError."""
    name = asset_name(rel.version)
    a = rel.asset(name)
    if not name or not a:
        raise UpdateError(f"release {rel.version} has no download for this computer ({name or sys.platform}); "
                          f"see {rel.url}")
    url, size = a
    return download(url, os.path.join(download_dir(), name), size, progress)
