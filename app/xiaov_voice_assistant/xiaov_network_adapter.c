/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "xiaov_network_adapter.h"

#include "xiaov/audio_frame.h"
#include "xiaov_protocol.h"

#include <libwebsockets.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define XV_NETWORK_CONTROL_CAPACITY 16U
#define XV_NETWORK_PCM_CAPACITY 100U
#define XV_NETWORK_TX_DATA_MAX 1024U
#define XV_NETWORK_RX_DATA_MAX (XV_AUDIO_HEADER_SIZE + XV_AUDIO_MAX_PAYLOAD)
#define XV_NETWORK_FRAME_SAMPLES 320U
#define XV_NETWORK_WORKER_STACK 32768U
#define XV_NETWORK_RETRY_DELAY_US 1000000U
#define XV_NETWORK_MAX_CONNECT_FAILURES 5U
#define XV_NETWORK_RECONNECT_INITIAL_MS 1000U
#define XV_NETWORK_RECONNECT_MAX_MS 30000U
#define XV_NETWORK_CONNECT_TIMEOUT_SECONDS 15U
#define XV_NETWORK_FD_LIMIT_PER_THREAD 8U
#define XV_NETWORK_PING_INTERVAL_MS 30000U
#define XV_NETWORK_PONG_TIMEOUT_MS 15000U

#ifndef CONFIG_EXAMPLES_XIAOV_CA_BUNDLE_PATH
/* The romfs resource partition carries the CA bundle; /etc has no certs. */
#define CONFIG_EXAMPLES_XIAOV_CA_BUNDLE_PATH \
  "/resource/etc/ssl/curl/ca-certificates.crt"
#endif

typedef struct xv_network_adapter xv_network_adapter_t;

typedef enum {
  XV_TX_TEXT = 0,
  XV_TX_BINARY
} xv_tx_kind_t;

typedef struct {
  union {
    uint64_t alignment;
    uint8_t bytes[LWS_PRE + XV_NETWORK_TX_DATA_MAX];
  } storage;
  size_t length;
  xv_tx_kind_t kind;
  bool after_pcm;
} xv_tx_slot_t;

struct xv_network_adapter {
  pthread_mutex_t lock;
  pthread_t thread;
  struct lws_context *lws_context;
  struct lws *wsi;
  xv_network_callbacks_t callbacks;
  xv_tx_slot_t control_tx[XV_NETWORK_CONTROL_CAPACITY];
  xv_tx_slot_t pcm_tx[XV_NETWORK_PCM_CAPACITY];
  union {
    uint64_t alignment;
    uint8_t bytes[XV_NETWORK_RX_DATA_MAX];
  } rx;
  char host[XV_NETWORK_HOST_MAX];
  char path[XV_NETWORK_PATH_MAX];
  char device_id[XV_NETWORK_DEVICE_ID_MAX];
  char token[XV_NETWORK_TOKEN_MAX];
  char turn_id[XV_NETWORK_TURN_ID_MAX];
  size_t control_head;
  size_t control_count;
  size_t pcm_head;
  size_t pcm_count;
  size_t rx_length;
  size_t dropped_frames;
  uint32_t event_sequence;
  uint32_t turn_sequence;
  uint32_t stream_id;
  uint32_t pcm_sequence;
  uint32_t tts_stream_id;
  uint32_t tts_sequence;
  uint32_t tts_samples;
  uint32_t music_stream_id;
  uint32_t music_sequence;
  uint32_t music_samples;
  uint16_t port;
  uint8_t connect_failures;
  uint8_t reconnect_failures;
  bool use_tls;
  bool thread_started;
  bool stop_requested;
  bool connecting;
  bool connected;
  bool session_ready;
  bool want_turn;
  bool turn_started;
  bool end_requested;
  bool capture_drained;
  bool stop_enqueued;
  bool rx_binary;
  bool tts_active;
  bool music_active;
  bool rx_pause_requested;
  bool rx_paused;
  bool touch_capable;
  bool video_assembling;
  uint32_t video_stream_id;
  uint32_t video_sequence;
  uint32_t video_timestamp_ms;
  uint32_t ping_nonce;
  uint8_t video_next_fragment;
  uint64_t next_ping_ms;
  uint64_t ping_deadline_ms;
  uint64_t next_connect_ms;
  uint64_t connect_attempts;
  uint64_t sessions_established;
  uint64_t disconnects;
  uint64_t service_errors;
  uint64_t pings_sent;
  uint64_t pongs_received;
  uint64_t ping_timeouts;
  uint64_t auth_rejections;
  uint32_t reconnect_backoff_ms;
  bool ping_outstanding;
  bool keepalive_closing;
};

static uint8_t g_video_assembly[XV_NETWORK_VIDEO_FRAME_BYTES];

_Static_assert(sizeof(struct xv_network_adapter) <=
                   XV_NETWORK_ADAPTER_STORAGE_SIZE,
               "increase XV_NETWORK_ADAPTER_STORAGE_SIZE");

static int network_lws_callback(struct lws *wsi,
                                enum lws_callback_reasons reason,
                                void *user, void *input, size_t length);

static const struct lws_protocols g_protocols[] = {
    {"xiaov-v1", network_lws_callback, 0, XV_NETWORK_RX_DATA_MAX, 0, NULL, 0},
    LWS_PROTOCOL_LIST_TERM};

/* The application already owns reconnect policy and the Python gateway sends
 * a low-frequency WebSocket ping.  libwebsockets 4.3.1 otherwise adds its own
 * 300/310 second validity ping/hangup policy.  Under a sustained one-way
 * RGB565 stream that second probe repeatedly ended healthy sockets with 1006,
 * and one attempt eventually remained stuck in connecting forever.  A zeroed
 * per-connection policy disables only that internal validity timer; the
 * explicit context connect timeout below still bounds failed handshakes. */
static const lws_retry_bo_t g_connection_policy = {
    .retry_ms_table = NULL,
    .retry_ms_table_count = 0U,
    .conceal_count = 0U,
    .secs_since_valid_ping = 0U,
    .secs_since_valid_hangup = 0U,
    .jitter_percent = 0U,
};

static bool copy_string(char *destination, size_t destination_size,
                        const char *source, bool allow_empty) {
  size_t length;
  if (destination == NULL || destination_size == 0U || source == NULL) {
    return false;
  }
  length = strlen(source);
  if ((!allow_empty && length == 0U) || length >= destination_size) {
    return false;
  }
  memcpy(destination, source, length + 1U);
  return true;
}

static void network_wake(xv_network_adapter_t *adapter) {
  struct lws_context *context;
  pthread_mutex_lock(&adapter->lock);
  context = adapter->lws_context;
  pthread_mutex_unlock(&adapter->lock);
  if (context != NULL) {
    lws_cancel_service(context);
  }
}

