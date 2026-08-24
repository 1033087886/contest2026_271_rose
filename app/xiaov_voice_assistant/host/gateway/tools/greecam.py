"""GreeCam v1 air-conditioner control with device-level ACK correlation."""

from __future__ import annotations

import math
import logging
import uuid
from typing import Any, Protocol

from gateway.tools.registry import ToolExecutionError

LOGGER = logging.getLogger(__name__)

GREECAM_PARAMETERS = {
    "type": "object",
    "properties": {
        "action": {
            "type": "string",
            "enum": ["turn_on", "turn_off", "set_temperature"],
        },
        "value": {"type": "number", "minimum": 16, "maximum": 30},
    },
    "required": ["action"],
    "additionalProperties": False,
}


class GreeCamRequestClient(Protocol):
    async def publish_and_wait_json(
        self,
        topic: str,
        payload: dict[str, Any],
        *,
        response_topic: str,
        correlation_key: str,
        correlation_value: str,
        qos: int,
        retain: bool,
        response_timeout_seconds: float | None = None,
    ) -> dict[str, Any]: ...


class GreeCamController:
    """Controls one configured ESPHome GreeCam device."""

    def __init__(
        self,
        client: GreeCamRequestClient,
        *,
        topic_prefix: str = "greecam/v1/",
        device_id: str = "gree-cam",
        ack_timeout_seconds: float = 4.0,
    ) -> None:
        if (
            not topic_prefix
            or topic_prefix.startswith("/")
            or "+" in topic_prefix
            or "#" in topic_prefix
        ):
            raise ValueError("GreeCam topic prefix must be a non-wildcard relative prefix")
        allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-"
        if not device_id or any(char not in allowed for char in device_id):
            raise ValueError("GreeCam device id is invalid")
        if ack_timeout_seconds <= 0 or not math.isfinite(ack_timeout_seconds):
            raise ValueError("GreeCam ACK timeout must be positive and finite")
        self._client = client
        self.topic_prefix = topic_prefix
        self.device_id = device_id
        self.ack_timeout_seconds = ack_timeout_seconds

    async def control(self, action: str, value: Any = None) -> dict[str, Any]:
        if action not in ("turn_on", "turn_off", "set_temperature"):
            raise ToolExecutionError(
                "unsupported air-conditioner action", code="invalid_arguments"
            )
        if action == "set_temperature":
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                raise ToolExecutionError(
                    "set_temperature requires a numeric value", code="invalid_arguments"
                )
            if not math.isfinite(float(value)) or not 16 <= float(value) <= 30:
                raise ToolExecutionError(
                    "temperature must be between 16 and 30 degrees Celsius",
                    code="invalid_arguments",
                )

        command_uuid = str(uuid.uuid4())
        payload: dict[str, Any] = {"uuid": command_uuid}
        if action == "turn_on":
            payload["power"] = True
        elif action == "turn_off":
            payload["power"] = False
        else:
            payload["temp"] = float(value)

        root = f"{self.topic_prefix.rstrip('/')}/{self.device_id}"
        response = await self._client.publish_and_wait_json(
            f"{root}/command/ac",
            payload,
            response_topic=f"{root}/ack",
            correlation_key="uuid",
            correlation_value=command_uuid,
            qos=1,
            retain=False,
            response_timeout_seconds=self.ack_timeout_seconds,
        )
        ack = response.get("device_ack")
        if not isinstance(ack, dict) or not isinstance(ack.get("ok"), bool):
            raise ToolExecutionError(
                "device returned a malformed acknowledgement", code="device_error"
            )
        if not ack["ok"]:
            error = ack.get("error")
            safe_error = (
                error
                if isinstance(error, str) and error
                else "device rejected command"
            )
            raise ToolExecutionError(safe_error, code="device_rejected")
        LOGGER.info(
            "GreeCam command acknowledged device=%s action=%s value=%s",
            self.device_id,
            action,
            value,
        )
        return {
            "executed": True,
            "device": self.device_id,
            "action": action,
            "value": value,
            "device_acknowledged": True,
            "command_uuid": command_uuid,
        }


def greecam_control_tool(controller: GreeCamController):
    async def handle(arguments: dict[str, Any]) -> Any:
        return await controller.control(arguments["action"], arguments.get("value"))

    return handle


__all__ = ["GREECAM_PARAMETERS", "GreeCamController", "greecam_control_tool"]
