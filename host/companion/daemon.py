"""Main loop: gather state from providers, stream it to the device, relay device actions."""
from __future__ import annotations

import json
import logging
import time

from .link import DeviceLink

log = logging.getLogger("companion")

HEARTBEAT_S = 2.0
TICK_S = 0.25


def build_providers(demo: bool):
    if demo:
        from .providers.demo import DemoProvider
        return [DemoProvider()]
    from .providers.sessions import SessionsProvider
    from .providers.usage import UsageProvider
    from .providers.hooks import HooksProvider
    return [SessionsProvider(), UsageProvider(), HooksProvider()]


def snapshot(providers) -> dict:
    state: dict = {"t": "state", "ts": int(time.time()), "tz": -time.altzone if time.localtime().tm_isdst else -time.timezone}
    for p in providers:
        try:
            p.contribute(state)
        except Exception:
            log.exception("contribute failed: %s", type(p).__name__)
    return state


def run(demo: bool = False, port: str | None = None, dry_run: bool = False):
    providers = build_providers(demo)
    link = DeviceLink(port)
    last_sent_body = None
    last_sent_at = 0.0
    last_connect_try = 0.0
    while True:
        now = time.time()
        for p in providers:
            try:
                p.poll(now)
            except Exception:
                log.exception("poll failed: %s", type(p).__name__)
        events = []
        for p in providers:
            try:
                events += p.drain_events()
            except Exception:
                log.exception("drain_events failed: %s", type(p).__name__)
        state = snapshot(providers)
        body = json.dumps({k: v for k, v in state.items() if k != "ts"}, sort_keys=True)

        if dry_run:
            for e in events:
                print(json.dumps(e))
            if body != last_sent_body or now - last_sent_at > HEARTBEAT_S:
                print(json.dumps(state))
                last_sent_body, last_sent_at = body, now
            time.sleep(TICK_S)
            continue

        if not link.connected and now - last_connect_try > 2:
            last_connect_try = now
            if link.connect():
                log.info("device connected on %s", link.ser.port)
                link.send({"t": "cmd", "hello": 1})
                last_sent_body = None
        if link.connected:
            for e in events:
                link.send(e)
            if body != last_sent_body or now - last_sent_at > HEARTBEAT_S:
                if link.send(state):
                    last_sent_body, last_sent_at = body, now
            for msg in link.read_messages():
                if msg.get("t") == "log":
                    log.info("device: %s", msg.get("msg"))
                for p in providers:
                    try:
                        p.on_device_message(msg)
                    except Exception:
                        log.exception("on_device_message failed")
        time.sleep(TICK_S)
