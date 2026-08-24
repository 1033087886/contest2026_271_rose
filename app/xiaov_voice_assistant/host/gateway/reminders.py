"""Delivery boundary for persistent timer reminders.

Timers are gateway-global today, so a reminder is delivered only when exactly
one logical device is connected. This avoids leaking one device's reminder to
another while the protocol still lacks per-device tool context.
"""

from __future__ import annotations

import asyncio
import inspect
import json
import logging
import math
import re
import uuid
from collections.abc import Awaitable, Callable, Mapping
from typing import Any

from gateway.protocol import AudioFrame, AudioKind, event

LOGGER = logging.getLogger(__name__)

ReminderSender = Callable[[dict[str, Any]], Awaitable[None]]
BinarySender = Callable[[bytes], Awaitable[None]]
MAX_REMINDER_SUBSCRIBERS = 32
MAX_PENDING_MEDIA_COMMANDS = 16
MUSIC_PREFETCH_FRAMES = 10
_TIMER_ID_RE = re.compile(r"timer-[0-9]{4,}\Z")
_COMMAND_ID_RE = re.compile(r"[0-9a-f]{32}\Z")
_MEDIA_ERROR_CODES = frozenset(
    ("backend_unavailable", "invalid_command", "playback_failed", "busy")
)


class ReminderDeliveryError(RuntimeError):
    """A sanitized delivery failure suitable for timer retry handling."""


