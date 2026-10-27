#!/usr/bin/env bash
# Package the PyInstaller onedir build for Linux: a .tar.gz always, an .AppImage when appimagetool is on PATH.
#
#   packaging/linux/make_appimage.sh DIST_DIR OUT_DIR [VERSION] [ARCH]
#     DIST_DIR  the onedir build (dist/claudius-maximus, containing the claudius-maximus binary)
#     OUT_DIR   where ClaudiusMaximus-<version>-linux-<arch>.{tar.gz,AppImage} are written
#     VERSION   default: host/companion/version.py      ARCH  default: from uname -m (x64 | arm64)
#
# The release workflow installs appimagetool from the AppImage project's GitHub release and sets
# REQUIRE_APPIMAGE=1 so a missing tool fails the build instead of silently skipping the AppImage.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PY="${PYTHON:-python3}"

DIST="${1:?usage: make_appimage.sh DIST_DIR OUT_DIR [VERSION] [ARCH]}"
OUT="${2:?usage: make_appimage.sh DIST_DIR OUT_DIR [VERSION] [ARCH]}"
VERSION="${3:-$("$PY" "$ROOT/packaging/check_version.py")}"
case "${4:-$(uname -m)}" in
  x86_64|amd64|x64) ARCH=x64; AI_ARCH=x86_64 ;;
  aarch64|arm64)    ARCH=arm64; AI_ARCH=aarch64 ;;
  *) echo "unsupported arch ${4:-$(uname -m)}" >&2; exit 1 ;;
esac

[ -x "$DIST/claudius-maximus" ] || { echo "no claudius-maximus binary in $DIST" >&2; exit 1; }
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
BASE="ClaudiusMaximus-$VERSION-linux-$ARCH"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# --- tar.gz: the onedir as claudius-maximus/, plus the .desktop file and icon for a manual install ---------------
mkdir -p "$WORK/tar"
cp -a "$DIST" "$WORK/tar/claudius-maximus"
"$PY" "$ROOT/packaging/subst.py" "$HERE/claudius-maximus.desktop" "$WORK/tar/claudius-maximus/claudius-maximus.desktop" \
  --version "$VERSION" --arch "$ARCH"
cp "$ROOT/packaging/icons/app.png" "$WORK/tar/claudius-maximus/claudius-maximus.png"
tar -C "$WORK/tar" -czf "$OUT/$BASE.tar.gz" claudius-maximus
echo "wrote $OUT/$BASE.tar.gz"

# --- AppImage ---------------------------------------------------------------------------------------------------
if ! command -v appimagetool >/dev/null 2>&1; then
  if [ "${REQUIRE_APPIMAGE:-0}" = 1 ]; then
    echo "appimagetool not found on PATH" >&2; exit 1
  fi
  echo "appimagetool not on PATH: skipping the AppImage (CI builds it)"
  exit 0
fi

APPDIR="$WORK/ClaudiusMaximus.AppDir"
mkdir -p "$APPDIR/usr/lib" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/1024x1024/apps"
cp -a "$DIST" "$APPDIR/usr/lib/claudius-maximus"
"$PY" "$ROOT/packaging/subst.py" "$HERE/claudius-maximus.desktop" "$APPDIR/claudius-maximus.desktop" \
  --version "$VERSION" --arch "$ARCH"
cp "$APPDIR/claudius-maximus.desktop" "$APPDIR/usr/share/applications/"
cp "$ROOT/packaging/icons/app.png" "$APPDIR/claudius-maximus.png"
cp "$ROOT/packaging/icons/app.png" "$APPDIR/usr/share/icons/hicolor/1024x1024/apps/claudius-maximus.png"
ln -s claudius-maximus.png "$APPDIR/.DirIcon"
cat > "$APPDIR/AppRun" <<'APPRUN'
#!/bin/sh
# No arguments = the tray app (packaging/entry.py). $APPIMAGE is the path of the .AppImage file itself: the
# app uses it (not the temporary mount path) for the hook command and the autostart entry.
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/lib/claudius-maximus/claudius-maximus" "$@"
APPRUN
chmod +x "$APPDIR/AppRun"

ARCH="$AI_ARCH" appimagetool --no-appstream "$APPDIR" "$OUT/$BASE.AppImage"
chmod +x "$OUT/$BASE.AppImage"
echo "wrote $OUT/$BASE.AppImage"
