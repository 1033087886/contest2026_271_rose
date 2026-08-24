from __future__ import annotations

import argparse
import asyncio
import json
import math
import struct
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from websockets.asyncio.client import ClientConnection, connect

from gateway.protocol import (
    AudioFrame,
    AudioKind,
    decode_control,
    encode_control,
    event,
    monotonic_ms,
)
from gateway.session import SUPPORTED_AUDIO


@dataclass(slots=True)
class SimulationResult:
    transcript: str
    answer: str
    tts_pcm: bytes
    speech_end_to_asr_final_ms: int
    speech_end_to_first_tts_ms: int
    total_turn_ms: int
    first_tts_before_answer_final: bool

    def summary(self) -> dict[str, Any]:
        return {
            "transcript": self.transcript,
            "answer": self.answer,
            "tts_bytes": len(self.tts_pcm),
            "speech_end_to_asr_final_ms": self.speech_end_to_asr_final_ms,
            "speech_end_to_first_tts_ms": self.speech_end_to_first_tts_ms,
            "total_turn_ms": self.total_turn_ms,
            "first_tts_before_answer_final": self.first_tts_before_answer_final,
        }


class SimulatedDevice:
    def __init__(self, uri: str, *, timeout_seconds: float = 10.0) -> None:
        if timeout_seconds <= 0:
            raise ValueError("timeout_seconds must be greater than zero")
        self.uri = uri
        self.timeout_seconds = timeout_seconds
        self.websocket: ClientConnection | None = None
        self.connect_ready_ms: int | None = None
        self._next_stream_id = 0

    async def __aenter__(self) -> "SimulatedDevice":
        started_ms = monotonic_ms()
        self.websocket = await connect(self.uri, max_size=2 * 1024 * 1024)
        try:
            await self.websocket.send(
                encode_control(
                    event(
                        "session.start",
                        payload={
                            "device_id": "pc-simulator",
                            "capabilities": {"touch": True, "barge_in": False},
                        },
                    )
                )
            )
            raw = await asyncio.wait_for(
                self.websocket.recv(), self.timeout_seconds
            )
            if not isinstance(raw, str):
                raise RuntimeError("expected a text session.ready event")
            ready = decode_control(raw)
            if ready["type"] != "session.ready":
                raise RuntimeError(f"expected session.ready, got {ready['type']}")
        except BaseException:
            await self.websocket.close()
            self.websocket = None
            raise
        self.connect_ready_ms = monotonic_ms() - started_ms
        return self

    async def __aexit__(self, exc_type, exc, traceback) -> None:
        if self.websocket is not None:
            await self.websocket.close()
            self.websocket = None

    async def run_turn(self, *, text: str, duration_ms: int = 1_000) -> SimulationResult:
        if self.websocket is None:
            raise RuntimeError("simulated device is not connected")
        if duration_ms <= 0:
            raise ValueError("duration_ms must be greater than zero")
        websocket = self.websocket
        turn_id = f"turn-{uuid.uuid4().hex[:12]}"
        self._next_stream_id = (self._next_stream_id + 1) & 0xFFFFFFFF
        if self._next_stream_id == 0:
            self._next_stream_id = 1
        stream_id = self._next_stream_id

        await websocket.send(
            encode_control(
                event(
                    "listen.start",
                    turn_id=turn_id,
                    payload={"stream_id": stream_id, "audio": SUPPORTED_AUDIO},
                )
            )
        )
        for frame in _microphone_frames(stream_id, duration_ms):
            await websocket.send(frame.encode())

        speech_end_ms = monotonic_ms()
        await websocket.send(
            encode_control(
                event(
                    "listen.stop",
                    turn_id=turn_id,
                    payload={"reason": "end_of_speech", "text_hint": text},
                )
            )
        )

        transcript = ""
        answer = ""
        tts_pcm = bytearray()
        asr_final_ms: int | None = None
        first_tts_ms: int | None = None
        first_tts_before_answer_final = False
        tts_stream_id: int | None = None
        expected_tts_sequence = 0
        tts_done = False
        answer_done = False
        deadline = asyncio.get_running_loop().time() + self.timeout_seconds
        while not (tts_done and answer_done):
            remaining = deadline - asyncio.get_running_loop().time()
            if remaining <= 0:
                raise TimeoutError("simulation timed out waiting for turn completion")
            raw = await asyncio.wait_for(websocket.recv(), remaining)
            now_ms = monotonic_ms()
            if isinstance(raw, bytes):
                frame = AudioFrame.decode(raw)
                if frame.kind is not AudioKind.TTS:
                    raise RuntimeError("server sent a non-TTS audio frame")
                if tts_stream_id is None:
                    raise RuntimeError("server sent TTS audio before tts.start")
                if frame.stream_id != tts_stream_id:
                    raise RuntimeError("TTS audio stream_id changed within a turn")
                if frame.sequence != expected_tts_sequence:
                    raise RuntimeError(
                        f"unexpected TTS sequence {frame.sequence}; expected {expected_tts_sequence}"
                    )
                expected_tts_sequence += 1
                if first_tts_ms is None:
                    first_tts_ms = now_ms
                    first_tts_before_answer_final = not answer_done
                tts_pcm.extend(frame.payload)
                continue

            message = decode_control(raw)
            if message["type"] == "error" and message.get("turn_id") in (None, turn_id):
                raise RuntimeError(message["payload"]["message"])
            if message.get("turn_id") != turn_id:
                continue
            if message["type"] == "asr.final":
                transcript = message["payload"]["text"]
                asr_final_ms = now_ms
            elif message["type"] == "assistant.final":
                answer = message["payload"]["text"]
                answer_done = True
            elif message["type"] == "tts.start":
                if tts_stream_id is not None:
                    raise RuntimeError("server sent duplicate tts.start")
                tts_stream_id = message["payload"]["stream_id"]
            elif message["type"] == "tts.end":
                if message["payload"].get("stream_id") != tts_stream_id:
                    raise RuntimeError("tts.end stream_id does not match tts.start")
                if message["payload"].get("samples") != len(tts_pcm) // 2:
                    raise RuntimeError("tts.end sample count does not match received PCM")
                tts_done = True

        end_ms = monotonic_ms()
        if asr_final_ms is None or first_tts_ms is None:
            raise RuntimeError("turn completed without ASR final or TTS audio")
        return SimulationResult(
            transcript=transcript,
            answer=answer,
            tts_pcm=bytes(tts_pcm),
            speech_end_to_asr_final_ms=asr_final_ms - speech_end_ms,
            speech_end_to_first_tts_ms=first_tts_ms - speech_end_ms,
            total_turn_ms=end_ms - speech_end_ms,
            first_tts_before_answer_final=first_tts_before_answer_final,
        )