static xv_tx_slot_t *control_slot(xv_network_adapter_t *adapter,
                                  size_t offset) {
  return &adapter->control_tx[(adapter->control_head + offset) %
                              XV_NETWORK_CONTROL_CAPACITY];
}

static xv_tx_slot_t *pcm_slot(xv_network_adapter_t *adapter, size_t offset) {
  return &adapter->pcm_tx[(adapter->pcm_head + offset) %
                          XV_NETWORK_PCM_CAPACITY];
}

static bool control_push_locked(xv_network_adapter_t *adapter,
                                const void *data, size_t length,
                                bool after_pcm) {
  xv_tx_slot_t *slot;
  if (data == NULL || length == 0U || length > XV_NETWORK_TX_DATA_MAX ||
      adapter->control_count == XV_NETWORK_CONTROL_CAPACITY) {
    return false;
  }

  slot = control_slot(adapter, adapter->control_count);
  memcpy(&slot->storage.bytes[LWS_PRE], data, length);
  slot->length = length;
  slot->kind = XV_TX_TEXT;
  slot->after_pcm = after_pcm;
  adapter->control_count += 1U;
  return true;
}

static bool pcm_push_locked(xv_network_adapter_t *adapter,
                            const void *data, size_t length) {
  xv_tx_slot_t *slot;
  if (data == NULL || length == 0U || length > XV_NETWORK_TX_DATA_MAX ||
      adapter->pcm_count == XV_NETWORK_PCM_CAPACITY) {
    adapter->dropped_frames += 1U;
    return false;
  }

  slot = pcm_slot(adapter, adapter->pcm_count);
  memcpy(&slot->storage.bytes[LWS_PRE], data, length);
  slot->length = length;
  slot->kind = XV_TX_BINARY;
  slot->after_pcm = false;
  adapter->pcm_count += 1U;
  return true;
}

static void control_pop_locked(xv_network_adapter_t *adapter) {
  adapter->control_head =
      (adapter->control_head + 1U) % XV_NETWORK_CONTROL_CAPACITY;
  adapter->control_count -= 1U;
}

static void pcm_pop_locked(xv_network_adapter_t *adapter) {
  adapter->pcm_head = (adapter->pcm_head + 1U) % XV_NETWORK_PCM_CAPACITY;
  adapter->pcm_count -= 1U;
}

static void queue_reset_locked(xv_network_adapter_t *adapter) {
  adapter->control_head = 0U;
  adapter->control_count = 0U;
  adapter->pcm_head = 0U;
  adapter->pcm_count = 0U;
  adapter->music_active = false;
}

static void next_event_id_locked(xv_network_adapter_t *adapter,
                                 char *destination,
                                 size_t destination_size) {
  adapter->event_sequence += 1U;
  snprintf(destination, destination_size, "evt-%08lu",
           (unsigned long)adapter->event_sequence);
}

static uint64_t network_monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0U;
  }
  return (uint64_t)now.tv_sec * 1000U +
         (uint64_t)now.tv_nsec / 1000000U;
}

static uint32_t network_reconnect_delay_ms(uint8_t failures) {
  uint32_t delay_ms = XV_NETWORK_RECONNECT_INITIAL_MS;

  while (failures > 1U && delay_ms < XV_NETWORK_RECONNECT_MAX_MS) {
    if (delay_ms > XV_NETWORK_RECONNECT_MAX_MS / 2U) {
      return XV_NETWORK_RECONNECT_MAX_MS;
    }
    delay_ms *= 2U;
    failures -= 1U;
  }
  return delay_ms > XV_NETWORK_RECONNECT_MAX_MS
             ? XV_NETWORK_RECONNECT_MAX_MS
             : delay_ms;
}

static void schedule_reconnect_locked(xv_network_adapter_t *adapter,
                                      uint64_t now_ms) {
  if (adapter->reconnect_failures < UINT8_MAX) {
    adapter->reconnect_failures += 1U;
  }
  adapter->reconnect_backoff_ms =
      network_reconnect_delay_ms(adapter->reconnect_failures);
  adapter->next_connect_ms =
      now_ms + (uint64_t)adapter->reconnect_backoff_ms;
}

static bool maybe_enqueue_stop_locked(xv_network_adapter_t *adapter);

static bool enqueue_ping_locked(xv_network_adapter_t *adapter,
                                uint64_t now_ms) {
  char event_id[24];
  char json[XV_NETWORK_TX_DATA_MAX];
  size_t length = 0U;
  xv_control_result_t result;

  adapter->ping_nonce += 1U;
  next_event_id_locked(adapter, event_id, sizeof(event_id));
  result = xv_control_encode_ping(json, sizeof(json), event_id, now_ms, true,
                                  adapter->ping_nonce, &length);
  if (result != XV_CONTROL_OK ||
      !control_push_locked(adapter, json, length, false)) {
    return false;
  }
  adapter->ping_outstanding = true;
  adapter->ping_deadline_ms = now_ms + XV_NETWORK_PONG_TIMEOUT_MS;
  adapter->pings_sent += 1U;
  return true;
}

static bool enqueue_session_start_locked(xv_network_adapter_t *adapter) {
  char event_id[24];
  char json[XV_NETWORK_TX_DATA_MAX];
  size_t length = 0U;
  xv_control_result_t result;

  next_event_id_locked(adapter, event_id, sizeof(event_id));
  result = xv_control_encode_session_start(
      json, sizeof(json), event_id, network_monotonic_ms(),
      adapter->device_id, adapter->token[0] == '\0' ? NULL : adapter->token,
      adapter->touch_capable, false, &length);
  return result == XV_CONTROL_OK &&
         control_push_locked(adapter, json, length, false);
}

static bool enqueue_listen_start_locked(xv_network_adapter_t *adapter) {
  char event_id[24];
  char json[XV_NETWORK_TX_DATA_MAX];
  size_t length = 0U;
  xv_control_result_t result;

  next_event_id_locked(adapter, event_id, sizeof(event_id));
  result = xv_control_encode_listen_start(
      json, sizeof(json), event_id, network_monotonic_ms(), adapter->turn_id,
      adapter->stream_id, &length);
  if (result != XV_CONTROL_OK ||
      !control_push_locked(adapter, json, length, false)) {
    return false;
  }
  adapter->turn_started = true;
  return maybe_enqueue_stop_locked(adapter);
}

static bool enqueue_listen_stop_locked(xv_network_adapter_t *adapter) {
  char event_id[24];
  char json[XV_NETWORK_TX_DATA_MAX];
  size_t length = 0U;
  xv_control_result_t result;

  next_event_id_locked(adapter, event_id, sizeof(event_id));
  result = xv_control_encode_listen_stop(
      json, sizeof(json), event_id, network_monotonic_ms(), adapter->turn_id,
      "end_of_speech", &length);
  return result == XV_CONTROL_OK &&
         control_push_locked(adapter, json, length, true);
}

