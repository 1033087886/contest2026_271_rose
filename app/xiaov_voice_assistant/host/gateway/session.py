from __future__ import annotations

import asyncio
import hmac
import inspect
import logging
import time
import uuid
from collections.abc import Awaitable, Callable
from dataclasses import dataclass, field
from typing import Any

from websockets.asyncio.server import ServerConnection
from websockets.exceptions import ConnectionClosed

from gateway.protocol import (
    AudioFrame,
    AudioKind,
    ProtocolError,
    decode_control,
    encode_control,
    event,
)
from gateway.providers.base import StreamingAsr, VoicePipeline
from gateway.providers.mock import MockPipeline
from gateway.reminders import ReminderDeliveryError, ReminderHub
from gateway.wake_gate import WakeVerdict, evaluate as evaluate_wake

LOGGER = logging.getLogger(__name__)
EventObserver = Callable[[dict[str, Any]], Awaitable[None] | None]
MAX_TURN_AUDIO_BYTES = 16_000 * 2 * 60
# The self-hosted MeloTTS endpoint buffers one sentence before returning PCM.
# A cold request has taken over 15 seconds while another request was draining,
# so a short gateway deadline can race a healthy response and move the board
# from speaking to error.  Let the provider's 60 second HTTP deadline remain
# authoritative, with a small margin for task scheduling and the SSH hop.
TTS_FIRST_AUDIO_TIMEOUT_SECONDS = 65.0
SUPPORTED_AUDIO = {
    "encoding": "pcm_s16le",
    "sample_rate": 16_000,
    "channels": 1,
    "frame_ms": 20,
}
# Spoken when the wake phrase arrives with nothing after it. Deliberately not an
# LLM call: the user has not asked anything yet, so a round trip would add
# seconds and spend tokens to say one word.
WAKE_ACKNOWLEDGEMENT = "我在。"


class SessionRejected(Exception):
    """Raised to abandon a session that failed authentication."""


@dataclass(slots=True)
class Turn:
    turn_id: str
    stream_id: int
    asr: StreamingAsr
    expected_sequence: int = 0
    received_bytes: int = 0
    pcm: bytearray = field(default_factory=bytearray)
    """
    Turn audio retained for the local wake pre-gate.

    Only accumulated when a spotter is installed, because the transcript path
    does not need it and a 60 second turn is 1.9 MB.
    """


