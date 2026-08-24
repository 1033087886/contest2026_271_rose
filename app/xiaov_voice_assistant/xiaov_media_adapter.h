/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XIAOV_OPENVELA_MEDIA_ADAPTER_H
#define XIAOV_OPENVELA_MEDIA_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XV_MEDIA_SAMPLE_RATE_HZ 16000U
#define XV_MEDIA_CHANNEL_COUNT 1U
#define XV_MEDIA_BITS_PER_SAMPLE 16U
#define XV_MEDIA_FRAME_DURATION_MS 20U
#define XV_MEDIA_FRAME_SAMPLES 320U
#define XV_MEDIA_FRAME_BYTES (XV_MEDIA_FRAME_SAMPLES * sizeof(int16_t))

#ifdef CONFIG_EXAMPLES_XIAOV_PLAYBACK_QUEUE_FRAMES
#define XV_MEDIA_PLAYBACK_QUEUE_FRAMES \
  CONFIG_EXAMPLES_XIAOV_PLAYBACK_QUEUE_FRAMES
#else
#define XV_MEDIA_PLAYBACK_QUEUE_FRAMES 64U
#endif

typedef enum {
  XV_MEDIA_EVENT_CAPTURE_STARTED = 0,
  XV_MEDIA_EVENT_CAPTURE_STOPPED,
  XV_MEDIA_EVENT_PLAYBACK_STARTED,
  XV_MEDIA_EVENT_PLAYBACK_STOPPED,
  XV_MEDIA_EVENT_PLAYBACK_DRAINED,
  XV_MEDIA_EVENT_PLAYBACK_SPACE_AVAILABLE,
  XV_MEDIA_EVENT_CAPTURE_ERROR,
  XV_MEDIA_EVENT_PLAYBACK_ERROR
} xv_media_event_t;

/* Called by the capture worker for each complete 20 ms PCM frame. The
 * callback must not block. Returning false records a dropped frame. */
typedef bool (*xv_media_capture_sink_fn)(void *context,
                                         const int16_t *samples,
                                         size_t sample_count,
                                         uint32_t timestamp_ms);

/* Runs on one of the media workers. Applications should only enqueue a
 * lightweight event here; runtime dispatch and UI work belong elsewhere. */
typedef void (*xv_media_event_fn)(void *context, xv_media_event_t event,
                                  int result);

typedef struct {
  void *context;
  xv_media_capture_sink_fn capture_sink;
  xv_media_event_fn emit_event;
} xv_media_callbacks_t;

typedef struct xv_media_adapter xv_media_adapter_t;

/* The adapter and all PCM queues live in caller-owned static storage. Media
 * Framework and pthread may still allocate their own internal resources. */
#define XV_MEDIA_ADAPTER_STORAGE_SIZE 65536U

typedef union {
  uint64_t alignment;
  uint8_t bytes[XV_MEDIA_ADAPTER_STORAGE_SIZE];
} xv_media_adapter_storage_t;

xv_media_adapter_t *xv_media_init(xv_media_adapter_storage_t *storage,
                                  const xv_media_callbacks_t *callbacks);
void xv_media_deinit(xv_media_adapter_t *adapter);

/* Runtime audio-adapter callbacks. These functions only update bounded state
 * and wake a worker; they never open, close, read, or write a media handle. */
bool xv_media_start_capture(void *context);
bool xv_media_stop_capture(void *context);
/* Stop capture and wait until the worker has closed the backend handle. This
 * is required before handing an exclusive audio device to another player. */
bool xv_media_stop_capture_and_wait(xv_media_adapter_t *adapter,
                                    uint32_t timeout_ms);
bool xv_media_start_playback(void *context);
bool xv_media_stop_playback(void *context);

/* Queue one PCM chunk for playback. A chunk may contain at most one 20 ms
 * frame. It may be queued before start_playback is dispatched. */
bool xv_media_enqueue_playback(xv_media_adapter_t *adapter,
                               const int16_t *samples,
                               size_t sample_count);

/* Marks the current TTS stream complete. PLAYBACK_DRAINED is emitted after
 * all previously accepted PCM has been written to Media Framework. */
bool xv_media_finish_playback(xv_media_adapter_t *adapter);

/* Drops queued PCM without changing whether playback is requested. */
void xv_media_clear_playback(xv_media_adapter_t *adapter);

size_t xv_media_playback_queued(const xv_media_adapter_t *adapter);
uint64_t xv_media_capture_dropped(const xv_media_adapter_t *adapter);
uint64_t xv_media_playback_rejected(const xv_media_adapter_t *adapter);
bool xv_media_capture_active(const xv_media_adapter_t *adapter);
bool xv_media_playback_active(const xv_media_adapter_t *adapter);

#ifdef __cplusplus
}
#endif

#endif
