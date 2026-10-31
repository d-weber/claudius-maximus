"""Fake but plausible data so the firmware can be developed without real agents running.

Simulates several sessions cycling busy → waiting → idle, with tool calls, subagents and todos.
Deterministic for a given seed so screenshots are reproducible.
Text and extra fields mirror what providers/sessions.py produces from real Claude Code data
(e.g. `waiting` text, `kind`, `perm_mode`, subagent `model`/`cost`, tool_error `error`).
"""
from __future__ import annotations

import random
import time

from .base import Provider

PROJECTS = [
    ("api-gateway", "Add rate limiting middleware"),
    ("web-dashboard", "Fix flaky chart tests"),
    ("iot-fleet", "Greengrass deploy pipeline"),
    ("claudius-maximus", "ESP32 companion display"),
    ("ml-pipeline", "Speed up feature extraction"),
]
TOOLS = [
    ("Read", ["src/server.ts", "README.md", "package.json", "src/routes/auth.ts"]),
    ("Edit", ["src/middleware/ratelimit.ts", "tests/chart.spec.ts", "src/main.cpp"]),
    ("Bash", ["npm test", "pio run -t upload", "git status", "pytest -x tests/", "Build firmware and flash"]),
    ("Grep", ["\"TODO\" in src/", "\"useEffect\" in *.tsx", "\"class Provider\""]),
    ("WebSearch", ["ST7701 init sequence", "LVGL 8 tileview"]),
    ("WebFetch", ["docs.lvgl.io/8.4/widgets/tileview.html", "waveshare.com/wiki/ESP32-S3-Touch-LCD-4B"]),
    ("Write", ["src/fx/alerts.cpp", "docs/PLAN.md"]),
    ("Agent", ["Explore auth module", "Research pricing"]),
]
TOOL_ERRORS = ["Exit code 1", "File has not been read yet", "String to replace not found in file"]
TODOS = ["Read codebase", "Write tests", "Implement feature", "Fix lint", "Run test suite", "Update docs"]


