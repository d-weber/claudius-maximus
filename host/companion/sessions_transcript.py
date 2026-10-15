"""Incremental Claude Code transcript (.jsonl) reader + per-transcript state (owned by host-sessions).

Transcript facts this relies on (Claude Code 2.1.x, verified on real data):
  * one line per record; `type` in user / assistant / attachment / system / ai-title / last-prompt /
    queue-operation / permission-mode / api-request* / file-history-* ...
  * one API message is split into several `assistant` lines (one per content block) that repeat
    message.id and message.usage -> usage is counted once per message.id (deltas if it grows).
  * `user` lines carry human prompts (origin.kind == "human", or legacy: plain string content), tool_result
    blocks (is_error), task notifications and subagent hand-backs (origin.kind == "peer", handback=True).
  * `system` subtype compact_boundary = context compaction; turn_duration = end of a turn.
  * subagents live in <session>/subagents/agent-<id>.jsonl (+ .meta.json: agentType, description, toolUseId).
"""
from __future__ import annotations

import json
import os
import re
import time
from collections import OrderedDict

from .sessions_text import (AGENT_TOOLS, ASK_TOOLS, ascii_text, context_window, parse_ts, plain, tool_detail,
                            tool_label, usage_cost)

# record types we never need (cheap prefix check before json.loads)
_SKIP_TYPES = {b"api-request", b"api-request-blob", b"api-request-shape", b"file-history-snapshot",
               b"file-history-delta", b"atis-latch", b"bridge-session", b"mode", b"frame-link",
               b"artifact-comment-monitor", b"artifact-autoreact-ledger", b"pr-link", b"cost-state"}
_TASK_NOTE = re.compile(r"<task-id>([0-9a-z]+)</task-id>.*?<status>(\w+)</status>", re.S)
_NON_PROMPT_PREFIXES = ("<command-", "<local-command", "<task-notification", "<system-reminder", "<bash-",
                        "<agent-message", "Caveat:", "[Request interrupted")


class Tail:
    """Reads complete new lines from a growing file, remembering the byte offset."""

    __slots__ = ("path", "offset", "ino", "size", "mtime")

    def __init__(self, path: str):
        self.path = path
        self.offset = 0
        self.ino = None
        self.size = -1
        self.mtime = 0.0

    def stat(self) -> bool:
        """Refresh size/mtime. Returns True if there may be unread bytes."""
        try:
            st = os.stat(self.path)
        except OSError:
            return False
        if self.ino is not None and st.st_ino != self.ino or st.st_size < self.offset:
            self.offset = 0          # replaced / truncated
        self.ino, self.size, self.mtime = st.st_ino, st.st_size, st.st_mtime
        return self.offset < self.size

    @property
    def pending(self) -> int:
        return max(0, self.size - self.offset)

    def read_lines(self, max_bytes: int = 4 << 20) -> list[bytes]:
        if self.offset >= self.size:
            return []
        try:
            with open(self.path, "rb") as fh:
                fh.seek(self.offset)
                chunk = fh.read(max_bytes)
                end = chunk.rfind(b"\n")
                if end < 0:
                    # a single very long line (big tool result): finish it, or wait for the writer
                    rest = fh.readline()
                    chunk += rest
                    if not chunk.endswith(b"\n"):
                        return []
                    end = len(chunk) - 1
        except OSError:
            return []
        self.offset += end + 1
        return chunk[:end + 1].split(b"\n")[:-1]


def _content_text(c) -> str:
    if isinstance(c, str):
        return c
    if isinstance(c, list):
        out = []
        for b in c:
            if isinstance(b, dict) and b.get("type") == "text":
                out.append(b.get("text") or "")
        return "\n".join(out)
    return ""


def _first_line(s: str) -> str:
    for line in (s or "").splitlines():
        line = line.strip()
        if line:
            return line
    return ""


