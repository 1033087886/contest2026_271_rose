"""ChKSz music API integration for the production music pipeline.

The API key is supplied by the host launcher through ``CHKSZ_API_KEY`` and is
only sent as a query parameter to ``api.chksz.com``.  This module never puts
the key in logs, exceptions, or command-line arguments.
"""

from __future__ import annotations

import asyncio
import json
import logging
import math
import re
import time
from collections.abc import Mapping
from typing import Any

import httpx

from gateway.providers.music import (
    MUSIC_CACHE_ENTRIES,
    MUSIC_CACHE_TTL_SECONDS,
    MUSIC_FFMPEG_TIMEOUT_SECONDS,
    MUSIC_PCM_MAX_BYTES,
    MUSIC_REQUEST_TIMEOUT_SECONDS,
    MusicSearchError,
    MusicSelection,
    MusicTrack,
    DeezerMusicLibrary,
    normalize_music_query,
)

LOGGER = logging.getLogger(__name__)

DEFAULT_CHKSZ_BASE_URL = "https://api.chksz.com"
DEFAULT_CHKSZ_PLATFORM = "qq"
DEFAULT_CHKSZ_SIZE = "320k"
CHKSZ_SIZES = ("128k", "320k", "flac", "hires", "master")
CHKSZ_PLATFORMS = ("qq", "kugou", "netease", "auto")
CHKSZ_NETEASE_LEVELS = {
    "128k": "standard",
    "320k": "exhigh",
    "flac": "lossless",
    "hires": "hires",
    "master": "jymaster",
}
CHKSZ_MAX_SEARCH_RESULTS = 50
CHKSZ_MAX_RETRY_AFTER_SECONDS = 60.0
CHKSZ_ERROR_MESSAGE_LIMIT = 180
CHKSZ_DOWNLOAD_MAX_BYTES = 32 * 1024 * 1024
CHKSZ_TRANSIENT_RETRIES = 1
CHKSZ_TRANSIENT_RETRY_DELAY_SECONDS = 0.35
CHKSZ_API_TIMEOUT_SECONDS = 6.0
CHKSZ_AUTO_PREFERRED_GRACE_SECONDS = 5.0
CHKSZ_AUTO_PLATFORM_PRIORITY = {"qq": 0, "kugou": 1, "netease": 2}
_KEY_PATTERN = re.compile(r"chksz_[A-Za-z0-9_-]+")


class ChkszApiError(MusicSearchError):
    """A bounded ChKSz API failure with status and provider code metadata."""

    def __init__(
        self,
        message: str,
        *,
        status_code: int | None = None,
        provider_code: int | str | None = None,
        retry_after: float | None = None,
    ) -> None:
        super().__init__(message)
        self.status_code = status_code
        self.provider_code = provider_code
        self.retry_after = retry_after


