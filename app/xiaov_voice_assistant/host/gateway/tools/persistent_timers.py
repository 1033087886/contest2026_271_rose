"""Atomic persistent timer storage with asynchronous reminder delivery."""

from __future__ import annotations

import asyncio
import inspect
import json
import math
import os
import re
import tempfile
import time
import uuid
from collections.abc import Awaitable, Callable, Iterable, Mapping
from contextlib import suppress
from pathlib import Path
from typing import Any

from gateway.tools.registry import ToolExecutionError

ReminderCallback = Callable[[Mapping[str, Any]], Awaitable[None]]

STATE_VERSION = 1
DEFAULT_MAX_ACTIVE_TIMERS = 256
DEFAULT_MAX_STATE_BYTES = 512 * 1024
DEFAULT_CALLBACK_TIMEOUT_SECONDS = 15.0
DEFAULT_RETRY_DELAY_SECONDS = 30.0
MAX_SCHEDULER_SLEEP_SECONDS = 30.0
MAX_DURATION_SECONDS = 604_800
MAX_LABEL_CHARS = 100
MAX_LABEL_BYTES = 400

_TIMER_ID_RE = re.compile(r"timer-([0-9]{4,})\Z")
_KINDS = frozenset(("timer", "pomodoro"))
_STATE_KEYS = frozenset(("version", "next_id", "timers"))
_RECORD_KEYS = frozenset(
    (
        "timer_id",
        "duration_seconds",
        "label",
        "kind",
        "created_at_epoch",
        "deadline_epoch",
        "delivery_attempts",
        "next_attempt_epoch",
    )
)


class TimerPersistenceError(RuntimeError):
    """A sanitized persistent-store failure safe for application logs."""


class _CorruptTimerState(ValueError):
    pass


