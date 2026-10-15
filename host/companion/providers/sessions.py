"""STUB — real Claude Code sessions provider (owned by the host-sessions agent)."""
from .base import Provider


class SessionsProvider(Provider):
    def contribute(self, state: dict) -> None:
        state.setdefault("sessions", [])
