"""Bounded Open-Meteo weather provider.

Open-Meteo's public geocoding and forecast APIs do not require credentials.
This adapter intentionally exposes only a small, validated current-weather
result through the existing :class:`WeatherProvider` interface.
"""

from __future__ import annotations

import asyncio
import json
import math
import time
from collections.abc import Mapping
from typing import Any

import httpx

from gateway.tools.registry import ToolExecutionError

DEFAULT_GEOCODING_URL = "https://geocoding-api.open-meteo.com/v1/search"
DEFAULT_FORECAST_URL = "https://api.open-meteo.com/v1/forecast"
DEFAULT_TIMEOUT_SECONDS = 8.0
DEFAULT_MAX_RESPONSE_BYTES = 64 * 1024
DEFAULT_MAX_LOCATION_CHARS = 100
DEFAULT_MAX_LOCATION_BYTES = 400
DEFAULT_GEOCODING_CACHE_TTL_SECONDS = 24 * 60 * 60
DEFAULT_CURRENT_CACHE_TTL_SECONDS = 60.0
DEFAULT_CACHE_MAX_ENTRIES = 256

_CURRENT_FIELDS = (
    "temperature_2m",
    "relative_humidity_2m",
    "apparent_temperature",
    "precipitation",
    "weather_code",
    "wind_speed_10m",
    "is_day",
)

_WEATHER_CODES = {
    0: "clear sky",
    1: "mainly clear",
    2: "partly cloudy",
    3: "overcast",
    45: "fog",
    48: "depositing rime fog",
    51: "light drizzle",
    53: "moderate drizzle",
    55: "dense drizzle",
    56: "light freezing drizzle",
    57: "dense freezing drizzle",
    61: "slight rain",
    63: "moderate rain",
    65: "heavy rain",
    66: "light freezing rain",
    67: "heavy freezing rain",
    71: "slight snow",
    73: "moderate snow",
    75: "heavy snow",
    77: "snow grains",
    80: "slight rain showers",
    81: "moderate rain showers",
    82: "violent rain showers",
    85: "slight snow showers",
    86: "heavy snow showers",
    95: "thunderstorm",
    96: "thunderstorm with slight hail",
    99: "thunderstorm with heavy hail",
}