static bool maybe_enqueue_stop_locked(xv_network_adapter_t *adapter) {
  if (!adapter->end_requested || !adapter->capture_drained ||
      !adapter->turn_started || adapter->stop_enqueued) {
    return true;
  }
  if (!enqueue_listen_stop_locked(adapter)) {
    return false;
  }
  adapter->stop_enqueued = true;
  return true;
}

static bool enqueue_cancel_locked(xv_network_adapter_t *adapter) {
  char event_id[24];
  char json[XV_NETWORK_TX_DATA_MAX];
  size_t length = 0U;
  xv_control_result_t result;

  next_event_id_locked(adapter, event_id, sizeof(event_id));
  result = xv_control_encode_turn_cancel(
      json, sizeof(json), event_id, network_monotonic_ms(), adapter->turn_id,
      "user_cancelled", &length);
  return result == XV_CONTROL_OK &&
         control_push_locked(adapter, json, length, false);
}

static void emit_event(xv_network_adapter_t *adapter, xv_app_event_t event) {
  if (adapter->callbacks.emit_event != NULL) {
    adapter->callbacks.emit_event(adapter->callbacks.context, event);
  }
}

static void emit_text(xv_network_adapter_t *adapter,
                      xv_network_text_kind_t kind,
                      const xv_server_event_t *event) {
  if (adapter->callbacks.emit_text != NULL) {
    adapter->callbacks.emit_text(adapter->callbacks.context, kind,
                                 event->turn_id, event->text);
  }
}

static void emit_reminder(xv_network_adapter_t *adapter,
                          const xv_server_event_t *event) {
  if (adapter->callbacks.emit_reminder != NULL) {
    adapter->callbacks.emit_reminder(
        adapter->callbacks.context, event->timer_id, event->label,
        event->reminder_kind, event->deadline_epoch);
  }
}

static void emit_media_command(xv_network_adapter_t *adapter,
                               const xv_server_event_t *event) {
  xv_network_media_command_t command;

  if (adapter->callbacks.emit_media_command == NULL) {
    return;
  }
  memset(&command, 0, sizeof(command));
  command.command_id = event->command_id;
  command.action = event->media_action;
  command.query = event->media_query;
  command.volume_percent = event->volume_percent;
  command.position_seconds = event->position_seconds;
  command.has_query = event->has_media_query;
  command.has_volume_percent = event->has_volume_percent;
  command.has_position_seconds = event->has_position_seconds;
  adapter->callbacks.emit_media_command(adapter->callbacks.context,
                                         &command);
}

static bool server_event_matches_turn(const xv_network_adapter_t *adapter,
                                      const xv_server_event_t *event) {
  return event->has_turn_id &&
         strcmp(event->turn_id, adapter->turn_id) == 0;
}

static void handle_server_control(xv_network_adapter_t *adapter,
                                  const uint8_t *data, size_t length) {
  xv_server_event_t event;
  xv_control_result_t result;
  bool wake = false;

  result = xv_control_decode_server_event((const char *)data, length, &event);
  if (result == XV_CONTROL_UNSUPPORTED_EVENT) {
    return;
  }
  if (result != XV_CONTROL_OK) {
    emit_event(adapter, XV_APP_EVENT_FAILURE);
    return;
  }

  switch (event.type) {
  case XV_SERVER_EVENT_PONG:
    pthread_mutex_lock(&adapter->lock);
    if (event.has_nonce && adapter->ping_outstanding &&
        event.nonce == adapter->ping_nonce) {
      adapter->ping_outstanding = false;
      adapter->ping_deadline_ms = 0U;
      adapter->next_ping_ms = network_monotonic_ms() +
                              XV_NETWORK_PING_INTERVAL_MS;
      adapter->pongs_received += 1U;
    }
    pthread_mutex_unlock(&adapter->lock);
    break;

  case XV_SERVER_EVENT_SESSION_READY:
    {
    bool attempted;
    pthread_mutex_lock(&adapter->lock);
    adapter->session_ready = true;
    adapter->reconnect_failures = 0U;
    adapter->reconnect_backoff_ms = 0U;
    adapter->next_connect_ms = 0U;
    attempted = adapter->want_turn && !adapter->turn_started;
    if (attempted) {
      wake = enqueue_listen_start_locked(adapter);
    }
    pthread_mutex_unlock(&adapter->lock);
    if (attempted && !wake) {
      emit_event(adapter, XV_APP_EVENT_FAILURE);
    } else if (wake) {
      network_wake(adapter);
    }
    break;
    }

  case XV_SERVER_EVENT_ASR_PARTIAL:
  case XV_SERVER_EVENT_ASR_FINAL:
  case XV_SERVER_EVENT_ASSISTANT_DELTA:
  case XV_SERVER_EVENT_ASSISTANT_FINAL:
    {
    bool matches;
    xv_network_text_kind_t kind;
    pthread_mutex_lock(&adapter->lock);
    matches = server_event_matches_turn(adapter, &event);
    pthread_mutex_unlock(&adapter->lock);
    if (!matches) {
      break;
    }
    if (event.type == XV_SERVER_EVENT_ASR_PARTIAL) {
      kind = XV_NETWORK_TEXT_ASR_PARTIAL;
    } else if (event.type == XV_SERVER_EVENT_ASR_FINAL) {
      kind = XV_NETWORK_TEXT_ASR_FINAL;
    } else if (event.type == XV_SERVER_EVENT_ASSISTANT_DELTA) {
      kind = XV_NETWORK_TEXT_ASSISTANT_DELTA;
    } else {
      kind = XV_NETWORK_TEXT_ASSISTANT_FINAL;
    }
    emit_text(adapter, kind, &event);
    break;
    }

  case XV_SERVER_EVENT_REMINDER:
    emit_reminder(adapter, &event);
    break;

  case XV_SERVER_EVENT_MEDIA_COMMAND:
    emit_media_command(adapter, &event);
    break;
  case XV_SERVER_EVENT_DISPLAY_STATE:
    if (adapter->callbacks.display_state != NULL) {
      adapter->callbacks.display_state(adapter->callbacks.context,
                                       event.display_mode,
                                       event.display_status);
    }
    break;

  case XV_SERVER_EVENT_TTS_START:
    if (!event.has_stream_id) {
      emit_event(adapter, XV_APP_EVENT_FAILURE);
      break;
    }
    pthread_mutex_lock(&adapter->lock);
    if (!server_event_matches_turn(adapter, &event)) {
      pthread_mutex_unlock(&adapter->lock);
      break;
    }
    adapter->tts_active = true;
    adapter->tts_stream_id = event.stream_id;
    adapter->tts_sequence = 0U;
    adapter->tts_samples = 0U;
    pthread_mutex_unlock(&adapter->lock);
    emit_event(adapter, XV_APP_EVENT_TTS_STARTED);
    break;

  case XV_SERVER_EVENT_TTS_END:
    pthread_mutex_lock(&adapter->lock);
    if (!server_event_matches_turn(adapter, &event)) {
      pthread_mutex_unlock(&adapter->lock);
      break;
    }
    if (!adapter->tts_active ||
        (event.has_stream_id && event.stream_id != adapter->tts_stream_id) ||
        (event.has_samples && event.samples != adapter->tts_samples)) {
      pthread_mutex_unlock(&adapter->lock);
      emit_event(adapter, XV_APP_EVENT_FAILURE);
      break;
    }
    adapter->tts_active = false;
    adapter->want_turn = false;
    adapter->turn_started = false;
    pthread_mutex_unlock(&adapter->lock);
    if (adapter->callbacks.finish_tts != NULL) {
      adapter->callbacks.finish_tts(adapter->callbacks.context,
                                    event.has_samples ? event.samples : 0U);
    }
    break;

  case XV_SERVER_EVENT_MUSIC_END:
    {
    bool valid;
    pthread_mutex_lock(&adapter->lock);
    valid = adapter->music_active &&
            event.stream_id == adapter->music_stream_id &&
            event.samples == adapter->music_samples;
    if (valid) {
      adapter->music_active = false;
    }
    pthread_mutex_unlock(&adapter->lock);
    if (!valid) {
      emit_event(adapter, XV_APP_EVENT_FAILURE);
    } else if (adapter->callbacks.finish_music != NULL) {
      adapter->callbacks.finish_music(adapter->callbacks.context,
                                      event.samples);
    }
    break;
    }

  case XV_SERVER_EVENT_ERROR:
    {
    bool active_turn;
    bool unauthorized;
    pthread_mutex_lock(&adapter->lock);
    if (event.has_turn_id &&
        !server_event_matches_turn(adapter, &event)) {
      pthread_mutex_unlock(&adapter->lock);
      break;
    }
    active_turn = adapter->want_turn || adapter->tts_active;
    unauthorized = event.has_error_code &&
                   strcmp(event.error_code, "unauthorized") == 0;
    if (unauthorized && !active_turn) {
      adapter->auth_rejections += 1U;
      pthread_mutex_unlock(&adapter->lock);
      break;
    }
    pthread_mutex_unlock(&adapter->lock);
    emit_event(adapter, XV_APP_EVENT_FAILURE);
    break;
    }

  case XV_SERVER_EVENT_TURN_CANCELLED:
    pthread_mutex_lock(&adapter->lock);
    if (!server_event_matches_turn(adapter, &event)) {
      pthread_mutex_unlock(&adapter->lock);
      break;
    }
    adapter->want_turn = false;
    adapter->turn_started = false;
    adapter->tts_active = false;
    pthread_mutex_unlock(&adapter->lock);
    break;
  }
}

