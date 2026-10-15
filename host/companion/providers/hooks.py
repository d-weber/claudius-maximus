"""Real-time signals from Claude Code hooks + statusLine (owned by host-runtime).

================================================================================================
FILES / IPC (all under CLAUDIUS_HOME, default ~/.claudius-maximus/)
================================================================================================
hooks.sock          unix DATAGRAM socket bound by the daemon (HooksProvider). Hook scripts send one JSON
                    datagram (<= 2000 bytes) per hook invocation. If the socket file is missing the hook
                    does nothing. See host/hooks/cm_hook.py for the datagram format (short keys).
statusline.json     **For providers/usage.py.** Written atomically (tmp + rename) by the statusLine wrapper
                    host/hooks/cm_statusline.py every time Claude Code refreshes its status line (after each
                    assistant message, and when a rate-limit window resets). Format:
                        {
                          "v": 1,
                          "captured_at": 1792068344,        # epoch s of this write
                          "session_id": "969ca7a9-....",     # session that triggered the refresh
                          "model": "Opus 5.5", "model_id": "claude-opus-5-5", "version": "2.1.300",
                          "ctx_pct": 42.0,                   # context_window.used_percentage (may be null)
                          "ctx_size": 200000,
                          "cost_usd": 3.42,                  # that session's cost.total_cost_usd
                          "rate_limits": {                   # verbatim from Claude Code, null if never seen
                            "five_hour": {"used_percentage": 23.5, "resets_at": 1738425600},
                            "seven_day": {"used_percentage": 41.2, "resets_at": 1738857600}
                            # (+ optional "spend_limit": {used_percentage, resets_at, used_usd, limit_usd, period})
                          },
                          "rate_limits_at": 1792068344       # epoch s when rate_limits was last present
                        }
                    rate_limits only exists for claude.ai Pro/Max subscribers, after the first API response
                    of a session; each window may be absent, and Claude drops a window once its resets_at
                    passes — so treat a window whose resets_at < now as unknown/0 %. The previous value is
                    kept when a refresh carries no rate_limits.
                    Live copy: HooksProvider.rate_limits / .rate_limits_at (same dict, from datagrams).
statusline-chain.json  {"command": "<user's previous statusLine command>"} saved by the installer; the
                    wrapper runs it and prints its output, so the user's status line is unchanged.
daemon.lock, status.json   single-instance lock and daemon status (see daemon.py / `companion status`).

================================================================================================
HOOK EVENTS USED  (installed as async command hooks, so they never block Claude Code)
================================================================================================
SessionStart, SessionEnd, UserPromptSubmit, PreToolUse, PostToolUse, PostToolUseFailure, PermissionRequest,
PermissionDenied, Notification, Stop, StopFailure, SubagentStart, SubagentStop, PreCompact.

Status derivation (per session, keyed by full session_id):
    UserPromptSubmit / PreToolUse / PostToolUse* / PermissionDenied   -> busy
    PermissionRequest, Notification(permission_prompt|elicitation_dialog),
    PreToolUse(AskUserQuestion|ExitPlanMode)                           -> waiting   (+ `waiting` event)
    Stop / StopFailure / Notification(idle_prompt) / interrupted tool  -> idle      (+ `stop` event)
    SessionStart -> idle,  SessionEnd -> offline

================================================================================================
MERGE CONTRACT WITH providers/sessions.py  (implemented in daemon.merge_hooks)
================================================================================================
* SessionsProvider (transcripts + ~/.claude/sessions/<pid>.json) contributes state["sessions"] with
  "id" = first 8 chars of the session uuid, "status" and "since" (epoch when *its* status last changed —
  please keep it stable between polls, don't bump it every tick).
* HooksProvider.live() returns {short_id: {...}} with "status", "since" (epoch of the hook that set it),
  "activity", "subagents", "ctx_pct", "cost", ...  The daemon overrides a session's status/since with the
  hook's iff hook.since >= session.since - 1  (i.e. whichever source saw the most recent change wins;
  e.g. a user interrupt with no Stop hook is corrected by the sessions provider's newer `idle`).
  "activity" is overridden if the hook's activity is newer. ctx_pct/cost from statusLine override.
  Sessions known only to hooks are appended with the minimal fields hooks know.
* Events: for a session that has sent hooks in the last 10 min, the daemon DROPS sessions-provider events
  of kinds hooks already cover (HOOK_KINDS below) and keeps the others (e.g. todo_done). Also a generic
  dedupe drops identical (kind, sid, tool, text, agent) events within 5 s.
"""
from __future__ import annotations

import json
import logging
import os
import socket
import time

from .base import Provider

log = logging.getLogger("companion.hooks")

