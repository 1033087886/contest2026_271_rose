import asyncio
import unittest

from gateway.tools.device_media import DeviceMediaController
from gateway.providers.music import MusicSelection, MusicTrack


class RecordingMediaHub:
    def __init__(self) -> None:
        self.calls = []

    async def media_command(self, action, parameters, *, timeout_seconds):
        self.calls.append((action, dict(parameters), timeout_seconds))
        return {"ok": True}


class StreamingMediaHub(RecordingMediaHub):
    def __init__(self) -> None:
        super().__init__()
        self.streams = []

    async def media_command(self, action, parameters, *, timeout_seconds):
        self.calls.append((action, dict(parameters), timeout_seconds))
        return {"ok": True, "command_id": "a" * 32}

    async def wait_media_ready(self, command_id, *, timeout_seconds):
        self.ready_command_id = command_id
        return {"ok": True}

    async def stream_music(self, command_id, stream_id, pcm):
        self.streams.append((command_id, stream_id, pcm))


class StaticMusicLibrary:
    async def resolve(self, query):
        return MusicSelection(
            MusicTrack("Song", "Artist", "https://cdn.example.test/song", 60),
            b"\x01\x00" * 320,
        )


class BlockingStreamingHub(StreamingMediaHub):
    def __init__(self) -> None:
        super().__init__()
        self.started = asyncio.Event()
        self.release = asyncio.Event()

    async def stream_music(self, command_id, stream_id, pcm):
        self.started.set()
        await self.release.wait()


class DeviceMediaControllerTests(unittest.IsolatedAsyncioTestCase):
    async def test_natural_language_play_query_uses_explicit_default_fallback(self) -> None:
        hub = RecordingMediaHub()
        controller = DeviceMediaController(hub, timeout_seconds=1.5)

        result = await controller.command("play", {"query": "play some light music"})

        self.assertEqual(
            hub.calls,
            [("play", {"query": "/data/xiaov-demo.wav"}, 1.5)],
        )
        self.assertEqual(result["requested_query"], "play some light music")
        self.assertEqual(result["resolved_source"], "/data/xiaov-demo.wav")
        self.assertTrue(result["default_fallback"])

    async def test_absolute_paths_are_forwarded_unchanged(self) -> None:
        for source in ("/data/music/song.wav", "/mnt/sdcard/song.wav"):
            with self.subTest(source=source):
                hub = RecordingMediaHub()
                result = await DeviceMediaController(hub).command(
                    "play", {"query": source}
                )

                self.assertEqual(hub.calls[0][1]["query"], source)
                self.assertEqual(result["requested_query"], source)
                self.assertEqual(result["resolved_source"], source)
                self.assertFalse(result["default_fallback"])

    async def test_windows_path_uses_board_default_source(self) -> None:
        hub = RecordingMediaHub()
        source = r"C:\media\song.wav"

        result = await DeviceMediaController(hub).command(
            "play", {"query": source}
        )

        self.assertEqual(hub.calls[0][1]["query"], "/data/xiaov-demo.wav")
        self.assertEqual(result["requested_query"], source)
        self.assertEqual(result["resolved_source"], "/data/xiaov-demo.wav")
        self.assertTrue(result["default_fallback"])

    async def test_http_urls_use_local_fallback_without_board_decoder(self) -> None:
        for source in (
            "http://media.example.test/song.wav",
            "https://media.example.test/live?id=7",
        ):
            with self.subTest(source=source):
                hub = RecordingMediaHub()
                result = await DeviceMediaController(hub).command(
                    "play", {"query": source}
                )

                self.assertEqual(hub.calls[0][1]["query"], "/data/xiaov-demo.wav")
                self.assertEqual(result["resolved_source"], "/data/xiaov-demo.wav")
                self.assertTrue(result["default_fallback"])

    async def test_network_music_waits_for_ready_then_streams_pcm(self) -> None:
        hub = StreamingMediaHub()
        controller = DeviceMediaController(hub, music_library=StaticMusicLibrary())

        result = await controller.command("play", {"query": "播放一首歌"})
        task = controller._music_task
        self.assertIsNotNone(task)
        assert task is not None
        await task

        self.assertTrue(result["network_music"])
        self.assertEqual(result["duration_seconds"], 1)
        self.assertEqual(hub.calls[0][1]["query"][:9], "stream://")
        self.assertEqual(hub.ready_command_id, "a" * 32)
        self.assertEqual(len(hub.streams), 1)
        self.assertEqual(hub.streams[0][0], "a" * 32)
        self.assertEqual(hub.streams[0][2], b"\x01\x00" * 320)

    async def test_second_play_cancels_previous_network_stream(self) -> None:
        hub = BlockingStreamingHub()
        controller = DeviceMediaController(hub, music_library=StaticMusicLibrary())

        await controller.command("play", {"query": "第一首"})
        first_task = controller._music_task
        self.assertIsNotNone(first_task)
        await hub.started.wait()

        await controller.command("play", {"query": "第二首"})

        assert first_task is not None
        self.assertTrue(first_task.cancelled())
        self.assertIsNot(controller._music_task, first_task)
        await controller.aclose()


if __name__ == "__main__":
    unittest.main()
