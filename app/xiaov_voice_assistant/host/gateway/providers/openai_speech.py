"""Self-hosted speech stages using OpenAI-compatible HTTP endpoints.

This module is an adapter only: it deliberately ships no inference runtime or
model weights. The configured service must accept 16 kHz mono signed 16-bit PCM
for transcription and must synthesize the same format.
"""

from __future__ import annotations

import asyncio
import io
import json
import os
import unicodedata
import wave
from collections.abc import AsyncIterator
from typing import Any

import httpx

SAMPLE_RATE = 16_000
CHANNELS = 1
SAMPLE_WIDTH = 2
PCM_FRAME_BYTES = SAMPLE_RATE * CHANNELS * SAMPLE_WIDTH * 20 // 1000
DEFAULT_MAX_AUDIO_BYTES = SAMPLE_RATE * CHANNELS * SAMPLE_WIDTH * 60
DEFAULT_MAX_TRANSCRIPT_BYTES = 64 * 1024
DEFAULT_MAX_TTS_BYTES = SAMPLE_RATE * CHANNELS * SAMPLE_WIDTH * 60
DEFAULT_MAX_TTS_TEXT_CHARS = 4_096
DEFAULT_BASE_URL = "http://127.0.0.1:8000/v1"
DEFAULT_ASR_MODEL = "whisper-1"
DEFAULT_TTS_MODEL = "tts-1"
DEFAULT_TTS_VOICE = "alloy"
API_KEY_ENV = "XIAOV_SPEECH_API_KEY"

_RAW_CONTENT_TYPES = {
    "",
    "application/octet-stream",
    "audio/pcm",
    "audio/raw",
    "audio/s16le",
}
_WAV_CONTENT_TYPES = {"audio/wav", "audio/wave", "audio/x-wav"}
_WAV_HEADER_ALLOWANCE = 64 * 1024


class SpeechProviderError(RuntimeError):
    """Raised when a speech endpoint returns an unusable result."""


class OpenAICompatibleSpeechProvider:
    """Buffered ASR and streaming TTS adapter for a self-hosted HTTP service."""

    def __init__(
        self,
        *,
        base_url: str = DEFAULT_BASE_URL,
        asr_model: str = DEFAULT_ASR_MODEL,
        tts_model: str = DEFAULT_TTS_MODEL,
        voice: str = DEFAULT_TTS_VOICE,
        timeout_seconds: float = 60.0,
        max_asr_bytes: int = DEFAULT_MAX_AUDIO_BYTES,
        max_transcript_bytes: int = DEFAULT_MAX_TRANSCRIPT_BYTES,
        max_tts_bytes: int = DEFAULT_MAX_TTS_BYTES,
        max_tts_text_chars: int = DEFAULT_MAX_TTS_TEXT_CHARS,
        client: httpx.AsyncClient | None = None,
    ) -> None:
        if not base_url.strip():
            raise ValueError("speech base_url must not be empty")
        if not asr_model.strip() or not tts_model.strip() or not voice.strip():
            raise ValueError("speech model and voice values must not be empty")
        if max_asr_bytes <= 0 or max_asr_bytes % SAMPLE_WIDTH:
            raise ValueError("max_asr_bytes must be a positive whole-sample size")
        if timeout_seconds <= 0:
            raise ValueError("speech timeout_seconds must be positive")
        if max_transcript_bytes <= 0 or max_tts_text_chars <= 0:
            raise ValueError("speech response limits must be positive")
        if max_tts_bytes <= 0 or max_tts_bytes % SAMPLE_WIDTH:
            raise ValueError("max_tts_bytes must be a positive whole-sample size")

        self.base_url = base_url.rstrip("/")
        self.asr_model = asr_model
        self.tts_model = tts_model
        self.voice = voice
        self.timeout_seconds = timeout_seconds
        self.max_asr_bytes = max_asr_bytes
        self.max_transcript_bytes = max_transcript_bytes
        self.max_tts_bytes = max_tts_bytes
        self.max_tts_text_chars = max_tts_text_chars
        self._client = client
        self._owns_client = client is None

    def start_asr(self) -> "BufferedHttpAsr":
        return BufferedHttpAsr(self)

    async def aclose(self) -> None:
        if self._client is not None and self._owns_client:
            await self._client.aclose()
            self._client = None

    def _ensure_client(self) -> httpx.AsyncClient:
        if self._client is None:
            self._client = httpx.AsyncClient(timeout=self.timeout_seconds)
        return self._client

    def _headers(self) -> dict[str, str]:
        key = os.environ.get(API_KEY_ENV)
        return {"Authorization": f"Bearer {key}"} if key else {}

    async def _transcribe(self, pcm: bytes, text_hint: str | None) -> str:
        wav = _encode_wav(pcm)
        fields: dict[str, str] = {
            "model": self.asr_model,
            "response_format": "json",
        }
        if text_hint and text_hint.strip():
            fields["prompt"] = text_hint.strip()

        try:
            async with self._ensure_client().stream(
                "POST",
                f"{self.base_url}/audio/transcriptions",
                headers=self._headers(),
                data=fields,
                files={"file": ("audio.wav", wav, "audio/wav")},
            ) as response:
                _raise_for_status(response, "ASR")
                body = await _read_bounded(
                    response, self.max_transcript_bytes, "ASR response"
                )
        except asyncio.CancelledError:
            raise
        except httpx.HTTPError as exc:
            raise SpeechProviderError(
                f"speech ASR request failed: {type(exc).__name__}"
            ) from exc

        try:
            payload: Any = json.loads(body)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise SpeechProviderError("speech ASR returned invalid JSON") from exc
        text = payload.get("text") if isinstance(payload, dict) else None
        if not isinstance(text, str) or not text.strip():
            raise SpeechProviderError("speech ASR returned no transcript text")
        return text.strip()

    async def synthesize(self, text: str) -> AsyncIterator[bytes]:
        spoken_text = _normalize_tts_text(text)
        if not spoken_text:
            raise SpeechProviderError("speech TTS input has no speakable text")
        if len(spoken_text) > self.max_tts_text_chars:
            raise SpeechProviderError("speech TTS input exceeds the configured limit")
        request = {
            "model": self.tts_model,
            "input": spoken_text,
            "voice": self.voice,
            "response_format": "pcm",
        }
        try:
            async with self._ensure_client().stream(
                "POST",
                f"{self.base_url}/audio/speech",
                headers={**self._headers(), "Content-Type": "application/json"},
                json=request,
            ) as response:
                _raise_for_status(response, "TTS")
                media_type, parameters = _parse_content_type(
                    response.headers.get("Content-Type", "")
                )
                if media_type in _WAV_CONTENT_TYPES:
                    encoded = await _read_bounded(
                        response,
                        self.max_tts_bytes + _WAV_HEADER_ALLOWANCE,
                        "TTS WAV response",
                    )
                    pcm = _decode_wav(encoded, self.max_tts_bytes)
                    for chunk in _pcm_chunks(pcm):
                        yield chunk
                    return
                if media_type not in _RAW_CONTENT_TYPES:
                    raise SpeechProviderError(
                        f"speech TTS returned unsupported Content-Type {media_type!r}"
                    )
                _validate_raw_parameters(parameters)
                async for chunk in _iter_raw_or_wav(
                    response,
                    self.max_tts_bytes,
                    sniff_wav=media_type in ("", "application/octet-stream"),
                ):
                    yield chunk
        except asyncio.CancelledError:
            raise
        except httpx.HTTPError as exc:
            raise SpeechProviderError(
                f"speech TTS request failed: {type(exc).__name__}"
            ) from exc


