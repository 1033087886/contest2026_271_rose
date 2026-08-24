"""MiMo speech recognition over the hosted chat endpoint.

MiMo lists a mimo-v2.5-asr model but does not expose OpenAI's
/audio/transcriptions route, which returns 404. Audio travels as an
`input_audio` content part inside /chat/completions instead, so
gateway/providers/openai_speech.py cannot reach it and this adapter exists
alongside it rather than replacing it.

Why this matters: the on-device wake model scored 18175 and 32255 on the same
phrase in consecutive board runs, straddling its 19660 threshold, while this
endpoint transcribed both recordings correctly. Recognition therefore moved off
the device, and the wake decision became a string question over the transcript
(see wake_gate.py).

Credentials come from the environment only. A key must never reach the
repository, a log line, or an error message.
"""

from __future__ import annotations

import asyncio
import base64
import io
import json
import os
import wave
from typing import Any

import httpx

SAMPLE_RATE = 16_000
CHANNELS = 1
SAMPLE_WIDTH = 2
DEFAULT_BASE_URL = "https://token-plan-cn.xiaomimimo.com/v1"
DEFAULT_ASR_MODEL = "mimo-v2.5-asr"
DEFAULT_MAX_AUDIO_BYTES = SAMPLE_RATE * CHANNELS * SAMPLE_WIDTH * 60
DEFAULT_MAX_TRANSCRIPT_BYTES = 64 * 1024
API_KEY_ENV = "MIMO_API_KEY"
BASE_URL_ENV = "MIMO_BASE_URL"


class MimoAsrError(RuntimeError):
    """Raised when MiMo ASR cannot be used or returns an unusable transcript."""


class MimoAsrProvider:
    """Buffered ASR adapter; one HTTP request per turn at listen.stop.

    Compose it with a separate answer and TTS stage through CompositePipeline so
    each provider stays independently replaceable.
    """

    def __init__(
        self,
        *,
        api_key: str | None = None,
        base_url: str | None = None,
        asr_model: str = DEFAULT_ASR_MODEL,
        timeout_seconds: float = 60.0,
        max_asr_bytes: int = DEFAULT_MAX_AUDIO_BYTES,
        max_transcript_bytes: int = DEFAULT_MAX_TRANSCRIPT_BYTES,
        client: httpx.AsyncClient | None = None,
    ) -> None:
        key = api_key or os.environ.get(API_KEY_ENV)
        if not key:
            raise MimoAsrError(
                f"no MiMo API key; set {API_KEY_ENV} or pass api_key explicitly"
            )
        if not asr_model.strip():
            raise ValueError("asr_model must not be empty")
        if timeout_seconds <= 0:
            raise ValueError("timeout_seconds must be positive")
        if max_asr_bytes <= 0 or max_asr_bytes % SAMPLE_WIDTH:
            raise ValueError("max_asr_bytes must be a positive whole-sample size")
        if max_transcript_bytes <= 0:
            raise ValueError("max_transcript_bytes must be positive")

        self._api_key = key
        resolved = base_url or os.environ.get(BASE_URL_ENV) or DEFAULT_BASE_URL
        if not resolved.strip():
            raise ValueError("base_url must not be empty")
        self.base_url = resolved.rstrip("/")
        self.asr_model = asr_model
        self.timeout_seconds = timeout_seconds
        self.max_asr_bytes = max_asr_bytes
        self.max_transcript_bytes = max_transcript_bytes
        self._client = client
        self._owns_client = client is None

    def start_asr(self) -> "BufferedMimoAsr":
        return BufferedMimoAsr(self)

    async def aclose(self) -> None:
        if self._client is not None and self._owns_client:
            await self._client.aclose()
            self._client = None

    def _ensure_client(self) -> httpx.AsyncClient:
        if self._client is None:
            self._client = httpx.AsyncClient(timeout=self.timeout_seconds)
        return self._client

    async def _transcribe(self, pcm: bytes, text_hint: str | None) -> str:
        content: list[dict[str, Any]] = [
            {
                "type": "input_audio",
                "input_audio": {
                    "data": base64.b64encode(_encode_wav(pcm)).decode("ascii"),
                    "format": "wav",
                },
            }
        ]
        if text_hint and text_hint.strip():
            content.append({"type": "text", "text": text_hint.strip()})

        request = {
            "model": self.asr_model,
            "messages": [{"role": "user", "content": content}],
        }
        try:
            async with self._ensure_client().stream(
                "POST",
                f"{self.base_url}/chat/completions",
                headers={
                    "Authorization": f"Bearer {self._api_key}",
                    "Content-Type": "application/json",
                },
                json=request,
            ) as response:
                if response.status_code >= 400:
                    await response.aread()
                    raise MimoAsrError(
                        f"MiMo ASR request failed with HTTP {response.status_code}"
                    )
                body = await _read_bounded(
                    response, self.max_transcript_bytes, "MiMo ASR response"
                )
        except asyncio.CancelledError:
            raise
        except httpx.HTTPError as exc:
            raise MimoAsrError(
                f"MiMo ASR request failed: {type(exc).__name__}"
            ) from exc

        try:
            payload: Any = json.loads(body)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise MimoAsrError("MiMo ASR returned invalid JSON") from exc
        text = _extract_transcript(payload)
        if text is None:
            raise MimoAsrError("MiMo ASR returned no transcript text")
        return text