class ChkszMusicLibrary(DeezerMusicLibrary):
    """Resolve music through ChKSz and convert the returned URL to board PCM.

    ``platform`` selects the default source.  A query beginning with
    ``QQ音乐``/``酷狗``/``网易云`` overrides it, which lets the existing LLM
    media tool honor an explicit platform without adding a second device API.
    ``auto`` tries QQ, Kugou, and NetEase only when a platform returns no
    result; authentication, quota, rate-limit, and service failures stop
    immediately.
    """

    def __init__(
        self,
        *,
        api_key: str,
        base_url: str = DEFAULT_CHKSZ_BASE_URL,
        platform: str = DEFAULT_CHKSZ_PLATFORM,
        size: str = DEFAULT_CHKSZ_SIZE,
        client: httpx.AsyncClient | None = None,
        cache_ttl_seconds: float = MUSIC_CACHE_TTL_SECONDS,
        request_timeout_seconds: float = MUSIC_REQUEST_TIMEOUT_SECONDS,
        ffmpeg_timeout_seconds: float = MUSIC_FFMPEG_TIMEOUT_SECONDS,
        cache_entries: int = MUSIC_CACHE_ENTRIES,
        ffmpeg: str | None = None,
    ) -> None:
        _validate_api_key(api_key)
        _protect_sensitive_http_logging()
        if not base_url.startswith("https://"):
            raise ValueError("ChKSz base URL must use HTTPS")
        if platform not in CHKSZ_PLATFORMS:
            raise ValueError(f"unsupported ChKSz platform: {platform}")
        if size not in CHKSZ_SIZES:
            raise ValueError(f"unsupported ChKSz size: {size}")
        if not 1 <= cache_entries <= 256:
            raise ValueError("ChKSz cache_entries must be between 1 and 256")
        super().__init__(
            search_url=base_url.rstrip("/") + "/unused",
            ffmpeg=ffmpeg,
            client=client,
            cache_ttl_seconds=cache_ttl_seconds,
            request_timeout_seconds=request_timeout_seconds,
            ffmpeg_timeout_seconds=ffmpeg_timeout_seconds,
        )
        self.api_key = api_key
        self.base_url = base_url.rstrip("/")
        self.platform = platform
        self.size = size
        self.cache_entries = cache_entries

    async def resolve(self, query: str) -> MusicSelection:
        normalized = normalize_music_query(query)
        platform, keyword = _select_platform(normalized, self.platform)
        cache_key = f"{platform}:{keyword.casefold()}"
        now = time.monotonic()
        async with self._lock:
            cached = self._cache.get(cache_key)
            if cached is not None and now - cached[0] < self.cache_ttl_seconds:
                LOGGER.info("chksz music cache hit platform=%s", platform)
                return cached[1]
            track = await self._search_platform(platform, keyword)
            audio = await self._download(track.preview_url)
            pcm = await self._decode(audio)
            selection = MusicSelection(track=track, pcm=pcm)
            self._cache[cache_key] = (time.monotonic(), selection)
            while len(self._cache) > self.cache_entries:
                oldest = min(self._cache, key=lambda key: self._cache[key][0])
                self._cache.pop(oldest, None)
            LOGGER.info(
                "chksz music resolved platform=%s track=%s duration_s=%s pcm_bytes=%d",
                track.platform,
                track.display_name,
                selection.duration_seconds,
                len(pcm),
            )
            return selection

    async def search(
        self, keyword: str, *, platform: str | None = None, limit: int = 5
    ) -> list[MusicTrack]:
        """Search metadata without downloading or decoding audio."""
        if not isinstance(keyword, str) or not keyword.strip():
            raise ValueError("music search keyword must not be empty")
        if isinstance(limit, bool) or not 1 <= limit <= CHKSZ_MAX_SEARCH_RESULTS:
            raise ValueError("music search limit must be between 1 and 50")
        selected = platform or self.platform
        if selected == "auto":
            selected = "qq"
        if selected not in CHKSZ_PLATFORMS[:-1]:
            raise ValueError(f"unsupported ChKSz platform: {selected}")
        return await self._search_tracks(selected, keyword.strip(), limit=limit)

    async def lyrics(self, track_id: str, *, platform: str = "netease") -> str:
        """Read lyrics for a NetEase track, returning only bounded text."""
        if platform != "netease":
            raise ValueError("ChKSz lyrics currently requires platform=netease")
        value = _bounded_id(track_id)
        payload = await self._request_json(
            "/api/163_lyric", params={"id": value}
        )
        root = _payload_root(payload)
        for key in ("lrc", "lyric", "lyrics", "text"):
            lyric = root.get(key)
            if isinstance(lyric, str):
                return lyric[:64 * 1024]
            if isinstance(lyric, dict) and isinstance(lyric.get("lyric"), str):
                return lyric["lyric"][:64 * 1024]
        return ""

    async def playlist(self, playlist_id: str) -> Mapping[str, Any]:
        """Read a bounded NetEase playlist payload for future playlist tools."""
        payload = await self._request_json(
            "/api/163_playlist", params={"id": _bounded_id(playlist_id)}
        )
        # Do not retain large upstream objects in the library cache.  The
        # caller receives the validated JSON payload as returned by ChKSz.
        encoded = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        if len(encoded) > 512 * 1024:
            raise MusicSearchError("playlist response is too large")
        return payload

    async def _search(self, query: str) -> MusicTrack:
        platform, keyword = _select_platform(query, self.platform)
        return await self._search_platform(platform, keyword)

    async def _search_platform(self, platform: str, keyword: str) -> MusicTrack:
        if platform == "auto":
            return await self._search_auto(keyword)
        tracks = await self._search_tracks(platform, keyword, limit=5)
        if not tracks:
            raise _ChkszNoResult("ChKSz returned no matching track")
        selected = tracks[0]
        detail = await self._resolve_track(platform, keyword, selected)
        if detail is None:
            raise _ChkszNoResult("ChKSz returned no playable track")
        return detail

    async def _search_auto(self, keyword: str) -> MusicTrack:
        """Resolve a playable result while preferring likely original sources.

        The providers have different response times. Returning the first
        completed request made a NetEase cover beat a slightly slower QQ
        original for common songs. Requests remain parallel, but once a
        lower-priority provider succeeds we give higher-priority providers a
        short grace period to finish.
        """
        candidates = ("qq", "kugou", "netease")
        tasks = {
            candidate: asyncio.create_task(
                self._search_platform(candidate, keyword)
            )
            for candidate in candidates
        }
        last_error: MusicSearchError | None = None
        successful: dict[str, MusicTrack] = {}
        try:
            pending = set(tasks.values())
            while pending:
                done, pending = await asyncio.wait(
                    pending, return_when=asyncio.FIRST_COMPLETED
                )
                for task in done:
                    candidate = next(
                        name for name, item in tasks.items() if item is task
                    )
                    try:
                        successful[candidate] = task.result()
                    except _ChkszNoResult as exc:
                        last_error = exc
                    except ChkszApiError as exc:
                        if exc.status_code in (400, 401, 402, 403, 429):
                            raise
                        last_error = exc
                        LOGGER.warning(
                            "chksz transient platform failure "
                            "platform=%s status=%s",
                            candidate,
                            exc.status_code,
                        )
                    except MusicSearchError as exc:
                        last_error = exc
                        LOGGER.warning(
                            "chksz platform could not produce playable audio "
                            "platform=%s error=%s",
                            candidate,
                            type(exc).__name__,
                        )
                if successful:
                    best_platform = min(
                        successful,
                        key=CHKSZ_AUTO_PLATFORM_PRIORITY.__getitem__,
                    )
                    best_priority = CHKSZ_AUTO_PLATFORM_PRIORITY[best_platform]
                    higher_priority_pending = [
                        task
                        for candidate, task in tasks.items()
                        if candidate not in successful
                        and CHKSZ_AUTO_PLATFORM_PRIORITY[candidate] < best_priority
                        and not task.done()
                    ]
                    if not higher_priority_pending:
                        return successful[best_platform]
                    try:
                        await asyncio.wait(
                            higher_priority_pending,
                            timeout=CHKSZ_AUTO_PREFERRED_GRACE_SECONDS,
                        )
                    except asyncio.CancelledError:
                        raise
                    # Process any higher-priority task that completed during
                    # the grace interval before selecting the current best.
                    for candidate, task in tasks.items():
                        if (
                            candidate in successful
                            or task not in higher_priority_pending
                            or not task.done()
                        ):
                            continue
                        try:
                            successful[candidate] = task.result()
                        except Exception as exc:
                            last_error = exc if isinstance(exc, MusicSearchError) else last_error
                    best_platform = min(
                        successful,
                        key=CHKSZ_AUTO_PLATFORM_PRIORITY.__getitem__,
                    )
                    return successful[best_platform]
            raise last_error or MusicSearchError(
                "ChKSz returned no playable track"
            )
        finally:
            for task in tasks.values():
                if not task.done():
                    task.cancel()
            await asyncio.gather(*tasks.values(), return_exceptions=True)

    async def _search_tracks(
        self, platform: str, keyword: str, *, limit: int
    ) -> list[MusicTrack]:
        if platform == "qq":
            payload = await self._request_json(
                "/api/qq_music",
                params={"msg": keyword, "num": limit, "size": self.size},
            )
            items = _as_list(payload, "list")
        elif platform == "kugou":
            payload = await self._request_json(
                "/api/kugou_music",
                params={"msg": keyword, "num": limit, "size": self.size},
            )
            items = _as_list(payload, "list")
        elif platform == "netease":
            payload = await self._request_json(
                "/api/163_search",
                params={"keyword": keyword, "limit": limit, "offset": 0},
            )
            root = _payload_root(payload)
            if isinstance(root.get("result"), dict):
                root = root["result"]
            items = root.get("songs") or root.get("list") or root.get("data")
            if not isinstance(items, list):
                items = []
        else:
            raise ValueError(f"unsupported ChKSz platform: {platform}")
        tracks: list[MusicTrack] = []
        for item in items[:limit]:
            track = _parse_search_item(item, platform)
            if track is not None:
                tracks.append(track)
        return tracks

    async def _resolve_track(
        self, platform: str, keyword: str, selected: MusicTrack
    ) -> MusicTrack | None:
        if platform == "qq":
            params = {"mid": selected.track_id, "size": self.size}
            if not selected.track_id and selected.selection_index is not None:
                params = {
                    "msg": keyword,
                    "n": selected.selection_index,
                    "size": self.size,
                }
            payload = await self._request_json("/api/qq_music", params=params)
        elif platform == "kugou":
            params = {"id": selected.track_id, "size": self.size}
            if not selected.track_id and selected.selection_index is not None:
                params = {
                    "msg": keyword,
                    "n": selected.selection_index,
                    "size": self.size,
                }
            payload = await self._request_json("/api/kugou_music", params=params)
        else:
            payload = await self._request_json(
                "/api/163_music",
                params={
                    "id": selected.track_id,
                    "level": CHKSZ_NETEASE_LEVELS[self.size],
                },
            )
        return _parse_detail(payload, platform, selected)

    async def _download(self, url: str) -> bytes:
        client = await self._http_client()
        try:
            response = await client.get(url)
            response.raise_for_status()
        except httpx.HTTPError as exc:
            raise MusicSearchError(
                f"music download failed: {type(exc).__name__}"
            ) from exc
        content_length = response.headers.get("Content-Length")
        if content_length is not None:
            try:
                if int(content_length) > CHKSZ_DOWNLOAD_MAX_BYTES:
                    raise MusicSearchError("ChKSz audio is too large")
            except ValueError as exc:
                raise MusicSearchError("ChKSz audio has an invalid length") from exc
        payload = response.content
        if not payload:
            raise MusicSearchError("ChKSz audio is empty")
        if len(payload) > CHKSZ_DOWNLOAD_MAX_BYTES:
            raise MusicSearchError("ChKSz audio is too large")
        return payload

    async def _request_json(
        self, path: str, *, params: Mapping[str, Any], _retry: bool = True
    ) -> dict[str, Any]:
        client = await self._http_client()
        query = dict(params)
        query["apikey"] = self.api_key
        for attempt in range(CHKSZ_TRANSIENT_RETRIES + 1):
            try:
                response = await client.get(
                    self.base_url + path,
                    params=query,
                    timeout=CHKSZ_API_TIMEOUT_SECONDS,
                )
                break
            except (httpx.HTTPError, asyncio.TimeoutError):
                if attempt < CHKSZ_TRANSIENT_RETRIES:
                    LOGGER.warning(
                        "chksz transient request failure path=%s retry=%d",
                        path,
                        attempt + 1,
                    )
                    await asyncio.sleep(CHKSZ_TRANSIENT_RETRY_DELAY_SECONDS)
                    continue
                # httpx exceptions can retain the complete request URL.  The
                # API key is in that URL by contract, so suppress exception
                # chaining and never expose the request traceback.
                raise ChkszApiError(
                    "ChKSz music service is unavailable"
                ) from None

        retry_after = _retry_after(response.headers.get("Retry-After"))
        if response.status_code == 429 and _retry and retry_after is not None:
            await asyncio.sleep(retry_after)
            return await self._request_json(path, params=params, _retry=False)
        if response.status_code == 503 and _retry:
            LOGGER.warning("chksz service unavailable path=%s retry=1", path)
            await asyncio.sleep(CHKSZ_TRANSIENT_RETRY_DELAY_SECONDS)
            return await self._request_json(path, params=params, _retry=False)

        payload = _json_object(response)
        if response.status_code != 200:
            message = _response_message(payload, response.status_code)
            raise ChkszApiError(
                message,
                status_code=response.status_code,
                retry_after=retry_after,
            )
        provider_code = payload.get("code")
        if isinstance(provider_code, str) and provider_code.isdigit():
            provider_code = int(provider_code)
        if isinstance(provider_code, int) and provider_code not in (0, 200):
            raise ChkszApiError(
                _response_message(payload, provider_code),
                status_code=provider_code if provider_code in (400, 401, 402, 403, 404, 429, 503) else None,
                provider_code=provider_code,
            )
        remaining = response.headers.get("X-Quota-Free-Remaining")
        if remaining is not None:
            LOGGER.info("chksz request path=%s free_remaining=%s", path, remaining)
        return payload


