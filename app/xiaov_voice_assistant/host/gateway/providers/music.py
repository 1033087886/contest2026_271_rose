"""Network music search and conversion for the production gateway.

The default adapter uses Deezer's public search endpoint and its official
preview URLs.  It deliberately handles previews rather than scraping a web
player or attempting to bypass DRM.  The board receives only normalized
16 kHz mono PCM, so the firmware does not need an MP3 decoder.
"""

from __future__ import annotations

import asyncio
import logging
import re
import shutil
import time
from collections.abc import Awaitable
from dataclasses import dataclass
from typing import Any, Protocol

import httpx

LOGGER = logging.getLogger(__name__)

DEFAULT_DEEZER_SEARCH_URL = "https://api.deezer.com/search"
MUSIC_DOWNLOAD_MAX_BYTES = 8 * 1024 * 1024
MUSIC_PCM_MAX_BYTES = 16_000 * 2 * 60
MUSIC_CACHE_TTL_SECONDS = 10 * 60
MUSIC_CACHE_ENTRIES = 8
MUSIC_REQUEST_TIMEOUT_SECONDS = 15.0
MUSIC_FFMPEG_TIMEOUT_SECONDS = 20.0


class MusicSearchError(RuntimeError):
    """Raised when a network track cannot be resolved or converted."""


@dataclass(frozen=True, slots=True)
class MusicTrack:
    title: str
    artist: str
    preview_url: str
    duration_seconds: int
    platform: str = ""
    track_id: str = ""
    album: str = ""
    cover_url: str = ""
    lyrics_url: str = ""
    selection_index: int | None = None

    @property
    def display_name(self) -> str:
        return f"{self.artist} - {self.title}" if self.artist else self.title


@dataclass(frozen=True, slots=True)
class MusicSelection:
    track: MusicTrack
    pcm: bytes

    @property
    def duration_seconds(self) -> int:
        samples = len(self.pcm) // 2
        return 0 if samples == 0 else (samples + 15_999) // 16_000


class MusicLibrary(Protocol):
    async def resolve(self, query: str) -> MusicSelection: ...


def normalize_music_query(query: str) -> str:
    """Remove speech command framing while retaining artist/title words."""
    if not isinstance(query, str):
        raise ValueError("music query must be a string")
    value = re.sub(r"\s+", " ", query.strip()).strip()
    for prefix in (
        "请播放一下",
        "请播放",
        "播放一下",
        "播放",
        "放一首",
        "来一首",
        "我想听",
        "想听",
        "给我放",
        "帮我播放",
        "帮我放",
        "play some",
        "play a",
        "play",
        "listen to",
    ):
        if value.casefold().startswith(prefix.casefold()):
            value = value[len(prefix) :].strip(" ，,。.!！")
            break
    return value or "music"


