"""usage.seven_day_models: per-model weekly plan windows (e.g. Fable) from the statusLine tee or Claude Code's
cached /usage snapshot in ~/.claude.json."""
from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import time
import unittest
from datetime import datetime, timezone
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from companion.providers import usage as usage_mod  # noqa: E402
from companion.providers.demo import DemoProvider  # noqa: E402


def iso(ts: int) -> str:
    return datetime.fromtimestamp(ts, timezone.utc).isoformat()


class ModelWeekRowsTests(unittest.TestCase):
    def test_limits_rows_preferred_first_and_normalised(self):
        now = 1_800_000_000
        limits = [
            {"kind": "session", "group": "session", "percent": 12, "resets_at": iso(now + 3600), "scope": None},
            {"kind": "weekly_all", "group": "weekly", "percent": 42, "resets_at": iso(now + 86400), "scope": None},
            {"kind": "weekly_scoped", "group": "weekly", "percent": 17.6, "resets_at": iso(now + 86400),
             "scope": {"model": {"id": None, "display_name": "Sonnet"}, "surface": None}},
            {"kind": "weekly_scoped", "group": "weekly", "percent": 61, "resets_at": iso(now + 86400),
             "scope": {"model": {"id": None, "display_name": " Fable "}, "surface": None}},
        ]
        rows = usage_mod.model_week_rows(None, limits, now, 1234)
        self.assertEqual([r["name"] for r in rows], ["Fable", "Sonnet"])
        self.assertEqual(rows[0], {"name": "Fable", "pct": 61, "resets_at": now + 86400, "at": 1234})
        self.assertEqual(rows[1]["pct"], 18)

    def test_scoped_shape_and_expired_windows(self):
        now = 1_800_000_000
        scoped = [{"display_name": "Fable", "utilization": 30, "resets_at": iso(now - 5)},      # rolled over: unknown
                  {"display_name": "Opus", "utilization": 55.4, "resets_at": None},
                  {"display_name": "", "utilization": 9, "resets_at": None},
                  {"display_name": "Haiku", "utilization": None, "resets_at": None}]
        rows = usage_mod.model_week_rows(scoped, None, now, 0)
        self.assertEqual(rows, [{"name": "Opus", "pct": 55, "at": 0}])

    def test_duplicate_names_keep_first(self):
        now = 1_800_000_000
        rows = usage_mod.model_week_rows([{"display_name": "Fable", "utilization": 10, "resets_at": None}],
                                         [{"kind": "weekly_scoped", "percent": 99, "resets_at": None,
                                           "scope": {"model": {"display_name": "fable"}}}], now, 0)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["pct"], 10)


