"""Text / model / pricing helpers for the sessions provider (owned by host-sessions).

Everything that ends up on the device goes through `ascii_text()` (the device fonts are ASCII-only).
"""
from __future__ import annotations

import ntpath
import os
import re
import sys
import unicodedata
from datetime import datetime
from urllib.parse import urlsplit

HOME = os.path.expanduser("~")
IS_WINDOWS = sys.platform == "win32"

# --------------------------------------------------------------------------------------------- text
_PUNCT = {
    "‘": "'", "’": "'", "‚": "'", "‛": "'", "′": "'",
    "“": '"', "”": '"', "„": '"', "‟": '"', "″": '"',
    "–": "-", "—": "-", "―": "-", "−": "-", "‐": "-", "‑": "-",
    "…": "...", "•": "*", "·": "*", "●": "*", "‣": "*",
    "→": "->", "←": "<-", "⇒": "=>", "↔": "<->", "↑": "^", "↓": "v",
    "≤": "<=", "≥": ">=", "≠": "!=", "≈": "~", "×": "x", "÷": "/",
    " ": " ", " ": " ", " ": " ", " ": " ", "　": " ",
    "​": "", "‌": "", "‍": "", "﻿": "", "️": "",
    "✓": "v", "✔": "v", "✗": "x", "✘": "x", "°": "deg",
    "«": '"', "»": '"', "‹": "<", "›": ">", "ß": "ss",
    "æ": "ae", "Æ": "AE", "œ": "oe", "Œ": "OE", "ø": "o", "Ø": "O",
    "ł": "l", "Ł": "L", "µ": "u", "€": "EUR", "£": "GBP", "©": "(c)",
    "®": "(R)", "™": "TM", "½": "1/2", "¼": "1/4", "¾": "3/4",
}
_WS = re.compile(r"\s+")
_ASCII_OK = re.compile(r"^[\x20-\x7e]*$")


def ascii_text(s, n: int | None = None) -> str:
    """Transliterate to printable ASCII, collapse whitespace, truncate to n chars ("..." suffix)."""
    if s is None:
        return ""
    if not isinstance(s, str):
        s = str(s)
    s = _WS.sub(" ", s).strip()
    if not _ASCII_OK.match(s):
        out = []
        for ch in s:
            o = ord(ch)
            if 0x20 <= o < 0x7f:
                out.append(ch)
                continue
            r = _PUNCT.get(ch)
            if r is not None:
                out.append(r)
                continue
            d = unicodedata.normalize("NFKD", ch).encode("ascii", "ignore").decode()
            out.append(d)   # emoji & co. -> dropped
        s = _WS.sub(" ", "".join(out)).strip()
    if n is not None and len(s) > n:
        s = s[: max(0, n - 3)].rstrip() + "..."
    return s


_MD = re.compile(r"(\*\*|__|`+|^#+\s*|^[-*>]\s+|\[([^\]]*)\]\([^)]*\))")


def plain(s: str) -> str:
    """Strip the most common markdown decorations from one line of model text."""
    return _MD.sub(lambda m: m.group(2) if m.group(2) is not None else "", s or "").strip()


def folder_name(cwd: str) -> str:
    """Last component of a directory path ("" if none). Windows paths may use either separator and end in one;
    Claude Code on Windows reports cwd like C:\\Users\\x\\proj."""
    if IS_WINDOWS:
        return ntpath.basename((cwd or "").rstrip("/\\"))
    return os.path.basename((cwd or "").rstrip("/"))


def _win_under(p: str, base: str) -> bool:
    """p == base or p inside base, Windows rules: case-insensitive, / and \\ are the same separator."""
    a, b = p.replace("/", "\\").lower(), base.replace("/", "\\").rstrip("\\").lower()
    return bool(b) and (a == b or a.startswith(b + "\\"))


def tilde(p: str) -> str:
    if IS_WINDOWS:
        return "~" + p[len(HOME.rstrip("/\\")):] if p and _win_under(p, HOME) else (p or "")
    if p and (p == HOME or p.startswith(HOME + "/")):
        return "~" + p[len(HOME):]
    return p or ""


def _short_path_win(p: str, cwd: str | None, maxlen: int) -> str:
    c = (cwd or "").rstrip("/\\")
    rel = p[len(c) + 1:] if c and _win_under(p, c) and len(p) > len(c) else tilde(p)
    if len(rel) <= maxlen:
        return rel
    parts = [x for x in re.split(r"[\\/]", p) if x]
    if len(parts) >= 2:
        two = parts[-2] + "\\" + parts[-1]
        if len(two) <= maxlen:
            return two
    return parts[-1] if parts else rel


