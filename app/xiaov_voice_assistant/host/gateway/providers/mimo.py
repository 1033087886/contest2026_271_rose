"""Xiaomi MiMo LLM provider over the hosted OpenAI-compatible API.

Only the answer stage is implemented here. ASR and TTS stay pluggable so each
one can be swapped in separately with mock as the regression path, as
docs/board-bringup.md requires.

Streaming matters for the two-second target: the gateway forwards each delta to
the device and starts synthesising as soon as a sentence closes, so wall-clock
latency is bounded by time-to-first-sentence rather than the full answer.

Credentials come from the environment only. A key must never be written to the
repository, a log line, or an error message.
"""

from __future__ import annotations

import json
import logging
import os
from collections.abc import AsyncIterator
from dataclasses import dataclass
from typing import Any
from urllib.parse import urlsplit

import httpx

from gateway.providers.composite import CompositePipeline
from gateway.tools import ToolRegistry

DEFAULT_BASE_URL = "https://api.xiaomimimo.com/v1"
DEFAULT_MODEL = "mimo-v2.5-pro"
TOKEN_PLAN_HOST = "token-plan-cn.xiaomimimo.com"
TOKEN_PLAN_BASE_URL = f"https://{TOKEN_PLAN_HOST}/v1"
LOGGER = logging.getLogger(__name__)
DEFAULT_SYSTEM_PROMPT = (
    "你是桌面语音助手小 V，运行在一块小屏幕的嵌入式设备上。"
    "回答要口语化、简短，通常两三句话，因为回答会被朗读出来。"
    "不要使用表格或代码块。"
)


class MimoError(RuntimeError):
    """Raised when the MiMo API cannot be used or returns an unusable reply."""


