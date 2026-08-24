"""In-process timer records; notification delivery is intentionally separate."""

from __future__ import annotations

import asyncio
import time
from collections.abc import Mapping
from typing import Any, Protocol

from gateway.tools.registry import ToolExecutionError


class TimerStore(Protocol):
    async def create(
        self, *, duration_seconds: int, label: str, kind: str
    ) -> Mapping[str, Any]: ...

    async def cancel(self, timer_id: str) -> Mapping[str, Any]: ...

    async def list_active(self) -> list[Mapping[str, Any]]: ...


class InMemoryTimerStore:
    """Stores deadlines but does not schedule notifications or device actions."""

    def __init__(self, *, monotonic=time.monotonic) -> None:
        self._monotonic = monotonic
        self._next_id = 1
        self._timers: dict[str, dict[str, Any]] = {}
        self._lock = asyncio.Lock()

    async def create(
        self, *, duration_seconds: int, label: str, kind: str
    ) -> Mapping[str, Any]:
        async with self._lock:
            timer_id = f"timer-{self._next_id:04d}"
            self._next_id += 1
            record = {
                "timer_id": timer_id,
                "duration_seconds": duration_seconds,
                "label": label,
                "kind": kind,
                "deadline_monotonic": round(self._monotonic() + duration_seconds, 3),
                "notification_delivery": "not_configured",
            }
            self._timers[timer_id] = record
            return dict(record)

    async def cancel(self, timer_id: str) -> Mapping[str, Any]:
        async with self._lock:
            record = self._timers.pop(timer_id, None)
        if record is None:
            raise ToolExecutionError("timer was not found", code="not_found")
        return {"timer_id": timer_id, "cancelled": True}

    async def list_active(self) -> list[Mapping[str, Any]]:
        async with self._lock:
            return [dict(item) for item in self._timers.values()]


def create_timer_tool(store: TimerStore):
    async def handle(arguments: dict[str, Any]) -> Mapping[str, Any]:
        return await store.create(
            duration_seconds=arguments["duration_seconds"],
            label=arguments.get("label", "计时器").strip() or "计时器",
            kind=arguments.get("kind", "timer"),
        )

    return handle


def cancel_timer_tool(store: TimerStore):
    async def handle(arguments: dict[str, Any]) -> Mapping[str, Any]:
        return await store.cancel(arguments["timer_id"])

    return handle


def list_timers_tool(store: TimerStore):
    async def handle(arguments: dict[str, Any]) -> dict[str, Any]:
        return {"timers": await store.list_active()}

    return handle


CREATE_TIMER_PARAMETERS = {
    "type": "object",
    "properties": {
        "duration_seconds": {"type": "integer", "minimum": 1, "maximum": 604800},
        "label": {"type": "string", "minLength": 1, "maxLength": 100},
        "kind": {"type": "string", "enum": ["timer", "pomodoro"]},
    },
    "required": ["duration_seconds"],
    "additionalProperties": False,
}

CANCEL_TIMER_PARAMETERS = {
    "type": "object",
    "properties": {
        "timer_id": {
            "type": "string",
            "pattern": r"timer-[0-9]{4,}",
            "maxLength": 64,
        }
    },
    "required": ["timer_id"],
    "additionalProperties": False,
}

LIST_TIMERS_PARAMETERS = {
    "type": "object",
    "properties": {},
    "additionalProperties": False,
}