async def run_simulation(
    uri: str,
    *,
    text: str,
    duration_ms: int = 1_000,
    timeout_seconds: float = 10.0,
) -> SimulationResult:
    async with SimulatedDevice(uri, timeout_seconds=timeout_seconds) as device:
        return await device.run_turn(text=text, duration_ms=duration_ms)


def _microphone_frames(stream_id: int, duration_ms: int):
    frame_ms = SUPPORTED_AUDIO["frame_ms"]
    frame_samples = SUPPORTED_AUDIO["sample_rate"] * frame_ms // 1000
    frame_count = max(1, math.ceil(duration_ms / frame_ms))
    silence = struct.pack(f"<{frame_samples}h", *([0] * frame_samples))
    for sequence in range(frame_count):
        yield AudioFrame(
            kind=AudioKind.MICROPHONE,
            stream_id=stream_id,
            sequence=sequence,
            timestamp_ms=sequence * frame_ms,
            payload=silence,
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Simulate a Xiao V device turn")
    parser.add_argument("--uri", default="ws://127.0.0.1:8765")
    parser.add_argument("--text", default="你好，openvela")
    parser.add_argument("--duration-ms", type=int, default=1_000)
    parser.add_argument("--output", type=Path, default=Path("artifacts/simulated_tts.pcm"))
    return parser


def main() -> None:
    args = build_parser().parse_args()
    result = asyncio.run(
        run_simulation(args.uri, text=args.text, duration_ms=args.duration_ms)
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(result.tts_pcm)
    print(json.dumps(result.summary(), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
