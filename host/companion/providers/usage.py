"""Usage provider: token/cost statistics from Claude Code transcripts (+ plan limits when available).

Sources (read-only):
  ~/.claude/projects/**/*.jsonl      every assistant line carries message.{id,model,usage} + timestamp.
                                     Includes subagents/ transcripts. One API message is split into several
                                     lines (one per content block) that repeat the usage; output_tokens grows,
                                     so we count each message.id once, adding deltas if it grows.
  ~/.claude/stats-cache.json         older daily activity (transcripts get cleaned up) -> streak only.
  ~/.claudius-maximus/statusline.json  plan limits teed from Claude Code's statusLine JSON by
                                     host/hooks/cm_statusline.py (see providers/hooks.py). Absent -> omitted.
  https://api.anthropic.com/api/oauth/usage   the claude.ai usage endpoint Claude Code's /usage dialog calls, GET
                                     with the OAuth bearer Claude Code stores (~/.claude/.credentials.json, or the
                                     "Claude Code-credentials" Keychain item on macOS). Polled every 15 minutes,
                                     never more often (also after errors), in a background thread. The only source
                                     of a *live* per-model weekly window (`limits[]` rows of kind "weekly_scoped",
                                     e.g. Fable); also fills five_hour/seven_day when there is no statusLine tee.
                                     Opt-out: {"usage_endpoint": false} in ~/.claudius-maximus/settings.json.
  ~/.claude.json                     Claude Code's global config; `cachedUsageUtilization` is the last answer of
                                     that endpoint Claude Code itself saw (refreshed when the /usage dialog runs).
                                     Fallback for seven_day_models when the endpoint is off or unreachable.

Incremental: files are read from a remembered byte offset; work is time-budgeted per poll() (startup scan of
hundreds of MB takes a few polls, tailing afterwards costs ~nothing). Only complete lines are consumed.

"tokens" below = input + cache_write + output (tokens actually processed; cache *reads* are excluded because
they would dwarf everything and are 10x+ cheaper). Cache reads are reported separately.
Cost = estimate from public API list prices (PRICES), incl. 5m/1h cache-write multipliers and fast mode.

`usage` object produced (all keys optional on the device side):
  five_hour / seven_day   {pct, resets_at, at}  real plan limits from the statusLine tee (claude.ai Pro/Max only);
                 `at` = when Claude Code last reported them; pct=0 & no resets_at once the window rolled over
  spend        {pct, resets_at, used_usd, limit_usd, period}  optional spend limit (same source)
  seven_day_models  [{name, pct, resets_at, at}]  per-model weekly windows (e.g. name "Fable"), preferred model
                 first (fable, opus, sonnet, then by name). Freshest source wins: the usage endpoint (<= 15 min
                 old), the statusLine tee should Claude Code forward them (`model_scoped` / `limits`), else Claude
                 Code's cached snapshot in ~/.claude.json. `at` says when the value was observed; the device dims
                 the row when that is old. Omitted when unknown.
  tpm          [60]  tokens per minute, last 60 min, oldest first
  hourly       [24]  tokens per local hour today
  daily        [14]  tokens per local day, oldest first, today last
  daily_cost   [14]  USD per day (same days as daily)
  by_model     {family: tokens} over the last 14 days (opus/fable/sonnet/haiku/other)
  streak_days  int   consecutive active days ending today (or yesterday if nothing yet today)
  -- additions (requested in PROTOCOL.md ## Requests) --
  today        {tokens, in, out, cache_read, cache_write, cost, msgs, cache_pct, saved}
                 cache_pct = cache_read / (cache_read + in + cache_write) * 100 (prompt cache hit ratio)
                 saved     = USD saved by cache reads vs paying full input price
  week         {tokens, cost}               last 7 local days incl. today
  tpm_now      int   avg tokens/min over the last 5 minutes
  burn         float USD/hour over the last 60 minutes
  peak_hour    int   local hour (0-23) with most tokens today, -1 if none
  window       {start, resets_at, tokens, cost, pct, est:1}  ESTIMATED current 5-hour block (ccusage-style:
                 starts at the first message after the previous block expired, floored to the hour).
                 pct = tokens vs. your biggest 5h block of the last 14 days (relative, not a plan limit).
  best_day     {tokens, cost}               max over the last 14 days
"""
from __future__ import annotations

