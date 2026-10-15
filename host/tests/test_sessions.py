"""Tests for companion/providers/sessions.py (+ sessions_text / sessions_transcript), on synthetic fixtures.

Run:  host/.venv/bin/python -m unittest host/tests/test_sessions.py -v
      (or: host/.venv/bin/python host/tests/test_sessions.py)
"""
from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import time
import unittest
from datetime import datetime, timezone

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from companion.providers.sessions import SessionsProvider, cwd_slug  # noqa: E402
from companion.sessions_text import (ascii_text, context_window, model_label, tool_detail,  # noqa: E402
                                     usage_cost)

SID = "11111111-2222-3333-4444-555555555555"
SID2 = "99999999-8888-7777-6666-555555555555"
CWD = "/Users/test/projects/demo-proj"


def iso(ts: float) -> str:
    return datetime.fromtimestamp(ts, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def user_prompt(text, ts):
    return {"type": "user", "isSidechain": False, "promptSource": "typed", "origin": {"kind": "human"},
            "message": {"role": "user", "content": text}, "timestamp": iso(ts), "cwd": CWD, "sessionId": SID,
            "gitBranch": "main"}


def assistant(mid, blocks, ts, stop="tool_use", model="claude-opus-5-5", usage=None, side=False, aid=None):
    u = usage or {"input_tokens": 10, "output_tokens": 100, "cache_read_input_tokens": 1000,
                  "cache_creation_input_tokens": 500,
                  "cache_creation": {"ephemeral_1h_input_tokens": 500, "ephemeral_5m_input_tokens": 0}}
    r = {"type": "assistant", "isSidechain": side, "message": {"model": model, "id": mid, "type": "message",
         "role": "assistant", "content": blocks, "stop_reason": stop, "usage": u},
         "timestamp": iso(ts), "cwd": CWD, "sessionId": SID}
    if aid:
        r["agentId"] = aid
    return r


def tool_use(tid, name, inp):
    return {"type": "tool_use", "id": tid, "name": name, "input": inp}


def tool_result(tid, ts, content="ok", is_error=False, aid=None, res=None):
    r = {"type": "user", "message": {"role": "user", "content": [
        {"tool_use_id": tid, "type": "tool_result", "content": content, "is_error": is_error}]},
        "timestamp": iso(ts), "cwd": CWD, "sessionId": SID}
    if aid:
        r["agentId"] = aid
    if res is not None:
        r["toolUseResult"] = res
    return r


class Fixture:
    def __init__(self):
        self.root = tempfile.mkdtemp(prefix="cm-sessions-test-")
        os.makedirs(os.path.join(self.root, "sessions"))
        self.pdir = os.path.join(self.root, "projects", cwd_slug(CWD))
        os.makedirs(self.pdir)
        self.tpath = os.path.join(self.pdir, SID + ".jsonl")

    def registry(self, status="busy", pid=None, sid=SID, waiting_for=None, upd=None, name="demo-proj-3"):
        pid = pid or os.getpid()
        upd = upd or time.time()
        d = {"pid": pid, "sessionId": sid, "cwd": CWD, "startedAt": int((upd - 100) * 1000), "kind": "interactive",
             "entrypoint": "cli", "name": name, "nameSource": "derived", "status": status,
             "updatedAt": int(upd * 1000), "statusUpdatedAt": int(upd * 1000), "version": "2.1.300"}
        if waiting_for:
            d["waitingFor"] = waiting_for
        p = os.path.join(self.root, "sessions", f"{pid}.json")
        with open(p, "w") as f:
            json.dump(d, f)
        return p

    def append(self, path, *records, partial: str = ""):
        with open(path, "a") as f:
            for r in records:
                f.write(json.dumps(r) + "\n")
            if partial:
                f.write(partial)

    def cleanup(self):
        shutil.rmtree(self.root, ignore_errors=True)


def poll(p, n=2):
    for _ in range(n):
        p.poll(time.time())
    st = {}
    p.contribute(st)
    return st, p.drain_events()


class TextTests(unittest.TestCase):
    def test_ascii(self):
        self.assertEqual(ascii_text("café — “ok” → \U0001F389 done…"),
                         'cafe - "ok" -> done...')
        self.assertEqual(ascii_text("a" * 50, 10), "aaaaaaa...")
        self.assertTrue(all(32 <= ord(c) < 127 for c in ascii_text("日本 abc ß")))

    def test_model(self):
        self.assertEqual(model_label("claude-opus-5-5"), "opus-5.5")
        self.assertEqual(model_label("claude-haiku-4-5-20251001"), "haiku-4.5")
        self.assertEqual(model_label("claude-fable-5-1"), "fable-5.1")
        self.assertEqual(model_label("claude-opus-5"), "opus-5")
        self.assertEqual(model_label("<synthetic>"), "")
        self.assertEqual(context_window("claude-opus-5-5"), 1_000_000)
        self.assertEqual(context_window("claude-haiku-4-5-20251001"), 200_000)
        self.assertEqual(context_window("claude-sonnet-4-5"), 200_000)
        self.assertEqual(context_window("claude-sonnet-4-5[1m]"), 1_000_000)

    def test_cost_matches_claude_code(self):
        # numbers from real `cost-state` records written by Claude Code
        self.assertAlmostEqual(usage_cost("claude-opus-5-5", 104, 2554, 262600, 36126, 36126), 0.393024, 4)
        self.assertAlmostEqual(usage_cost("claude-fable-5-1", 34, 482, 82476, 25785, 25785), 0.560759, 4)
        self.assertAlmostEqual(usage_cost("claude-haiku-4-5-20251001", 902, 21, 0, 0), 0.001007, 6)

    def test_details(self):
        self.assertEqual(tool_detail("Read", {"file_path": CWD + "/src/main.cpp"}, CWD), "src/main.cpp")
        self.assertEqual(tool_detail("WebFetch", {"url": "https://www.waveshare.com/wiki/X?a=1"}),
                         "waveshare.com/wiki/X")
        self.assertEqual(tool_detail("Grep", {"pattern": "TODO", "glob": "*.py"}), '"TODO" in *.py')
        self.assertEqual(tool_detail("Agent", {"description": "Explore auth", "prompt": "..."}), "Explore auth")
        self.assertEqual(tool_detail("WebSearch", {"query": "lvgl tileview"}), "lvgl tileview")
        long_cmd = "pio run " + "-v " * 60
        self.assertLessEqual(len(tool_detail("Bash", {"command": long_cmd})), 80)
        self.assertEqual(tool_detail("Bash", {"command": long_cmd, "description": "Build fw"}), "Build fw")


class ProviderTests(unittest.TestCase):
    def setUp(self):
        self.fx = Fixture()

    def tearDown(self):
        self.fx.cleanup()

    def test_history_then_live_events(self):
        fx = self.fx
        old = time.time() - 3600
        fx.append(fx.tpath,
                  {"type": "permission-mode", "permissionMode": "auto", "sessionId": SID},
                  user_prompt("Old prompt — fix things", old),
                  assistant("m1", [{"type": "thinking", "thinking": ""}], old + 1),
                  assistant("m1", [tool_use("t1", "Read", {"file_path": CWD + "/a.py"})], old + 1),
                  tool_result("t1", old + 2),
                  assistant("m2", [{"type": "text", "text": "All **done**."}], old + 3, stop="end_turn"),
                  {"type": "ai-title", "aiTitle": "Fix things ✨", "sessionId": SID})
        fx.registry("idle", upd=old + 3)
        p = SessionsProvider(base=fx.root)
        st, ev = poll(p)
        self.assertEqual(ev, [], "no events for history")
        s = st["sessions"][0]
        self.assertEqual(s["id"], SID[:8])
        self.assertEqual(s["name"], "demo-proj")
        self.assertEqual(s["project"], "demo-proj")
        self.assertEqual(s["title"], "Fix things")
        self.assertEqual(s["status"], "idle")
        self.assertEqual(s["since"], int(old + 3))
        self.assertEqual(s["model"], "opus-5.5")
        self.assertEqual(s["last_prompt"], "Old prompt - fix things")
        self.assertEqual(s["tool_count"], 1)
        self.assertEqual(s["perm_mode"], "auto")
        # usage counted once per message id (m1 appears twice)
        self.assertEqual(s["tokens"], {"in": 20, "out": 200, "cache_read": 2000, "cache_write": 1000})
        self.assertEqual(s["ctx_pct"], 0)  # 1510 / 1M
        self.assertEqual(s["activity"]["tool"], "writing")
        self.assertEqual(s["activity"]["detail"], "All done.")

        # --- live activity
        now = time.time()
        fx.registry("busy", upd=now)
        fx.append(fx.tpath, user_prompt("Run the tests please", now),
                  assistant("m3", [tool_use("t2", "Bash", {"command": "pytest -x"})], now + 0.1),
                  tool_result("t2", now + 0.2, content="Exit code 1\nFAILED", is_error=True),
                  assistant("m4", [tool_use("t3", "TodoWrite", {"todos": [
                      {"content": "Fix test", "activeForm": "Fixing test", "status": "in_progress"},
                      {"content": "Ship", "activeForm": "Shipping", "status": "pending"}]})], now + 0.3))
        st, ev = poll(p)
        kinds = [e["kind"] for e in ev]
        self.assertEqual(kinds, ["prompt", "tool", "tool_error", "tool"])
        self.assertEqual(ev[0]["text"], "Run the tests please")
        self.assertEqual(ev[1]["tool"], "Bash")
        self.assertEqual(ev[1]["text"], "pytest -x")
        self.assertEqual(ev[2]["error"], "Exit code 1")
        s = st["sessions"][0]
        self.assertEqual(s["status"], "busy")
        self.assertEqual(s["todos"], {"done": 0, "total": 2, "current": "Fixing test"})
        self.assertEqual(st["totals"]["busy"], 1)

        # --- todo done + waiting (permission prompt) with pending tool
        now = time.time()
        fx.append(fx.tpath, tool_result("t3", now),
                  assistant("m5", [tool_use("t4", "TodoWrite", {"todos": [
                      {"content": "Fix test", "activeForm": "Fixing test", "status": "completed"},
                      {"content": "Ship", "activeForm": "Shipping", "status": "in_progress"}]})], now + 0.1),
                  tool_result("t4", now + 0.15),
                  assistant("m6", [tool_use("t5", "Bash", {"command": "git push"})], now + 0.2))
        fx.registry("waiting", waiting_for="permission prompt", upd=now + 0.3)
        st, ev = poll(p)
        kinds = [e["kind"] for e in ev]
        self.assertIn("todo_done", kinds)
        self.assertIn("waiting", kinds)
        w = next(e for e in ev if e["kind"] == "waiting")
        self.assertEqual(w["text"], "Allow Bash: git push?")
        s = st["sessions"][0]
        self.assertEqual(s["status"], "waiting")
        self.assertEqual(s["waiting"], "Allow Bash: git push?")
        self.assertEqual(s["todos"]["done"], 1)
        self.assertEqual(st["totals"]["waiting"], 1)

        # --- stop
        now = time.time()
        fx.append(fx.tpath, tool_result("t5", now),
                  assistant("m7", [{"type": "text", "text": "Pushed. \U0001F680"}], now + 0.1, stop="end_turn"),
                  {"type": "system", "subtype": "turn_duration", "durationMs": 1000, "timestamp": iso(now + 0.2)})
        fx.registry("idle", upd=now + 0.2)
        st, ev = poll(p)
        stops = [e for e in ev if e["kind"] == "stop"]
        self.assertEqual(len(stops), 1)
        self.assertEqual(stops[0]["text"], "Pushed.")
        self.assertEqual(st["sessions"][0]["status"], "idle")

        # --- compaction
        now = time.time()
        fx.append(fx.tpath, {"type": "system", "subtype": "compact_boundary", "timestamp": iso(now),
                             "compactMetadata": {"trigger": "auto", "preTokens": 900000, "postTokens": 30000}})
        st, ev = poll(p)
        self.assertEqual([e["kind"] for e in ev], ["compact"])
        self.assertEqual(st["sessions"][0]["ctx_pct"], 3)

        # --- exit: registry file removed -> offline + session_end
        os.remove(os.path.join(fx.root, "sessions", f"{os.getpid()}.json"))
        st, ev = poll(p)
        self.assertEqual([e["kind"] for e in ev], ["session_end"])
        self.assertEqual(st["sessions"][0]["status"], "offline")
        self.assertEqual(st["totals"]["sessions"], 0)

    def test_partial_lines_and_usage_growth(self):
        fx = self.fx
        fx.registry("busy")
        p = SessionsProvider(base=fx.root)
        poll(p)
        now = time.time()
        rec = json.dumps(assistant("mx", [tool_use("tx", "Grep", {"pattern": "foo"})], now)) + "\n"
        fx.append(fx.tpath, partial=rec[:40])
        st, ev = poll(p)
        self.assertEqual(ev, [])
        with open(fx.tpath, "a") as f:
            f.write(rec[40:])
        st, ev = poll(p)
        self.assertEqual([e["kind"] for e in ev], ["tool"])
        self.assertEqual(ev[0]["text"], '"foo"')
        # same message id, more output tokens -> only the delta is added
        u = {"input_tokens": 10, "output_tokens": 300, "cache_read_input_tokens": 1000,
             "cache_creation_input_tokens": 500}
        fx.append(fx.tpath, assistant("mx", [{"type": "text", "text": "x"}], now + 1, usage=u))
        st, _ = poll(p)
        self.assertEqual(st["sessions"][0]["tokens"]["out"], 300)
        self.assertEqual(st["sessions"][0]["tokens"]["cache_read"], 1000)

    def test_subagents(self):
        fx = self.fx
        fx.registry("busy")
        fx.append(fx.tpath, user_prompt("go", time.time() - 10))
        p = SessionsProvider(base=fx.root)
        poll(p)
        sdir = os.path.join(fx.pdir, SID, "subagents")
        os.makedirs(sdir)
        aid = "a16e14b797dc90724"
        apath = os.path.join(sdir, f"agent-{aid}.jsonl")
        with open(os.path.join(sdir, f"agent-{aid}.meta.json"), "w") as f:
            json.dump({"agentType": "Explore", "description": "Research pinout", "toolUseId": "tA"}, f)
        now = time.time()
        fx.append(fx.tpath, assistant("p1", [tool_use("tA", "Agent", {"description": "Research pinout",
                                                                       "prompt": "..."})], now))
        fx.append(apath, {"type": "user", "isSidechain": True, "agentId": aid, "timestamp": iso(now + 0.1),
                          "message": {"role": "user", "content": "Research the pinout"}},
                  assistant("s1", [tool_use("s1t", "WebFetch", {"url": "https://example.com/a/b"})], now + 0.2,
                            side=True, aid=aid))
        time.sleep(1.1)  # subagent dir scan interval
        st, ev = poll(p)
        kinds = [(e["kind"], e.get("agent")) for e in ev]
        self.assertIn(("tool", None), kinds)
        self.assertIn(("subagent_start", aid[:8]), kinds)
        self.assertIn(("tool", aid[:8]), kinds)
        sub = st["sessions"][0]["subagents"][0]
        self.assertEqual(sub["id"], aid[:8])
        self.assertEqual(sub["desc"], "Research pinout")
        self.assertEqual(sub["type"], "Explore")
        self.assertEqual(sub["status"], "busy")
        self.assertEqual(sub["tool"], "WebFetch")
        self.assertEqual(sub["detail"], "example.com/a/b")
        self.assertEqual(st["totals"]["subagents_active"], 1)
        # session tokens include the subagent's
        self.assertEqual(st["sessions"][0]["tokens"]["out"], 200)
        # hand-back -> done
        now = time.time()
        fx.append(apath, assistant("s2", [tool_use("s2t", "SubagentHandback", {"message": "report"})], now,
                                   side=True, aid=aid),
                  tool_result("s2t", now + 0.1, aid=aid))
        st, ev = poll(p)
        self.assertIn(("subagent_stop", aid[:8]), [(e["kind"], e.get("agent")) for e in ev])
        self.assertEqual(st["sessions"][0]["subagents"][0]["status"], "done")
        self.assertEqual(st["totals"]["subagents_active"], 0)

    def test_task_tools_todos(self):
        fx = self.fx
        fx.registry("busy")
        p = SessionsProvider(base=fx.root)
        poll(p)
        now = time.time()
        fx.append(fx.tpath,
                  assistant("a", [tool_use("c1", "TaskCreate", {"subject": "Read code", "activeForm": "Reading"})], now),
                  tool_result("c1", now, res={"task": {"id": "1", "subject": "Read code"}}),
                  assistant("b", [tool_use("c2", "TaskCreate", {"subject": "Write code"})], now),
                  tool_result("c2", now, res={"task": {"id": "2"}}),
                  assistant("c", [tool_use("c3", "TaskUpdate", {"taskId": "1", "status": "in_progress"})], now),
                  assistant("d", [tool_use("c4", "TaskUpdate", {"taskId": "1", "status": "completed"})], now),
                  assistant("e", [tool_use("c5", "TaskUpdate", {"taskId": "2", "status": "in_progress",
                                                                 "activeForm": "Writing code"})], now))
        st, ev = poll(p)
        self.assertEqual(st["sessions"][0]["todos"], {"done": 1, "total": 2, "current": "Writing code"})
        self.assertEqual([e["text"] for e in ev if e["kind"] == "todo_done"], ["Read code"])

    def test_dead_pid_and_headless(self):
        fx = self.fx
        fx.registry("busy", pid=999999)   # not alive
        p = SessionsProvider(base=fx.root)
        st, ev = poll(p)
        self.assertEqual(st["sessions"], [])
        # a transcript growing with no registry entry (e.g. SDK child session) -> headless session
        tp = os.path.join(fx.pdir, SID2 + ".jsonl")
        time.sleep(0.05)
        now = time.time()
        fx.append(tp, user_prompt("headless job", now),
                  assistant("h1", [tool_use("h", "Read", {"file_path": CWD + "/x.md"})], now + 0.1))
        p.last_scan = 0
        st, ev = poll(p)
        ids = [s["id"] for s in st["sessions"]]
        self.assertEqual(ids, [SID2[:8]])
        self.assertEqual(st["sessions"][0]["kind"], "headless")
        self.assertEqual(st["sessions"][0]["status"], "busy")
        self.assertIn("session_start", [e["kind"] for e in ev])

    def test_waiting_before_transcript_flush(self):
        fx = self.fx
        fx.registry("busy")
        fx.append(fx.tpath, user_prompt("deploy", time.time() - 5))
        p = SessionsProvider(base=fx.root)
        poll(p)
        fx.registry("waiting", waiting_for="permission prompt")
        st, ev = poll(p, 1)
        self.assertEqual(st["sessions"][0]["status"], "waiting")
        self.assertEqual(ev, [], "waits a moment for the pending tool_use to show up")
        fx.append(fx.tpath, assistant("w", [tool_use("w1", "Bash", {"command": "npm run deploy"})], time.time()))
        st, ev = poll(p, 1)
        self.assertEqual([(e["kind"], e["text"]) for e in ev if e["kind"] == "waiting"],
                         [("waiting", "Allow Bash: npm run deploy?")])
        st, ev = poll(p, 1)
        self.assertEqual(ev, [], "only once")

    def test_stop_waits_for_last_block(self):
        fx = self.fx
        fx.registry("busy")
        p = SessionsProvider(base=fx.root)
        poll(p)
        now = time.time()
        fx.append(fx.tpath, user_prompt("hi", now),
                  assistant("z", [{"type": "thinking", "thinking": ""}], now + 0.1, stop="end_turn"))
        st, ev = poll(p)
        self.assertEqual([e["kind"] for e in ev], ["prompt"])
        fx.append(fx.tpath, assistant("z", [{"type": "text", "text": "Hello there"}], now + 0.2, stop="end_turn"))
        st, ev = poll(p)
        self.assertEqual(ev, [])
        time.sleep(1.6)
        st, ev = poll(p)
        self.assertEqual([(e["kind"], e["text"]) for e in ev], [("stop", "Hello there")])

    def test_clear_replaces_session(self):
        fx = self.fx
        fx.registry("idle")
        p = SessionsProvider(base=fx.root)
        st, _ = poll(p)
        self.assertEqual([s["id"] for s in st["sessions"]], [SID[:8]])
        time.sleep(0.02)
        fx.registry("idle", sid=SID2)      # same pid, new session id (/clear)
        st, ev = poll(p)
        self.assertEqual([s["id"] for s in st["sessions"]], [SID2[:8]])
        self.assertEqual([e["kind"] for e in ev], ["session_start"])

    def test_ask_user_question_without_registry_status(self):
        fx = self.fx
        fx.registry(status=None)
        p = SessionsProvider(base=fx.root)
        poll(p)
        now = time.time()
        fx.append(fx.tpath, user_prompt("plan it", now),
                  assistant("q", [tool_use("q1", "AskUserQuestion",
                                           {"questions": [{"question": "Which DB?", "header": "DB"}]})], now))
        st, ev = poll(p)
        self.assertEqual(st["sessions"][0]["status"], "waiting")
        self.assertEqual(st["sessions"][0]["waiting"], "Question: Which DB?")
        self.assertIn("waiting", [e["kind"] for e in ev])


if __name__ == "__main__":
    unittest.main(verbosity=2)