class DemoProvider(Provider):
    def __init__(self, n_sessions: int = 4, seed: int = 7, speed: float = 1.0, calm: bool = False,
                 no_five_hour: bool = False):
        self.rng = random.Random(seed)
        self.speed = speed
        self.calm = calm  # never put a session in the waiting state (no "needs you" alerts / edge glow)
        self.no_five_hour = no_five_hour  # a plan without a 5-hour window (five_hour {off: 1})
        self.events: list[dict] = []
        now = int(time.time())
        self.sessions = []
        for i in range(n_sessions):
            proj, title = PROJECTS[i % len(PROJECTS)]
            self.sessions.append({
                "id": f"{self.rng.getrandbits(32):08x}",
                "name": proj, "project": proj, "cwd": f"~/projects/{proj}", "title": title,
                "status": ["busy", "busy", "busy" if calm else "waiting", "idle", "busy"][i % 5],
                "since": now - self.rng.randint(5, 300),
                "started": now - self.rng.randint(600, 7200),
                "model": ["opus-5.5", "opus-5.5", "sonnet-5.5", "opus-5.5", "haiku-5.5"][i % 5],
                "activity": {"tool": "Read", "detail": "src/server.ts", "ts": now},
                "last_prompt": f"Please {title.lower()} and make sure tests pass",
                "tokens": {"in": 1000 * i, "out": 20000 + 5000 * i, "cache_read": 400000 * (i + 1), "cache_write": 50000},
                "cost": round(1.2 + i * 2.3, 2),
                "ctx_pct": 20 + 15 * i,
                "tool_count": 10 + 7 * i,
                "todos": {"done": i, "total": 5, "current": TODOS[i]},
                "subagents": [],
                "kind": "interactive", "perm_mode": ["default", "auto", "acceptEdits", "plan", "auto"][i % 5],
                "branch": "main",
            })
            if self.sessions[-1]["status"] == "waiting":
                self.sessions[-1]["waiting"] = "Allow Bash: npm run deploy?"
        self.next_change = time.time()
        self.tpm = [self.rng.randint(0, 8000) for _ in range(60)]
        self.hourly = [0] * 24
        for h in range(9, 15):
            self.hourly[h] = self.rng.randint(50_000, 600_000)
        self.daily = [self.rng.randint(200_000, 6_000_000) for _ in range(14)]
        self.tools_today = 212
        self.prompts_today = 9

    def _ev(self, kind: str, s: dict, **kw) -> None:
        self.events.append({"t": "event", "kind": kind, "sid": s["id"], "name": s["name"], "ts": int(time.time()), **kw})

    def poll(self, now: float) -> None:
        if now < self.next_change:
            return
        self.next_change = now + self.rng.uniform(0.6, 2.0) / self.speed
        s = self.rng.choice(self.sessions)
        t = int(now)
        r = self.rng.random()
        if s["status"] == "busy":
            if r < 0.70:
                tool, details = self.rng.choice(TOOLS)
                detail = self.rng.choice(details)
                s["activity"] = {"tool": tool, "detail": detail, "ts": t}
                s["tool_count"] += 1
                self.tools_today += 1
                s["tokens"]["out"] += self.rng.randint(200, 3000)
                s["tokens"]["cache_read"] += self.rng.randint(20_000, 90_000)
                s["cost"] = round(s["cost"] + self.rng.uniform(0.01, 0.12), 2)
                s["ctx_pct"] = min(98, s["ctx_pct"] + self.rng.randint(0, 2))
                self._ev("tool", s, tool=tool, text=detail)
                if tool in ("Bash", "Edit") and self.rng.random() < 0.12:
                    self._ev("tool_error", s, tool=tool, text=detail, error=self.rng.choice(TOOL_ERRORS))
                if tool == "Agent" and len(s["subagents"]) < 3:
                    a = {"id": f"{self.rng.getrandbits(32):08x}", "desc": detail,
                         "type": self.rng.choice(["Explore", "general-purpose"]),
                         "status": "busy", "tool": "Grep", "detail": "\"auth\" in src/", "started": t, "tokens": 0,
                         "cost": 0.0, "model": "opus-5.5", "ctx_pct": 3}
                    s["subagents"].append(a)
                    self._ev("subagent_start", s, agent=a["id"], text=detail)
            elif r < 0.80 and not self.calm:
                s["status"], s["since"] = "waiting", t
                s["waiting"] = f"Allow Bash: {self.rng.choice(TOOLS[2][1])}?"
                self._ev("waiting", s, text=s["waiting"])
            elif r < 0.90:
                s["status"], s["since"] = "idle", t
                self._ev("stop", s, text="Done - all tests passing.")
            elif s.get("todos") and s["todos"]["done"] < s["todos"]["total"]:
                td = s["todos"]
                self._ev("todo_done", s, text=td["current"])
                td["done"] += 1
                td["current"] = TODOS[td["done"] % len(TODOS)]
        elif s["status"] == "waiting" and r < 0.5:
            s["status"], s["since"] = "busy", t
            s.pop("waiting", None)
            s["activity"] = {"tool": "Bash", "detail": "npm test", "ts": t}
            self._ev("tool", s, tool="Bash", text="npm test")
        elif s["status"] == "idle" and r < 0.4:
            s["status"], s["since"] = "busy", t
            s["last_prompt"] = "Now refactor the error handling"
            self.prompts_today += 1
            self._ev("prompt", s, text=s["last_prompt"])
        for a in s["subagents"]:
            if a["status"] == "busy":
                a["tokens"] += self.rng.randint(2000, 15000)
                a["cost"] = round(a["tokens"] * 9e-6, 2)
                a["ctx_pct"] = min(60, 3 + a["tokens"] // 10000)
                a["tool"], a["detail"] = self.rng.choice([("Read", "src/routes/auth.ts"), ("Grep", "\"session\" in src/"),
                                                          ("Glob", "src/**/*.ts"), ("WebFetch", "docs.stripe.com/api")])
                if self.rng.random() < 0.15:
                    a["status"] = "done"
                    self._ev("subagent_stop", s, agent=a["id"], text=a["desc"])
        s["subagents"] = [a for a in s["subagents"] if a["status"] == "busy" or t - a["started"] < 120]
        self.tpm = self.tpm[1:] + [self.rng.randint(0, 9000) if any(x["status"] == "busy" for x in self.sessions) else 0]

    def contribute(self, state: dict) -> None:
        now = int(time.time())
        ss = sorted(self.sessions, key=lambda s: s["activity"]["ts"], reverse=True)
        state["sessions"] = ss
        tok = sum(sum(s["tokens"].values()) for s in ss)
        state["totals"] = {
            "sessions": len(ss),
            "busy": sum(s["status"] == "busy" for s in ss),
            "waiting": sum(s["status"] == "waiting" for s in ss),
            "idle": sum(s["status"] == "idle" for s in ss),
            "offline": sum(s["status"] == "offline" for s in ss),
            "subagents_active": sum(a["status"] == "busy" for s in ss for a in s["subagents"]),
            "tokens_today": tok, "cost_today": round(sum(s["cost"] for s in ss), 2),
            "tools_today": self.tools_today, "prompts_today": self.prompts_today,
        }
        state["usage"] = {
            "five_hour": {"off": 1, "at": now} if self.no_five_hour else {"pct": 42, "resets_at": now + 2 * 3600 + 900},
            "seven_day": {"pct": 18, "resets_at": now + 4 * 86400},
            "seven_day_models": [{"name": "Fable", "pct": 61, "resets_at": now + 4 * 86400, "at": int(now)}],
            "tpm": self.tpm, "hourly": self.hourly, "daily": self.daily,
            "by_model": {"opus": int(tok * 0.8), "sonnet": int(tok * 0.15), "haiku": int(tok * 0.05)},
            "streak_days": 6,
            "daily_cost": [round(d * 8e-6, 2) for d in self.daily],
            "today": {"tokens": tok, "cost": round(sum(s["cost"] for s in ss), 2), "msgs": 412,
                      "cache_pct": 91, "saved": 38.4},
            "week": {"tokens": sum(self.daily[-7:]), "cost": round(sum(self.daily[-7:]) * 8e-6, 2)},
            "tpm_now": sum(self.tpm[-5:]) // 5, "burn": 6.8, "peak_hour": 11,
            "window": {"start": now - 9000, "resets_at": now + 2 * 3600 + 900, "tokens": 2_100_000,
                       "cost": 14.2, "pct": 63, "est": 1},
        }

    def drain_events(self) -> list[dict]:
        ev, self.events = self.events, []
        return ev
