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
{ "t": "hello", "fw": "0.1.0", "w": 480, "h": 480 }       // on boot / on host connect
{ "t": "log", "msg": "..." }
{ "t": "action", "action": "focus", "sid": "969ca7a9" }   // user tapped "focus" → host raises that terminal
{ "t": "shot", "w": 480, "h": 480, "fmt": "rgb565le", "len": 460800 } // followed immediately by len raw bytes
```

### `state.system` — host machine metrics (providers/system.py, owned by the "system" agent)
Schema defined at the top of host/companion/providers/system.py. Present in both real and demo mode.
- [from usage] added optional fields under `state.usage` (real host fills them; firmware falls back when absent; full doc at top of host/companion/providers/usage.py): `daily_cost[14]` USD/day; `today {tokens,in,out,cache_read,cache_write,cost,msgs,cache_pct,saved}`; `week {tokens,cost}`; `tpm_now` (avg tok/min last 5 min); `burn` (USD last 60 min); `peak_hour` (0-23 or -1); `best_day {tokens,cost}`; `window {start,resets_at,tokens,cost,pct,est:1}` = ESTIMATED current 5-hour block (pct relative to your biggest recent block, not a plan limit). Definition: usage "tokens" = input + cache_write + output (cache reads excluded, reported in `today.cache_read`). `by_model` keys are families (opus/fable/sonnet/haiku/other), summed over the 14 days. Please add to the spec; host-sessions: demo.py could emit these too (see usage report).