static void handle_server_audio(xv_network_adapter_t *adapter,
                                const uint8_t *data, size_t length) {
  xv_audio_header_t header;
  const uint8_t *payload;
  xv_network_tts_pcm_fn sink;
  xv_network_music_pcm_fn music_sink;
  void *sink_context;
  bool valid;

  if (!xv_audio_frame_decode(data, length, &header, &payload)) {
    emit_event(adapter, XV_APP_EVENT_FAILURE);
    return;
  }
  if (header.kind == XV_AUDIO_KIND_VIDEO_RGB565) {
    uint8_t fragment = (uint8_t)(header.flags & 0xffU);
    uint8_t count = (uint8_t)(header.flags >> 8U);
    size_t offset = (size_t)fragment * XV_NETWORK_VIDEO_FRAGMENT_BYTES;
    bool valid_fragment =
        count == XV_NETWORK_VIDEO_FRAGMENT_COUNT && fragment < count &&
        header.payload_length == XV_NETWORK_VIDEO_FRAGMENT_BYTES;
    if (fragment == 0U) {
      adapter->video_assembling = valid_fragment;
      adapter->video_stream_id = header.stream_id;
      adapter->video_sequence = header.sequence;
      adapter->video_timestamp_ms = header.timestamp_ms;
      adapter->video_next_fragment = 0U;
    }
    valid_fragment =
        valid_fragment && adapter->video_assembling &&
        header.stream_id == adapter->video_stream_id &&
        header.sequence == adapter->video_sequence &&
        fragment == adapter->video_next_fragment &&
        offset + header.payload_length <= sizeof(g_video_assembly);
    if (!valid_fragment) {
      adapter->video_assembling = false;
      return;
    }
    memcpy(&g_video_assembly[offset], payload, header.payload_length);
    adapter->video_next_fragment += 1U;
    if (adapter->video_next_fragment == XV_NETWORK_VIDEO_FRAGMENT_COUNT) {
      adapter->video_assembling = false;
      if (adapter->callbacks.accept_video_frame != NULL) {
        (void)adapter->callbacks.accept_video_frame(
            adapter->callbacks.context, g_video_assembly,
            sizeof(g_video_assembly), adapter->video_sequence,
            adapter->video_timestamp_ms);
      }
    }
    return;
  }
  if ((header.kind != XV_AUDIO_KIND_TTS &&
       header.kind != XV_AUDIO_KIND_MUSIC) ||
      (header.payload_length & 1U) != 0U) {
    emit_event(adapter, XV_APP_EVENT_FAILURE);
    return;
  }

  pthread_mutex_lock(&adapter->lock);
  if (header.kind == XV_AUDIO_KIND_TTS) {
    valid = adapter->tts_active &&
            header.stream_id == adapter->tts_stream_id &&
            header.sequence == adapter->tts_sequence;
  } else {
    valid = adapter->music_active &&
            header.stream_id == adapter->music_stream_id &&
            header.sequence == adapter->music_sequence;
  }
  if (valid) {
    if (header.kind == XV_AUDIO_KIND_TTS) {
      adapter->tts_sequence += 1U;
      adapter->tts_samples += header.payload_length / sizeof(int16_t);
    } else {
      adapter->music_sequence += 1U;
      adapter->music_samples += header.payload_length / sizeof(int16_t);
    }
  }
  sink = adapter->callbacks.accept_tts_pcm;
  music_sink = adapter->callbacks.accept_music_pcm;
  sink_context = adapter->callbacks.context;
  pthread_mutex_unlock(&adapter->lock);

  if (!valid ||
      (header.kind == XV_AUDIO_KIND_TTS
           ? sink == NULL ||
                 !sink(sink_context, (const int16_t *)payload,
                      header.payload_length / sizeof(int16_t))
           : music_sink == NULL ||
                 !music_sink(sink_context, (const int16_t *)payload,
                             header.payload_length / sizeof(int16_t)))) {
    emit_event(adapter, XV_APP_EVENT_FAILURE);
  }
}

