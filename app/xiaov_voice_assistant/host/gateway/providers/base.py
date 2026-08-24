from __future__ import annotations

from collections.abc import AsyncIterator
from typing import Protocol


class VoicePipeline(Protocol):
    """Boundary implemented by mock and future ASR/MiMo/TTS providers.

    synthesize yields raw 16 kHz mono pcm_s16le chunks without a WAV header.
    """

    def start_asr(self) -> "StreamingAsr": ...

    def answer_chunks(self, text: str) -> AsyncIterator[str]: ...

    def synthesize(self, text: str) -> AsyncIterator[bytes]: ...


class StreamingAsr(Protocol):
    """Incremental ASR session for one listen.start/listen.stop turn."""

    async def push_audio(self, pcm: bytes) -> str | None: ...

    async def finish(self, text_hint: str | None) -> str: ...

    async def cancel(self) -> None: ...
