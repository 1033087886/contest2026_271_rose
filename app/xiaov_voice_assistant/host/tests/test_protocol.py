import json
import unittest

from gateway.protocol import (
    AUDIO_HEADER_SIZE,
    AudioFrame,
    AudioKind,
    ProtocolError,
    decode_control,
    encode_control,
    event,
    VIDEO_FRAGMENT_COUNT,
    VIDEO_FRAME_BYTES,
    decode_video_fragment_flags,
    video_frames,
)


class ControlProtocolTests(unittest.TestCase):
    def test_unicode_control_round_trip(self) -> None:
        original = event(
            "asr.final",
            turn_id="turn-1",
            payload={"text": "你好，openvela"},
            event_id="event-1",
            ts_ms=123,
        )
        self.assertEqual(decode_control(encode_control(original)), original)

    def test_rejects_invalid_version(self) -> None:
        raw = json.dumps(
            {"v": 2, "type": "ping", "id": "1", "ts_ms": 0, "payload": {}}
        )
        with self.assertRaisesRegex(ProtocolError, "version"):
            decode_control(raw)

    def test_rejects_boolean_timestamp(self) -> None:
        raw = json.dumps(
            {"v": 1, "type": "ping", "id": "1", "ts_ms": True, "payload": {}}
        )
        with self.assertRaisesRegex(ProtocolError, "ts_ms"):
            decode_control(raw)


class AudioProtocolTests(unittest.TestCase):
    def test_music_frame_round_trip(self) -> None:
        frame = AudioFrame(
            kind=AudioKind.MUSIC,
            stream_id=9,
            sequence=3,
            timestamp_ms=60,
            payload=b"\x01\x00" * 320,
        )
        self.assertEqual(AudioFrame.decode(frame.encode()), frame)

    def test_audio_frame_round_trip_and_network_order(self) -> None:
        frame = AudioFrame(
            kind=AudioKind.MICROPHONE,
            stream_id=0x01020304,
            sequence=7,
            timestamp_ms=40,
            payload=b"\x01\x02\x03\x04",
            flags=1,
        )
        encoded = frame.encode()
        self.assertEqual(len(encoded), AUDIO_HEADER_SIZE + 4)
        self.assertEqual(encoded[:8], b"XVAF\x01\x01\x00\x01")
        self.assertEqual(encoded[8:12], b"\x01\x02\x03\x04")
        self.assertEqual(AudioFrame.decode(encoded), frame)

    def test_rejects_truncated_payload(self) -> None:
        encoded = AudioFrame(
            kind=AudioKind.TTS,
            stream_id=2,
            sequence=0,
            timestamp_ms=0,
            payload=b"1234",
        ).encode()
        with self.assertRaisesRegex(ProtocolError, "length"):
            AudioFrame.decode(encoded[:-1])

    def test_rejects_unknown_kind(self) -> None:
        encoded = bytearray(
            AudioFrame(
                kind=AudioKind.TTS,
                stream_id=2,
                sequence=0,
                timestamp_ms=0,
                payload=b"",
            ).encode()
        )
        encoded[5] = 99
        with self.assertRaisesRegex(ProtocolError, "kind"):
            AudioFrame.decode(bytes(encoded))

    def test_rgb565_video_frame_has_six_bounded_fragments(self) -> None:
        payload = bytes((index & 0xFF) for index in range(VIDEO_FRAME_BYTES))
        fragments = video_frames(
            payload, stream_id=9, sequence=12, timestamp_ms=34
        )

        self.assertEqual(len(fragments), VIDEO_FRAGMENT_COUNT)
        self.assertEqual(b"".join(frame.payload for frame in fragments), payload)
        for index, frame in enumerate(fragments):
            self.assertEqual(frame.kind, AudioKind.VIDEO_RGB565)
            self.assertEqual(decode_video_fragment_flags(frame.flags), (index, 6))
            self.assertEqual(AudioFrame.decode(frame.encode()), frame)

    def test_rejects_malformed_video_fragment_layout(self) -> None:
        with self.assertRaisesRegex(ProtocolError, "wrong size"):
            AudioFrame(
                AudioKind.VIDEO_RGB565,
                stream_id=1,
                sequence=1,
                timestamp_ms=1,
                payload=b"short",
                flags=(6 << 8),
            ).encode()


if __name__ == "__main__":
    unittest.main()