import glob
import json
import logging
import os
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta

from .base import Provider

log = logging.getLogger("companion.usage")

HOME = os.path.expanduser("~")
# Claude Code's config dir: $CLAUDE_CONFIG_DIR, else ~/.claude (%USERPROFILE%\.claude on Windows), like
# providers/sessions.claude_dir().
CLAUDE_DIR = os.environ.get("CLAUDE_CONFIG_DIR") or os.path.join(HOME, ".claude")
PROJECTS_GLOB = os.path.join(CLAUDE_DIR, "projects", "**", "*.jsonl")
STATS_CACHE = os.path.join(CLAUDE_DIR, "stats-cache.json")
# statusLine tee written by host/hooks/cm_statusline.py (format documented at the top of providers/hooks.py).
CLAUDIUS_HOME = os.environ.get("CLAUDIUS_HOME") or os.path.join(HOME, ".claudius-maximus")
RATE_LIMIT_FILES = [
    os.environ.get("CLAUDIUS_RATE_LIMITS", ""),
    os.path.join(CLAUDIUS_HOME, "statusline.json"),
]
RATE_LIMIT_MAX_AGE = 8 * 86400   # ignore a tee whose rate_limits were last seen longer ago than this
# Claude Code's global config (holds `cachedUsageUtilization`): $CLAUDE_CONFIG_DIR/.claude.json or ~/.claude.json
CLAUDE_CONFIG_JSON = os.path.join(os.environ["CLAUDE_CONFIG_DIR"], ".claude.json") \
    if os.environ.get("CLAUDE_CONFIG_DIR") else os.path.join(HOME, ".claude.json")
CACHE_RECHECK_S = 30             # ~/.claude.json is rewritten often and is large: re-read it at most this often
# claude.ai usage endpoint (see the module doc). The bearer is read from Claude Code's own credential store and
# used for nothing else; it is never written, refreshed or logged.
USAGE_ENDPOINT = "https://api.anthropic.com/api/oauth/usage"
USAGE_POLL_S = 15 * 60           # hard floor between two requests, also after an error (user's requirement)
USAGE_TIMEOUT_S = 15
CREDENTIALS_FILE = os.path.join(CLAUDE_DIR, ".credentials.json")
KEYCHAIN_SERVICE = "Claude Code-credentials"   # macOS: Claude Code keeps the same JSON in the login Keychain
MODEL_WEEK_PREFERRED = ("fable", "opus", "sonnet")

# USD per million tokens: (input, output, cache_read). Cache write = 1.25x input (5m) / 2x input (1h).
PRICES = [
    ("claude-fable-5-1", (10.0, 50.0, 0.25)),
    ("claude-mythos", (10.0, 50.0, 0.25)),
    ("claude-fable", (10.0, 50.0, 1.0)),
    ("claude-opus-5-5", (4.0, 20.0, 0.20)),
    ("claude-opus", (5.0, 25.0, 0.50)),
    ("claude-sonnet-5", (2.0, 10.0, 0.20)),
    ("claude-sonnet", (3.0, 15.0, 0.30)),
    ("claude-haiku-5", (0.10, 0.50, 0.01)),
    ("claude-haiku", (1.0, 5.0, 0.10)),
]
DEFAULT_PRICE = (4.0, 20.0, 0.20)
FAMILIES = ("opus", "fable", "sonnet", "haiku")

RESCAN_S = 10.0         # how often to glob for new / grown files (stats are recomputed <= every 10 s anyway)
BUDGET_S = 0.15         # max parse time per poll()
MINUTE_KEEP_S = 48 * 3600
DAY_KEEP = 400


