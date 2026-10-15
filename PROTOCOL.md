# Claudius Maximus — host ⇄ device protocol

Transport: USB CDC serial (`/dev/cu.usbmodem*`, Espressif VID `0x303a`), 115200 nominal (USB, speed irrelevant).
Framing: one JSON object per line (`\n`). Max line 32 KB. Every message has a `"t"` (type) key.
Times are **unix epoch seconds** (integers). Strings are UTF-8; the device fonts only render ASCII + LVGL symbols,
so the host should strip/transliterate exotic characters (emoji etc.).

Firmware screens must read every field **defensively** (missing key ⇒ sensible default). Hosts may add fields freely.

---

## Host → device

### `state` — full snapshot (sent on change, and at least every 2 s as heartbeat)
```jsonc
{
  "t": "state",
  "ts": 1792068344,            // host clock, epoch s (device syncs its clock to this)
  "tz": 7200,                  // local UTC offset in seconds
  "sessions": [                // one per Claude Code session alive (or recently ended), most-recently-active first
    {
      "id": "969ca7a9",        // short session id (first 8 chars of the sessionId uuid)
      "name": "claudius-maximus", // session display name
      "project": "claudius-maximus", // basename of cwd
      "cwd": "~/projects/claudius-maximus",
      "title": "ESP32 companion display", // AI-generated session title if any
      "status": "busy",        // busy | waiting | idle | offline
                               //  busy    = model is working
                               //  waiting = blocked on the human (permission prompt, question)
                               //  idle    = turn finished, waiting for next prompt
                               //  offline = process gone (kept ~10 min then dropped)
      "since": 1792068300,     // epoch when status last changed
      "started": 1792068083,   // session start epoch
      "model": "opus-5.5",     // short model label of the latest assistant message
      "activity": {            // the latest thing the agent did
        "tool": "Bash",        // tool name, or "thinking" / "writing" / "" 
        "detail": "pio run -t upload", // short human summary (file name, command, query...) ≤ 80 chars
        "ts": 1792068340
      },
      "last_prompt": "Hello, plugged on this computer...", // ≤ 120 chars
      "tokens": { "in": 1200, "out": 45000, "cache_read": 900000, "cache_write": 120000 },
      "cost": 3.42,            // USD, estimated from public API pricing
      "ctx_pct": 42,           // % of context window used by the latest request
      "tool_count": 37,        // tool calls this session
      "todos": { "done": 3, "total": 7, "current": "Building firmware" }, // omitted if no todo list
      "subagents": [
        { "id": "a16e14b7", "desc": "Research Waveshare board pinout", "type": "general-purpose",
          "status": "busy",    // busy | done
          "tool": "WebFetch", "detail": "waveshare.com/wiki/...", "started": 1792068400, "tokens": 154000 }
      ]
    }
  ],
  "totals": {
    "sessions": 2, "busy": 1, "waiting": 0, "idle": 1,
    "subagents_active": 3,
    "tokens_today": 4200000, "cost_today": 12.40, "tools_today": 312, "prompts_today": 14
  },
  "usage": {                   // see host/companion/providers/usage.py
    "five_hour":  { "pct": 34, "resets_at": 1792080000 },  // omitted when unknown
    "seven_day":  { "pct": 12, "resets_at": 1792500000 },
    "tpm": [0, 1200, 5400],    // output+input tokens per minute, last 60 minutes, oldest first
    "hourly": [0, 0, ..., 0],  // 24 values: tokens per hour today (local time)
    "daily": [ ... ],          // 14 values: tokens per day, oldest first, today last
    "by_model": { "opus": 3900000, "sonnet": 300000, "haiku": 0 },
    "streak_days": 5           // consecutive days with activity
  }
}
```

### Partial `state` (perf round, 2026-10-15) — only for firmware announcing `"caps":["part",...]` in its hello
```jsonc
{ "t": "state", "part": 1, "ts": 1792068345, "tz": 7200, "system": { ... } }   // only `system` changed
{ "t": "state", "part": 1, "ts": 1792068347, "tz": 7200 }                        // heartbeat, nothing changed
```
Every top-level key present (other than `t`/`ts`/`tz`/`part`) **replaces** that key of the device's snapshot;
keys absent are kept; `"key": null` removes it. The host sends a full `state` after (re)connect, on `hello` and on
`refresh`, then partials with only the parts whose content changed (`sessions`, `totals`, `usage`, `system`, ...),
and an empty partial as heartbeat (≥ every 2 s). Firmware without the cap keeps getting full snapshots (same
send-on-change + 2 s heartbeat as before). Firmware: `model_part_version("usage")` tells which parts changed;
`model_state_version()` no longer bumps on empty heartbeats (but at least every 10 s).
`system` is refreshed only every 30 s while the device reports a screen other than `system` (see `screen` below).
`usage` is recomputed only on minute boundaries (stable within a minute, costs rounded to cents), `usage.*.at` is
floored to the minute. In `sessions`/`totals`, a change of only the counters `tokens`, `cost`, `ctx_pct`,
`tool_count`, `*_today` is sent at most every 5 s (status/activity/subagent changes go out immediately).
Bursts of > 12 events in one host tick keep every non-`tool` event and only the most recent `tool` events.

