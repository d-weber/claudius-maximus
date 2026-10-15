"""Real Claude Code sessions provider (owned by host-sessions).

Turns the Claude Code data on this machine into `state.sessions`, `state.totals` and `event`s (PROTOCOL.md).
Read-only: never writes anything under ~/.claude.

Sources
-------
~/.claude/sessions/<pid>.json        live session registry, written by every Claude Code process (interactive
                                     and `claude -p`): pid, sessionId, cwd, name/nameSource, kind, startedAt and
                                     -- for the REPL -- status busy|idle|waiting (+ waitingFor: "permission
                                     prompt" | "input needed" | "dialog open" | ...) with statusUpdatedAt (ms).
                                     The file is deleted on exit; we also check pid liveness (os.kill(pid, 0)).
~/.claude/projects/<slug>/<sid>.jsonl            transcript (see companion/sessions_transcript.py)
~/.claude/projects/<slug>/<sid>/subagents/agent-<id>.jsonl (+ .meta.json)   subagent transcripts

Status
------
  offline  pid gone / registry file removed (kept OFFLINE_KEEP_S, then dropped)
  waiting  registry status == "waiting" (Claude Code sets it while a permission prompt, question, elicitation
           or blocking dialog is shown). Without a registry status (non-REPL runs): a pending
           AskUserQuestion / ExitPlanMode tool call in the transcript.
  busy     registry "busy" (Claude Code also stays busy while background subagents run), else transcript turn
           in progress (no end_turn yet and recent writes / pending tool calls)
  idle     otherwise
`since` = registry statusUpdatedAt when available, else the moment we observed the change; it is stable
between polls (daemon.merge_hooks compares it with hook timestamps).

Events: session_start/session_end, prompt, tool, tool_error, stop, waiting, subagent_start/subagent_stop,
todo_done, compact. Only for activity that happens after the provider started (no history flood).
The daemon de-duplicates against providers/hooks.py (see MERGE CONTRACT there): when hooks are active for a
session it drops our events of the kinds hooks cover and keeps the rest (todo_done ...). `tool_error` events
carry an extra `error` field (first line of the error).

Extra session fields (beyond PROTOCOL.md; firmware may ignore): kind ("interactive"/"headless"/...), branch,
perm_mode, waiting (why it's waiting), subagents[].{cost, model, ctx_pct}. `tokens`, `cost` and `tool_count`
include the session's subagents; `ctx_pct` is the main thread's latest request vs the model's context window.

Cost is an ESTIMATE at public API list prices (companion/sessions_text.py PRICES).
"""
from __future__ import annotations

import glob
import json
import logging
import os
import re
import time

from .base import Provider
from ..sessions_text import ascii_text, model_label, tilde
from ..sessions_transcript import Transcript

log = logging.getLogger("companion.sessions")

OFFLINE_KEEP_S = 600        # keep exited sessions this long
SUB_DONE_KEEP_S = 300       # keep finished subagents in the list this long
SUB_STALE_S = 1800          # a subagent silent for this long is considered done
SUB_MAX = 12                # max subagents listed per session
HEADLESS_ALIVE_S = 180      # transcript-only session: offline after this long without writes
SCAN_S = 3.0                # glob ~/.claude/projects for new/grown transcripts
SUB_SCAN_S = 1.0            # list a live session's subagents dir
REST_POLL_S = 2.0           # poll transcripts of non-live sessions this often
POLL_BUDGET_S = 0.12        # max parse time per poll (startup catch-up spreads over several polls)
EVENT_MAX_AGE_S = 90        # don't emit events for records older than this (late catch-up)


def claude_dir() -> str:
    return os.environ.get("CLAUDE_CONFIG_DIR") or os.path.join(os.path.expanduser("~"), ".claude")


def cwd_slug(cwd: str) -> str:
    return re.sub(r"[^A-Za-z0-9]", "-", cwd or "")


def pid_alive(pid) -> bool:
    try:
        pid = int(pid)
    except (TypeError, ValueError):
        return False
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except OSError:
        return False
    return True


