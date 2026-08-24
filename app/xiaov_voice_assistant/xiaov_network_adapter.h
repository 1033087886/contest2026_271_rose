#ifndef XIAOV_OPENVELA_NETWORK_ADAPTER_H
#define XIAOV_OPENVELA_NETWORK_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xiaov/state_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XV_NETWORK_HOST_MAX 128U
#define XV_NETWORK_PATH_MAX 192U
#define XV_NETWORK_DEVICE_ID_MAX 64U
#define XV_NETWORK_TOKEN_MAX 128U
#define XV_NETWORK_TURN_ID_MAX 32U
#define XV_NETWORK_VIDEO_WIDTH 320U
#define XV_NETWORK_VIDEO_HEIGHT 240U
#define XV_NETWORK_VIDEO_FRAME_BYTES \
  (XV_NETWORK_VIDEO_WIDTH * XV_NETWORK_VIDEO_HEIGHT * 2U)
#define XV_NETWORK_VIDEO_FRAGMENT_BYTES 25600U
#define XV_NETWORK_VIDEO_FRAGMENT_COUNT 6U

typedef void (*xv_network_event_fn)(void *context, xv_app_event_t event);
typedef enum {
  XV_NETWORK_TEXT_ASR_PARTIAL = 0,
  XV_NETWORK_TEXT_ASR_FINAL,
  XV_NETWORK_TEXT_ASSISTANT_DELTA,
  XV_NETWORK_TEXT_ASSISTANT_FINAL
} xv_network_text_kind_t;

/* The callback must consume text before returning; storage belongs to the
 * network worker. Text is validated UTF-8 and always NUL-terminated. */
typedef void (*xv_network_text_fn)(void *context,
                                   xv_network_text_kind_t kind,
                                   const char *turn_id, const char *text);
typedef void (*xv_network_reminder_fn)(void *context, const char *timer_id,
                                       const char *label, const char *kind,
                                       double deadline_epoch);
typedef struct {
  const char *command_id;
  const char *action;
  const char *query;
  uint32_t volume_percent;
  double position_seconds;
  bool has_query;
  bool has_volume_percent;
  bool has_position_seconds;
} xv_network_media_command_t;
typedef void (*xv_network_media_command_fn)(
    void *context, const xv_network_media_command_t *command);
typedef bool (*xv_network_tts_pcm_fn)(void *context, const int16_t *samples,
                                      size_t sample_count);
typedef void (*xv_network_tts_end_fn)(void *context, uint32_t sample_count);
typedef bool (*xv_network_music_pcm_fn)(void *context, const int16_t *samples,
                                        size_t sample_count);
typedef void (*xv_network_music_end_fn)(void *context, uint32_t sample_count);
typedef bool (*xv_network_video_frame_fn)(void *context,
                                          const uint8_t *rgb565,
                                          size_t byte_count,
                                          uint32_t sequence,
                                          uint32_t timestamp_ms);
typedef void (*xv_network_display_state_fn)(void *context, const char *mode,
                                            const char *status);

typedef struct {
  const char *host;
  const char *path;
  const char *device_id;
  const char *token;
  uint16_t port;
  bool use_tls;
} xv_network_config_t;

typedef struct {
  uint64_t connect_attempts;
  uint64_t sessions_established;
  uint64_t disconnects;
  uint64_t service_errors;
  uint64_t pings_sent;
  uint64_t pongs_received;
  uint64_t ping_timeouts;
  uint64_t auth_rejections;
  uint32_t reconnect_backoff_ms;
} xv_network_stats_t;

typedef struct {
  void *context;
  xv_network_event_fn emit_event;
  xv_network_text_fn emit_text;
  xv_network_reminder_fn emit_reminder;
  xv_network_media_command_fn emit_media_command;
  xv_network_tts_pcm_fn accept_tts_pcm;
  xv_network_tts_end_fn finish_tts;
  xv_network_music_pcm_fn accept_music_pcm;
  xv_network_music_end_fn finish_music;
  xv_network_video_frame_fn accept_video_frame;
  xv_network_display_state_fn display_state;
} xv_network_callbacks_t;

/*
 * The implementation owns a worker thread and bounded queues. Keep it opaque
 * so libwebsockets details never leak into the portable core or application.
 */
typedef struct xv_network_adapter xv_ws_adapter_t;

#define XV_NETWORK_ADAPTER_STORAGE_SIZE 262144U

typedef union {
  uint64_t alignment;
  uint8_t bytes[XV_NETWORK_ADAPTER_STORAGE_SIZE];
} xv_network_adapter_storage_t;

xv_ws_adapter_t *xv_network_init(
    xv_network_adapter_storage_t *storage,
    const xv_network_config_t *config,
    const xv_network_callbacks_t *callbacks);
void xv_network_deinit(xv_ws_adapter_t *adapter);

bool xv_network_start(xv_ws_adapter_t *adapter);
void xv_network_stop(xv_ws_adapter_t *adapter);

/* Runtime adapter callbacks. They only update bounded state and wake worker. */
bool xv_network_ensure_session(void *context);
/*
 * Atomically require an established session and reserve the next turn.
 * One-shot capture uses this after its bounded ready wait so a disconnect
 * cannot reopen the PCM queue-overflow window between readiness and capture.
 */
bool xv_network_ensure_ready_session(void *context);
bool xv_network_request_audio_end(void *context);
bool xv_network_cancel_turn(void *context);

/* Arms the receive path for a gateway-owned normalized PCM music stream. */
bool xv_network_begin_music_stream(xv_ws_adapter_t *adapter,
                                   uint32_t stream_id);
void xv_network_stop_music_stream(xv_ws_adapter_t *adapter);

/* Called from the capture worker. v1 requires exactly 20 ms mono PCM frames. */
bool xv_network_send_pcm(xv_ws_adapter_t *adapter,
                         const int16_t *samples, size_t sample_count,
                         uint32_t timestamp_ms);

/*
 * Called by the capture worker after media_recorder has stopped and no more
 * PCM can arrive. This is the only point that may enqueue listen.stop.
 */
void xv_network_capture_drained(xv_ws_adapter_t *adapter);

size_t xv_network_dropped_frames(const xv_ws_adapter_t *adapter);
bool xv_network_is_connected(const xv_ws_adapter_t *adapter);
void xv_network_get_stats(const xv_ws_adapter_t *adapter,
                          xv_network_stats_t *stats);

/* Applies libwebsockets receive flow control from the network worker thread. */
bool xv_network_set_rx_paused(xv_ws_adapter_t *adapter, bool paused);
bool xv_network_set_touch_capable(xv_ws_adapter_t *adapter, bool capable);

/* Queues the bounded completion ACK on the existing control TX queue. */
bool xv_network_send_media_result(xv_ws_adapter_t *adapter,
                                  const char *command_id, bool ok,
                                  const char *error_code);
bool xv_network_send_media_ready(xv_ws_adapter_t *adapter,
                                 const char *command_id, bool ok,
                                 const char *error_code);
bool xv_network_send_display_command(xv_ws_adapter_t *adapter,
                                     const char *action,
                                     const char *source);

#ifdef __cplusplus
}
#endif

#endif