class PersistentTimerStore:
    """Persist active timers and deliver reminders with at-least-once semantics.

    The state file is replaced atomically after every mutation. A malformed or
    oversized file is moved aside with a ``.corrupt-*`` suffix and an empty
    store is started, so one damaged write cannot permanently prevent startup.

    Callbacks run one at a time, have a fixed timeout, and are retried after a
    bounded delay. A process crash after a callback but before the successful
    state-file replacement can cause that reminder to be delivered again.
    """

    def __init__(
        self,
        state_path: str | os.PathLike[str],
        *,
        on_expire: ReminderCallback,
        clock: Callable[[], float] = time.time,
        max_active_timers: int = DEFAULT_MAX_ACTIVE_TIMERS,
        max_state_bytes: int = DEFAULT_MAX_STATE_BYTES,
        callback_timeout_seconds: float = DEFAULT_CALLBACK_TIMEOUT_SECONDS,
        retry_delay_seconds: float = DEFAULT_RETRY_DELAY_SECONDS,
    ) -> None:
        if os.fspath(state_path) == "":
            raise ValueError("timer state path must not be empty")
        path = Path(state_path)
        if not callable(on_expire):
            raise ValueError("timer reminder callback must be callable")
        if not callable(clock):
            raise ValueError("timer clock must be callable")
        if not 1 <= max_active_timers <= 10_000:
            raise ValueError("max_active_timers must be between 1 and 10000")
        if not 512 <= max_state_bytes <= 16 * 1024 * 1024:
            raise ValueError("max_state_bytes must be between 512 and 16777216")
        if callback_timeout_seconds <= 0 or not math.isfinite(
            callback_timeout_seconds
        ):
            raise ValueError("callback timeout must be positive and finite")
        if not 0.1 <= retry_delay_seconds <= 86_400 or not math.isfinite(
            retry_delay_seconds
        ):
            raise ValueError("retry delay must be between 0.1 and 86400 seconds")

        self.state_path = path
        self._on_expire = on_expire
        self._clock = clock
        self.max_active_timers = max_active_timers
        self.max_state_bytes = max_state_bytes
        self.callback_timeout_seconds = callback_timeout_seconds
        self.retry_delay_seconds = retry_delay_seconds

        self._next_id = 1
        self._timers: dict[str, dict[str, Any]] = {}
        self._lock = asyncio.Lock()
        self._changed = asyncio.Event()
        self._scheduler_task: asyncio.Task[None] | None = None
        self._delivery_tasks: dict[str, asyncio.Task[None]] = {}
        self._started = False
        self._closed = False
        self.recovered_corrupt_path: Path | None = None

    async def __aenter__(self) -> "PersistentTimerStore":
        await self.start()
        return self

    async def __aexit__(self, *_exc_info: object) -> None:
        await self.aclose()

    async def start(self) -> None:
        """Load persisted timers and start the expiry scheduler."""
        async with self._lock:
            if self._closed:
                raise TimerPersistenceError("timer store is closed")
            if self._started:
                return
            try:
                state, recovered_path = await asyncio.to_thread(
                    _load_or_recover,
                    self.state_path,
                    self.max_state_bytes,
                    self.max_active_timers,
                )
            except Exception as exc:
                raise TimerPersistenceError("timer state could not be loaded") from exc
            self._next_id = state["next_id"]
            self._timers = {
                item["timer_id"]: item for item in state["timers"]
            }
            self.recovered_corrupt_path = recovered_path
            # Validate storage permissions at startup and immediately replace a
            # quarantined/missing file with a canonical empty or recovered state.
            await self._persist_locked()
            self._started = True
            self._scheduler_task = asyncio.create_task(
                self._scheduler_loop(), name="xiaov-persistent-timers"
            )
            self._changed.set()

    async def aclose(self) -> None:
        """Stop scheduling and cancel an in-progress reminder callback."""
        async with self._lock:
            if self._closed:
                return
            self._closed = True
            scheduler = self._scheduler_task
            deliveries = list(self._delivery_tasks.values())
            self._changed.set()

        for task in deliveries:
            task.cancel()
        if scheduler is not None:
            scheduler.cancel()
        for task in deliveries:
            await _discard_task_result(task)
        if scheduler is not None:
            with suppress(asyncio.CancelledError):
                await scheduler

    def notify_clock_changed(self) -> None:
        """Wake the scheduler after an externally observed wall-clock change."""
        self._changed.set()

    async def create(
        self, *, duration_seconds: int, label: str, kind: str
    ) -> Mapping[str, Any]:
        duration, clean_label, clean_kind = _validate_create(
            duration_seconds=duration_seconds, label=label, kind=kind
        )
        await self._ensure_started()
        now = self._now()

        async with self._lock:
            self._ensure_open()
            if len(self._timers) >= self.max_active_timers:
                raise ToolExecutionError(
                    "active timer limit was reached", code="timer_limit_reached"
                )
            previous_next_id = self._next_id
            timer_id = f"timer-{self._next_id:04d}"
            self._next_id += 1
            record = {
                "timer_id": timer_id,
                "duration_seconds": duration,
                "label": clean_label,
                "kind": clean_kind,
                "created_at_epoch": now,
                "deadline_epoch": now + duration,
                "delivery_attempts": 0,
                "next_attempt_epoch": now + duration,
            }
            self._timers[timer_id] = record
            try:
                await self._persist_locked()
            except Exception:
                self._timers.pop(timer_id, None)
                self._next_id = previous_next_id
                raise
            self._changed.set()
            return self._public_record(record, now=now)

    async def cancel(self, timer_id: str) -> Mapping[str, Any]:
        clean_id = _validate_timer_id(timer_id)
        await self._ensure_started()
        async with self._lock:
            self._ensure_open()
            record = self._timers.pop(clean_id, None)
            if record is None:
                raise ToolExecutionError("timer was not found", code="not_found")
            try:
                await self._persist_locked()
            except Exception:
                self._timers[clean_id] = record
                raise
            delivery = self._delivery_tasks.get(clean_id)
            self._changed.set()

        if delivery is not None:
            delivery.cancel()
            await _discard_task_result(delivery)
        return {"timer_id": clean_id, "cancelled": True}

    async def list_active(self) -> list[Mapping[str, Any]]:
        await self._ensure_started()
        now = self._now()
        async with self._lock:
            self._ensure_open()
            records = sorted(
                self._timers.values(),
                key=lambda item: (item["deadline_epoch"], item["timer_id"]),
            )
            return [self._public_record(item, now=now) for item in records]

    async def _ensure_started(self) -> None:
        if not self._started:
            await self.start()

    def _ensure_open(self) -> None:
        if self._closed:
            raise TimerPersistenceError("timer store is closed")

    def _now(self) -> float:
        try:
            value = float(self._clock())
        except Exception as exc:
            raise TimerPersistenceError("timer clock is unavailable") from exc
        if not math.isfinite(value):
            raise TimerPersistenceError("timer clock is unavailable")
        return value

    def _public_record(
        self, record: Mapping[str, Any], *, now: float
    ) -> dict[str, Any]:
        return {
            "timer_id": record["timer_id"],
            "duration_seconds": record["duration_seconds"],
            "label": record["label"],
            "kind": record["kind"],
            "created_at_epoch": record["created_at_epoch"],
            "deadline_epoch": record["deadline_epoch"],
            "remaining_seconds": max(0, math.ceil(record["deadline_epoch"] - now)),
            "notification_delivery": "scheduled",
        }

    async def _persist_locked(self) -> None:
        try:
            encoded = _encode_state(
                next_id=self._next_id,
                timers=self._timers.values(),
                max_state_bytes=self.max_state_bytes,
            )
            write_task = asyncio.create_task(
                asyncio.to_thread(_atomic_write, self.state_path, encoded)
            )
            try:
                await asyncio.shield(write_task)
            except asyncio.CancelledError:
                # A thread cannot be cancelled once it starts. Wait for the
                # atomic replacement so memory and disk cannot diverge.
                await write_task
                raise
        except TimerPersistenceError:
            raise
        except Exception as exc:
            raise TimerPersistenceError("timer state could not be saved") from exc

    async def _scheduler_loop(self) -> None:
        try:
            while True:
                async with self._lock:
                    if self._closed:
                        return
                    self._changed.clear()
                    now = self._now()
                    candidates = [
                        item
                        for timer_id, item in self._timers.items()
                        if timer_id not in self._delivery_tasks
                    ]
                    next_record = min(
                        candidates,
                        key=lambda item: (item["next_attempt_epoch"], item["timer_id"]),
                        default=None,
                    )
                    delay = (
                        None
                        if next_record is None
                        else max(0.0, next_record["next_attempt_epoch"] - now)
                    )

                if delay is None:
                    await self._changed.wait()
                    continue
                if delay > 0:
                    try:
                        await asyncio.wait_for(
                            self._changed.wait(),
                            timeout=min(delay, MAX_SCHEDULER_SLEEP_SECONDS),
                        )
                    except TimeoutError:
                        pass
                    continue
                await self._deliver(next_record["timer_id"])
        except asyncio.CancelledError:
            raise
        except Exception:
            # A clock or filesystem failure must not turn into a tight loop.
            if not self._closed:
                await asyncio.sleep(min(self.retry_delay_seconds, 1.0))
                if not self._closed:
                    self._scheduler_task = asyncio.create_task(
                        self._scheduler_loop(), name="xiaov-persistent-timers-recovery"
                    )

    async def _deliver(self, timer_id: str) -> None:
        async with self._lock:
            record = self._timers.get(timer_id)
            if record is None or record["next_attempt_epoch"] > self._now():
                return
            callback_record = self._public_record(record, now=self._now())
            delivery = asyncio.create_task(
                self._invoke_callback(callback_record),
                name=f"xiaov-reminder-{timer_id}",
            )
            self._delivery_tasks[timer_id] = delivery

        succeeded = False
        try:
            await asyncio.wait_for(delivery, timeout=self.callback_timeout_seconds)
            succeeded = True
        except TimeoutError:
            pass
        except asyncio.CancelledError:
            if asyncio.current_task() is not None and asyncio.current_task().cancelling():
                delivery.cancel()
                with suppress(asyncio.CancelledError):
                    await delivery
                raise
            pass
        except Exception:
            pass

        async with self._lock:
            self._delivery_tasks.pop(timer_id, None)
            current = self._timers.get(timer_id)
            if current is None:
                return
            if succeeded:
                self._timers.pop(timer_id, None)
                try:
                    await self._persist_locked()
                except TimerPersistenceError:
                    # Do not immediately duplicate a reminder that already ran.
                    pass
            else:
                current["delivery_attempts"] += 1
                current["next_attempt_epoch"] = self._now() + self.retry_delay_seconds
                try:
                    await self._persist_locked()
                except TimerPersistenceError:
                    pass
            self._changed.set()

    async def _invoke_callback(self, record: Mapping[str, Any]) -> None:
        result = self._on_expire(record)
        if not inspect.isawaitable(result):
            raise TypeError("timer reminder callback must be asynchronous")
        await result