static void handle_receive_fragment(xv_network_adapter_t *adapter,
                                    struct lws *wsi, const void *input,
                                    size_t length) {
  bool first = lws_is_first_fragment(wsi);
  bool final = lws_is_final_fragment(wsi) &&
               lws_remaining_packet_payload(wsi) == 0U;
  bool binary = lws_frame_is_binary(wsi);
  bool overflow = false;

  pthread_mutex_lock(&adapter->lock);
  if (first) {
    adapter->rx_length = 0U;
    adapter->rx_binary = binary;
  } else if (adapter->rx_binary != binary) {
    overflow = true;
  }
  if (!overflow && length <= XV_NETWORK_RX_DATA_MAX - adapter->rx_length) {
    memcpy(&adapter->rx.bytes[adapter->rx_length], input, length);
    adapter->rx_length += length;
  } else {
    overflow = true;
    adapter->rx_length = 0U;
  }
  length = adapter->rx_length;
  binary = adapter->rx_binary;
  pthread_mutex_unlock(&adapter->lock);

  if (overflow) {
    emit_event(adapter, XV_APP_EVENT_FAILURE);
  } else if (final) {
    if (binary) {
      handle_server_audio(adapter, adapter->rx.bytes, length);
    } else {
      handle_server_control(adapter, adapter->rx.bytes, length);
    }
    pthread_mutex_lock(&adapter->lock);
    adapter->rx_length = 0U;
    pthread_mutex_unlock(&adapter->lock);
  }
}

static int send_front(xv_network_adapter_t *adapter, struct lws *wsi) {
  xv_tx_slot_t *slot;
  bool control;
  bool sendable;
  int result;

  pthread_mutex_lock(&adapter->lock);
  control = adapter->control_count > 0U &&
            (!control_slot(adapter, 0U)->after_pcm ||
             adapter->pcm_count == 0U);
  if (control) {
    slot = control_slot(adapter, 0U);
  } else if (adapter->pcm_count > 0U &&
             (adapter->turn_started || adapter->music_active)) {
    slot = pcm_slot(adapter, 0U);
  } else {
    pthread_mutex_unlock(&adapter->lock);
    return 0;
  }
  result = lws_write(wsi, &slot->storage.bytes[LWS_PRE], slot->length,
                     slot->kind == XV_TX_BINARY ? LWS_WRITE_BINARY
                                                : LWS_WRITE_TEXT);
  if (result == (int)slot->length) {
    if (control) {
      control_pop_locked(adapter);
    } else {
      pcm_pop_locked(adapter);
    }
  } else {
    pthread_mutex_unlock(&adapter->lock);
    return -1;
  }
  (void)maybe_enqueue_stop_locked(adapter);
  sendable =
      (adapter->control_count > 0U &&
       (!control_slot(adapter, 0U)->after_pcm ||
        adapter->pcm_count == 0U)) ||
      (adapter->pcm_count > 0U && adapter->turn_started);
  if (sendable) {
    lws_callback_on_writable(wsi);
  }
  pthread_mutex_unlock(&adapter->lock);
  return result < 0 ? -1 : 0;
}

static int network_lws_callback(struct lws *wsi,
                                enum lws_callback_reasons reason,
                                void *user, void *input, size_t length) {
  xv_network_adapter_t *adapter =
      (xv_network_adapter_t *)lws_context_user(lws_get_context(wsi));
  (void)user;
  if (adapter == NULL) {
    return 0;
  }

  switch (reason) {
  case LWS_CALLBACK_CLIENT_ESTABLISHED:
    {
    bool queued;
    uint64_t established;
    pthread_mutex_lock(&adapter->lock);
    adapter->wsi = wsi;
    adapter->connecting = false;
    adapter->connected = true;
    adapter->session_ready = false;
    adapter->ping_outstanding = false;
    adapter->keepalive_closing = false;
    adapter->ping_deadline_ms = 0U;
    adapter->next_ping_ms = network_monotonic_ms() +
                            XV_NETWORK_PING_INTERVAL_MS;
    adapter->turn_started = false;
    adapter->connect_failures = 0U;
    adapter->sessions_established += 1U;
    established = adapter->sessions_established;
    adapter->rx_pause_requested = false;
    adapter->rx_paused = false;
    queued = enqueue_session_start_locked(adapter);
    pthread_mutex_unlock(&adapter->lock);
    printf("xiaov-network: session established total=%llu\n",
           (unsigned long long)established);
    if (!queued) {
      emit_event(adapter, XV_APP_EVENT_FAILURE);
      return -1;
    }
    lws_callback_on_writable(wsi);
    break;
    }

  case LWS_CALLBACK_CLIENT_WRITEABLE:
    return send_front(adapter, wsi);

  case LWS_CALLBACK_CLIENT_RECEIVE:
    handle_receive_fragment(adapter, wsi, input, length);
    break;

  case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
  case LWS_CALLBACK_CLIENT_CLOSED:
    {
    bool active_turn;
    bool active_music;
    bool was_ready;
    bool stopping;
    uint64_t disconnects;
    uint32_t retry_ms = 0U;
    pthread_mutex_lock(&adapter->lock);
    active_turn = adapter->want_turn || adapter->tts_active;
    active_music = adapter->music_active;
    was_ready = adapter->session_ready;
    stopping = adapter->stop_requested;
    adapter->wsi = NULL;
    adapter->connecting = false;
    adapter->connected = false;
    adapter->session_ready = false;
    adapter->turn_started = false;
    adapter->rx_pause_requested = false;
    adapter->rx_paused = false;
    adapter->video_assembling = false;
    adapter->ping_outstanding = false;
    adapter->keepalive_closing = false;
    adapter->ping_deadline_ms = 0U;
    adapter->next_ping_ms = 0U;
    adapter->disconnects += 1U;
    disconnects = adapter->disconnects;
    if (!stopping) {
      if (was_ready) {
        adapter->reconnect_failures = 0U;
      }
      schedule_reconnect_locked(adapter, network_monotonic_ms());
      retry_ms = adapter->reconnect_backoff_ms;
    }
    queue_reset_locked(adapter);
    pthread_mutex_unlock(&adapter->lock);
    printf("xiaov-network: session closed callback=%d total=%llu active=%u "
           "retry_ms=%u\n",
           (int)reason, (unsigned long long)disconnects,
           active_turn ? 1U : 0U, (unsigned int)retry_ms);
    if (active_turn && !stopping) {
      /* A transient Wi-Fi / websocket break is not a device fault. Every
       * active state handles CANCEL by releasing audio focus and returning to
       * standby; FAILURE strands the persistent daemon in ERROR until reset. */
      printf("xiaov-network: connection lost during active turn; cancelling\n");
      emit_event(adapter, XV_APP_EVENT_CANCEL);
    }
    if (active_music && !stopping && adapter->callbacks.finish_music != NULL) {
      adapter->callbacks.finish_music(adapter->callbacks.context, 0U);
    }
    break;
    }

  default:
    break;
  }
  return 0;
}