def short_path(p: str, cwd: str | None = None, maxlen: int = 40) -> str:
    """cwd-relative path if short, else the last 2 components, else the basename."""
    if not p:
        return ""
    p = str(p)
    if IS_WINDOWS:
        return _short_path_win(p, cwd, maxlen)
    c = (cwd or "").rstrip("/")
    if c and p.startswith(c + "/"):
        rel = p[len(c) + 1:]
    else:
        rel = tilde(p)
    if len(rel) <= maxlen:
        return rel
    parts = [x for x in p.split("/") if x]
    if len(parts) >= 2:
        two = parts[-2] + "/" + parts[-1]
        if len(two) <= maxlen:
            return two
    return parts[-1] if parts else rel


def _url_detail(u: str) -> str:
    try:
        sp = urlsplit(u)
        host = sp.netloc
        if host.startswith("www."):
            host = host[4:]
        return host + (sp.path if sp.path not in ("", "/") else "")
    except Exception:
        return u


AGENT_TOOLS = ("Agent", "Task")
ASK_TOOLS = ("AskUserQuestion", "ExitPlanMode")


def tool_label(name: str) -> str:
    """Short tool name for the device. mcp__server__tool -> tool."""
    if name and name.startswith("mcp__"):
        parts = name.split("__")
        return ascii_text(parts[-1] if len(parts) > 2 else name, 24)
    return ascii_text(name or "", 24)


def tool_detail(name: str, inp, cwd: str | None = None, n: int = 80) -> str:
    """Crisp human summary of a tool call (<= n chars, ASCII)."""
    if not isinstance(inp, dict):
        inp = {}
    t = name or ""
    g = inp.get
    d = ""
    if t in ("Read", "Edit", "Write", "MultiEdit", "NotebookEdit", "NotebookRead"):
        d = short_path(g("file_path") or g("notebook_path") or g("path") or "", cwd)
    elif t == "Bash":
        cmd = g("command") or ""
        d = cmd if len(_WS.sub(" ", cmd)) <= n else (g("description") or cmd)
    elif t == "BashOutput" or t == "KillShell" or t == "KillBash":
        d = g("bash_id") or g("shell_id") or ""
    elif t == "Grep":
        pat = g("pattern") or ""
        where = g("glob") or g("type") or (short_path(g("path"), cwd, 24) if g("path") else "")
        d = f'"{pat}"' + (f" in {where}" if where else "")
    elif t == "Glob":
        d = g("pattern") or ""
        if g("path"):
            d += " in " + short_path(g("path"), cwd, 24)
    elif t == "WebFetch":
        d = _url_detail(g("url") or "")
    elif t == "WebSearch":
        d = g("query") or ""
    elif t in AGENT_TOOLS:
        d = g("description") or g("prompt") or ""
    elif t == "SendMessage":
        d = g("summary") or g("message") or g("to") or ""
    elif t == "SubagentHandback":
        d = "Report to caller"
    elif t == "Skill":
        d = g("skill") or g("command") or ""
    elif t == "AskUserQuestion":
        qs = g("questions")
        if isinstance(qs, list) and qs and isinstance(qs[0], dict):
            d = qs[0].get("question") or qs[0].get("header") or "Question"
        else:
            d = g("question") or "Question"
    elif t == "ExitPlanMode":
        d = "Plan ready for review"
    elif t == "TodoWrite":
        todos = g("todos") if isinstance(g("todos"), list) else []
        cur = next((x for x in todos if isinstance(x, dict) and x.get("status") == "in_progress"), None)
        d = (cur.get("activeForm") or cur.get("content")) if cur else f"{len(todos)} todos"
    elif t == "TaskCreate":
        d = g("subject") or g("description") or ""
    elif t == "TaskUpdate":
        d = f"#{g('taskId', '?')} {g('status') or ''}".strip()
    elif t in ("Monitor", "TaskStop"):
        d = g("description") or g("task_id") or g("command") or ""
    elif t == "ToolSearch":
        d = g("query") or ""
    elif t == "Artifact":
        d = g("action") or "publish"
        if g("file_path"):
            d += " " + (ntpath if IS_WINDOWS else os.path).basename(g("file_path"))
    elif t.startswith("mcp__"):
        parts = t.split("__")
        d = parts[1] if len(parts) > 2 else ""
        for k in ("query", "url", "path", "name", "title", "id"):
            if isinstance(g(k), str) and g(k):
                d = (d + ": " if d else "") + g(k)
                break
    else:
        for k in ("description", "command", "file_path", "path", "pattern", "url", "query", "prompt", "name"):
            v = g(k)
            if isinstance(v, str) and v:
                d = short_path(v, cwd) if k in ("file_path", "path") else v
                break
    return ascii_text(d, n)


