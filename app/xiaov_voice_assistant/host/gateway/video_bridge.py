from __future__ import annotations

import asyncio
import ipaddress
import json
import logging
import shutil
import socket
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from collections.abc import Awaitable, Callable, Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from gateway.greecam_video import GreeCamFrameError, decode_gcf1
from gateway.protocol import VIDEO_FRAME_BYTES, monotonic_ms, video_frames
from gateway.tools.registry import ToolExecutionError

LOGGER = logging.getLogger(__name__)
VIDEO_FPS = 1
VIDEO_FRAGMENT_PACING_SECONDS = 0.02
MAX_VIDEO_DOWNLOAD_BYTES = 64 * 1024 * 1024
DOWNLOAD_TIMEOUT_SECONDS = 12.0
MQTT_RETRY_DELAY_SECONDS = 1.0

BinarySender = Callable[[bytes], Awaitable[None]]
ControlSender = Callable[[dict[str, Any]], Awaitable[None]]


@dataclass(frozen=True, slots=True)
class _Viewer:
    device_id: str
    send_binary: BinarySender
    send_control: ControlSender


class VideoBridge:
    """Conflated video source that sends bounded RGB565 fragments to one board."""

    def __init__(
        self,
        *,
        mqtt: Any | None = None,
        topic_prefix: str = "greecam/v1/",
        device_id: str = "gree-cam",
        local_video_source: str | Path | None = None,
        ffmpeg: str | None = None,
        desired_fps: int = VIDEO_FPS,
        fragment_pacing_s: float = VIDEO_FRAGMENT_PACING_SECONDS,
    ) -> None:
        if not 1 <= desired_fps <= 10:
            raise ValueError("video fps must be between 1 and 10")
        if not 0.0 <= fragment_pacing_s <= 0.1:
            raise ValueError("video fragment pacing must be between 0 and 100 ms")
        self.mqtt = mqtt
        self.frame_topic = f"{topic_prefix}{device_id}/stream/frame"
        self.heartbeat_topic = f"{topic_prefix}{device_id}/stream/heartbeat"
        self.local_video_source = (
            Path(local_video_source).expanduser().resolve()
            if local_video_source is not None
            else None
        )
        self.ffmpeg = ffmpeg or shutil.which("ffmpeg")
        self.desired_fps = desired_fps
        self.fragment_pacing_s = fragment_pacing_s
        self._viewers: dict[str, _Viewer] = {}
        self._viewer_ready: dict[str, asyncio.Event] = {}
        self._lock = asyncio.Lock()
        self._source_task: asyncio.Task[None] | None = None
        self._source_generation = 0
        self._latest_mqtt: bytes | None = None
        self._mqtt_ready = asyncio.Event()
        self._mqtt_subscribed = False
        self._stream_id = 0
        self._sequence = 0
        self._active_subscription: str | None = None
        self._active_device_id: str | None = None
        self._active_mode: str | None = None
        self._active_status: str | None = None
        self._temporary_source: Path | None = None

    async def start(self) -> None:
        if self.ffmpeg is None:
            LOGGER.warning("video bridge disabled: ffmpeg was not found")

    async def aclose(self) -> None:
        await self.stop()

    async def register(
        self,
        device_id: str,
        send_binary: BinarySender,
        send_control: ControlSender,
    ) -> str:
        subscription = uuid.uuid4().hex
        async with self._lock:
            viewer = _Viewer(
                device_id=device_id,
                send_binary=send_binary,
                send_control=send_control,
            )
            self._viewers[subscription] = viewer
            ready = asyncio.Event()
            restoring = self._active_device_id == device_id
            if restoring:
                # Don't send a resumed frame before session.ready. restore()
                # publishes the display state first and then releases frames.
                self._active_subscription = subscription
            else:
                ready.set()
            self._viewer_ready[subscription] = ready
        return subscription

    async def restore(self, subscription: str | None) -> None:
        """Resume a display source after the same physical device reconnects."""
        if subscription is None:
            return
        async with self._lock:
            if subscription != self._active_subscription:
                return
            viewer = self._viewers.get(subscription)
            ready = self._viewer_ready.get(subscription)
            mode = self._active_mode
            status = self._active_status
        if viewer is None or ready is None or mode is None or status is None:
            return
        await self._send_state(viewer, mode=mode, status=status)
        ready.set()

    async def unregister(self, subscription: str | None) -> None:
        if subscription is None:
            return
        ready: asyncio.Event | None = None
        async with self._lock:
            self._viewers.pop(subscription, None)
            ready = self._viewer_ready.pop(subscription, None)
            if self._active_subscription == subscription:
                # Keep the source alive. The board reconnects autonomously and
                # register() rebinds it by stable device_id. Explicit close or
                # server shutdown remains responsible for stopping the source.
                self._active_subscription = None
        if ready is not None:
            ready.set()

    async def pause(self, subscription: str | None) -> None:
        """Pause video fragments while the device is carrying a voice turn."""
        if subscription is None:
            return
        async with self._lock:
            ready = self._viewer_ready.get(subscription)
            if ready is not None:
                ready.clear()

    async def resume(self, subscription: str | None) -> None:
        if subscription is None:
            return
        async with self._lock:
            ready = self._viewer_ready.get(subscription)
            if ready is not None:
                ready.set()

    async def command(
        self,
        action: str,
        parameters: Mapping[str, Any] | None = None,
        *,
        subscription: str | None = None,
    ) -> dict[str, Any]:
        parameters = dict(parameters or {})
        if action in ("close_monitor", "stop_video"):
            await self.stop(subscription=subscription)
            return {"executed": True, "action": action, "state": "closed"}
        if action not in ("open_monitor", "play_video"):
            raise ToolExecutionError("unknown display action", code="invalid_arguments")
        viewer_id = await self._select_viewer(subscription)
        if self.ffmpeg is None:
            raise ToolExecutionError("ffmpeg is unavailable", code="video_unavailable")
        source = parameters.get("source")
        if source is not None and (not isinstance(source, str) or not source.strip()):
            raise ToolExecutionError("video source is invalid", code="invalid_arguments")
        if action == "open_monitor":
            if self.mqtt is None:
                raise ToolExecutionError(
                    "camera stream is not configured", code="video_unavailable"
                )
            await self.stop()
            await self._replace_source(viewer_id, "monitor", self._monitor_loop)
            return {"executed": True, "action": action, "state": "connecting"}

        await self.stop()
        resolved, fallback = await self._resolve_video_source(source)
        await self._replace_source(
            viewer_id, "video", lambda generation: self._video_loop(generation, resolved)
        )
        return {
            "executed": True,
            "action": action,
            "state": "playing",
            "source": str(resolved),
            "local_fallback": fallback,
        }

    async def stop(self, *, subscription: str | None = None) -> None:
        task: asyncio.Task[None] | None
        viewer: _Viewer | None = None
        async with self._lock:
            if (
                subscription is not None
                and self._active_subscription is not None
                and subscription != self._active_subscription
            ):
                return
            task = self._source_task
            self._source_task = None
            self._source_generation += 1
            if self._active_subscription is not None:
                viewer = self._viewers.get(self._active_subscription)
            self._active_subscription = None
            self._active_device_id = None
            self._active_mode = None
            self._active_status = None
        if task is not None:
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
        await self._unsubscribe_monitor()
        self._clear_temporary_source()
        if viewer is not None:
            await self._send_state(viewer, mode="closed", status="closed")

    async def _select_viewer(self, subscription: str | None) -> str:
        async with self._lock:
            if subscription is not None:
                if subscription not in self._viewers:
                    raise ToolExecutionError(
                        "device display is disconnected", code="video_unavailable"
                    )
                return subscription
            device_ids = {viewer.device_id for viewer in self._viewers.values()}
            if not self._viewers:
                raise ToolExecutionError(
                    "no device display is connected", code="video_unavailable"
                )
            if len(device_ids) != 1:
                raise ToolExecutionError(
                    "display command is ambiguous across devices",
                    code="video_unavailable",
                )
            return next(reversed(self._viewers))

    async def _replace_source(
        self,
        subscription: str,
        mode: str,
        factory: Callable[[int], Awaitable[None]],
    ) -> None:
        async with self._lock:
            viewer = self._viewers.get(subscription)
            if viewer is None:
                raise ToolExecutionError(
                    "device display is disconnected", code="video_unavailable"
                )
            self._source_generation += 1
            generation = self._source_generation
            self._stream_id = (self._stream_id + 1) & 0xFFFFFFFF
            self._sequence = 0
            self._active_subscription = subscription
            self._active_device_id = viewer.device_id
            self._active_mode = mode
            self._active_status = "connecting"
            self._source_task = asyncio.create_task(factory(generation))
        await self._send_state(viewer, mode=mode, status="connecting")

    def _on_mqtt_frame(self, _topic: str, payload: bytes) -> None:
        self._latest_mqtt = payload
        self._mqtt_ready.set()

    async def _monitor_loop(self, generation: int) -> None:
        assert self.mqtt is not None
        heartbeat_at = 0.0
        first = True
        try:
            while self._source_generation == generation:
                if not self._mqtt_subscribed:
                    try:
                        await self.mqtt.subscribe_raw(
                            self.frame_topic, self._on_mqtt_frame, qos=0
                        )
                        self._mqtt_subscribed = True
                        heartbeat_at = 0.0
                    except asyncio.CancelledError:
                        raise
                    except Exception as exc:
                        LOGGER.warning(
                            "camera MQTT subscribe failed; retrying: %s",
                            type(exc).__name__,
                        )
                        await asyncio.sleep(MQTT_RETRY_DELAY_SECONDS)
                        continue
                now = time.monotonic()
                if now >= heartbeat_at:
                    heartbeat = json.dumps(
                        {
                            "session_id": f"xiaov-{self._stream_id:08x}",
                            "desired_fps": self.desired_fps,
                            "measured_latency_ms": 0,
                            "decoded_fps": float(self.desired_fps),
                        },
                        separators=(",", ":"),
                    )
                    try:
                        # This is a best-effort camera keepalive, not a device
                        # command. QoS1 routes through gmqtt's persistent ACK
                        # storage; during broker reconnect that storage can race
                        # resend cleanup and stall the video loop.
                        await self.mqtt.publish_raw(
                            self.heartbeat_topic, heartbeat, qos=0, retain=False
                        )
                    except asyncio.CancelledError:
                        raise
                    except Exception as exc:
                        # gmqtt clears its confirmed-topic set on disconnect;
                        # forcing a fresh subscribe on the next pass restores
                        # camera frames after the broker reconnects.
                        self._mqtt_subscribed = False
                        self._latest_mqtt = None
                        heartbeat_at = 0.0
                        LOGGER.warning(
                            "camera MQTT heartbeat failed; retrying: %s",
                            type(exc).__name__,
                        )
                        await asyncio.sleep(MQTT_RETRY_DELAY_SECONDS)
                        continue
                    heartbeat_at = now + 5.0
                timeout = max(0.05, heartbeat_at - time.monotonic())
                try:
                    async with asyncio.timeout(timeout):
                        await self._mqtt_ready.wait()
                except TimeoutError:
                    continue
                self._mqtt_ready.clear()
                payload = self._latest_mqtt
                self._latest_mqtt = None
                if payload is None:
                    continue
                try:
                    jpeg = decode_gcf1(payload).jpeg
                    rgb565 = await self._jpeg_to_rgb565(jpeg)
                except (GreeCamFrameError, RuntimeError):
                    LOGGER.warning("dropped malformed camera frame", exc_info=True)
                    continue
                if first:
                    await self._state_for_active("monitor", "streaming")
                    first = False
                await self._send_frame(rgb565, generation)
        except asyncio.CancelledError:
            raise
        except asyncio.CancelledError:
            raise
        except Exception:
            LOGGER.exception("camera monitor stopped")
            await self._state_for_active("monitor", "error")
        finally:
            await self._unsubscribe_monitor()

    async def _unsubscribe_monitor(self) -> None:
        if self.mqtt is not None and self._mqtt_subscribed:
            self._mqtt_subscribed = False
            try:
                await self.mqtt.unsubscribe_raw(self.frame_topic, self._on_mqtt_frame)
            except Exception:
                LOGGER.exception("camera frame unsubscribe failed")

    async def _video_loop(self, generation: int, source: Path) -> None:
        process = await asyncio.create_subprocess_exec(
            self.ffmpeg,
            "-v",
            "error",
            "-re",
            "-stream_loop",
            "-1",
            "-i",
            str(source),
            "-an",
            "-vf",
            _video_filter(self.desired_fps),
            "-pix_fmt",
            "rgb565le",
            "-f",
            "rawvideo",
            "pipe:1",
            stdout=asyncio.subprocess.PIPE,
            # Nothing consumes stderr.  Keeping it as PIPE leaves a Proactor
            # transport alive after ffmpeg exits and Windows reports an
            # unclosed transport when the gateway event loop shuts down.
            stderr=asyncio.subprocess.DEVNULL,
        )
        assert process.stdout is not None
        try:
            await self._state_for_active("video", "playing")
            while self._source_generation == generation:
                try:
                    frame = await process.stdout.readexactly(VIDEO_FRAME_BYTES)
                except asyncio.IncompleteReadError:
                    break
                await self._send_frame(frame, generation)
        except asyncio.CancelledError:
            raise
        finally:
            if process.returncode is None:
                process.kill()
            # Cancellation can now land during inter-fragment pacing, when no
            # readexactly() is pending.  On Windows, wait() alone may deadlock
            # while the rawvideo PIPE still contains unread bytes; communicate
            # drains that bounded remainder while reaping the killed process.
            await process.communicate()

    async def _jpeg_to_rgb565(self, jpeg: bytes) -> bytes:
        process = await asyncio.create_subprocess_exec(
            self.ffmpeg,
            "-v",
            "error",
            "-f",
            "image2pipe",
            "-c:v",
            "mjpeg",
            "-i",
            "pipe:0",
            "-frames:v",
            "1",
            "-vf",
            _video_filter(None),
            "-pix_fmt",
            "rgb565le",
            "-f",
            "rawvideo",
            "pipe:1",
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, stderr = await process.communicate(jpeg)
        if process.returncode != 0 or len(stdout) != VIDEO_FRAME_BYTES:
            raise RuntimeError(f"ffmpeg JPEG conversion failed ({stderr[:160]!r})")
        return stdout

    async def _send_frame(self, payload: bytes, generation: int) -> None:
        async with self._lock:
            if generation != self._source_generation:
                return
            subscription = self._active_subscription
            viewer = self._viewers.get(subscription or "")
            stream_id = self._stream_id
            sequence = self._sequence
            self._sequence = (self._sequence + 1) & 0xFFFFFFFF
        if viewer is None:
            return
        fragments = video_frames(
            payload,
            stream_id=stream_id,
            sequence=sequence,
            timestamp_ms=monotonic_ms() & 0xFFFFFFFF,
        )
        for index, frame in enumerate(fragments):
            async with self._lock:
                if generation != self._source_generation:
                    return
                ready = self._viewer_ready.get(subscription or "")
            if ready is None:
                return
            await ready.wait()
            async with self._lock:
                if (
                    generation != self._source_generation
                    or subscription != self._active_subscription
                    or self._viewers.get(subscription or "") is not viewer
                ):
                    return
            try:
                await viewer.send_binary(frame.encode())
            except Exception:
                # The source belongs to the device, not this socket. Leave it
                # running so the board's automatic reconnect can resume it.
                LOGGER.info("display transport disconnected; awaiting device reconnect")
                async with self._lock:
                    if self._active_subscription == subscription:
                        self._active_subscription = None
                return
            if index + 1 < len(fragments) and self.fragment_pacing_s:
                # websocket.send() can return after queueing into the host TCP
                # transport, so six 25.6 KiB fragments otherwise arrive as a
                # single 153.6 KiB burst.  The R528 libwebsockets client
                # repeatedly reset such streams despite a modest average rate.
                await asyncio.sleep(self.fragment_pacing_s)

    async def _state_for_active(self, mode: str, status: str) -> None:
        async with self._lock:
            self._active_mode = mode
            self._active_status = status
            subscription = self._active_subscription
            viewer = self._viewers.get(subscription or "")
        if viewer is not None:
            try:
                await self._send_state(viewer, mode=mode, status=status)
            except Exception:
                LOGGER.info("display state transport disconnected; awaiting reconnect")
                async with self._lock:
                    if self._active_subscription == subscription:
                        self._active_subscription = None

    async def _send_state(self, viewer: _Viewer, *, mode: str, status: str) -> None:
        from gateway.protocol import event

        await viewer.send_control(
            event(
                "display.state",
                payload={
                    "mode": mode,
                    "status": status,
                    "width": 320,
                    "height": 240,
                    "fps": self.desired_fps,
                },
            )
        )

    async def _resolve_video_source(self, source: object) -> tuple[Path, bool]:
        if source is None:
            return self._local_source(), True
        assert isinstance(source, str)
        parsed = urllib.parse.urlsplit(source.strip())
        if parsed.scheme:
            try:
                downloaded = await asyncio.to_thread(_download_public_https, source.strip())
            except (OSError, ValueError, urllib.error.URLError) as exc:
                LOGGER.warning("online video unavailable; using local fallback: %s", type(exc).__name__)
                return self._local_source(), True
            self._temporary_source = downloaded
            return downloaded, False
        path = Path(source).expanduser().resolve()
        if not path.is_file():
            return self._local_source(), True
        return path, False

    def _local_source(self) -> Path:
        if self.local_video_source is None or not self.local_video_source.is_file():
            raise ToolExecutionError(
                "local fallback video is unavailable", code="video_unavailable"
            )
        return self.local_video_source

    def _clear_temporary_source(self) -> None:
        path = self._temporary_source
        self._temporary_source = None
        if path is not None:
            try:
                path.unlink(missing_ok=True)
            except OSError:
                LOGGER.warning("failed to remove temporary video", exc_info=True)


def _video_filter(fps: int | None) -> str:
    stages = [] if fps is None else [f"fps={fps}"]
    stages.extend(
        (
            "scale=320:240:force_original_aspect_ratio=decrease",
            "pad=320:240:(ow-iw)/2:(oh-ih)/2:black",
        )
    )
    return ",".join(stages)


def _download_public_https(url: str) -> Path:
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme.lower() != "https" or not parsed.hostname or parsed.username:
        raise ValueError("online video must be an HTTPS URL without user info")
    addresses = socket.getaddrinfo(parsed.hostname, parsed.port or 443, type=socket.SOCK_STREAM)
    if not addresses or any(
        not ipaddress.ip_address(item[4][0]).is_global for item in addresses
    ):
        raise ValueError("online video host does not resolve exclusively to public IPs")

    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *_args: Any, **_kwargs: Any) -> None:
            return None

    request = urllib.request.Request(url, headers={"User-Agent": "xiaov-video/1"})
    opener = urllib.request.build_opener(NoRedirect)
    path: Path | None = None
    try:
        with opener.open(request, timeout=DOWNLOAD_TIMEOUT_SECONDS) as response:
            length = response.headers.get("Content-Length")
            if length is not None and int(length) > MAX_VIDEO_DOWNLOAD_BYTES:
                raise ValueError("online video is too large")
            with tempfile.NamedTemporaryFile(
                prefix="xiaov-video-", suffix=".media", delete=False
            ) as output:
                path = Path(output.name)
                total = 0
                while True:
                    chunk = response.read(64 * 1024)
                    if not chunk:
                        break
                    total += len(chunk)
                    if total > MAX_VIDEO_DOWNLOAD_BYTES:
                        raise ValueError("online video is too large")
                    output.write(chunk)
                if total == 0:
                    raise ValueError("online video is empty")
        return path
    except Exception:
        if path is not None:
            path.unlink(missing_ok=True)
        raise


__all__ = ["VideoBridge", "VIDEO_FPS"]
