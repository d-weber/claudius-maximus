"""Where things live on this OS (macOS and Linux are both supported).

macOS: logs in ~/Library/Logs, the daemon is a LaunchAgent (host/launchd/*.plist).
Linux: logs in $XDG_STATE_HOME/claudius-maximus (default ~/.local/state/...), the daemon is a systemd
       user service (host/systemd/claudius-maximus.service).
State (socket, status.json, statusline.json, locks) is ~/.claudius-maximus on both; see daemon.state_dir().
"""
from __future__ import annotations

import os
import sys

IS_MAC = sys.platform == "darwin"
IS_LINUX = sys.platform.startswith("linux")

SERVICE_LABEL = "com.claudius-maximus.daemon"      # launchd label
SERVICE_UNIT = "claudius-maximus.service"          # systemd user unit


def log_dir() -> str:
    if IS_MAC:
        return os.path.expanduser("~/Library/Logs")
    base = os.environ.get("XDG_STATE_HOME") or os.path.expanduser("~/.local/state")
    return os.path.join(base, "claudius-maximus")


def log_path() -> str:
    return os.path.join(log_dir(), "claudius-maximus.log")


def service_path() -> str:
    """The installed background-service definition (LaunchAgent plist / systemd user unit)."""
    if IS_MAC:
        return os.path.expanduser(f"~/Library/LaunchAgents/{SERVICE_LABEL}.plist")
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(base, "systemd", "user", SERVICE_UNIT)


def service_name() -> str:
    return "launch agent" if IS_MAC else "systemd unit"


def service_installed() -> bool:
    return os.path.exists(service_path())