# Event kinds hooks are authoritative for (sessions-provider duplicates are dropped by the daemon).
HOOK_KINDS = {"prompt", "tool", "tool_error", "waiting", "stop", "subagent_start", "subagent_stop",
              "session_start", "session_end", "compact"}

ASK_TOOLS = {"AskUserQuestion", "ExitPlanMode"}
QUIET_TOOLS = {"SubagentHandback"}  # internal plumbing: no `tool` event
AGENT_TOOLS = {"Agent", "Task"}
OFFLINE_KEEP_S = 600
FORGET_S = 6 * 3600


def state_dir() -> str:
    return os.environ.get("CLAUDIUS_HOME") or os.path.join(os.path.expanduser("~"), ".claudius-maximus")


def sock_path() -> str:
    return os.path.join(state_dir(), "hooks.sock")


def statusline_cache_path() -> str:
    return os.path.join(state_dir(), "statusline.json")


def _short(s: str, n: int = 80) -> str:
    s = " ".join(str(s).split())
    return s if len(s) <= n else s[: n - 3] + "..."


def _relpath(p: str, cwd: str | None) -> str:
    if cwd and p.startswith(cwd.rstrip("/") + "/"):
        return p[len(cwd.rstrip("/")) + 1:]
    home = os.path.expanduser("~")
    if p.startswith(home + "/"):
        return "~" + p[len(home):]
    return p


def summarize_tool(tool: str, inp: dict | None, cwd: str | None = None) -> str:
    """Short human summary of a tool call (<= 80 chars)."""
    inp = inp or {}
    t = tool or ""
    if t == "Bash":
        return _short(inp.get("description") or inp.get("command") or "")
    if t in ("Read", "Edit", "Write", "MultiEdit", "NotebookEdit"):
        p = inp.get("file_path") or inp.get("notebook_path") or inp.get("path") or ""
        return _short(_relpath(p, cwd))
    if t in ("Grep", "Glob"):
        return _short(inp.get("pattern") or inp.get("glob") or "")
    if t == "WebFetch":
        u = inp.get("url") or ""
        for pre in ("https://", "http://", "www."):
            if u.startswith(pre):
                u = u[len(pre):]
        return _short(u)
    if t == "WebSearch":
        return _short(inp.get("query") or "")
    if t in AGENT_TOOLS:
        return _short(inp.get("description") or inp.get("prompt") or "")
    if t == "Skill":
        return _short(inp.get("skill") or "")
    if t == "AskUserQuestion":
        return _short(inp.get("questions") or "Question")
    if t == "ExitPlanMode":
        return "Plan ready for review"
    if t.startswith("mcp__"):
        parts = t.split("__")
        return _short(" ".join(parts[1:]))
    for k in ("description", "command", "file_path", "path", "pattern", "url", "query", "prompt"):
        if inp.get(k):
            return _short(_relpath(inp[k], cwd) if k in ("file_path", "path") else inp[k])
    return ""


def tool_label(tool: str) -> str:
    if tool and tool.startswith("mcp__"):
        parts = tool.split("__")
        return parts[-1][:24] if len(parts) > 2 else tool[:24]
    return tool or ""