class MimoLlm:
    """Streaming chat client for the MiMo hosted API.

    Deliberately not a full VoicePipeline: compose it with mock or real ASR/TTS
    via CompositePipeline so providers can be replaced one at a time.
    """

    def __init__(
        self,
        *,
        api_key: str | None = None,
        base_url: str | None = None,
        model: str = DEFAULT_MODEL,
        system_prompt: str = DEFAULT_SYSTEM_PROMPT,
        timeout_seconds: float = 30.0,
        max_output_tokens: int = 512,
        tool_registry: ToolRegistry | None = None,
        tool_timeout_seconds: float = 5.0,
        max_tool_rounds: int = 3,
        max_tool_calls_per_round: int = 8,
        max_tool_argument_chars: int = 16_384,
        client: httpx.AsyncClient | None = None,
    ) -> None:
        key = api_key or os.environ.get("MIMO_API_KEY")
        if not key:
            raise MimoError(
                "no MiMo API key; set MIMO_API_KEY or pass api_key explicitly"
            )
        self._api_key = key
        configured_base_url = base_url or os.environ.get("MIMO_BASE_URL")
        resolved_base_url = configured_base_url or (
            TOKEN_PLAN_BASE_URL if key.startswith("tp-") else DEFAULT_BASE_URL
        )
        self.base_url = resolved_base_url.rstrip("/")
        self.model = model
        self.system_prompt = system_prompt
        self.timeout_seconds = timeout_seconds
        self.max_output_tokens = max_output_tokens
        if tool_timeout_seconds <= 0:
            raise ValueError("tool_timeout_seconds must be positive")
        if max_tool_rounds < 0:
            raise ValueError("max_tool_rounds cannot be negative")
        if max_tool_calls_per_round < 1:
            raise ValueError("max_tool_calls_per_round must be positive")
        if max_tool_argument_chars < 256:
            raise ValueError("max_tool_argument_chars must be at least 256")
        self.tool_registry = tool_registry
        self.tool_timeout_seconds = tool_timeout_seconds
        self.max_tool_rounds = max_tool_rounds
        self.max_tool_calls_per_round = max_tool_calls_per_round
        self.max_tool_argument_chars = max_tool_argument_chars
        self._client = client
        self._owns_client = client is None

    async def aclose(self) -> None:
        if self._client is not None and self._owns_client:
            await self._client.aclose()
            self._client = None

    def _ensure_client(self) -> httpx.AsyncClient:
        if self._client is None:
            self._client = httpx.AsyncClient(timeout=self.timeout_seconds)
        return self._client

    def _request_headers(self) -> dict[str, str]:
        headers = {"Content-Type": "application/json"}
        if urlsplit(self.base_url).hostname == TOKEN_PLAN_HOST:
            # Competition tokens are accepted by the token-plan endpoint only
            # through OpenAI-style Bearer authentication. The general MiMo API
            # uses its native api-key header instead.
            headers["Authorization"] = f"Bearer {self._api_key}"
        else:
            headers["api-key"] = self._api_key
        return headers

    async def answer_chunks(self, text: str) -> AsyncIterator[str]:
        """Yields answer deltas while resolving bounded function-call rounds.

        Raises MimoError without echoing the key or the raw Authorization header,
        so a failing turn cannot leak credentials into gateway logs.
        """
        messages: list[dict[str, Any]] = [
            {"role": "system", "content": self.system_prompt},
            {"role": "user", "content": text},
        ]
        tool_definitions = (
            self.tool_registry.openai_tools()
            if self.tool_registry is not None and self.max_tool_rounds > 0
            else []
        )
        client = self._ensure_client()
        tool_rounds = 0

        while True:
            request: dict[str, Any] = {
                "model": self.model,
                "messages": messages,
                "max_completion_tokens": self.max_output_tokens,
                "stream": True,
            }
            if tool_definitions:
                request["tools"] = tool_definitions
                request["tool_choice"] = "auto"

            content_parts: list[str] = []
            call_parts: dict[int, _ToolCallParts] = {}
            try:
                async with client.stream(
                    "POST",
                    f"{self.base_url}/chat/completions",
                    headers=self._request_headers(),
                    json=request,
                ) as response:
                    if response.status_code != 200:
                        body = await response.aread()
                        raise MimoError(
                            f"MiMo returned HTTP {response.status_code}: "
                            f"{_safe_detail(body, secret=self._api_key)}"
                        )
                    async for event in _iter_sse_events(response):
                        for choice in event.get("choices") or ():
                            # The request asks for one completion. Ignore any
                            # unsolicited alternative choices rather than
                            # mixing separate answers and tool-call indices.
                            if choice.get("index", 0) != 0:
                                continue
                            delta = choice.get("delta") or {}
                            content = delta.get("content")
                            if isinstance(content, str) and content:
                                content_parts.append(content)
                                yield content
                            fragments = delta.get("tool_calls")
                            if fragments:
                                _merge_tool_call_fragments(
                                    call_parts,
                                    fragments,
                                    max_calls=self.max_tool_calls_per_round,
                                    max_argument_chars=self.max_tool_argument_chars,
                                )
            except httpx.HTTPError as exc:
                raise MimoError(
                    f"MiMo request failed: {type(exc).__name__}"
                ) from exc

            if not call_parts:
                if not content_parts:
                    raise MimoError("MiMo stream produced no answer text")
                return

            if self.tool_registry is None:
                raise MimoError("MiMo requested a tool but no registry is configured")
            if tool_rounds >= self.max_tool_rounds:
                raise MimoError(
                    f"MiMo exceeded the {self.max_tool_rounds} tool-round limit"
                )

            calls = _disambiguate_display_stop(text, _finish_tool_calls(call_parts))
            messages.append(
                {
                    "role": "assistant",
                    "content": "".join(content_parts) or None,
                    "tool_calls": calls,
                }
            )
            for call in calls:
                function = call["function"]
                result = await self.tool_registry.execute(
                    function["name"],
                    function["arguments"],
                    timeout_seconds=self.tool_timeout_seconds,
                )
                messages.append(
                    {
                        "role": "tool",
                        "tool_call_id": call["id"],
                        "content": result.as_json(),
                    }
                )
            tool_rounds += 1


async def _iter_sse_deltas(response: httpx.Response) -> AsyncIterator[str]:
    """Extracts assistant content deltas from an OpenAI-style SSE stream.

    reasoning_content is skipped: it is the model's thinking channel, not the
    spoken answer, and forwarding it would put internal deliberation on screen
    and into TTS.
    """
    async for event in _iter_sse_events(response):
        for choice in event.get("choices") or ():
            delta = choice.get("delta") or {}
            content = delta.get("content")
            if isinstance(content, str) and content:
                yield content


async def _iter_sse_events(response: httpx.Response) -> AsyncIterator[dict[str, Any]]:
    """Yields valid object payloads from an OpenAI-compatible SSE response."""
    async for line in response.aiter_lines():
        if not line or not line.startswith("data:"):
            continue
        payload = line[len("data:") :].strip()
        if not payload or payload == "[DONE]":
            continue
        try:
            event = json.loads(payload)
        except json.JSONDecodeError:
            continue
        if isinstance(event, dict):
            yield event


@dataclass(slots=True)
class _ToolCallParts:
    identifier: str = ""
    call_type: str = "function"
    name: str = ""
    arguments: str = ""