def price_of(model: str):
    for prefix, p in PRICES:
        if model.startswith(prefix):
            return p
    return DEFAULT_PRICE


def family_of(model: str) -> str:
    for f in FAMILIES:
        if f in model:
            return f
    return "other"


def parse_ts(s) -> int:
    if isinstance(s, (int, float)):
        return int(s / 1000 if s > 1e11 else s)
    try:
        return int(datetime.fromisoformat(str(s).replace("Z", "+00:00")).timestamp())
    except Exception:
        return 0


def day_key(ts: int) -> str:
    return time.strftime("%Y-%m-%d", time.localtime(ts))


class _FileState:
    __slots__ = ("offset", "size", "mtime")

    def __init__(self):
        self.offset = 0
        self.size = -1
        self.mtime = 0.0


class UsageProvider(Provider):
    def __init__(self, projects_glob: str = PROJECTS_GLOB):
        self.glob = projects_glob
        self.files: dict[str, _FileState] = {}
        self.queue: list[str] = []          # files with unread bytes
        self.last_scan = 0.0
        # message id -> (in, out, cr, cw) already counted
        self.counted: dict[str, tuple] = {}
        # minute epoch -> [tokens, cost, in, out, cr, cw, msgs]
        self.minutes: dict[int, list] = {}
        # local day "YYYY-MM-DD" -> [tokens, cost, in, out, cr, cw, msgs, saved]
        self.days: dict[str, list] = {}
        # local day -> {family: tokens}
        self.day_models: dict[str, dict] = {}
        self.stats_days: set[str] = set()
        self._load_stats_cache()
        self.dirty = True
        self.cache_key = None
        self.computed_at = 0
        self.cached: dict = {}
        self.initial_done = False
        self._rl_mtime: dict = {}
        self._rl_doc: dict = {}
        self._mw_checked = 0.0
        self._mw_mtime = None
        self._mw_rows: list = []
        self._mw_at = 0
        self.endpoint = UsageEndpoint(enabled=usage_endpoint_enabled())

    # ------------------------------------------------------------------ ingest
    def _load_stats_cache(self):
        try:
            with open(STATS_CACHE, encoding="utf-8") as f:
                d = json.load(f)
            for row in d.get("dailyActivity", []):
                if row.get("messageCount", 0) > 0 and row.get("date"):
                    self.stats_days.add(row["date"])
        except Exception:
            pass

    def _scan_files(self, now: float):
        self.last_scan = now
        try:
            paths = glob.glob(self.glob, recursive=True)
        except Exception:
            return
        for p in paths:
            try:
                st = os.stat(p)
            except OSError:
                continue
            fs = self.files.get(p)
            if fs is None:
                fs = self.files[p] = _FileState()
            if st.st_size != fs.size or st.st_mtime != fs.mtime:
                if st.st_size < fs.offset:      # truncated / rewritten
                    fs.offset = 0
                fs.size, fs.mtime = st.st_size, st.st_mtime
                if fs.offset < fs.size and p not in self.queue:
                    self.queue.append(p)
        # newest first so the device gets useful numbers quickly at startup
        self.queue.sort(key=lambda p: -self.files[p].mtime)

    def _read_some(self, deadline: float):
        CH = 4 << 20
        while self.queue and time.monotonic() < deadline:
            p = self.queue[0]
            fs = self.files[p]
            done = True
            try:
                with open(p, "rb") as fh:
                    fh.seek(fs.offset)
                    while True:
                        if time.monotonic() >= deadline:
                            done = False
                            break
                        chunk = fh.read(CH)
                        if not chunk:
                            break
                        end = chunk.rfind(b"\n")
                        if end < 0:
                            chunk += fh.readline()      # very long line: complete it
                            if not chunk.endswith(b"\n"):
                                break                   # partial last line: wait for the writer
                            end = len(chunk) - 1
                        self._parse_block(chunk[: end + 1])
                        fs.offset += end + 1
                        fh.seek(fs.offset)
            except OSError:
                pass
            if done:
                self.queue.pop(0)

    def _parse_block(self, block: bytes):
        for line in block.split(b"\n"):
            if b'"type":"assistant"' not in line or b'"usage"' not in line:
                continue
            try:
                d = json.loads(line)
            except Exception:
                continue
            if d.get("type") != "assistant":
                continue
            m = d.get("message") or {}
            u = m.get("usage")
            model = m.get("model") or ""
            if not u or model.startswith("<"):
                continue
            ts = parse_ts(d.get("timestamp"))
            if not ts:
                continue
            self._add(m.get("id") or d.get("requestId") or d.get("uuid"), model, u, ts)

    def _add(self, mid, model: str, u: dict, ts: int):
        i = int(u.get("input_tokens") or 0)
        o = int(u.get("output_tokens") or 0)
        cr = int(u.get("cache_read_input_tokens") or 0)
        cw = int(u.get("cache_creation_input_tokens") or 0)
        prev = self.counted.get(mid)
        new_msg = prev is None
        if prev:
            di, do, dcr, dcw = (max(0, a - b) for a, b in zip((i, o, cr, cw), prev))
            if not (di or do or dcr or dcw):
                return
            self.counted[mid] = (max(i, prev[0]), max(o, prev[1]), max(cr, prev[2]), max(cw, prev[3]))
        else:
            di, do, dcr, dcw = i, o, cr, cw
            self.counted[mid] = (i, o, cr, cw)
        pin, pout, pcr = price_of(model)
        cc = u.get("cache_creation") or {}
        cw1h = int(cc.get("ephemeral_1h_input_tokens") or 0)
        frac1h = (cw1h / cw) if cw else 0.0
        mult = 2.0 if u.get("speed") == "fast" else 1.0
        cost = mult * (di * pin + do * pout + dcr * pcr + dcw * pin * (1.25 + 0.75 * frac1h)) / 1e6
        saved = dcr * (pin - pcr) / 1e6
        tok = di + do + dcw
        n = 1 if new_msg else 0

        mk = ts - ts % 60
        if ts > time.time() - MINUTE_KEEP_S:
            b = self.minutes.get(mk)
            if b is None:
                b = self.minutes[mk] = [0, 0.0, 0, 0, 0, 0, 0]
            b[0] += tok; b[1] += cost; b[2] += di; b[3] += do; b[4] += dcr; b[5] += dcw; b[6] += n
        dk = day_key(ts)
        b = self.days.get(dk)
        if b is None:
            b = self.days[dk] = [0, 0.0, 0, 0, 0, 0, 0, 0.0]
        b[0] += tok; b[1] += cost; b[2] += di; b[3] += do; b[4] += dcr; b[5] += dcw; b[6] += n; b[7] += saved
        dm = self.day_models.setdefault(dk, {})
        fam = family_of(model)
        dm[fam] = dm.get(fam, 0) + tok
        self.dirty = True

    # ------------------------------------------------------------------ provider API
    def poll(self, now: float) -> None:
        self.endpoint.poll(now)
        if now - self.last_scan > RESCAN_S or not self.files:
            self._scan_files(now)
        if self.queue:
            # first pass: allow a bigger budget so the first snapshot is meaningful
            budget = BUDGET_S if self.initial_done else 1.5
            self._read_some(time.monotonic() + budget)
            if not self.queue:
                if not self.initial_done:
                    log.info("usage: indexed %d files, %d messages", len(self.files), len(self.counted))
                self.initial_done = True
        elif not self.initial_done:
            self.initial_done = True
        # prune old minutes occasionally
        if len(self.minutes) > 3000:
            lim = now - MINUTE_KEEP_S
            self.minutes = {k: v for k, v in self.minutes.items() if k >= lim}

    def contribute(self, state: dict) -> None:
        now = int(time.time())
        # recompute only at minute boundaries (tpm/hourly/today stay stable within a minute; the device
        # string-compares and skips redraws, and every change of the ~1 KB usage part is re-sent)
        minute = now // 60
        if not self.cached or minute != self.cache_key or (self.dirty and not self.initial_done):
            self.cached = self._compute(now)
            self.dirty = False
            self.cache_key = minute
            self.computed_at = now
        usage = dict(self.cached)
        usage.update(self._rate_limits(now))
        # the endpoint's windows fill what the tee lacks; for the per-model rows the fresher observation wins
        for k, v in self.endpoint.windows(now).items():
            cur = usage.get(k)
            cur_at = (cur[0] if isinstance(cur, list) and cur else cur or {}).get("at", 0) if cur else 0
            if cur is None or (k == "seven_day_models" and v[0]["at"] >= cur_at):
                usage[k] = v
        if "seven_day_models" not in usage:
            mw = self._cached_model_week(now)
            if mw:
                usage["seven_day_models"] = mw
        state["usage"] = usage

    # ------------------------------------------------------------------ derived stats
    def _compute(self, now: int) -> dict:
        cur_min = now - now % 60
        mins = self.minutes
        tpm = [mins[cur_min - 60 * (59 - k)][0] if (cur_min - 60 * (59 - k)) in mins else 0 for k in range(60)]
        tpm_now = sum(tpm[-5:]) // 5
        burn = sum(v[1] for k, v in mins.items() if k > now - 3600)

        lt = time.localtime(now)
        midnight = int(time.mktime((lt.tm_year, lt.tm_mon, lt.tm_mday, 0, 0, 0, 0, 0, -1)))
        hourly = [0] * 24
        for k, v in mins.items():
            if k >= midnight:
                h = time.localtime(k).tm_hour
                hourly[h] += v[0]
        peak_hour = max(range(24), key=lambda h: hourly[h]) if any(hourly) else -1

        today = datetime.fromtimestamp(now).date()
        keys = [(today - timedelta(days=13 - k)).isoformat() for k in range(14)]
        daily = [self.days[k][0] if k in self.days else 0 for k in keys]
        daily_cost = [round(self.days[k][1], 2) if k in self.days else 0 for k in keys]
        by_model: dict[str, int] = {}
        for k in keys:
            for fam, t in self.day_models.get(k, {}).items():
                by_model[fam] = by_model.get(fam, 0) + t
        td = self.days.get(keys[-1]) or [0, 0.0, 0, 0, 0, 0, 0, 0.0]
        denom = td[4] + td[2] + td[5]
        today_obj = {
            "tokens": td[0], "in": td[2], "out": td[3], "cache_read": td[4], "cache_write": td[5],
            "cost": round(td[1], 2), "msgs": td[6],
            "cache_pct": int(round(100 * td[4] / denom)) if denom else 0,
            "saved": round(td[7], 2),
        }
        week = {"tokens": sum(daily[-7:]), "cost": round(sum(daily_cost[-7:]), 2)}
        bi = max(range(14), key=lambda k: daily[k])
        best_day = {"tokens": daily[bi], "cost": daily_cost[bi]}

        # streak
        active = {k for k, v in self.days.items() if v[6] > 0} | self.stats_days
        d = today if today.isoformat() in active else today - timedelta(days=1)
        streak = 0
        while d.isoformat() in active and streak < DAY_KEEP:
            streak += 1
            d -= timedelta(days=1)

        out = {
            "tpm": tpm, "hourly": hourly, "daily": daily, "daily_cost": daily_cost,
            "by_model": by_model, "streak_days": streak,
            "today": today_obj, "week": week, "tpm_now": tpm_now, "burn": round(burn, 2),
            "peak_hour": peak_hour, "best_day": best_day,
        }
        w = self._window(now)
        if w:
            out["window"] = w
        return out

    def _window(self, now: int):
        """ccusage-style 5-hour blocks over the per-minute buckets (last 48 h)."""
        blocks = []          # [start, end, tokens, cost]
        for k in sorted(self.minutes):
            v = self.minutes[k]
            if v[6] == 0 and v[0] == 0:
                continue
            if not blocks or k >= blocks[-1][1]:
                st = k - k % 3600
                blocks.append([st, st + 5 * 3600, 0, 0.0])
            blocks[-1][2] += v[0]
            blocks[-1][3] += v[1]
        if not blocks or blocks[-1][1] <= now:
            return None
        cur = blocks[-1]
        # reference: biggest single-day... use the largest block we can see; if this is the only one, use
        # 1/3 of the best day as a rough "heavy 5h" yardstick
        ref = max([b[2] for b in blocks[:-1]] + [max((v[0] for v in self.days.values()), default=0) // 3, 1])
        ref = max(ref, cur[2])
        return {"start": cur[0], "resets_at": cur[1], "tokens": cur[2], "cost": round(cur[3], 2),
                "pct": int(round(100 * cur[2] / ref)) if ref else 0, "est": 1}

    def _rate_limits(self, now: int) -> dict:
        """Plan limits from the statusLine tee. A window whose resets_at has passed is reported as 0 %."""
        for p in RATE_LIMIT_FILES:
            if not p:
                continue
            try:
                st = os.stat(p)
                if st.st_mtime == self._rl_mtime.get(p):
                    d = self._rl_doc[p]
                else:
                    with open(p, encoding="utf-8") as f:
                        d = json.load(f)
                    self._rl_mtime[p], self._rl_doc[p] = st.st_mtime, d
            except Exception:
                continue
            rl = d.get("rate_limits")
            if not isinstance(rl, dict):
                continue
            seen = parse_ts(d.get("rate_limits_at") or d.get("captured_at") or st.st_mtime)
            if now - seen > RATE_LIMIT_MAX_AGE:
                continue
            out = {}
            for key in ("five_hour", "seven_day", "spend_limit"):
                v = rl.get(key)
                if not isinstance(v, dict):
                    continue
                pct = v.get("used_percentage", v.get("utilization", v.get("pct")))
                if pct is None:
                    continue
                ra = parse_ts(v.get("resets_at") or 0)
                # `at` floored to the minute: the tee is rewritten every few seconds and each change of the
                # usage part is re-sent to the device
                o = {"pct": 0 if (ra and ra < now) else int(round(float(pct))), "at": seen - seen % 60}
                if ra and ra >= now:
                    o["resets_at"] = ra
                if key == "spend_limit":
                    for k in ("used_usd", "limit_usd", "period"):
                        if k in v:
                            o[k] = round(v[k], 2) if isinstance(v[k], float) else v[k]
                    key = "spend"
                out[key] = o
            # per-model weekly windows, should Claude Code forward them to the status line one day: either the
            # /usage shape (`model_scoped`: [{display_name, utilization, resets_at}]) or the raw server `limits[]`
            mw = model_week_rows(rl.get("model_scoped"), rl.get("limits"), now, seen - seen % 60)
            if mw:
                out["seven_day_models"] = mw
            if out:
                return out
        return {}

    def _cached_model_week(self, now: int) -> list:
        """Per-model weekly windows from Claude Code's cached usage snapshot (~/.claude.json), see model_week_rows()."""
        if now - self._mw_checked >= CACHE_RECHECK_S:
            self._mw_checked = now
            try:
                st = os.stat(CLAUDE_CONFIG_JSON)
                if st.st_mtime != self._mw_mtime:
                    with open(CLAUDE_CONFIG_JSON, encoding="utf-8") as f:
                        cu = (json.load(f).get("cachedUsageUtilization") or {})
                    self._mw_mtime = st.st_mtime
                    self._mw_at = int((cu.get("fetchedAtMs") or 0) / 1000)
                    u = cu.get("utilization") if isinstance(cu.get("utilization"), dict) else {}
                    rows = list(u.get("limits") or [])
                    # older snapshots name the buckets instead of listing them
                    for key, name in (("seven_day_opus", "Opus"), ("seven_day_sonnet", "Sonnet")):
                        v = u.get(key)
                        if isinstance(v, dict) and v.get("utilization") is not None:
                            rows.append({"kind": "weekly_scoped", "percent": v["utilization"],
                                         "resets_at": v.get("resets_at"), "scope": {"model": {"display_name": name}}})
                    self._mw_rows = rows
            except Exception:
                self._mw_mtime, self._mw_rows, self._mw_at = None, [], 0
        if not self._mw_rows or not self._mw_at or now - self._mw_at > RATE_LIMIT_MAX_AGE:
            return []
        return model_week_rows(None, self._mw_rows, now, self._mw_at - self._mw_at % 60)


def usage_endpoint_enabled() -> bool:
    """settings.json {"usage_endpoint": false} turns the endpoint off (default on; it is what /usage itself does)."""
    try:
        from ..hostos import settings_get
        return bool(settings_get("usage_endpoint", True))
    except Exception:
        return True


def read_oauth_token(now: float) -> str | None:
    """Claude Code's claude.ai OAuth access token, or None when there is none / it has expired (Claude Code
    refreshes it on its own use; we never touch the refresh token)."""
    doc = None
    try:
        with open(CREDENTIALS_FILE, encoding="utf-8") as f:
            doc = json.load(f)
    except Exception:
        doc = None
    if doc is None and sys.platform == "darwin":
        try:
            r = subprocess.run(["security", "find-generic-password", "-s", KEYCHAIN_SERVICE, "-w"],
                               capture_output=True, text=True, timeout=5)
            if r.returncode == 0 and r.stdout.strip():
                doc = json.loads(r.stdout.strip())
        except Exception:
            doc = None
    o = (doc or {}).get("claudeAiOauth") if isinstance(doc, dict) else None
    if not isinstance(o, dict):
        return None
    tok = o.get("accessToken")
    exp = o.get("expiresAt") or 0
    if not isinstance(tok, str) or not tok:
        return None
    if exp and float(exp) / 1000 < now + 60:     # ms epoch; leave a minute of slack
        return None
    return tok


class UsageEndpoint:
    """Polls the claude.ai usage endpoint in a background thread, at most once per USAGE_POLL_S."""

    def __init__(self, enabled: bool = True):
        self.enabled = enabled
        self.next_at = 0.0
        self.thread: threading.Thread | None = None
        self.lock = threading.Lock()
        self.at = 0                 # epoch s of the last successful answer
        self.data: dict = {}        # that answer (five_hour, seven_day, limits, ...)
        self.error = ""             # last failure, for the log / status
        self._warned_token = False

    def poll(self, now: float) -> None:
        if not self.enabled or now < self.next_at or (self.thread and self.thread.is_alive()):
            return
        self.next_at = now + USAGE_POLL_S       # fixed cadence whatever happens below
        tok = read_oauth_token(now)
        if not tok:
            if not self._warned_token:
                log.info("usage endpoint: no valid claude.ai OAuth token (not a claude.ai login?); will look again")
                self._warned_token = True
            return
        self._warned_token = False
        self.thread = threading.Thread(target=self._fetch, args=(tok,), name="usage-endpoint", daemon=True)
        self.thread.start()

    def _fetch(self, tok: str) -> None:
        req = urllib.request.Request(USAGE_ENDPOINT, headers={
            "Authorization": "Bearer " + tok, "anthropic-beta": "oauth-2025-04-20",
            "Accept": "application/json", "User-Agent": "claudius-maximus"})
        try:
            with urllib.request.urlopen(req, timeout=USAGE_TIMEOUT_S) as r:
                doc = json.loads(r.read().decode("utf-8", "replace"))
            if not isinstance(doc, dict):
                raise ValueError("not a JSON object")
            with self.lock:
                self.data, self.at, self.error = doc, int(time.time()), ""
            log.info("usage endpoint: ok (5h %s%%, 7d %s%%, %d per-model rows)",
                     (doc.get("five_hour") or {}).get("utilization"), (doc.get("seven_day") or {}).get("utilization"),
                     sum(1 for r in doc.get("limits") or [] if isinstance(r, dict) and r.get("kind") == "weekly_scoped"))
        except urllib.error.HTTPError as e:
            self._fail(f"HTTP {e.code}")
        except (urllib.error.URLError, OSError, ValueError) as e:
            self._fail(str(e)[:120])

    def _fail(self, why: str) -> None:
        with self.lock:
            first = self.error != why
            self.error = why
        (log.warning if first else log.debug)("usage endpoint: %s (next try in %d min)", why, USAGE_POLL_S // 60)

    def windows(self, now: int) -> dict:
        """five_hour / seven_day {pct, resets_at, at} and seven_day_models from the last answer (<= RATE_LIMIT_MAX_AGE)."""
        with self.lock:
            data, at = self.data, self.at
        if not data or not at or now - at > RATE_LIMIT_MAX_AGE:
            return {}
        out = {}
        for key in ("five_hour", "seven_day"):
            v = data.get(key)
            if not isinstance(v, dict) or v.get("utilization") is None:
                continue
            ra = parse_ts(v.get("resets_at") or 0)
            o = {"pct": 0 if (ra and ra < now) else int(round(float(v["utilization"]))), "at": at}
            if ra and ra >= now:
                o["resets_at"] = ra
            out[key] = o
        mw = model_week_rows(None, data.get("limits"), now, at)
        if mw:
            out["seven_day_models"] = mw
        return out


def model_week_rows(scoped, limits, now: int, at: int) -> list:
    """Normalise per-model weekly windows to [{name, pct, resets_at?, at}], preferred model first.
    `scoped`: Claude Code's /usage shape [{display_name, utilization, resets_at}]; `limits`: the usage endpoint's
    raw rows [{kind: "weekly_scoped", percent, resets_at, scope: {model: {display_name}}}]. A window whose
    resets_at has passed is dropped (the value belongs to the previous week; the current one is unknown)."""
    out: dict[str, dict] = {}
    rows = []
    for r in scoped or []:
        if isinstance(r, dict):
            rows.append((r.get("display_name"), r.get("utilization"), r.get("resets_at")))
    for r in limits or []:
        if isinstance(r, dict) and r.get("kind") == "weekly_scoped":
            model = ((r.get("scope") or {}).get("model") or {}) if isinstance(r.get("scope"), dict) else {}
            rows.append((model.get("display_name"), r.get("percent", r.get("utilization")), r.get("resets_at")))
    for name, pct, ra in rows:
        if not isinstance(name, str) or not name.strip() or pct is None:
            continue
        try:
            pct = int(round(float(pct)))
        except (TypeError, ValueError):
            continue
        ra = parse_ts(ra) if ra else 0
        if ra and ra < now:
            continue
        name = name.strip()
        o = {"name": name, "pct": max(0, pct), "at": at}
        if ra:
            o["resets_at"] = ra
        out.setdefault(name.lower(), o)

    def rank(k: str):
        for i, pref in enumerate(MODEL_WEEK_PREFERRED):
            if pref in k:
                return (i, k)
        return (len(MODEL_WEEK_PREFERRED), k)
    return [out[k] for k in sorted(out, key=rank)]

if __name__ == "__main__":      # quick check: python -m companion.providers.usage
    t0 = time.time()
    up = UsageProvider()
    while True:
        up.poll(time.time())
        if up.initial_done and not up.queue:
            break
    s: dict = {}
    up.contribute(s)
    print(f"indexed in {time.time() - t0:.2f}s, {len(up.counted)} msgs, {len(up.files)} files")
    print(json.dumps(s["usage"], indent=1))