### `event` — something just happened (for feeds, animations, alerts)
```jsonc
{ "t": "event", "kind": "tool", "sid": "969ca7a9", "name": "claudius-maximus",
  "tool": "Edit", "text": "src/main.cpp", "ts": 1792068344, "agent": "a16e14b7" /* present if from a subagent */ }
```
`kind` values:
| kind | meaning | extra fields |
|---|---|---|
| `session_start` / `session_end` | a Claude Code process appeared / exited | |
| `prompt` | the human sent a prompt | `text` |
| `tool` | a tool call started | `tool`, `text` |
| `tool_error` | a tool call failed | `tool`, `text` |
| `waiting` | the agent needs the human (permission / question) | `text` |
| `stop` | the agent finished its turn (now idle) | `text` (short last message) |
| `subagent_start` / `subagent_stop` | a subagent launched / finished | `agent`, `text` (description) |
| `todo_done` | a todo item got completed | `text` |
| `compact` | context compaction happened | |

### `cmd` — direct commands
```jsonc
{ "t": "cmd", "nav": "usage" }            // jump to a screen id (see firmware/src/ui/ui.cpp)
{ "t": "cmd", "open": "detail", "sid": "969ca7a9" } // open session detail
{ "t": "cmd", "bl": 80 }                  // backlight percent 0-100
{ "t": "cmd", "shot": 1 }                 // request screenshot (see below)
{ "t": "cmd", "demo_alert": "waiting" }   // dev: trigger alert visuals
```

## Device → host
```jsonc
{ "t": "hello", "fw": "0.2.0", "w": 480, "h": 480, "caps": ["part", "screen"], "screen": "home" } // on boot / on host connect
{ "t": "screen", "id": "system" }                          // visible screen changed (tile id, or "detail")
{ "t": "log", "msg": "..." }
{ "t": "action", "action": "focus", "sid": "969ca7a9" }   // user tapped "focus" → host raises that terminal
{ "t": "shot", "w": 480, "h": 480, "fmt": "rgb565le", "len": 460800 } // followed immediately by len raw bytes
```

### Device → host actions
| action | meaning |
|---|---|
| `focus` + `sid` (8-char or full id) | raise the terminal/editor window running that session (host/companion/runtime_focus.py) |
| `refresh` | host re-sends the full state now |

Unknown actions are logged and ignored.

## Extensions (fields added during the build; all optional, firmware falls back when absent)

### Session extras
`waiting` (why it waits, e.g. "Allow Bash: git push?"), `kind` (interactive / headless), `branch`, `perm_mode`;
subagents also carry `cost`, `model`, `ctx_pct`. Event `tool_error` carries `error`. `totals.offline`.

### `state.usage` extras (doc: top of host/companion/providers/usage.py)
`daily_cost[14]` USD/day; `today {tokens,in,out,cache_read,cache_write,cost,msgs,cache_pct,saved}`; `week {tokens,cost}`;
`tpm_now`; `burn` (USD last 60 min); `peak_hour`; `best_day {tokens,cost}`;
`window {start,resets_at,tokens,cost,pct,est:1}` = ESTIMATED current 5-hour block (not a plan limit).
`five_hour`/`seven_day` come from Claude Code's statusLine `rate_limits` (tee'd to ~/.claudius-maximus/statusline.json)
and carry `at` (when observed). usage "tokens" = input + cache_write + output (cache reads excluded).

### `state.system` — host machine metrics
Schema at the top of host/companion/providers/system.py (cpu, cores, kinds, hist, load, mem, procs, claude, net, disk,
uptime, batt, thermal). Present in real and demo mode.

### Dev-only events (handled by firmware, ignore in production)
`dev_ambient` {saver, dim_s, saver_s}, `dev_orbit` {flags}, `dev_settings` {scroll, log}, `usage_mode` {mode}.
