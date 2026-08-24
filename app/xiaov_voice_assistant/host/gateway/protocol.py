from __future__ import annotations

import json
import struct
import time
import uuid
from dataclasses import dataclass
from enum import IntEnum
from typing import Any, Mapping

PROTOCOL_VERSION = 1
AUDIO_MAGIC = b"XVAF"
AUDIO_HEADER = struct.Struct("!4sBBHIIII")
AUDIO_HEADER_SIZE = AUDIO_HEADER.size
MAX_AUDIO_PAYLOAD = 32_768
VIDEO_WIDTH = 320
VIDEO_HEIGHT = 240
VIDEO_BYTES_PER_PIXEL = 2
VIDEO_FRAME_BYTES = VIDEO_WIDTH * VIDEO_HEIGHT * VIDEO_BYTES_PER_PIXEL
VIDEO_FRAGMENT_BYTES = 25_600
VIDEO_FRAGMENT_COUNT = VIDEO_FRAME_BYTES // VIDEO_FRAGMENT_BYTES
VIDEO_FLAG_INDEX_MASK = 0x00FF
VIDEO_FLAG_COUNT_SHIFT = 8


class ProtocolError(ValueError):
    """Raised when a wire message violates protocol v1."""


class AudioKind(IntEnum):
    MICROPHONE = 1
    TTS = 2
    VIDEO_RGB565 = 3
    MUSIC = 4


def video_fragment_flags(index: int, count: int = VIDEO_FRAGMENT_COUNT) -> int:
    if isinstance(index, bool) or not isinstance(index, int) or not 0 <= index < count:
        raise ProtocolError("video fragment index is out of range")
    if isinstance(count, bool) or not isinstance(count, int) or not 1 <= count <= 0xFF:
        raise ProtocolError("video fragment count is out of range")
    return (count << VIDEO_FLAG_COUNT_SHIFT) | index


def decode_video_fragment_flags(flags: int) -> tuple[int, int]:
    index = flags & VIDEO_FLAG_INDEX_MASK
    count = flags >> VIDEO_FLAG_COUNT_SHIFT
    if count != VIDEO_FRAGMENT_COUNT or index >= count:
        raise ProtocolError("invalid RGB565 video fragment flags")
    return index, count


def video_frames(
    payload: bytes, *, stream_id: int, sequence: int, timestamp_ms: int
) -> tuple["AudioFrame", ...]:
    if len(payload) != VIDEO_FRAME_BYTES:
        raise ProtocolError("RGB565 video frame has the wrong size")
    return tuple(
        AudioFrame(
            kind=AudioKind.VIDEO_RGB565,
            stream_id=stream_id,
            sequence=sequence,
            timestamp_ms=timestamp_ms,
            payload=payload[offset : offset + VIDEO_FRAGMENT_BYTES],
            flags=video_fragment_flags(index),
        )
        for index, offset in enumerate(
            range(0, VIDEO_FRAME_BYTES, VIDEO_FRAGMENT_BYTES)
        )
    )


def monotonic_ms() -> int:
    return time.monotonic_ns() // 1_000_000


def event(
    message_type: str,
    *,
    payload: Mapping[str, Any] | None = None,
    turn_id: str | None = None,
    event_id: str | None = None,
    ts_ms: int | None = None,
) -> dict[str, Any]:
    message: dict[str, Any] = {
        "v": PROTOCOL_VERSION,
        "type": message_type,
        "id": event_id or uuid.uuid4().hex,
        "ts_ms": monotonic_ms() if ts_ms is None else ts_ms,
        "payload": dict(payload or {}),
    }
    if turn_id is not None:
        message["turn_id"] = turn_id
    return message


def encode_control(message: Mapping[str, Any]) -> str:
    validate_control(message)
    return json.dumps(message, ensure_ascii=False, separators=(",", ":"))


def decode_control(raw: str) -> dict[str, Any]:
    try:
        message = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ProtocolError(f"invalid JSON: {exc.msg}") from exc
    validate_control(message)
    return message