def _merge_tool_call_fragments(
    calls: dict[int, _ToolCallParts],
    fragments: object,
    *,
    max_calls: int,
    max_argument_chars: int,
) -> None:
    if not isinstance(fragments, list):
        raise MimoError("MiMo returned malformed tool_calls")
    for fragment in fragments:
        if not isinstance(fragment, dict):
            raise MimoError("MiMo returned a malformed tool-call fragment")
        index = fragment.get("index")
        if isinstance(index, bool) or not isinstance(index, int) or index < 0:
            raise MimoError("MiMo returned a tool call with an invalid index")
        if index not in calls:
            if len(calls) >= max_calls:
                raise MimoError(
                    f"MiMo exceeded the {max_calls} tools-per-round limit"
                )
            calls[index] = _ToolCallParts()
        call = calls[index]

        identifier = fragment.get("id")
        if identifier is not None:
            if not isinstance(identifier, str):
                raise MimoError("MiMo returned a tool call with an invalid id")
            call.identifier += identifier
            if len(call.identifier) > 256:
                raise MimoError("MiMo returned an oversized tool-call id")

        call_type = fragment.get("type")
        if call_type is not None:
            if call_type != "function":
                raise MimoError("MiMo returned an unsupported tool-call type")
            call.call_type = call_type

        function = fragment.get("function")
        if function is None:
            continue
        if not isinstance(function, dict):
            raise MimoError("MiMo returned malformed tool-call function data")
        name = function.get("name")
        if name is not None:
            if not isinstance(name, str):
                raise MimoError("MiMo returned a tool call with an invalid name")
            call.name += name
            if len(call.name) > 64:
                raise MimoError("MiMo returned an oversized tool name")
        arguments = function.get("arguments")
        if arguments is not None:
            if not isinstance(arguments, str):
                raise MimoError("MiMo returned non-text tool arguments")
            call.arguments += arguments
            if len(call.arguments) > max_argument_chars:
                raise MimoError("MiMo returned oversized tool arguments")


def _finish_tool_calls(calls: dict[int, _ToolCallParts]) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for _, call in sorted(calls.items()):
        if not call.identifier:
            raise MimoError("MiMo returned a tool call without an id")
        if not call.name:
            raise MimoError("MiMo returned a tool call without a function name")
        result.append(
            {
                "id": call.identifier,
                "type": call.call_type,
                "function": {
                    "name": call.name,
                    "arguments": call.arguments or "{}",
                },
            }
        )
    return result


_VIDEO_MARKERS = (
    "视频",
    "影片",
    "电影",
    ".mp4",
    ".mkv",
    ".avi",
    ".mov",
    "video",
)
_AUDIO_MARKERS = ("音乐", "歌曲", "音频", "声音", "music", "audio", "song")


def _disambiguate_display_stop(
    user_text: str, calls: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    """Route an unambiguous video stop away from the audio-only media tool.

    ``control_media`` controls the device audio player, but its historical name
    can tempt a model to use it for video.  Keep model choice for every other
    intent and only repair the exact contradiction observed on RC34: the user
    explicitly names video, no audio object is named, and the model asks the
    audio controller to stop.
    """

    normalized = user_text.casefold()
    if not any(marker in normalized for marker in _VIDEO_MARKERS):
        return calls
    if any(marker in normalized for marker in _AUDIO_MARKERS):
        return calls

    for call in calls:
        function = call.get("function")
        if not isinstance(function, dict) or function.get("name") != "control_media":
            continue
        raw_arguments = function.get("arguments")
        if not isinstance(raw_arguments, str):
            continue
        try:
            arguments = json.loads(raw_arguments or "{}")
        except json.JSONDecodeError:
            continue
        if not isinstance(arguments, dict) or arguments.get("action") != "stop":
            continue
        function["name"] = "control_monitor"
        function["arguments"] = json.dumps(
            {"action": "stop_video"},
            ensure_ascii=False,
            separators=(",", ":"),
        )
        LOGGER.info(
            "rerouted ambiguous audio media stop to display stop_video"
        )
    return calls


def _safe_detail(body: bytes, limit: int = 200, *, secret: str | None = None) -> str:
    """Renders a bounded error body while redacting the configured credential."""
    try:
        text = body.decode("utf-8", errors="replace")
    except Exception:  # pragma: no cover - decode with errors= cannot raise
        return "<undecodable body>"
    text = " ".join(text.split())
    if secret:
        text = text.replace(secret, "<redacted>")
    return text[:limit] if text else "<empty body>"
