#!/bin/bash
# Claudius Maximus — install the host side for the current user.
#   tools/install.sh [--no-launchd] [--no-hooks] [--no-statusline]
# Idempotent: safe to re-run (e.g. after moving the repo or updating).
#  1. Python venv in host/.venv with pyserial (+ psutil, optional)
#  2. Claude Code hooks + statusLine wrapper merged into ~/.claude/settings.json (timestamped backup)
#  3. LaunchAgent ~/Library/LaunchAgents/com.claudius-maximus.daemon.plist (RunAtLoad, KeepAlive)
# If $HOME is not your real home directory (testing), launchctl is never called.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL=com.claudius-maximus.daemon
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
VENV="$ROOT/host/.venv"
PY="$VENV/bin/python"
DO_LAUNCHD=1; DO_HOOKS=1; SL_FLAG=""
for a in "$@"; do
  case "$a" in
    --no-launchd) DO_LAUNCHD=0 ;;
    --no-hooks) DO_HOOKS=0 ;;
    --no-statusline) SL_FLAG="--no-statusline" ;;
    -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done

REAL_HOME="$(/usr/bin/python3 -c 'import pwd,os;print(pwd.getpwuid(os.getuid()).pw_dir)' 2>/dev/null || echo "$HOME")"
if [ "$HOME" != "$REAL_HOME" ]; then
  echo "note: HOME=$HOME is not the real home ($REAL_HOME): test mode, launchctl will not be called"
  TEST_MODE=1
else
  TEST_MODE=0
fi

echo "==> Python environment ($VENV)"
if [ ! -x "$PY" ]; then
  BASEPY="$(command -v python3.15 || command -v python3.14 || command -v python3.13 || command -v python3.12 || command -v python3)"
  "$BASEPY" -m venv "$VENV"
fi
if ! "$PY" -c 'import serial' 2>/dev/null; then
  "$PY" -m pip install --quiet --disable-pip-version-check pyserial
fi
# optional: host metrics for the System screen (providers/system.py degrades gracefully without it)
"$PY" -c 'import psutil' 2>/dev/null || "$PY" -m pip install --quiet --disable-pip-version-check psutil \
  || echo "  (psutil not installed: System screen metrics disabled)"
"$PY" -c 'import serial, sys; print("  python", sys.version.split()[0], "pyserial", serial.VERSION)'

mkdir -p "$HOME/.claudius-maximus" "$HOME/Library/Logs"
chmod 700 "$HOME/.claudius-maximus"

if [ "$DO_HOOKS" = 1 ]; then
  echo "==> Claude Code hooks + statusLine (~/.claude/settings.json)"
  "$PY" "$ROOT/host/hooks/cm_settings.py" install --root "$ROOT" --python "$PY" $SL_FLAG
fi

if [ "$DO_LAUNCHD" = 1 ]; then
  echo "==> LaunchAgent ($PLIST)"
  mkdir -p "$HOME/Library/LaunchAgents"
  sed -e "s|@PYTHON@|$PY|g" -e "s|@ROOT@|$ROOT|g" -e "s|@HOME@|$HOME|g" \
      "$ROOT/host/launchd/$LABEL.plist" > "$PLIST.tmp"
  /usr/bin/plutil -lint "$PLIST.tmp" >/dev/null
  mv "$PLIST.tmp" "$PLIST"
  if [ "$TEST_MODE" = 0 ]; then
    launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
    launchctl bootstrap "gui/$(id -u)" "$PLIST"
    launchctl enable "gui/$(id -u)/$LABEL" || true
    echo "  daemon started"
  else
    echo "  (test mode: skipped launchctl bootstrap)"
  fi
fi

echo
echo "Done. Check with:   $PY -m companion status      (run from $ROOT/host)"
echo "Logs:               ~/Library/Logs/claudius-maximus.log"
echo "Hooks take effect in new Claude Code sessions (or after /hooks reload)."
