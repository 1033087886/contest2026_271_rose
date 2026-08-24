"""Weather tool with an injectable provider and deterministic local default."""

from __future__ import annotations

import hashlib
from collections.abc import Mapping
from typing import Any, Protocol

from gateway.tools.registry import ToolExecutionError


class WeatherProvider(Protocol):
    async def current(self, location: str) -> Mapping[str, Any]: ...


class LocalWeatherProvider:
    """Offline provider used by default; values are explicitly marked simulated."""

    _CONDITIONS = ("晴", "多云", "阴", "小雨")

    def __init__(self, readings: Mapping[str, Mapping[str, Any]] | None = None) -> None:
        self._readings = dict(readings or {})

    async def current(self, location: str) -> Mapping[str, Any]:
        configured = self._readings.get(location)
        if configured is not None:
            return {**configured, "source": "local_stub", "simulated": True}
        digest = hashlib.sha256(location.encode("utf-8")).digest()
        return {
            "condition": self._CONDITIONS[digest[0] % len(self._CONDITIONS)],
            "temperature_c": 12 + digest[1] % 20,
            "humidity_percent": 35 + digest[2] % 56,
            "source": "local_stub",
            "simulated": True,
        }


def weather_tool(provider: WeatherProvider):
    async def handle(arguments: dict[str, Any]) -> dict[str, Any]:
        location = arguments["location"].strip()
        if not location:
            raise ToolExecutionError(
                "location cannot contain only whitespace", code="invalid_arguments"
            )
        unit = arguments.get("unit", "celsius")
        reading = dict(await provider.current(location))
        temperature_c = reading.pop("temperature_c", None)
        if temperature_c is not None:
            if unit == "fahrenheit":
                reading["temperature"] = round(float(temperature_c) * 9 / 5 + 32, 1)
                reading["unit"] = "fahrenheit"
            else:
                reading["temperature"] = temperature_c
                reading["unit"] = "celsius"
        return {"location": location, **reading}

    return handle


WEATHER_PARAMETERS = {
    "type": "object",
    "properties": {
        "location": {
            "type": "string",
            "description": "City or district name, for example 北京 or 海淀区",
            "minLength": 1,
            "maxLength": 100,
        },
        "unit": {
            "type": "string",
            "enum": ["celsius", "fahrenheit"],
            "default": "celsius",
        },
    },
    "required": ["location"],
    "additionalProperties": False,
}