class BufferedMimoAsr:
    """One bounded ASR turn; audio accumulates until finish sends it."""

    def __init__(self, provider: MimoAsrProvider) -> None:
        self._provider = provider
        self._pcm = bytearray()
        self._closed = False

    @property
    def buffered_bytes(self) -> int:
        return len(self._pcm)

    async def push_audio(self, pcm: bytes) -> str | None:
        if self._closed:
            raise MimoAsrError("ASR turn is already closed")
        if not isinstance(pcm, bytes) or not pcm or len(pcm) % SAMPLE_WIDTH:
            raise MimoAsrError("ASR audio must contain complete pcm_s16le samples")
        if len(self._pcm) + len(pcm) > self._provider.max_asr_bytes:
            raise MimoAsrError("ASR audio exceeds the configured turn limit")
        self._pcm.extend(pcm)
        # No partial transcripts: the endpoint scores one complete utterance.
        return None

    async def finish(self, text_hint: str | None) -> str:
        if self._closed:
            raise MimoAsrError("ASR turn is already closed")
        self._closed = True
        if not self._pcm:
            raise MimoAsrError("cannot transcribe an empty ASR turn")
        pcm = bytes(self._pcm)
        self._pcm.clear()
        return await self._provider._transcribe(pcm, text_hint)

    async def cancel(self) -> None:
        self._closed = True
        self._pcm.clear()


def _encode_wav(pcm: bytes) -> bytes:
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(CHANNELS)
        wav.setsampwidth(SAMPLE_WIDTH)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(pcm)
    return output.getvalue()


def _extract_transcript(payload: Any) -> str | None:
    """Reads the transcript out of a chat completion, tolerating content parts."""
    if not isinstance(payload, dict):
        return None
    choices = payload.get("choices")
    if not isinstance(choices, list) or not choices:
        return None
    message = choices[0].get("message") if isinstance(choices[0], dict) else None
    if not isinstance(message, dict):
        return None
    content = message.get("content")
    if isinstance(content, str):
        return content.strip() or None
    if isinstance(content, list):
        parts = [
            part["text"]
            for part in content
            if isinstance(part, dict) and isinstance(part.get("text"), str)
        ]
        joined = " ".join(parts).strip()
        return joined or None
    return None


async def _read_bounded(
    response: httpx.Response, limit: int, description: str
) -> bytes:
    result = bytearray()
    async for chunk in response.aiter_bytes():
        if len(result) + len(chunk) > limit:
            raise MimoAsrError(f"{description} exceeds the configured limit")
        result.extend(chunk)
    return bytes(result)
