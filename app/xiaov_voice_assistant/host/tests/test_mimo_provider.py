"""Tests for the MiMo provider using httpx MockTransport.

No network access and no API key are required: MockTransport serves canned SSE
bodies, so these run in CI and offline. The point is to pin down SSE parsing,
error handling and the guarantee that a key never reaches a log or exception.
"""

from __future__ import annotations

import asyncio
import json
import unittest

import httpx

from gateway.providers.composite import CompositePipeline as CanonicalCompositePipeline
from gateway.providers.mimo import CompositePipeline, MimoError, MimoLlm
from gateway.providers.mock import MockPipeline
from gateway.tools import ToolRegistry

KEY = "sk-test-not-a-real-key"


def sse(*events: dict | str) -> bytes:
    lines = []
    for item in events:
        payload = item if isinstance(item, str) else json.dumps(item, ensure_ascii=False)
        lines.append(f"data: {payload}\n\n")
    return "".join(lines).encode("utf-8")


def delta(content: str) -> dict:
    return {"choices": [{"index": 0, "delta": {"content": content}}]}


def tool_delta(*calls: dict) -> dict:
    return {"choices": [{"index": 0, "delta": {"tool_calls": list(calls)}}]}


def tool_call(
    index: int,
    *,
    call_id: str | None = None,
    name: str | None = None,
    arguments: str | None = None,
) -> dict:
    call: dict = {"index": index}
    if call_id is not None:
        call["id"] = call_id
        call["type"] = "function"
    function = {}
    if name is not None:
        function["name"] = name
    if arguments is not None:
        function["arguments"] = arguments
    if function:
        call["function"] = function
    return call


def echo_registry(handler=None) -> ToolRegistry:
    async def default_handler(arguments):
        return arguments

    registry = ToolRegistry()
    registry.add(
        name="echo",
        description="Echo one value.",
        parameters={
            "type": "object",
            "properties": {"value": {"type": "string"}},
            "required": ["value"],
            "additionalProperties": False,
        },
        handler=handler or default_handler,
    )
    return registry


def client_for(handler) -> httpx.AsyncClient:
    return httpx.AsyncClient(transport=httpx.MockTransport(handler))


async def collect(llm: MimoLlm, text: str = "今天天气怎么样") -> list[str]:
    return [chunk async for chunk in llm.answer_chunks(text)]


