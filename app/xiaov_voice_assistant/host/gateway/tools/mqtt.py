"""MQTT device-control boundary with topic confinement and dry-run default."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any, Protocol

from gateway.tools.registry import ToolExecutionError


class MqttPublisher(Protocol):
    async def publish(
        self,
        topic: str,
        payload: Mapping[str, Any],
        *,
        qos: int,
        retain: bool,
    ) -> Any: ...


class DryRunMqttPublisher:
    async def publish(
        self,
        topic: str,
        payload: Mapping[str, Any],
        *,
        qos: int,
        retain: bool,
    ) -> Any:
        return {
            "executed": False,
            "mode": "dry_run",
            "topic": topic,
            "payload": dict(payload),
            "qos": qos,
            "retain": retain,
        }


def mqtt_control_tool(
    publisher: MqttPublisher, *, topic_prefix: str = "xiaov/devices/"
):
    if (
        not topic_prefix
        or topic_prefix.startswith("/")
        or "+" in topic_prefix
        or "#" in topic_prefix
    ):
        raise ValueError("topic_prefix must be a non-wildcard relative MQTT prefix")

    async def handle(arguments: dict[str, Any]) -> Any:
        action = arguments["action"]
        value = arguments.get("value")
        if action in ("set_temperature", "set_value") and "value" not in arguments:
            raise ToolExecutionError(
                f"value is required for {action}", code="invalid_arguments"
            )
        if action == "set_temperature" and (
            isinstance(value, bool) or not isinstance(value, (int, float))
        ):
            raise ToolExecutionError(
                "set_temperature requires a numeric value", code="invalid_arguments"
            )
        topic = f"{topic_prefix}{arguments['device_id']}/set"
        payload = {"action": action}
        if "value" in arguments:
            payload["value"] = value
        return await publisher.publish(topic, payload, qos=1, retain=False)

    return handle


MQTT_PARAMETERS = {
    "type": "object",
    "properties": {
        "device_id": {
            "type": "string",
            "pattern": r"[A-Za-z0-9_-]{1,64}",
            "maxLength": 64,
        },
        "action": {
            "type": "string",
            "enum": ["turn_on", "turn_off", "set_temperature", "set_value"],
        },
        "value": {
            "anyOf": [
                {"type": "string", "maxLength": 200},
                {"type": "number"},
                {"type": "boolean"},
            ]
        },
    },
    "required": ["device_id", "action"],
    "additionalProperties": False,
}