def validate_control(message: object) -> None:
    if not isinstance(message, dict):
        raise ProtocolError("control message must be a JSON object")
    if message.get("v") != PROTOCOL_VERSION:
        raise ProtocolError("unsupported protocol version")
    if not isinstance(message.get("type"), str) or not message["type"]:
        raise ProtocolError("type must be a non-empty string")
    if not isinstance(message.get("id"), str) or not message["id"]:
        raise ProtocolError("id must be a non-empty string")
    ts_ms = message.get("ts_ms")
    if isinstance(ts_ms, bool) or not isinstance(ts_ms, int) or ts_ms < 0:
        raise ProtocolError("ts_ms must be a non-negative integer")
    if not isinstance(message.get("payload"), dict):
        raise ProtocolError("payload must be an object")
    if "turn_id" in message and (
        not isinstance(message["turn_id"], str) or not message["turn_id"]
    ):
        raise ProtocolError("turn_id must be a non-empty string")


@dataclass(frozen=True, slots=True)
class AudioFrame:
    kind: AudioKind
    stream_id: int
    sequence: int
    timestamp_ms: int
    payload: bytes
    flags: int = 0

    def encode(self) -> bytes:
        _validate_u32("stream_id", self.stream_id)
        _validate_u32("sequence", self.sequence)
        _validate_u32("timestamp_ms", self.timestamp_ms)
        if isinstance(self.flags, bool) or not 0 <= self.flags <= 0xFFFF:
            raise ProtocolError("flags must fit uint16")
        if len(self.payload) > MAX_AUDIO_PAYLOAD:
            raise ProtocolError("binary payload exceeds protocol limit")
        if self.kind is AudioKind.VIDEO_RGB565:
            index, _ = decode_video_fragment_flags(self.flags)
            expected = VIDEO_FRAGMENT_BYTES
            if index == VIDEO_FRAGMENT_COUNT - 1:
                expected = VIDEO_FRAME_BYTES - VIDEO_FRAGMENT_BYTES * index
            if len(self.payload) != expected:
                raise ProtocolError("RGB565 video fragment has the wrong size")
        header = AUDIO_HEADER.pack(
            AUDIO_MAGIC,
            PROTOCOL_VERSION,
            int(self.kind),
            self.flags,
            self.stream_id,
            self.sequence,
            self.timestamp_ms,
            len(self.payload),
        )
        return header + self.payload

    @classmethod
    def decode(cls, raw: bytes) -> "AudioFrame":
        if len(raw) < AUDIO_HEADER_SIZE:
            raise ProtocolError("audio frame is shorter than its header")
        magic, version, kind, flags, stream_id, sequence, timestamp_ms, length = (
            AUDIO_HEADER.unpack_from(raw)
        )
        if magic != AUDIO_MAGIC:
            raise ProtocolError("invalid audio magic")
        if version != PROTOCOL_VERSION:
            raise ProtocolError("unsupported audio protocol version")
        try:
            audio_kind = AudioKind(kind)
        except ValueError as exc:
            raise ProtocolError(f"unknown audio kind: {kind}") from exc
        if length > MAX_AUDIO_PAYLOAD:
            raise ProtocolError("binary payload exceeds protocol limit")
        if len(raw) != AUDIO_HEADER_SIZE + length:
            raise ProtocolError("audio payload length mismatch")
        frame = cls(
            kind=audio_kind,
            stream_id=stream_id,
            sequence=sequence,
            timestamp_ms=timestamp_ms,
            payload=raw[AUDIO_HEADER_SIZE:],
            flags=flags,
        )
        if frame.kind is AudioKind.VIDEO_RGB565:
            index, _ = decode_video_fragment_flags(frame.flags)
            expected = VIDEO_FRAGMENT_BYTES
            if index == VIDEO_FRAGMENT_COUNT - 1:
                expected = VIDEO_FRAME_BYTES - VIDEO_FRAGMENT_BYTES * index
            if len(frame.payload) != expected:
                raise ProtocolError("RGB565 video fragment has the wrong size")
        return frame


def _validate_u32(name: str, value: int) -> None:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 0xFFFFFFFF:
        raise ProtocolError(f"{name} must fit uint32")