class DeviceEventHub:
    """Routes reminders to one authenticated logical device.

    Reconnecting the same device may briefly leave two sessions registered;
    the newest session wins. Different device ids make delivery ambiguous, so
    no reminder is sent until only one logical device remains connected.
    """

    def __init__(self, *, max_subscribers: int = MAX_REMINDER_SUBSCRIBERS) -> None:
        if not 1 <= max_subscribers <= 1024:
            raise ValueError("reminder subscriber limit must be between 1 and 1024")
        self.max_subscribers = max_subscribers
        self._lock = asyncio.Lock()
        self._subscribers: dict[
            str, tuple[str, ReminderSender, BinarySender | None]
        ] = {}
        self._pending_media: dict[
            str, tuple[str, asyncio.Future[dict[str, Any]]]
        ] = {}
        self._pending_media_ready: dict[
            str, tuple[str, asyncio.Future[dict[str, Any]]]
        ] = {}

    async def register(
        self,
        device_id: str,
        sender: ReminderSender,
        send_binary: BinarySender | None = None,
    ) -> str:
        if not _valid_device_id(device_id):
            raise ValueError("invalid reminder device id")
        if not callable(sender):
            raise ValueError("reminder sender must be callable")
        async with self._lock:
            if len(self._subscribers) >= self.max_subscribers:
                raise ReminderDeliveryError("reminder subscriber limit reached")
            subscription = uuid.uuid4().hex
            self._subscribers[subscription] = (device_id, sender, send_binary)
            return subscription

    async def unregister(self, subscription: str | None) -> None:
        if subscription is None:
            return
        failed: list[asyncio.Future[dict[str, Any]]] = []
        async with self._lock:
            self._subscribers.pop(subscription, None)
            for command_id, (owner, future) in list(self._pending_media.items()):
                if owner == subscription:
                    self._pending_media.pop(command_id, None)
                    failed.append(future)
            for command_id, (owner, future) in list(
                self._pending_media_ready.items()
            ):
                if owner == subscription:
                    self._pending_media_ready.pop(command_id, None)
                    failed.append(future)
        for future in failed:
            if not future.done():
                future.set_exception(
                    ReminderDeliveryError("device disconnected before command result")
                )

    async def publish(self, record: Mapping[str, Any]) -> None:
        payload = _reminder_payload(record)
        async with self._lock:
            _, sender, _ = self._select_sender_locked("reminder")
        try:
            result = sender(event("reminder", payload=payload))
            if not inspect.isawaitable(result):
                raise TypeError
            await result
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            raise ReminderDeliveryError("reminder could not be delivered") from exc

    async def media_command(
        self,
        action: str,
        parameters: Mapping[str, Any],
        *,
        timeout_seconds: float = 4.0,
    ) -> dict[str, Any]:
        payload = _media_command_payload(action, parameters)
        message = event("media.command", payload=payload)
        command_id = message["id"]
        message["payload"]["command_id"] = command_id
        stream_command = (
            action == "play"
            and isinstance(parameters.get("query"), str)
            and parameters["query"].startswith("stream://")
        )
        loop = asyncio.get_running_loop()
        async with self._lock:
            subscription, sender, _ = self._select_sender_locked("media command")
            if len(self._pending_media) >= MAX_PENDING_MEDIA_COMMANDS:
                raise ReminderDeliveryError("media command limit reached")
            future: asyncio.Future[dict[str, Any]] = loop.create_future()
            self._pending_media[command_id] = (subscription, future)
            if stream_command:
                self._pending_media_ready[command_id] = (
                    subscription,
                    loop.create_future(),
                )
        try:
            result = sender(message)
            if not inspect.isawaitable(result):
                raise TypeError
            await result
            async with asyncio.timeout(timeout_seconds):
                return await asyncio.shield(future)
        except asyncio.CancelledError:
            raise
        except TimeoutError as exc:
            raise ReminderDeliveryError("device media command timed out") from exc
        except ReminderDeliveryError:
            raise
        except Exception as exc:
            raise ReminderDeliveryError("media command could not be delivered") from exc
        finally:
            async with self._lock:
                self._pending_media.pop(command_id, None)
                keep_ready = (
                    stream_command
                    and future.done()
                    and not future.cancelled()
                    and bool(future.result().get("ok"))
                )
                if not keep_ready:
                    self._pending_media_ready.pop(command_id, None)
            if not future.done():
                future.cancel()

    async def resolve_media_result(
        self, subscription: str | None, payload: Mapping[str, Any]
    ) -> bool:
        parsed = _parse_media_result(payload)
        if subscription is None or parsed is None:
            return False
        command_id = parsed["command_id"]
        async with self._lock:
            pending = self._pending_media.get(command_id)
            if pending is None or pending[0] != subscription:
                return False
            self._pending_media.pop(command_id, None)
            future = pending[1]
        if not future.done():
            future.set_result(parsed)
        return True

    async def wait_media_ready(
        self, command_id: str, *, timeout_seconds: float = 65.0
    ) -> dict[str, Any]:
        """Wait until a deferred network music command owns the audio device."""
        if timeout_seconds <= 0:
            raise ValueError("media ready timeout must be positive")
        async with self._lock:
            pending = self._pending_media_ready.get(command_id)
            if pending is None:
                raise ReminderDeliveryError("media stream is not pending")
            future = pending[1]
        keep_owner = False
        try:
            async with asyncio.timeout(timeout_seconds):
                result = await asyncio.shield(future)
            keep_owner = bool(result.get("ok"))
            return result
        except TimeoutError as exc:
            raise ReminderDeliveryError("device media stream was not started") from exc
        finally:
            if not keep_owner:
                async with self._lock:
                    self._pending_media_ready.pop(command_id, None)
            if not future.done():
                future.cancel()

    async def resolve_media_ready(
        self, subscription: str | None, payload: Mapping[str, Any]
    ) -> bool:
        parsed = _parse_media_ready(payload)
        if subscription is None or parsed is None:
            return False
        command_id = parsed["command_id"]
        async with self._lock:
            pending = self._pending_media_ready.get(command_id)
            if pending is None or pending[0] != subscription:
                return False
            future = pending[1]
        if not future.done():
            future.set_result(parsed)
        return True

    async def stream_music(
        self,
        command_id: str,
        stream_id: int,
        pcm: bytes,
        *,
        frame_samples: int = 320,
        sample_rate: int = 16_000,
        prefetch_frames: int = MUSIC_PREFETCH_FRAMES,
    ) -> None:
        """Send PCM through the same session that acknowledged media.ready."""
        if _COMMAND_ID_RE.fullmatch(command_id) is None:
            raise ValueError("music command id is invalid")
        if not isinstance(stream_id, int) or not 0 <= stream_id <= 0xFFFFFFFF:
            raise ValueError("music stream id must fit uint32")
        if (
            frame_samples <= 0
            or sample_rate <= 0
            or prefetch_frames < 0
            or len(pcm) % 2
        ):
            raise ValueError("music PCM format is invalid")
        async with self._lock:
            pending = self._pending_media_ready.pop(command_id, None)
            if pending is None or not pending[1].done() or pending[1].cancelled():
                raise ReminderDeliveryError("media stream is not ready")
            ready = pending[1].result()
            subscriber = self._subscribers.get(pending[0])
            if not ready.get("ok") or subscriber is None:
                raise ReminderDeliveryError("media stream device disconnected")
            _, sender, binary_sender = subscriber
        if binary_sender is None:
            raise ReminderDeliveryError("connected device has no binary channel")

        LOGGER.info(
            "music stream starting command=%s stream=%08x bytes=%d prefetch_frames=%d",
            command_id,
            stream_id,
            len(pcm),
            prefetch_frames,
        )
        started = asyncio.get_running_loop().time()
        sequence = 0
        samples_sent = 0
        frame_bytes = frame_samples * 2
        prefetch_samples = min(
            len(pcm) // 2,
            prefetch_frames * frame_samples,
        )
        for offset in range(0, len(pcm), frame_bytes):
            chunk = pcm[offset : offset + frame_bytes]
            await binary_sender(
                AudioFrame(
                    kind=AudioKind.MUSIC,
                    stream_id=stream_id,
                    sequence=sequence,
                    timestamp_ms=samples_sent * 1000 // sample_rate,
                    payload=chunk,
                ).encode()
            )
            sequence += 1
            samples_sent += len(chunk) // 2
            # Prime the board's bounded queue before pacing. Starting with an
            # empty queue made the ALSA worker underrun immediately, repeatedly
            # enter prepare(), and sometimes hold RX flow control long enough
            # for the board's application pong deadline to close the socket.
            # Ten 20 ms frames remain well below the 48-frame pause threshold.
            paced_samples = max(0, samples_sent - prefetch_samples)
            deadline = started + paced_samples / sample_rate
            delay = deadline - asyncio.get_running_loop().time()
            if delay > 0:
                await asyncio.sleep(delay)
        await sender(
            event(
                "music.end",
                payload={"stream_id": stream_id, "samples": samples_sent},
            )
        )
        LOGGER.info(
            "music stream completed command=%s stream=%08x frames=%d samples=%d",
            command_id,
            stream_id,
            sequence,
            samples_sent,
        )

    async def connected_device_count(self) -> int:
        async with self._lock:
            return len({device_id for device_id, _, _ in self._subscribers.values()})

    def _select_sender_locked(
        self, operation: str
    ) -> tuple[str, ReminderSender, BinarySender | None]:
        subscribers = list(self._subscribers.items())
        device_ids = {device_id for _, (device_id, _, _) in subscribers}
        if not subscribers:
            raise ReminderDeliveryError(f"no device is connected for {operation}")
        if len(device_ids) != 1:
            raise ReminderDeliveryError(f"{operation} is ambiguous across devices")
        subscription, (_, sender, binary_sender) = subscribers[-1]
        return subscription, sender, binary_sender


