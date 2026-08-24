from __future__ import annotations

import inspect
from collections.abc import AsyncIterator


class CompositePipeline:
    """Composes independently selectable ASR, LLM, and TTS stages."""

    def __init__(
        self, *, asr_source, llm, tts_source, resources=(), video_bridge=None
    ) -> None:
        self._asr_source = asr_source
        self._llm = llm
        self._tts_source = tts_source
        self._resources = tuple(resources)
        self.video_bridge = video_bridge

    def start_asr(self):
        return self._asr_source.start_asr()

    def answer_chunks(self, text: str) -> AsyncIterator[str]:
        return self._llm.answer_chunks(text)

    def synthesize(self, text: str) -> AsyncIterator[bytes]:
        return self._tts_source.synthesize(text)

    async def astart(self) -> None:
        """Starts optional long-lived resources such as timer schedulers."""
        seen: set[int] = set()
        for resource in self._resources:
            identity = id(resource)
            if identity in seen:
                continue
            seen.add(identity)
            start = getattr(resource, "start", None)
            if start is None:
                continue
            result = start()
            if inspect.isawaitable(result):
                await result
        if self.video_bridge is not None and id(self.video_bridge) not in seen:
            result = self.video_bridge.start()
            if inspect.isawaitable(result):
                await result

    async def aclose(self) -> None:
        """Closes every distinct provider owned by the composed pipeline."""
        seen: set[int] = set()
        first_error: BaseException | None = None
        objects = (
            self._asr_source,
            self._llm,
            self._tts_source,
            self.video_bridge,
            *reversed(self._resources),
        )
        for provider in objects:
            if provider is None:
                continue
            identity = id(provider)
            if identity in seen:
                continue
            seen.add(identity)
            close = getattr(provider, "aclose", None)
            if close is None:
                continue
            try:
                result = close()
                if inspect.isawaitable(result):
                    await result
            except BaseException as exc:
                if first_error is None:
                    first_error = exc
        if first_error is not None:
            raise first_error