async def _discard_task_result(task: asyncio.Task[Any]) -> None:
    try:
        await task
    except asyncio.CancelledError:
        pass
    except Exception:
        pass


def _validate_create(
    *, duration_seconds: object, label: object, kind: object
) -> tuple[int, str, str]:
    if (
        isinstance(duration_seconds, bool)
        or not isinstance(duration_seconds, int)
        or not 1 <= duration_seconds <= MAX_DURATION_SECONDS
    ):
        raise ToolExecutionError(
            "timer duration must be between 1 and 604800 seconds",
            code="invalid_arguments",
        )
    if not isinstance(label, str):
        raise ToolExecutionError("timer label must be text", code="invalid_arguments")
    if any(ord(character) < 32 or ord(character) == 127 for character in label):
        raise ToolExecutionError(
            "timer label contains control characters", code="invalid_arguments"
        )
    clean_label = label.strip()
    if (
        not clean_label
        or len(clean_label) > MAX_LABEL_CHARS
        or _utf8_length(clean_label, invalid_arguments=True) > MAX_LABEL_BYTES
    ):
        raise ToolExecutionError(
            "timer label is empty or exceeds its configured limit",
            code="invalid_arguments",
        )
    if not isinstance(kind, str) or kind not in _KINDS:
        raise ToolExecutionError("timer kind is invalid", code="invalid_arguments")
    return duration_seconds, clean_label, kind


