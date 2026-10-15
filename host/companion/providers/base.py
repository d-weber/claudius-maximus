class Provider:
    def poll(self, now: float) -> None:
        pass

    def contribute(self, state: dict) -> None:
        pass

    def drain_events(self) -> list[dict]:
        return []

    def on_device_message(self, msg: dict) -> None:
        pass