class MimoStreamingTests(unittest.IsolatedAsyncioTestCase):
    async def test_streams_content_deltas_in_order(self) -> None:
        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(
                200,
                content=sse(delta("北京"), delta("今天"), delta("晴。"), "[DONE]"),
                headers={"Content-Type": "text/event-stream"},
            )

        async with client_for(handler) as client:
            llm = MimoLlm(api_key=KEY, client=client)
            self.assertEqual(await collect(llm), ["北京", "今天", "晴。"])

    async def test_yields_text_before_the_http_stream_finishes(self) -> None:
        release_tail = asyncio.Event()

        class GatedStream(httpx.AsyncByteStream):
            async def __aiter__(self):
                yield sse(delta("第一段"))
                await release_tail.wait()
                yield sse(delta("第二段"), "[DONE]")

        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(
                200,
                stream=GatedStream(),
                headers={"Content-Type": "text/event-stream"},
            )

        async with client_for(handler) as client:
            llm = MimoLlm(api_key=KEY, client=client)
            iterator = llm.answer_chunks("测试流式输出")
            first = await asyncio.wait_for(anext(iterator), timeout=0.2)
            self.assertEqual(first, "第一段")
            self.assertFalse(release_tail.is_set())
            release_tail.set()
            self.assertEqual([item async for item in iterator], ["第二段"])

    async def test_sends_streaming_request_with_api_key_header(self) -> None:
        seen: dict = {}

        def handler(request: httpx.Request) -> httpx.Response:
            seen["url"] = str(request.url)
            seen["api_key"] = request.headers.get("api-key")
            seen["body"] = json.loads(request.content)
            return httpx.Response(200, content=sse(delta("好"), "[DONE]"))

        async with client_for(handler) as client:
            llm = MimoLlm(api_key=KEY, client=client, model="mimo-v2.5-pro")
            await collect(llm)

        self.assertTrue(seen["url"].endswith("/chat/completions"))
        self.assertEqual(seen["api_key"], KEY)
        self.assertTrue(seen["body"]["stream"])
        self.assertEqual(seen["body"]["model"], "mimo-v2.5-pro")
        # max_completion_tokens, not max_tokens, on the OpenAI-format endpoint.
        self.assertIn("max_completion_tokens", seen["body"])
        roles = [message["role"] for message in seen["body"]["messages"]]
        self.assertEqual(roles, ["system", "user"])

    async def test_tp_key_automatically_uses_token_plan_bearer_endpoint(self) -> None:
        seen = {}

        def handler(request: httpx.Request) -> httpx.Response:
            seen["url"] = str(request.url)
            seen["authorization"] = request.headers.get("Authorization")
            seen["api_key"] = request.headers.get("api-key")
            return httpx.Response(200, content=sse(delta("OK"), "[DONE]"))

        client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
        try:
            llm = MimoLlm(api_key="tp-test-key-12345678", client=client)
            self.assertEqual(await collect(llm, "hello"), ["OK"])
        finally:
            await client.aclose()

        self.assertTrue(seen["url"].startswith("https://token-plan-cn.xiaomimimo.com/v1/"))
        self.assertEqual(seen["authorization"], "Bearer tp-test-key-12345678")
        self.assertIsNone(seen["api_key"])

    async def test_token_plan_endpoint_uses_bearer_authentication(self) -> None:
        seen: dict = {}

        def handler(request: httpx.Request) -> httpx.Response:
            seen["authorization"] = request.headers.get("Authorization")
            seen["api_key"] = request.headers.get("api-key")
            return httpx.Response(200, content=sse(delta("OK"), "[DONE]"))

        async with client_for(handler) as client:
            llm = MimoLlm(
                api_key=KEY,
                base_url="https://token-plan-cn.xiaomimimo.com/v1",
                client=client,
            )
            await collect(llm)

        self.assertEqual(seen["authorization"], f"Bearer {KEY}")
        self.assertIsNone(seen["api_key"])

    async def test_skips_reasoning_content(self) -> None:
        """Thinking output must not reach the screen or TTS."""

        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(
                200,
                content=sse(
                    {"choices": [{"delta": {"reasoning_content": "用户在问天气……"}}]},
                    delta("晴天。"),
                    "[DONE]",
                ),
            )

        async with client_for(handler) as client:
            llm = MimoLlm(api_key=KEY, client=client)
            self.assertEqual(await collect(llm), ["晴天。"])

    async def test_tolerates_keepalive_and_malformed_lines(self) -> None:
        def handler(request: httpx.Request) -> httpx.Response:
            body = (
                b": keep-alive\n\n"
                b"\n"
                b"data: {not valid json}\n\n"
                b"data: " + json.dumps(delta("\xe5\xa5\xbd")).encode() + b"\n\n"
                b"data: [DONE]\n\n"
            )
            return httpx.Response(200, content=body)

        async with client_for(handler) as client:
            llm = MimoLlm(api_key=KEY, client=client)
            self.assertEqual(len(await collect(llm)), 1)

    async def test_http_error_does_not_leak_the_key(self) -> None:
        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(
                401,
                content=(
                    f"Authorization: Bearer {KEY}; api-key={KEY}; invalid credential"
                ).encode(),
            )

        async with client_for(handler) as client:
            llm = MimoLlm(api_key=KEY, client=client)
            with self.assertRaises(MimoError) as caught:
                await collect(llm)
        message = str(caught.exception)
        self.assertIn("401", message)
        self.assertNotIn(KEY, message)
        self.assertIn("Authorization: Bearer <redacted>", message)
        self.assertIn("api-key=<redacted>", message)

    async def test_http_error_redacts_before_truncating_long_detail(self) -> None:
        prefix = "x" * 176 + " Authorization: Bearer "

        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(500, content=(prefix + KEY + " trailing" * 100).encode())

        async with client_for(handler) as client:
            llm = MimoLlm(api_key=KEY, client=client)
            with self.assertRaises(MimoError) as caught:
                await collect(llm)

        detail = str(caught.exception).split(": ", 1)[1]
        self.assertEqual(len(detail), 200)
        self.assertNotIn(KEY, detail)
        self.assertNotIn(KEY[:2], detail)

    async def test_transport_error_does_not_leak_the_key(self) -> None:
        def handler(request: httpx.Request) -> httpx.Response:
            raise httpx.ConnectError("connection refused", request=request)

        async with client_for(handler) as client:
            llm = MimoLlm(api_key=KEY, client=client)
            with self.assertRaises(MimoError) as caught:
                await collect(llm)
        self.assertNotIn(KEY, str(caught.exception))

    async def test_empty_stream_is_an_error(self) -> None:
        """A silent empty answer would strand the device in thinking state."""

        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(200, content=sse("[DONE]"))

        async with client_for(handler) as client:
            llm = MimoLlm(api_key=KEY, client=client)
            with self.assertRaises(MimoError):
                await collect(llm)

    def test_missing_key_fails_fast(self) -> None:
        with self.assertRaises(MimoError):
            MimoLlm(api_key="")


