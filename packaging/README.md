# Packaging the Claudius Maximus desktop app

The desktop app is one PyInstaller **onedir** build of `host/companion` (entry point `packaging/entry.py`: no
arguments = the tray app, any argument = the CLI, e.g. `hook`, `status`, `flash`). It bundles the firmware images
found in `host/companion/firmware/<board>/` at build time, so the app can flash or update the board by itself.

| OS | Build output | Installer | Release asset |
|---|---|---|---|
| Windows | `dist/ClaudiusMaximus/ClaudiusMaximus.exe` (no console) | Inno Setup, per-user, no admin | `ClaudiusMaximus-<ver>-windows-x64.exe` |
| macOS | `dist/Claudius Maximus.app` (menu bar only, no Dock icon) | drag-to-Applications `.dmg` | `ClaudiusMaximus-<ver>-macos-arm64.dmg`, `-macos-x64.dmg` |
| Linux | `dist/claudius-maximus/claudius-maximus` | `.AppImage` + `.tar.gz` | `ClaudiusMaximus-<ver>-linux-x64.AppImage`, `.tar.gz` |

Plus the raw firmware bundles `firmware-<board>-<fwver>.zip` (bootloader, partitions, boot_app0, app image and
`manifest.json` with the offsets) for flashing with esptool or the web flasher by hand.

## Build locally

Python 3.11+ and a checkout of the repo. Commands are run from the repo root.

```bash
python3 -m venv host/.venv
host/.venv/bin/pip install -e "host[dev]"                     # the app + PyInstaller + PlatformIO
host/.venv/bin/python tools/fw_bundle.py --board all --out host/companion/firmware   # optional: bundle firmware
host/.venv/bin/python packaging/make_icons.py                  # icons from companion.tray.icon (committed)
host/.venv/bin/pyinstaller --noconfirm packaging/claudius-maximus.spec   # -> dist/ (build/ is scratch)
```

Without the `fw_bundle.py` step the app builds fine but ships no firmware (the tray then offers no flashing).
On Windows the venv is `host\.venv\Scripts\` instead of `host/.venv/bin/`.

Then, per OS:

- **Linux**: `packaging/linux/make_appimage.sh dist/claudius-maximus out` writes the `.tar.gz` (the onedir plus
  a `.desktop` file and icon) and, if `appimagetool` is on `PATH`, the `.AppImage`.
- **macOS**: `packaging/macos/make_dmg.sh "dist/Claudius Maximus.app" out/ClaudiusMaximus-0.3.0-macos-arm64.dmg`.
  PyInstaller builds for the architecture of the Python you run it with: use an arm64 Python for Apple silicon
  and an x86_64 one (or an Intel Mac) for the x64 build.
- **Windows**: install [Inno Setup 6](https://jrsoftware.org/isinfo.php), then
  `"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DAppVersion=0.3.0 packaging\windows\installer.iss`
  (output in `dist\installer\`). The installer goes to `%LOCALAPPDATA%\Programs\ClaudiusMaximus`, adds a "Start at
  login" task (HKCU `Run` value `ClaudiusMaximus`), starts the app when it finishes, and its uninstaller removes
  the Claude Code hooks (`ClaudiusMaximus.exe hooks uninstall`).

Quick check of any build: `<binary> status` must print the daemon/device report, and
`echo '{}' | <binary> hook` must exit 0 instantly.

## CI

- `.github/workflows/ci.yml` (every push and PR): unit tests on Ubuntu, macOS and Windows (Python 3.12), a
  PyInstaller smoke build on each (runs `status` and `hook` with the frozen binary), and both firmware builds
  (`pio run -e companion -e companion_v4`).
- `.github/workflows/release.yml` (tags `v*`, or run by hand from the Actions tab): builds the firmware bundle,
  the app for Windows x64, macOS arm64, macOS x64 and Linux x64 with that firmware inside, the installers, and
  publishes a GitHub Release with auto-generated notes. Run by hand on a branch, it only uploads the artifacts.

## Cut a release

1. Bump `host/companion/version.py` (`__version__`, the app version).
2. If the firmware changed since the last release, bump `FW_VERSION` in `firmware/src/core/version.h`: that is
   how installed apps know to offer a firmware update.
3. Commit, then tag and push:
   ```bash
   git tag v0.3.0 && git push origin main v0.3.0
   ```
   The tag must be exactly `v` + `__version__`; the release workflow checks it (`packaging/check_version.py`)
   and fails otherwise.
4. Wait for the `release` workflow; the release appears on the repo's Releases page with the installers and the
   firmware zips attached. Edit the notes if needed.

## Unsigned builds

The builds are not code-signed or notarized, so the OSes warn the first time:

- **macOS** (Gatekeeper: "cannot be opened because the developer cannot be verified" or "is damaged"): drag the
  app to Applications, then **right-click (or Control-click) it → Open → Open**. On macOS 15+, if there is no
  Open button: try to open it once, then System Settings → Privacy & Security → **Open Anyway**. From a terminal:
  `xattr -dr com.apple.quarantine "/Applications/Claudius Maximus.app"`.
- **Windows** (SmartScreen: "Windows protected your PC"): click **More info → Run anyway**. Some antivirus tools
  flag unsigned PyInstaller apps; the source and the build logs are public in the repo's Actions tab.
- **Linux**: `chmod +x ClaudiusMaximus-*.AppImage` and run it. The AppImage needs FUSE 2 (`libfuse2` /
  `fuse2` package); without it, run it with `--appimage-extract-and-run`, or use the `.tar.gz` instead.