class Transcript:
    """State derived from one transcript file (a main session or a subagent).

    feed() is called for every record in file order. Events are queued in self.events as
    (kind, fields) tuples, only for records whose timestamp is >= self.emit_after (so the history read at
    startup doesn't flood the device).
    """

    def __init__(self, path: str, sid: str, is_sub: bool = False, emit_after: float = 0.0):
        self.path = path
        self.sid = sid                    # full session id (for subagents: the parent's)
        self.is_sub = is_sub
        self.agent_id = ""                # subagents: full agent id
        self.tail = Tail(path)
        self.emit_after = emit_after
        self.events: list[tuple[str, dict]] = []
        self.cwd = ""
        self.branch = ""
        self.title = ""
        self.custom_title = ""
        self.last_prompt = ""
        self.last_prompt_ts = 0.0
        self.fallback_prompt = ""
        self.model = ""
        self.first_ts = 0.0
        self.last_ts = 0.0                # timestamp of the newest record seen
        self.last_activity_ts = 0.0       # newest assistant/user(prompt/tool_result) record
        self.tokens = [0, 0, 0, 0]        # in, out, cache_read, cache_write
        self.cost = 0.0
        self.ctx_tokens = 0
        self.ctx_model = ""
        self.tool_count = 0
        self.activity = {"tool": "", "detail": "", "ts": 0}
        self.last_text = ""
        self.turn_open = False            # a prompt/tool loop is in progress (no end_turn yet)
        self.turn_text = ""               # newest assistant text of the current turn
        self.stop_pending = None          # (msg id, ts, observed wall time): end_turn seen, stop not emitted
        self.turn_end_ts = 0.0
        self.pending: OrderedDict[str, tuple] = OrderedDict()   # tool_use_id -> (name, detail, ts)
        self.todos: list[dict] = []
        self.task_ids: dict[str, str] = {}  # TaskCreate tool_use_id -> todo id
        self.perm_mode = ""
        self.compactions = 0
        self.handback = False             # subagent called SubagentHandback
        self.done_agents: dict[str, float] = {}      # (main) agent ids reported finished -> ts
        self.agent_tool_done: dict[str, float] = {}  # (main) Agent tool_use ids with a final (sync) result
        self._msgs: OrderedDict[str, tuple] = OrderedDict()
        self._stopped_msgs: set[str] = set()
        # local-day counters (records timestamped today, local time)
        self.day = ""
        self.today = [0, 0.0, 0, 0]       # tokens (in+out+cache_write), cost, tools, prompts

    # ------------------------------------------------------------------ io
    def poll(self, budget_bytes: int = 4 << 20, now: float | None = None) -> int:
        """Read & parse new complete lines. Returns bytes consumed."""
        if not self.tail.stat():
            return 0
        start = self.tail.offset
        while self.tail.pending and self.tail.offset - start < budget_bytes:
            lines = self.tail.read_lines(min(4 << 20, budget_bytes))
            if not lines:
                break
            for line in lines:
                self.feed_line(line)
        return self.tail.offset - start

    def feed_line(self, line: bytes) -> None:
        if not line:
            return
        if line.startswith(b'{"type":"'):
            e = line.find(b'"', 9)
            if line[9:e] in _SKIP_TYPES:
                return
        try:
            r = json.loads(line)
        except Exception:
            return
        if isinstance(r, dict):
            try:
                self.feed(r)
            except Exception:
                pass  # never let one odd record break the stream

    # ------------------------------------------------------------------ helpers
    def _emit(self, ts: float, kind: str, **kw) -> None:
        if ts and ts >= self.emit_after:
            ev = {k: v for k, v in kw.items() if v not in (None, "")}
            ev["ts"] = int(ts)
            self.events.append((kind, ev))

    def _day_bucket(self, ts: float):
        if not ts:
            return None
        d = time.strftime("%Y-%m-%d", time.localtime(ts))
        today = time.strftime("%Y-%m-%d")
        if d != today:
            return None
        if self.day != today:
            self.day = today
            self.today = [0, 0.0, 0, 0]
        return self.today

    def todos_summary(self) -> dict | None:
        if not self.todos:
            return None
        done = sum(1 for t in self.todos if t.get("status") == "completed")
        cur = next((t for t in self.todos if t.get("status") == "in_progress"), None)
        if cur is None:
            cur = next((t for t in self.todos if t.get("status") != "completed"), None)
        txt = ""
        if cur:
            txt = cur.get("activeForm") if cur.get("status") == "in_progress" and cur.get("activeForm") \
                else cur.get("content")
        return {"done": done, "total": len(self.todos), "current": ascii_text(txt or "", 60)}

    def ctx_pct(self) -> int:
        if not self.ctx_tokens:
            return 0
        return max(0, min(100, int(round(100.0 * self.ctx_tokens / context_window(self.ctx_model or self.model)))))

    def pending_ask(self):
        """(tool, detail) of a pending AskUserQuestion/ExitPlanMode, else None."""
        for name, detail, _ts in reversed(self.pending.values()):
            if name in ASK_TOOLS:
                return name, detail
        return None

    def last_pending(self):
        if not self.pending:
            return None
        return next(reversed(self.pending.values()))

    # ------------------------------------------------------------------ record handling
    def feed(self, r: dict) -> None:
        t = r.get("type")
        if t == "assistant":
            self._assistant(r)
        elif t == "user":
            self._user(r)
        elif t == "system":
            self._system(r)
        elif t == "ai-title":
            self.title = r.get("aiTitle") or self.title
        elif t == "custom-title":
            self.custom_title = r.get("customTitle") or self.custom_title
        elif t == "summary":
            if not self.title and r.get("summary"):
                self.title = r["summary"]
        elif t == "last-prompt":
            self.fallback_prompt = r.get("lastPrompt") or self.fallback_prompt
        elif t == "permission-mode":
            self.perm_mode = r.get("permissionMode") or self.perm_mode
        elif t == "queue-operation":
            c = r.get("content")
            if isinstance(c, str) and "<task-id>" in c:
                self._task_notes(c, parse_ts(r.get("timestamp")))

    def _stamp(self, r: dict) -> float:
        ts = parse_ts(r.get("timestamp"))
        if ts:
            if not self.first_ts:
                self.first_ts = ts
            if ts > self.last_ts:
                self.last_ts = ts
        if r.get("cwd") and not self.cwd:
            self.cwd = r["cwd"]
        if r.get("gitBranch"):
            self.branch = r["gitBranch"]
        if self.is_sub and r.get("agentId") and not self.agent_id:
            self.agent_id = r["agentId"]
        return ts

    def _task_notes(self, text: str, ts: float) -> None:
        for aid, status in _TASK_NOTE.findall(text):
            if status.lower() in ("completed", "failed", "killed", "stopped", "cancelled", "canceled", "error"):
                self.done_agents.setdefault(aid, ts or time.time())

    def _assistant(self, r: dict) -> None:
        ts = self._stamp(r)
        m = r.get("message") or {}
        model = m.get("model") or ""
        if model and not model.startswith("<"):
            self.model = model
        self.last_activity_ts = max(self.last_activity_ts, ts)
        mid = m.get("id") or r.get("requestId") or r.get("uuid") or ""
        if self.stop_pending and self.stop_pending[0] != mid:
            self.flush_stop()
        if mid not in self._stopped_msgs:
            self.turn_open = True
        u = m.get("usage")
        if isinstance(u, dict) and model and not model.startswith("<"):
            self._usage(mid, model, u, ts)
        for b in m.get("content") or []:
            if not isinstance(b, dict):
                continue
            bt = b.get("type")
            if bt == "tool_use":
                self._tool_use(b, ts)
            elif bt == "text":
                txt = (b.get("text") or "").strip()
                if txt:
                    self.last_text = txt
                    self.turn_text = txt
                    self.activity = {"tool": "writing", "detail": ascii_text(plain(_first_line(txt)), 80), "ts": int(ts)}
            elif bt in ("thinking", "redacted_thinking"):
                self.activity = {"tool": "thinking", "detail": "", "ts": int(ts)}
        if m.get("stop_reason") == "end_turn" and mid not in self._stopped_msgs:
            self._stopped_msgs.add(mid)
            if len(self._stopped_msgs) > 256:
                self._stopped_msgs = set(list(self._stopped_msgs)[-64:])
            # the message's remaining content blocks (e.g. the final text after a thinking block) arrive as
            # further lines with the same id: close the turn now, emit `stop` once the message is complete
            self.turn_open = False
            self.turn_end_ts = ts
            self.pending.clear()
            self.stop_pending = (mid, ts, time.time())

    def flush_stop(self, now: float | None = None, grace: float = 0.0) -> None:
        """Emit the deferred `stop` (called on the next record, or by the provider after `grace` s)."""
        sp = self.stop_pending
        if not sp or (now is not None and now - sp[2] < grace):
            return
        self.stop_pending = None
        if not self.is_sub:
            self._emit(sp[1], "stop", text=ascii_text(plain(_first_line(self.turn_text)), 80))

    def _turn_end(self, ts: float) -> None:
        """End of turn without (or after) an end_turn message (system turn_duration)."""
        if self.stop_pending:
            self.flush_stop()
            return
        if not self.turn_open:
            return
        self.turn_open = False
        self.turn_end_ts = ts
        self.pending.clear()     # interrupted / never-answered tool calls
        if not self.is_sub:
            self._emit(ts, "stop", text=ascii_text(plain(_first_line(self.turn_text)), 80))

    def _usage(self, mid: str, model: str, u: dict, ts: float) -> None:
        i = int(u.get("input_tokens") or 0)
        o = int(u.get("output_tokens") or 0)
        cr = int(u.get("cache_read_input_tokens") or 0)
        cw = int(u.get("cache_creation_input_tokens") or 0)
        prev = self._msgs.get(mid)
        if prev:
            d = [max(0, a - b) for a, b in zip((i, o, cr, cw), prev)]
            if not any(d):
                return
            self._msgs[mid] = tuple(max(a, b) for a, b in zip((i, o, cr, cw), prev))
        else:
            d = [i, o, cr, cw]
            self._msgs[mid] = (i, o, cr, cw)
            if len(self._msgs) > 64:
                self._msgs.popitem(last=False)
            # context size of this request (first time we see the message)
            self.ctx_tokens = i + cr + cw
            self.ctx_model = model
        cc = u.get("cache_creation") or {}
        cw1h = int(cc.get("ephemeral_1h_input_tokens") or 0) if isinstance(cc, dict) else 0
        cost = usage_cost(model, d[0], d[1], d[2], d[3], min(cw1h, d[3]) if prev else cw1h,
                          u.get("speed") == "fast")
        for k in range(4):
            self.tokens[k] += d[k]
        self.cost += cost
        b = self._day_bucket(ts)
        if b is not None:
            b[0] += d[0] + d[1] + d[3]
            b[1] += cost

    def _tool_use(self, b: dict, ts: float) -> None:
        name = b.get("name") or ""
        inp = b.get("input") if isinstance(b.get("input"), dict) else {}
        detail = tool_detail(name, inp, self.cwd)
        tid = b.get("id") or ""
        if tid in self.pending:
            return
        self.pending[tid] = (name, detail, ts)
        if len(self.pending) > 64:
            self.pending.popitem(last=False)
        self.tool_count += 1
        bk = self._day_bucket(ts)
        if bk is not None:
            bk[2] += 1
        self.activity = {"tool": tool_label(name), "detail": detail, "ts": int(ts)}
        ev = {"tool": tool_label(name), "text": detail}
        if self.is_sub:
            ev["agent"] = (self.agent_id or "")[:8]
        self._emit(ts, "tool", **ev)
        if name == "SubagentHandback":
            self.handback = True
        if name == "TodoWrite":
            self._todowrite(inp, ts)
        elif name == "TaskCreate":
            todo_id = str(len(self.todos) + 1)
            self.task_ids[tid] = todo_id
            self.todos.append({"id": todo_id, "content": inp.get("subject") or inp.get("description") or "",
                               "activeForm": inp.get("activeForm") or "", "status": "pending"})
        elif name == "TaskUpdate":
            self._taskupdate(inp, ts)

    def _todowrite(self, inp: dict, ts: float) -> None:
        todos = inp.get("todos")
        if not isinstance(todos, list):
            return
        old = {t.get("content"): t.get("status") for t in self.todos}
        new = []
        for t in todos:
            if not isinstance(t, dict):
                continue
            new.append({"content": t.get("content") or "", "activeForm": t.get("activeForm") or "",
                        "status": t.get("status") or "pending"})
        for t in new:
            if t["status"] == "completed" and old.get(t["content"]) not in (None, "completed"):
                self._emit(ts, "todo_done", text=ascii_text(t["content"], 80))
        self.todos = new

    def _taskupdate(self, inp: dict, ts: float) -> None:
        tid = str(inp.get("taskId") or inp.get("id") or "")
        t = next((x for x in self.todos if x.get("id") == tid), None)
        if t is None:
            return
        st = inp.get("status")
        if st == "deleted":
            self.todos.remove(t)
            return
        for k_in, k in (("subject", "content"), ("activeForm", "activeForm")):
            if inp.get(k_in):
                t[k] = inp[k_in]
        if st:
            if st == "completed" and t.get("status") != "completed":
                self._emit(ts, "todo_done", text=ascii_text(t["content"], 80))
            t["status"] = st

    def _user(self, r: dict) -> None:
        ts = self._stamp(r)
        if self.stop_pending:
            self.flush_stop()
        m = r.get("message") or {}
        c = m.get("content")
        origin = r.get("origin") if isinstance(r.get("origin"), dict) else {}
        okind = origin.get("kind")
        if not (isinstance(c, list) and any(isinstance(b, dict) and b.get("type") == "tool_result" for b in c)):
            self.turn_text = ""          # a new turn starts (prompt, notification, hand-back...)
        if okind == "peer":
            if origin.get("handback") and origin.get("from"):
                self.done_agents.setdefault(str(origin["from"]), ts or time.time())
            return
        if isinstance(c, list):
            had_result = False
            for b in c:
                if isinstance(b, dict) and b.get("type") == "tool_result":
                    had_result = True
                    self._tool_result(b, r, ts)
            if had_result:
                self.last_activity_ts = max(self.last_activity_ts, ts)
                self.turn_open = True
                return
        text = _content_text(c)
        if okind == "task-notification" or "<task-notification>" in text[:200]:
            self._task_notes(text, ts)
            return
        if r.get("isMeta") or r.get("isCompactSummary") or r.get("isVisibleInTranscriptOnly"):
            return
        human = okind == "human" or (not okind and not r.get("toolUseResult"))
        stripped = text.strip()
        if not human or not stripped or stripped.startswith(_NON_PROMPT_PREFIXES):
            return
        if self.is_sub:
            # the subagent's task prompt
            if not self.last_prompt:
                self.last_prompt = ascii_text(stripped, 120)
            self.turn_open = True
            self.last_activity_ts = max(self.last_activity_ts, ts)
            return
        self.last_prompt = ascii_text(stripped, 120)
        self.last_prompt_ts = ts
        self.pending.clear()
        self.last_activity_ts = max(self.last_activity_ts, ts)
        self.turn_open = True
        self.activity = {"tool": "thinking", "detail": "", "ts": int(ts)}
        bk = self._day_bucket(ts)
        if bk is not None:
            bk[3] += 1
        self._emit(ts, "prompt", text=self.last_prompt)

    def _tool_result(self, b: dict, r: dict, ts: float) -> None:
        tid = b.get("tool_use_id") or ""
        name, detail, _t0 = self.pending.pop(tid, ("", "", 0))
        res = r.get("toolUseResult")
        if name == "TaskCreate" and isinstance(res, dict):
            task = res.get("task") if isinstance(res.get("task"), dict) else res
            real = task.get("id") if isinstance(task, dict) else None
            mine = self.task_ids.pop(tid, None)
            if real is not None and mine is not None:
                for t in self.todos:
                    if t.get("id") == mine:
                        t["id"] = str(real)
        if name in AGENT_TOOLS:
            txt = _content_text(b.get("content"))
            if "launched" not in txt[:200].lower() and not (isinstance(res, dict) and res.get("isAsync")):
                self.agent_tool_done[tid] = ts or time.time()
        if b.get("is_error"):
            err = _first_line(_content_text(b.get("content")) if not isinstance(b.get("content"), str)
                              else b.get("content"))
            err = re.sub(r"</?tool_use_error>", "", err)
            ev = {"tool": tool_label(name), "text": detail or ascii_text(err, 80), "error": ascii_text(err, 80)}
            if self.is_sub:
                ev["agent"] = (self.agent_id or "")[:8]
            self._emit(ts, "tool_error", **ev)

    def _system(self, r: dict) -> None:
        ts = self._stamp(r)
        st = r.get("subtype")
        if st == "compact_boundary":
            self.compactions += 1
            meta = r.get("compactMetadata") or r.get("compact_metadata") or {}
            post = meta.get("postTokens") or meta.get("post_tokens") if isinstance(meta, dict) else None
            self.ctx_tokens = int(post or 0)
            if not self.is_sub:
                trig = meta.get("trigger") if isinstance(meta, dict) else None
                self._emit(ts, "compact", text=trig or "")
        elif st == "turn_duration":
            self._turn_end(ts)