class OpenMeteoWeatherProvider:
    """Fetch current weather through Open-Meteo's keyless public APIs.

    A supplied ``httpx.AsyncClient`` is never closed by this object. When no
    client is supplied, the provider lazily creates and owns one.
    """

    def __init__(
        self,
        *,
        geocoding_url: str = DEFAULT_GEOCODING_URL,
        forecast_url: str = DEFAULT_FORECAST_URL,
        timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS,
        max_response_bytes: int = DEFAULT_MAX_RESPONSE_BYTES,
        max_location_chars: int = DEFAULT_MAX_LOCATION_CHARS,
        max_location_bytes: int = DEFAULT_MAX_LOCATION_BYTES,
        geocoding_cache_ttl_seconds: float = DEFAULT_GEOCODING_CACHE_TTL_SECONDS,
        current_cache_ttl_seconds: float = DEFAULT_CURRENT_CACHE_TTL_SECONDS,
        cache_max_entries: int = DEFAULT_CACHE_MAX_ENTRIES,
        client: httpx.AsyncClient | None = None,
    ) -> None:
        if not geocoding_url.strip() or not forecast_url.strip():
            raise ValueError("Open-Meteo endpoint URLs must not be empty")
        if timeout_seconds <= 0 or not math.isfinite(timeout_seconds):
            raise ValueError("Open-Meteo timeout_seconds must be positive and finite")
        if not 256 <= max_response_bytes <= 1024 * 1024:
            raise ValueError("Open-Meteo response limit must be 256-1048576 bytes")
        if not 1 <= max_location_chars <= 1000:
            raise ValueError("Open-Meteo location character limit is invalid")
        if not 1 <= max_location_bytes <= 4000:
            raise ValueError("Open-Meteo location byte limit is invalid")
        if (
            geocoding_cache_ttl_seconds < 0
            or not math.isfinite(geocoding_cache_ttl_seconds)
        ):
            raise ValueError("geocoding_cache_ttl_seconds must be finite and non-negative")
        if current_cache_ttl_seconds < 0 or not math.isfinite(current_cache_ttl_seconds):
            raise ValueError("current_cache_ttl_seconds must be finite and non-negative")
        if isinstance(cache_max_entries, bool) or not 1 <= cache_max_entries <= 10_000:
            raise ValueError("cache_max_entries must be an integer from 1 to 10000")

        self.geocoding_url = geocoding_url
        self.forecast_url = forecast_url
        self.timeout_seconds = timeout_seconds
        self.max_response_bytes = max_response_bytes
        self.max_location_chars = max_location_chars
        self.max_location_bytes = max_location_bytes
        self.geocoding_cache_ttl_seconds = geocoding_cache_ttl_seconds
        self.current_cache_ttl_seconds = current_cache_ttl_seconds
        self.cache_max_entries = cache_max_entries
        self._geocoding_cache: dict[str, tuple[float, dict[str, Any]]] = {}
        self._current_cache: dict[str, tuple[float, dict[str, Any]]] = {}
        self._client = client
        self._owns_client = client is None

    async def __aenter__(self) -> "OpenMeteoWeatherProvider":
        return self

    async def __aexit__(self, *_exc_info: object) -> None:
        await self.aclose()

    async def aclose(self) -> None:
        self._geocoding_cache.clear()
        self._current_cache.clear()
        if self._client is not None and self._owns_client:
            await self._client.aclose()
            self._client = None

    def _ensure_client(self) -> httpx.AsyncClient:
        if self._client is None:
            self._client = httpx.AsyncClient(
                timeout=self.timeout_seconds,
                follow_redirects=False,
                headers={"User-Agent": "xiaov-voice-assistant/0.1"},
            )
        return self._client

    async def current(self, location: str) -> Mapping[str, Any]:
        query = _validate_location(
            location,
            max_chars=self.max_location_chars,
            max_bytes=self.max_location_bytes,
        )
        cache_key = _cache_key(query)
        now = time.monotonic()
        cached = self._current_cache.get(cache_key)
        if (
            cached is not None
            and self.current_cache_ttl_seconds > 0
            and now - cached[0] < self.current_cache_ttl_seconds
        ):
            return dict(cached[1])
        try:
            place = await self._resolve_location(query)
            payload = await self._request_json(
                self.forecast_url,
                params={
                    "latitude": place["latitude"],
                    "longitude": place["longitude"],
                    "current": ",".join(_CURRENT_FIELDS),
                    "temperature_unit": "celsius",
                    "wind_speed_unit": "kmh",
                    "precipitation_unit": "mm",
                    "timezone": "auto",
                },
                stage="forecast",
            )
        except asyncio.CancelledError:
            raise
        except TimeoutError as exc:
            raise ToolExecutionError(
                "weather service timed out", code="weather_timeout"
            ) from exc
        except httpx.HTTPError as exc:
            raise ToolExecutionError(
                "weather service is unavailable", code="weather_unavailable"
            ) from exc
        except ToolExecutionError:
            raise
        except Exception as exc:
            raise ToolExecutionError(
                "weather service request failed", code="weather_unavailable"
            ) from exc

        result = _parse_forecast(payload, place)
        if self.current_cache_ttl_seconds > 0:
            self._cache_put(self._current_cache, cache_key, result)
        return dict(result)

    async def _resolve_location(self, location: str) -> dict[str, Any]:
        cache_key = _cache_key(location)
        now = time.monotonic()
        cached = self._geocoding_cache.get(cache_key)
        if (
            cached is not None
            and self.geocoding_cache_ttl_seconds > 0
            and now - cached[0] < self.geocoding_cache_ttl_seconds
        ):
            return dict(cached[1])
        payload = await self._request_json(
            self.geocoding_url,
            params={"name": location, "count": 1, "language": "zh", "format": "json"},
            stage="geocoding",
        )
        results = payload.get("results")
        if results is None:
            raise ToolExecutionError(
                "weather location was not found", code="location_not_found"
            )
        if not isinstance(results, list):
            raise _invalid_response("geocoding")
        if not results:
            raise ToolExecutionError(
                "weather location was not found", code="location_not_found"
            )
        item = results[0]
        if not isinstance(item, dict):
            raise _invalid_response("geocoding")

        latitude = _finite_number(item.get("latitude"), "geocoding")
        longitude = _finite_number(item.get("longitude"), "geocoding")
        if not -90 <= latitude <= 90 or not -180 <= longitude <= 180:
            raise _invalid_response("geocoding")
        name = _bounded_service_text(item.get("name"), "geocoding", required=True)
        country = _bounded_service_text(item.get("country"), "geocoding")
        admin1 = _bounded_service_text(item.get("admin1"), "geocoding")
        place = {
            "latitude": latitude,
            "longitude": longitude,
            "name": name,
            "country": country,
            "admin1": admin1,
        }
        if self.geocoding_cache_ttl_seconds > 0:
            self._cache_put(self._geocoding_cache, cache_key, place)
        return dict(place)

    def _cache_put(
        self,
        cache: dict[str, tuple[float, dict[str, Any]]],
        key: str,
        value: Mapping[str, Any],
    ) -> None:
        cache.pop(key, None)
        while len(cache) >= self.cache_max_entries:
            cache.pop(next(iter(cache)))
        cache[key] = (time.monotonic(), dict(value))

    async def _request_json(
        self, url: str, *, params: Mapping[str, Any], stage: str
    ) -> dict[str, Any]:
        # Geocoding and forecast are independent upstream calls. Give each call
        # the configured budget; sharing one deadline made a slow geocoder leave
        # only a fraction of a second for an otherwise healthy forecast.
        async with asyncio.timeout(self.timeout_seconds):
            async with self._ensure_client().stream(
                "GET", url, params=params
            ) as response:
                if response.status_code != 200:
                    raise ToolExecutionError(
                        "weather service is unavailable", code="weather_unavailable"
                    )
                media_type = response.headers.get("Content-Type", "").partition(";")[0]
                if media_type.strip().lower() != "application/json":
                    raise _invalid_response(stage)
                body = await _read_bounded(response, self.max_response_bytes)
        try:
            payload = json.loads(body)
        except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as exc:
            raise _invalid_response(stage) from exc
        if not isinstance(payload, dict):
            raise _invalid_response(stage)
        return payload


