from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
from pathlib import Path

from websockets.asyncio.server import ServerConnection, serve

from gateway.providers.base import VoicePipeline
from gateway.providers.mock import MockPipeline
from gateway.reminders import ReminderHub
from gateway.session import EventObserver, GatewaySession

LOGGER = logging.getLogger(__name__)
# A public gateway spends real MiMo tokens, so the shared secret has to be long
# enough that guessing it is not worth attempting. 32 hex chars from
# `python -c "import secrets; print(secrets.token_hex(16))"` clears this.
MIN_TOKEN_LENGTH = 16


class JsonlEventLog:
    """Append opt-in control-event evidence without recording any audio."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._file = self.path.open("a", encoding="utf-8", buffering=1)
        self._lock = asyncio.Lock()

    async def __call__(self, record: dict[str, object]) -> None:
        line = json.dumps(record, ensure_ascii=False, separators=(",", ":"))
        async with self._lock:
            self._file.write(line + "\n")
            self._file.flush()

    def close(self) -> None:
        self._file.close()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Xiao V development WebSocket gateway")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=8765, type=int)
    parser.add_argument(
        "--asr",
        choices=("mock", "openai", "mimo"),
        default="mock",
        help=(
            "speech recognition provider; mimo talks to the hosted chat endpoint "
            "because MiMo has no /audio/transcriptions route"
        ),
    )
    parser.add_argument(
        "--require-wake-phrase",
        action="store_true",
        help=(
            "answer a turn only when the transcript names the device, so the "
            "board can stream on speech detection and the wake decision happens "
            "here instead of in an on-device model"
        ),
    )
    parser.add_argument(
        "--local-wake",
        action="store_true",
        help=(
            "run a local sherpa-onnx wake spotter before ASR, so unaddressed "
            "speech is dropped without a cloud round trip; requires "
            "--require-wake-phrase and the bilingual KWS model under models/"
        ),
    )
    parser.add_argument(
        "--llm",
        choices=("mock", "mimo"),
        default="mock",
        help="answer provider; mimo requires MIMO_API_KEY in the environment",
    )
    parser.add_argument(
        "--mimo-model",
        default="mimo-v2.5-pro",
        help="MiMo model id; mimo-v2-* ids were retired on 2026-06-30",
    )
    parser.add_argument(
        "--tool-timeout-seconds",
        type=float,
        default=20.0,
        help=(
            "maximum runtime for one function tool; production weather can "
            "perform separate geocoding and forecast requests"
        ),
    )
    parser.add_argument(
        "--tools",
        choices=("auto", "off", "development", "production"),
        default="auto",
        help=(
            "MiMo function tools: auto enables safe development backends; "
            "production enables Open-Meteo and persistent reminders"
        ),
    )
    parser.add_argument(
        "--timer-state",
        default=None,
        help=(
            "persistent timer JSON path for --tools production; defaults to "
            "XIAOV_TIMER_STATE or ~/.local/state/xiaov/timers.json"
        ),
    )
    parser.add_argument(
        "--media-default-source",
        default="/data/xiaov-demo.wav",
        help="device-local source used when a play query is not a path or HTTP(S) URL",
    )
    parser.add_argument(
        "--music-provider",
        choices=("off", "deezer", "chksz"),
        default=None,
        help=(
            "production network music source; defaults to XIAOV_MUSIC_PROVIDER "
            "or ChKSz when CHKSZ_API_KEY is configured"
        ),
    )
    parser.add_argument(
        "--music-search-url",
        default=None,
        help="HTTPS search endpoint for the selected music provider",
    )
    parser.add_argument(
        "--music-ffmpeg",
        default=None,
        help="ffmpeg executable used to convert network music to PCM",
    )
    parser.add_argument(
        "--chksz-platform",
        choices=("qq", "kugou", "netease", "auto"),
        default=None,
        help="default ChKSz music platform; defaults to XIAOV_CHKSZ_PLATFORM or auto",
    )
    parser.add_argument(
        "--chksz-size",
        choices=("128k", "320k", "flac", "hires", "master"),
        default=None,
        help="ChKSz source quality; defaults to XIAOV_CHKSZ_SIZE or 320k",
    )
    parser.add_argument(
        "--video-local-source",
        default=None,
        help=(
            "local MP4 fallback for display video; defaults to "
            "XIAOV_VIDEO_LOCAL_SOURCE or the repository birds.mp4 sample"
        ),
    )
    parser.add_argument(
        "--video-fps",
        type=int,
        choices=range(1, 11),
        default=1,
        metavar="1..10",
        help="RGB565 display frame rate; use 1 for constrained board/Wi-Fi links",
    )
    parser.add_argument(
        "--mqtt-host",
        default=None,
        help=(
            "MQTT broker host for --tools production; credentials come only "
            "from XIAOV_MQTT_USERNAME/XIAOV_MQTT_PASSWORD"
        ),
    )
    parser.add_argument("--mqtt-port", default=None, type=int)
    parser.add_argument("--mqtt-tls", action="store_true")
    parser.add_argument(
        "--mqtt-ca-file",
        default=None,
        help="private broker CA path; defaults to XIAOV_MQTT_CA_FILE",
    )
    parser.add_argument(
        "--mqtt-profile",
        choices=("generic", "greecam_v1"),
        default=None,
        help="MQTT wire protocol; greecam_v1 waits for the real device ACK",
    )
    parser.add_argument(
        "--mqtt-device-id",
        default=None,
        help="configured device id for a protocol-specific MQTT profile",
    )
    parser.add_argument(
        "--mqtt-topic-prefix",
        default=None,
        help="MQTT topic prefix; defaults from --mqtt-profile",
    )
    parser.add_argument(
        "--mqtt-config",
        default=None,
        help=(
            "JSON MQTT config path; credentials stay in the file and are never "
            "accepted on the command line"
        ),
    )
    parser.add_argument(
        "--tts",
        choices=("mock", "openai"),
        default="mock",
        help="speech synthesis provider",
    )
    parser.add_argument(
        "--speech-base-url",
        default="http://127.0.0.1:8000/v1",
        help=(
            "shared OpenAI-compatible ASR/TTS base URL; optional Bearer auth "
            "comes only from XIAOV_SPEECH_API_KEY"
        ),
    )
    parser.add_argument(
        "--asr-base-url",
        default=None,
        help="optional ASR base URL override",
    )
    parser.add_argument(
        "--tts-base-url",
        default=None,
        help="optional TTS base URL override",
    )
    parser.add_argument("--asr-model", default="whisper-1")
    parser.add_argument(
        "--mimo-asr-model",
        default="mimo-v2.5-asr",
        help="MiMo transcription model id, used with --asr mimo",
    )
    parser.add_argument("--tts-model", default="tts-1")
    parser.add_argument("--tts-voice", default="alloy")
    parser.add_argument(
        "--mock-latency-ms",
        default=15,
        type=int,
        help="artificial latency per mock provider chunk",
    )
    parser.add_argument(
        "--mock-tts-amplitude",
        default=3_000,
        type=int,
        help=(
            "mock 440 Hz PCM amplitude; set to 0 only for acoustic tests where "
            "speaker feedback would contaminate the microphone"
        ),
    )
    parser.add_argument("--log-level", default="INFO")
    parser.add_argument(
        "--event-log",
        default=None,
        help=(
            "append outbound control events to a JSONL file for explicit HIL "
            "auditing; includes transcripts and answers but never audio or keys"
        ),
    )
    return parser


def build_pipeline(
    args: argparse.Namespace, *, reminder_hub: ReminderHub | None = None
) -> VoicePipeline:
    """Builds independently selectable ASR, answer, and TTS stages."""
    mock = MockPipeline(
        latency_ms=args.mock_latency_ms,
        tts_amplitude=args.mock_tts_amplitude,
    )
    if (
        args.asr == args.llm == args.tts == "mock"
        and args.tools in ("auto", "off")
    ):
        return mock

    from gateway.providers.composite import CompositePipeline

    asr_source = mock
    tts_source = mock
    if args.asr == "mimo":
        # Imported lazily so the all-mock path has no httpx dependency.
        from gateway.providers.mimo_asr import MimoAsrProvider

        if not os.environ.get("MIMO_API_KEY"):
            raise SystemExit(
                "--asr mimo requires MIMO_API_KEY in the environment; "
                "never pass the key on the command line, where it lands in shell history"
            )
        # CompositePipeline.aclose already closes asr_source, so it does not need
        # to be listed as a separate resource.
        asr_source = MimoAsrProvider(
            base_url=args.asr_base_url or os.environ.get("MIMO_BASE_URL") or None,
            asr_model=args.mimo_asr_model,
        )
    speech_providers = {}
    if args.asr == "openai":
        speech_providers[args.asr_base_url or args.speech_base_url] = None
    if args.tts == "openai":
        speech_providers[args.tts_base_url or args.speech_base_url] = None
    if speech_providers:
        # Imported lazily so the all-mock path has no httpx dependency.
        from gateway.providers.openai_speech import OpenAICompatibleSpeechProvider

        for base_url in speech_providers:
            speech_providers[base_url] = OpenAICompatibleSpeechProvider(
                base_url=base_url,
                asr_model=args.asr_model,
                tts_model=args.tts_model,
                voice=args.tts_voice,
            )
        if args.asr == "openai":
            asr_source = speech_providers[args.asr_base_url or args.speech_base_url]
        if args.tts == "openai":
            tts_source = speech_providers[args.tts_base_url or args.speech_base_url]

    llm = mock
    resources: list[object] = []
    video_bridge = None
    weather_provider = None
    music_library = None
    if args.llm == "mimo":
        from gateway.providers.mimo import DEFAULT_SYSTEM_PROMPT, MimoLlm
        from gateway.skills import compose_system_prompt, load_skill_context
        from gateway.tools import build_default_tool_registry

        if not os.environ.get("MIMO_API_KEY"):
            raise SystemExit(
                "--llm mimo requires MIMO_API_KEY in the environment; "
                "never pass the key on the command line, where it lands in shell history"
            )
        tool_profile = "development" if args.tools == "auto" else args.tools
        registry = None
        if tool_profile == "development":
            registry = build_default_tool_registry()
            LOGGER.warning(
                "MiMo tools use development backends: weather is simulated, timers "
                "are process-local, and media/MQTT actions are dry runs"
            )
        elif tool_profile == "production":
            from gateway.tools.open_meteo import OpenMeteoWeatherProvider
            from gateway.tools.persistent_timers import PersistentTimerStore

            reminder_hub = reminder_hub or ReminderHub()
            timer_state = Path(
                args.timer_state
                or os.environ.get("XIAOV_TIMER_STATE")
                or Path.home() / ".local" / "state" / "xiaov" / "timers.json"
            ).expanduser()
            weather_provider = OpenMeteoWeatherProvider()
            timers = PersistentTimerStore(timer_state, on_expire=reminder_hub.publish)
            resources.extend((weather_provider, timers))
            from gateway.tools.device_media import DeviceMediaController

            music_provider_name = (
                args.music_provider
                or os.environ.get("XIAOV_MUSIC_PROVIDER")
                or ("chksz" if os.environ.get("CHKSZ_API_KEY") else "deezer")
            )
            if music_provider_name == "deezer":
                from gateway.providers.music import DeezerMusicLibrary

                music_library = DeezerMusicLibrary(
                    search_url=(
                        args.music_search_url
                        or os.environ.get("XIAOV_MUSIC_SEARCH_URL")
                        or "https://api.deezer.com/search"
                    ),
                    ffmpeg=args.music_ffmpeg
                    or os.environ.get("XIAOV_MUSIC_FFMPEG")
                    or None,
                )
                resources.append(music_library)
            elif music_provider_name == "chksz":
                from gateway.providers.chksz_music import ChkszMusicLibrary

                api_key = os.environ.get("CHKSZ_API_KEY")
                if not api_key:
                    raise SystemExit(
                        "--music-provider chksz requires CHKSZ_API_KEY; "
                        "store it in the private launcher key file"
                    )
                music_library = ChkszMusicLibrary(
                    api_key=api_key,
                    platform=(
                        args.chksz_platform
                        or os.environ.get("XIAOV_CHKSZ_PLATFORM")
                        or "auto"
                    ),
                    size=(
                        args.chksz_size
                        or os.environ.get("XIAOV_CHKSZ_SIZE")
                        or "320k"
                    ),
                    ffmpeg=args.music_ffmpeg
                    or os.environ.get("XIAOV_MUSIC_FFMPEG")
                    or None,
                )
                resources.append(music_library)
                LOGGER.info(
                    "network music provider=chksz platform=%s size=%s",
                    music_library.platform,
                    music_library.size,
                )
            elif music_provider_name != "off":
                raise SystemExit(
                    f"unsupported music provider: {music_provider_name}"
                )

            media = DeviceMediaController(
                reminder_hub,
                default_source=args.media_default_source,
                music_library=music_library,
            )
            # Close the streaming task before the underlying music library.
            # Otherwise a gateway restart can leave an in-flight PCM sender
            # alive until the event loop is torn down implicitly.
            resources.append(media)
            mqtt = None
            greecam = None
            mqtt_config: dict[str, object] = {}
            config_path = args.mqtt_config or os.environ.get("XIAOV_MQTT_CONFIG")
            if config_path:
                try:
                    config_file = Path(config_path).expanduser()
                    loaded = json.loads(config_file.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError) as exc:
                    raise SystemExit(
                        f"invalid --mqtt-config: {type(exc).__name__}"
                    ) from exc
                if not isinstance(loaded, dict):
                    raise SystemExit("invalid --mqtt-config: root must be an object")
                mqtt_config = loaded
            mqtt_host = (
                args.mqtt_host
                or os.environ.get("XIAOV_MQTT_HOST")
                or _config_text(mqtt_config, "host")
            )
            mqtt_port = (
                args.mqtt_port
                if args.mqtt_port is not None
                else _config_int(mqtt_config, "port", 1883)
            )
            mqtt_tls = bool(args.mqtt_tls or _config_bool(mqtt_config, "tls", False))
            mqtt_ca_file = (
                args.mqtt_ca_file
                or os.environ.get("XIAOV_MQTT_CA_FILE")
                or _config_text(mqtt_config, "ca_file")
            )
            if mqtt_ca_file and config_path and not Path(mqtt_ca_file).exists():
                sibling_ca = Path(config_path).expanduser().parent / Path(mqtt_ca_file).name
                if sibling_ca.exists():
                    mqtt_ca_file = str(sibling_ca)
            mqtt_username = (
                os.environ.get("XIAOV_MQTT_USERNAME")
                or _config_text(mqtt_config, "username")
            )
            mqtt_password = (
                os.environ.get("XIAOV_MQTT_PASSWORD")
                or _config_text(mqtt_config, "password")
            )
            mqtt_profile = (
                args.mqtt_profile
                or _config_text(mqtt_config, "topic_profile")
                or "generic"
            )
            mqtt_device_id = (
                args.mqtt_device_id
                or _config_text(mqtt_config, "device_id")
                or "gree-cam"
            )
            mqtt_topic_prefix = (
                args.mqtt_topic_prefix
                or _config_text(mqtt_config, "topic_prefix")
                or (
                    "greecam/v1/"
                    if mqtt_profile == "greecam_v1"
                    else "xiaov/devices/"
                )
            )
            if mqtt_profile not in ("generic", "greecam_v1"):
                raise SystemExit(f"unsupported MQTT profile: {mqtt_profile}")
            if mqtt_host:
                from gateway.tools.gmqtt_publisher import GmqttPublisher

                mqtt = GmqttPublisher(
                    host=mqtt_host,
                    port=mqtt_port,
                    tls=mqtt_tls,
                    ca_file=mqtt_ca_file,
                    username=mqtt_username,
                    password=mqtt_password,
                )
                resources.append(mqtt)
                if mqtt_profile == "greecam_v1":
                    from gateway.tools.greecam import GreeCamController

                    greecam = GreeCamController(
                        mqtt,
                        topic_prefix=mqtt_topic_prefix,
                        device_id=mqtt_device_id,
                    )
            elif mqtt_profile == "greecam_v1":
                raise SystemExit("--mqtt-profile greecam_v1 requires --mqtt-host")
            from gateway.video_bridge import VideoBridge

            default_video = None
            video_bridge = VideoBridge(
                mqtt=mqtt if mqtt_profile == "greecam_v1" else None,
                topic_prefix=mqtt_topic_prefix,
                device_id=mqtt_device_id,
                desired_fps=args.video_fps,
                local_video_source=(
                    args.video_local_source
                    or os.environ.get("XIAOV_VIDEO_LOCAL_SOURCE")
                    or default_video
                ),
            )
            registry = build_default_tool_registry(
                weather=weather_provider,
                timers=timers,
                media=media,
                mqtt=mqtt,
                mqtt_topic_prefix=mqtt_topic_prefix,
                greecam=greecam,
                monitor=video_bridge,
            )
            LOGGER.warning(
                "production tools enabled: weather/timers are live; media requires "
                "a device ACK; MQTT is %s",
                "live" if mqtt is not None else "a dry run",
            )
        skill_context = load_skill_context()
        if skill_context:
            LOGGER.info("loaded custom skills count=%d", skill_context.count("\n## Skill:"))
        llm = MimoLlm(
            model=args.mimo_model,
            system_prompt=compose_system_prompt(DEFAULT_SYSTEM_PROMPT, skill_context),
            tool_registry=registry,
            tool_timeout_seconds=args.tool_timeout_seconds,
        )
        if weather_provider is not None:
            from gateway.providers.fast_weather import FastWeatherLlm

            llm = FastWeatherLlm(llm, weather_provider)
    elif args.tools not in ("auto", "off"):
        raise SystemExit("--tools development/production requires --llm mimo")

    LOGGER.info(
        "using providers ASR=%s LLM=%s TTS=%s",
        args.asr,
        args.llm,
        args.tts,
    )
    return CompositePipeline(
        asr_source=asr_source,
        llm=llm,
        tts_source=tts_source,
        resources=resources,
        video_bridge=video_bridge,
    )


def resolve_auth_token(host: str) -> str | None:
    """Returns the shared secret devices must present in session.start.

    Read from XIAOV_GATEWAY_TOKEN rather than a flag so the secret does not land
    in shell history or the process list. Binding to anything other than
    loopback without a token would expose the MiMo key's spending power to
    anyone who can reach the port, so that combination is refused outright.
    """
    token = os.environ.get("XIAOV_GATEWAY_TOKEN")
    if token:
        # A short secret is guessable offline once an attacker reaches the port,
        # and the failure mode is someone else spending the MiMo quota.
        if len(token) < MIN_TOKEN_LENGTH:
            raise SystemExit(
                f"XIAOV_GATEWAY_TOKEN must be at least {MIN_TOKEN_LENGTH} characters; "
                "generate one with: python -c \"import secrets;print(secrets.token_urlsafe(32))\""
            )
        return token
    if host not in ("127.0.0.1", "::1", "localhost"):
        raise SystemExit(
            f"refusing to listen on {host} without authentication: set "
            "XIAOV_GATEWAY_TOKEN, or bind --host 127.0.0.1 for local development"
        )
    LOGGER.warning(
        "no XIAOV_GATEWAY_TOKEN set; accepting unauthenticated sessions on %s", host
    )
    return None


def _config_text(config: dict[str, object], key: str) -> str | None:
    value = config.get(key)
    if value is None:
        return None
    if not isinstance(value, str) or not value.strip():
        raise SystemExit(f"invalid MQTT config field: {key}")
    return value.strip()


def _config_int(config: dict[str, object], key: str, default: int) -> int:
    value = config.get(key, default)
    if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= 65535:
        raise SystemExit(f"invalid MQTT config field: {key}")
    return value


def _config_bool(config: dict[str, object], key: str, default: bool) -> bool:
    value = config.get(key, default)
    if not isinstance(value, bool):
        raise SystemExit(f"invalid MQTT config field: {key}")
    return value


def build_wake_spotter(args: argparse.Namespace) -> object | None:
    """Loads the local KWS pre-gate, or exits with a usable message.

    Exiting rather than warning is deliberate: --local-wake is an explicit
    request for a cheaper wake path, and silently serving without it would look
    identical while still billing every utterance to the cloud.
    """
    if not args.local_wake:
        return None
    if not args.require_wake_phrase:
        raise SystemExit(
            "--local-wake only has an effect with --require-wake-phrase; without "
            "it every turn is answered regardless of the wake decision"
        )

    from gateway.local_kws import LocalKwsUnavailable, LocalWakeSpotter

    models_root = Path(__file__).resolve().parents[1] / "models"
    try:
        spotter = LocalWakeSpotter(models_root)
    except LocalKwsUnavailable as exc:
        raise SystemExit(
            f"--local-wake requested but the local spotter is unavailable: {exc}\n"
            "Install sherpa-onnx and unpack the bilingual KWS model, or drop the flag."
        ) from exc
    LOGGER.info("local wake pre-gate enabled; unaddressed speech will not reach ASR")
    return spotter


async def run_server(
    host: str,
    port: int,
    pipeline: VoicePipeline,
    auth_token: str | None,
    reminder_hub: ReminderHub | None = None,
    require_wake_phrase: bool = False,
    wake_spotter: object | None = None,
    event_observer: EventObserver | None = None,
    video_bridge: object | None = None,
) -> None:
    async def handler(websocket: ServerConnection) -> None:
        await GatewaySession(
            websocket,
            pipeline=pipeline,
            auth_token=auth_token,
            reminder_hub=reminder_hub,
            require_wake_phrase=require_wake_phrase,
            wake_spotter=wake_spotter,
            event_observer=event_observer,
            video_bridge=video_bridge,
        ).run()

    try:
        start = getattr(pipeline, "astart", None)
        if start is not None:
            await start()
        # RC31 and newer firmware owns liveness with nonce-bearing application
        # ping/pong events and reconnects after its bounded pong deadline.  A
        # second WebSocket-level ping from websockets repeatedly made the NuttX
        # libwebsockets client close otherwise healthy video sessions exactly on
        # the 60 s server interval, without incrementing the firmware's
        # ping_timeouts counter.  Disable that redundant control-frame source.
        async with serve(
            handler,
            host,
            port,
            max_size=2 * 1024 * 1024,
            ping_interval=None,
        ) as server:
            addresses = ", ".join(str(sock.getsockname()) for sock in server.sockets)
            LOGGER.info(
                "gateway listening on %s (auth %s)",
                addresses,
                "required" if auth_token else "disabled",
            )
            await server.serve_forever()
    finally:
        close = getattr(pipeline, "aclose", None)
        if close is not None:
            await close()


def main() -> None:
    args = build_parser().parse_args()
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    auth_token = resolve_auth_token(args.host)
    reminder_hub = ReminderHub()
    pipeline = build_pipeline(args, reminder_hub=reminder_hub)
    wake_spotter = build_wake_spotter(args)
    event_log = JsonlEventLog(Path(args.event_log).expanduser()) if args.event_log else None
    if event_log is not None:
        LOGGER.warning("event audit enabled at %s; transcripts and answers are recorded", event_log.path)
    try:
        asyncio.run(
            run_server(
                args.host,
                args.port,
                pipeline,
                auth_token,
                reminder_hub,
                require_wake_phrase=args.require_wake_phrase,
                wake_spotter=wake_spotter,
                event_observer=event_log,
                video_bridge=getattr(pipeline, "video_bridge", None),
            )
        )
    except KeyboardInterrupt:
        pass
    finally:
        if event_log is not None:
            event_log.close()


if __name__ == "__main__":
    main()