def _validate_timer_id(timer_id: object) -> str:
    if (
        not isinstance(timer_id, str)
        or len(timer_id) > 64
        or _TIMER_ID_RE.fullmatch(timer_id) is None
    ):
        raise ToolExecutionError("timer id is invalid", code="invalid_arguments")
    return timer_id


def _load_or_recover(
    path: Path, max_state_bytes: int, max_active_timers: int
) -> tuple[dict[str, Any], Path | None]:
    try:
        encoded = path.read_bytes()
    except FileNotFoundError:
        return _empty_state(), None
    if len(encoded) > max_state_bytes:
        return _recover_corrupt(path)
    try:
        payload = json.loads(
            encoded,
            parse_constant=lambda value: _reject_constant(value),
        )
        state = _validate_state(payload, max_active_timers)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, _CorruptTimerState):
        return _recover_corrupt(path)
    return state, None


def _recover_corrupt(path: Path) -> tuple[dict[str, Any], Path]:
    recovered = path.with_name(f"{path.name}.corrupt-{uuid.uuid4().hex}")
    os.replace(path, recovered)
    return _empty_state(), recovered


def _empty_state() -> dict[str, Any]:
    return {"version": STATE_VERSION, "next_id": 1, "timers": []}


def _validate_state(payload: object, max_active_timers: int) -> dict[str, Any]:
    if not isinstance(payload, dict) or set(payload) != _STATE_KEYS:
        raise _CorruptTimerState
    if type(payload.get("version")) is not int or payload["version"] != STATE_VERSION:
        raise _CorruptTimerState
    next_id = payload.get("next_id")
    timers = payload.get("timers")
    if (
        isinstance(next_id, bool)
        or not isinstance(next_id, int)
        or not 1 <= next_id <= 10**18
        or not isinstance(timers, list)
        or len(timers) > max_active_timers
    ):
        raise _CorruptTimerState

    validated: list[dict[str, Any]] = []
    ids: set[str] = set()
    largest_id = 0
    for raw in timers:
        record = _validate_record(raw)
        timer_id = record["timer_id"]
        if timer_id in ids:
            raise _CorruptTimerState
        ids.add(timer_id)
        largest_id = max(largest_id, int(_TIMER_ID_RE.fullmatch(timer_id).group(1)))
        validated.append(record)
    if next_id <= largest_id:
        raise _CorruptTimerState
    return {"version": STATE_VERSION, "next_id": next_id, "timers": validated}


