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
hdiutil create -volname "$VOLNAME" -srcfolder "$STAGE" -fs HFS+ -format UDZO -imagekey zlib-level=9 -ov "$DMG"
hdiutil verify "$DMG"
echo "wrote $DMG"