class GatewaySession:
    def __init__(
        self,
        websocket: ServerConnection,
        *,
        pipeline: VoicePipeline | None = None,
        auth_token: str | None = None,
        reminder_hub: ReminderHub | None = None,
        require_wake_phrase: bool = False,
        wake_spotter: Any | None = None,
        event_observer: EventObserver | None = None,
        video_bridge: Any | None = None,
    ) -> None:
        self.websocket = websocket
        self.pipeline = pipeline or MockPipeline()
        self.auth_token = auth_token
        self.reminder_hub = reminder_hub
        # When set, a turn is answered only if the transcript names the device.
        # This is what lets the board stream on plain speech detection and leave
        # "was I addressed" to the cloud, instead of deciding on-device with a
        # model whose score straddled its own threshold.
        self.require_wake_phrase = require_wake_phrase
        # Optional local sherpa-onnx spotter. When present it runs before ASR and
        # can end a turn without spending a cloud round trip. It only ever
        # short-circuits a *confident* negative, meaning silence: recall is ~93%
        # and its misses are indistinguishable from hits by loudness, so a miss
        # over speech falls through to the transcript gate, which stays the
        # authority on what was said.
        self.wake_spotter = wake_spotter
        # Optional, explicit HIL/audit hook for server-to-device control events.
        # It never sees microphone or TTS PCM and is disabled in normal runs.
        self.event_observer = event_observer
        self.video_bridge = video_bridge
        self.session_id = uuid.uuid4().hex
        self.started = False
        self.capture: Turn | None = None
        self.turn_task: asyncio.Task[None] | None = None
        self.active_turn_id: str | None = None
        self._send_lock = asyncio.Lock()
        self._tts_stream_id = 1000
        self._reminder_subscription: str | None = None
        self._video_subscription: str | None = None

    async def run(self) -> None:
        try:
            async for raw in self.websocket:
                try:
                    if isinstance(raw, str):
                        await self._handle_control(decode_control(raw))
                    else:
                        await self._handle_audio(AudioFrame.decode(raw))
                except ProtocolError as exc:
                    await self.send_error("protocol_error", str(exc), recoverable=True)
        except ConnectionClosed as exc:
            LOGGER.info(
                "device websocket closed code=%s reason=%s",
                exc.code,
                exc.reason or "none",
            )
        except SessionRejected:
            pass
        finally:
            if self.video_bridge is not None:
                await self.video_bridge.unregister(self._video_subscription)
                self._video_subscription = None
            if self.reminder_hub is not None:
                await self.reminder_hub.unregister(self._reminder_subscription)
                self._reminder_subscription = None
            await self._cancel_active(send_event=False)

    async def _handle_control(self, message: dict[str, Any]) -> None:
        message_type = message["type"]
        if not self.started:
            if message_type != "session.start":
                raise ProtocolError("session.start must be the first event")
            await self._start_session(message)
            return

        if message_type == "listen.start":
            await self._start_capture(message)
        elif message_type == "listen.stop":
            await self._stop_capture(message)
        elif message_type == "turn.cancel":
            await self._cancel_active(send_event=True, reason=message["payload"].get("reason"))
        elif message_type == "ping":
            await self.send_control(
                event("pong", payload=message["payload"], turn_id=message.get("turn_id"))
            )
        elif message_type == "media.result":
            if (
                self.reminder_hub is None
                or not await self.reminder_hub.resolve_media_result(
                    self._reminder_subscription, message["payload"]
                )
            ):
                raise ProtocolError("media.result does not match a pending command")
        elif message_type == "media.ready":
            if (
                self.reminder_hub is None
                or not await self.reminder_hub.resolve_media_ready(
                    self._reminder_subscription, message["payload"]
                )
            ):
                raise ProtocolError("media.ready does not match a pending stream")
        elif message_type == "display.command":
            await self._handle_display_command(message)
        else:
            raise ProtocolError(f"unexpected client event: {message_type}")

    async def _start_session(self, message: dict[str, Any]) -> None:
        device_id = message["payload"].get("device_id")
        if (
            not isinstance(device_id, str)
            or not 1 <= len(device_id) <= 64
            or any(ord(character) < 32 or ord(character) == 127 for character in device_id)
        ):
            raise ProtocolError("session.start payload.device_id is required")
        if self.auth_token is not None:
            await self._verify_token(message["payload"].get("token"))
        self.started = True
        if self.reminder_hub is not None:
            try:
                self._reminder_subscription = await self.reminder_hub.register(
                    device_id, self.send_control, self.send_binary
                )
            except ReminderDeliveryError as exc:
                self.started = False
                raise ProtocolError("reminder subscriber limit reached") from exc
        if self.video_bridge is not None:
            self._video_subscription = await self.video_bridge.register(
                device_id, self.send_binary, self.send_control
            )
        await self.send_control(
            event(
                "session.ready",
                payload={"session_id": self.session_id, "audio": SUPPORTED_AUDIO},
            )
        )
        if self.video_bridge is not None:
            await self.video_bridge.restore(self._video_subscription)

    async def _handle_display_command(self, message: dict[str, Any]) -> None:
        if self.video_bridge is None or self._video_subscription is None:
            raise ProtocolError("display video is unavailable")
        payload = message["payload"]
        action = payload.get("action")
        source = payload.get("source")
        if action not in (
            "open_monitor",
            "close_monitor",
            "play_video",
            "stop_video",
        ):
            raise ProtocolError("display.command payload.action is invalid")
        if source is not None and (
            not isinstance(source, str) or not 1 <= len(source) <= 1024
        ):
            raise ProtocolError("display.command payload.source is invalid")
        parameters = {} if source is None else {"source": source}
        try:
            await self.video_bridge.command(
                action, parameters, subscription=self._video_subscription
            )
        except Exception:
            LOGGER.exception("display command failed")
            await self.send_control(
                event(
                    "display.state",
                    payload={"mode": "closed", "status": "error"},
                )
            )

    async def _verify_token(self, presented: object) -> None:
        """Rejects a session whose token does not match the configured secret.

        compare_digest keeps the comparison constant-time. On failure the
        connection is closed rather than left open with started=False: a client
        that may retry session.start indefinitely turns a public gateway into an
        offline guessing oracle.
        """
        ok = isinstance(presented, str) and hmac.compare_digest(
            presented, self.auth_token or ""
        )
        if ok:
            return
        LOGGER.warning("rejected session.start with an invalid token")
        await self.send_error(
            "unauthorized", "invalid or missing token", recoverable=False
        )
        await self.websocket.close(code=1008, reason="unauthorized")
        raise SessionRejected("invalid or missing session token")

    async def _start_capture(self, message: dict[str, Any]) -> None:
        turn_id = _required_turn_id(message)
        payload = message["payload"]
        stream_id = payload.get("stream_id")
        if isinstance(stream_id, bool) or not isinstance(stream_id, int) or not 0 <= stream_id <= 0xFFFFFFFF:
            raise ProtocolError("listen.start payload.stream_id must fit uint32")
        if payload.get("audio") != SUPPORTED_AUDIO:
            raise ProtocolError("unsupported audio format")
        await self._cancel_active(
            send_event=self.capture is not None or self.turn_task is not None,
            reason="superseded",
        )
        if self.video_bridge is not None:
            await self.video_bridge.pause(self._video_subscription)
        self.capture = Turn(
            turn_id=turn_id,
            stream_id=stream_id,
            asr=self.pipeline.start_asr(),
        )

    async def _handle_audio(self, frame: AudioFrame) -> None:
        if not self.started:
            raise ProtocolError("session.start must precede audio")
        if frame.kind is not AudioKind.MICROPHONE:
            raise ProtocolError("client may only send microphone audio")
        if self.capture is None:
            raise ProtocolError("microphone audio received outside listen.start/listen.stop")
        if frame.stream_id != self.capture.stream_id:
            raise ProtocolError("microphone stream_id does not match active turn")
        if frame.sequence != self.capture.expected_sequence:
            raise ProtocolError(
                f"unexpected audio sequence {frame.sequence}; expected {self.capture.expected_sequence}"
            )
        if not frame.payload or len(frame.payload) % 2 != 0:
            raise ProtocolError("pcm_s16le payload must contain complete samples")
        if self.capture.received_bytes + len(frame.payload) > MAX_TURN_AUDIO_BYTES:
            raise ProtocolError("turn audio exceeds 60 second limit")
        self.capture.received_bytes += len(frame.payload)
        self.capture.expected_sequence += 1
        if self.wake_spotter is not None and self.require_wake_phrase:
            self.capture.pcm.extend(frame.payload)
        partial = await self.capture.asr.push_audio(frame.payload)
        if partial:
            await self.send_control(
                event("asr.partial", payload={"text": partial}, turn_id=self.capture.turn_id)
            )

    async def _stop_capture(self, message: dict[str, Any]) -> None:
        turn_id = _required_turn_id(message)
        if self.capture is None or self.capture.turn_id != turn_id:
            raise ProtocolError("listen.stop does not match the active turn")
        turn = self.capture
        self.capture = None
        text_hint = message["payload"].get("text_hint")
        if text_hint is not None and not isinstance(text_hint, str):
            raise ProtocolError("listen.stop payload.text_hint must be a string")
        self.active_turn_id = turn.turn_id
        self.turn_task = asyncio.create_task(self._run_pipeline(turn, text_hint))

    async def _local_wake_allows(self, turn: Turn) -> bool:
        """Run the local spotter; False means the turn can be dropped before ASR.

        Returns True on anything short of a confident negative, including engine
        errors. The cost of a wrong True is one cloud round trip; the cost of a
        wrong False is the device ignoring a real request, which is the failure
        the on-device model was retired for.
        """
        if self.wake_spotter is None or not self.require_wake_phrase:
            return True
        pcm = bytes(turn.pcm)
        turn.pcm.clear()
        if not pcm:
            return True
        try:
            verdict = await asyncio.to_thread(self.wake_spotter.evaluate_pcm, pcm)
        except Exception:
            LOGGER.exception("local wake spotter failed; falling through to ASR")
            return True

        await self.send_control(
            event(
                "wake.local",
                payload={
                    "fired": verdict.fired,
                    "matched": verdict.matched,
                    "uncertain": verdict.uncertain,
                },
                turn_id=turn.turn_id,
            )
        )
        return verdict.fired or verdict.uncertain

    async def _run_pipeline(self, turn: Turn, text_hint: str | None) -> None:
        try:
            if not await self._local_wake_allows(turn):
                # No wake phrase and no speech worth transcribing. Ending here is
                # the point of the pre-gate: no upload, no ASR, no tokens.
                await self.send_control(
                    event(
                        "turn.ignored",
                        payload={"reason": "local_wake_absent"},
                        turn_id=turn.turn_id,
                    )
                )
                return

            transcript = await turn.asr.finish(text_hint)
            await self.send_control(
                event("asr.final", payload={"text": transcript}, turn_id=turn.turn_id)
            )

            prompt = transcript
            if self.require_wake_phrase:
                verdict = evaluate_wake(transcript)
                await self.send_control(
                    event(
                        "wake.verdict",
                        payload={
                            "woke": verdict.woke,
                            "matched": verdict.matched_text,
                            "request": verdict.request,
                        },
                        turn_id=turn.turn_id,
                    )
                )
                if not verdict.woke:
                    # Speech that was not addressed to the device. Ending the turn
                    # here is the whole economy of the design: no answer, no TTS,
                    # no tokens.
                    await self.send_control(
                        event(
                            "turn.ignored",
                            payload={"reason": "wake_phrase_absent"},
                            turn_id=turn.turn_id,
                        )
                    )
                    return
                prompt = verdict.request or WAKE_ACKNOWLEDGEMENT
                if not verdict.request:
                    # Only the wake phrase was spoken. Answer locally so the
                    # device can respond immediately and then listen again.
                    await self._speak_fixed_reply(turn, WAKE_ACKNOWLEDGEMENT)
                    return

            answer_parts: list[str] = []
            sentence_buffer = ""
            stream_id: int | None = None
            sequence = 0
            samples = 0
            sentence_queue: asyncio.Queue[str | None] = asyncio.Queue(maxsize=8)
            first_audio_sent = asyncio.Event()

            async def send_tts_sentence(sentence: str) -> None:
                nonlocal stream_id, sequence, samples
                if stream_id is None:
                    self._tts_stream_id = (self._tts_stream_id + 1) & 0xFFFFFFFF
                    stream_id = self._tts_stream_id
                    await self.send_control(
                        event(
                            "tts.start",
                            payload={"stream_id": stream_id, "audio": SUPPORTED_AUDIO},
                            turn_id=turn.turn_id,
                        )
                    )
                async for pcm in self.pipeline.synthesize(sentence):
                    timestamp_ms = samples * 1000 // SUPPORTED_AUDIO["sample_rate"]
                    await self.send_audio(
                        AudioFrame(
                            kind=AudioKind.TTS,
                            stream_id=stream_id,
                            sequence=sequence,
                            timestamp_ms=timestamp_ms,
                            payload=pcm,
                        )
                    )
                    sequence += 1
                    samples += len(pcm) // 2
                    first_audio_sent.set()

            async def tts_worker() -> None:
                while True:
                    sentence = await sentence_queue.get()
                    if sentence is None:
                        return
                    await send_tts_sentence(sentence)

            tts_task = asyncio.create_task(tts_worker())
            queued_sentence = False
            waited_for_first_audio = False

            async def enqueue_sentence(sentence: str) -> None:
                nonlocal queued_sentence, waited_for_first_audio
                if not _has_speakable_content(sentence):
                    return
                await sentence_queue.put(sentence)
                queued_sentence = True
                if not waited_for_first_audio:
                    await asyncio.wait_for(
                        first_audio_sent.wait(),
                        timeout=TTS_FIRST_AUDIO_TIMEOUT_SECONDS,
                    )
                    waited_for_first_audio = True

            try:
                async for chunk in self.pipeline.answer_chunks(prompt):
                    answer_parts.append(chunk)
                    await self.send_control(
                        event(
                            "assistant.delta",
                            payload={"text": chunk},
                            turn_id=turn.turn_id,
                        )
                    )
                    sentence_buffer += chunk
                    sentences, sentence_buffer = _take_complete_sentences(sentence_buffer)
                    for sentence in sentences:
                        await enqueue_sentence(sentence)
                answer = "".join(answer_parts)
                await self.send_control(
                    event("assistant.final", payload={"text": answer}, turn_id=turn.turn_id)
                )
                if sentence_buffer:
                    await enqueue_sentence(sentence_buffer)
                if not queued_sentence:
                    await enqueue_sentence(answer)
                if not queued_sentence:
                    await enqueue_sentence("抱歉，这个回答暂时无法朗读。")
                await sentence_queue.put(None)
                await tts_task
            finally:
                if not tts_task.done():
                    tts_task.cancel()
                    try:
                        await tts_task
                    except asyncio.CancelledError:
                        pass
            assert stream_id is not None
            await self.send_control(
                event(
                    "tts.end",
                    payload={"stream_id": stream_id, "samples": samples},
                    turn_id=turn.turn_id,
                )
            )
        except asyncio.CancelledError:
            raise
        except ConnectionClosed:
            return
        except Exception:
            LOGGER.exception("turn pipeline failed")
            await self.send_error(
                "pipeline_failed", "turn pipeline failed", recoverable=True, turn_id=turn.turn_id
            )
        finally:
            await turn.asr.cancel()
            if self.video_bridge is not None:
                await self.video_bridge.resume(self._video_subscription)
            if self.turn_task is asyncio.current_task():
                self.turn_task = None
                self.active_turn_id = None

    async def _speak_fixed_reply(self, turn: Turn, text: str) -> None:
        """Synthesise a canned line without consulting the answer provider.

        Used for the bare wake phrase, where an LLM round trip would add seconds
        and spend tokens to produce a single acknowledgement.
        """
        await self.send_control(
            event("assistant.final", payload={"text": text}, turn_id=turn.turn_id)
        )
        self._tts_stream_id = (self._tts_stream_id + 1) & 0xFFFFFFFF
        stream_id = self._tts_stream_id
        await self.send_control(
            event(
                "tts.start",
                payload={"stream_id": stream_id, "audio": SUPPORTED_AUDIO},
                turn_id=turn.turn_id,
            )
        )
        sequence = 0
        samples = 0
        async for pcm in self.pipeline.synthesize(text):
            await self.send_audio(
                AudioFrame(
                    kind=AudioKind.TTS,
                    stream_id=stream_id,
                    sequence=sequence,
                    timestamp_ms=samples * 1000 // SUPPORTED_AUDIO["sample_rate"],
                    payload=pcm,
                )
            )
            sequence += 1
            samples += len(pcm) // 2
        await self.send_control(
            event(
                "tts.end",
                payload={"stream_id": stream_id, "samples": samples},
                turn_id=turn.turn_id,
            )
        )

    async def _cancel_active(
        self,
        *,
        send_event: bool,
        reason: str | None = None,
    ) -> None:
        cancelled_turn_id = self.capture.turn_id if self.capture else self.active_turn_id
        cancelled_capture = self.capture
        self.capture = None
        task = self.turn_task
        self.turn_task = None
        self.active_turn_id = None
        if cancelled_capture is not None:
            await cancelled_capture.asr.cancel()
        if task is not None:
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
        if send_event and (cancelled_turn_id or task is not None):
            await self.send_control(
                event(
                    "turn.cancelled",
                    payload={"reason": reason or "client_request"},
                    turn_id=cancelled_turn_id,
                )
            )

    async def send_control(self, message: dict[str, Any]) -> None:
        encoded = encode_control(message)
        async with self._send_lock:
            await self.websocket.send(encoded)
            if self.event_observer is not None:
                try:
                    result = self.event_observer(
                        {
                            "observed_at_unix_ms": time.time_ns() // 1_000_000,
                            "session_id": self.session_id,
                            "message": message,
                        }
                    )
                    if inspect.isawaitable(result):
                        await result
                except Exception:
                    # Auditing must never make a live voice turn fail. The
                    # caller can detect a missing record in its HIL report.
                    LOGGER.exception("gateway event observer failed")

    async def send_audio(self, frame: AudioFrame) -> None:
        encoded = frame.encode()
        async with self._send_lock:
            await self.websocket.send(encoded)

    async def send_binary(self, encoded: bytes) -> None:
        async with self._send_lock:
            await self.websocket.send(encoded)

    async def send_error(
        self,
        code: str,
        message: str,
        *,
        recoverable: bool,
        turn_id: str | None = None,
    ) -> None:
        await self.send_control(
            event(
                "error",
                payload={"code": code, "message": message, "recoverable": recoverable},
                turn_id=turn_id,
            )
        )


def _required_turn_id(message: dict[str, Any]) -> str:
    turn_id = message.get("turn_id")
    if not isinstance(turn_id, str) or not turn_id:
        raise ProtocolError(f"{message['type']} requires turn_id")
    return turn_id


def _take_complete_sentences(text: str) -> tuple[list[str], str]:
    sentences: list[str] = []
    start = 0
    for index, character in enumerate(text):
        if character in "。！？.!?\n":
            if (
                character == "."
                and index > 0
                and text[index - 1].isdigit()
                and (index + 1 == len(text) or text[index + 1].isdigit())
            ):
                # A streamed decimal can temporarily end at "26." before the
                # next chunk contributes "7". Hold it until the next chunk (or
                # end of answer) instead of creating two TTS requests.
                continue
            sentence = text[start : index + 1].strip()
            if sentence:
                sentences.append(sentence)
            start = index + 1
    return sentences, text[start:]


def _has_speakable_content(text: str) -> bool:
    """Return whether a streaming fragment contains a pronounceable token."""
    return any(character.isalnum() for character in text)