static bool connect_client(xv_network_adapter_t *adapter) {
  struct lws_client_connect_info info;
  struct lws *wsi;

  memset(&info, 0, sizeof(info));
  info.context = adapter->lws_context;
  info.address = adapter->host;
  info.port = adapter->port;
  info.path = adapter->path;
  info.host = adapter->host;
  info.origin = adapter->host;
  info.local_protocol_name = g_protocols[0].name;
  info.ssl_connection = adapter->use_tls ? LCCSCF_USE_SSL : 0;
  info.retry_and_idle_policy = &g_connection_policy;

  pthread_mutex_lock(&adapter->lock);
  adapter->connecting = true;
  adapter->connect_attempts += 1U;
  pthread_mutex_unlock(&adapter->lock);
  wsi = lws_client_connect_via_info(&info);
  if (wsi == NULL) {
    pthread_mutex_lock(&adapter->lock);
    adapter->connecting = false;
    pthread_mutex_unlock(&adapter->lock);
    return false;
  }
  return true;
}

static void *network_worker(void *argument) {
  xv_network_adapter_t *adapter = argument;
  struct lws_context_creation_info info;
  struct lws_context *context;

  memset(&info, 0, sizeof(info));
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.protocols = g_protocols;
  /* Only ask lws to bring up its TLS stack when this session actually needs it.
     Setting it unconditionally made a plaintext ws:// run fail on DShanPi: lws
     looked for client CA certs under /etc/ssl/curl, which does not exist on a
     read-only romfs /etc, and tore down the default vhost 42 ms after context
     creation. No TCP connect was ever attempted even though ICMP worked. */
  info.options = adapter->use_tls ? LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT : 0;
  if (adapter->use_tls) {
    /* lws defaults to /etc/ssl/curl, but on DShanPi /etc is a read-only romfs
       without certs; the CA bundle ships in the resource partition instead. */
    info.client_ssl_ca_filepath = CONFIG_EXAMPLES_XIAOV_CA_BUNDLE_PATH;
  }
  info.user = adapter;
  /* cancel pipe + active socket already consume three descriptors.  Leave
   * room for a reconnect socket and resolver / TLS internals instead of
   * depending on every old descriptor disappearing in the same service pass. */
  info.fd_limit_per_thread = XV_NETWORK_FD_LIMIT_PER_THREAD;
  info.connect_timeout_secs = XV_NETWORK_CONNECT_TIMEOUT_SECONDS;

  context = lws_create_context(&info);
  if (context == NULL) {
    emit_event(adapter, XV_APP_EVENT_FAILURE);
    return NULL;
  }
  pthread_mutex_lock(&adapter->lock);
  adapter->lws_context = context;
  pthread_mutex_unlock(&adapter->lock);

  for (;;) {
    bool stop_requested;
    bool should_connect;
    bool should_write;
    bool change_rx_flow;
    bool pause_rx;
    bool enqueue_ping = false;
    bool expire_connection = false;
    uint64_t now_ms;
    struct lws *wsi;

    now_ms = network_monotonic_ms();
    pthread_mutex_lock(&adapter->lock);
    stop_requested = adapter->stop_requested;
    should_connect = !adapter->connected && !adapter->connecting &&
                     now_ms >= adapter->next_connect_ms;
    should_write = adapter->connected &&
                   (adapter->control_count > 0U ||
                    adapter->pcm_count > 0U);
    wsi = adapter->wsi;
    change_rx_flow = adapter->connected && wsi != NULL &&
                     adapter->rx_pause_requested != adapter->rx_paused;
    pause_rx = adapter->rx_pause_requested;
    if (adapter->connected && adapter->session_ready &&
        !adapter->keepalive_closing) {
      if (adapter->ping_outstanding && now_ms >= adapter->ping_deadline_ms) {
        adapter->keepalive_closing = true;
        adapter->ping_timeouts += 1U;
        expire_connection = wsi != NULL;
      } else if (!adapter->ping_outstanding &&
                 now_ms >= adapter->next_ping_ms) {
        enqueue_ping = enqueue_ping_locked(adapter, now_ms);
      }
    }
    if (change_rx_flow) {
      adapter->rx_paused = pause_rx;
    }
    pthread_mutex_unlock(&adapter->lock);

    if (stop_requested) {
      break;
    }
    if (enqueue_ping && wsi != NULL) {
      lws_callback_on_writable(wsi);
    }
    if (expire_connection) {
      printf("xiaov-network: pong timeout; closing stale session\n");
      lws_set_timeout(wsi, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_ASYNC);
    }
    if (should_connect && !connect_client(adapter)) {
      bool report_failure = false;
      pthread_mutex_lock(&adapter->lock);
      if (adapter->connect_failures < UINT8_MAX) {
        adapter->connect_failures += 1U;
      }
      if (adapter->want_turn &&
          adapter->connect_failures == XV_NETWORK_MAX_CONNECT_FAILURES) {
        report_failure = true;
      }
      schedule_reconnect_locked(adapter, now_ms);
      pthread_mutex_unlock(&adapter->lock);
      if (report_failure) {
        emit_event(adapter, XV_APP_EVENT_FAILURE);
      }
      continue;
    }
    if (should_write && wsi != NULL) {
      lws_callback_on_writable(wsi);
    }
    if (change_rx_flow) {
      lws_rx_flow_control(wsi, pause_rx ? 0 : 1);
    }
    if (lws_service(context, 100) < 0) {
      pthread_mutex_lock(&adapter->lock);
      adapter->service_errors += 1U;
      pthread_mutex_unlock(&adapter->lock);
      usleep(XV_NETWORK_RETRY_DELAY_US);
    }
  }

  pthread_mutex_lock(&adapter->lock);
  adapter->lws_context = NULL;
  adapter->wsi = NULL;
  adapter->connected = false;
  adapter->connecting = false;
  pthread_mutex_unlock(&adapter->lock);
  lws_context_destroy(context);
  return NULL;
}

xv_network_adapter_t *xv_network_init(
    xv_network_adapter_storage_t *storage,
    const xv_network_config_t *config,
    const xv_network_callbacks_t *callbacks) {
  xv_network_adapter_t *adapter;
  if (storage == NULL || config == NULL || callbacks == NULL ||
      config->port == 0U || callbacks->emit_event == NULL ||
      callbacks->accept_tts_pcm == NULL ||
      callbacks->accept_music_pcm == NULL ||
      callbacks->finish_music == NULL) {
    return NULL;
  }

  adapter = (xv_network_adapter_t *)storage->bytes;
  memset(adapter, 0, sizeof(*adapter));
  if (!copy_string(adapter->host, sizeof(adapter->host), config->host, false) ||
      !copy_string(adapter->path, sizeof(adapter->path), config->path, false) ||
      !copy_string(adapter->device_id, sizeof(adapter->device_id),
                   config->device_id, false) ||
      (config->token != NULL &&
       !copy_string(adapter->token, sizeof(adapter->token), config->token,
                    true))) {
    return NULL;
  }
  adapter->callbacks = *callbacks;
  adapter->port = config->port;
  adapter->use_tls = config->use_tls;
  if (pthread_mutex_init(&adapter->lock, NULL) != 0) {
    return NULL;
  }
  return adapter;
}