class _ChkszNoResult(MusicSearchError):
    """Internal marker used by the optional auto platform fallback."""


def _protect_sensitive_http_logging() -> None:
    """Suppress URL-level client logs while ChKSz query credentials are active.

    httpx logs complete request URLs at INFO and httpcore logs request targets at
    DEBUG.  ChKSz puts the API key in the query string by contract, so those
    otherwise useful diagnostics are unsafe for this provider.  Application
    logs still record the endpoint path, selected platform, quota remaining,
    and bounded error status without the credential.
    """
    manager = logging.Logger.manager.loggerDict
    names = {"httpx", "httpcore"}
    names.update(
        name
        for name in manager
        if name.startswith("httpx.") or name.startswith("httpcore.")
    )
    for name in names:
        logger = logging.getLogger(name)
        if logger.level == logging.NOTSET or logger.level < logging.WARNING:
            logger.setLevel(logging.WARNING)


def _validate_api_key(value: str) -> None:
    if not isinstance(value, str) or not re.fullmatch(r"chksz_[A-Za-z0-9_-]{8,200}", value):
        raise ValueError("CHKSZ_API_KEY must start with chksz_ and contain a valid key")


def _bounded_id(value: str) -> str:
    if not isinstance(value, str) or not 1 <= len(value.strip()) <= 128:
        raise ValueError("ChKSz track or playlist id is invalid")
    return value.strip()


