#ifndef XIAOV_OPENVELA_PROTOCOL_H
#define XIAOV_OPENVELA_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XV_CONTROL_PROTOCOL_VERSION 1U
#define XV_CONTROL_TURN_ID_MAX 64U
#define XV_CONTROL_TEXT_MAX 2048U
#define XV_CONTROL_REMINDER_TIMER_ID_MAX 65U
#define XV_CONTROL_REMINDER_LABEL_MAX 401U
#define XV_CONTROL_REMINDER_KIND_MAX 32U
#define XV_CONTROL_MEDIA_ACTION_MAX 32U
#define XV_CONTROL_MEDIA_QUERY_MAX 1024U
#define XV_CONTROL_COMMAND_ID_MAX 64U
#define XV_CONTROL_ERROR_CODE_MAX 64U
#define XV_CONTROL_DISPLAY_MODE_MAX 16U
#define XV_CONTROL_DISPLAY_STATUS_MAX 16U

typedef enum {
  XV_CONTROL_OK = 0,
  XV_CONTROL_BAD_ARGUMENT,
  XV_CONTROL_BUFFER_TOO_SMALL,
  XV_CONTROL_MALFORMED_JSON,
  XV_CONTROL_INVALID_MESSAGE,
  XV_CONTROL_UNSUPPORTED_EVENT
} xv_control_result_t;

typedef enum {
  XV_SERVER_EVENT_SESSION_READY = 0,
  XV_SERVER_EVENT_ASR_PARTIAL,
  XV_SERVER_EVENT_ASR_FINAL,
  XV_SERVER_EVENT_ASSISTANT_DELTA,
  XV_SERVER_EVENT_ASSISTANT_FINAL,
  XV_SERVER_EVENT_REMINDER,
  XV_SERVER_EVENT_MEDIA_COMMAND,
  XV_SERVER_EVENT_DISPLAY_STATE,
  XV_SERVER_EVENT_TTS_START,
  XV_SERVER_EVENT_TTS_END,
  XV_SERVER_EVENT_MUSIC_END,
  XV_SERVER_EVENT_ERROR,
  XV_SERVER_EVENT_TURN_CANCELLED,
  XV_SERVER_EVENT_PONG
} xv_server_event_type_t;

typedef struct {
  xv_server_event_type_t type;
  uint32_t stream_id;
  uint32_t samples;
  uint32_t volume_percent;
  uint32_t nonce;
  double position_seconds;
  double deadline_epoch;
  char turn_id[XV_CONTROL_TURN_ID_MAX];
  char text[XV_CONTROL_TEXT_MAX];
  char timer_id[XV_CONTROL_REMINDER_TIMER_ID_MAX];
  char label[XV_CONTROL_REMINDER_LABEL_MAX];
  char reminder_kind[XV_CONTROL_REMINDER_KIND_MAX];
  char media_action[XV_CONTROL_MEDIA_ACTION_MAX];
  char media_query[XV_CONTROL_MEDIA_QUERY_MAX];
  char command_id[XV_CONTROL_COMMAND_ID_MAX];
  char error_code[XV_CONTROL_ERROR_CODE_MAX];
  char display_mode[XV_CONTROL_DISPLAY_MODE_MAX];
  char display_status[XV_CONTROL_DISPLAY_STATUS_MAX];
  bool media_ok;
  bool recoverable;
  bool has_turn_id;
  bool has_text;
  bool has_stream_id;
  bool has_samples;
  bool has_recoverable;
  bool has_timer_id;
  bool has_label;
  bool has_reminder_kind;
  bool has_deadline_epoch;
  bool has_media_action;
  bool has_media_query;
  bool has_volume_percent;
  bool has_position_seconds;
  bool has_command_id;
  bool has_error_code;
  bool has_media_ok;
  bool has_display_mode;
  bool has_display_status;
  bool has_nonce;
} xv_server_event_t;

xv_control_result_t xv_control_encode_session_start(
    char *destination, size_t destination_size, const char *event_id,
    uint64_t timestamp_ms, const char *device_id, const char *token,
    bool touch_capable, bool barge_in_capable, size_t *encoded_size);

xv_control_result_t xv_control_encode_listen_start(
    char *destination, size_t destination_size, const char *event_id,
    uint64_t timestamp_ms, const char *turn_id, uint32_t stream_id,
    size_t *encoded_size);

xv_control_result_t xv_control_encode_listen_stop(
    char *destination, size_t destination_size, const char *event_id,
    uint64_t timestamp_ms, const char *turn_id, const char *reason,
    size_t *encoded_size);

xv_control_result_t xv_control_encode_turn_cancel(
    char *destination, size_t destination_size, const char *event_id,
    uint64_t timestamp_ms, const char *turn_id, const char *reason,
    size_t *encoded_size);

xv_control_result_t xv_control_encode_ping(
    char *destination, size_t destination_size, const char *event_id,
    uint64_t timestamp_ms, bool include_nonce, uint32_t nonce,
    size_t *encoded_size);

xv_control_result_t xv_control_encode_media_result(
    char *destination, size_t destination_size, const char *event_id,
    uint64_t timestamp_ms, const char *command_id, bool ok,
    const char *error_code, size_t *encoded_size);

xv_control_result_t xv_control_encode_media_ready(
    char *destination, size_t destination_size, const char *event_id,
    uint64_t timestamp_ms, const char *command_id, bool ok,
    const char *error_code, size_t *encoded_size);

xv_control_result_t xv_control_encode_display_command(
    char *destination, size_t destination_size, const char *event_id,
    uint64_t timestamp_ms, const char *action, const char *source,
    size_t *encoded_size);

xv_control_result_t xv_control_decode_server_event(
    const char *json, size_t json_size, xv_server_event_t *event);

#ifdef __cplusplus
}
#endif

#endif