void xv_network_deinit(xv_network_adapter_t *adapter) {
  if (adapter == NULL) {
    return;
  }
  xv_network_stop(adapter);
  pthread_mutex_destroy(&adapter->lock);
  memset(adapter, 0, sizeof(*adapter));
}

bool xv_network_start(xv_network_adapter_t *adapter) {
  pthread_attr_t attributes;
  int result;
  if (adapter == NULL || adapter->thread_started) {
    return adapter != NULL && adapter->thread_started;
  }
  adapter->stop_requested = false;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, XV_NETWORK_WORKER_STACK);
  result = pthread_create(&adapter->thread, &attributes, network_worker, adapter);
  pthread_attr_destroy(&attributes);
  if (result != 0) {
    return false;
  }
  adapter->thread_started = true;
  return true;
}

void xv_network_stop(xv_network_adapter_t *adapter) {
  if (adapter == NULL || !adapter->thread_started) {
    return;
  }
  pthread_mutex_lock(&adapter->lock);
  adapter->stop_requested = true;
  pthread_mutex_unlock(&adapter->lock);
  network_wake(adapter);
  pthread_join(adapter->thread, NULL);
  adapter->thread_started = false;
}

static void reserve_turn_locked(xv_network_adapter_t *adapter) {
  if (adapter->want_turn) {
    return;
  }
  adapter->turn_sequence += 1U;
  adapter->stream_id += 1U;
  adapter->pcm_sequence = 0U;
  adapter->end_requested = false;
  adapter->capture_drained = false;
  adapter->stop_enqueued = false;
  adapter->want_turn = true;
  snprintf(adapter->turn_id, sizeof(adapter->turn_id), "turn-%08lu",
           (unsigned long)adapter->turn_sequence);
}

bool xv_network_ensure_session(void *context) {
  xv_network_adapter_t *adapter = context;
  bool result = true;
  if (adapter == NULL || !adapter->thread_started) {
    return false;
  }

  pthread_mutex_lock(&adapter->lock);
  if (adapter->stop_requested) {
    pthread_mutex_unlock(&adapter->lock);
    return false;
  }
  reserve_turn_locked(adapter);
  if (adapter->session_ready && !adapter->turn_started) {
    result = enqueue_listen_start_locked(adapter);
  }
  pthread_mutex_unlock(&adapter->lock);
  network_wake(adapter);
  return result;
}

bool xv_network_ensure_ready_session(void *context) {
  xv_network_adapter_t *adapter = context;
  bool result = true;
  if (adapter == NULL || !adapter->thread_started) {
    return false;
  }

  pthread_mutex_lock(&adapter->lock);
  if (adapter->stop_requested || !adapter->connected ||
      !adapter->session_ready) {
    pthread_mutex_unlock(&adapter->lock);
    return false;
  }
  reserve_turn_locked(adapter);
  if (!adapter->turn_started) {
    result = enqueue_listen_start_locked(adapter);
  }
  pthread_mutex_unlock(&adapter->lock);
  if (result) {
    network_wake(adapter);
  }
  return result;
}

bool xv_network_request_audio_end(void *context) {
  xv_network_adapter_t *adapter = context;
  bool result;
  if (adapter == NULL || !adapter->thread_started) {
    return false;
  }
  pthread_mutex_lock(&adapter->lock);
  if (adapter->stop_requested) {
    pthread_mutex_unlock(&adapter->lock);
    return false;
  }
  adapter->end_requested = true;
  result = maybe_enqueue_stop_locked(adapter);
  pthread_mutex_unlock(&adapter->lock);
  if (result) {
    network_wake(adapter);
  }
  return result;
}

bool xv_network_cancel_turn(void *context) {
  xv_network_adapter_t *adapter = context;
  bool result = true;
  if (adapter == NULL || !adapter->thread_started) {
    return false;
  }
  pthread_mutex_lock(&adapter->lock);
  if (adapter->stop_requested) {
    pthread_mutex_unlock(&adapter->lock);
    return false;
  }
  queue_reset_locked(adapter);
  if (adapter->session_ready && adapter->turn_id[0] != '\0') {
    result = enqueue_cancel_locked(adapter);
  }
  adapter->want_turn = false;
  adapter->turn_started = false;
  adapter->end_requested = false;
  adapter->capture_drained = false;
  adapter->stop_enqueued = false;
  adapter->tts_active = false;
  adapter->music_active = false;
  pthread_mutex_unlock(&adapter->lock);
  network_wake(adapter);
  return result;
}

bool xv_network_send_media_result(xv_network_adapter_t *adapter,
                                  const char *command_id, bool ok,
                                  const char *error_code) {
  char event_id[24];
  char json[XV_NETWORK_TX_DATA_MAX];
  size_t length = 0U;
  xv_control_result_t encoded;
  bool accepted = false;

  if (adapter == NULL || command_id == NULL || !adapter->thread_started) {
    return false;
  }
  pthread_mutex_lock(&adapter->lock);
  if (!adapter->stop_requested) {
    next_event_id_locked(adapter, event_id, sizeof(event_id));
    encoded = xv_control_encode_media_result(
        json, sizeof(json), event_id, network_monotonic_ms(), command_id, ok,
        error_code, &length);
    accepted = encoded == XV_CONTROL_OK &&
               control_push_locked(adapter, json, length, false);
  }
  pthread_mutex_unlock(&adapter->lock);
  if (accepted) {
    network_wake(adapter);
  }
  return accepted;
}

bool xv_network_send_media_ready(xv_network_adapter_t *adapter,
                                 const char *command_id, bool ok,
                                 const char *error_code) {
  char event_id[24];
  char json[XV_NETWORK_TX_DATA_MAX];
  size_t length = 0U;
  xv_control_result_t encoded;
  bool accepted = false;

  if (adapter == NULL || command_id == NULL || !adapter->thread_started) {
    return false;
  }
  pthread_mutex_lock(&adapter->lock);
  if (!adapter->stop_requested) {
    next_event_id_locked(adapter, event_id, sizeof(event_id));
    encoded = xv_control_encode_media_ready(
        json, sizeof(json), event_id, network_monotonic_ms(), command_id, ok,
        error_code, &length);
    accepted = encoded == XV_CONTROL_OK &&
               control_push_locked(adapter, json, length, false);
  }
  pthread_mutex_unlock(&adapter->lock);
  if (accepted) {
    network_wake(adapter);
  }
  return accepted;
}

