from __future__ import annotations

import asyncio
import math
import struct
from collections.abc import AsyncIterator, Iterator


class MockPipeline:
    """Deterministic provider used to exercise streaming without cloud services."""

    def __init__(self, latency_ms: int = 15, *, tts_amplitude: int = 3_000) -> None:
        if isinstance(tts_amplitude, bool) or not 0 <= tts_amplitude <= 32_767:
            raise ValueError("tts_amplitude must be an integer in [0, 32767]")
        self.latency_seconds = max(0, latency_ms) / 1000
        self.tts_amplitude = tts_amplitude

    def start_asr(self) -> "MockAsr":
        return MockAsr(self.latency_seconds)

    async def answer_chunks(self, text: str) -> AsyncIterator[str]:
        answer = f"小 V 收到：{text}。当前是离线模拟链路。"
        for chunk in _text_chunks(answer, 6):
            await asyncio.sleep(self.latency_seconds)
            yield chunk

    async def synthesize(self, text: str) -> AsyncIterator[bytes]:
        sample_rate = 16_000
        frame_samples = 320
        duration_ms = min(1_200, max(240, len(text) * 35))
        total_samples = duration_ms * sample_rate // 1000
        for start in range(0, total_samples, frame_samples):
            count = min(frame_samples, total_samples - start)
            samples = (
                int(
                    self.tts_amplitude
                    * math.sin(2 * math.pi * 440 * (start + i) / sample_rate)
                )
                for i in range(count)
            )
            await asyncio.sleep(self.latency_seconds / 4)
            yield struct.pack(f"<{count}h", *samples)


def _text_chunks(text: str, size: int) -> Iterator[str]:
    for offset in range(0, len(text), size):
        yield text[offset : offset + size]


class MockAsr:
    def __init__(self, latency_seconds: float) -> None:
        self.latency_seconds = latency_seconds
        self.received_bytes = 0
        self._next_partial_bytes = 16_000

    async def push_audio(self, pcm: bytes) -> str | None:
        self.received_bytes += len(pcm)
        if self.received_bytes < self._next_partial_bytes:
            return None
        self._next_partial_bytes += 16_000
        await asyncio.sleep(self.latency_seconds)
        duration_ms = self.received_bytes * 1000 // (16_000 * 2)
        return f"已听到约 {duration_ms} 毫秒"

    async def finish(self, text_hint: str | None) -> str:
        await asyncio.sleep(self.latency_seconds)
        if text_hint:
            return text_hint.strip()
        duration_ms = self.received_bytes * 1000 // (16_000 * 2)
        return f"收到一段约 {duration_ms} 毫秒的音频"

    async def cancel(self) -> None:
        return None
