"""Data providers. Each provider contributes part of the `state` snapshot and emits `event`s.

Interface (duck-typed):
    poll(now: float) -> None             # called every tick (~0.5 s); do incremental work here
    contribute(state: dict) -> None      # add/merge your keys into the state dict (see PROTOCOL.md)
    drain_events() -> list[dict]         # events ({"t":"event",...}) produced since last call
    on_device_message(msg: dict) -> None # optional: messages from the device (actions)
"""
