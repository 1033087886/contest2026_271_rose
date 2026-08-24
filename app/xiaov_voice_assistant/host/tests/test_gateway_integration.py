import asyncio
import unittest

from websockets.asyncio.client import connect
from websockets.asyncio.server import serve

from gateway.protocol import AudioFrame, AudioKind, decode_control, encode_control, event
from gateway.providers.mock import MockPipeline
from gateway.reminders import ReminderHub
from gateway.session import (
    GatewaySession,
    SUPPORTED_AUDIO,
    TTS_FIRST_AUDIO_TIMEOUT_SECONDS,
    _has_speakable_content,
    _take_complete_sentences,
)
from gateway.tools.device_media import DeviceMediaController
from simulator.device import run_simulation
from simulator.benchmark import run_benchmark


class GatewayIntegrationTests(unittest.IsolatedAsyncioTestCase):
    def test_first_tts_audio_deadline_allows_arm64_melotts_cold_start(self) -> None:
        # The provider owns a 60 s HTTP timeout. The session must not impose a
        # shorter first-audio deadline that races a healthy queued inference.
        self.assertGreater(TTS_FIRST_AUDIO_TIMEOUT_SECONDS, 60.0)

    def test_sentence_splitter_does_not_split_decimal_temperature(self) -> None:
        sentences, remainder = _take_complete_sentences("北京现在26.7度。下一段")
        self.assertEqual(sentences, ["北京现在26.7度。"])
        self.assertEqual(remainder, "下一段")

        sentences, remainder = _take_complete_sentences("北京现在26.")
        self.assertEqual(sentences, [])
        self.assertEqual(remainder, "北京现在26.")

    def test_tts_fragment_requires_a_real_token(self) -> None:
        self.assertTrue(_has_speakable_content("你好！"))
        self.assertTrue(_has_speakable_content("OpenVela"))
        self.assertFalse(_has_speakable_content(' "😄'))

    async def test_device_acknowledges_media_tool_command(self) -> None:
        hub = ReminderHub()

        async def handler(websocket):
            await GatewaySession(
                websocket,
                pipeline=MockPipeline(latency_ms=0),
                reminder_hub=hub,
            ).run()

        async with serve(handler, "127.0.0.1", 0) as server:
            port = server.sockets[0].getsockname()[1]
            async with connect(f"ws://127.0.0.1:{port}") as websocket:
                await websocket.send(
                    encode_control(
                        event("session.start", payload={"device_id": "media-test"})
                    )
                )
                self.assertEqual(decode_control(await websocket.recv())["type"], "session.ready")
                command_task = asyncio.create_task(
                    DeviceMediaController(hub).command("set_volume", {"volume_percent": 45})
                )
                command = decode_control(await websocket.recv())
                self.assertEqual(command["type"], "media.command")
                self.assertEqual(command["payload"]["volume_percent"], 45)
                await websocket.send(
                    encode_control(
                        event(
                            "media.result",
                            payload={"command_id": command["id"], "ok": True},
                        )
                    )
                )
                result = await asyncio.wait_for(command_task, 1)
                self.assertTrue(result["executed"])
                self.assertEqual(result["mode"], "device")

    async def test_authenticated_session_receives_active_reminder(self) -> None:
        hub = ReminderHub()

        async def handler(websocket):
            await GatewaySession(
                websocket,
                pipeline=MockPipeline(latency_ms=0),
                reminder_hub=hub,
            ).run()

        async with serve(handler, "127.0.0.1", 0) as server:
            port = server.sockets[0].getsockname()[1]
            async with connect(f"ws://127.0.0.1:{port}") as websocket:
                await websocket.send(
                    encode_control(
                        event("session.start", payload={"device_id": "reminder-test"})
                    )
                )
                self.assertEqual(decode_control(await websocket.recv())["type"], "session.ready")
                await hub.publish(
                    {
                        "timer_id": "timer-0007",
                        "label": "站起来活动",
                        "kind": "pomodoro",
                        "deadline_epoch": 1234.5,
                    }
                )
                reminder = decode_control(await websocket.recv())
                self.assertEqual(reminder["type"], "reminder")
                self.assertEqual(reminder["payload"]["timer_id"], "timer-0007")
                self.assertEqual(reminder["payload"]["label"], "站起来活动")

    async def test_complete_mock_voice_turn(self) -> None:
        observed: list[dict[str, object]] = []

        async def handler(websocket):
            await GatewaySession(
                websocket,
                pipeline=MockPipeline(latency_ms=0),
                event_observer=observed.append,
            ).run()

        async with serve(handler, "127.0.0.1", 0) as server:
            port = server.sockets[0].getsockname()[1]
            result = await run_simulation(
                f"ws://127.0.0.1:{port}",
                text="请设置一个番茄钟",
                duration_ms=40,
            )

        self.assertEqual(result.transcript, "请设置一个番茄钟")
        self.assertIn("离线模拟链路", result.answer)
        self.assertGreater(len(result.tts_pcm), 0)
        self.assertGreaterEqual(result.speech_end_to_first_tts_ms, 0)
        self.assertTrue(result.first_tts_before_answer_final)
        observed_types = [item["message"]["type"] for item in observed]
        self.assertIn("asr.final", observed_types)
        self.assertIn("assistant.final", observed_types)
        self.assertIn("tts.start", observed_types)
        self.assertIn("tts.end", observed_types)
        self.assertTrue(all("observed_at_unix_ms" in item for item in observed))

    async def test_new_turn_cancels_previous_pipeline(self) -> None:
        async def handler(websocket):
            await GatewaySession(websocket, pipeline=MockPipeline(latency_ms=100)).run()

        async with serve(handler, "127.0.0.1", 0) as server:
            port = server.sockets[0].getsockname()[1]
            async with connect(f"ws://127.0.0.1:{port}") as websocket:
                await websocket.send(
                    encode_control(
                        event("session.start", payload={"device_id": "cancel-test"})
                    )
                )
                ready = decode_control(await websocket.recv())
                self.assertEqual(ready["type"], "session.ready")

                for turn_id, stream_id in (("turn-old", 1), ("turn-new", 2)):
                    await websocket.send(
                        encode_control(
                            event(
                                "listen.start",
                                turn_id=turn_id,
                                payload={"stream_id": stream_id, "audio": SUPPORTED_AUDIO},
                            )
                        )
                    )
                    await websocket.send(
                        encode_control(
                            event(
                                "listen.stop",
                                turn_id=turn_id,
                                payload={"reason": "end_of_speech", "text_hint": turn_id},
                            )
                        )
                    )

                old_cancelled = False
                new_completed = False
                old_completed = False
                async with asyncio.timeout(5):
                    while not new_completed:
                        raw = await websocket.recv()
                        if isinstance(raw, bytes):
                            continue
                        message = decode_control(raw)
                        if (
                            message["type"] == "turn.cancelled"
                            and message.get("turn_id") == "turn-old"
                        ):
                            old_cancelled = True
                        if message["type"] == "tts.end":
                            if message.get("turn_id") == "turn-old":
                                old_completed = True
                            if message.get("turn_id") == "turn-new":
                                new_completed = True

                self.assertTrue(old_cancelled)
                self.assertFalse(old_completed)

    async def test_protocol_error_is_recoverable(self) -> None:
        async def handler(websocket):
            await GatewaySession(websocket, pipeline=MockPipeline(latency_ms=0)).run()

        async with serve(handler, "127.0.0.1", 0) as server:
            port = server.sockets[0].getsockname()[1]
            async with connect(f"ws://127.0.0.1:{port}") as websocket:
                await websocket.send(encode_control(event("ping", payload={"nonce": 1})))
                error = decode_control(await websocket.recv())
                self.assertEqual(error["type"], "error")
                self.assertTrue(error["payload"]["recoverable"])

    async def test_asr_partial_is_emitted_while_audio_is_still_arriving(self) -> None:
        async def handler(websocket):
            await GatewaySession(websocket, pipeline=MockPipeline(latency_ms=0)).run()

        async with serve(handler, "127.0.0.1", 0) as server:
            port = server.sockets[0].getsockname()[1]
            async with connect(f"ws://127.0.0.1:{port}") as websocket:
                await websocket.send(
                    encode_control(event("session.start", payload={"device_id": "partial-test"}))
                )
                ready = decode_control(await websocket.recv())
                self.assertEqual(ready["type"], "session.ready")
                turn_id = "turn-partial"
                stream_id = 7
                await websocket.send(
                    encode_control(
                        event(
                            "listen.start",
                            turn_id=turn_id,
                            payload={"stream_id": stream_id, "audio": SUPPORTED_AUDIO},
                        )
                    )
                )
                pcm = b"\x00" * 640
                for sequence in range(25):  # 500 ms reaches the mock partial threshold.
                    await websocket.send(
                        AudioFrame(
                            kind=AudioKind.MICROPHONE,
                            stream_id=stream_id,
                            sequence=sequence,
                            timestamp_ms=sequence * 20,
                            payload=pcm,
                        ).encode()
                    )
                partial = decode_control(await asyncio.wait_for(websocket.recv(), 1))
                self.assertEqual(partial["type"], "asr.partial")
                self.assertEqual(partial["turn_id"], turn_id)
                self.assertIn("500", partial["payload"]["text"])

    async def test_benchmark_distinguishes_hot_and_reconnect_sessions(self) -> None:
        session_count = 0

        async def handler(websocket):
            nonlocal session_count
            session_count += 1
            await GatewaySession(websocket, pipeline=MockPipeline(latency_ms=0)).run()

        async with serve(handler, "127.0.0.1", 0) as server:
            port = server.sockets[0].getsockname()[1]
            uri = f"ws://127.0.0.1:{port}"
            hot = await run_benchmark(uri, "热连接", 2, "hot")
            self.assertEqual(session_count, 1)
            reconnect = await run_benchmark(uri, "重连接", 2, "reconnect")

        self.assertEqual(session_count, 3)
        self.assertEqual(hot["metrics_ms"]["session_connect_ready_ms"]["samples"], 1)
        self.assertEqual(
            reconnect["metrics_ms"]["session_connect_ready_ms"]["samples"], 2
        )


if __name__ == "__main__":
    unittest.main()