def _validate_record(raw: object) -> dict[str, Any]:
    if not isinstance(raw, dict) or set(raw) != _RECORD_KEYS:
        raise _CorruptTimerState
    timer_id = raw.get("timer_id")
    duration = raw.get("duration_seconds")
    label = raw.get("label")
    kind = raw.get("kind")
    attempts = raw.get("delivery_attempts")
    if (
        not isinstance(timer_id, str)
        or len(timer_id) > 64
        or _TIMER_ID_RE.fullmatch(timer_id) is None
        or isinstance(duration, bool)
        or not isinstance(duration, int)
        or not 1 <= duration <= MAX_DURATION_SECONDS
        or not isinstance(label, str)
        or not label
        or len(label) > MAX_LABEL_CHARS
        or _utf8_length(label) > MAX_LABEL_BYTES
        or any(ord(character) < 32 or ord(character) == 127 for character in label)
        or not isinstance(kind, str)
        or kind not in _KINDS
        or isinstance(attempts, bool)
        or not isinstance(attempts, int)
        or not 0 <= attempts <= 10**9
    ):
        raise _CorruptTimerState
    created = _state_number(raw.get("created_at_epoch"))
    deadline = _state_number(raw.get("deadline_epoch"))
    next_attempt = _state_number(raw.get("next_attempt_epoch"))
    if deadline < created or next_attempt < deadline:
        raise _CorruptTimerState
    return {
        "timer_id": timer_id,
        "duration_seconds": duration,
        "label": label,
        "kind": kind,
        "created_at_epoch": created,
        "deadline_epoch": deadline,
        "delivery_attempts": attempts,
        "next_attempt_epoch": next_attempt,
    }


def _state_number(value: object) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise _CorruptTimerState
    try:
        converted = float(value)
    except (OverflowError, ValueError) as exc:
        raise _CorruptTimerState from exc
    if not math.isfinite(converted):
        raise _CorruptTimerState
    return converted


def _utf8_length(value: str, *, invalid_arguments: bool = False) -> int:
    try:
        return len(value.encode("utf-8"))
    except UnicodeEncodeError as exc:
        if invalid_arguments:
            raise ToolExecutionError(
                "timer label contains invalid text", code="invalid_arguments"
            ) from exc
        raise _CorruptTimerState from exc


def _reject_constant(_value: str) -> None:
    raise _CorruptTimerState


def _encode_state(
    *,
    next_id: int,
    timers: Iterable[Mapping[str, Any]],
    max_state_bytes: int,
) -> bytes:
    payload = {
        "version": STATE_VERSION,
        "next_id": next_id,
        "timers": sorted(timers, key=lambda item: item["timer_id"]),
    }
    encoded = json.dumps(
        payload,
        ensure_ascii=False,
        allow_nan=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    if len(encoded) > max_state_bytes:
        raise TimerPersistenceError("timer state exceeded the configured limit")
    return encoded


def _atomic_write(path: Path, encoded: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=path.parent, prefix=f".{path.name}.", suffix=".tmp"
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(encoded)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
        _fsync_directory(path.parent)
    except BaseException:
        with suppress(OSError):
            temporary.unlink()
        raise


def _fsync_directory(directory: Path) -> None:
    if os.name == "nt":
        return
    flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
    try:
        descriptor = os.open(directory, flags)
    except OSError:
        return
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


__all__ = [
    "PersistentTimerStore",
    "ReminderCallback",
    "TimerPersistenceError",
]
