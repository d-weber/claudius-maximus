"""Cross-platform advisory file locks (fcntl.flock on POSIX, msvcrt.locking on Windows).

Used for the shared device lock (<repo>/.devlock, see tools/dev.py and link.py) and the daemon's
single-instance lock. Non-blocking: `try_lock` returns False when another process holds the lock.
"""
from __future__ import annotations

import os
import sys

if sys.platform == "win32":
    import msvcrt

    def try_lock(fd: int) -> bool:
        try:
            os.lseek(fd, 0, os.SEEK_SET)
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
            return True
        except OSError:
            return False

    def unlock(fd: int) -> None:
        try:
            os.lseek(fd, 0, os.SEEK_SET)
            msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
        except OSError:
            pass
else:
    import fcntl

    def try_lock(fd: int) -> bool:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except OSError:
            return False

    def unlock(fd: int) -> None:
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        except OSError:
            pass


def open_lock_file(path: str | os.PathLike) -> int:
    """Open (create) a lock file, returning the fd. Caller uses try_lock/unlock and os.close."""
    return os.open(path, os.O_CREAT | os.O_RDWR, 0o644)


def write_holder(fd: int, text: str) -> None:
    """Best effort: record who holds the lock (readable with open().read() by others). Windows note:
    msvcrt.locking locks byte 0, so the holder text is written after a leading space."""
    try:
        os.lseek(fd, 0, os.SEEK_SET)
        os.ftruncate(fd, 0)
        os.write(fd, (" " + text).encode())
    except OSError:
        pass
