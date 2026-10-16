#!/bin/bash
# Claudius Maximus — reverse tools/install.sh exactly (macOS or Linux):
#  stop + remove the background service, remove our hooks / statusLine from ~/.claude/settings.json (restoring
#  a chained statusLine; timestamped backup first), delete ~/.claudius-maximus and our log files.
# The repo and its host/.venv are left in place.   tools/uninstall.sh [--keep-logs]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL=com.claudius-maximus.daemon
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
UNIT="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/claudius-maximus.service"
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
  rm -f "$PLIST"
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