class _Sub:
    __slots__ = ("aid", "tr", "meta", "meta_path", "announced", "done_ts", "stopped")

    def __init__(self, aid: str, tr: Transcript, meta_path: str):
        self.aid = aid
        self.tr = tr
        self.meta: dict = {}
        self.meta_path = meta_path
        self.announced = False
        self.done_ts = 0.0       # 0 = busy
        self.stopped = False     # subagent_stop emitted


class _Session:
    def __init__(self, sid: str, kind: str = "interactive"):
        self.sid = sid
        self.short = sid[:8]
        self.kind = kind
        self.pid = None
        self.cwd = ""
        self.reg: dict = {}
        self.reg_live = False
        self.ended = 0.0
        self.started = 0.0
        self.tr: Transcript | None = None
        self.tr_path = ""
        self.status = ""
        self.since = 0.0
        self.waiting = ""
        self.waiting_seen = 0.0
        self.waiting_emit = False
        self.subs: dict[str, _Sub] = {}
        self.sub_scan_at = 0.0

    @property
    def name(self) -> str:
        r = self.reg
        if r.get("name") and r.get("nameSource") in ("user", "peer"):
            return ascii_text(r["name"], 32)
        return ascii_text(os.path.basename((self.cwd or "").rstrip("/")) or self.short, 32)


