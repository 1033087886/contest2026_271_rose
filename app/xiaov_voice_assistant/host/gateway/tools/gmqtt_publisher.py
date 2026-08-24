"""Bounded MQTT 3.1.1 publisher backed by the optional MIT-licensed gmqtt."""

from __future__ import annotations

import asyncio
import json
import logging
import math
import uuid
from collections.abc import Callable, Mapping
from contextlib import suppress
from typing import Any

from gateway.tools.registry import ToolExecutionError

DEFAULT_CONNECT_TIMEOUT_SECONDS = 10.0
DEFAULT_PUBLISH_TIMEOUT_SECONDS = 5.0
DEFAULT_MAX_PAYLOAD_BYTES = 2048


class _IntentionalDisconnectFilter(logging.Filter):
    def filter(self, record: logging.LogRecord) -> bool:
        return record.msg != "[EXC: CONN LOST]"


class GmqttPublisher:
    """Maintains one broker connection and waits for QoS acknowledgement."""

    def __init__(
        self,
        *,
        host: str,
        port: int = 1883,
        tls: bool = False,
        ca_file: str | None = None,
        username: str | None = None,
        password: str | None = None,
        client_id: str | None = None,
        connect_timeout_seconds: float = DEFAULT_CONNECT_TIMEOUT_SECONDS,
        publish_timeout_seconds: float = DEFAULT_PUBLISH_TIMEOUT_SECONDS,
        max_payload_bytes: int = DEFAULT_MAX_PAYLOAD_BYTES,
        client: Any = None,
        storage: Any = None,
        protocol_version: Any = None,
        ssl_context: Any = None,
    ) -> None:
        clean_host = _bounded_text(host, "MQTT host", 253)
        clean_username = _optional_text(username, "MQTT username", 256)
        clean_password = _optional_text(password, "MQTT password", 1024)
        clean_ca_file = _optional_text(ca_file, "MQTT CA file", 1024)
        clean_client_id = _bounded_text(
            client_id or f"xiaov-gateway-{uuid.uuid4().hex[:12]}",
            "MQTT client id",
            64,
        )
        if isinstance(port, bool) or not isinstance(port, int) or not 1 <= port <= 65535:
            raise ValueError("MQTT port must be between 1 and 65535")
        if clean_password is not None and clean_username is None:
            raise ValueError("MQTT password requires a username")
        if clean_ca_file is not None and not tls:
            raise ValueError("MQTT CA file requires TLS")
        if ssl_context is not None and not tls:
            raise ValueError("MQTT SSL context requires TLS")
        for name, value in (
            ("connect timeout", connect_timeout_seconds),
            ("publish timeout", publish_timeout_seconds),
        ):
            if value <= 0 or not math.isfinite(value):
                raise ValueError(f"MQTT {name} must be positive and finite")
        if not 64 <= max_payload_bytes <= 1024 * 1024:
            raise ValueError("MQTT payload limit must be between 64 and 1048576 bytes")
        if (client is None) != (storage is None):
            raise ValueError("injected MQTT client and storage must be supplied together")

        self.host = clean_host
        self.port = port
        self.tls = bool(tls)
        self.ca_file = clean_ca_file
        self.username = clean_username
        self.password = clean_password
        self.client_id = clean_client_id
        self.connect_timeout_seconds = connect_timeout_seconds
        self.publish_timeout_seconds = publish_timeout_seconds
        self.max_payload_bytes = max_payload_bytes
        self._client = client
        self._storage = storage
        self._protocol_version = protocol_version
        self._ssl_context = ssl_context
        self._lock = asyncio.Lock()
        self._closed = False
        self._subscribed_topics: set[str] = set()
        self._subscription_waiters: dict[int, tuple[str, asyncio.Future[bool]]] = {}
        self._message_waiters: dict[
            str, list[tuple[str, str, asyncio.Future[dict[str, Any]]]]
        ] = {}
        self._raw_subscribers: dict[str, set[Callable[[str, bytes], None]]] = {}
        self._callbacks_installed = False

    async def start(self) -> None:
        async with self._lock:
            self._ensure_open()
            await self._ensure_connected_locked()

    async def aclose(self) -> None:
        async with self._lock:
            if self._closed:
                return
            self._closed = True
            client = self._client
            if client is None:
                return
            stop_reconnect = getattr(client, "stop_reconnect", None)
            if stop_reconnect is not None:
                stop_reconnect()
            if getattr(client, "is_connected", False):
                protocol_logger = logging.getLogger("gmqtt.mqtt.protocol")
                disconnect_filter = _IntentionalDisconnectFilter()
                protocol_logger.addFilter(disconnect_filter)
                try:
                    with suppress(Exception):
                        await client.disconnect()
                finally:
                    protocol_logger.removeFilter(disconnect_filter)

    async def publish(
        self,
        topic: str,
        payload: Mapping[str, Any],
        *,
        qos: int,
        retain: bool,
    ) -> Mapping[str, Any]:
        clean_topic = _validate_topic(topic)
        if qos != 1:
            raise ToolExecutionError(
                "MQTT publisher requires QoS 1", code="mqtt_invalid_request"
            )
        encoded = self._encode_payload(payload)

        async with self._lock:
            self._ensure_open()
            await self._ensure_connected_locked()
            try:
                self._client.publish(
                    clean_topic,
                    encoded,
                    qos=1,
                    retain=bool(retain),
                    content_type="application/json",
                )
                # gmqtt enqueues QoS storage in a scheduled task. Yield once so
                # wait_empty observes either the queued packet or its PUBACK.
                await asyncio.sleep(0)
                async with asyncio.timeout(self.publish_timeout_seconds):
                    await self._storage.wait_empty()
            except asyncio.CancelledError:
                raise
            except TimeoutError as exc:
                raise ToolExecutionError(
                    "MQTT broker acknowledgement timed out", code="mqtt_timeout"
                ) from exc
            except ToolExecutionError:
                raise
            except Exception as exc:
                raise ToolExecutionError(
                    "MQTT publish failed", code="mqtt_unavailable"
                ) from exc
        return {
            "executed": True,
            "mode": "mqtt",
            "topic": clean_topic,
            "qos": 1,
            "retain": bool(retain),
        }

    async def publish_and_wait_json(
        self,
        topic: str,
        payload: Mapping[str, Any],
        *,
        response_topic: str,
        correlation_key: str,
        correlation_value: str,
        qos: int,
        retain: bool,
        response_timeout_seconds: float | None = None,
    ) -> Mapping[str, Any]:
        """Publish JSON and wait for a matching device response.

        MQTT QoS acknowledgement only confirms that the broker accepted the
        packet. Device protocols need a second acknowledgement after the
        command has been applied; this method provides that correlation point.
        """
        clean_topic = _validate_topic(topic)
        clean_response_topic = _validate_topic(response_topic)
        clean_key = _bounded_text(correlation_key, "MQTT correlation key", 64)
        clean_value = _bounded_text(correlation_value, "MQTT correlation value", 256)
        if qos != 1:
            raise ToolExecutionError(
                "MQTT publisher requires QoS 1", code="mqtt_invalid_request"
            )
        encoded = self._encode_payload(payload)
        timeout_seconds = (
            self.publish_timeout_seconds
            if response_timeout_seconds is None
            else response_timeout_seconds
        )
        if timeout_seconds <= 0 or not math.isfinite(timeout_seconds):
            raise ValueError("MQTT response timeout must be positive and finite")

        async with self._lock:
            self._ensure_open()
            await self._ensure_connected_locked()
            await self._ensure_subscription_locked(clean_response_topic)
            loop = asyncio.get_running_loop()
            response_future: asyncio.Future[dict[str, Any]] = loop.create_future()
            waiters = self._message_waiters.setdefault(clean_response_topic, [])
            waiter = (clean_key, clean_value, response_future)
            waiters.append(waiter)
            try:
                self._client.publish(
                    clean_topic,
                    encoded,
                    qos=1,
                    retain=bool(retain),
                    content_type="application/json",
                )
                await asyncio.sleep(0)
                async with asyncio.timeout(self.publish_timeout_seconds):
                    await self._storage.wait_empty()
                try:
                    async with asyncio.timeout(timeout_seconds):
                        device_ack = await response_future
                except TimeoutError as exc:
                    raise ToolExecutionError(
                        "device acknowledgement timed out",
                        code="mqtt_device_timeout",
                    ) from exc
            except asyncio.CancelledError:
                raise
            except ToolExecutionError:
                raise
            except TimeoutError as exc:
                raise ToolExecutionError(
                    "MQTT broker acknowledgement timed out", code="mqtt_timeout"
                ) from exc
            except Exception as exc:
                raise ToolExecutionError(
                    "MQTT publish failed", code="mqtt_unavailable"
                ) from exc
            finally:
                current = self._message_waiters.get(clean_response_topic, [])
                self._message_waiters[clean_response_topic] = [
                    item for item in current if item[2] is not response_future
                ]
                if not self._message_waiters[clean_response_topic]:
                    self._message_waiters.pop(clean_response_topic, None)

        return {
            "executed": True,
            "mode": "mqtt",
            "topic": clean_topic,
            "qos": 1,
            "retain": bool(retain),
            "device_ack": device_ack,
        }

    async def subscribe_raw(
        self, topic: str, callback: Callable[[str, bytes], None], *, qos: int = 0
    ) -> None:
        clean_topic = _validate_topic(topic)
        if qos not in (0, 1) or not callable(callback):
            raise ValueError("raw MQTT subscription requires qos 0 or 1 and a callback")
        async with self._lock:
            self._ensure_open()
            await self._ensure_connected_locked()
            callbacks = self._raw_subscribers.setdefault(clean_topic, set())
            callbacks.add(callback)
            if clean_topic not in self._subscribed_topics:
                await self._ensure_subscription_locked(clean_topic, qos=qos)

    async def unsubscribe_raw(
        self, topic: str, callback: Callable[[str, bytes], None]
    ) -> None:
        clean_topic = _validate_topic(topic)
        async with self._lock:
            callbacks = self._raw_subscribers.get(clean_topic)
            if callbacks is None:
                return
            callbacks.discard(callback)
            if callbacks:
                return
            self._raw_subscribers.pop(clean_topic, None)
            if self._client is not None and getattr(self._client, "is_connected", False):
                self._client.unsubscribe(clean_topic)
            self._subscribed_topics.discard(clean_topic)

    async def publish_raw(
        self, topic: str, payload: bytes | str, *, qos: int = 0, retain: bool = False
    ) -> None:
        clean_topic = _validate_topic(topic)
        encoded = payload.encode("utf-8") if isinstance(payload, str) else bytes(payload)
        if qos not in (0, 1) or len(encoded) > self.max_payload_bytes:
            raise ToolExecutionError("MQTT raw publish is invalid", code="mqtt_invalid_request")
        async with self._lock:
            self._ensure_open()
            await self._ensure_connected_locked()
            self._client.publish(clean_topic, encoded, qos=qos, retain=bool(retain))
            if qos == 1:
                await asyncio.sleep(0)
                async with asyncio.timeout(self.publish_timeout_seconds):
                    await self._storage.wait_empty()

    def _encode_payload(self, payload: Mapping[str, Any]) -> bytes:
        try:
            encoded = json.dumps(
                dict(payload),
                ensure_ascii=False,
                allow_nan=False,
                separators=(",", ":"),
                sort_keys=True,
            ).encode("utf-8")
        except (TypeError, ValueError, UnicodeEncodeError) as exc:
            raise ToolExecutionError(
                "MQTT payload is not valid bounded JSON", code="mqtt_invalid_request"
            ) from exc
        if len(encoded) > self.max_payload_bytes:
            raise ToolExecutionError(
                "MQTT payload exceeded the configured limit",
                code="mqtt_invalid_request",
            )
        return encoded

    def _ensure_open(self) -> None:
        if self._closed:
            raise ToolExecutionError("MQTT publisher is closed", code="mqtt_unavailable")

    def _ensure_runtime(self) -> None:
        if self._client is not None:
            return
        try:
            from gmqtt import Client
            from gmqtt.mqtt.constants import MQTTv311
            from gmqtt.storage import HeapPersistentStorage
        except ImportError as exc:
            raise ToolExecutionError(
                "MQTT support is not installed; install the mqtt extra",
                code="mqtt_unavailable",
            ) from exc
        self._storage = HeapPersistentStorage()
        self._protocol_version = MQTTv311
        self._client = Client(
            self.client_id,
            clean_session=True,
            persistent_storage=self._storage,
        )
        self._client.set_config({"reconnect_retries": 0})
        if self.username is not None:
            self._client.set_auth_credentials(self.username, self.password)
        self._install_callbacks()

    def _install_callbacks(self) -> None:
        if self._callbacks_installed or self._client is None:
            return
        self._client.on_message = self._on_message
        self._client.on_subscribe = self._on_subscribe
        self._client.on_disconnect = self._on_disconnect
        self._callbacks_installed = True

    def _connect_ssl(self) -> Any:
        if not self.tls:
            return False
        if self._ssl_context is None:
            import ssl

            self._ssl_context = ssl.create_default_context(cafile=self.ca_file)
        return self._ssl_context

    async def _ensure_connected_locked(self) -> None:
        self._ensure_runtime()
        self._install_callbacks()
        if getattr(self._client, "is_connected", False):
            return
        try:
            async with asyncio.timeout(self.connect_timeout_seconds):
                await self._client.connect(
                    self.host,
                    port=self.port,
                    ssl=self._connect_ssl(),
                    keepalive=60,
                    version=self._protocol_version,
                    raise_exc=True,
                )
        except asyncio.CancelledError:
            raise
        except TimeoutError as exc:
            raise ToolExecutionError(
                "MQTT broker connection timed out", code="mqtt_timeout"
            ) from exc
        except Exception as exc:
            raise ToolExecutionError(
                "MQTT broker is unavailable", code="mqtt_unavailable"
            ) from exc

    async def _ensure_subscription_locked(self, topic: str, *, qos: int = 1) -> None:
        if topic in self._subscribed_topics:
            return
        loop = asyncio.get_running_loop()
        future: asyncio.Future[bool] = loop.create_future()
        try:
            mid = self._client.subscribe(topic, qos=qos)
        except Exception as exc:
            raise ToolExecutionError(
                "MQTT subscription failed", code="mqtt_unavailable"
            ) from exc
        self._subscription_waiters[int(mid)] = (topic, future)
        try:
            async with asyncio.timeout(self.connect_timeout_seconds):
                accepted = await future
        except TimeoutError as exc:
            self._subscription_waiters.pop(int(mid), None)
            raise ToolExecutionError(
                "MQTT subscription acknowledgement timed out",
                code="mqtt_timeout",
            ) from exc
        if not accepted:
            raise ToolExecutionError(
                "MQTT subscription was rejected", code="mqtt_unavailable"
            )

    def _on_subscribe(self, _client: Any, mid: int, granted_qos: Any, _properties: Any) -> None:
        entry = self._subscription_waiters.pop(int(mid), None)
        if entry is None:
            return
        topic, future = entry
        accepted = bool(granted_qos) and all(int(qos) < 0x80 for qos in granted_qos)
        if accepted:
            self._subscribed_topics.add(topic)
        if not future.done():
            future.set_result(accepted)

    def _on_message(
        self, _client: Any, topic: str, payload: bytes, _qos: int, _properties: Any
    ) -> int:
        clean_topic = str(topic)
        for callback in tuple(self._raw_subscribers.get(clean_topic, ())):
            try:
                callback(clean_topic, bytes(payload))
            except Exception:
                logging.getLogger(__name__).exception("raw MQTT subscriber failed")
        waiters = self._message_waiters.get(clean_topic)
        if not waiters:
            return 0
        try:
            decoded = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return 0
        if not isinstance(decoded, dict):
            return 0
        for key, value, future in tuple(waiters):
            if not future.done() and decoded.get(key) == value:
                future.set_result(decoded)
        return 0

    def _on_disconnect(self, *_args: Any) -> None:
        self._subscribed_topics.clear()


def _bounded_text(value: object, name: str, maximum: int) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{name} must be text")
    clean = value.strip()
    if (
        not clean
        or len(clean) > maximum
        or any(ord(character) < 32 or ord(character) == 127 for character in clean)
    ):
        raise ValueError(f"{name} is invalid")
    try:
        clean.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise ValueError(f"{name} is invalid") from exc
    return clean


def _optional_text(value: object, name: str, maximum: int) -> str | None:
    if value is None:
        return None
    return _bounded_text(value, name, maximum)


def _validate_topic(value: object) -> str:
    try:
        topic = _bounded_text(value, "MQTT topic", 512)
    except ValueError as exc:
        raise ToolExecutionError("MQTT topic is invalid", code="mqtt_invalid_request") from exc
    if topic.startswith("/") or "+" in topic or "#" in topic or "\x00" in topic:
        raise ToolExecutionError("MQTT topic is invalid", code="mqtt_invalid_request")
    return topic


__all__ = ["GmqttPublisher"]
