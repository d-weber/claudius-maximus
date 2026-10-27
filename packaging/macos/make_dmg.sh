#!/usr/bin/env bash
# Wrap "Claudius Maximus.app" into a drag-to-Applications .dmg (unsigned; see packaging/README.md).
#
#   packaging/macos/make_dmg.sh "dist/Claudius Maximus.app" out/ClaudiusMaximus-<version>-macos-<arch>.dmg
set -euo pipefail

APP="${1:?usage: make_dmg.sh APP_BUNDLE OUT.dmg}"
DMG="${2:?usage: make_dmg.sh APP_BUNDLE OUT.dmg}"
VOLNAME="Claudius Maximus"

[ -d "$APP/Contents/MacOS" ] || { echo "not an app bundle: $APP" >&2; exit 1; }
mkdir -p "$(dirname "$DMG")"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

# ditto keeps the bundle's symlinks, extended attributes and (ad-hoc) code signature intact.
ditto "$APP" "$STAGE/$(basename "$APP")"
ln -s /Applications "$STAGE/Applications"

rm -f "$DMG"
# hdiutil on CI runners fails now and then with "Resource busy" (Spotlight / XProtect scanning the image).
for attempt in 1 2 3 4; do
  if hdiutil create -volname "$VOLNAME" -srcfolder "$STAGE" -fs HFS+ -format UDZO -imagekey zlib-level=9 -ov "$DMG"; then
    break
  fi
  [ "$attempt" = 4 ] && { echo "hdiutil create failed" >&2; exit 1; }
  echo "hdiutil create failed (attempt $attempt), retrying" >&2
  sleep $((attempt * 5))
done
hdiutil verify "$DMG"
echo "wrote $DMG"
