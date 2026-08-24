from __future__ import annotations

import struct
import time
from dataclasses import dataclass


GCF1_MAGIC = b"GCF1"
GCF1_VERSION = 1
GCF1_HEADER = struct.Struct("!4sBBHIQHHI")
GCF1_FLAG_TIME_SYNCED = 0x01
MAX_JPEG_BYTES = 512 * 1024


class GreeCamFrameError(ValueError):
    """Raised when a GreeCam video frame violates the bounded wire format."""


@dataclass(frozen=True, slots=True)
class GreeCamFrame:
    sequence: int
    capture_epoch_ms: int
    time_synced: bool
    width: int
    height: int
    jpeg: bytes
    received_epoch_ms: int

    @property
    def latency_ms(self) -> int | None:
        if not self.time_synced or self.capture_epoch_ms <= 0:
            return None
        return max(0, self.received_epoch_ms - self.capture_epoch_ms)


def decode_gcf1(payload: bytes, *, received_epoch_ms: int | None = None) -> GreeCamFrame:
    if len(payload) < GCF1_HEADER.size:
        raise GreeCamFrameError("frame is shorter than the GCF1 header")
    magic, version, flags, header_length, sequence, captured, width, height, length = (
        GCF1_HEADER.unpack_from(payload)
    )
    if magic != GCF1_MAGIC:
        raise GreeCamFrameError("invalid GCF1 magic")
    if version != GCF1_VERSION:
        raise GreeCamFrameError("unsupported GCF1 version")
    if header_length != GCF1_HEADER.size:
        raise GreeCamFrameError("invalid GCF1 header length")
    if width <= 0 or height <= 0 or width > 4096 or height > 4096:
        raise GreeCamFrameError("invalid frame dimensions")
    if length <= 4 or length > MAX_JPEG_BYTES:
        raise GreeCamFrameError("invalid JPEG length")
    if len(payload) != header_length + length:
        raise GreeCamFrameError("GCF1 payload length mismatch")
    jpeg = payload[header_length:]
    if jpeg[:2] != b"\xff\xd8" or jpeg[-2:] != b"\xff\xd9":
        raise GreeCamFrameError("GCF1 payload is not a complete JPEG")
    return GreeCamFrame(
        sequence=sequence,
        capture_epoch_ms=captured,
        time_synced=bool(flags & GCF1_FLAG_TIME_SYNCED),
        width=width,
        height=height,
        jpeg=jpeg,
        received_epoch_ms=(
            time.time_ns() // 1_000_000
            if received_epoch_ms is None
            else received_epoch_ms
        ),
    )


__all__ = [
    "GCF1_HEADER",
    "GCF1_MAGIC",
    "GCF1_VERSION",
    "GreeCamFrame",
    "GreeCamFrameError",
    "decode_gcf1",
]
