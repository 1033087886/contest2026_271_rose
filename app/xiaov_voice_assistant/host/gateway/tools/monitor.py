from __future__ import annotations

from collections.abc import Mapping
from typing import Any, Protocol


class MonitorController(Protocol):
    async def command(self, action: str, parameters: Mapping[str, Any]) -> Any: ...


class DryRunMonitorController:
    async def command(self, action: str, parameters: Mapping[str, Any]) -> Any:
        return {
            "executed": False,
            "mode": "dry_run",
            "action": action,
            "parameters": dict(parameters),
        }


def monitor_control_tool(controller: MonitorController):
    async def handle(arguments: dict[str, Any]) -> Any:
        action = arguments["action"]
        parameters = {key: value for key, value in arguments.items() if key != "action"}
        return await controller.command(action, parameters)

    return handle


MONITOR_PARAMETERS = {
    "type": "object",
    "properties": {
        "action": {
            "type": "string",
            "enum": ["open_monitor", "close_monitor", "play_video", "stop_video"],
        },
        "source": {"type": "string", "minLength": 1, "maxLength": 1024},
    },
    "required": ["action"],
    "additionalProperties": False,
}


__all__ = [
    "DryRunMonitorController",
    "MONITOR_PARAMETERS",
    "MonitorController",
    "monitor_control_tool",
]