def _valid_device_id(value: object) -> bool:
    return (
        isinstance(value, str)
        and 1 <= len(value) <= 64
        and not any(ord(character) < 32 or ord(character) == 127 for character in value)
    )


def _reminder_payload(record: Mapping[str, Any]) -> dict[str, Any]:
    timer_id = record.get("timer_id")
    label = record.get("label")
    kind = record.get("kind")
    deadline = record.get("deadline_epoch")
    if (
        not isinstance(timer_id, str)
        or len(timer_id) > 64
        or _TIMER_ID_RE.fullmatch(timer_id) is None
        or not isinstance(label, str)
        or not 1 <= len(label) <= 100
        or any(ord(character) < 32 or ord(character) == 127 for character in label)
        or not isinstance(kind, str)
        or kind not in ("timer", "pomodoro")
        or not _valid_deadline(deadline)
    ):
        raise ReminderDeliveryError("timer produced an invalid reminder")
    try:
        if len(label.encode("utf-8")) > 400:
            raise ReminderDeliveryError("timer produced an invalid reminder")
    except UnicodeEncodeError as exc:
        raise ReminderDeliveryError("timer produced an invalid reminder") from exc
    return {
        "timer_id": timer_id,
        "label": label,
        "kind": kind,
        "deadline_epoch": deadline,
    }


def _valid_deadline(value: object) -> bool:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return False
    try:
        return math.isfinite(float(value))
    except (OverflowError, ValueError):
        return False


def _media_command_payload(
    action: object, parameters: Mapping[str, Any]
) -> dict[str, Any]:
    allowed_actions = {
        "play",
        "pause",
        "resume",
        "stop",
        "next",
        "previous",
        "set_volume",
        "seek",
    }
    if not isinstance(action, str) or action not in allowed_actions:
        raise ReminderDeliveryError("media command is invalid")
    payload = {"action": action}
    for key in ("query", "volume_percent", "position_seconds"):
        if key in parameters:
            payload[key] = parameters[key]
    try:
        encoded = json.dumps(payload, ensure_ascii=False, allow_nan=False).encode("utf-8")
    except (TypeError, ValueError, UnicodeEncodeError) as exc:
        raise ReminderDeliveryError("media command is invalid") from exc
    if len(encoded) > 1024:
        raise ReminderDeliveryError("media command is invalid")
    return payload


def _parse_media_result(payload: Mapping[str, Any]) -> dict[str, Any] | None:
    if not isinstance(payload, Mapping):
        return None
    command_id = payload.get("command_id")
    ok = payload.get("ok")
    error_code = payload.get("error_code")
    if (
        not isinstance(command_id, str)
        or _COMMAND_ID_RE.fullmatch(command_id) is None
        or not isinstance(ok, bool)
    ):
        return None
    if ok:
        if error_code is not None:
            return None
        return {"command_id": command_id, "ok": True}
    if not isinstance(error_code, str) or error_code not in _MEDIA_ERROR_CODES:
        return None
    return {"command_id": command_id, "ok": False, "error_code": error_code}


def _parse_media_ready(payload: Mapping[str, Any]) -> dict[str, Any] | None:
    if not isinstance(payload, Mapping):
        return None
    command_id = payload.get("command_id")
    ok = payload.get("ok")
    error_code = payload.get("error_code")
    if (
        not isinstance(command_id, str)
        or _COMMAND_ID_RE.fullmatch(command_id) is None
        or not isinstance(ok, bool)
    ):
        return None
    if ok:
        if error_code is not None:
            return None
        return {"command_id": command_id, "ok": True}
    if not isinstance(error_code, str) or error_code not in _MEDIA_ERROR_CODES:
        return None
    return {"command_id": command_id, "ok": False, "error_code": error_code}


ReminderHub = DeviceEventHub

__all__ = [
    "DeviceEventHub",
    "ReminderDeliveryError",
    "ReminderHub",
    "ReminderSender",
]
