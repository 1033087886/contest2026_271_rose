"""Low-latency direct path for unambiguous current-weather questions."""

from __future__ import annotations

import inspect
import logging
import math
import re
import time
from collections.abc import AsyncIterator, Mapping
from typing import Any

LOGGER = logging.getLogger(__name__)

_WEATHER_MARKER = re.compile(r"天气|气温|温度")
_MULTI_INTENT_MARKER = re.compile(
    r"并|然后|同时|顺便|另外|以及|设置|提醒|定时|播放|音乐|歌曲|视频|摄像头|空调|打开|关闭|控制"
)
_FORECAST_MARKER = re.compile(r"明天|后天|未来|一周|几天|周末|最高|最低|预报")
_COMMAND_PREFIXES = (
    "请告诉我",
    "告诉我",
    "我想知道",
    "我想了解",
    "帮我查询",
    "帮我查一下",
    "帮我查",
    "查询一下",
    "查询",
    "查一下",
    "查查",
    "查",
    "看一下",
    "看看",
    "帮我看看",
    "请问",
    "请",
)
_LOCATION_NOISE = ("今天", "当前", "现在", "本地", "当地", "的")
_CONDITIONS_ZH = {
    0: "晴朗",
    1: "晴",
    2: "多云",
    3: "阴",
    45: "雾",
    48: "雾",
    51: "小雨",
    53: "中雨",
    55: "大雨",
    56: "冻雨",
    57: "冻雨",
    61: "小雨",
    63: "中雨",
    65: "大雨",
    66: "冻雨",
    67: "冻雨",
    71: "小雪",
    73: "中雪",
    75: "大雪",
    77: "雪粒",
    80: "阵雨",
    81: "阵雨",
    82: "强阵雨",
    85: "阵雪",
    86: "阵雪",
    95: "雷雨",
    96: "雷雨伴冰雹",
    99: "雷雨伴冰雹",
}


class FastWeatherLlm:
    """Wrap an LLM and bypass it for one-city current-weather questions."""

    def __init__(self, fallback: Any, weather_provider: Any) -> None:
        self._fallback = fallback
        self._weather_provider = weather_provider

    def __getattr__(self, name: str) -> Any:
        # Preserve the existing provider inspection/configuration surface used
        # by deployment checks and callers that need the underlying LLM.
        return getattr(self._fallback, name)

    async def aclose(self) -> None:
        close = getattr(self._fallback, "aclose", None)
        if close is not None:
            result = close()
            if inspect.isawaitable(result):
                await result

    async def answer_chunks(self, text: str) -> AsyncIterator[str]:
        location = extract_current_weather_location(text)
        if location is None:
            async for chunk in self._fallback.answer_chunks(text):
                yield chunk
            return

        started = time.monotonic()
        try:
            reading = await self._weather_provider.current(location)
            answer = format_current_weather(location, reading)
        except Exception as exc:
            LOGGER.warning(
                "fast weather fallback location=%s error=%s",
                location,
                type(exc).__name__,
            )
            async for chunk in self._fallback.answer_chunks(text):
                yield chunk
            return

        LOGGER.info(
            "fast weather hit location=%s elapsed_ms=%d",
            location,
            round((time.monotonic() - started) * 1000),
        )
        yield answer


def extract_current_weather_location(text: str) -> str | None:
    """Return a city/district only for a simple current-weather question."""
    if not isinstance(text, str):
        return None
    normalized = "".join(text.split())
    marker = _WEATHER_MARKER.search(normalized)
    if marker is None or _MULTI_INTENT_MARKER.search(normalized):
        return None
    if _FORECAST_MARKER.search(normalized):
        return None

    before_weather = normalized[: marker.start()]
    candidate = before_weather
    while True:
        for prefix in _COMMAND_PREFIXES:
            if candidate.startswith(prefix):
                candidate = candidate[len(prefix) :]
                break
        else:
            break
    for noise in _LOCATION_NOISE:
        candidate = candidate.replace(noise, "")
    candidate = candidate.strip("的呢吗呀嘛，。？！?！")
    if not candidate:
        return None
    # Avoid claiming a two-location comparison. Requiring at least two
    # characters on both sides keeps real names such as 和田、永和县 and
    # 呼和浩特 eligible for the geocoder.
    if re.fullmatch(r".{2,}和.{2,}", candidate):
        return None

    if not re.fullmatch(r"[\u4e00-\u9fffA-Za-z0-9·-]{2,24}", candidate):
        return None
    return candidate


def format_current_weather(location: str, reading: Mapping[str, Any]) -> str:
    """Format validated Open-Meteo fields as one short sentence for TTS."""
    temperature = _format_number(reading["temperature_c"])
    apparent = _format_number(reading["apparent_temperature_c"])
    humidity = _format_number(reading["humidity_percent"])
    code = reading.get("weather_code")
    if code not in _CONDITIONS_ZH:
        raise ValueError("unsupported weather code")
    condition = _CONDITIONS_ZH[code]
    return f"{location}现在{temperature}度，体感{apparent}度，{condition}，湿度{humidity}%。"


def _format_number(value: Any) -> str:
    number = float(value)
    if not math.isfinite(number):
        raise ValueError("weather value must be finite")
    if number.is_integer():
        return str(int(number))
    return f"{number:.1f}".rstrip("0").rstrip(".")


__all__ = [
    "FastWeatherLlm",
    "extract_current_weather_location",
    "format_current_weather",
]