bool xv_network_begin_music_stream(xv_ws_adapter_t *adapter,
                                   uint32_t stream_id) {
  bool accepted = false;

  if (adapter == NULL || !adapter->thread_started) {
    return false;
  }
  pthread_mutex_lock(&adapter->lock);
  if (!adapter->stop_requested && adapter->session_ready &&
      !adapter->music_active && stream_id != 0U) {
    adapter->music_active = true;
    adapter->music_stream_id = stream_id;
    adapter->music_sequence = 0U;
    adapter->music_samples = 0U;
    accepted = true;
  }
  pthread_mutex_unlock(&adapter->lock);
  return accepted;
}

void xv_network_stop_music_stream(xv_ws_adapter_t *adapter) {
  if (adapter == NULL) {
    return;
  }
  pthread_mutex_lock(&adapter->lock);
  adapter->music_active = false;
  pthread_mutex_unlock(&adapter->lock);
}

bool xv_network_send_display_command(xv_network_adapter_t *adapter,
                                     const char *action,
                                     const char *source) {
  char event_id[24];
  char json[XV_NETWORK_TX_DATA_MAX];
  size_t length = 0U;
  xv_control_result_t encoded;
  bool accepted = false;

  if (adapter == NULL || action == NULL || !adapter->thread_started) {
    return false;
  }
  pthread_mutex_lock(&adapter->lock);
  if (!adapter->stop_requested && adapter->session_ready) {
    next_event_id_locked(adapter, event_id, sizeof(event_id));
    encoded = xv_control_encode_display_command(
        json, sizeof(json), event_id, network_monotonic_ms(), action, source,
        &length);
    accepted = encoded == XV_CONTROL_OK &&
               control_push_locked(adapter, json, length, false);
  }
  pthread_mutex_unlock(&adapter->lock);
  if (accepted) {
    network_wake(adapter);
  }
  return accepted;
}

bool xv_network_send_pcm(xv_network_adapter_t *adapter,
                         const int16_t *samples, size_t sample_count,
                         uint32_t timestamp_ms) {
  uint8_t frame[XV_AUDIO_HEADER_SIZE + XV_NETWORK_FRAME_SAMPLES *
                                           sizeof(int16_t)];
  xv_audio_header_t header;
  bool result;

  if (adapter == NULL || samples == NULL ||
      sample_count != XV_NETWORK_FRAME_SAMPLES) {
    return false;
  }
  pthread_mutex_lock(&adapter->lock);
  if (adapter->stop_requested || !adapter->want_turn ||
      adapter->capture_drained) {
    pthread_mutex_unlock(&adapter->lock);
    return false;
  }
  memset(&header, 0, sizeof(header));
  header.kind = XV_AUDIO_KIND_MICROPHONE;
  header.stream_id = adapter->stream_id;
  header.sequence = adapter->pcm_sequence;
  header.timestamp_ms = timestamp_ms;
  header.payload_length = sample_count * sizeof(int16_t);
  result = xv_audio_header_encode(frame, sizeof(frame), &header);
  if (result) {
    memcpy(&frame[XV_AUDIO_HEADER_SIZE], samples, header.payload_length);
    result = pcm_push_locked(adapter, frame, sizeof(frame));
  }
  if (result) {
    adapter->pcm_sequence += 1U;
  }
  pthread_mutex_unlock(&adapter->lock);
  if (result) {
    network_wake(adapter);
  }
  return result;
}

void xv_network_capture_drained(xv_network_adapter_t *adapter) {
  bool result;
  if (adapter == NULL) {
    return;
  }
  pthread_mutex_lock(&adapter->lock);
  if (adapter->stop_requested) {
    pthread_mutex_unlock(&adapter->lock);
    return;
  }
  adapter->capture_drained = true;
  result = maybe_enqueue_stop_locked(adapter);
  pthread_mutex_unlock(&adapter->lock);
  if (result) {
    network_wake(adapter);
  } else {
    emit_event(adapter, XV_APP_EVENT_FAILURE);
  }
}

size_t xv_network_dropped_frames(const xv_network_adapter_t *adapter) {
  size_t count;
  if (adapter == NULL) {
    return 0U;
  }
  pthread_mutex_lock((pthread_mutex_t *)&adapter->lock);
  count = adapter->dropped_frames;
  pthread_mutex_unlock((pthread_mutex_t *)&adapter->lock);
  return count;
}

bool xv_network_is_connected(const xv_network_adapter_t *adapter) {
  bool connected;
  if (adapter == NULL) {
    return false;
  }
  pthread_mutex_lock((pthread_mutex_t *)&adapter->lock);
  connected = adapter->connected && adapter->session_ready;
  pthread_mutex_unlock((pthread_mutex_t *)&adapter->lock);
  return connected;
}

void xv_network_get_stats(const xv_network_adapter_t *adapter,
                          xv_network_stats_t *stats) {
  if (stats == NULL) {
    return;
  }
  memset(stats, 0, sizeof(*stats));
  if (adapter == NULL) {
    return;
  }
  pthread_mutex_lock((pthread_mutex_t *)&adapter->lock);
  stats->connect_attempts = adapter->connect_attempts;
  stats->sessions_established = adapter->sessions_established;
  stats->disconnects = adapter->disconnects;
  stats->service_errors = adapter->service_errors;
  stats->pings_sent = adapter->pings_sent;
  stats->pongs_received = adapter->pongs_received;
  stats->ping_timeouts = adapter->ping_timeouts;
  stats->auth_rejections = adapter->auth_rejections;
  stats->reconnect_backoff_ms = adapter->reconnect_backoff_ms;
  pthread_mutex_unlock((pthread_mutex_t *)&adapter->lock);
}

bool xv_network_set_rx_paused(xv_network_adapter_t *adapter, bool paused) {
  if (adapter == NULL) {
    return false;
  }
  pthread_mutex_lock(&adapter->lock);
  if (adapter->stop_requested) {
    pthread_mutex_unlock(&adapter->lock);
    return false;
  }
  adapter->rx_pause_requested = paused;
  pthread_mutex_unlock(&adapter->lock);
  network_wake(adapter);
  return true;
}

bool xv_network_set_touch_capable(xv_network_adapter_t *adapter,
                                  bool capable) {
  if (adapter == NULL) {
    return false;
  }
  pthread_mutex_lock(&adapter->lock);
  if (adapter->thread_started || adapter->connected || adapter->connecting) {
    pthread_mutex_unlock(&adapter->lock);
    return false;
  }
  adapter->touch_capable = capable;
  pthread_mutex_unlock(&adapter->lock);
  return true;
}