def _select_platform(query: str, default: str) -> tuple[str, str]:
    value = normalize_music_query(query)
    prefixes = (
        ("网易云音乐", "netease"),
        ("网易云", "netease"),
        ("QQ音乐", "qq"),
        ("qq音乐", "qq"),
        ("酷狗音乐", "kugou"),
        ("酷狗", "kugou"),
    )
    for prefix, platform in prefixes:
        if value.casefold().startswith(prefix.casefold()):
            keyword = value[len(prefix) :].strip(" ：:，,。.!！")
            return platform, keyword or value
    return default, value


def _payload_root(payload: Mapping[str, Any]) -> dict[str, Any]:
    data = payload.get("data")
    return data if isinstance(data, dict) else dict(payload)


def _as_list(payload: Mapping[str, Any], key: str) -> list[Any]:
    value = payload.get(key)
    return value if isinstance(value, list) else []


def _parse_search_item(item: Any, platform: str) -> MusicTrack | None:
    if not isinstance(item, dict):
        return None
    title = item.get("name") or item.get("title")
    artist = item.get("singer") or item.get("artist") or item.get("artists")
    if isinstance(artist, list):
        artist = ", ".join(
            str(entry.get("name", "")) for entry in artist if isinstance(entry, dict)
        )
    if isinstance(artist, dict):
        artist = artist.get("name", "")
    identifier = item.get("mid") if platform == "qq" else item.get("id")
    if platform == "netease" and identifier is None:
        identifier = item.get("songid")
    selection_index = item.get("n")
    if (
        isinstance(selection_index, bool)
        or not isinstance(selection_index, int)
        or not 1 <= selection_index <= CHKSZ_MAX_SEARCH_RESULTS
    ):
        selection_index = None
    if not isinstance(title, str) or not title.strip():
        return None
    if not isinstance(artist, str):
        artist = ""
    if not isinstance(identifier, (str, int)) or isinstance(identifier, bool):
        return None
    return MusicTrack(
        title=title.strip()[:120],
        artist=artist.strip()[:120],
        preview_url="",
        duration_seconds=_duration_seconds(item.get("duration") or item.get("interval")),
        platform=platform,
        track_id=str(identifier),
        selection_index=selection_index,
    )


