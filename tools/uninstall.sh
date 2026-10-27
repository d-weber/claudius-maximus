#!/bin/bash
# Claudius Maximus — reverse tools/install.sh exactly (macOS or Linux):
#  stop + remove the background service, remove our hooks / statusLine from ~/.claude/settings.json (restoring
#  a chained statusLine; timestamped backup first), delete ~/.claudius-maximus and our log files.
# Also removes the desktop app's start-at-login entry if present (best effort): ~/.config/autostart/
#  claudius-maximus.desktop (Linux), ~/Library/LaunchAgents/com.claudius-maximus.app.plist (macOS), and the
#  service plist the app set aside (*.plist.disabled-by-app). It does not quit a running app or delete the app
#  itself (AppImage, /Applications/Claudius Maximus.app); quit it from its menu first, or it recreates
#  ~/.claudius-maximus. The desktop app alone: see README "Uninstall".
# The repo and its host/.venv are left in place.   tools/uninstall.sh [--keep-logs]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL=com.claudius-maximus.daemon
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
UNIT="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/claudius-maximus.service"
APP_PLIST="$HOME/Library/LaunchAgents/com.claudius-maximus.app.plist"
APP_DESKTOP="${XDG_CONFIG_HOME:-$HOME/.config}/autostart/claudius-maximus.desktop"
OS="$(uname -s)"
KEEP_LOGS=0
[ "${1:-}" = "--keep-logs" ] && KEEP_LOGS=1

REAL_HOME="$(python3 -c 'import pwd,os;print(pwd.getpwuid(os.getuid()).pw_dir)' 2>/dev/null || echo "$HOME")"

if [ "$OS" = Darwin ]; then
  echo "==> LaunchAgent"
  if [ "$HOME" = "$REAL_HOME" ]; then
    launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null && echo "  stopped" || echo "  not loaded"
  else
    echo "  (test mode: HOME=$HOME, skipped launchctl)"
  fi
  rm -f "$PLIST" "$PLIST.disabled-by-app"
else
  echo "==> systemd user unit"
  if [ "$HOME" = "$REAL_HOME" ] && command -v systemctl >/dev/null; then
    systemctl --user disable --now claudius-maximus.service 2>/dev/null && echo "  stopped" || echo "  not loaded"
  else
    echo "  (test mode: HOME=$HOME, skipped systemctl)"
  fi
  rm -f "$UNIT"
  command -v systemctl >/dev/null && systemctl --user daemon-reload 2>/dev/null || true
fi

echo "==> Desktop app start-at-login entry"
APP_FOUND=0
for f in "$APP_PLIST" "$APP_DESKTOP"; do
  if [ -e "$f" ]; then
    rm -f "$f" && echo "  removed $f" || echo "  could not remove $f"
    APP_FOUND=1
  fi
done
[ "$APP_FOUND" = 1 ] || echo "  none"
# (not booted out: launchd only reads the app's plist at login, and booting it out would kill a running app)

echo "==> Claude Code settings"
PY="$ROOT/host/.venv/bin/python"
[ -x "$PY" ] || PY=/usr/bin/python3
"$PY" "$ROOT/host/hooks/cm_settings.py" uninstall

echo "==> State and logs"
rm -rf "${HOME:?}/.claudius-maximus"
if [ "$KEEP_LOGS" = 0 ]; then
  rm -f "${HOME:?}"/Library/Logs/claudius-maximus.log* "${HOME:?}"/Library/Logs/claudius-maximus.out.log
  rm -rf "${XDG_STATE_HOME:-${HOME:?}/.local/state}/claudius-maximus"
fi
echo "Done. (settings backups: ~/.claude/settings.json.bak-claudius-*)"
echo "If you also use the desktop app, quit it from its menu and delete it (AppImage / Claudius Maximus.app);"
echo "on its next start it would install the hooks and start at login again."