class HooksProvider(Provider):
    def __init__(self, path: str | None = None):
        self.path = path or sock_path()
        self.sock: socket.socket | None = None
        self.sessions: dict[str, dict] = {}   # full session_id -> record
        self.events: list[dict] = []
        self.rate_limits: dict | None = None
        self.rate_limits_at = 0
        self.received = 0
        self.last_hook_at = 0.0
        self._next_bind_try = 0.0
        self._bind_warned = False

    # -- socket ------------------------------------------------------------------------------
    def _bind(self):
        try:
            os.makedirs(os.path.dirname(self.path), mode=0o700, exist_ok=True)
        except OSError:
            pass
        if os.path.exists(self.path):
            # Is someone (another daemon) alive behind it?
            probe = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            try:
                probe.connect(self.path)
                probe.close()
                if not self._bind_warned:
                    log.warning("hooks socket %s is in use by another process; hooks disabled here", self.path)
                    self._bind_warned = True
                return
            except OSError:
                probe.close()
                try:
                    os.unlink(self.path)
                except OSError:
                    pass
        s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            s.bind(self.path)
            os.chmod(self.path, 0o600)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
            s.setblocking(False)
        except OSError as e:
            s.close()
            log.warning("cannot bind hooks socket %s: %s", self.path, e)
            return
        self.sock = s
        log.info("listening for Claude Code hooks on %s", self.path)

    def fileno(self) -> int | None:
        return self.sock.fileno() if self.sock else None

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            finally:
                self.sock = None
                try:
                    os.unlink(self.path)
                except OSError:
                    pass

    # -- provider API ------------------------------------------------------------------------
    def poll(self, now: float) -> None:
        if not self.sock:
            if now >= self._next_bind_try:
                self._next_bind_try = now + 10
                self._bind()
            if not self.sock:
                return
        elif not os.path.exists(self.path):  # someone removed our socket file: rebind
            self.sock.close()
            self.sock = None
            self._bind()
            if not self.sock:
                return
        for _ in range(1000):
            try:
                data = self.sock.recv(4096)
            except (BlockingIOError, InterruptedError):
                break
            except OSError as e:
                log.warning("hooks socket error: %s", e)
                break
            try:
                msg = json.loads(data.decode("utf-8", "replace"))
                if isinstance(msg, dict):
                    self.handle(msg)
            except Exception:
                log.debug("bad hook datagram: %r", data[:200], exc_info=True)
        self._prune(now)

    def drain_events(self) -> list[dict]:
        ev, self.events = self.events, []
        return ev

    def contribute(self, state: dict) -> None:
        pass  # merged by daemon.merge_hooks(state, self)

    # -- state ------------------------------------------------------------------------------
    def _sess(self, sid: str, ts: float, cwd: str | None) -> dict:
        s = self.sessions.get(sid)
        if s is None:
            s = self.sessions[sid] = {
                "sid": sid, "status": None, "since": 0, "started": int(ts), "last": ts, "cwd": cwd or "",
                "activity": None, "subagents": {}, "pending_agent_desc": [], "last_prompt": None,
                "title": None, "model": None, "ctx_pct": None, "cost": None, "waiting_text": None,
            }
        s["last"] = max(s["last"], ts)
        if cwd and not s["cwd"]:
            s["cwd"] = cwd
        return s

    def _set_status(self, s: dict, status: str, ts: float) -> bool:
        if s["status"] != status:
            s["status"] = status
            s["since"] = int(ts)
            return True
        return False

    def _event(self, kind: str, s: dict, ts: float, **kw):
        e = {"t": "event", "kind": kind, "sid": s["sid"][:8], "name": self._name(s), "ts": int(ts)}
        for k, v in kw.items():
            if v is not None and v != "":
                e[k] = v
        self.events.append(e)

    @staticmethod
    def _name(s: dict) -> str:
        return os.path.basename((s.get("cwd") or "").rstrip("/")) or s["sid"][:8]

    def handle(self, m: dict) -> None:
        ev = m.get("e") or ""
        sid = m.get("sid")
        if not sid or not isinstance(sid, str):
            return
        ts = m.get("ts")
        ts = float(ts) if isinstance(ts, (int, float)) else time.time()
        self.received += 1
        self.last_hook_at = time.time()
        s = self._sess(sid, ts, m.get("cwd"))
        aid = (m.get("aid") or "")[:8] or None
        tool = m.get("tool") or ""
        inp = m.get("inp") if isinstance(m.get("inp"), dict) else {}
        if m.get("title"):
            s["title"] = m["title"]
        if m.get("model") and isinstance(m["model"], str):
            s["model"] = m["model"]

        if ev == "SessionStart":
            self._set_status(s, "idle", ts)
            s["started"] = int(ts) if m.get("src") in (None, "startup") else s["started"]
            if m.get("src") in (None, "startup", "resume", "fork"):
                self._event("session_start", s, ts, text=m.get("src"))
        elif ev == "SessionEnd":
            self._set_status(s, "offline", ts)
            self._event("session_end", s, ts, text=m.get("why"))
        elif ev == "UserPromptSubmit":
            self._set_status(s, "busy", ts)
            txt = _short(m.get("prompt") or "", 120)
            # skip machine-injected turns (subagent hand-backs, task notifications, wakeups)
            if m.get("src") in (None, "user") and not txt.startswith("<"):
                s["last_prompt"] = txt
                self._event("prompt", s, ts, text=txt)
        elif ev == "PreToolUse":
            detail = summarize_tool(tool, inp, s["cwd"])
            if tool in AGENT_TOOLS:
                s["pending_agent_desc"].append((detail, inp.get("subagent_type") or ""))
                del s["pending_agent_desc"][:-8]
            act = {"tool": tool_label(tool), "detail": detail, "ts": int(ts)}
            if aid:
                sub = s["subagents"].get(aid)
                if sub:
                    sub["tool"], sub["detail"] = act["tool"], detail
            else:
                s["activity"] = act
            if tool in ASK_TOOLS:
                self._waiting(s, ts, detail or "Question for you")
            else:
                self._set_status(s, "busy", ts)
            if tool not in QUIET_TOOLS:
                self._event("tool", s, ts, tool=tool_label(tool), text=detail, agent=aid)
        elif ev == "PermissionRequest":
            detail = summarize_tool(tool, inp, s["cwd"])
            self._waiting(s, ts, f"Allow {tool_label(tool)}: {detail}" if detail else f"Allow {tool_label(tool)}?")
        elif ev == "Notification":
            nt = m.get("nt") or ""
            if nt in ("permission_prompt", "elicitation_dialog"):
                self._waiting(s, ts, m.get("msg") or "Needs your attention")
            elif nt == "idle_prompt":
                if s["status"] in (None, "busy", "waiting"):
                    self._set_status(s, "idle", ts)
        elif ev in ("PostToolUse", "PermissionDenied"):
            if not aid or s["status"] == "waiting":
                self._set_status(s, "busy", ts)
            s["waiting_text"] = None
        elif ev == "PostToolUseFailure":
            if m.get("intr"):
                self._set_status(s, "idle", ts)  # user interrupted: no Stop hook will follow
            else:
                self._set_status(s, "busy", ts)
                self._event("tool_error", s, ts, tool=tool_label(tool), text=_short(m.get("err") or "", 80),
                            agent=aid)
            s["waiting_text"] = None
        elif ev == "Stop":
            self._set_status(s, "idle", ts)
            s["waiting_text"] = None
            self._event("stop", s, ts, text=_short(m.get("last") or "", 80))
        elif ev == "StopFailure":
            self._set_status(s, "idle", ts)
            self._event("stop", s, ts, text=_short("API error: " + (m.get("err") or "unknown"), 80))
        elif ev == "SubagentStart":
            if aid:
                desc, typ = "", m.get("at") or ""
                pend = s["pending_agent_desc"]
                if pend:
                    # prefer the pending Agent call with the same subagent type
                    idx = next((i for i, (_, t) in enumerate(pend) if t and t == typ), 0)
                    desc, ptyp = pend.pop(idx)
                    typ = typ or ptyp
                s["subagents"][aid] = {"id": aid, "desc": desc or typ or "subagent", "type": typ or "",
                                       "status": "busy", "tool": "", "detail": "", "started": int(ts),
                                       "ended": 0}
                self._event("subagent_start", s, ts, agent=aid, text=desc or typ)
        elif ev == "SubagentStop":
            if aid:
                sub = s["subagents"].get(aid)
                if sub:
                    sub["status"] = "done"
                    sub["ended"] = int(ts)
                self._event("subagent_stop", s, ts, agent=aid, text=(sub or {}).get("desc") or m.get("at"))
        elif ev == "PreCompact":
            self._event("compact", s, ts, text=m.get("trig"))
        elif ev == "StatusLine":
            if isinstance(m.get("ctx_pct"), (int, float)):
                s["ctx_pct"] = int(round(m["ctx_pct"]))
            if isinstance(m.get("cost_usd"), (int, float)):
                s["cost"] = round(float(m["cost_usd"]), 2)
            rl = m.get("rl")
            if isinstance(rl, dict) and rl:
                self.rate_limits = rl
                self.rate_limits_at = int(ts)

    def _waiting(self, s: dict, ts: float, text: str):
        text = _short(text, 100)
        # PermissionRequest and Notification(permission_prompt) both fire for one prompt: one event only
        if s["status"] == "waiting" and ts - s["since"] < 30:
            s["waiting_text"] = s["waiting_text"] or text
            return
        self._set_status(s, "waiting", ts)
        s["waiting_text"] = text
        self._event("waiting", s, ts, text=text)

    def _prune(self, now: float):
        for sid, s in list(self.sessions.items()):
            if s["status"] == "offline" and now - s["since"] > OFFLINE_KEEP_S:
                del self.sessions[sid]
            elif now - s["last"] > FORGET_S:
                del self.sessions[sid]
            else:
                for aid, sub in list(s["subagents"].items()):
                    if sub["status"] == "done" and now - sub["ended"] > 120:
                        del s["subagents"][aid]

    # -- for the daemon ----------------------------------------------------------------------
    def active_sids(self, now: float, window: float = 600) -> set[str]:
        return {sid[:8] for sid, s in self.sessions.items() if now - s["last"] < window}

    def live(self) -> dict[str, dict]:
        """short id -> what hooks know about that session (see module docstring)."""
        out = {}
        for sid, s in self.sessions.items():
            out[sid[:8]] = {
                "full_id": sid,
                "status": s["status"], "since": s["since"], "started": s["started"], "last": s["last"],
                "cwd": s["cwd"], "name": self._name(s), "title": s["title"], "model": s["model"],
                "activity": s["activity"], "last_prompt": s["last_prompt"], "waiting_text": s["waiting_text"],
                "ctx_pct": s["ctx_pct"], "cost": s["cost"],
                "subagents": [dict(v) for v in s["subagents"].values()],
            }
        return out