def _parse_detail(
    payload: Mapping[str, Any], platform: str, fallback: MusicTrack
) -> MusicTrack | None:
    root = _payload_root(payload)
    url = root.get("url") or root.get("play_url") or root.get("playUrl")
    if not isinstance(url, str) or not url.startswith(("https://", "http://")):
        return None
    title = root.get("name") or fallback.title
    artist = root.get("singer") or root.get("artist") or fallback.artist
    if isinstance(artist, dict):
        artist = artist.get("name", fallback.artist)
    if not isinstance(title, str):
        title = fallback.title
    if not isinstance(artist, str):
        artist = fallback.artist
    return MusicTrack(
        title=title.strip()[:120],
        artist=artist.strip()[:120],
        preview_url=url,
        duration_seconds=_duration_seconds(
            root.get("interval") or root.get("duration") or fallback.duration_seconds
        ),
        platform=platform,
        track_id=fallback.track_id,
        album=_bounded_text(root.get("album")),
        cover_url=_bounded_url(root.get("cover")),
        lyrics_url=_bounded_url(root.get("lrc")),
        selection_index=fallback.selection_index,
    )


def _duration_seconds(value: Any) -> int:
    if isinstance(value, bool):
        return 0
    if isinstance(value, (int, float)) and math.isfinite(float(value)):
        return max(0, min(int(value), 60 * 60))
    if isinstance(value, str):
        parts = value.strip().split(":")
        try:
            if len(parts) == 2:
                return max(0, min(int(parts[0]) * 60 + int(float(parts[1])), 60 * 60))
            return max(0, min(int(float(value)), 60 * 60))
        except ValueError:
            return 0
    return 0