class CachedSnapshotTests(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="cm-usage-mw-")
        self.cfg = os.path.join(self.d, ".claude.json")
        self.tee = os.path.join(self.d, "statusline.json")
        self.now = int(time.time())

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def write_cfg(self, fetched_at: int, limits: list, extra: dict | None = None):
        u = {"five_hour": {"utilization": 12, "resets_at": iso(self.now + 3600)},
             "seven_day": {"utilization": 42, "resets_at": iso(self.now + 86400)},
             "seven_day_opus": None, "seven_day_sonnet": None, "limits": limits}
        u.update(extra or {})
        with open(self.cfg, "w", encoding="utf-8") as f:
            json.dump({"numStartups": 3, "cachedUsageUtilization": {"fetchedAtMs": fetched_at * 1000,
                                                                       "accountUuid": "x", "utilization": u}}, f)

    def provider(self):
        return usage_mod.UsageProvider(projects_glob=os.path.join(self.d, "none", "*.jsonl"))

    def test_fallback_from_claude_json(self):
        fetched = self.now - 3 * 3600
        self.write_cfg(fetched, [{"kind": "weekly_scoped", "percent": 61, "resets_at": iso(self.now + 4 * 86400),
                                  "scope": {"model": {"display_name": "Fable"}}}])
        with mock.patch.object(usage_mod, "RATE_LIMIT_FILES", [self.tee]), \
                mock.patch.object(usage_mod, "CLAUDE_CONFIG_JSON", self.cfg):
            st: dict = {}
            self.provider().contribute(st)
        mw = st["usage"]["seven_day_models"]
        self.assertEqual(mw, [{"name": "Fable", "pct": 61, "resets_at": self.now + 4 * 86400,
                               "at": fetched - fetched % 60}])
        self.assertNotIn("five_hour", st["usage"])   # the snapshot is used for the per-model rows only

    def test_legacy_named_buckets(self):
        self.write_cfg(self.now - 60, [], {"seven_day_opus": {"utilization": 33, "resets_at": iso(self.now + 86400)}})
        with mock.patch.object(usage_mod, "RATE_LIMIT_FILES", [self.tee]), \
                mock.patch.object(usage_mod, "CLAUDE_CONFIG_JSON", self.cfg):
            st: dict = {}
            self.provider().contribute(st)
        self.assertEqual([r["name"] for r in st["usage"]["seven_day_models"]], ["Opus"])

    def test_old_snapshot_and_missing_file_are_omitted(self):
        self.write_cfg(self.now - usage_mod.RATE_LIMIT_MAX_AGE - 60,
                       [{"kind": "weekly_scoped", "percent": 61, "resets_at": iso(self.now + 86400),
                         "scope": {"model": {"display_name": "Fable"}}}])
        with mock.patch.object(usage_mod, "RATE_LIMIT_FILES", [self.tee]), \
                mock.patch.object(usage_mod, "CLAUDE_CONFIG_JSON", self.cfg):
            st: dict = {}
            self.provider().contribute(st)
            self.assertNotIn("seven_day_models", st["usage"])
        with mock.patch.object(usage_mod, "RATE_LIMIT_FILES", [self.tee]), \
                mock.patch.object(usage_mod, "CLAUDE_CONFIG_JSON", os.path.join(self.d, "nope.json")):
            st = {}
            self.provider().contribute(st)
            self.assertNotIn("seven_day_models", st["usage"])

    def test_tee_wins_over_snapshot(self):
        self.write_cfg(self.now - 60, [{"kind": "weekly_scoped", "percent": 61, "resets_at": iso(self.now + 86400),
                                        "scope": {"model": {"display_name": "Fable"}}}])
        with open(self.tee, "w", encoding="utf-8") as f:
            json.dump({"captured_at": self.now, "rate_limits_at": self.now, "rate_limits": {
                "five_hour": {"used_percentage": 3, "resets_at": self.now + 3600},
                "seven_day": {"used_percentage": 28, "resets_at": self.now + 86400},
                "model_scoped": [{"display_name": "Fable", "utilization": 64, "resets_at": iso(self.now + 86400)}],
            }}, f)
        with mock.patch.object(usage_mod, "RATE_LIMIT_FILES", [self.tee]), \
                mock.patch.object(usage_mod, "CLAUDE_CONFIG_JSON", self.cfg):
            st: dict = {}
            self.provider().contribute(st)
        u = st["usage"]
        self.assertEqual(u["five_hour"]["pct"], 3)
        self.assertEqual(u["seven_day_models"][0]["pct"], 64)
        self.assertEqual(u["seven_day_models"][0]["at"], self.now - self.now % 60)

    def test_snapshot_reread_only_when_changed(self):
        self.write_cfg(self.now - 60, [{"kind": "weekly_scoped", "percent": 61, "resets_at": iso(self.now + 86400),
                                        "scope": {"model": {"display_name": "Fable"}}}])
        up = self.provider()
        with mock.patch.object(usage_mod, "CLAUDE_CONFIG_JSON", self.cfg):
            self.assertEqual(up._cached_model_week(self.now)[0]["pct"], 61)
            self.write_cfg(self.now - 30, [{"kind": "weekly_scoped", "percent": 70, "resets_at": iso(self.now + 86400),
                                            "scope": {"model": {"display_name": "Fable"}}}])
            os.utime(self.cfg, (self.now + 5, self.now + 5))
            self.assertEqual(up._cached_model_week(self.now + 1)[0]["pct"], 61)    # within the recheck interval
            self.assertEqual(up._cached_model_week(self.now + usage_mod.CACHE_RECHECK_S + 1)[0]["pct"], 70)


class DemoTests(unittest.TestCase):
    def test_demo_carries_a_fable_row(self):
        st: dict = {}
        d = DemoProvider()
        d.poll(time.time())
        d.contribute(st)
        self.assertEqual(st["usage"]["seven_day_models"][0]["name"], "Fable")
        self.assertEqual(st["usage"]["five_hour"]["pct"], 42)
        st = {}
        DemoProvider(no_five_hour=True).contribute(st)
        self.assertEqual(st["usage"]["five_hour"]["off"], 1)


if __name__ == "__main__":
    unittest.main()