async def _read_bounded(response: httpx.Response, limit: int) -> bytes:
    body = bytearray()
    async for chunk in response.aiter_bytes():
        if len(body) + len(chunk) > limit:
            raise ToolExecutionError(
                "weather service response exceeded the configured limit",
                code="weather_response_too_large",
            )
        body.extend(chunk)
    return bytes(body)


def _validate_location(location: object, *, max_chars: int, max_bytes: int) -> str:
    if not isinstance(location, str):
        raise ToolExecutionError(
            "weather location must be text", code="invalid_arguments"
        )
    if any(ord(character) < 32 or ord(character) == 127 for character in location):
        raise ToolExecutionError(
            "weather location contains control characters", code="invalid_arguments"
        )
    value = location.strip()
    if not value:
        raise ToolExecutionError(
            "weather location must not be empty", code="invalid_arguments"
        )
    try:
        encoded_length = len(value.encode("utf-8"))
    except UnicodeEncodeError as exc:
        raise ToolExecutionError(
            "weather location contains invalid text", code="invalid_arguments"
        ) from exc
    if len(value) > max_chars or encoded_length > max_bytes:
        raise ToolExecutionError(
            "weather location exceeded the configured limit", code="invalid_arguments"
        )
    return value


def _cache_key(location: str) -> str:
    """Normalizes harmless spacing/case differences for TTL cache keys."""
    return " ".join(location.casefold().split())


def _parse_forecast(payload: Mapping[str, Any], place: Mapping[str, Any]) -> dict[str, Any]:
    current = payload.get("current")
    if not isinstance(current, dict):
        raise _invalid_response("forecast")

    temperature = _finite_number(current.get("temperature_2m"), "forecast")
    apparent = _finite_number(current.get("apparent_temperature"), "forecast")
    precipitation = _finite_number(current.get("precipitation"), "forecast")
    wind_speed = _finite_number(current.get("wind_speed_10m"), "forecast")
    humidity = _integer(current.get("relative_humidity_2m"), "forecast")
    weather_code = _integer(current.get("weather_code"), "forecast")
    is_day = _integer(current.get("is_day"), "forecast")
    observed_at = _bounded_service_text(
        current.get("time"), "forecast", required=True, max_chars=64
    )
    timezone = _bounded_service_text(
        payload.get("timezone"), "forecast", required=True, max_chars=100
    )

    if not -100 <= temperature <= 70 or not -100 <= apparent <= 70:
        raise _invalid_response("forecast")
    if not 0 <= humidity <= 100 or precipitation < 0 or wind_speed < 0:
        raise _invalid_response("forecast")
    if weather_code not in _WEATHER_CODES or is_day not in (0, 1):
        raise _invalid_response("forecast")

    result: dict[str, Any] = {
        "condition": _WEATHER_CODES[weather_code],
        "weather_code": weather_code,
        "temperature_c": temperature,
        "apparent_temperature_c": apparent,
        "humidity_percent": humidity,
        "precipitation_mm": precipitation,
        "wind_speed_kmh": wind_speed,
        "is_day": bool(is_day),
        "observed_at": observed_at,
        "timezone": timezone,
        "resolved_name": place["name"],
        "source": "open_meteo",
        "simulated": False,
    }
    if place.get("admin1"):
        result["admin1"] = place["admin1"]
    if place.get("country"):
        result["country"] = place["country"]
    return result


def _finite_number(value: object, stage: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise _invalid_response(stage)
    converted = float(value)
    if not math.isfinite(converted):
        raise _invalid_response(stage)
    return converted


def _integer(value: object, stage: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise _invalid_response(stage)
    return value


def _bounded_service_text(
    value: object,
    stage: str,
    *,
    required: bool = False,
    max_chars: int = 200,
) -> str | None:
    if value is None and not required:
        return None
    if not isinstance(value, str) or not value.strip() or len(value) > max_chars:
        raise _invalid_response(stage)
    text = value.strip()
    try:
        text.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise _invalid_response(stage) from exc
    if any(ord(character) < 32 or ord(character) == 127 for character in text):
        raise _invalid_response(stage)
    return text


def _invalid_response(stage: str) -> ToolExecutionError:
    return ToolExecutionError(
        f"weather service returned an invalid {stage} response",
        code="weather_invalid_response",
    )


__all__ = [
    "DEFAULT_FORECAST_URL",
    "DEFAULT_GEOCODING_URL",
    "OpenMeteoWeatherProvider",
]
