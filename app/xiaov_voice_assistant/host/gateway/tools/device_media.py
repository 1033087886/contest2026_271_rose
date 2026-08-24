"""Media controller that requires an acknowledged command from the device."""

from __future__ import annotations

import asyncio
import logging
import secrets
from collections.abc import Mapping
from pathlib import PurePosixPath
from typing import Any

from gateway.providers.chksz_music import ChkszApiError
from gateway.providers.music import MusicLibrary, MusicSelection, MusicSearchError
from gateway.reminders import DeviceEventHub, ReminderDeliveryError
from gateway.tools.registry import ToolExecutionError

DEFAULT_MEDIA_SOURCE = "/data/xiaov-demo.wav"
MUSIC_STREAM_PREFIX = "stream://"
LOGGER = logging.getLogger(__name__)


def _is_direct_source(query: str) -> bool:
    if PurePosixPath(query).is_absolute():
        return True
    return False


class DeviceMediaController:
    def __init__(
        self,
        hub: DeviceEventHub,
        *,
        timeout_seconds: float = 4.0,
        default_source: str = DEFAULT_MEDIA_SOURCE,
        music_library: MusicLibrary | None = None,
        music_ready_timeout_seconds: float = 65.0,
    ) -> None:
        if timeout_seconds <= 0:
            raise ValueError("media command timeout must be positive")
        if not isinstance(default_source, str) or not default_source.strip():
            raise ValueError("default media source must be a non-empty string")
        if music_ready_timeout_seconds <= 0:
            raise ValueError("music ready timeout must be positive")
        self.hub = hub
        self.timeout_seconds = timeout_seconds
        self.default_source = default_source
        self.music_library = music_library
        self.music_ready_timeout_seconds = music_ready_timeout_seconds
        self._music_task: asyncio.Task[None] | None = None

    async def command(self, action: str, parameters: Mapping[str, Any]) -> Any:
        if action in {"play", "stop", "next", "previous"}:
            await self._cancel_music_stream()

        device_parameters = dict(parameters)
        source_result: dict[str, Any] = {}
        requested_query = parameters.get("query")
        selection: MusicSelection | None = None
        stream_id: int | None = None
        if action == "play" and isinstance(requested_query, str):
            if self.music_library is not None and not _is_direct_source(requested_query):
                try:
                    selection = await self.music_library.resolve(requested_query)
                except ChkszApiError as exc:
                    error_code = {
                        400: "music_invalid_request",
                        401: "music_auth",
                        402: "music_quota_exhausted",
                        403: "music_forbidden",
                        404: "music_not_found",
                        429: "music_rate_limited",
                        503: "music_unavailable",
                    }.get(exc.status_code, "music_provider_error")
                    raise ToolExecutionError(str(exc), code=error_code) from exc
                except MusicSearchError as exc:
                    raise ToolExecutionError(
                        "network music search failed", code="music_unavailable"
                    ) from exc
                stream_id = secrets.randbits(32) or 1
                resolved_source = f"{MUSIC_STREAM_PREFIX}{stream_id:08x}"
                default_fallback = False
            else:
                default_fallback = not _is_direct_source(requested_query)
                resolved_source = (
                    self.default_source if default_fallback else requested_query
                )
            device_parameters["query"] = resolved_source
            source_result = {
                "requested_query": requested_query,
                "resolved_source": resolved_source,
                "default_fallback": default_fallback,
            }
        try:
            result = await self.hub.media_command(
                action,
                device_parameters,
                timeout_seconds=self.timeout_seconds,
            )
        except ReminderDeliveryError as exc:
            raise ToolExecutionError(
                "device media command was not delivered",
                code="media_unavailable",
            ) from exc
        if not result["ok"]:
            error_code = result["error_code"]
            raise ToolExecutionError(
                "device did not execute the media command",
                code=f"media_{error_code}",
            )
        if selection is not None and stream_id is not None:
            command_id = result.get("command_id")
            if not isinstance(command_id, str):
                raise ToolExecutionError(
                    "device media command omitted its command id",
                    code="media_unavailable",
                )
            self._music_task = asyncio.create_task(
                self._stream_selection(command_id, stream_id, selection)
            )
            self._music_task.add_done_callback(self._music_task_done)
            source_result.update(
                {
                    "network_music": True,
                    "title": selection.track.title,
                    "artist": selection.track.artist,
                    "duration_seconds": selection.duration_seconds,
                }
            )
        return {
            "executed": True,
            "mode": "device",
            "action": action,
            **source_result,
        }

    async def _stream_selection(
        self, command_id: str, stream_id: int, selection: MusicSelection
    ) -> None:
        try:
            ready = await self.hub.wait_media_ready(
                command_id,
                timeout_seconds=self.music_ready_timeout_seconds,
            )
            if not ready["ok"]:
                LOGGER.warning(
                    "network music stream rejected stream=%08x error=%s",
                    stream_id,
                    ready.get("error_code", "unknown"),
                )
                return
            await self.hub.stream_music(command_id, stream_id, selection.pcm)
        except asyncio.CancelledError:
            raise
        except Exception:
            LOGGER.exception("network music stream failed stream=%08x", stream_id)

    def _music_task_done(self, task: asyncio.Task[None]) -> None:
        if self._music_task is task:
            self._music_task = None
        if not task.cancelled():
            failure = task.exception()
            if failure is not None:
                LOGGER.error(
                    "network music stream task failed",
                    exc_info=(type(failure), failure, failure.__traceback__),
                )

    async def _cancel_music_stream(self) -> None:
        task = self._music_task
        self._music_task = None
        if task is None or task.done():
            return
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass

    async def aclose(self) -> None:
        await self._cancel_music_stream()


__all__ = [
    "DEFAULT_MEDIA_SOURCE",
    "DeviceMediaController",
    "MUSIC_STREAM_PREFIX",
]