class DeezerMusicLibrary:
    """Resolve a natural-language query to a converted Deezer preview."""

    def __init__(
        self,
        *,
        search_url: str = DEFAULT_DEEZER_SEARCH_URL,
        ffmpeg: str | None = None,
        client: httpx.AsyncClient | None = None,
        cache_ttl_seconds: float = MUSIC_CACHE_TTL_SECONDS,
        request_timeout_seconds: float = MUSIC_REQUEST_TIMEOUT_SECONDS,
        ffmpeg_timeout_seconds: float = MUSIC_FFMPEG_TIMEOUT_SECONDS,
    ) -> None:
        if not search_url.startswith("https://"):
            raise ValueError("music search URL must use HTTPS")
        if cache_ttl_seconds < 0:
            raise ValueError("music cache TTL cannot be negative")
        if request_timeout_seconds <= 0 or ffmpeg_timeout_seconds <= 0:
            raise ValueError("music timeouts must be positive")
        self.search_url = search_url
        self.ffmpeg = ffmpeg or shutil.which("ffmpeg")
        self.cache_ttl_seconds = cache_ttl_seconds
        self.request_timeout_seconds = request_timeout_seconds
        self.ffmpeg_timeout_seconds = ffmpeg_timeout_seconds
        self._client = client
        self._owns_client = client is None
        self._cache: dict[str, tuple[float, MusicSelection]] = {}
        self._lock = asyncio.Lock()

    async def _http_client(self) -> httpx.AsyncClient:
        if self._client is None:
            self._client = httpx.AsyncClient(
                timeout=httpx.Timeout(self.request_timeout_seconds),
                follow_redirects=True,
                headers={"User-Agent": "xiaov-gateway/1 music"},
            )
        return self._client

    async def resolve(self, query: str) -> MusicSelection:
        normalized = normalize_music_query(query)
        now = time.monotonic()
        async with self._lock:
            cached = self._cache.get(normalized)
            if cached is not None and now - cached[0] < self.cache_ttl_seconds:
                LOGGER.info("music cache hit query=%s", normalized)
                return cached[1]

            track = await self._search(normalized)
            audio = await self._download(track.preview_url)
            pcm = await self._decode(audio)
            selection = MusicSelection(track=track, pcm=pcm)
            self._cache[normalized] = (time.monotonic(), selection)
            while len(self._cache) > MUSIC_CACHE_ENTRIES:
                oldest = min(self._cache, key=lambda key: self._cache[key][0])
                self._cache.pop(oldest, None)
            LOGGER.info(
                "music resolved query=%s track=%s duration_s=%s pcm_bytes=%d",
                normalized,
                track.display_name,
                selection.duration_seconds,
                len(pcm),
            )
            return selection

    async def _search(self, query: str) -> MusicTrack:
        client = await self._http_client()
        try:
            response = await client.get(
                self.search_url,
                params={"q": query, "limit": 5, "order": "RANKING"},
            )
            response.raise_for_status()
            payload = response.json()
        except (httpx.HTTPError, ValueError) as exc:
            raise MusicSearchError(
                f"music search failed: {type(exc).__name__}"
            ) from exc
        if not isinstance(payload, dict) or not isinstance(payload.get("data"), list):
            raise MusicSearchError("music search returned an invalid response")
        for item in payload["data"]:
            track = _parse_track(item)
            if track is not None:
                return track
        raise MusicSearchError("music search returned no playable preview")

    async def _download(self, url: str) -> bytes:
        client = await self._http_client()
        try:
            response = await client.get(url)
            response.raise_for_status()
        except httpx.HTTPError as exc:
            raise MusicSearchError(
                f"music preview download failed: {type(exc).__name__}"
            ) from exc
        content_length = response.headers.get("Content-Length")
        if content_length is not None:
            try:
                if int(content_length) > MUSIC_DOWNLOAD_MAX_BYTES:
                    raise MusicSearchError("music preview is too large")
            except ValueError as exc:
                raise MusicSearchError("music preview has an invalid length") from exc
        payload = response.content
        if not payload:
            raise MusicSearchError("music preview is empty")
        if len(payload) > MUSIC_DOWNLOAD_MAX_BYTES:
            raise MusicSearchError("music preview is too large")
        return payload

    async def _decode(self, encoded: bytes) -> bytes:
        if not self.ffmpeg:
            raise MusicSearchError("ffmpeg is unavailable for music conversion")
        try:
            process = await asyncio.create_subprocess_exec(
                self.ffmpeg,
                "-hide_banner",
                "-loglevel",
                "error",
                "-i",
                "pipe:0",
                "-t",
                "60",
                "-f",
                "s16le",
                "-acodec",
                "pcm_s16le",
                "-ac",
                "1",
                "-ar",
                "16000",
                "pipe:1",
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            stdout, stderr = await asyncio.wait_for(
                process.communicate(encoded), timeout=self.ffmpeg_timeout_seconds
            )
        except (OSError, asyncio.TimeoutError) as exc:
            if "process" in locals() and process.returncode is None:
                process.kill()
                await process.wait()
            raise MusicSearchError(
                f"music preview conversion failed: {type(exc).__name__}"
            ) from exc
        if process.returncode != 0 or not stdout:
            detail = stderr.decode("utf-8", errors="replace").strip()[:120]
            raise MusicSearchError(
                "music preview conversion failed"
                + (f": {detail}" if detail else "")
            )
        if len(stdout) > MUSIC_PCM_MAX_BYTES or len(stdout) % 2:
            raise MusicSearchError("converted music preview is invalid")
        return stdout

    async def aclose(self) -> None:
        if self._client is not None and self._owns_client:
            await self._client.aclose()
            self._client = None


def _parse_track(item: Any) -> MusicTrack | None:
    if not isinstance(item, dict):
        return None
    title = item.get("title")
    preview = item.get("preview")
    artist = item.get("artist")
    artist_name = artist.get("name") if isinstance(artist, dict) else ""
    duration = item.get("duration", 0)
    if (
        not isinstance(title, str)
        or not title.strip()
        or not isinstance(artist_name, str)
        or not isinstance(preview, str)
        or not preview.startswith("https://")
        or isinstance(duration, bool)
        or not isinstance(duration, int)
    ):
        return None
    return MusicTrack(
        title=title.strip()[:120],
        artist=artist_name.strip()[:120],
        preview_url=preview,
        duration_seconds=max(0, min(duration, 60)),
    )


__all__ = [
    "DEFAULT_DEEZER_SEARCH_URL",
    "DeezerMusicLibrary",
    "MusicLibrary",
    "MusicSearchError",
    "MusicSelection",
    "MusicTrack",
    "normalize_music_query",
]