class MimoToolCallingTests(unittest.IsolatedAsyncioTestCase):
    async def test_video_stop_is_repaired_from_audio_media_tool(self) -> None:
        requests: list[dict] = []
        handled: list[tuple[str, dict]] = []
        registry = ToolRegistry()

        async def media(arguments):
            handled.append(("media", arguments))
            return {"executed": True}

        async def monitor(arguments):
            handled.append(("monitor", arguments))
            return {"executed": True}

        def action_schema(actions):
            return {
                "type": "object",
                "properties": {"action": {"type": "string", "enum": actions}},
                "required": ["action"],
                "additionalProperties": False,
            }
        registry.add(
            name="control_media",
            description="Control audio only.",
            parameters=action_schema(["stop"]),
            handler=media,
        )
        registry.add(
            name="control_monitor",
            description="Control video.",
            parameters=action_schema(["stop_video"]),
            handler=monitor,
        )

        def handler(request: httpx.Request) -> httpx.Response:
            body = json.loads(request.content)
            requests.append(body)
            if len(requests) == 1:
                return httpx.Response(
                    200,
                    content=sse(
                        tool_delta(
                            tool_call(
                                0,
                                call_id="stop_1",
                                name="control_media",
                                arguments='{"action":"stop"}',
                            )
                        ),
                        "[DONE]",
                    ),
                )
            return httpx.Response(200, content=sse(delta("视频已停止。"), "[DONE]"))

        async with client_for(handler) as client:
            llm = MimoLlm(api_key=KEY, client=client, tool_registry=registry)
            self.assertEqual(await collect(llm, "停止视频。"), ["视频已停止。"])

        self.assertEqual(handled, [("monitor", {"action": "stop_video"})])
        repaired = requests[1]["messages"][-2]["tool_calls"][0]["function"]
        self.assertEqual(repaired["name"], "control_monitor")
        self.assertEqual(json.loads(repaired["arguments"]), {"action": "stop_video"})

    async def test_video_context_audio_stop_is_not_repaired(self) -> None:
        handled: list[tuple[str, dict]] = []
        request_count = 0
        registry = ToolRegistry()

        async def media(arguments):
            handled.append(("media", arguments))
            return {"executed": True}

        registry.add(
            name="control_media",
            description="Control audio only.",
            parameters={
                "type": "object",
                "properties": {
                    "action": {"type": "string", "enum": ["stop"]}
                },
                "required": ["action"],
                "additionalProperties": False,
            },
            handler=media,
        )

        def handler(_request: httpx.Request) -> httpx.Response:
            nonlocal request_count
            request_count += 1
            if request_count == 1:
                return httpx.Response(
                    200,
                    content=sse(
                        tool_delta(
                            tool_call(
                                0,
                                call_id="stop_audio",
                                name="control_media",
                                arguments='{"action":"stop"}',
                            )
                        ),
                        "[DONE]",
                    ),
                )
            return httpx.Response(200, content=sse(delta("音乐已停止。"), "[DONE]"))

        async with client_for(handler) as client:
            llm = MimoLlm(api_key=KEY, client=client, tool_registry=registry)
            self.assertEqual(
                await collect(llm, "停止视频里的音乐。"), ["音乐已停止。"]
            )

        self.assertEqual(handled, [("media", {"action": "stop"})])

    async def test_reconstructs_fragmented_call_and_streams_both_text_rounds(self) -> None:
        requests: list[dict] = []
        handled: list[dict] = []

        async def echo(arguments):
            handled.append(arguments)
            return {"echoed": arguments["value"]}

        def handler(request: httpx.Request) -> httpx.Response:
            body = json.loads(request.content)
            requests.append(body)
            if len(requests) == 1:
                return httpx.Response(
                    200,
                    content=sse(
                        delta("我查一下。"),
                        tool_delta(
                            tool_call(
                                0,
                                call_id="call_1",
                                name="ec",
                                arguments='{"val',
                            )
                        ),
                        tool_delta(
                            tool_call(0, name="ho", arguments='ue":"天气"}')
                        ),
                        "[DONE]",
                    ),
                )
            tool_message = body["messages"][-1]
            self.assertEqual(tool_message["role"], "tool")
            self.assertEqual(tool_message["tool_call_id"], "call_1")
            self.assertEqual(json.loads(tool_message["content"])["result"], {"echoed": "天气"})
            assistant = body["messages"][-2]
            self.assertEqual(assistant["content"], "我查一下。")
            self.assertEqual(assistant["tool_calls"][0]["function"]["name"], "echo")
            self.assertEqual(
                assistant["tool_calls"][0]["function"]["arguments"],
                '{"value":"天气"}',
            )
            return httpx.Response(200, content=sse(delta("今天晴。"), "[DONE]"))

        async with client_for(handler) as client:
            llm = MimoLlm(
                api_key=KEY, client=client, tool_registry=echo_registry(echo)
            )
            chunks = await collect(llm)

        self.assertEqual(chunks, ["我查一下。", "今天晴。"])
        self.assertEqual(handled, [{"value": "天气"}])
        self.assertEqual(len(requests), 2)
        self.assertEqual(requests[0]["tool_choice"], "auto")
        self.assertEqual(requests[0]["tools"][0]["function"]["name"], "echo")

    async def test_invalid_and_unknown_calls_are_returned_for_model_recovery(self) -> None:
        requests: list[dict] = []
        called = False

        async def echo(arguments):
            nonlocal called
            called = True
            return arguments

        def handler(request: httpx.Request) -> httpx.Response:
            body = json.loads(request.content)
            requests.append(body)
            if len(requests) == 1:
                return httpx.Response(
                    200,
                    content=sse(
                        tool_delta(
                            tool_call(0, call_id="bad_args", name="echo", arguments="{}"),
                            tool_call(
                                1,
                                call_id="bad_name",
                                name="not_registered",
                                arguments="{}",
                            ),
                        ),
                        "[DONE]",
                    ),
                )
            results = [
                json.loads(message["content"])
                for message in body["messages"]
                if message["role"] == "tool"
            ]
            self.assertEqual(
                [item["error"]["code"] for item in results],
                ["invalid_arguments", "unknown_tool"],
            )
            return httpx.Response(200, content=sse(delta("无法执行。"), "[DONE]"))

        async with client_for(handler) as client:
            llm = MimoLlm(
                api_key=KEY, client=client, tool_registry=echo_registry(echo)
            )
            self.assertEqual(await collect(llm), ["无法执行。"])
        self.assertFalse(called)

    async def test_tool_timeout_is_a_result_and_does_not_abort_answer(self) -> None:
        cancelled = False
        request_count = 0

        async def slow(_arguments):
            nonlocal cancelled
            try:
                await asyncio.sleep(10)
            finally:
                cancelled = True

        def handler(request: httpx.Request) -> httpx.Response:
            nonlocal request_count
            request_count += 1
            if request_count == 1:
                return httpx.Response(
                    200,
                    content=sse(
                        tool_delta(
                            tool_call(
                                0,
                                call_id="slow_1",
                                name="echo",
                                arguments='{"value":"x"}',
                            )
                        ),
                        "[DONE]",
                    ),
                )
            body = json.loads(request.content)
            result = json.loads(body["messages"][-1]["content"])
            self.assertEqual(result["error"]["code"], "timeout")
            return httpx.Response(200, content=sse(delta("工具超时。"), "[DONE]"))

        async with client_for(handler) as client:
            llm = MimoLlm(
                api_key=KEY,
                client=client,
                tool_registry=echo_registry(slow),
                tool_timeout_seconds=0.01,
            )
            self.assertEqual(await collect(llm), ["工具超时。"])
        self.assertTrue(cancelled)

    async def test_repeated_tool_requests_stop_at_round_limit(self) -> None:
        request_count = 0
        execution_count = 0

        async def echo(arguments):
            nonlocal execution_count
            execution_count += 1
            return arguments

        def handler(request: httpx.Request) -> httpx.Response:
            nonlocal request_count
            request_count += 1
            return httpx.Response(
                200,
                content=sse(
                    tool_delta(
                        tool_call(
                            0,
                            call_id=f"call_{request_count}",
                            name="echo",
                            arguments='{"value":"again"}',
                        )
                    ),
                    "[DONE]",
                ),
            )

        async with client_for(handler) as client:
            llm = MimoLlm(
                api_key=KEY,
                client=client,
                tool_registry=echo_registry(echo),
                max_tool_rounds=1,
            )
            with self.assertRaisesRegex(MimoError, "tool-round limit"):
                await collect(llm)
        self.assertEqual(request_count, 2)
        self.assertEqual(execution_count, 1)

    async def test_tools_per_round_and_argument_size_are_bounded(self) -> None:
        responses = [
            sse(
                tool_delta(
                    tool_call(0, call_id="one", name="echo", arguments='{"value":"1"}'),
                    tool_call(1, call_id="two", name="echo", arguments='{"value":"2"}'),
                ),
                "[DONE]",
            ),
            sse(
                tool_delta(
                    tool_call(
                        0,
                        call_id="large",
                        name="echo",
                        arguments='{"value":"' + "x" * 300 + '"}',
                    )
                ),
                "[DONE]",
            ),
        ]

        for response, kwargs, message in (
            (responses[0], {"max_tool_calls_per_round": 1}, "tools-per-round"),
            (responses[1], {"max_tool_argument_chars": 256}, "oversized tool arguments"),
        ):
            async with client_for(
                lambda _request, response=response: httpx.Response(200, content=response)
            ) as client:
                llm = MimoLlm(
                    api_key=KEY,
                    client=client,
                    tool_registry=echo_registry(),
                    **kwargs,
                )
                with self.assertRaisesRegex(MimoError, message):
                    await collect(llm)


class CompositePipelineTests(unittest.IsolatedAsyncioTestCase):
    def test_legacy_import_reexports_canonical_pipeline(self) -> None:
        self.assertIs(CompositePipeline, CanonicalCompositePipeline)

    async def test_real_llm_with_mock_asr_and_tts(self) -> None:
        """Step 4 of the bring-up checklist: swap one provider at a time."""

        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(200, content=sse(delta("晴，"), delta("26 度。"), "[DONE]"))

        mock = MockPipeline(latency_ms=0)
        async with client_for(handler) as client:
            pipeline = CompositePipeline(
                asr_source=mock,
                llm=MimoLlm(api_key=KEY, client=client),
                tts_source=mock,
            )
            asr = pipeline.start_asr()
            await asr.push_audio(bytes(640))
            transcript = await asr.finish("今天天气")
            self.assertTrue(transcript)

            answer = "".join([chunk async for chunk in pipeline.answer_chunks(transcript)])
            self.assertEqual(answer, "晴，26 度。")

            pcm = b"".join([frame async for frame in pipeline.synthesize(answer)])
            self.assertGreater(len(pcm), 0)
            self.assertEqual(len(pcm) % 2, 0)


if __name__ == "__main__":
    unittest.main()
