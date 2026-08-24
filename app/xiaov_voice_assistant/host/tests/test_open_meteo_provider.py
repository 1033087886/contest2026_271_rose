"""Offline tests for the bounded Open-Meteo weather provider."""

from __future__ import annotations

import asyncio
import json
import unittest

import httpx

from gateway.tools.open_meteo import OpenMeteoWeatherProvider
from gateway.tools.registry import ToolExecutionError
from gateway.tools.weather import weather_tool


def geocoding_payload() -> dict:
    return {
        "results": [
            {
                "name": "Beijing",
                "latitude": 39.9042,
                "longitude": 116.4074,
                "country": "China",
                "admin1": "Beijing",
            }
        ]
    }


def forecast_payload() -> dict:
    return {
        "timezone": "Asia/Shanghai",
        "current": {
            "time": "2026-08-05T11:15",
            "temperature_2m": 28.5,
            "relative_humidity_2m": 62,
            "apparent_temperature": 30.1,
            "precipitation": 0.0,
            "weather_code": 2,
            "wind_speed_10m": 8.4,
            "is_day": 1,
        },
    }


def json_response(payload: object) -> httpx.Response:
    return httpx.Response(200, json=payload)


class OpenMeteoWeatherProviderTests(unittest.IsolatedAsyncioTestCase):
    async def test_geocodes_then_fetches_bounded_current_weather(self) -> None:
        requests: list[httpx.Request] = []

        def handler(request: httpx.Request) -> httpx.Response:
            requests.append(request)
            if "geocoding" in request.url.host:
                return json_response(geocoding_payload())
            return json_response(forecast_payload())

        async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
            provider = OpenMeteoWeatherProvider(client=client)
            result = await provider.current("  北京  ")

        self.assertEqual(len(requests), 2)
        self.assertEqual(requests[0].url.params["name"], "北京")
        self.assertEqual(requests[0].url.params["count"], "1")
        self.assertIn("temperature_2m", requests[1].url.params["current"])
        self.assertEqual(requests[1].url.params["temperature_unit"], "celsius")
        self.assertEqual(result["condition"], "partly cloudy")
        self.assertEqual(result["temperature_c"], 28.5)
        self.assertEqual(result["humidity_percent"], 62)
        self.assertEqual(result["resolved_name"], "Beijing")
        self.assertEqual(result["source"], "open_meteo")
        self.assertFalse(result["simulated"])

    async def test_reuses_current_weather_and_geocoding_within_ttl(self) -> None:
        requests: list[httpx.Request] = []

        def handler(request: httpx.Request) -> httpx.Response:
            requests.append(request)
            if "geocoding" in request.url.host:
                return json_response(geocoding_payload())
            return json_response(forecast_payload())

        async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
            provider = OpenMeteoWeatherProvider(client=client)
            first = await provider.current(" 北京 ")
            second = await provider.current("北京")

        self.assertEqual(first, second)
        self.assertEqual(len(requests), 2)

    async def test_refreshes_current_weather_after_ttl_but_reuses_coordinates(self) -> None:
        requests: list[httpx.Request] = []

        def handler(request: httpx.Request) -> httpx.Response:
            requests.append(request)
            if "geocoding" in request.url.host:
                return json_response(geocoding_payload())
            return json_response(forecast_payload())

        async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
            provider = OpenMeteoWeatherProvider(
                client=client,
                current_cache_ttl_seconds=0.01,
            )
            await provider.current("北京")
            await asyncio.sleep(0.02)
            await provider.current("北京")

        self.assertEqual(
            ["geocoding" in request.url.host for request in requests],
            [True, False, False],
        )

    async def test_cache_is_bounded(self) -> None:
        def handler(request: httpx.Request) -> httpx.Response:
            if "geocoding" in request.url.host:
                return json_response(geocoding_payload())
            return json_response(forecast_payload())

        async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
            provider = OpenMeteoWeatherProvider(client=client, cache_max_entries=2)
            for location in ("北京", "上海", "广州"):
                await provider.current(location)

        self.assertEqual(len(provider._geocoding_cache), 2)
        self.assertEqual(len(provider._current_cache), 2)

    async def test_is_compatible_with_weather_tool_unit_conversion(self) -> None:
        def handler(request: httpx.Request) -> httpx.Response:
            return (
                json_response(geocoding_payload())
                if "geocoding" in request.url.host
                else json_response(forecast_payload())
            )

        async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
            tool = weather_tool(OpenMeteoWeatherProvider(client=client))
            result = await tool({"location": "北京", "unit": "fahrenheit"})

        self.assertEqual(result["temperature"], 83.3)
        self.assertEqual(result["unit"], "fahrenheit")
        self.assertEqual(result["source"], "open_meteo")

    async def test_location_not_found_is_a_safe_tool_error(self) -> None:
        async with httpx.AsyncClient(
            transport=httpx.MockTransport(lambda _request: json_response({}))
        ) as client:
            provider = OpenMeteoWeatherProvider(client=client)
            with self.assertRaises(ToolExecutionError) as raised:
                await provider.current("no such place")

        self.assertEqual(raised.exception.code, "location_not_found")

    async def test_rejects_invalid_input_without_an_http_request(self) -> None:
        calls = 0

        def handler(_request: httpx.Request) -> httpx.Response:
            nonlocal calls
            calls += 1
            return json_response(geocoding_payload())

        invalid = ("", "   ", "a\ncity", "x" * 101, "\ud800")
        async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
            provider = OpenMeteoWeatherProvider(client=client)
            for location in invalid:
                with self.subTest(location=ascii(location)):
                    with self.assertRaises(ToolExecutionError) as raised:
                        await provider.current(location)
                    self.assertEqual(raised.exception.code, "invalid_arguments")
        self.assertEqual(calls, 0)

    async def test_rejects_oversized_and_malformed_responses(self) -> None:
        cases = (
            httpx.Response(
                200,
                content=b"x" * 257,
                headers={"Content-Type": "application/json"},
            ),
            httpx.Response(
                200,
                content=b"{broken",
                headers={"Content-Type": "application/json"},
            ),
            httpx.Response(200, text="{}", headers={"Content-Type": "text/plain"}),
        )
        for response in cases:
            with self.subTest(response=response):
                async with httpx.AsyncClient(
                    transport=httpx.MockTransport(lambda _request, r=response: r)
                ) as client:
                    provider = OpenMeteoWeatherProvider(
                        client=client, max_response_bytes=256
                    )
                    with self.assertRaises(ToolExecutionError) as raised:
                        await provider.current("Beijing")
                self.assertIn(
                    raised.exception.code,
                    {"weather_response_too_large", "weather_invalid_response"},
                )

    async def test_rejects_invalid_forecast_fields(self) -> None:
        bad_forecast = forecast_payload()
        bad_forecast["current"]["relative_humidity_2m"] = 101

        def handler(request: httpx.Request) -> httpx.Response:
            return (
                json_response(geocoding_payload())
                if "geocoding" in request.url.host
                else json_response(bad_forecast)
            )

        async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
            provider = OpenMeteoWeatherProvider(client=client)
            with self.assertRaises(ToolExecutionError) as raised:
                await provider.current("Beijing")
        self.assertEqual(raised.exception.code, "weather_invalid_response")

    async def test_http_and_transport_failures_do_not_leak_details(self) -> None:
        secret = "private-upstream-token"

        def http_failure(_request: httpx.Request) -> httpx.Response:
            return httpx.Response(500, content=secret.encode())

        def transport_failure(request: httpx.Request) -> httpx.Response:
            raise httpx.ConnectError(secret, request=request)

        for handler in (http_failure, transport_failure):
            async with httpx.AsyncClient(
                transport=httpx.MockTransport(handler)
            ) as client:
                provider = OpenMeteoWeatherProvider(client=client)
                with self.assertRaises(ToolExecutionError) as raised:
                    await provider.current("Beijing")
            self.assertEqual(raised.exception.code, "weather_unavailable")
            self.assertNotIn(secret, str(raised.exception))

    async def test_timeout_closes_stream_and_returns_safe_error(self) -> None:
        stream = _BlockingStream()

        def handler(_request: httpx.Request) -> httpx.Response:
            return httpx.Response(
                200,
                headers={"Content-Type": "application/json"},
                stream=stream,
            )

        async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
            provider = OpenMeteoWeatherProvider(client=client, timeout_seconds=0.01)
            with self.assertRaises(ToolExecutionError) as raised:
                await provider.current("Beijing")
        self.assertEqual(raised.exception.code, "weather_timeout")
        self.assertTrue(stream.closed.is_set())

    async def test_geocoding_and_forecast_each_receive_the_timeout_budget(self) -> None:
        async def handler(request: httpx.Request) -> httpx.Response:
            await asyncio.sleep(0.1)
            return (
                json_response(geocoding_payload())
                if "geocoding" in request.url.host
                else json_response(forecast_payload())
            )

        async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
            provider = OpenMeteoWeatherProvider(
                client=client, timeout_seconds=0.18
            )
            result = await provider.current("Beijing")

        self.assertEqual(result["resolved_name"], "Beijing")

    async def test_call_cancellation_propagates_and_closes_stream(self) -> None:
        stream = _BlockingStream()

        def handler(_request: httpx.Request) -> httpx.Response:
            return httpx.Response(
                200,
                headers={"Content-Type": "application/json"},
                stream=stream,
            )

        async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
            provider = OpenMeteoWeatherProvider(client=client, timeout_seconds=10)
            task = asyncio.create_task(provider.current("Beijing"))
            await asyncio.wait_for(stream.started.wait(), 1)
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
            await asyncio.wait_for(stream.closed.wait(), 1)


class _BlockingStream(httpx.AsyncByteStream):
    def __init__(self) -> None:
        self.started = asyncio.Event()
        self.closed = asyncio.Event()

    async def __aiter__(self):
        self.started.set()
        try:
            await asyncio.Future()
            yield json.dumps(geocoding_payload()).encode()
        finally:
            self.closed.set()

    async def aclose(self) -> None:
        self.closed.set()


if __name__ == "__main__":
    unittest.main()