class SessionsProvider(Provider):
    def __init__(self, base: str | None = None):
        self.base = base or claude_dir()
        self.reg_dir = os.path.join(self.base, "sessions")
        self.proj_dir = os.path.join(self.base, "projects")
        self.start = time.time()
        self.primed = False                         # first poll done (no events before)
        self.sessions: dict[str, _Session] = {}
        self.transcripts: dict[str, Transcript] = {}  # path -> transcript (live sessions + today's files)
        self.sid_paths: dict[str, str] = {}           # sid -> main transcript path (from scans)
        self.reg_cache: dict[str, tuple] = {}         # registry filename -> (mtime, size, data)
        self.reg_sids: set[str] = set()               # every sid ever seen in the registry
        self.last_scan = 0.0
        self.events: list[dict] = []
        self.poll_ms = 0.0
        self.rest_poll_at = 0.0

    # ================================================================== provider API
    def poll(self, now: float) -> None:
        t0 = time.monotonic()
        self._read_registry(now)
        if now - self.last_scan >= SCAN_S:
            self._scan_projects(now)
        self._scan_subagents(now)
        self._read_transcripts(now, t0)
        self._update(now)
        self.primed = True
        self.poll_ms = (time.monotonic() - t0) * 1000

    def drain_events(self) -> list[dict]:
        ev, self.events = self.events, []
        return ev

    def contribute(self, state: dict) -> None:
        now = time.time()
        out = [self._session_json(s, now) for s in self.sessions.values()]

        def recency(x):
            return max(x.get("since") or 0, (x.get("activity") or {}).get("ts") or 0)
        out.sort(key=lambda x: (x["status"] == "offline", -recency(x)))
        state["sessions"] = out
        st = [x["status"] for x in out]
        today = time.strftime("%Y-%m-%d")
        tok = cost = tools = prompts = 0
        for tr in self.transcripts.values():
            if tr.day == today:
                tok += tr.today[0]
                cost += tr.today[1]
                tools += tr.today[2]
                prompts += tr.today[3]
        state["totals"] = {
            "sessions": sum(1 for x in st if x != "offline"),
            "busy": st.count("busy"), "waiting": st.count("waiting"), "idle": st.count("idle"),
            "offline": st.count("offline"),
            "subagents_active": sum(1 for x in out for a in x.get("subagents", []) if a["status"] == "busy"),
            "tokens_today": int(tok), "cost_today": round(cost, 2),
            "tools_today": int(tools), "prompts_today": int(prompts),
        }

    # ================================================================== registry
    def _read_registry(self, now: float) -> None:
        try:
            names = [n for n in os.listdir(self.reg_dir) if n.endswith(".json")]
        except OSError:
            names = []
        live: dict[str, dict] = {}
        seen = set()
        for n in names:
            p = os.path.join(self.reg_dir, n)
            seen.add(n)
            try:
                st = os.stat(p)
            except OSError:
                continue
            c = self.reg_cache.get(n)
            if c is None or c[0] != st.st_mtime or c[1] != st.st_size:
                try:
                    with open(p, "rb") as f:
                        data = json.loads(f.read())
                    if not isinstance(data, dict):
                        continue
                except Exception:
                    if c is None:
                        continue      # torn write: retry next poll
                    data = c[2]
                c = self.reg_cache[n] = (st.st_mtime, st.st_size, data)
            d = c[2]
            sid = d.get("sessionId")
            if not isinstance(sid, str) or not sid:
                continue
            if not pid_alive(d.get("pid")):
                continue
            prev = live.get(sid)
            if prev is None or (d.get("updatedAt") or 0) > (prev.get("updatedAt") or 0):
                live[sid] = d
        for n in list(self.reg_cache):
            if n not in seen:
                del self.reg_cache[n]

        for sid, d in live.items():
            self.reg_sids.add(sid)
            s = self.sessions.get(sid)
            if s is None or s.status == "offline":
                if s is None:
                    s = self.sessions[sid] = _Session(sid, d.get("kind") or "interactive")
                s.ended = 0.0
                s.status = ""
                if self.primed:
                    self._event("session_start", s, now, text=ascii_text(d.get("entrypoint") or "", 20))
            s.reg = d
            s.reg_live = True
            s.kind = d.get("kind") or s.kind
            s.pid = d.get("pid")
            s.cwd = d.get("cwd") or s.cwd
            if d.get("startedAt"):
                s.started = float(d["startedAt"]) / 1000.0
        live_pids = {d.get("pid") for d in live.values()}
        for sid, s in list(self.sessions.items()):
            if s.reg_live and sid not in live:
                s.reg_live = False
                if s.pid in live_pids:
                    # same process switched to a new session id (/clear, /resume): replace, no ghost
                    del self.sessions[sid]
                    continue
                self._end(s, now)

    def _end(self, s: _Session, now: float) -> None:
        if s.status == "offline":
            return
        s.status, s.since, s.ended, s.waiting = "offline", now, now, ""
        self._event("session_end", s, now)

    # ================================================================== transcripts
    def _main_path(self, s: _Session) -> str:
        if s.tr_path:
            return s.tr_path
        p = os.path.join(self.proj_dir, cwd_slug(s.cwd), s.sid + ".jsonl")
        if os.path.exists(p):
            return p
        p = self.sid_paths.get(s.sid, "")
        if not p:
            hits = glob.glob(os.path.join(glob.escape(self.proj_dir), "*", s.sid + ".jsonl"))
            p = hits[0] if hits else ""
        return p

    def _get_tr(self, path: str, sid: str, is_sub: bool) -> Transcript:
        tr = self.transcripts.get(path)
        if tr is None:
            tr = self.transcripts[path] = Transcript(path, sid, is_sub=is_sub, emit_after=self.start - 1.0)
            if is_sub:
                tr.agent_id = os.path.basename(path)[6:-6]   # agent-<id>.jsonl
        return tr

    def _scan_projects(self, now: float) -> None:
        """Track every transcript touched today (for *_today totals) and spot headless sessions."""
        self.last_scan = now
        lt = time.localtime(now)
        midnight = time.mktime((lt.tm_year, lt.tm_mon, lt.tm_mday, 0, 0, 0, 0, 0, -1))
        root = glob.escape(self.proj_dir)
        mains = glob.glob(os.path.join(root, "*", "*.jsonl"))
        subs = glob.glob(os.path.join(root, "*", "*", "subagents", "agent-*.jsonl"))
        for p in mains:
            sid = os.path.basename(p)[:-6]
            self.sid_paths[sid] = p
            if p in self.transcripts:
                continue
            try:
                if os.stat(p).st_mtime >= midnight:
                    self._get_tr(p, sid, False)
            except OSError:
                pass
        for p in subs:
            if p in self.transcripts:
                continue
            try:
                if os.stat(p).st_mtime >= midnight:
                    sid = os.path.basename(os.path.dirname(os.path.dirname(p)))
                    self._get_tr(p, sid, True)
            except OSError:
                pass
        # forget transcripts of files that disappeared or were last touched before today and aren't live
        live_paths = {s.tr_path for s in self.sessions.values()}
        live_paths |= {sub.tr.path for s in self.sessions.values() for sub in s.subs.values()}
        for p, tr in list(self.transcripts.items()):
            if p in live_paths:
                continue
            if not os.path.exists(p) or (tr.tail.mtime and tr.tail.mtime < midnight - 3600 and not tr.tail.pending):
                del self.transcripts[p]

    def _scan_subagents(self, now: float) -> None:
        for s in self.sessions.values():
            if s.status == "offline" and now - s.ended > 30:
                continue
            if now - s.sub_scan_at < SUB_SCAN_S or not s.tr_path:
                continue
            s.sub_scan_at = now
            d = os.path.join(os.path.dirname(s.tr_path), s.sid, "subagents")
            try:
                names = os.listdir(d)
            except OSError:
                continue
            for n in names:
                if not (n.startswith("agent-") and n.endswith(".jsonl")):
                    continue
                aid = n[6:-6]
                if aid in s.subs:
                    continue
                p = os.path.join(d, n)
                sub = _Sub(aid, self._get_tr(p, s.sid, True), p[:-6] + ".meta.json")
                s.subs[aid] = sub
            for sub in s.subs.values():
                if not sub.meta or "description" not in sub.meta:
                    try:
                        with open(sub.meta_path, "rb") as f:
                            m = json.loads(f.read())
                        if isinstance(m, dict):
                            sub.meta = m
                    except Exception:
                        pass

    def _read_transcripts(self, now: float, t0: float) -> None:
        # live sessions (and their subagents) first, then the rest of today's files
        order: list[Transcript] = []
        for s in self.sessions.values():
            if not s.tr_path or s.tr is None:
                p = self._main_path(s)
                if p:
                    s.tr_path = p
                    s.tr = self._get_tr(p, s.sid, False)
            if s.tr is not None:
                order.append(s.tr)
            order.extend(sub.tr for sub in s.subs.values())
        seen = {id(t) for t in order}
        rest = [t for t in self.transcripts.values() if id(t) not in seen]
        # other transcripts (today's totals, headless detection): stat them every REST_POLL_S only, unless a
        # catch-up is still in progress (each stat is a syscall; there can be dozens of files)
        if now - self.rest_poll_at >= REST_POLL_S or any(t.tail.pending for t in rest):
            self.rest_poll_at = now
            order.extend(rest)
        deadline = t0 + POLL_BUDGET_S
        for tr in order:
            while time.monotonic() < deadline:
                if not tr.poll(budget_bytes=1 << 20) or not tr.tail.pending:
                    break
            if time.monotonic() >= deadline:
                break

    # ================================================================== status / subagents
    def _update(self, now: float) -> None:
        # headless sessions: transcripts growing after startup with no registry entry
        for p, tr in self.transcripts.items():
            if tr.is_sub or tr.sid in self.sessions or tr.sid in self.reg_sids:
                continue
            if tr.last_ts > self.start and now - tr.tail.mtime < 60 and tr.tail.mtime > self.start:
                s = self.sessions[tr.sid] = _Session(tr.sid, "headless")
                s.tr, s.tr_path, s.cwd = tr, p, tr.cwd
                s.started = tr.first_ts or now
                self._event("session_start", s, now, text="headless")

        for sid, s in list(self.sessions.items()):
            tr = s.tr
            if tr is not None:
                if not s.cwd and tr.cwd:
                    s.cwd = tr.cwd
                if not s.started and tr.first_ts:
                    s.started = tr.first_ts
                tr.flush_stop(now, grace=1.5)
                # forward transcript events
                for kind, ev in tr.events:
                    self._event(kind, s, ev.pop("ts", now), **ev)
                tr.events.clear()
            if s.kind == "headless" and s.status != "offline" and tr is not None \
                    and now - tr.tail.mtime > HEADLESS_ALIVE_S:
                self._end(s, now)
            if s.status == "offline":
                self._update_subs(s, now)
                if now - s.ended > OFFLINE_KEEP_S:
                    del self.sessions[sid]
                continue
            self._update_status(s, now)
            self._update_subs(s, now)
        # events of transcripts that belong to no tracked session are dropped
        for tr in self.transcripts.values():
            if tr.events:
                tr.events.clear()

    def _update_status(self, s: _Session, now: float) -> None:
        tr = s.tr
        r = s.reg
        status = r.get("status") if s.reg_live else None
        since = None
        waiting_for = ""
        if status in ("busy", "idle", "waiting"):
            since = float(r.get("statusUpdatedAt") or r.get("updatedAt") or now * 1000) / 1000.0
            waiting_for = r.get("waitingFor") or ""
        else:
            status = "idle"
            if tr is not None:
                if tr.pending_ask():
                    status = "waiting"
                elif tr.turn_open and (tr.pending or now - max(tr.last_ts, tr.tail.mtime) < 120):
                    status = "busy"
        if status != s.status:
            prev = s.status
            s.status = status
            s.since = since if since else now
            s.waiting = ""
            s.waiting_seen = now
            s.waiting_emit = bool(status == "waiting" and prev and self.primed)
        elif since:
            s.since = since
        if status == "waiting":
            # the pending tool_use may hit the transcript a moment after the registry flips to waiting:
            # refresh the text, and emit the event once it is specific (or after a short grace period)
            text, specific = self._waiting_text(s, waiting_for)
            s.waiting = text
            if s.waiting_emit and (specific or now - s.waiting_seen >= 0.75):
                s.waiting_emit = False
                self._event("waiting", s, s.since, text=text)

    @staticmethod
    def _waiting_text(s: _Session, waiting_for: str) -> tuple[str, bool]:
        """(text, specific). E.g. "Allow Bash: git push?", "Question: Which DB?", "Dialog open"."""
        tr = s.tr
        if tr is not None:
            ask = tr.pending_ask()
            if ask:
                return ascii_text(("Plan: " if ask[0] == "ExitPlanMode" else "Question: ") + ask[1], 100), True
            lp = tr.last_pending()
            if lp and waiting_for in ("", "permission prompt") and lp[0] not in ("Agent", "Task"):
                return ascii_text(f"Allow {lp[0]}: {lp[1]}?" if lp[1] else f"Allow {lp[0]}?", 100), True
        if waiting_for and waiting_for != "permission prompt":
            return ascii_text(waiting_for.capitalize(), 100), True
        return ascii_text((waiting_for or "needs your input").capitalize(), 100), False

    def _update_subs(self, s: _Session, now: float) -> None:
        ptr = s.tr
        for aid, sub in list(s.subs.items()):
            tr = sub.tr
            if not tr.first_ts:
                continue          # nothing parsed yet
            # events from the subagent's own transcript (tool / tool_error, tagged with agent)
            if tr.events:
                for kind, ev in tr.events:
                    if kind in ("tool", "tool_error"):
                        self._event(kind, s, ev.pop("ts", now), **ev)
                tr.events.clear()
            if not sub.announced:
                sub.announced = True
                if tr.first_ts >= self.start - 2 and self.primed:
                    self._event("subagent_start", s, tr.first_ts, agent=aid[:8], text=self._sub_desc(sub))
            done = 0.0
            if tr.handback:
                done = tr.last_ts
            elif not tr.turn_open and tr.turn_end_ts:
                done = tr.turn_end_ts
            if ptr is not None and not done:
                done = ptr.done_agents.get(aid) or ptr.agent_tool_done.get(sub.meta.get("toolUseId", "")) or 0.0
            last = max(tr.last_ts, tr.tail.mtime)
            if not done and (now - last > SUB_STALE_S or s.status == "offline"):
                done = last
            if done and tr.last_ts > done + 2 and not tr.handback and tr.turn_open:
                done = 0.0        # resumed after it had finished (SendMessage)
            if done and not sub.done_ts:
                sub.done_ts = done
                if not sub.stopped and sub.announced and sub.done_ts >= self.start and self.primed:
                    self._event("subagent_stop", s, now, agent=aid[:8], text=self._sub_desc(sub))
                sub.stopped = True
            elif not done and sub.done_ts:
                sub.done_ts = 0.0
                sub.stopped = False

    @staticmethod
    def _sub_desc(sub: _Sub) -> str:
        m = sub.meta
        return ascii_text(m.get("description") or sub.tr.last_prompt or m.get("agentType") or "subagent", 60)

    # ================================================================== output
    def _event(self, kind: str, s: _Session, ts: float, **kw) -> None:
        now = time.time()
        if kind not in ("session_start", "session_end") and ts < now - EVENT_MAX_AGE_S:
            return
        e = {"t": "event", "kind": kind, "sid": s.short, "name": s.name, "ts": int(ts)}
        for k, v in kw.items():
            if v is not None and v != "":
                e[k] = ascii_text(v, 120) if isinstance(v, str) else v
        self.events.append(e)

    def _session_json(self, s: _Session, now: float) -> dict:
        tr = s.tr
        tok = [0, 0, 0, 0]
        cost = 0.0
        tools = 0
        if tr is not None:
            tok = list(tr.tokens)
            cost = tr.cost
            tools = tr.tool_count
        subs_out = []
        for aid, sub in s.subs.items():
            st = sub.tr
            for k in range(4):
                tok[k] += st.tokens[k]
            cost += st.cost
            tools += st.tool_count
            if not st.first_ts:
                continue
            if sub.done_ts and now - sub.done_ts > SUB_DONE_KEEP_S:
                continue
            m = sub.meta
            subs_out.append({
                "id": aid[:8], "desc": self._sub_desc(sub),
                "type": ascii_text(m.get("agentType") or "", 24),
                "status": "done" if sub.done_ts else "busy",
                "tool": st.activity.get("tool", ""), "detail": st.activity.get("detail", ""),
                "started": int(st.first_ts),
                "tokens": int(st.tokens[0] + st.tokens[1] + st.tokens[3]),
                "cost": round(st.cost, 2), "model": model_label(st.model), "ctx_pct": st.ctx_pct(),
            })
        subs_out.sort(key=lambda x: (x["status"] != "busy", -x["started"]))
        subs_out = subs_out[:SUB_MAX]
        activity = dict(tr.activity) if tr else {"tool": "", "detail": "", "ts": int(s.started or now)}
        busy_subs = [x for x in subs_out if x["status"] == "busy"]
        if s.status == "busy" and busy_subs and tr is not None and not tr.turn_open:
            # main thread finished its turn but background subagents keep the session busy
            n = len(busy_subs)
            ts = max([int(s.subs[a].tr.activity.get("ts") or 0) for a in s.subs
                      if a[:8] in {x["id"] for x in busy_subs}] + [activity.get("ts") or 0])
            activity = {"tool": "Agent", "detail": f"{n} subagent{'s' if n > 1 else ''} working", "ts": ts}
        project = ascii_text(os.path.basename((s.cwd or "").rstrip("/")), 32)
        out = {
            "id": s.short, "name": s.name, "project": project or s.short, "cwd": ascii_text(tilde(s.cwd), 80),
            "title": ascii_text((tr.custom_title or tr.title) if tr else "", 60),
            "status": s.status or "idle", "since": int(s.since or now),
            "started": int(s.started or (tr.first_ts if tr else 0) or now),
            "model": model_label(tr.model) if tr else "",
            "activity": activity,
            "last_prompt": (tr.last_prompt or ascii_text(tr.fallback_prompt, 120)) if tr else "",
            "tokens": {"in": tok[0], "out": tok[1], "cache_read": tok[2], "cache_write": tok[3]},
            "cost": round(cost, 2),
            "ctx_pct": tr.ctx_pct() if tr else 0,
            "tool_count": tools,
            "subagents": subs_out,
            "kind": ascii_text(s.kind, 16),
        }
        if not out["activity"].get("ts"):
            out["activity"]["ts"] = int(s.started or now)
        if tr is not None:
            td = tr.todos_summary()
            if td:
                out["todos"] = td
            if tr.branch:
                out["branch"] = ascii_text(tr.branch, 40)
            if tr.perm_mode:
                out["perm_mode"] = ascii_text(tr.perm_mode, 16)
        if s.status == "waiting" and s.waiting:
            out["waiting"] = s.waiting
        return out
