"""The claude.ai usage endpoint poller: 15-minute cadence (never faster, also after errors), token handling,
and how its windows merge with the statusLine tee and the cached snapshot."""
from __future__ import annotations

import io
import json
import os
import shutil
import sys
import tempfile
import time
import unittest
import urllib.error
from datetime import datetime, timezone
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from companion.providers import usage as usage_mod  # noqa: E402


def iso(ts: int) -> str:
    return datetime.fromtimestamp(ts, timezone.utc).isoformat()


class FakeResponse(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class EndpointTests(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="cm-usage-ep-")
        self.cred = os.path.join(self.d, ".credentials.json")
        self.now = int(time.time())
        self.calls: list = []
        self.body = {
            "five_hour": {"utilization": 3, "resets_at": iso(self.now + 3600)},
            "seven_day": {"utilization": 28, "resets_at": iso(self.now + 4 * 86400)},
            "seven_day_opus": None,
            "limits": [
                {"kind": "session", "group": "session", "percent": 3, "resets_at": iso(self.now + 3600), "scope": None},
                {"kind": "weekly_all", "group": "weekly", "percent": 28, "resets_at": iso(self.now + 4 * 86400)},
                {"kind": "weekly_scoped", "group": "weekly", "percent": 61, "resets_at": iso(self.now + 4 * 86400),
                 "scope": {"model": {"id": None, "display_name": "Fable"}, "surface": None}},
            ],
        }
        self.write_cred(self.now + 3600)

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def write_cred(self, expires_s: int):
        with open(self.cred, "w", encoding="utf-8") as f:
            json.dump({"claudeAiOauth": {"accessToken": "sk-ant-oat01-test", "refreshToken": "r",
                                         "expiresAt": expires_s * 1000, "scopes": ["user:profile"]}}, f)

    def fake_urlopen(self, req, timeout=None):
        self.calls.append((req.full_url, dict(req.header_items()), timeout))
        if isinstance(self.body, Exception):
            raise self.body
        return FakeResponse(json.dumps(self.body).encode())

    def run_poll(self, ep: usage_mod.UsageEndpoint, now: float):
        with mock.patch.object(usage_mod, "CREDENTIALS_FILE", self.cred), \
                mock.patch.object(usage_mod.urllib.request, "urlopen", self.fake_urlopen):
            ep.poll(now)
            if ep.thread:
                ep.thread.join(5)

    def test_fetch_parses_windows_and_sends_bearer(self):
        ep = usage_mod.UsageEndpoint()
        self.run_poll(ep, self.now)
        self.assertEqual(len(self.calls), 1)
        url, headers, timeout = self.calls[0]
        self.assertEqual(url, usage_mod.USAGE_ENDPOINT)
        self.assertEqual(headers["Authorization"], "Bearer sk-ant-oat01-test")
        self.assertEqual(headers["Anthropic-beta"], "oauth-2025-04-20")
        self.assertEqual(timeout, usage_mod.USAGE_TIMEOUT_S)
        w = ep.windows(self.now)
        self.assertEqual(w["five_hour"]["pct"], 3)
        self.assertEqual(w["seven_day"], {"pct": 28, "resets_at": self.now + 4 * 86400, "at": ep.at})
        self.assertEqual(w["seven_day_models"], [{"name": "Fable", "pct": 61, "resets_at": self.now + 4 * 86400,
                                                  "at": ep.at}])

    def test_fifteen_minute_floor_even_after_errors(self):
        ep = usage_mod.UsageEndpoint()
        self.body = urllib.error.HTTPError(usage_mod.USAGE_ENDPOINT, 401, "unauthorized", {}, None)
        t = self.now
        self.run_poll(ep, t)
        self.assertEqual(len(self.calls), 1)
        self.assertEqual(ep.error, "HTTP 401")
        for dt in (1, 60, 14 * 60 + 59):           # nothing until 15 minutes have passed
            self.run_poll(ep, t + dt)
        self.assertEqual(len(self.calls), 1)
        self.body = urllib.error.URLError("no network")
        self.run_poll(ep, t + 15 * 60)
        self.assertEqual(len(self.calls), 2)
        self.assertEqual(ep.windows(t + 15 * 60), {})
        self.run_poll(ep, t + 29 * 60)
        self.assertEqual(len(self.calls), 2)
        self.assertEqual(usage_mod.USAGE_POLL_S, 900)

    def test_no_or_expired_token_means_no_request(self):
        ep = usage_mod.UsageEndpoint()
        self.write_cred(self.now - 1)
        self.run_poll(ep, self.now)
        self.assertEqual(self.calls, [])
        os.unlink(self.cred)
        with mock.patch.object(usage_mod.sys, "platform", "linux"):
            self.run_poll(ep, self.now + 2 * usage_mod.USAGE_POLL_S)
        self.assertEqual(self.calls, [])
        self.write_cred(self.now + 3600)
        self.run_poll(ep, self.now + 3 * usage_mod.USAGE_POLL_S)
        self.assertEqual(len(self.calls), 1)

    def test_disabled_by_setting(self):
        ep = usage_mod.UsageEndpoint(enabled=False)
        self.run_poll(ep, self.now)
        self.assertEqual(self.calls, [])
        with mock.patch("companion.hostos.settings_get", return_value=False):
            self.assertFalse(usage_mod.usage_endpoint_enabled())
        with mock.patch("companion.hostos.settings_get", return_value=None):
            self.assertFalse(usage_mod.usage_endpoint_enabled())
        with mock.patch("companion.hostos.settings_get", side_effect=lambda k, d=None: d):
            self.assertTrue(usage_mod.usage_endpoint_enabled())

    def test_merge_with_tee_and_snapshot(self):
        tee = os.path.join(self.d, "statusline.json")
        cfg = os.path.join(self.d, ".claude.json")
        with open(tee, "w", encoding="utf-8") as f:
            json.dump({"captured_at": self.now, "rate_limits_at": self.now, "rate_limits": {
                "five_hour": {"used_percentage": 5, "resets_at": self.now + 3600},
                "seven_day": {"used_percentage": 30, "resets_at": self.now + 86400}}}, f)
        with open(cfg, "w", encoding="utf-8") as f:
            json.dump({"cachedUsageUtilization": {"fetchedAtMs": (self.now - 3600) * 1000, "utilization": {
                "limits": [{"kind": "weekly_scoped", "percent": 50, "resets_at": iso(self.now + 86400),
                            "scope": {"model": {"display_name": "Fable"}}}]}}}, f)
        up = usage_mod.UsageProvider(projects_glob=os.path.join(self.d, "none", "*.jsonl"))
        with mock.patch.object(usage_mod, "RATE_LIMIT_FILES", [tee]), \
                mock.patch.object(usage_mod, "CLAUDE_CONFIG_JSON", cfg):
            st: dict = {}
            up.contribute(st)                      # before any endpoint answer: tee + snapshot
            self.assertEqual(st["usage"]["five_hour"]["pct"], 5)
            self.assertEqual(st["usage"]["seven_day_models"][0]["pct"], 50)
            self.run_poll(up.endpoint, self.now)
            st = {}
            up.contribute(st)                      # the tee keeps 5h/7d; the endpoint's fresher Fable row wins
            self.assertEqual(st["usage"]["five_hour"]["pct"], 5)
            self.assertEqual(st["usage"]["seven_day"]["pct"], 30)
            self.assertEqual(st["usage"]["seven_day_models"][0]["pct"], 61)
        with mock.patch.object(usage_mod, "RATE_LIMIT_FILES", [os.path.join(self.d, "no-tee.json")]), \
                mock.patch.object(usage_mod, "CLAUDE_CONFIG_JSON", cfg):
            st = {}
            up.contribute(st)                      # no tee: the endpoint supplies 5h/7d too
            self.assertEqual(st["usage"]["five_hour"]["pct"], 3)
            self.assertEqual(st["usage"]["seven_day"]["pct"], 28)

    def test_token_never_logged(self):
        ep = usage_mod.UsageEndpoint()
        self.body = urllib.error.HTTPError(usage_mod.USAGE_ENDPOINT, 403, "forbidden", {}, None)
        with self.assertLogs("companion.usage", level="DEBUG") as cm:
            self.run_poll(ep, self.now)
        self.assertNotIn("sk-ant-oat01-test", "\n".join(cm.output))


if __name__ == "__main__":
    unittest.main()