def _bounded_text(value: Any) -> str:
    return value.strip()[:512] if isinstance(value, str) else ""


def _bounded_url(value: Any) -> str:
    if isinstance(value, str) and value.startswith(("https://", "http://")):
        return value[:2048]
    return ""


def _json_object(response: httpx.Response) -> dict[str, Any]:
    try:
        payload = response.json()
    except (ValueError, json.JSONDecodeError) as exc:
        raise ChkszApiError(
            f"ChKSz returned invalid JSON (HTTP {response.status_code})",
            status_code=response.status_code,
        ) from exc
    if not isinstance(payload, dict):
        raise ChkszApiError(
            f"ChKSz returned an invalid response (HTTP {response.status_code})",
            status_code=response.status_code,
        )
    return payload


def _response_message(payload: Mapping[str, Any], status_code: int) -> str:
    value = payload.get("msg") or payload.get("message") or "request failed"
    message = str(value).replace("\r", " ").replace("\n", " ").strip()
    message = _KEY_PATTERN.sub("<redacted>", message)
    return f"ChKSz request failed (HTTP {status_code}): {message[:CHKSZ_ERROR_MESSAGE_LIMIT]}"


def _retry_after(value: str | None) -> float | None:
    if value is None:
        return None
    try:
        seconds = float(value)
    except ValueError:
        return None
    if not math.isfinite(seconds) or seconds < 0:
        return None
    return min(seconds, CHKSZ_MAX_RETRY_AFTER_SECONDS)


__all__ = [
    "CHKSZ_PLATFORMS",
    "CHKSZ_SIZES",
    "DEFAULT_CHKSZ_BASE_URL",
    "DEFAULT_CHKSZ_SIZE",
    "ChkszApiError",
    "ChkszMusicLibrary",
]