# --------------------------------------------------------------------------------------------- models
_DATE = re.compile(r"-\d{8}$")


def model_label(model: str) -> str:
    """claude-opus-5-5 -> opus-5.5, claude-haiku-4-5-20251001 -> haiku-4.5, claude-3-5-sonnet-x -> sonnet-3.5"""
    if not model or model.startswith("<"):
        return ""
    m = model.lower().split("[")[0]
    for pre in ("anthropic.", "us.anthropic.", "eu.anthropic.", "global.anthropic."):
        if m.startswith(pre):
            m = m[len(pre):]
    m = m.split("@")[0]
    m = _DATE.sub("", m)
    m = re.sub(r"-v\d+(:\d+)?$", "", m)
    if m.startswith("claude-"):
        m = m[7:]
    parts = m.split("-")
    words = [p for p in parts if not p.isdigit()]
    nums = [p for p in parts if p.isdigit()]
    if not words:
        return ascii_text(m, 16)
    return ascii_text(words[0] + ("-" + ".".join(nums) if nums else ""), 16)


def model_family(model: str) -> str:
    m = (model or "").lower()
    for f in ("opus", "fable", "mythos", "sonnet", "haiku"):
        if f in m:
            return f
    return "other"


def context_window(model: str) -> int:
    """Context window in tokens. Current generation (Opus 4.6+, Sonnet 4.6+, Fable, Haiku 5.5) = 1M;
    older models (Haiku 4.5, *-4-5 and before, Claude 3.x) = 200k. '[1m]' suffix forces 1M."""
    m = (model or "").lower()
    if "[1m]" in m or "-1m" in m:
        return 1_000_000
    if not m or m.startswith("<"):
        return 200_000
    if "claude-3" in m or "haiku-4" in m:
        return 200_000
    mm = re.search(r"(opus|sonnet)-(\d+)(?:-(\d+))?", m)
    if mm:
        major = int(mm.group(2))
        minor = int(mm.group(3)) if mm.group(3) and len(mm.group(3)) <= 2 else 0
        if major < 4 or (major == 4 and minor < 6):
            return 200_000
    return 1_000_000


# --------------------------------------------------------------------------------------------- pricing
# ESTIMATE from public Anthropic API list prices, USD per million tokens: (input, output, cache_read).
# Cache writes = 1.25x input (5-minute TTL) or 2x input (1-hour TTL, which Claude Code uses by default);
# fast mode (usage.speed == "fast") = 2x. First matching prefix wins. Validated against Claude Code's own
# `cost-state` records in transcripts (matches to the cent). Kept identical to providers/usage.py PRICES.
# Subscription (Pro/Max) users don't pay this - it's an "API-equivalent" figure.
PRICES = [
    ("claude-fable-5-1", (10.0, 50.0, 0.25)),
    ("claude-mythos", (10.0, 50.0, 0.25)),
    ("claude-fable", (10.0, 50.0, 1.0)),
    ("claude-opus-5-5", (4.0, 20.0, 0.20)),
    ("claude-opus", (5.0, 25.0, 0.50)),        # opus 5 / 4.x
    ("claude-sonnet-5", (2.0, 10.0, 0.20)),    # sonnet 5.5 / 5
    ("claude-sonnet", (3.0, 15.0, 0.30)),      # sonnet 4.x
    ("claude-haiku-5", (0.10, 0.50, 0.01)),
    ("claude-haiku", (1.0, 5.0, 0.10)),        # haiku 4.5
]
DEFAULT_PRICE = (4.0, 20.0, 0.20)


def price_of(model: str):
    m = (model or "").lower()
    for prefix, p in PRICES:
        if m.startswith(prefix):
            return p
    return DEFAULT_PRICE


def usage_cost(model: str, i: int, o: int, cr: int, cw: int, cw1h: int = 0, fast: bool = False) -> float:
    pin, pout, pcr = price_of(model)
    frac1h = min(1.0, cw1h / cw) if cw else 0.0
    c = (i * pin + o * pout + cr * pcr + cw * pin * (1.25 + 0.75 * frac1h)) / 1e6
    return c * (2.0 if fast else 1.0)


# --------------------------------------------------------------------------------------------- time
def parse_ts(s) -> float:
    """ISO-8601 ('2026-10-15T12:47:39.628Z') or epoch (s or ms) -> epoch seconds (float). 0 if unknown."""
    if isinstance(s, (int, float)):
        return float(s) / 1000.0 if s > 1e11 else float(s)
    if not s:
        return 0.0
    try:
        return datetime.fromisoformat(str(s).replace("Z", "+00:00")).timestamp()
    except Exception:
        return 0.0
