from __future__ import annotations

import asyncio
import shutil
import time
import unittest
from types import SimpleNamespace
from unittest.mock import patch
from pathlib import Path

from gateway.protocol import AudioFrame, AudioKind, VIDEO_FRAME_BYTES
from gateway.video_bridge import MQTT_RETRY_DELAY_SECONDS, VideoBridge

ROOT = Path(__file__).resolve().parents[1]
SAMPLE = ROOT / "build/lvgl-api-probe/examples/libs/ffmpeg/birds.mp4"


class VideoBridgeTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        if shutil.which("ffmpeg") is None or not SAMPLE.is_file():
            self.skipTest("ffmpeg sample is unavailable")

    async def test_local_video_emits_exact_rgb565_fragments_at_bounded_rate(self) -> None:
        binary: list[tuple[float, bytes]] = []
        states: list[dict] = []

        async def send_binary(payload: bytes) -> None:
            binary.append((time.monotonic(), payload))

        async def send_control(message: dict) -> None:
            states.append(message)

        bridge = VideoBridge(
            local_video_source=SAMPLE,
            desired_fps=3,
            fragment_pacing_s=0.01,
        )
        subscription = await bridge.register("board-1", send_binary, send_control)
        try:
            result = await bridge.command("play_video", {}, subscription=subscription)
            async with asyncio.timeout(4):
                while len(binary) < 12:
                    await asyncio.sleep(0.02)
        finally:
            await bridge.aclose()

        frames = [AudioFrame.decode(payload) for _, payload in binary[:12]]
        sequences = [frame.sequence for frame in frames]
        self.assertTrue(result["local_fallback"])
        self.assertTrue(all(frame.kind is AudioKind.VIDEO_RGB565 for frame in frames))
        self.assertEqual(sequences[:6], [0] * 6)
        self.assertEqual(sequences[6:12], [1] * 6)
        self.assertEqual(sum(len(frame.payload) for frame in frames[:6]), VIDEO_FRAME_BYTES)
        self.assertTrue(
            all(binary[index + 1][0] - binary[index][0] >= 0.005 for index in range(5))
        )
        self.assertGreaterEqual(binary[6][0] - binary[0][0], 0.20)
        self.assertTrue(any(item["payload"]["status"] == "playing" for item in states))

    async def test_same_device_reconnect_resumes_active_source(self) -> None:
        first_binary: list[bytes] = []
        resumed_binary: list[bytes] = []
        resumed_states: list[dict] = []

        async def send_binary(payload: bytes) -> None:
            first_binary.append(payload)

        async def send_control(_message: dict) -> None:
            pass

        bridge = VideoBridge(local_video_source=SAMPLE)
        subscription = await bridge.register("board-1", send_binary, send_control)
        await bridge.command("play_video", {}, subscription=subscription)
        async with asyncio.timeout(3):
            while len(first_binary) < 6:
                await asyncio.sleep(0.02)
        await bridge.unregister(subscription)
        count = len(first_binary)
        await asyncio.sleep(0.4)
        self.assertEqual(len(first_binary), count)

        async def resumed_send_binary(payload: bytes) -> None:
            resumed_binary.append(payload)

        async def resumed_send_control(message: dict) -> None:
            resumed_states.append(message)

        resumed = await bridge.register(
            "board-1", resumed_send_binary, resumed_send_control
        )
        await bridge.restore(resumed)
        async with asyncio.timeout(3):
            while len(resumed_binary) < 6:
                await asyncio.sleep(0.02)
        self.assertTrue(
            any(item["payload"]["mode"] == "video" for item in resumed_states)
        )
        await bridge.aclose()

    async def test_pause_stops_fragments_and_resume_continues_stream(self) -> None:
        binary: list[bytes] = []

        async def send_binary(payload: bytes) -> None:
            binary.append(payload)

        async def send_control(_message: dict) -> None:
            pass

        bridge = VideoBridge(local_video_source=SAMPLE, desired_fps=3)
        subscription = await bridge.register("board-1", send_binary, send_control)
        try:
            await bridge.command("play_video", {}, subscription=subscription)
            async with asyncio.timeout(3):
                while len(binary) < 6:
                    await asyncio.sleep(0.02)
            await bridge.pause(subscription)
            # One fragment may already be inside websocket.send when pause wins
            # the event-loop race; no complete second frame may pass.
            paused_count = len(binary)
            await asyncio.sleep(0.6)
            self.assertLessEqual(len(binary), paused_count + 1)
            await bridge.resume(subscription)
            async with asyncio.timeout(3):
                while len(binary) < paused_count + 6:
                    await asyncio.sleep(0.02)
        finally:
            await bridge.aclose()

    async def test_monitor_resubscribes_after_mqtt_heartbeat_failure(self) -> None:
        class FlakyMqtt:
            def __init__(self) -> None:
                self.subscribe_calls = 0
                self.publish_calls = 0
                self.callback = None

            async def subscribe_raw(self, _topic, callback, *, qos):
                self.subscribe_calls += 1
                self.callback = callback

            async def unsubscribe_raw(self, _topic, _callback):
                return None

            async def publish_raw(self, _topic, _payload, *, qos, retain):
                self.publish_calls += 1
                assert qos == 0
                if self.publish_calls == 1:
                    raise TimeoutError("simulated broker reconnect")
                assert self.callback is not None
                self.callback("camera/frame", b"jpeg")

        mqtt = FlakyMqtt()
        binary: list[bytes] = []

        async def send_binary(payload: bytes) -> None:
            binary.append(payload)

        async def send_control(_message: dict) -> None:
            pass

        bridge = VideoBridge(mqtt=mqtt, local_video_source=SAMPLE)
        bridge._jpeg_to_rgb565 = lambda _jpeg: asyncio.sleep(0, result=b"\0" * VIDEO_FRAME_BYTES)
        subscription = await bridge.register("board-1", send_binary, send_control)
        try:
            with patch(
                "gateway.video_bridge.decode_gcf1",
                return_value=SimpleNamespace(jpeg=b"jpeg"),
            ):
                await bridge.command("open_monitor", {}, subscription=subscription)
                async with asyncio.timeout(4):
                    while len(binary) < 6:
                        await asyncio.sleep(0.02)
        finally:
            await bridge.aclose()

        self.assertGreaterEqual(mqtt.publish_calls, 2)
        self.assertGreaterEqual(mqtt.subscribe_calls, 2)
        self.assertEqual(len(binary), 6)
