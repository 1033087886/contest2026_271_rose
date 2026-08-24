"""Media command abstraction with a side-effect-free default controller."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any, Protocol

from gateway.tools.registry import ToolExecutionError


class MediaController(Protocol):
    async def command(self, action: str, parameters: Mapping[str, Any]) -> Any: ...


class DryRunMediaController:
    async def command(self, action: str, parameters: Mapping[str, Any]) -> Any:
        return {
            "executed": False,
            "mode": "dry_run",
            "action": action,
            "parameters": dict(parameters),
        }


def media_control_tool(controller: MediaController):
    async def handle(arguments: dict[str, Any]) -> Any:
        action = arguments["action"]
        parameters = {key: value for key, value in arguments.items() if key != "action"}
        required_by_action = {
            "play": "query",
            "set_volume": "volume_percent",
            "seek": "position_seconds",
        }
        required = required_by_action.get(action)
        if required is not None and required not in parameters:
            raise ToolExecutionError(
                f"{required} is required for {action}", code="invalid_arguments"
            )
        return await controller.command(action, parameters)

    return handle


MEDIA_PARAMETERS = {
    "type": "object",
    "properties": {
        "action": {
            "type": "string",
            "enum": [
                "play",
                "pause",
                "resume",
                "stop",
                "next",
                "previous",
                "set_volume",
                "seek",
            ],
        },
        "query": {"type": "string", "minLength": 1, "maxLength": 300},
        "volume_percent": {"type": "integer", "minimum": 0, "maximum": 100},
        "position_seconds": {"type": "number", "minimum": 0, "maximum": 86400},
    },
    "required": ["action"],
    "additionalProperties": False,
}