class BufferedHttpAsr:
    """One bounded ASR turn; the HTTP request starts only at listen.stop."""

    def __init__(self, provider: OpenAICompatibleSpeechProvider) -> None:
        self._provider = provider
        self._pcm = bytearray()
        self._closed = False

    @property
    def buffered_bytes(self) -> int:
        return len(self._pcm)

    async def push_audio(self, pcm: bytes) -> str | None:
        if self._closed:
            raise SpeechProviderError("ASR turn is already closed")
        if not isinstance(pcm, bytes) or not pcm or len(pcm) % SAMPLE_WIDTH:
            raise SpeechProviderError("ASR audio must contain complete pcm_s16le samples")
        if len(self._pcm) + len(pcm) > self._provider.max_asr_bytes:
            raise SpeechProviderError("ASR audio exceeds the configured turn limit")
        self._pcm.extend(pcm)
        return None

    async def finish(self, text_hint: str | None) -> str:
        if self._closed:
            raise SpeechProviderError("ASR turn is already closed")
        self._closed = True
        if not self._pcm:
            raise SpeechProviderError("cannot transcribe an empty ASR turn")
        pcm = bytes(self._pcm)
        self._pcm.clear()
        return await self._provider._transcribe(pcm, text_hint)

    async def cancel(self) -> None:
        self._closed = True
        self._pcm.clear()


def _normalize_tts_text(text: str) -> str:
    """Remove code points that local speech engines commonly cannot tokenize."""
    filtered = "".join(
        char
        for char in unicodedata.normalize("NFKC", text)
        if unicodedata.category(char) not in {"Cc", "Cf", "Cn", "Co", "Cs", "So"}
    )
    collapsed = " ".join(filtered.split())
    # Quotes and punctuation can survive emoji removal as a standalone
    # streaming sentence (for example `"😄`). MeloTTS returns HTTP 500 for
    # those tokenless requests, so treat them exactly like empty input.
    return collapsed if any(char.isalnum() for char in collapsed) else ""


def _encode_wav(pcm: bytes) -> bytes:
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(CHANNELS)
        wav.setsampwidth(SAMPLE_WIDTH)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(pcm)
    return output.getvalue()


