"""Cross-platform advisory file locks (fcntl.flock on POSIX, msvcrt.locking on Windows).

Used for the shared device lock (<repo>/.devlock, see tools/dev.py and link.py) and the daemon's
single-instance lock. Non-blocking: `try_lock` returns False when another process holds the lock.
"""
from __future__ import annotations

import os
import sys

# Windows: msvcrt.locking locks a byte range starting at the current file position, and the lock is
# mandatory (other handles can neither read nor write a locked byte). Lock one byte far past the end of the
# file (allowed by LockFile) so the holder text at offset 0 stays readable by others and can be rewritten and
# truncated freely. 1 GiB stays below 2**31 for any CRT offset arithmetic.
WIN_LOCK_OFFSET = 1 << 30

if sys.platform == "win32":
    import msvcrt

    def try_lock(fd: int) -> bool:
        try:
            os.lseek(fd, WIN_LOCK_OFFSET, os.SEEK_SET)
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)   # one attempt (LK_LOCK would retry for 10 s)
            return True
        except OSError:
            return False
        finally:
            try:
                os.lseek(fd, 0, os.SEEK_SET)
            except OSError:
                pass

    def unlock(fd: int) -> None:
        try:
            os.lseek(fd, WIN_LOCK_OFFSET, os.SEEK_SET)
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
    """Open (create) a lock file, returning the fd. Caller uses try_lock/unlock and os.close.
    O_BINARY: no CRLF translation of the holder text on Windows; O_NOINHERIT: a child process (esptool,
    the status-line chain...) must not keep the lock handle alive after we exit."""
    flags = os.O_CREAT | os.O_RDWR | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOINHERIT", 0)
    return os.open(path, flags, 0o644)


def write_holder(fd: int, text: str) -> None:
    """Best effort: record who holds the lock (readable with open().read() by others; readers strip()).
    Written first, then truncated to its length, so a failing truncate never leaves the file empty. The
    leading space is kept for readers of the earlier format (they used to skip byte 0 on Windows)."""
    try:
        data = (" " + text).encode("utf-8")
        os.lseek(fd, 0, os.SEEK_SET)
        os.write(fd, data)
        os.ftruncate(fd, len(data))
    except OSError:
        pass
