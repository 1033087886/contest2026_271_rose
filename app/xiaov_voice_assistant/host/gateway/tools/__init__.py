"""Built-in function tools for the Xiao V gateway.

``build_default_tool_registry`` is intentionally safe for development: weather
is deterministic, timers are records only, and media/MQTT report ``dry_run``.
Production integrations must inject explicit backend implementations.
"""

from __future__ import annotations

from gateway.tools.greecam import (
    GREECAM_PARAMETERS,
    GreeCamController,
    greecam_control_tool,
)
from gateway.tools.media import (
    MEDIA_PARAMETERS,
    DryRunMediaController,
    MediaController,
    media_control_tool,
)
from gateway.tools.monitor import (
    MONITOR_PARAMETERS,
    DryRunMonitorController,
    MonitorController,
    monitor_control_tool,
)
from gateway.tools.mqtt import (
    MQTT_PARAMETERS,
    DryRunMqttPublisher,
    MqttPublisher,
    mqtt_control_tool,
)
from gateway.tools.registry import (
    ToolExecutionError,
    ToolRegistry,
    ToolRegistryError,
    ToolResult,
    ToolSpec,
    ToolValidationError,
    validate_json,
)
from gateway.tools.timers import (
    CANCEL_TIMER_PARAMETERS,
    CREATE_TIMER_PARAMETERS,
    LIST_TIMERS_PARAMETERS,
    InMemoryTimerStore,
    TimerStore,
    cancel_timer_tool,
    create_timer_tool,
    list_timers_tool,
)
from gateway.tools.weather import (
    WEATHER_PARAMETERS,
    LocalWeatherProvider,
    WeatherProvider,
    weather_tool,
)


def build_default_tool_registry(
    *,
    weather: WeatherProvider | None = None,
    timers: TimerStore | None = None,
    media: MediaController | None = None,
    monitor: MonitorController | None = None,
    mqtt: MqttPublisher | None = None,
    mqtt_topic_prefix: str = "xiaov/devices/",
    greecam: GreeCamController | None = None,
) -> ToolRegistry:
    """Build the standard registry with replaceable capability backends."""
    weather = weather if weather is not None else LocalWeatherProvider()
    timers = timers if timers is not None else InMemoryTimerStore()
    media = media if media is not None else DryRunMediaController()
    monitor = monitor if monitor is not None else DryRunMonitorController()
    mqtt = mqtt if mqtt is not None else DryRunMqttPublisher()
    registry = ToolRegistry()
    registry.add(
        name="get_weather",
        description="Get current weather for a location.",
        parameters=WEATHER_PARAMETERS,
        handler=weather_tool(weather),
    )
    registry.add(
        name="create_timer",
        description="Create a countdown timer or Pomodoro timer.",
        parameters=CREATE_TIMER_PARAMETERS,
        handler=create_timer_tool(timers),
    )
    registry.add(
        name="cancel_timer",
        description="Cancel an active timer by its timer id.",
        parameters=CANCEL_TIMER_PARAMETERS,
        handler=cancel_timer_tool(timers),
    )
    registry.add(
        name="list_timers",
        description="List active countdown and Pomodoro timers.",
        parameters=LIST_TIMERS_PARAMETERS,
        handler=list_timers_tool(timers),
    )
    registry.add(
        name="control_media",
        description=(
            "Control music and audio playback only on the Xiao V device. "
            "Never use this tool for video, movies, the camera monitor, or the "
            "display; use control_monitor for those."
        ),
        parameters=MEDIA_PARAMETERS,
        handler=media_control_tool(media),
    )
    registry.add(
        name="control_monitor",
        description=(
            "Control every camera-monitor and video request on the Xiao V "
            "display. Use stop_video whenever the user asks to stop or close "
            "a video; music and audio belong to control_media."
        ),
        parameters=MONITOR_PARAMETERS,
        handler=monitor_control_tool(monitor),
    )
    if greecam is None:
        registry.add(
            name="control_mqtt_device",
            description="Control a configured home device through the MQTT gateway.",
            parameters=MQTT_PARAMETERS,
            handler=mqtt_control_tool(mqtt, topic_prefix=mqtt_topic_prefix),
        )
    else:
        registry.add(
            name="control_air_conditioner",
            description=(
                "Control the Gree air conditioner. Use turn_on, turn_off, or "
                "set_temperature with a Celsius value from 16 to 30."
            ),
            parameters=GREECAM_PARAMETERS,
            handler=greecam_control_tool(greecam),
        )
    return registry


__all__ = [
    "DryRunMediaController",
    "DryRunMonitorController",
    "DryRunMqttPublisher",
    "GREECAM_PARAMETERS",
    "GreeCamController",
    "InMemoryTimerStore",
    "LocalWeatherProvider",
    "MONITOR_PARAMETERS",
    "MediaController",
    "MonitorController",
    "MqttPublisher",
    "TimerStore",
    "ToolExecutionError",
    "ToolRegistry",
    "ToolRegistryError",
    "ToolResult",
    "ToolSpec",
    "ToolValidationError",
    "WeatherProvider",
    "build_default_tool_registry",
    "greecam_control_tool",
    "validate_json",
]