def _decode_wav(encoded: bytes, max_pcm_bytes: int) -> bytes:
    try:
        with wave.open(io.BytesIO(encoded), "rb") as wav:
            if (
                wav.getnchannels() != CHANNELS
                or wav.getsampwidth() != SAMPLE_WIDTH
                or wav.getframerate() != SAMPLE_RATE
                or wav.getcomptype() != "NONE"
            ):
                raise SpeechProviderError(
                    "speech TTS WAV must be 16 kHz mono uncompressed signed 16-bit PCM"
                )
            expected_bytes = wav.getnframes() * SAMPLE_WIDTH
            if expected_bytes > max_pcm_bytes:
                raise SpeechProviderError("speech TTS audio exceeds the configured limit")
            pcm = wav.readframes(wav.getnframes())
    except (EOFError, wave.Error) as exc:
        raise SpeechProviderError("speech TTS returned an invalid WAV file") from exc
    if not pcm or len(pcm) != expected_bytes:
        raise SpeechProviderError("speech TTS returned an empty or truncated WAV file")
    return pcm


async def _read_bounded(
    response: httpx.Response, limit: int, description: str
) -> bytes:
    result = bytearray()
    async for chunk in response.aiter_bytes():
        if len(result) + len(chunk) > limit:
            raise SpeechProviderError(f"{description} exceeds the configured limit")
        result.extend(chunk)
    return bytes(result)


async def _iter_raw_or_wav(
    response: httpx.Response, max_pcm_bytes: int, *, sniff_wav: bool
) -> AsyncIterator[bytes]:
    mode: str | None = None if sniff_wav else "raw"
    undecided = bytearray()
    wav_body = bytearray()
    pending = b""
    total = 0

    async for incoming in response.aiter_bytes():
        total += len(incoming)
        maximum = max_pcm_bytes + (_WAV_HEADER_ALLOWANCE if mode != "raw" else 0)
        if total > maximum:
            raise SpeechProviderError("speech TTS audio exceeds the configured limit")

        if mode is None:
            undecided.extend(incoming)
            if len(undecided) < 12:
                continue
            mode = "wav" if undecided[:4] == b"RIFF" else "raw"
            incoming = bytes(undecided)
            undecided.clear()

        if mode == "wav":
            wav_body.extend(incoming)
            continue

        if total > max_pcm_bytes:
            raise SpeechProviderError("speech TTS audio exceeds the configured limit")
        data = pending + incoming
        complete = len(data) - (len(data) % PCM_FRAME_BYTES)
        for offset in range(0, complete, PCM_FRAME_BYTES):
            yield data[offset : offset + PCM_FRAME_BYTES]
        pending = data[complete:]

    if mode is None:
        if undecided[:4] == b"RIFF":
            mode = "wav"
            wav_body.extend(undecided)
        else:
            mode = "raw"
            pending = bytes(undecided)

    if mode == "wav":
        pcm = _decode_wav(bytes(wav_body), max_pcm_bytes)
        for chunk in _pcm_chunks(pcm):
            yield chunk
        return

    if total > max_pcm_bytes:
        raise SpeechProviderError("speech TTS audio exceeds the configured limit")
    if total == 0:
        raise SpeechProviderError("speech TTS returned empty PCM audio")
    if len(pending) % SAMPLE_WIDTH:
        raise SpeechProviderError("speech TTS returned an incomplete pcm_s16le sample")
    if pending:
        yield pending


def _pcm_chunks(pcm: bytes):
    for offset in range(0, len(pcm), PCM_FRAME_BYTES):
        yield pcm[offset : offset + PCM_FRAME_BYTES]


def _parse_content_type(value: str) -> tuple[str, dict[str, str]]:
    parts = [part.strip() for part in value.split(";")]
    media_type = parts[0].lower()
    parameters: dict[str, str] = {}
    for part in parts[1:]:
        name, separator, raw_value = part.partition("=")
        if separator:
            parameters[name.strip().lower()] = raw_value.strip().strip('"').lower()
    return media_type, parameters


def _validate_raw_parameters(parameters: dict[str, str]) -> None:
    for name in ("rate", "sample_rate", "samplerate"):
        if name in parameters and parameters[name] != str(SAMPLE_RATE):
            raise SpeechProviderError("speech TTS raw PCM sample rate must be 16000")
    if "channels" in parameters and parameters["channels"] != "1":
        raise SpeechProviderError("speech TTS raw PCM must be mono")
    for name in ("bits", "bit_depth", "sample_size"):
        if name in parameters and parameters[name] != "16":
            raise SpeechProviderError("speech TTS raw PCM samples must be 16-bit")
    if "endian" in parameters and parameters["endian"] not in ("little", "le"):
        raise SpeechProviderError("speech TTS raw PCM must be little-endian")


def _raise_for_status(response: httpx.Response, stage: str) -> None:
    if not response.is_success:
        raise SpeechProviderError(
            f"speech {stage} returned HTTP {response.status_code}"
        )
