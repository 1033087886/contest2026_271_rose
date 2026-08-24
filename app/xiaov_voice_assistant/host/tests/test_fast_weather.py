"""Tests for the direct current-weather path."""

from __future__ import annotations

import unittest

from websockets.asyncio.server import serve

from gateway.providers.fast_weather import (
    FastWeatherLlm,
    extract_current_weather_location,
    format_current_weather,
)
from gateway.providers.composite import CompositePipeline
from gateway.providers.mock import MockPipeline
from gateway.session import GatewaySession
from simulator.device import run_simulation


class _WeatherProvider:
    def __init__(self, reading=None, error: Exception | None = None) -> None:
        self.reading = reading or {
            "temperature_c": 26.7,
            "apparent_temperature_c": 31.0,
            "humidity_percent": 80,
            "weather_code": 0,
        }
        self.error = error
        self.locations: list[str] = []

    async def current(self, location: str):
        self.locations.append(location)
        if self.error is not None:
            raise self.error
        return dict(self.reading)


class _Fallback:
    def __init__(self) -> None:
        self.calls: list[str] = []
        self.closed = False
        self.tool_registry = object()

    async def answer_chunks(self, text: str):
        self.calls.append(text)
        yield "来自 MiMo。"

    async def aclose(self) -> None:
        self.closed = True


class _RecordingTts:
    def __init__(self) -> None:
        self.texts: list[str] = []

    async def synthesize(self, text: str):
        self.texts.append(text)
        yield b"\x01\x00" * 160


class FastWeatherTests(unittest.IsolatedAsyncioTestCase):
    def test_extracts_supported_single_city_current_questions(self) -> None:
        cases = {
            "北京今天天气怎么样": "北京",
            "查一下上海今天的天气": "上海",
            "请告诉我海淀区现在的天气": "海淀区",
            "我想知道深圳气温": "深圳",
            "和田天气怎么样": "和田",
            "呼和浩特现在天气": "呼和浩特",
        }
        for text, expected in cases.items():
            with self.subTest(text=text):
                self.assertEqual(extract_current_weather_location(text), expected)

    def test_rejects_ambiguous_or_non_current_questions(self) -> None:
        rejected = (
            "今天天气怎么样",
            "北京天气并设置提醒",
            "北京和上海天气怎么样",
            "北京明天天气怎么样",
            "北京未来一周天气",
            "北京天气和音乐",
        )
        for text in rejected:
            with self.subTest(text=text):
                self.assertIsNone(extract_current_weather_location(text))

    def test_formats_one_short_chinese_sentence(self) -> None:
        answer = format_current_weather(
            "北京",
            {
                "temperature_c": 26.7,
                "apparent_temperature_c": 31.0,
                "humidity_percent": 80,
                "weather_code": 0,
            },
        )
        self.assertEqual(answer, "北京现在26.7度，体感31度，晴朗，湿度80%。")

    async def test_hit_calls_weather_only_and_yields_one_chunk(self) -> None:
        weather = _WeatherProvider()
        fallback = _Fallback()
        llm = FastWeatherLlm(fallback, weather)

        chunks = [chunk async for chunk in llm.answer_chunks("北京今天天气怎么样")]

        self.assertEqual(chunks, ["北京现在26.7度，体感31度，晴朗，湿度80%。"])
        self.assertEqual(weather.locations, ["北京"])
        self.assertEqual(fallback.calls, [])

    async def test_non_match_delegates_to_mimo(self) -> None:
        weather = _WeatherProvider()
        fallback = _Fallback()
        llm = FastWeatherLlm(fallback, weather)

        chunks = [chunk async for chunk in llm.answer_chunks("讲个笑话")]

        self.assertEqual(chunks, ["来自 MiMo。"])
        self.assertEqual(weather.locations, [])
        self.assertEqual(fallback.calls, ["讲个笑话"])

    async def test_weather_failure_falls_back_without_leaking_error(self) -> None:
        weather = _WeatherProvider(error=RuntimeError("upstream secret"))
        fallback = _Fallback()
        llm = FastWeatherLlm(fallback, weather)

        chunks = [chunk async for chunk in llm.answer_chunks("北京天气")]

        self.assertEqual(chunks, ["来自 MiMo。"])
        self.assertEqual(fallback.calls, ["北京天气"])

    async def test_aclose_closes_underlying_llm(self) -> None:
        fallback = _Fallback()
        await FastWeatherLlm(fallback, _WeatherProvider()).aclose()
        self.assertTrue(fallback.closed)

    async def test_gateway_sends_fast_answer_to_tts_once(self) -> None:
        tts = _RecordingTts()
        answer = FastWeatherLlm(_Fallback(), _WeatherProvider())
        pipeline = CompositePipeline(
            asr_source=MockPipeline(latency_ms=0),
            llm=answer,
            tts_source=tts,
        )

        async def handler(websocket):
            await GatewaySession(websocket, pipeline=pipeline).run()

        async with serve(handler, "127.0.0.1", 0) as server:
            port = server.sockets[0].getsockname()[1]
            result = await run_simulation(
                f"ws://127.0.0.1:{port}",
                text="北京今天天气怎么样",
                duration_ms=40,
            )

        expected = "北京现在26.7度，体感31度，晴朗，湿度80%。"
        self.assertEqual(result.answer, expected)
        self.assertEqual(tts.texts, [expected])


if __name__ == "__main__":
    unittest.main()
