/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "xiaov_service.h"

#include "xiaov/audio_visualizer.h"
#include "xiaov/event_queue.h"
#ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
#  include "xiaov/offline_command.h"
#endif
#include "xiaov/reminder_queue.h"
#include "xiaov/ring_buffer.h"
#include "xiaov/runtime.h"
#include "xiaov/vad.h"
#include "xiaov_protocol.h"
#include "xiaov_media_adapter.h"
#include "xiaov_music_adapter.h"
#include "xiaov_network_adapter.h"
#include "xiaov_power_adapter.h"
#include "xiaov_ui.h"
#if defined(CONFIG_EXAMPLES_XIAOV_KWS) && \
    defined(CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA)
#  include "xiaov_alsa_backend.h"
#  include "xiaov_local_wav.h"
#endif
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
#  include "xiaov_capture_gain.h"
#  include "xiaov_kws_frontend.h"
#  include "xiaov_kws_model.h"
#  include "xiaov_tflm_kws_adapter.h"
#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
#    include "xiaov_command_model.h"
#  endif
#endif

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define XV_SERVICE_PCM_SAMPLES 32000U
#define XV_SERVICE_EVENT_CAPACITY 32U
#define XV_SERVICE_REMINDER_CAPACITY 8U

/* End-of-speech needs a hangover. Ending the turn on the first silent frame
 * after speech cuts a talker off at the pause between words: a continuous 30 s
 * tone played into the microphone produced a 580 ms capture. Wait for a run of
 * consecutive silent frames instead, so intra-utterance pauses survive. */
#ifndef CONFIG_EXAMPLES_XIAOV_CAPTURE_HANGOVER_MS
#  define CONFIG_EXAMPLES_XIAOV_CAPTURE_HANGOVER_MS 2000
#endif
#define XV_SERVICE_CAPTURE_HANGOVER_FRAMES                             \
  ((CONFIG_EXAMPLES_XIAOV_CAPTURE_HANGOVER_MS +                        \
    XV_MEDIA_FRAME_DURATION_MS - 1U) /                                 \
   XV_MEDIA_FRAME_DURATION_MS)

#ifndef CONFIG_EXAMPLES_XIAOV_MAX_CAPTURE_MS
#  define CONFIG_EXAMPLES_XIAOV_MAX_CAPTURE_MS 30000
#endif
#define XV_SERVICE_MAX_CAPTURE_FRAMES                                  \
  ((CONFIG_EXAMPLES_XIAOV_MAX_CAPTURE_MS +                             \
    XV_MEDIA_FRAME_DURATION_MS - 1U) /                                 \
   XV_MEDIA_FRAME_DURATION_MS)

#ifndef CONFIG_EXAMPLES_XIAOV_TOKEN_PATH
/* /etc is a read-only romfs on DShanPi, so a token written there fails with
   EACCES. /data is the yaffs partition and is writable. */
#define CONFIG_EXAMPLES_XIAOV_TOKEN_PATH "/data/xiaov.token"
#endif

#ifndef CONFIG_EXAMPLES_XIAOV_MUSIC_DEFAULT_SOURCE
#define CONFIG_EXAMPLES_XIAOV_MUSIC_DEFAULT_SOURCE "/data/xiaov-demo.wav"
#endif

#ifndef CONFIG_EXAMPLES_XIAOV_WAKE_PROMPT_PATH
#define CONFIG_EXAMPLES_XIAOV_WAKE_PROMPT_PATH "/data/xiaov-wake-ack.wav"
#endif

#ifndef CONFIG_EXAMPLES_XIAOV_HEARTBEAT_MS
#define CONFIG_EXAMPLES_XIAOV_HEARTBEAT_MS 30000
#endif

#ifndef CONFIG_EXAMPLES_XIAOV_TURN_TIMEOUT_MS
#define CONFIG_EXAMPLES_XIAOV_TURN_TIMEOUT_MS 60000
#endif

#ifndef CONFIG_EXAMPLES_XIAOV_CONNECT_TIMEOUT_MS
#define CONFIG_EXAMPLES_XIAOV_CONNECT_TIMEOUT_MS 10000
#endif

#define XV_SERVICE_CONNECT_POLL_MS 50U
#define XV_SERVICE_MUSIC_CAPTURE_STOP_TIMEOUT_MS 2000U

#ifndef CONFIG_EXAMPLES_XIAOV_FRAMEBUFFER_DEVPATH
#define CONFIG_EXAMPLES_XIAOV_FRAMEBUFFER_DEVPATH "/dev/fb0"
#endif

#ifndef CONFIG_EXAMPLES_XIAOV_ACTIVE_BRIGHTNESS
#define CONFIG_EXAMPLES_XIAOV_ACTIVE_BRIGHTNESS 255
#endif

#ifndef CONFIG_EXAMPLES_XIAOV_STANDBY_BRIGHTNESS
#define CONFIG_EXAMPLES_XIAOV_STANDBY_BRIGHTNESS 16
#endif

#ifdef CONFIG_EXAMPLES_XIAOV_KWS
#  ifndef CONFIG_EXAMPLES_XIAOV_KWS_SCORE_THRESHOLD_Q15
#    define CONFIG_EXAMPLES_XIAOV_KWS_SCORE_THRESHOLD_Q15 28672
#  endif
#  ifndef CONFIG_EXAMPLES_XIAOV_KWS_CONSECUTIVE_HITS
#    define CONFIG_EXAMPLES_XIAOV_KWS_CONSECUTIVE_HITS 1
#  endif
#  ifndef CONFIG_EXAMPLES_XIAOV_KWS_COOLDOWN_MS
#    define CONFIG_EXAMPLES_XIAOV_KWS_COOLDOWN_MS 2000
#  endif
#  ifndef CONFIG_EXAMPLES_XIAOV_KWS_CAPTURE_TARGET_PEAK
#    define CONFIG_EXAMPLES_XIAOV_KWS_CAPTURE_TARGET_PEAK 20000
#  endif
#  ifndef CONFIG_EXAMPLES_XIAOV_KWS_CAPTURE_MAX_GAIN_Q8
#    define CONFIG_EXAMPLES_XIAOV_KWS_CAPTURE_MAX_GAIN_Q8 768
#  endif
#  ifndef CONFIG_EXAMPLES_XIAOV_KWS_VAD_HOLD_MS
#    define CONFIG_EXAMPLES_XIAOV_KWS_VAD_HOLD_MS 2500
#  endif
#  ifndef CONFIG_EXAMPLES_XIAOV_KWS_VAD_FALLBACK_FRAMES
#    define CONFIG_EXAMPLES_XIAOV_KWS_VAD_FALLBACK_FRAMES 110
#  endif
#  define XV_SERVICE_KWS_VAD_HOLD_FRAMES                              \
    ((CONFIG_EXAMPLES_XIAOV_KWS_VAD_HOLD_MS +                        \
      XV_MEDIA_FRAME_DURATION_MS - 1U) /                             \
     XV_MEDIA_FRAME_DURATION_MS)
/* Scored wake-model windows required before the VAD fallback may claim the
 * wake. Wake mode scores every second frontend window, so three verdicts cover
 * 1.2 seconds and give the single-hit rule a real opportunity before fallback.
 */
#  define XV_SERVICE_KWS_FALLBACK_MIN_INFERENCES 3U
/* The frontend materializes a window every 200 ms for command mode. Wake mode
 * compares each adjacent pair and enqueues the window with the greater feature
 * sum. That keeps the measured 377 ms Invoke on a sustainable 400 ms schedule
 * without blindly discarding the only strong window in a weak microphone
 * capture. Full production-path simulation over 447 clips gives 170/177 wake
 * recall and 4/270 false accepts at Q15=28672 with one required hit.
 */
/* Eight entries absorb scheduler jitter and route transitions without making
 * normal wake detection accumulate seconds of stale inference work.
 */
#  define XV_SERVICE_KWS_WINDOW_QUEUE_DEPTH 8U
#  define XV_SERVICE_KWS_WORKER_STACKSIZE 8192U
#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
#    ifndef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMAND_THRESHOLD_Q15
#      define CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMAND_THRESHOLD_Q15 27520
#    endif
#    ifndef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMAND_CONSECUTIVE_HITS
#      define CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMAND_CONSECUTIVE_HITS 2
#    endif
#    ifndef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMAND_WINDOW_MS
#      define CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMAND_WINDOW_MS 6000
#    endif
#    ifndef CONFIG_EXAMPLES_XIAOV_OFFLINE_VOLUME_STEP_PERCENT
#      define CONFIG_EXAMPLES_XIAOV_OFFLINE_VOLUME_STEP_PERCENT 10
#    endif
#    define XV_SERVICE_COMMAND_WINDOW_FRAMES                         \
      ((CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMAND_WINDOW_MS +           \
        XV_MEDIA_FRAME_DURATION_MS - 1U) /                          \
       XV_MEDIA_FRAME_DURATION_MS)
#    define XV_SERVICE_COMMAND_CANDIDATE_COUNT 3U
#  endif
#endif

#define XV_SERVICE_EVENT_WAKE_DETECTED 0x100U
#define XV_SERVICE_EVENT_OFFLINE_COMMAND 0x101U
#define XV_SERVICE_EVENT_OFFLINE_TIMEOUT 0x102U
#define XV_SERVICE_EVENT_MUSIC_IDLE 0x103U
#define XV_SERVICE_EVENT_OFFLINE_REQUEST 0x104U
#define XV_SERVICE_EVENT_MEDIA_PLAY_DEFERRED 0x105U
#define XV_SERVICE_EVENT_MUSIC_STREAM_DRAINED 0x106U

#define XV_SERVICE_EOS_REASON_EXTERNAL 0U
#define XV_SERVICE_EOS_REASON_SILENCE 1U
#define XV_SERVICE_EOS_REASON_MAX_DURATION 2U

typedef enum {
  XV_SERVICE_CAPTURE_IDLE = 0,
  XV_SERVICE_CAPTURE_KWS,
  XV_SERVICE_CAPTURE_COMMAND,
  XV_SERVICE_CAPTURE_VOICE
} xv_service_capture_route_t;

#ifdef CONFIG_EXAMPLES_XIAOV_KWS
typedef enum {
  XV_SERVICE_KWS_MODEL_WAKE = 0,
  XV_SERVICE_KWS_MODEL_COMMAND
} xv_service_kws_model_t;
#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
/* RC11 only executes commands represented by confirmed board-microphone
 * training turns. Other outputs remain in the model contract but cannot
 * trigger an action. */
static const uint16_t g_command_candidate_output_indices[] = {
    7U,  /* volume_up */
    12U, /* media_mute */
    13U, /* media_unmute */
};
_Static_assert(sizeof(g_command_candidate_output_indices) /
                       sizeof(g_command_candidate_output_indices[0]) ==
                   XV_SERVICE_COMMAND_CANDIDATE_COUNT,
               "command candidate count mismatch");
#  endif
#endif

typedef struct {
  pthread_mutex_t lock;
  sem_t event_wake;
  xv_runtime_t runtime;
  xv_pcm_ring_t pcm_ring;
  xv_event_queue_t event_queue;
  xv_vad_t vad;
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  xv_vad_t kws_vad;
  xv_kws_pcm_frontend_t kws_frontend;
#endif
  xv_media_adapter_storage_t media_storage;
  xv_music_adapter_storage_t music_storage;
  xv_network_adapter_storage_t network_storage;
  xv_power_adapter_storage_t power_storage;
  xv_ui_storage_t ui_storage;
  xv_media_adapter_t *media;
  xv_music_adapter_t *music;
  xv_ws_adapter_t *network;
  xv_power_adapter_t *power;
  xv_ui_t *ui;
  int16_t pcm_storage[XV_SERVICE_PCM_SAMPLES];
  int16_t capture_visualizer_samples[XV_AUDIO_VISUALIZER_FRAME_SAMPLES];
  int16_t tts_visualizer_samples[XV_AUDIO_VISUALIZER_FRAME_SAMPLES];
  xv_event_t event_storage[XV_SERVICE_EVENT_CAPACITY];
  xv_reminder_queue_t reminders;
  xv_reminder_t reminder_storage[XV_SERVICE_REMINDER_CAPACITY];
  uint32_t capture_frames;
  uint32_t silent_frames_since_speech;
  bool speech_seen;
  bool eos_posted;
  bool network_failure_posted;
  bool lock_initialized;
  bool event_wake_initialized;
  bool reminder_active;
  uint64_t media_commands_succeeded;
  uint64_t media_commands_failed;
  bool voice_focus_held;
  bool tts_focus_held;
  bool music_stream_playing;
  uint32_t music_stream_id;
  char music_stream_command_id[XV_CONTROL_COMMAND_ID_MAX];
  bool persistent;
  bool network_capture_pending;
  xv_service_capture_route_t capture_route;
  size_t capture_visualizer_fill;
  size_t tts_visualizer_fill;
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  bool kws_ready;
  xv_service_kws_model_t kws_model;
  bool kws_error_logged;
  /* Windows waiting to be scored, plus the thread that scores them. Guarded by
   * kws_queue_lock rather than the service lock: the capture thread must be able
   * to enqueue while the worker is inside a 377 ms Invoke, and the worker must
   * not hold the service lock across that Invoke either. */
  pthread_mutex_t kws_queue_lock;
  pthread_cond_t kws_queue_ready;
  pthread_t kws_worker;
  bool kws_queue_initialized;
  bool kws_worker_running;
  bool kws_worker_stop;
  xv_tflm_kws_window_t kws_windows[XV_SERVICE_KWS_WINDOW_QUEUE_DEPTH];
  size_t kws_queue_read;
  size_t kws_queue_count;
  bool kws_queue_gap;
  uint32_t kws_queue_generation;
  uint64_t kws_windows_queued;
  uint64_t kws_windows_dropped;
  uint32_t kws_queue_peak;
  xv_tflm_kws_window_t kws_wake_pending_window;
  bool kws_wake_pending_valid;
  uint32_t kws_vad_hold_frames;
  uint32_t kws_vad_fallback_speech_frames;
  uint32_t kws_wake_inferences;
  bool kws_wake_event_posted;
  uint64_t kws_vad_fallbacks;
  uint64_t kws_triggers_posted;
  bool music_capture_suspended;
  bool music_restore_pending;
  /* A voice tool call can ask for PLAY while the voice/TTS focus is still
   * held.  Keep one bounded request until the service thread has released
   * that focus; the network callback must acknowledge it immediately or the
   * gateway would wait for a result before it can finish the turn. */
  bool deferred_media_play_valid;
  char deferred_media_command_id[XV_CONTROL_COMMAND_ID_MAX];
  char deferred_media_source[XV_CONTROL_MEDIA_QUERY_MAX];
  uint64_t kws_triggers_rejected;
  uint64_t kws_processing_errors;
  bool wake_prompt_pending;
  uint64_t wake_prompts_played;
  uint64_t wake_prompts_failed;
#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
  uint32_t command_capture_frames;
  bool command_event_posted;
  uint8_t offline_pre_mute_volume;
  uint64_t offline_commands_executed;
  uint64_t offline_commands_cloud;
  uint64_t offline_commands_failed;
  uint64_t offline_command_timeouts;
#  endif
#endif
} xv_service_t;

#ifdef CONFIG_EXAMPLES_XIAOV_KWS
static bool service_start_standby_kws(xv_service_t *service);
static void service_flush_kws_queue(xv_service_t *service);
static void service_execute_deferred_media_play(xv_service_t *service);
static void service_discard_deferred_media_play(xv_service_t *service);
static const char *service_media_wire_error(xv_music_error_t error);
static bool service_parse_music_stream(const char *source,
                                       uint32_t *stream_id);
static bool service_start_network_music(xv_service_t *service,
                                        const char *command_id,
                                        uint32_t stream_id);
static void service_stop_network_music(xv_service_t *service);
#endif

static xv_service_t g_service;
static pthread_mutex_t g_service_owner_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_service_in_use;

static bool service_claim(void) {
  bool claimed = false;
  pthread_mutex_lock(&g_service_owner_lock);
  if (!g_service_in_use) {
    g_service_in_use = true;
    claimed = true;
  }
  pthread_mutex_unlock(&g_service_owner_lock);
  return claimed;
}

static void service_release(void) {
  pthread_mutex_lock(&g_service_owner_lock);
  g_service_in_use = false;
  pthread_mutex_unlock(&g_service_owner_lock);
}

static bool service_read_token(char *token, size_t token_size) {
  FILE *file;
  size_t length;

  token[0] = '\0';
  file = fopen(CONFIG_EXAMPLES_XIAOV_TOKEN_PATH, "r");
  if (file == NULL) {
    return false;
  }
  if (fgets(token, (int)token_size, file) == NULL) {
    fclose(file);
    token[0] = '\0';
    return false;
  }
  fclose(file);
  length = strlen(token);
  while (length > 0U &&
         (token[length - 1U] == '\r' || token[length - 1U] == '\n' ||
          token[length - 1U] == ' ' || token[length - 1U] == '\t')) {
    token[--length] = '\0';
  }
  return length > 0U;
}

static bool service_post_typed_event(xv_service_t *service, uint16_t type,
                                     uint32_t value) {
  xv_event_t event;
  bool accepted;
  uint64_t rejected;

  memset(&event, 0, sizeof(event));
  event.type = type;
  event.value = value;
  event.timestamp_ms = xv_runtime_monotonic_ms(&service->runtime);
  pthread_mutex_lock(&service->lock);
  accepted = xv_event_queue_push(&service->event_queue, &event);
  rejected = xv_event_queue_rejected(&service->event_queue);
  pthread_mutex_unlock(&service->lock);
  if (accepted) {
    sem_post(&service->event_wake);
  } else {
    printf("xiaov: service event queue overflow type=%u total=%llu\n",
           (unsigned int)type,
           (unsigned long long)rejected);
  }
  return accepted;
}

static bool service_post_event(xv_service_t *service,
                               xv_app_event_t app_event) {
  return service_post_typed_event(service, (uint16_t)app_event, 0U);
}

static void service_reset_visualizer(xv_service_t *service, bool tts) {
  pthread_mutex_lock(&service->lock);
  if (tts) {
    service->tts_visualizer_fill = 0U;
  } else {
    service->capture_visualizer_fill = 0U;
  }
  pthread_mutex_unlock(&service->lock);
}

static void service_visualize_pcm(xv_service_t *service,
                                  const int16_t *samples,
                                  size_t sample_count, bool tts) {
  int16_t frame[XV_AUDIO_VISUALIZER_FRAME_SAMPLES];

  while (sample_count > 0U) {
    int16_t *pending;
    size_t *fill;
    size_t copied;
    bool frame_ready = false;
    xv_audio_visualizer_result_t result;

    pthread_mutex_lock(&service->lock);
    pending = tts ? service->tts_visualizer_samples
                  : service->capture_visualizer_samples;
    fill = tts ? &service->tts_visualizer_fill
               : &service->capture_visualizer_fill;
    copied = XV_AUDIO_VISUALIZER_FRAME_SAMPLES - *fill;
    if (copied > sample_count) {
      copied = sample_count;
    }
    memcpy(pending + *fill, samples, copied * sizeof(*samples));
    *fill += copied;
    if (*fill == XV_AUDIO_VISUALIZER_FRAME_SAMPLES) {
      memcpy(frame, pending, sizeof(frame));
      *fill = 0U;
      frame_ready = true;
    }
    pthread_mutex_unlock(&service->lock);

    samples += copied;
    sample_count -= copied;
    if (frame_ready &&
        xv_audio_visualizer_process(frame,
                                    XV_AUDIO_VISUALIZER_FRAME_SAMPLES,
                                    &result)) {
      (void)xv_ui_update_audio_levels(service->ui, result.bands,
                                      result.energy);
    }
  }
}

#ifdef CONFIG_EXAMPLES_XIAOV_KWS
static const char *service_wake_label(uint16_t output_index) {
  size_t index;

  for (index = 0U; index < xv_kws_model_wake_class_count; ++index) {
    if (xv_kws_model_wake_class_indices[index] == output_index) {
      return xv_kws_model_wake_class_labels[index];
    }
  }
  return "unknown";
}

#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
static const char *service_command_label(uint16_t output_index) {
  if ((size_t)output_index >= xv_command_model_output_elements) {
    return "unknown";
  }
  return xv_command_model_labels[output_index];
}
#  endif

static int32_t service_kws_window_strength(
    const xv_tflm_kws_window_t *window) {
  int32_t sum = 0;
  size_t index;

  for (index = 0U; index < window->element_count; ++index) {
    sum += (int32_t)window->data[index];
  }
  return sum;
}
#endif

#ifdef CONFIG_EXAMPLES_XIAOV_KWS
/* Runs on the capture thread. Advances the frontend and hands finished windows
 * to the inference worker without ever invoking the model, so the thread that
 * drains the 80 ms ALSA ring stays at real-time speed.
 */
static void service_enqueue_kws_window(xv_service_t *service,
                                       const int16_t *samples,
                                       size_t sample_count) {
  static const xv_capture_gain_config_t gain_config = {
      (uint16_t)CONFIG_EXAMPLES_XIAOV_KWS_CAPTURE_TARGET_PEAK,
      (uint16_t)CONFIG_EXAMPLES_XIAOV_KWS_CAPTURE_MAX_GAIN_Q8,
  };
  int16_t conditioned_samples[XV_MEDIA_FRAME_SAMPLES];
  const int16_t *kws_samples = samples;
  xv_tflm_kws_window_t window;
  xv_tflm_kws_status_t status;
  bool inference_enabled;
  bool vad_active;
  bool wake_model;
  bool log_error = false;

  /* The digital microphone path is about 7-8 dB below the model's useful
   * range even with hardware gain at maximum. Select gain from each 20 ms
   * block's peak: quiet speech can use up to 3x, while already-loud blocks are
   * unchanged and therefore cannot be clipped by the preamp. Keep VAD on raw
   * PCM: applying this gain before VAD made 551/561 frames inference-enabled
   * on a board recording whose real speech occupies only 8.98-10.52 seconds.
   */
  if (sample_count <= XV_MEDIA_FRAME_SAMPLES &&
      xv_capture_gain_apply(&gain_config, samples, sample_count,
                            conditioned_samples)) {
    kws_samples = conditioned_samples;
  }

  pthread_mutex_lock(&service->lock);
  vad_active = xv_vad_process(&service->kws_vad, samples, sample_count);
  if (!vad_active) {
    service->kws_vad_fallback_speech_frames = 0U;
  } else if (service->kws_vad.speech_run > 0U &&
             service->kws_vad_fallback_speech_frames < UINT32_MAX) {
    service->kws_vad_fallback_speech_frames += 1U;
  }
  if (vad_active) {
    service->kws_vad_hold_frames = (uint32_t)XV_SERVICE_KWS_VAD_HOLD_FRAMES;
  } else if (service->kws_vad_hold_frames > 0U) {
    service->kws_vad_hold_frames -= 1U;
  }
  inference_enabled = vad_active || service->kws_vad_hold_frames > 0U;
  pthread_mutex_unlock(&service->lock);

  status = xv_tflm_kws_advance_frontend(
      kws_samples, sample_count, XV_KWS_FRONTEND_SAMPLE_RATE_HZ,
      xv_runtime_monotonic_ms(&service->runtime), inference_enabled, &window);
  if (status == XV_TFLM_KWS_STATUS_NEED_MORE_AUDIO) {
    return;
  }
  if (status != XV_TFLM_KWS_STATUS_OK) {
    pthread_mutex_lock(&service->lock);
    service->kws_processing_errors += 1U;
    if (!service->kws_error_logged) {
      service->kws_error_logged = true;
      log_error = true;
    }
    pthread_mutex_unlock(&service->lock);
    if (log_error) {
      printf("xiaov: KWS frontend error status=%d; touch remains available\n",
             (int)status);
    }
    return;
  }

  /* Wake inference takes 377 ms, so feeding every 200 ms window creates a
   * permanent deficit. Keep one adjacent window pending and enqueue the richer
   * of each pair. Comparing the already-quantized feature sums is cheap on the
   * capture thread and avoids the phase-sensitive failure of always choosing
   * the first window. Command mode retains every window and its two-hit rule.
   */
  pthread_mutex_lock(&service->lock);
  wake_model = service->kws_model == XV_SERVICE_KWS_MODEL_WAKE;
  pthread_mutex_unlock(&service->lock);

  pthread_mutex_lock(&service->kws_queue_lock);
  if (wake_model) {
    if (!service->kws_wake_pending_valid) {
      service->kws_wake_pending_window = window;
      service->kws_wake_pending_valid = true;
      pthread_mutex_unlock(&service->kws_queue_lock);
      return;
    }
    if ((!service->kws_wake_pending_window.inference_enabled &&
         window.inference_enabled) ||
        (service->kws_wake_pending_window.inference_enabled ==
             window.inference_enabled &&
         service_kws_window_strength(&window) >
             service_kws_window_strength(
                 &service->kws_wake_pending_window))) {
      service->kws_wake_pending_window = window;
    }
    window = service->kws_wake_pending_window;
    service->kws_wake_pending_valid = false;
  }
  if (service->kws_queue_count == XV_SERVICE_KWS_WINDOW_QUEUE_DEPTH) {
    /* Overflow means the worker fell far enough behind that the oldest window
     * is stale. Drop it and mark the gap so the decision rule cannot treat the
     * windows either side of the hole as consecutive.
     */
    service->kws_queue_read =
        (service->kws_queue_read + 1U) % XV_SERVICE_KWS_WINDOW_QUEUE_DEPTH;
    service->kws_queue_count -= 1U;
    service->kws_queue_gap = true;
    service->kws_windows_dropped += 1U;
  }
  service->kws_windows[(service->kws_queue_read + service->kws_queue_count) %
                       XV_SERVICE_KWS_WINDOW_QUEUE_DEPTH] = window;
  service->kws_queue_count += 1U;
  service->kws_windows_queued += 1U;
  if (service->kws_queue_count > service->kws_queue_peak) {
    service->kws_queue_peak = (uint32_t)service->kws_queue_count;
  }
  pthread_cond_signal(&service->kws_queue_ready);
  pthread_mutex_unlock(&service->kws_queue_lock);
}

/* Runs on the inference worker. Scores exactly one dequeued window and applies
 * the same trigger, fallback and command-timeout logic that used to live inline
 * in the capture callback. `contiguous` is false when the queue overflowed
 * between this window and the previous one.
 */
static void service_score_kws_window(xv_service_t *service,
                                     const xv_tflm_kws_window_t *dequeued,
                                     bool contiguous) {
  xv_service_capture_route_t capture_route;
  {
    xv_tflm_kws_result_t kws_result;
    xv_tflm_kws_status_t kws_status = XV_TFLM_KWS_STATUS_NEED_MORE_AUDIO;
    xv_service_kws_model_t kws_model = XV_SERVICE_KWS_MODEL_WAKE;
    const char *kws_label = NULL;
    uint16_t source_class = 0U;
    bool log_error = false;
    bool trigger = false;
    bool vad_fallback = false;
    uint32_t vad_fallback_speech_frames = 0U;
    bool timeout = false;
    const bool inference_enabled = dequeued->inference_enabled;

    /* Invoke happens outside the service lock. Holding it across 377 ms would
     * block the capture thread's VAD update and the heartbeat, which is the
     * starvation this whole split exists to remove.
     */
    kws_status = xv_tflm_kws_score_window(dequeued, contiguous, &kws_result);

#  ifdef CONFIG_EXAMPLES_XIAOV_KWS_TRACE_SCORES
    if (kws_status == XV_TFLM_KWS_STATUS_OK) {
      /* The score alone cannot separate "the features are wrong" from "the
       * model scores correct features as zero": the host scores 32639 on the
       * same audio, so one of the two must differ on device. Report the
       * feature window's spread alongside the score. A live speech window
       * reaches max around -33..+8 with std near 25; an all-silence window
       * stays pinned at INT8_MIN, which is what a stalled or never-filled
       * frontend looks like.
       */
      xv_tflm_kws_feature_stats_t trace_features;
      xv_tflm_kws_score_debug_t trace_score;

      xv_tflm_kws_get_feature_stats(&trace_features);
      xv_tflm_kws_get_score_debug(&trace_score);
      printf("xiaov: KWStrace t=%lu score_q15=%u index=%u decision=%d "
             "feat_min=%d feat_max=%d feat_mean_q4=%ld raw=%d "
             "r1=%d r2=%d ozp=%ld oscq16=%ld probq14=%ld dbg_score=%u\n",
             (unsigned long)dequeued->now_ms,
             (unsigned int)kws_result.score_q15,
             (unsigned int)kws_result.output_index, (int)kws_result.decision,
             (int)trace_features.minimum, (int)trace_features.maximum,
             (long)trace_features.mean_q4, (int)kws_result.raw_score,
             (int)trace_score.raw_class1, (int)trace_score.raw_class2,
             (long)trace_score.output_zero_point,
             (long)trace_score.output_scale_q16,
             (long)trace_score.best_probability_q14,
             (unsigned int)trace_score.score_q15);
    }
#  endif

    pthread_mutex_lock(&service->lock);
    capture_route = service->capture_route;
    if (capture_route == XV_SERVICE_CAPTURE_KWS ||
        capture_route == XV_SERVICE_CAPTURE_COMMAND) {
      kws_model = service->kws_model;
      /* Count only windows the wake model actually scored. STATUS_OK means the
       * frontend had a full context and inference ran; NEED_MORE_AUDIO and
       * GATED mean it did not, so those must not unlock the fallback.
       */
      if (kws_model == XV_SERVICE_KWS_MODEL_WAKE &&
          kws_status == XV_TFLM_KWS_STATUS_OK && inference_enabled &&
          service->kws_wake_inferences < UINT32_MAX) {
        service->kws_wake_inferences += 1U;
      }
      if (kws_status != XV_TFLM_KWS_STATUS_OK &&
          kws_status != XV_TFLM_KWS_STATUS_NEED_MORE_AUDIO &&
          kws_status != XV_TFLM_KWS_STATUS_GATED) {
        service->kws_processing_errors += 1U;
        if (!service->kws_error_logged) {
          service->kws_error_logged = true;
          log_error = true;
        }
      } else if (kws_status == XV_TFLM_KWS_STATUS_OK &&
                 kws_result.decision == XV_KWS_DECISION_TRIGGERED) {
        if (capture_route == XV_SERVICE_CAPTURE_KWS &&
            kws_model == XV_SERVICE_KWS_MODEL_WAKE &&
            !service->kws_wake_event_posted) {
          trigger = true;
          service->kws_wake_event_posted = true;
          kws_label = service_wake_label(kws_result.output_index);
          service->capture_route = XV_SERVICE_CAPTURE_IDLE;
        }
#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
        else if (capture_route == XV_SERVICE_CAPTURE_COMMAND &&
                 kws_model == XV_SERVICE_KWS_MODEL_COMMAND &&
                 !service->command_event_posted &&
                 (size_t)kws_result.output_index <
                     xv_command_model_output_elements) {
          source_class =
              xv_command_model_output_to_source_class[kws_result.output_index];
          if (source_class >= XV_OFFLINE_COMMAND_CLASS_FIRST &&
              source_class <= XV_OFFLINE_COMMAND_CLASS_LAST) {
            trigger = true;
            kws_label = service_command_label(kws_result.output_index);
            service->command_event_posted = true;
            service->capture_route = XV_SERVICE_CAPTURE_IDLE;
          }
        }
#  endif
      }
#  ifdef CONFIG_EXAMPLES_XIAOV_KWS_VAD_FALLBACK
      /* The fallback and the wake model above compete for the same
       * kws_wake_event_posted latch, so whichever fires first silences the
       * other. Only let the fallback claim the wake once the model has
       * actually had the chance to rule: it needs a full feature context plus
       * a scored wake window, which the frame threshold alone does not guarantee
       * because the speech counter resets on every pause.
       * Requiring real model verdicts keeps the fallback a backstop instead of
       * a preemption.
       */
      if (!trigger && capture_route == XV_SERVICE_CAPTURE_KWS &&
          kws_model == XV_SERVICE_KWS_MODEL_WAKE &&
          !service->kws_wake_event_posted &&
          service->kws_wake_inferences >=
              (uint32_t)XV_SERVICE_KWS_FALLBACK_MIN_INFERENCES &&
          service->kws_vad_fallback_speech_frames >=
              (uint32_t)CONFIG_EXAMPLES_XIAOV_KWS_VAD_FALLBACK_FRAMES) {
        trigger = true;
        vad_fallback = true;
        vad_fallback_speech_frames =
        service->kws_vad_fallback_speech_frames;
        kws_label = "vad_fallback";
        service->kws_wake_event_posted = true;
        service->capture_route = XV_SERVICE_CAPTURE_IDLE;
      }
#  endif
#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
      if (capture_route == XV_SERVICE_CAPTURE_COMMAND) {
        /* This counter used to advance once per 20 ms capture frame. It now runs
         * on the worker, which sees one window per stride, so count the audio the
         * window represents instead -- otherwise the 6 s command window would
         * only expire after 60 s of speech.
         */
        service->command_capture_frames +=
            (uint32_t)XV_KWS_FRONTEND_EMIT_STRIDE_FRAMES;
        if (!trigger && !service->command_event_posted &&
            service->command_capture_frames >=
                (uint32_t)XV_SERVICE_COMMAND_WINDOW_FRAMES) {
          service->command_event_posted = true;
          service->capture_route = XV_SERVICE_CAPTURE_IDLE;
          timeout = true;
        }
      }
#  endif
    }
    pthread_mutex_unlock(&service->lock);

    if (capture_route == XV_SERVICE_CAPTURE_KWS ||
        capture_route == XV_SERVICE_CAPTURE_COMMAND) {
      if (log_error) {
        printf("xiaov: KWS processing error status=%d model=%s; touch "
               "remains available\n",
               (int)kws_status,
               kws_model == XV_SERVICE_KWS_MODEL_COMMAND ? "command"
                                                         : "wake");
      }
      if (trigger) {
        bool posted;
#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
        if (kws_model == XV_SERVICE_KWS_MODEL_COMMAND) {
          posted = service_post_typed_event(
              service, XV_SERVICE_EVENT_OFFLINE_COMMAND, source_class);
        } else {
#  endif
          posted = service_post_typed_event(
              service, XV_SERVICE_EVENT_WAKE_DETECTED, 0U);
#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
        }
#  endif
        pthread_mutex_lock(&service->lock);
        if (posted) {
          service->kws_triggers_posted += 1U;
          if (vad_fallback) {
            service->kws_vad_fallbacks += 1U;
          }
        } else {
          service->kws_triggers_rejected += 1U;
          if (kws_model == XV_SERVICE_KWS_MODEL_WAKE) {
            service->kws_wake_event_posted = false;
          }
          service->capture_route = XV_SERVICE_CAPTURE_KWS;
#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
          if (kws_model == XV_SERVICE_KWS_MODEL_COMMAND) {
            service->capture_route = XV_SERVICE_CAPTURE_COMMAND;
            service->command_event_posted = false;
          }
#  endif
        }
        pthread_mutex_unlock(&service->lock);
        if (vad_fallback) {
          printf("xiaov: KWS wake source=vad_fallback speech_frames=%u "
                 "required_frames=%u posted=%u\n",
                 (unsigned int)vad_fallback_speech_frames,
                 (unsigned int)CONFIG_EXAMPLES_XIAOV_KWS_VAD_FALLBACK_FRAMES,
                 posted ? 1U : 0U);
        } else {
          /* source_class is only mapped for the command model, so reporting it
           * on a wake line always printed 0 and read as a class mismatch
           * against the model's actual output index.
           */
          printf("xiaov: KWS %s label=%s class=%u score_q15=%u posted=%u\n",
                 kws_model == XV_SERVICE_KWS_MODEL_COMMAND ? "command"
                                                           : "wake",
                 kws_label,
                 kws_model == XV_SERVICE_KWS_MODEL_COMMAND
                     ? (unsigned int)source_class
                     : (unsigned int)kws_result.output_index,
                 (unsigned int)kws_result.score_q15,
                 posted ? 1U : 0U);
        }
      }
#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
      if (timeout) {
        if (!service_post_typed_event(
                service, XV_SERVICE_EVENT_OFFLINE_TIMEOUT, 0U)) {
          pthread_mutex_lock(&service->lock);
          service->command_event_posted = false;
          service->capture_route = XV_SERVICE_CAPTURE_COMMAND;
          pthread_mutex_unlock(&service->lock);
        }
      }
#  endif
    }
  }
}

static void *service_kws_worker(void *argument) {
  xv_service_t *service = argument;

  for (;;) {
    xv_tflm_kws_window_t window;
    bool contiguous = false;
    bool stop;
    uint32_t generation = 0U;

    pthread_mutex_lock(&service->kws_queue_lock);
    while (!service->kws_worker_stop && service->kws_queue_count == 0U) {
      pthread_cond_wait(&service->kws_queue_ready, &service->kws_queue_lock);
    }
    stop = service->kws_worker_stop;
    if (!stop) {
      window = service->kws_windows[service->kws_queue_read];
      service->kws_queue_read =
          (service->kws_queue_read + 1U) % XV_SERVICE_KWS_WINDOW_QUEUE_DEPTH;
      service->kws_queue_count -= 1U;
      contiguous = !service->kws_queue_gap;
      service->kws_queue_gap = false;
      generation = service->kws_queue_generation;
    }
    pthread_mutex_unlock(&service->kws_queue_lock);

    if (stop) {
      return NULL;
    }
    service_score_kws_window(service, &window, contiguous);

    /* If a flush happened while that window was being scored, the session it
     * belonged to is over; make sure the next window starts a fresh run. */
    pthread_mutex_lock(&service->kws_queue_lock);
    if (service->kws_queue_generation != generation) {
      service->kws_queue_gap = true;
    }
    pthread_mutex_unlock(&service->kws_queue_lock);
  }
}

/* Discards queued windows so a new standby session does not score audio from the
 * previous one, and marks the gap so the rule cannot bridge across the reset.
 *
 * Emptying the queue is not enough on its own: the worker may already have a
 * window in hand from before the flush. Bumping the generation lets it discard
 * that window instead of scoring stale audio into the new session's run.
 */
static void service_flush_kws_queue(xv_service_t *service) {
  pthread_mutex_lock(&service->kws_queue_lock);
  service->kws_queue_read = 0U;
  service->kws_queue_count = 0U;
  service->kws_wake_pending_valid = false;
  service->kws_queue_gap = true;
  service->kws_queue_generation += 1U;
  pthread_mutex_unlock(&service->kws_queue_lock);
}
#endif

static bool service_capture_pcm(void *context, const int16_t *samples,
                                size_t sample_count, uint32_t timestamp_ms) {
  xv_service_t *service = context;
  xv_service_capture_route_t capture_route;
  bool network_accepted;
  bool vad_active;

  pthread_mutex_lock(&service->lock);
  capture_route = service->capture_route;
  pthread_mutex_unlock(&service->lock);

#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  if (capture_route == XV_SERVICE_CAPTURE_KWS ||
      capture_route == XV_SERVICE_CAPTURE_COMMAND) {
    service_enqueue_kws_window(service, samples, sample_count);
    return true;
  }
#endif

  if (capture_route != XV_SERVICE_CAPTURE_VOICE) {
    return true;
  }

  pthread_mutex_lock(&service->lock);
  xv_pcm_ring_write(&service->pcm_ring, samples, sample_count);
  pthread_mutex_unlock(&service->lock);
  service_visualize_pcm(service, samples, sample_count, false);

  network_accepted = xv_network_send_pcm(service->network, samples,
                                         sample_count, timestamp_ms);
  if (!network_accepted && !service->network_failure_posted) {
    /* A full/down network queue is a recoverable turn failure. Returning
     * false here makes the media worker report capture_drop and can strand a
     * persistent daemon in ERROR. Cancel the active turn and let the normal
     * standby path restart KWS instead. */
    service->network_failure_posted =
        service_post_event(service, XV_APP_EVENT_CANCEL);
  }

  vad_active = xv_vad_process(&service->vad, samples, sample_count);
  service->capture_frames += 1U;
  if (vad_active) {
    service->speech_seen = true;
    service->silent_frames_since_speech = 0U;
  } else if (service->speech_seen && !service->eos_posted) {
    service->silent_frames_since_speech += 1U;
    if (service->silent_frames_since_speech >=
        (uint32_t)XV_SERVICE_CAPTURE_HANGOVER_FRAMES) {
      service->eos_posted =
          service_post_typed_event(service,
                                   (uint16_t)XV_APP_EVENT_END_OF_SPEECH,
                                   XV_SERVICE_EOS_REASON_SILENCE);
    }
  }
  if (service->capture_frames >= XV_SERVICE_MAX_CAPTURE_FRAMES &&
      !service->eos_posted) {
    service->eos_posted =
        service_post_typed_event(service,
                                 (uint16_t)XV_APP_EVENT_END_OF_SPEECH,
                                 XV_SERVICE_EOS_REASON_MAX_DURATION);
  }
  /* Network backpressure must not be reported as a hardware capture error. */
  return true;
}

static void service_media_event(void *context, xv_media_event_t event,
                                int result) {
  xv_service_t *service = context;
  switch (event) {
  case XV_MEDIA_EVENT_CAPTURE_STOPPED: {
    bool notify_network;
    pthread_mutex_lock(&service->lock);
    notify_network = service->network_capture_pending;
    service->network_capture_pending = false;
    pthread_mutex_unlock(&service->lock);
    if (notify_network) {
      xv_network_capture_drained(service->network);
    }
    if (result < 0) {
      service_post_event(service, XV_APP_EVENT_FAILURE);
    }
    break;
  }
  case XV_MEDIA_EVENT_PLAYBACK_DRAINED: {
    bool network_music;
    pthread_mutex_lock(&service->lock);
    network_music = service->music_stream_playing;
    pthread_mutex_unlock(&service->lock);
    if (network_music) {
      (void)service_post_typed_event(
          service, XV_SERVICE_EVENT_MUSIC_STREAM_DRAINED, 0U);
    } else {
      service_post_event(service, XV_APP_EVENT_TURN_COMPLETED);
    }
    break;
  }
  case XV_MEDIA_EVENT_PLAYBACK_SPACE_AVAILABLE:
    xv_network_set_rx_paused(service->network, false);
    break;
  case XV_MEDIA_EVENT_CAPTURE_ERROR:
  case XV_MEDIA_EVENT_PLAYBACK_ERROR:
    service_post_event(service, XV_APP_EVENT_FAILURE);
    break;
  case XV_MEDIA_EVENT_PLAYBACK_STOPPED:
    if (result < 0) {
      service_post_event(service, XV_APP_EVENT_FAILURE);
    }
    break;
  default:
    break;
  }
}

static void service_network_event(void *context, xv_app_event_t event) {
  xv_service_t *service = context;
  if (event == XV_APP_EVENT_TTS_STARTED) {
    service_reset_visualizer(service, true);
  }
  service_post_event(service, event);
}

static void service_network_text(void *context,
                                 xv_network_text_kind_t kind,
                                 const char *turn_id, const char *text) {
  const char *label;
  xv_conversation_text_kind_t conversation_kind;
  xv_service_t *service = context;

  switch (kind) {
  case XV_NETWORK_TEXT_ASR_PARTIAL:
    label = "asr.partial";
    conversation_kind = XV_CONVERSATION_TEXT_ASR_PARTIAL;
    break;
  case XV_NETWORK_TEXT_ASR_FINAL:
    label = "asr.final";
    conversation_kind = XV_CONVERSATION_TEXT_ASR_FINAL;
    break;
  case XV_NETWORK_TEXT_ASSISTANT_DELTA:
    label = "assistant.delta";
    conversation_kind = XV_CONVERSATION_TEXT_ASSISTANT_DELTA;
    break;
  case XV_NETWORK_TEXT_ASSISTANT_FINAL:
    label = "assistant.final";
    conversation_kind = XV_CONVERSATION_TEXT_ASSISTANT_FINAL;
    break;
  default:
    return;
  }
  if (!xv_ui_post_text(service->ui, conversation_kind, turn_id, text)) {
    printf("xiaov: UI dropped %s turn=%s\n", label, turn_id);
  }
}

static void service_network_reminder(void *context, const char *timer_id,
                                     const char *label, const char *kind,
                                     double deadline_epoch) {
  xv_service_t *service = context;
  xv_reminder_t reminder;
  bool accepted;
  size_t pending;
  uint64_t rejected;

  memset(&reminder, 0, sizeof(reminder));
  snprintf(reminder.timer_id, sizeof(reminder.timer_id), "%s", timer_id);
  snprintf(reminder.label, sizeof(reminder.label), "%s", label);
  snprintf(reminder.kind, sizeof(reminder.kind), "%s", kind);
  reminder.deadline_epoch = deadline_epoch;
  pthread_mutex_lock(&service->lock);
  accepted = xv_reminder_queue_push(&service->reminders, &reminder);
  pending = xv_reminder_queue_size(&service->reminders);
  rejected = xv_reminder_queue_rejected(&service->reminders);
  pthread_mutex_unlock(&service->lock);
  if (!accepted) {
    printf("xiaov: reminder queue overflow timer=%s total=%llu\n", timer_id,
           (unsigned long long)rejected);
    return;
  }
  printf("xiaov: reminder queued timer=%s pending=%u\n", timer_id,
         (unsigned int)pending);
  (void)service_post_event(service, XV_APP_EVENT_ALARM_FIRED);
}

static void service_music_state(void *context,
                                const xv_music_snapshot_t *snapshot) {
  xv_service_t *service = context;
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  bool request_restore = false;
#endif

  if (service->ui != NULL && !xv_ui_post_music_state(service->ui, snapshot)) {
    printf("xiaov: UI dropped music state=%s\n",
           xv_music_output_name(snapshot->output));
  }
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  if (snapshot->output == XV_MUSIC_OUTPUT_ERROR ||
      (snapshot->output == XV_MUSIC_OUTPUT_STOPPED &&
       snapshot->desired == XV_MUSIC_DESIRED_STOPPED)) {
    pthread_mutex_lock(&service->lock);
    if (service->music_capture_suspended &&
        !service->music_restore_pending) {
      service->music_restore_pending = true;
      request_restore = true;
    }
    pthread_mutex_unlock(&service->lock);
    if (request_restore &&
        !service_post_typed_event(service, XV_SERVICE_EVENT_MUSIC_IDLE, 0U)) {
      pthread_mutex_lock(&service->lock);
      service->music_restore_pending = false;
      pthread_mutex_unlock(&service->lock);
      printf("xiaov: music KWS restore event dropped\n");
    }
  }
#endif
}

static void service_music_alert_error(void *context, int result) {
  (void)context;
  /* Reminder audio is optional on DShanPi: ALSA owns the codec while the
   * alert path uses Media Framework. Keep the reminder banner authoritative
   * when an asynchronous alert callback reports a backend failure. */
  printf("xiaov: reminder audio failed result=%d\n", result);
}

#ifdef CONFIG_EXAMPLES_XIAOV_KWS
static bool service_suspend_kws_capture_for_music(
    xv_service_t *service, bool *newly_suspended) {
  xv_service_capture_route_t route;
  bool persistent;
  bool was_suspended;

  *newly_suspended = false;
  pthread_mutex_lock(&service->lock);
  route = service->capture_route;
  if (route == XV_SERVICE_CAPTURE_VOICE) {
    pthread_mutex_unlock(&service->lock);
    printf("xiaov: media play rejected during active voice capture\n");
    return false;
  }
  persistent = service->persistent;
  was_suspended = service->music_capture_suspended;
  if (persistent) {
    service->music_capture_suspended = true;
    service->music_restore_pending = false;
    *newly_suspended = !was_suspended;
  }
  service->capture_route = XV_SERVICE_CAPTURE_IDLE;
  pthread_mutex_unlock(&service->lock);

  /* An IDLE route can still have a capture worker finishing its last read.
   * Always wait for the backend acknowledgement before opening the codec. */
  if (!xv_media_stop_capture_and_wait(
          service->media, XV_SERVICE_MUSIC_CAPTURE_STOP_TIMEOUT_MS)) {
    pthread_mutex_lock(&service->lock);
    if (*newly_suspended) {
      service->music_capture_suspended = false;
    }
    pthread_mutex_unlock(&service->lock);
    if (persistent &&
        (route == XV_SERVICE_CAPTURE_KWS ||
         route == XV_SERVICE_CAPTURE_COMMAND) &&
        service->runtime.machine.state == XV_STATE_STANDBY) {
      (void)service_start_standby_kws(service);
    }
    printf("xiaov: media play could not drain capture route=%u\n",
           (unsigned int)route);
    return false;
  }
  printf("xiaov: capture drained for music route=%u persistent=%u\n",
         (unsigned int)route, persistent ? 1U : 0U);
  return true;
}

static void service_request_music_capture_restore(xv_service_t *service) {
  bool post_restore = false;

  pthread_mutex_lock(&service->lock);
  if (service->music_capture_suspended &&
      !service->music_restore_pending) {
    service->music_restore_pending = true;
    post_restore = true;
  }
  pthread_mutex_unlock(&service->lock);
  if (post_restore &&
      !service_post_typed_event(service, XV_SERVICE_EVENT_MUSIC_IDLE, 0U)) {
    pthread_mutex_lock(&service->lock);
    service->music_restore_pending = false;
    pthread_mutex_unlock(&service->lock);
    printf("xiaov: music KWS restore event dropped\n");
  }
}

static bool service_parse_music_stream(const char *source,
                                       uint32_t *stream_id) {
  const char *digits;
  char *end;
  unsigned long value;

  if (source == NULL || stream_id == NULL ||
      strncmp(source, "stream://", 9U) != 0 || strlen(source) != 17U) {
    return false;
  }
  digits = source + 9U;
  errno = 0;
  value = strtoul(digits, &end, 16);
  if (errno != 0 || end != source + 17U || value == 0UL ||
      value > UINT32_MAX) {
    return false;
  }
  *stream_id = (uint32_t)value;
  return true;
}

static bool service_start_network_music(xv_service_t *service,
                                        const char *command_id,
                                        uint32_t stream_id) {
  bool newly_suspended = false;

  if (service == NULL || command_id == NULL || stream_id == 0U) {
    return false;
  }
  service_stop_network_music(service);
  if (!service_suspend_kws_capture_for_music(service, &newly_suspended)) {
    return false;
  }
  if (!xv_media_start_playback(service->media) ||
      !xv_network_begin_music_stream(service->network, stream_id)) {
    (void)xv_media_stop_playback(service->media);
    if (newly_suspended) {
      service_request_music_capture_restore(service);
    }
    return false;
  }
  pthread_mutex_lock(&service->lock);
  service->music_stream_playing = true;
  service->music_stream_id = stream_id;
  (void)snprintf(service->music_stream_command_id,
                 sizeof(service->music_stream_command_id), "%s", command_id);
  pthread_mutex_unlock(&service->lock);
  printf("xiaov: network music ready stream=%08x command=%s\n",
         (unsigned int)stream_id, command_id);
  return true;
}

static void service_stop_network_music(xv_service_t *service) {
  bool was_playing;

  if (service == NULL) {
    return;
  }
  pthread_mutex_lock(&service->lock);
  was_playing = service->music_stream_playing;
  service->music_stream_playing = false;
  service->music_stream_id = 0U;
  service->music_stream_command_id[0] = '\0';
  pthread_mutex_unlock(&service->lock);
  xv_network_stop_music_stream(service->network);
  if (was_playing) {
    (void)xv_media_stop_playback(service->media);
    service_request_music_capture_restore(service);
  }
}

static bool service_queue_deferred_media_play(
    xv_service_t *service, const xv_network_media_command_t *command) {
  size_t command_id_length;
  size_t source_length;
  bool queued = false;

  if (command == NULL || command->command_id == NULL ||
      command->query == NULL) {
    return false;
  }
  command_id_length = strlen(command->command_id);
  source_length = strlen(command->query);
  if (command_id_length == 0U ||
      command_id_length >= sizeof(service->deferred_media_command_id) ||
      source_length == 0U ||
      source_length >= sizeof(service->deferred_media_source)) {
    return false;
  }

  pthread_mutex_lock(&service->lock);
  if (!service->deferred_media_play_valid &&
      service->runtime.machine.state != XV_STATE_ERROR) {
    memcpy(service->deferred_media_command_id, command->command_id,
           command_id_length + 1U);
    memcpy(service->deferred_media_source, command->query,
           source_length + 1U);
    service->deferred_media_play_valid = true;
    queued = true;
  }
  pthread_mutex_unlock(&service->lock);
  if (!queued) {
    return false;
  }

  if (!service_post_typed_event(service,
                                XV_SERVICE_EVENT_MEDIA_PLAY_DEFERRED, 0U)) {
    pthread_mutex_lock(&service->lock);
    if (service->deferred_media_play_valid &&
        strcmp(service->deferred_media_command_id,
               command->command_id) == 0) {
      service->deferred_media_play_valid = false;
      service->deferred_media_command_id[0] = '\0';
      service->deferred_media_source[0] = '\0';
    }
    pthread_mutex_unlock(&service->lock);
    return false;
  }
  return true;
}

static void service_execute_deferred_media_play(xv_service_t *service) {
  char command_id[XV_CONTROL_COMMAND_ID_MAX];
  char source[XV_CONTROL_MEDIA_QUERY_MAX];
  xv_music_command_t request;
  xv_music_command_result_t result;
  const char *error_code;
  bool execute = false;
  bool network_stream = false;
  uint32_t network_stream_id = 0U;
  bool newly_suspended = false;

  pthread_mutex_lock(&service->lock);
  if (service->deferred_media_play_valid &&
      service->runtime.machine.state == XV_STATE_STANDBY &&
      !service->voice_focus_held && !service->tts_focus_held) {
    memcpy(command_id, service->deferred_media_command_id,
           sizeof(command_id));
    memcpy(source, service->deferred_media_source, sizeof(source));
    service->deferred_media_play_valid = false;
    service->deferred_media_command_id[0] = '\0';
    service->deferred_media_source[0] = '\0';
    execute = true;
  }
  pthread_mutex_unlock(&service->lock);
  if (!execute) {
    return;
  }

  memset(&request, 0, sizeof(request));
  memset(&result, 0, sizeof(result));
  request.action = XV_MUSIC_ACTION_PLAY;
  request.source = source;
  if (service_parse_music_stream(source, &network_stream_id)) {
    network_stream = true;
    if (service_start_network_music(service, command_id, network_stream_id)) {
      result.ok = true;
      result.error = XV_MUSIC_ERROR_NONE;
      result.platform_result = 0;
    } else {
      result.error = XV_MUSIC_ERROR_PLAYBACK_FAILED;
      result.platform_result = -EIO;
    }
  } else if (!service_suspend_kws_capture_for_music(service,
                                                    &newly_suspended)) {
    result.error = XV_MUSIC_ERROR_FOCUS_BLOCKED;
    result.platform_result = -EBUSY;
  } else if (!xv_music_execute(service->music, &request, &result) &&
             result.error == XV_MUSIC_ERROR_NONE) {
    result.error = XV_MUSIC_ERROR_INTERNAL;
  }
  if (newly_suspended && !result.ok) {
    service_request_music_capture_restore(service);
  }

  pthread_mutex_lock(&service->lock);
  if (result.ok) {
    service->media_commands_succeeded += 1U;
  } else {
    service->media_commands_failed += 1U;
  }
  pthread_mutex_unlock(&service->lock);
  error_code = xv_music_error_code(result.error);
  printf("xiaov: deferred media.command action=play id=%s ok=%u "
         "result=%d error=%s\n",
         command_id, result.ok ? 1U : 0U, result.platform_result,
         error_code == NULL ? "none" : error_code);
  if (network_stream &&
      !xv_network_send_media_ready(service->network, command_id, result.ok,
                                   result.ok ? NULL :
                                                service_media_wire_error(
                                                    result.error))) {
    printf("xiaov: deferred media.ready queue rejected command=%s\n",
           command_id);
  }
}

static void service_discard_deferred_media_play(xv_service_t *service) {
  bool discarded = false;

  pthread_mutex_lock(&service->lock);
  if (service->deferred_media_play_valid) {
    service->deferred_media_play_valid = false;
    service->deferred_media_command_id[0] = '\0';
    service->deferred_media_source[0] = '\0';
    service->media_commands_failed += 1U;
    discarded = true;
  }
  pthread_mutex_unlock(&service->lock);
  if (discarded) {
    printf("xiaov: deferred media.command discarded before playback\n");
  }
}
#endif

static const char *service_media_wire_error(xv_music_error_t error) {
  switch (error) {
  case XV_MUSIC_ERROR_NONE:
    return NULL;
  case XV_MUSIC_ERROR_INVALID_ACTION:
  case XV_MUSIC_ERROR_INVALID_SOURCE:
  case XV_MUSIC_ERROR_INVALID_ARGUMENT:
  case XV_MUSIC_ERROR_OUT_OF_RANGE:
  case XV_MUSIC_ERROR_PLAYLIST_FULL:
  case XV_MUSIC_ERROR_PLAYLIST_EMPTY:
  case XV_MUSIC_ERROR_NO_TRACK:
    return "invalid_command";
  case XV_MUSIC_ERROR_FOCUS_BLOCKED:
  case XV_MUSIC_ERROR_QUEUE_FULL:
  case XV_MUSIC_ERROR_TIMED_OUT:
  case XV_MUSIC_ERROR_SHUTTING_DOWN:
    return "busy";
  case XV_MUSIC_ERROR_PREPARE_FAILED:
  case XV_MUSIC_ERROR_PLAYBACK_FAILED:
    return "playback_failed";
  case XV_MUSIC_ERROR_INTERNAL:
  default:
    return "backend_unavailable";
  }
}

static void service_network_media_command(
    void *context, const xv_network_media_command_t *command) {
  xv_service_t *service = context;
  xv_music_command_t request;
  xv_music_command_result_t result;
  xv_music_action_t action = XV_MUSIC_ACTION_PLAY;
  const char *error_code;
  const char *wire_error_code;
  bool deferred = false;
  bool network_stream = false;
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  uint32_t network_stream_id = 0U;
  bool newly_suspended = false;
  bool service_error;
  bool should_defer;
#endif

  memset(&request, 0, sizeof(request));
  memset(&result, 0, sizeof(result));
  if (!xv_music_action_from_string(command->action, &action)) {
    result.error = XV_MUSIC_ERROR_INVALID_ACTION;
  } else {
    request.action = action;
    if (action == XV_MUSIC_ACTION_PLAY) {
      if (!command->has_query) {
        result.error = XV_MUSIC_ERROR_INVALID_ARGUMENT;
      } else {
        request.source = command->query;
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
        network_stream =
            service_parse_music_stream(command->query, &network_stream_id);
#endif
      }
    } else if (action == XV_MUSIC_ACTION_SET_VOLUME) {
      if (!command->has_volume_percent) {
        result.error = XV_MUSIC_ERROR_INVALID_ARGUMENT;
      } else {
        request.volume_percent = command->volume_percent;
      }
    } else if (action == XV_MUSIC_ACTION_SEEK) {
      if (!command->has_position_seconds ||
          command->position_seconds != command->position_seconds ||
          command->position_seconds < 0.0 ||
          command->position_seconds > 86400.0) {
        result.error = XV_MUSIC_ERROR_OUT_OF_RANGE;
      } else {
        request.position_ms =
            (uint32_t)(command->position_seconds * 1000.0 + 0.5);
      }
    }
    if (result.error == XV_MUSIC_ERROR_NONE &&
        action == XV_MUSIC_ACTION_PLAY) {
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
      pthread_mutex_lock(&service->lock);
      service_error =
          service->runtime.machine.state == XV_STATE_ERROR;
      should_defer =
          !service_error &&
          (service->runtime.machine.state != XV_STATE_STANDBY ||
           service->voice_focus_held || service->tts_focus_held);
      pthread_mutex_unlock(&service->lock);
      if (service_error) {
        result.error = XV_MUSIC_ERROR_FOCUS_BLOCKED;
        result.platform_result = -EBUSY;
      } else if (should_defer) {
        if (service_queue_deferred_media_play(service, command)) {
          deferred = true;
        } else {
          result.error = XV_MUSIC_ERROR_QUEUE_FULL;
          result.platform_result = -EBUSY;
        }
      } else if (network_stream) {
        if (service_start_network_music(service, command->command_id,
                                        network_stream_id)) {
          result.ok = true;
          result.error = XV_MUSIC_ERROR_NONE;
          result.platform_result = 0;
        } else {
          result.error = XV_MUSIC_ERROR_PLAYBACK_FAILED;
          result.platform_result = -EIO;
        }
      } else if (!service_suspend_kws_capture_for_music(
                     service, &newly_suspended)) {
        result.error = XV_MUSIC_ERROR_FOCUS_BLOCKED;
        result.platform_result = -EBUSY;
      }
#endif
    }
    if (!deferred && result.error == XV_MUSIC_ERROR_NONE &&
        !network_stream &&
        !xv_music_execute(service->music, &request, &result) &&
        result.error == XV_MUSIC_ERROR_NONE) {
      result.error = XV_MUSIC_ERROR_INTERNAL;
    }
  }
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  if (deferred) {
    printf("xiaov: media.command action=play accepted=deferred\n");
    if (!xv_network_send_media_result(service->network,
                                      command->command_id, true, NULL)) {
      printf("xiaov: deferred media.result queue rejected command=%s\n",
             command->command_id);
      service_discard_deferred_media_play(service);
    }
    return;
  }
  if (action == XV_MUSIC_ACTION_STOP) {
    service_discard_deferred_media_play(service);
    pthread_mutex_lock(&service->lock);
    network_stream = service->music_stream_playing;
    pthread_mutex_unlock(&service->lock);
    if (network_stream) {
      service_stop_network_music(service);
      result.ok = true;
      result.error = XV_MUSIC_ERROR_NONE;
      result.platform_result = 0;
    }
  }
  if (newly_suspended && !result.ok) {
    service_request_music_capture_restore(service);
  }
#endif

  pthread_mutex_lock(&service->lock);
  if (result.ok) {
    service->media_commands_succeeded += 1U;
  } else {
    service->media_commands_failed += 1U;
  }
  pthread_mutex_unlock(&service->lock);
  error_code = xv_music_error_code(result.error);
  wire_error_code = service_media_wire_error(result.error);
  printf("xiaov: media.command action=%s ok=%u result=%d error=%s\n",
         command->action, result.ok ? 1U : 0U, result.platform_result,
         error_code == NULL ? "none" : error_code);
  if (!xv_network_send_media_result(service->network, command->command_id,
                                     result.ok, wire_error_code)) {
    printf("xiaov: media.result queue rejected command=%s\n",
           command->command_id);
  }
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  if (action == XV_MUSIC_ACTION_PLAY && network_stream) {
    if (!xv_network_send_media_ready(service->network, command->command_id,
                                     result.ok,
                                     result.ok ? NULL : wire_error_code)) {
      printf("xiaov: media.ready queue rejected command=%s\n",
             command->command_id);
    }
  }
#endif
}

static bool service_accept_video_frame(void *context,
                                       const uint8_t *rgb565,
                                       size_t byte_count,
                                       uint32_t sequence,
                                       uint32_t timestamp_ms) {
  xv_service_t *service = context;
  return service->ui != NULL &&
         xv_ui_update_video_frame(service->ui, rgb565, byte_count, sequence,
                                  timestamp_ms);
}

static void service_display_state(void *context, const char *mode,
                                  const char *status) {
  xv_service_t *service = context;
  if (service->ui != NULL &&
      !xv_ui_post_display_state(service->ui, mode, status)) {
    printf("xiaov: UI dropped display state mode=%s status=%s\n", mode,
           status);
  }
}

static bool service_accept_tts(void *context, const int16_t *samples,
                               size_t sample_count) {
  xv_service_t *service = context;
  size_t required =
      (sample_count + XV_MEDIA_FRAME_SAMPLES - 1U) / XV_MEDIA_FRAME_SAMPLES;
  size_t offset = 0U;

  if (required > XV_MEDIA_PLAYBACK_QUEUE_FRAMES -
                     xv_media_playback_queued(service->media)) {
    return false;
  }
  while (offset < sample_count) {
    size_t count = sample_count - offset;
    if (count > XV_MEDIA_FRAME_SAMPLES) {
      count = XV_MEDIA_FRAME_SAMPLES;
    }
    if (!xv_media_enqueue_playback(service->media, samples + offset, count)) {
      return false;
    }
    offset += count;
  }
  service_visualize_pcm(service, samples, sample_count, true);
  if (xv_media_playback_queued(service->media) >=
      (XV_MEDIA_PLAYBACK_QUEUE_FRAMES * 3U) / 4U) {
    xv_network_set_rx_paused(service->network, true);
  }
  return true;
}

static void service_finish_tts(void *context, uint32_t sample_count) {
  xv_service_t *service = context;
  (void)sample_count;
  if (!xv_media_finish_playback(service->media)) {
    service_post_event(service, XV_APP_EVENT_FAILURE);
  }
}

static bool service_accept_music(void *context, const int16_t *samples,
                                 size_t sample_count) {
  xv_service_t *service = context;
  size_t required;
  bool active;

  if (service == NULL || samples == NULL || sample_count == 0U ||
      sample_count > XV_MEDIA_FRAME_SAMPLES) {
    return false;
  }
  pthread_mutex_lock(&service->lock);
  active = service->music_stream_playing;
  pthread_mutex_unlock(&service->lock);
  if (!active) {
    return false;
  }
  required = (sample_count + XV_MEDIA_FRAME_SAMPLES - 1U) /
             XV_MEDIA_FRAME_SAMPLES;
  if (required > XV_MEDIA_PLAYBACK_QUEUE_FRAMES -
                     xv_media_playback_queued(service->media)) {
    return false;
  }
  if (!xv_media_enqueue_playback(service->media, samples, sample_count)) {
    return false;
  }
  service_visualize_pcm(service, samples, sample_count, true);
  if (xv_media_playback_queued(service->media) >=
      (XV_MEDIA_PLAYBACK_QUEUE_FRAMES * 3U) / 4U) {
    (void)xv_network_set_rx_paused(service->network, true);
  }
  return true;
}

static void service_finish_music(void *context, uint32_t sample_count) {
  xv_service_t *service = context;
  (void)sample_count;
  if (!xv_media_finish_playback(service->media)) {
    service_post_event(service, XV_APP_EVENT_FAILURE);
  }
}

static uint64_t service_monotonic_ms(void *context) {
  struct timespec now;
  (void)context;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0U;
  }
  return (uint64_t)now.tv_sec * 1000U +
         (uint64_t)now.tv_nsec / 1000000U;
}

static xv_music_interrupt_policy_t service_voice_interrupt_policy(void) {
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  /* ALSA voice playback owns hw:audiocodec; release any media-player handle. */
  return XV_MUSIC_INTERRUPT_STOP;
#else
  return XV_MUSIC_INTERRUPT_PAUSE;
#endif
}

#ifdef CONFIG_EXAMPLES_XIAOV_KWS
static bool service_play_wake_prompt(void) {
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  xv_alsa_playback_backend_t playback;
  xv_local_wav_t wav;
  int16_t samples[XV_LOCAL_WAV_FRAME_SAMPLES];
  uint32_t duration_ms = 0U;
  const char *failed_stage = "wav_open";
  bool eof = false;
  bool success = false;
  size_t samples_read = 0U;
  int result;

  xv_alsa_playback_backend_reset(&playback);
  xv_local_wav_reset(&wav);
  result = xv_local_wav_open(&wav, CONFIG_EXAMPLES_XIAOV_WAKE_PROMPT_PATH);
  if (result < 0) {
    goto done;
  }
  duration_ms = xv_local_wav_duration_ms(&wav);
  failed_stage = "alsa_open";
  result = xv_alsa_playback_backend_open(&playback);
  if (result < 0) {
    goto done;
  }
  while (!eof) {
    failed_stage = "wav_read";
    result = xv_local_wav_read(&wav, samples, XV_LOCAL_WAV_FRAME_SAMPLES,
                               &samples_read, &eof);
    if (result < 0) {
      goto done;
    }
    if (samples_read > 0U) {
      failed_stage = "alsa_write";
      result = xv_alsa_playback_backend_write(&playback, samples,
                                              samples_read);
      if (result < 0) {
        goto done;
      }
    }
  }
  failed_stage = "alsa_drain";
  result = xv_alsa_playback_backend_drain(&playback);
  success = result >= 0;

done:
  if (xv_alsa_playback_backend_close(&playback) < 0 && success) {
    failed_stage = "alsa_close";
    result = -EIO;
    success = false;
  }
  xv_local_wav_close(&wav);
  if (success) {
    printf("xiaov: wake prompt played duration_ms=%u path=%s\n",
           (unsigned int)duration_ms,
           CONFIG_EXAMPLES_XIAOV_WAKE_PROMPT_PATH);
  } else {
    printf("xiaov: wake prompt failed stage=%s result=%d path=%s\n",
           failed_stage, result, CONFIG_EXAMPLES_XIAOV_WAKE_PROMPT_PATH);
  }
  return success;
#else
  printf("xiaov: wake prompt failed stage=backend result=%d path=%s\n",
         -ENOTSUP, CONFIG_EXAMPLES_XIAOV_WAKE_PROMPT_PATH);
  return false;
#endif
}
#endif

static bool service_start_capture(void *context) {
  xv_service_t *service = context;
  bool focus_acquired = false;
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  bool play_wake_prompt = false;
  bool prompt_played = false;
#endif

  if (!service->voice_focus_held) {
    if (!xv_music_set_interruption(
            service->music, XV_MUSIC_REASON_VOICE_TURN,
            service_voice_interrupt_policy(), true)) {
      return false;
    }
    service->voice_focus_held = true;
    focus_acquired = true;
  }
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  pthread_mutex_lock(&service->lock);
  play_wake_prompt = service->wake_prompt_pending;
  service->wake_prompt_pending = false;
  if (play_wake_prompt) {
    service->capture_route = XV_SERVICE_CAPTURE_IDLE;
  }
  pthread_mutex_unlock(&service->lock);
  if (play_wake_prompt) {
    if (xv_media_stop_capture_and_wait(
            service->media, XV_SERVICE_MUSIC_CAPTURE_STOP_TIMEOUT_MS)) {
      prompt_played = service_play_wake_prompt();
    } else {
      printf("xiaov: wake prompt failed stage=capture_stop result=%d path=%s\n",
             -ETIMEDOUT, CONFIG_EXAMPLES_XIAOV_WAKE_PROMPT_PATH);
    }
    pthread_mutex_lock(&service->lock);
    if (prompt_played) {
      service->wake_prompts_played += 1U;
    } else {
      service->wake_prompts_failed += 1U;
    }
    pthread_mutex_unlock(&service->lock);
  }
#endif
  pthread_mutex_lock(&service->lock);
  service->capture_frames = 0U;
  service->silent_frames_since_speech = 0U;
  service->speech_seen = false;
  service->eos_posted = false;
  service->network_failure_posted = false;
  pthread_mutex_unlock(&service->lock);
  service_reset_visualizer(service, false);
  xv_vad_reset(&service->vad);
  if (!(service->persistent
            ? xv_network_ensure_session(service->network)
            : xv_network_ensure_ready_session(service->network))) {
    if (focus_acquired) {
      (void)xv_music_set_interruption(
          service->music, XV_MUSIC_REASON_VOICE_TURN,
          service_voice_interrupt_policy(), false);
      service->voice_focus_held = false;
    }
    return false;
  }
  pthread_mutex_lock(&service->lock);
  service->capture_route = XV_SERVICE_CAPTURE_VOICE;
  service->network_capture_pending = true;
  pthread_mutex_unlock(&service->lock);
  if (!xv_media_start_capture(service->media)) {
    pthread_mutex_lock(&service->lock);
    service->capture_route = XV_SERVICE_CAPTURE_IDLE;
    service->network_capture_pending = false;
    pthread_mutex_unlock(&service->lock);
    if (focus_acquired) {
      (void)xv_music_set_interruption(
          service->music, XV_MUSIC_REASON_VOICE_TURN,
          service_voice_interrupt_policy(), false);
      service->voice_focus_held = false;
    }
    return false;
  }
  printf("xiaov: online capture ready route=voice\n");
  return true;
}

static bool service_stop_capture(void *context) {
  xv_service_t *service = context;
  return xv_media_stop_capture(service->media);
}

static bool service_start_playback(void *context) {
  xv_service_t *service = context;
  if (!service->tts_focus_held) {
    if (!xv_music_set_interruption(service->music, XV_MUSIC_REASON_TTS,
                                   service_voice_interrupt_policy(), true)) {
      return false;
    }
    service->tts_focus_held = true;
  }
  if (!xv_media_start_playback(service->media)) {
    (void)xv_music_set_interruption(service->music, XV_MUSIC_REASON_TTS,
                                    service_voice_interrupt_policy(), false);
    service->tts_focus_held = false;
    return false;
  }
  return true;
}

static bool service_stop_playback(void *context) {
  xv_service_t *service = context;
  bool stopped = xv_media_stop_playback(service->media);
  bool released = true;

  if (service->tts_focus_held) {
    released = xv_music_set_interruption(
        service->music, XV_MUSIC_REASON_TTS, service_voice_interrupt_policy(),
        false);
    service->tts_focus_held = false;
  }
  return stopped && released;
}

static bool service_start_alert(void *context) {
  xv_service_t *service = context;
  if (!xv_music_start_alert(service->music)) {
    /* Reminder UI remains authoritative when the optional media backend is
     * unavailable; an alert sound must not break the voice state machine. */
    printf("xiaov: reminder audio unavailable; using UI reminder only\n");
  }
  return true;
}

static bool service_stop_alert(void *context) {
  xv_service_t *service = context;
  if (!xv_music_stop_alert(service->music)) {
    /* Do not report standby while an active alert may still own the codec.
     * Runtime error cleanup will make another best-effort STOP attempt. */
    printf("xiaov: reminder audio stop failed; entering error cleanup\n");
    return false;
  }
  return true;
}

static bool service_show_error(void *context) {
  xv_service_t *service = context;
  printf("xiaov: service entered error, failed_action=0x%08lx\n",
         (unsigned long)xv_runtime_last_failed_action(&service->runtime));
  (void)xv_ui_post_error(service->ui, "Voice service error");
  return true;
}

static bool service_clear_error(void *context) {
  (void)context;
  printf("xiaov: service error cleared\n");
  return true;
}

#ifdef CONFIG_EXAMPLES_XIAOV_KWS
static bool service_configure_kws(xv_service_t *service,
                                  xv_service_kws_model_t model) {
  xv_tflm_kws_config_t config;
  xv_tflm_kws_stats_t stats;
  xv_tflm_kws_status_t status;
  const char *model_name = "wake";
  const char *model_sha256 = xv_kws_model_sha256;
  const char *model_license_id = xv_kws_model_license_id;
  size_t model_size = xv_kws_model_size;

  pthread_mutex_lock(&service->lock);
  service->capture_route = XV_SERVICE_CAPTURE_IDLE;
  service->kws_ready = false;
  pthread_mutex_unlock(&service->lock);
  xv_kws_frontend_init(&service->kws_frontend);
  memset(&config, 0, sizeof(config));
#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
  if (model == XV_SERVICE_KWS_MODEL_COMMAND) {
    model_name = "command";
    model_sha256 = xv_command_model_sha256;
    model_license_id = xv_command_model_license_id;
    model_size = xv_command_model_size;
    config.model_data = xv_command_model_data;
    config.model_size = xv_command_model_size;
    config.input_elements = xv_command_model_input_elements;
    config.input_scale = xv_command_model_input_scale;
    config.input_zero_point = xv_command_model_input_zero_point;
    config.output_elements = xv_command_model_output_elements;
    config.output_scale = xv_command_model_output_scale;
    config.output_zero_point = xv_command_model_output_zero_point;
    config.wake_class_indices = g_command_candidate_output_indices;
    config.wake_class_count = XV_SERVICE_COMMAND_CANDIDATE_COUNT;
    config.decision.score_threshold_q15 =
        (uint16_t)CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMAND_THRESHOLD_Q15;
    config.decision.consecutive_hits =
        (uint16_t)CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMAND_CONSECUTIVE_HITS;
    config.decision.cooldown_ms = 0U;
  } else
#  else
  (void)model;
#  endif
  {
    config.model_data = xv_kws_model_data;
    config.model_size = xv_kws_model_size;
    config.input_elements = xv_kws_model_input_elements;
    config.input_scale = xv_kws_model_input_scale;
    config.input_zero_point = xv_kws_model_input_zero_point;
    config.output_elements = xv_kws_model_output_elements;
    config.output_scale = xv_kws_model_output_scale;
    config.output_zero_point = xv_kws_model_output_zero_point;
    config.wake_class_indices = xv_kws_model_wake_class_indices;
    config.wake_class_count = xv_kws_model_wake_class_count;
    config.decision.score_threshold_q15 =
        (uint16_t)CONFIG_EXAMPLES_XIAOV_KWS_SCORE_THRESHOLD_Q15;
    config.decision.consecutive_hits =
        (uint16_t)CONFIG_EXAMPLES_XIAOV_KWS_CONSECUTIVE_HITS;
    config.decision.cooldown_ms =
        (uint32_t)CONFIG_EXAMPLES_XIAOV_KWS_COOLDOWN_MS;
  }
  config.frontend.context = &service->kws_frontend;
  config.frontend.sample_rate_hz = XV_KWS_FRONTEND_SAMPLE_RATE_HZ;
  config.frontend.process = xv_kws_frontend_process;
  config.frontend.reset = xv_kws_frontend_reset;
#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
  if (model == XV_SERVICE_KWS_MODEL_COMMAND) {
    config.frontend.reset = xv_kws_frontend_prefill_silence;
  }
#  endif

  status = xv_tflm_kws_init(&config);
  if (status != XV_TFLM_KWS_STATUS_OK) {
    printf("xiaov: KWS initialization failed status=%d mode=%s model=%s\n",
           (int)status, model_name, model_sha256);
    return false;
  }
  memset(&stats, 0, sizeof(stats));
  xv_tflm_kws_get_stats(&stats);
  pthread_mutex_lock(&service->lock);
  service->kws_ready = true;
  service->kws_model = model;
  pthread_mutex_unlock(&service->lock);
  printf("xiaov: KWS ready mode=%s model_bytes=%lu arena_used=%lu "
         "sha256=%s license=%s\n",
         model_name, (unsigned long)model_size,
         (unsigned long)stats.arena_used_bytes, model_sha256,
         model_license_id);
  return true;
}

static bool service_init_kws(xv_service_t *service) {
  return service_configure_kws(service, XV_SERVICE_KWS_MODEL_WAKE);
}

static bool service_start_standby_kws(xv_service_t *service) {
  xv_tflm_kws_status_t status;
  bool wake_ready;
  bool started;

  pthread_mutex_lock(&service->lock);
  if (service->music_capture_suspended) {
    pthread_mutex_unlock(&service->lock);
    return true;
  }
  wake_ready = service->kws_ready &&
               service->kws_model == XV_SERVICE_KWS_MODEL_WAKE;
  pthread_mutex_unlock(&service->lock);
  if (!wake_ready &&
      !service_configure_kws(service, XV_SERVICE_KWS_MODEL_WAKE)) {
    return false;
  }

  /* Windows from the previous session describe audio that is now seconds old.
   * Drop them before the route reopens so the rule never scores stale features.
   */
  service_flush_kws_queue(service);
  pthread_mutex_lock(&service->lock);
  if (service->music_capture_suspended) {
    pthread_mutex_unlock(&service->lock);
    return true;
  }
  status = xv_tflm_kws_reset_stream();
  if (status == XV_TFLM_KWS_STATUS_OK) {
    xv_vad_reset(&service->kws_vad);
    service->kws_vad_hold_frames = 0U;
    service->kws_vad_fallback_speech_frames = 0U;
    service->kws_wake_inferences = 0U;
    service->kws_error_logged = false;
    service->kws_wake_event_posted = false;
    service->capture_route = XV_SERVICE_CAPTURE_KWS;
  }
  started = status == XV_TFLM_KWS_STATUS_OK &&
            xv_media_start_capture(service->media);
  if (!started) {
    service->capture_route = XV_SERVICE_CAPTURE_IDLE;
  }
  pthread_mutex_unlock(&service->lock);
  if (status != XV_TFLM_KWS_STATUS_OK) {
    printf("xiaov: KWS stream reset failed status=%d\n", (int)status);
    return false;
  }
  if (!started) {
    printf("xiaov: KWS standby capture start failed\n");
    return false;
  }
  return true;
}

#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
static bool service_start_command_kws(xv_service_t *service) {
  xv_tflm_kws_status_t status;
  bool started;

  if (!service_configure_kws(service, XV_SERVICE_KWS_MODEL_COMMAND)) {
    return false;
  }
  service_flush_kws_queue(service);
  pthread_mutex_lock(&service->lock);
  if (service->music_capture_suspended) {
    pthread_mutex_unlock(&service->lock);
    return false;
  }
  status = xv_tflm_kws_reset_stream();
  if (status == XV_TFLM_KWS_STATUS_OK) {
    xv_vad_reset(&service->kws_vad);
    service->kws_vad_hold_frames = 0U;
    service->kws_vad_fallback_speech_frames = 0U;
    service->kws_wake_inferences = 0U;
    service->kws_error_logged = false;
    service->kws_wake_event_posted = false;
    service->command_capture_frames = 0U;
    service->command_event_posted = false;
    service->capture_route = XV_SERVICE_CAPTURE_COMMAND;
  }
  started = status == XV_TFLM_KWS_STATUS_OK &&
            xv_media_start_capture(service->media);
  if (!started) {
    service->capture_route = XV_SERVICE_CAPTURE_IDLE;
  }
  pthread_mutex_unlock(&service->lock);
  if (status != XV_TFLM_KWS_STATUS_OK) {
    printf("xiaov: command KWS stream reset failed status=%d\n",
           (int)status);
    return false;
  }
  if (!started) {
    printf("xiaov: command KWS capture start failed\n");
    return false;
  }
  printf("xiaov: offline command window started duration_ms=%u\n",
         (unsigned int)CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMAND_WINDOW_MS);
  return true;
}
#  endif
#endif

#if defined(CONFIG_EXAMPLES_XIAOV_KWS) && \
    defined(CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS)
static bool service_offline_media(void *context,
                                  xv_offline_media_action_t action) {
  xv_service_t *service = context;
  xv_music_command_t command;
  xv_music_command_result_t result;
  xv_music_snapshot_t snapshot;
  bool newly_suspended = false;
  bool success;

  memset(&command, 0, sizeof(command));
  memset(&result, 0, sizeof(result));
  switch (action) {
  case XV_OFFLINE_MEDIA_PLAY:
    if (!xv_music_get_snapshot(service->music, &snapshot)) {
      return false;
    }
    if (snapshot.has_current &&
        snapshot.output == XV_MUSIC_OUTPUT_PAUSED) {
      command.action = XV_MUSIC_ACTION_RESUME;
    } else {
      command.action = XV_MUSIC_ACTION_PLAY;
      command.source = snapshot.has_current
                           ? snapshot.source
                           : CONFIG_EXAMPLES_XIAOV_MUSIC_DEFAULT_SOURCE;
    }
    if (!service_suspend_kws_capture_for_music(service,
                                               &newly_suspended)) {
      return false;
    }
    break;
  case XV_OFFLINE_MEDIA_PAUSE:
    command.action = XV_MUSIC_ACTION_PAUSE;
    break;
  case XV_OFFLINE_MEDIA_RESUME:
    command.action = XV_MUSIC_ACTION_RESUME;
    break;
  case XV_OFFLINE_MEDIA_STOP:
    command.action = XV_MUSIC_ACTION_STOP;
    break;
  case XV_OFFLINE_MEDIA_PREVIOUS:
    command.action = XV_MUSIC_ACTION_PREVIOUS;
    break;
  case XV_OFFLINE_MEDIA_NEXT:
    command.action = XV_MUSIC_ACTION_NEXT;
    break;
  default:
    return false;
  }
  success = xv_music_execute(service->music, &command, &result) && result.ok;
  if (newly_suspended && !success) {
    service_request_music_capture_restore(service);
  }
  return success;
}

static bool service_offline_adjust_volume(void *context,
                                          int16_t delta_percent) {
  xv_service_t *service = context;
  xv_music_snapshot_t snapshot;
  xv_music_command_t command;
  xv_music_command_result_t result;
  int32_t volume;

  if (!xv_music_get_snapshot(service->music, &snapshot)) {
    return false;
  }
  volume = (int32_t)snapshot.volume_percent + (int32_t)delta_percent;
  if (volume < 0) {
    volume = 0;
  } else if (volume > 100) {
    volume = 100;
  }
  memset(&command, 0, sizeof(command));
  memset(&result, 0, sizeof(result));
  command.action = XV_MUSIC_ACTION_SET_VOLUME;
  command.volume_percent = (uint32_t)volume;
  return xv_music_execute(service->music, &command, &result) && result.ok;
}

static bool service_offline_set_muted(void *context, bool muted) {
  xv_service_t *service = context;
  xv_music_snapshot_t snapshot;
  xv_music_command_t command;
  xv_music_command_result_t result;
  uint8_t volume;

  if (!xv_music_get_snapshot(service->music, &snapshot)) {
    return false;
  }
  if (muted) {
    if (snapshot.volume_percent > 0U) {
      service->offline_pre_mute_volume = snapshot.volume_percent;
    }
    volume = 0U;
  } else {
    volume = service->offline_pre_mute_volume;
    if (volume == 0U) {
      volume = XV_MUSIC_DEFAULT_VOLUME_PERCENT;
    }
  }
  memset(&command, 0, sizeof(command));
  memset(&result, 0, sizeof(result));
  command.action = XV_MUSIC_ACTION_SET_VOLUME;
  command.volume_percent = volume;
  return xv_music_execute(service->music, &command, &result) && result.ok;
}

static bool service_offline_set_screen_active(void *context, bool active) {
  xv_service_t *service = context;
  return xv_power_set_active(service->power, active);
}
#endif

static bool service_set_animation_enabled(void *context, bool enabled) {
  xv_service_t *service = context;
  return service->ui != NULL &&
         xv_ui_post_low_power(service->ui, !enabled);
}

static bool service_low_power(void *context) {
  xv_service_t *service = context;
  bool success = true;
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  bool music_blocks_capture;
#endif
  if (!xv_power_set_active(service->power, false)) {
    printf("xiaov: standby display power capability unavailable\n");
  }
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  pthread_mutex_lock(&service->lock);
  music_blocks_capture = service->music_capture_suspended;
  pthread_mutex_unlock(&service->lock);
  if (service->persistent && service->kws_ready &&
      !music_blocks_capture &&
      !service_start_standby_kws(service)) {
    success = false;
  }
#endif
  printf("xiaov: service turn idle\n");
  return success;
}

static void service_state_changed(void *context,
                                  const xv_transition_t *transition) {
  xv_service_t *service = context;
  printf("xiaov: service %s -> %s actions=0x%08lx\n",
         xv_state_name(transition->previous),
         xv_state_name(transition->current),
         (unsigned long)transition->actions);
  pthread_mutex_lock(&service->lock);
  service->capture_route = XV_SERVICE_CAPTURE_IDLE;
  pthread_mutex_unlock(&service->lock);
  if (transition->previous == XV_STATE_STANDBY &&
      transition->current != XV_STATE_STANDBY &&
      !xv_power_set_active(service->power, true)) {
    printf("xiaov: active display power capability unavailable\n");
  }
  if (transition->previous == XV_STATE_STANDBY &&
      transition->current == XV_STATE_ALERTING &&
      !xv_media_stop_capture(service->media)) {
    printf("xiaov: failed to stop standby capture for alert\n");
    (void)service_post_event(service, XV_APP_EVENT_FAILURE);
  }
  if (!xv_ui_post_state(service->ui, transition->current)) {
    printf("xiaov: UI dropped state=%s\n",
           xv_state_name(transition->current));
  }
}

static void service_ui_action(void *context, xv_app_event_t event) {
  xv_service_t *service = context;
  if (!service_post_event(service, event)) {
    printf("xiaov: touch action dropped event=%u\n", (unsigned int)event);
  }
}

#if defined(CONFIG_EXAMPLES_XIAOV_KWS) && \
    defined(CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS)
static void service_ui_offline_command(void *context) {
  xv_service_t *service = context;

  if (!service_post_typed_event(
          service, XV_SERVICE_EVENT_OFFLINE_REQUEST, 0U)) {
    printf("xiaov: touch offline command request dropped\n");
  }
}
#endif

static void service_ui_media_action(void *context, xv_music_action_t action,
                                    uint32_t value) {
  xv_service_t *service = context;
  xv_music_command_t command;
  xv_music_snapshot_t snapshot;
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  bool newly_suspended = false;
#endif
  bool submitted;

  memset(&command, 0, sizeof(command));
  command.action = action;
  if (action == XV_MUSIC_ACTION_PLAY) {
    if (!xv_music_get_snapshot(service->music, &snapshot)) {
      printf("xiaov: local music snapshot unavailable\n");
      return;
    }
    if (snapshot.has_current &&
        snapshot.output == XV_MUSIC_OUTPUT_PAUSED) {
      command.action = XV_MUSIC_ACTION_RESUME;
    } else {
      command.source = snapshot.has_current
                           ? snapshot.source
                           : CONFIG_EXAMPLES_XIAOV_MUSIC_DEFAULT_SOURCE;
    }
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
    if (!service_suspend_kws_capture_for_music(service,
                                               &newly_suspended)) {
      return;
    }
#endif
  } else if (action == XV_MUSIC_ACTION_SET_VOLUME) {
    command.volume_percent = value;
  } else if (action == XV_MUSIC_ACTION_SEEK) {
    command.position_ms = value;
  }
  submitted = xv_music_submit(service->music, &command);
  if (!submitted) {
    printf("xiaov: local music command rejected action=%u\n",
           (unsigned int)action);
  }
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  if (newly_suspended && !submitted) {
    service_request_music_capture_restore(service);
  }
#endif
}

static void service_ui_display_action(void *context, const char *action) {
  xv_service_t *service = context;
  if (!xv_network_send_display_command(service->network, action, NULL)) {
    printf("xiaov: display command rejected action=%s\n", action);
    (void)xv_ui_post_display_state(service->ui, "closed", "error");
  }
}

static bool service_wait_network_ready(xv_service_t *service) {
  struct timespec delay;
  unsigned int attempt;
  unsigned int attempts;

  attempts = ((unsigned int)CONFIG_EXAMPLES_XIAOV_CONNECT_TIMEOUT_MS +
              XV_SERVICE_CONNECT_POLL_MS - 1U) /
             XV_SERVICE_CONNECT_POLL_MS;
  delay.tv_sec = 0;
  delay.tv_nsec = (long)XV_SERVICE_CONNECT_POLL_MS * 1000000L;
  for (attempt = 0U; attempt < attempts; ++attempt) {
    if (xv_network_is_connected(service->network)) {
      return true;
    }
    (void)nanosleep(&delay, NULL);
  }
  return xv_network_is_connected(service->network);
}

static bool service_init(xv_service_t *service, const char *host,
                         uint16_t port, const char *path, bool use_tls) {
  xv_platform_adapters_t platform;
  xv_media_callbacks_t media_callbacks;
  xv_music_callbacks_t music_callbacks;
  xv_network_callbacks_t network_callbacks;
  xv_network_config_t network_config;
  xv_power_config_t power_config;
  xv_ui_callbacks_t ui_callbacks;
  char gateway_token[XV_NETWORK_TOKEN_MAX];

  memset(service, 0, sizeof(*service));
  if (pthread_mutex_init(&service->lock, NULL) != 0) {
    return false;
  }
  service->lock_initialized = true;
  if (sem_init(&service->event_wake, 0, 0) < 0) {
    return false;
  }
  service->event_wake_initialized = true;
  if (!xv_pcm_ring_init(&service->pcm_ring, service->pcm_storage,
                        XV_SERVICE_PCM_SAMPLES) ||
      !xv_event_queue_init(&service->event_queue, service->event_storage,
                           XV_SERVICE_EVENT_CAPACITY) ||
      !xv_reminder_queue_init(&service->reminders,
                              service->reminder_storage,
                              XV_SERVICE_REMINDER_CAPACITY) ||
      !xv_vad_init(&service->vad, NULL)) {
    return false;
  }
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  if (!xv_vad_init(&service->kws_vad, NULL) ||
      !service_init_kws(service)) {
    return false;
  }
  /* The inference worker must exist before capture can start: the capture
   * callback enqueues unconditionally once the route is kws, and nothing would
   * drain the queue. */
  if (pthread_mutex_init(&service->kws_queue_lock, NULL) != 0) {
    return false;
  }
  service->kws_queue_initialized = true;
  if (pthread_cond_init(&service->kws_queue_ready, NULL) != 0) {
    return false;
  }
  {
    pthread_attr_t attributes;

    if (pthread_attr_init(&attributes) != 0) {
      return false;
    }
    if (pthread_attr_setstacksize(&attributes,
                                  XV_SERVICE_KWS_WORKER_STACKSIZE) != 0 ||
        pthread_create(&service->kws_worker, &attributes, service_kws_worker,
                       service) != 0) {
      (void)pthread_attr_destroy(&attributes);
      return false;
    }
    (void)pthread_attr_destroy(&attributes);
    service->kws_worker_running = true;
  }
#endif

  memset(&media_callbacks, 0, sizeof(media_callbacks));
  media_callbacks.context = service;
  media_callbacks.capture_sink = service_capture_pcm;
  media_callbacks.emit_event = service_media_event;
  service->media = xv_media_init(&service->media_storage, &media_callbacks);
  if (service->media == NULL) {
    return false;
  }

  memset(&network_callbacks, 0, sizeof(network_callbacks));
  network_callbacks.context = service;
  network_callbacks.emit_event = service_network_event;
  network_callbacks.emit_text = service_network_text;
  network_callbacks.emit_reminder = service_network_reminder;
  network_callbacks.emit_media_command = service_network_media_command;
  network_callbacks.accept_tts_pcm = service_accept_tts;
  network_callbacks.finish_tts = service_finish_tts;
  network_callbacks.accept_music_pcm = service_accept_music;
  network_callbacks.finish_music = service_finish_music;
  network_callbacks.accept_video_frame = service_accept_video_frame;
  network_callbacks.display_state = service_display_state;
  memset(&network_config, 0, sizeof(network_config));
  network_config.host = host;
  network_config.port = port;
  network_config.path = path;
  network_config.device_id = "dshanpi-xiaov";
  if (service_read_token(gateway_token, sizeof(gateway_token))) {
    network_config.token = gateway_token;
  }
  network_config.use_tls = use_tls;
  service->network = xv_network_init(&service->network_storage,
                                     &network_config, &network_callbacks);
  if (service->network == NULL) {
    return false;
  }

  memset(&ui_callbacks, 0, sizeof(ui_callbacks));
  ui_callbacks.context = service;
  ui_callbacks.request_action = service_ui_action;
#if defined(CONFIG_EXAMPLES_XIAOV_KWS) && \
    defined(CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS)
  ui_callbacks.request_offline_command = service_ui_offline_command;
#endif
  ui_callbacks.request_media = service_ui_media_action;
  ui_callbacks.request_display = service_ui_display_action;
  service->ui = xv_ui_init(&service->ui_storage, &ui_callbacks);
  if (service->ui == NULL) {
    return false;
  }

  memset(&power_config, 0, sizeof(power_config));
  power_config.fb_path = CONFIG_EXAMPLES_XIAOV_FRAMEBUFFER_DEVPATH;
  power_config.active_brightness =
      (uint8_t)CONFIG_EXAMPLES_XIAOV_ACTIVE_BRIGHTNESS;
  power_config.standby_brightness =
      (uint8_t)CONFIG_EXAMPLES_XIAOV_STANDBY_BRIGHTNESS;
  power_config.set_animation_enabled = service_set_animation_enabled;
  power_config.animation_context = service;
  service->power = xv_power_init(&service->power_storage, &power_config);
  if (service->power == NULL) {
    return false;
  }

  memset(&music_callbacks, 0, sizeof(music_callbacks));
  music_callbacks.context = service;
  music_callbacks.state_changed = service_music_state;
  music_callbacks.alert_error = service_music_alert_error;
  service->music = xv_music_init(&service->music_storage, &music_callbacks);
  if (service->music == NULL) {
    return false;
  }

  memset(&platform, 0, sizeof(platform));
  platform.audio.context = service;
  platform.audio.start_capture = service_start_capture;
  platform.audio.stop_capture = service_stop_capture;
  platform.audio.start_playback = service_start_playback;
  platform.audio.stop_playback = service_stop_playback;
  platform.audio.start_alert = service_start_alert;
  platform.audio.stop_alert = service_stop_alert;
  platform.network.context = service->network;
  platform.network.ensure_session = xv_network_ensure_session;
  platform.network.send_audio_end = xv_network_request_audio_end;
  platform.network.cancel_turn = xv_network_cancel_turn;
  platform.clock.context = service;
  platform.clock.monotonic_ms = service_monotonic_ms;
  platform.app.context = service;
  platform.app.show_error = service_show_error;
  platform.app.clear_error = service_clear_error;
  platform.app.enter_low_power = service_low_power;
  platform.app.state_changed = service_state_changed;
  if (!xv_runtime_init(&service->runtime, &platform, false)) {
    return false;
  }
  if (!xv_ui_start(service->ui)) {
    return false;
  }
  if (!xv_power_set_active(service->power, false)) {
    printf("xiaov: initial display power capability unavailable\n");
  }
  if (!xv_network_set_touch_capable(
          service->network, xv_ui_is_interactive(service->ui))) {
    return false;
  }
  return xv_network_start(service->network);
}

static void service_deinit(xv_service_t *service) {
  if (service->network != NULL) {
    xv_network_stop(service->network);
  }
  /* Touch callbacks can submit service/media commands; stop the UI before
   * destroying any adapter targeted by those callbacks. */
  if (service->ui != NULL) {
    xv_ui_stop(service->ui);
  }
  if (service->media != NULL) {
    xv_media_deinit(service->media);
    service->media = NULL;
  }
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  /* Join the worker before tearing the model down: it may be inside Invoke, and
   * xv_tflm_kws_shutdown destroys the interpreter it is reading. Capture is
   * already stopped above, so nothing new can be enqueued. */
  if (service->kws_worker_running) {
    pthread_mutex_lock(&service->kws_queue_lock);
    service->kws_worker_stop = true;
    pthread_cond_broadcast(&service->kws_queue_ready);
    pthread_mutex_unlock(&service->kws_queue_lock);
    (void)pthread_join(service->kws_worker, NULL);
    service->kws_worker_running = false;
  }
  if (service->kws_queue_initialized) {
    pthread_cond_destroy(&service->kws_queue_ready);
    pthread_mutex_destroy(&service->kws_queue_lock);
    service->kws_queue_initialized = false;
  }
  if (service->kws_ready) {
    xv_tflm_kws_shutdown();
    service->kws_ready = false;
  }
#endif
  if (service->music != NULL) {
    xv_music_deinit(service->music);
    service->music = NULL;
  }
  if (service->network != NULL) {
    xv_network_deinit(service->network);
    service->network = NULL;
  }
  if (service->power != NULL) {
    xv_power_deinit(service->power);
    service->power = NULL;
  }
  if (service->ui != NULL) {
    xv_ui_deinit(service->ui);
    service->ui = NULL;
  }
  if (service->event_wake_initialized) {
    sem_destroy(&service->event_wake);
    service->event_wake_initialized = false;
  }
  if (service->lock_initialized) {
    pthread_mutex_destroy(&service->lock);
    service->lock_initialized = false;
  }
}

static bool service_next_event(xv_service_t *service, xv_event_t *event,
                               uint64_t deadline_ms) {
  int result;
  for (;;) {
    struct timespec wake_time;
    if (service_monotonic_ms(service) >= deadline_ms ||
        clock_gettime(CLOCK_REALTIME, &wake_time) != 0) {
      return false;
    }
    wake_time.tv_sec += 1;
    do {
      result = sem_timedwait(&service->event_wake, &wake_time);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
      break;
    }
    if (errno != ETIMEDOUT) {
      return false;
    }
  }
  pthread_mutex_lock(&service->lock);
  result = xv_event_queue_pop(&service->event_queue, event) ? 0 : -1;
  pthread_mutex_unlock(&service->lock);
  return result == 0;
}

static xv_runtime_result_t service_activate_reminder(xv_service_t *service) {
  xv_reminder_t reminder;
  const xv_reminder_t *pending;
  xv_runtime_result_t result;

  if (service->runtime.machine.state != XV_STATE_STANDBY) {
    return XV_RUNTIME_IGNORED;
  }
  pthread_mutex_lock(&service->lock);
  pending = xv_reminder_queue_peek(&service->reminders);
  if (service->reminder_active || pending == NULL) {
    pthread_mutex_unlock(&service->lock);
    return XV_RUNTIME_IGNORED;
  }
  reminder = *pending;
  service->reminder_active = true;
  pthread_mutex_unlock(&service->lock);

  if (!xv_ui_post_reminder(service->ui, reminder.label, true)) {
    printf("xiaov: UI dropped active reminder timer=%s\n",
           reminder.timer_id);
  }
  printf("xiaov: reminder active timer=%s label=%s deadline=%.3f\n",
         reminder.timer_id, reminder.label,
         reminder.deadline_epoch);
  result = xv_runtime_dispatch(&service->runtime,
                               XV_APP_EVENT_ALARM_FIRED, NULL);
  if (result != XV_RUNTIME_APPLIED) {
    pthread_mutex_lock(&service->lock);
    service->reminder_active = false;
    pthread_mutex_unlock(&service->lock);
    (void)xv_ui_post_reminder(service->ui, "", false);
  }
  return result;
}

static void service_complete_reminder(xv_service_t *service) {
  xv_reminder_t reminder;
  bool completed = false;

  pthread_mutex_lock(&service->lock);
  if (service->reminder_active) {
    completed = xv_reminder_queue_pop(&service->reminders, &reminder);
    service->reminder_active = false;
  }
  pthread_mutex_unlock(&service->lock);
  if (completed) {
    printf("xiaov: reminder acknowledged timer=%s\n", reminder.timer_id);
    if (!xv_ui_post_reminder(service->ui, "", false)) {
      printf("xiaov: UI dropped reminder clear timer=%s\n",
             reminder.timer_id);
    }
  }
}

static void service_suspend_reminder(xv_service_t *service) {
  bool suspended = false;

  pthread_mutex_lock(&service->lock);
  if (service->reminder_active) {
    service->reminder_active = false;
    suspended = true;
  }
  pthread_mutex_unlock(&service->lock);
  if (suspended) {
    (void)xv_ui_post_reminder(service->ui, "", false);
    printf("xiaov: active reminder suspended until service reset\n");
  }
}

static bool service_release_turn_focus(xv_service_t *service) {
  bool success = true;

  if (service->tts_focus_held) {
    success = xv_music_set_interruption(
                   service->music, XV_MUSIC_REASON_TTS,
                   service_voice_interrupt_policy(), false) &&
              success;
    service->tts_focus_held = false;
  }
  if (service->voice_focus_held) {
    success = xv_music_set_interruption(
                  service->music, XV_MUSIC_REASON_VOICE_TURN,
                  service_voice_interrupt_policy(), false) &&
              success;
    service->voice_focus_held = false;
  }
  return success;
}

static xv_runtime_result_t service_dispatch_event(xv_service_t *service,
                                                   xv_app_event_t event) {
  xv_app_state_t previous = service->runtime.machine.state;
  xv_runtime_result_t result;

  if (event == XV_APP_EVENT_ALARM_FIRED) {
    return service_activate_reminder(service);
  }
  result = xv_runtime_dispatch(&service->runtime, event, NULL);
  if ((service->runtime.machine.state == XV_STATE_STANDBY ||
       service->runtime.machine.state == XV_STATE_ERROR) &&
      !service_release_turn_focus(service)) {
    printf("xiaov: failed to release voice/music focus\n");
    (void)service_post_event(service, XV_APP_EVENT_FAILURE);
  }
  if (service->runtime.machine.state == XV_STATE_ERROR) {
    service_suspend_reminder(service);
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
    service_discard_deferred_media_play(service);
#endif
  }
  if (result == XV_RUNTIME_APPLIED &&
      event == XV_APP_EVENT_ALARM_ACKNOWLEDGED &&
      previous == XV_STATE_ALERTING) {
    service_complete_reminder(service);
  }
  if (result != XV_RUNTIME_ADAPTER_ERROR &&
      service->runtime.machine.state == XV_STATE_STANDBY) {
    xv_runtime_result_t reminder_result = service_activate_reminder(service);
    if (reminder_result == XV_RUNTIME_ADAPTER_ERROR) {
      return reminder_result;
    }
  }
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  /* xv_runtime_dispatch invokes enter_low_power before this function releases
   * voice/TTS focus.  PLAY therefore has to run here, after focus release, not
   * from service_low_power(). */
  if (service->runtime.machine.state == XV_STATE_STANDBY) {
    service_execute_deferred_media_play(service);
  }
#endif
  return result;
}

#ifdef CONFIG_EXAMPLES_XIAOV_KWS
static xv_runtime_result_t service_handle_wake_detected(
    xv_service_t *service) {
  xv_runtime_result_t result;

  if (service->runtime.machine.state != XV_STATE_STANDBY) {
    return XV_RUNTIME_IGNORED;
  }
  pthread_mutex_lock(&service->lock);
  service->wake_prompt_pending = true;
  pthread_mutex_unlock(&service->lock);
  result = service_dispatch_event(service, XV_APP_EVENT_USER_TRIGGER);
  pthread_mutex_lock(&service->lock);
  service->wake_prompt_pending = false;
  pthread_mutex_unlock(&service->lock);
  return result;
}

#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
static xv_runtime_result_t service_handle_offline_request(
    xv_service_t *service) {
  const char *failed_stage = "capture_stop";
  uint64_t failures;

  if (service->runtime.machine.state != XV_STATE_STANDBY) {
    return XV_RUNTIME_IGNORED;
  }
  pthread_mutex_lock(&service->lock);
  service->capture_route = XV_SERVICE_CAPTURE_IDLE;
  pthread_mutex_unlock(&service->lock);

  if (xv_media_stop_capture_and_wait(
          service->media, XV_SERVICE_MUSIC_CAPTURE_STOP_TIMEOUT_MS)) {
    failed_stage = "command_start";
    if (service_start_command_kws(service)) {
      return XV_RUNTIME_APPLIED;
    }
  }

  pthread_mutex_lock(&service->lock);
  service->offline_commands_failed += 1U;
  failures = service->offline_commands_failed;
  pthread_mutex_unlock(&service->lock);
  printf("xiaov: offline command request failed stage=%s total=%llu\n",
         failed_stage, (unsigned long long)failures);
  if (!service_start_standby_kws(service)) {
    printf("xiaov: standby KWS restore after command request failed\n");
    (void)service_post_event(service, XV_APP_EVENT_FAILURE);
    return XV_RUNTIME_ADAPTER_ERROR;
  }
  printf("xiaov: standby KWS restored after command request failure\n");
  return XV_RUNTIME_IGNORED;
}

static xv_runtime_result_t service_handle_offline_command(
    xv_service_t *service, uint16_t class_index) {
  const xv_offline_command_descriptor_t *descriptor;
  xv_offline_command_executor_t executor;
  xv_offline_execution_result_t execution;
  bool restart_ok;

  if (service->runtime.machine.state != XV_STATE_STANDBY) {
    return XV_RUNTIME_IGNORED;
  }
  memset(&executor, 0, sizeof(executor));
  executor.context = service;
  executor.media = service_offline_media;
  executor.adjust_volume = service_offline_adjust_volume;
  executor.set_muted = service_offline_set_muted;
  executor.set_screen_active = service_offline_set_screen_active;
  executor.volume_step_percent =
      (uint8_t)CONFIG_EXAMPLES_XIAOV_OFFLINE_VOLUME_STEP_PERCENT;
  descriptor = xv_offline_command_lookup(class_index);
  execution = xv_offline_command_execute(class_index, &executor);

  pthread_mutex_lock(&service->lock);
  if (execution == XV_OFFLINE_EXECUTION_EXECUTED) {
    service->offline_commands_executed += 1U;
  } else if (execution == XV_OFFLINE_EXECUTION_REQUIRES_CLOUD) {
    service->offline_commands_cloud += 1U;
  } else {
    service->offline_commands_failed += 1U;
  }
  pthread_mutex_unlock(&service->lock);
  printf("xiaov: offline command class=%u label=%s result=%s\n",
         (unsigned int)class_index,
         descriptor == NULL ? "unknown" : descriptor->label,
         xv_offline_execution_result_name(execution));

  if (execution == XV_OFFLINE_EXECUTION_REQUIRES_CLOUD) {
    return service_dispatch_event(service, XV_APP_EVENT_USER_TRIGGER);
  }
  restart_ok = service_start_standby_kws(service);
  if (!restart_ok) {
    (void)service_post_event(service, XV_APP_EVENT_FAILURE);
    return XV_RUNTIME_ADAPTER_ERROR;
  }
  return execution == XV_OFFLINE_EXECUTION_EXECUTED
             ? XV_RUNTIME_APPLIED
             : XV_RUNTIME_IGNORED;
}

static xv_runtime_result_t service_handle_offline_timeout(
    xv_service_t *service) {
  if (service->runtime.machine.state != XV_STATE_STANDBY) {
    return XV_RUNTIME_IGNORED;
  }
  pthread_mutex_lock(&service->lock);
  service->offline_command_timeouts += 1U;
  pthread_mutex_unlock(&service->lock);
  printf("xiaov: offline command window timed out\n");
  if (!service_start_standby_kws(service)) {
    (void)service_post_event(service, XV_APP_EVENT_FAILURE);
    return XV_RUNTIME_ADAPTER_ERROR;
  }
  return XV_RUNTIME_APPLIED;
}
#  endif
#endif

#ifdef CONFIG_EXAMPLES_XIAOV_KWS
static xv_runtime_result_t service_handle_music_idle(xv_service_t *service) {
  bool restore;

  pthread_mutex_lock(&service->lock);
  if (!service->music_restore_pending) {
    pthread_mutex_unlock(&service->lock);
    return XV_RUNTIME_IGNORED;
  }
  restore = service->music_capture_suspended;
  service->music_capture_suspended = false;
  service->music_restore_pending = false;
  pthread_mutex_unlock(&service->lock);
  if (!restore || !service->persistent ||
      service->runtime.machine.state != XV_STATE_STANDBY) {
    return XV_RUNTIME_IGNORED;
  }
  if (!service_start_standby_kws(service)) {
    printf("xiaov: standby KWS restore after music failed\n");
    (void)service_post_event(service, XV_APP_EVENT_FAILURE);
    return XV_RUNTIME_ADAPTER_ERROR;
  }
  printf("xiaov: standby KWS capture restored after music\n");
  return XV_RUNTIME_APPLIED;
}

static xv_runtime_result_t service_handle_music_stream_drained(
    xv_service_t *service) {
  bool active;

  pthread_mutex_lock(&service->lock);
  active = service->music_stream_playing;
  pthread_mutex_unlock(&service->lock);
  if (!active) {
    return XV_RUNTIME_IGNORED;
  }
  service_stop_network_music(service);
  printf("xiaov: network music stream drained\n");
  return XV_RUNTIME_APPLIED;
}
#endif

static const char *service_capture_route_name(
    xv_service_capture_route_t route) {
  switch (route) {
  case XV_SERVICE_CAPTURE_KWS:
    return "kws";
  case XV_SERVICE_CAPTURE_COMMAND:
    return "command";
  case XV_SERVICE_CAPTURE_VOICE:
    return "voice";
  case XV_SERVICE_CAPTURE_IDLE:
  default:
    return "idle";
  }
}

static void service_print_stats(xv_service_t *service, int exit_code,
                                bool daemon, bool heartbeat) {
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  xv_tflm_kws_stats_t kws_stats;
  uint64_t kws_triggers_posted;
  uint64_t kws_triggers_rejected;
  uint64_t kws_processing_errors;
  uint64_t kws_vad_fallbacks;
  uint64_t kws_vad_speech_frames;
  uint64_t kws_vad_total_frames;
  uint64_t kws_windows_queued;
  uint64_t kws_windows_dropped;
  uint64_t wake_prompts_played;
  uint64_t wake_prompts_failed;
  uint32_t kws_queue_peak;
  bool kws_ready_snapshot;
#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
  uint64_t offline_executed;
  uint64_t offline_cloud;
  uint64_t offline_failed;
  uint64_t offline_timeouts;
#  endif
#endif
  xv_power_stats_t power_stats;
  xv_ui_stats_t ui_stats;
  size_t reminders_pending;
  uint64_t reminders_rejected;
  uint64_t event_rejected;
  uint64_t media_succeeded;
  uint64_t media_failed;
  uint32_t capture_frames;
  bool network_failure_posted;
  xv_service_capture_route_t capture_route;
  xv_app_state_t state;
  bool network_connected;
  xv_network_stats_t network_stats;

  memset(&power_stats, 0, sizeof(power_stats));
  memset(&ui_stats, 0, sizeof(ui_stats));
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  memset(&kws_stats, 0, sizeof(kws_stats));
#endif
  xv_power_get_stats(service->power, &power_stats);
  xv_ui_get_stats(service->ui, &ui_stats);
  pthread_mutex_lock(&service->lock);
  reminders_pending = xv_reminder_queue_size(&service->reminders);
  reminders_rejected = xv_reminder_queue_rejected(&service->reminders);
  event_rejected = xv_event_queue_rejected(&service->event_queue);
  media_succeeded = service->media_commands_succeeded;
  media_failed = service->media_commands_failed;
  capture_frames = service->capture_frames;
  network_failure_posted = service->network_failure_posted;
  capture_route = service->capture_route;
  state = service->runtime.machine.state;
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  kws_ready_snapshot = service->kws_ready;
  kws_triggers_posted = service->kws_triggers_posted;
  kws_triggers_rejected = service->kws_triggers_rejected;
  kws_processing_errors = service->kws_processing_errors;
  kws_vad_fallbacks = service->kws_vad_fallbacks;
  kws_vad_speech_frames = service->kws_vad.speech_frames;
  kws_vad_total_frames = service->kws_vad.total_frames;
  wake_prompts_played = service->wake_prompts_played;
  wake_prompts_failed = service->wake_prompts_failed;
#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
  offline_executed = service->offline_commands_executed;
  offline_cloud = service->offline_commands_cloud;
  offline_failed = service->offline_commands_failed;
  offline_timeouts = service->offline_command_timeouts;
#  endif
#endif
  pthread_mutex_unlock(&service->lock);
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  /* Read the engine's stats outside service->lock. get_stats now takes the
   * adapter's own mutex, which the inference worker holds for the duration of an
   * Invoke; nesting it under service->lock would stall the capture thread's VAD
   * update for up to 377 ms every heartbeat. */
  if (kws_ready_snapshot) {
    xv_tflm_kws_get_stats(&kws_stats);
  }
  /* Queue depth lives under its own lock, so read it separately. These three
   * counters are what distinguish "the worker keeps up" from "it falls behind
   * and drops windows", which is the failure this split is meant to remove. */
  pthread_mutex_lock(&service->kws_queue_lock);
  kws_windows_queued = service->kws_windows_queued;
  kws_windows_dropped = service->kws_windows_dropped;
  kws_queue_peak = service->kws_queue_peak;
  pthread_mutex_unlock(&service->kws_queue_lock);
#endif
  network_connected = xv_network_is_connected(service->network);
  xv_network_get_stats(service->network, &network_stats);
  if (heartbeat) {
    printf("xiaov: daemon heartbeat state=%s route=%s capture_frames=%u "
           "network_connected=%u network_cancel=%u capture_drop=%llu "
           "playback_reject=%llu network_drop=%u event_drop=%llu "
           "ui_drop=%llu ui_render_drop=%llu reminder_pending=%u "
           "video_frames=%llu video_replaced=%llu "
           "reminder_drop=%llu media_ok=%llu media_failed=%llu "
           "power_transitions=%llu power_brightness_fail=%llu "
           "power_animation_fail=%llu framebuffer_open=%u\n",
           xv_state_name(state), service_capture_route_name(capture_route),
           (unsigned int)capture_frames, network_connected ? 1U : 0U,
           network_failure_posted ? 1U : 0U,
           (unsigned long long)xv_media_capture_dropped(service->media),
           (unsigned long long)xv_media_playback_rejected(service->media),
           (unsigned int)xv_network_dropped_frames(service->network),
           (unsigned long long)event_rejected,
           (unsigned long long)ui_stats.queue_rejected,
           (unsigned long long)ui_stats.render_rejected,
           (unsigned int)reminders_pending,
           (unsigned long long)ui_stats.video_frames,
           (unsigned long long)ui_stats.video_replaced,
           (unsigned long long)reminders_rejected,
           (unsigned long long)media_succeeded,
           (unsigned long long)media_failed,
           (unsigned long long)power_stats.transitions,
           (unsigned long long)power_stats.brightness_failures,
           (unsigned long long)power_stats.animation_failures,
           power_stats.framebuffer_open ? 1U : 0U);
  } else {
    printf("xiaov: %s finished result=%d capture_drop=%llu "
           "playback_reject=%llu network_drop=%u event_drop=%llu "
           "ui_drop=%llu ui_render_drop=%llu reminder_pending=%u "
           "video_frames=%llu video_replaced=%llu "
           "reminder_drop=%llu media_ok=%llu media_failed=%llu "
           "power_transitions=%llu power_brightness_fail=%llu "
           "power_animation_fail=%llu framebuffer_open=%u\n",
         daemon ? "daemon" : "service", exit_code,
         (unsigned long long)xv_media_capture_dropped(service->media),
         (unsigned long long)xv_media_playback_rejected(service->media),
         (unsigned int)xv_network_dropped_frames(service->network),
         (unsigned long long)event_rejected,
         (unsigned long long)ui_stats.queue_rejected,
         (unsigned long long)ui_stats.render_rejected,
         (unsigned int)reminders_pending,
         (unsigned long long)ui_stats.video_frames,
         (unsigned long long)ui_stats.video_replaced,
         (unsigned long long)reminders_rejected,
         (unsigned long long)media_succeeded,
         (unsigned long long)media_failed,
         (unsigned long long)power_stats.transitions,
         (unsigned long long)power_stats.brightness_failures,
         (unsigned long long)power_stats.animation_failures,
         power_stats.framebuffer_open ? 1U : 0U);
  }
  printf("xiaov: network stats attempts=%llu established=%llu "
         "disconnects=%llu service_errors=%llu pings=%llu pongs=%llu "
         "ping_timeouts=%llu auth_rejections=%llu reconnect_backoff_ms=%u\n",
         (unsigned long long)network_stats.connect_attempts,
         (unsigned long long)network_stats.sessions_established,
         (unsigned long long)network_stats.disconnects,
         (unsigned long long)network_stats.service_errors,
         (unsigned long long)network_stats.pings_sent,
         (unsigned long long)network_stats.pongs_received,
         (unsigned long long)network_stats.ping_timeouts,
         (unsigned long long)network_stats.auth_rejections,
         (unsigned int)network_stats.reconnect_backoff_ms);
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  printf("xiaov: KWS stats pcm=%llu frontend_wait=%llu gated=%llu "
         "inferences=%llu triggers=%llu max_score_q15=%u "
         "max_output_index=%u posted=%llu rejected=%llu "
         "errors=%llu class_switch=%llu arena_used=%lu vad_speech=%llu "
         "vad_total=%llu vad_fallback=%llu queued=%llu qdropped=%llu "
         "qpeak=%u wake_prompt_ok=%llu wake_prompt_fail=%llu\n",
         (unsigned long long)kws_stats.pcm_calls,
         (unsigned long long)kws_stats.frontend_need_more,
         (unsigned long long)kws_stats.inference_gated,
         (unsigned long long)kws_stats.inference_successes,
         (unsigned long long)kws_stats.wake_triggers,
         (unsigned int)kws_stats.max_score_q15,
         (unsigned int)kws_stats.max_output_index,
         (unsigned long long)kws_triggers_posted,
         (unsigned long long)kws_triggers_rejected,
         (unsigned long long)kws_processing_errors,
         (unsigned long long)kws_stats.class_switch_resets,
         (unsigned long)kws_stats.arena_used_bytes,
         (unsigned long long)kws_vad_speech_frames,
         (unsigned long long)kws_vad_total_frames,
         (unsigned long long)kws_vad_fallbacks,
         (unsigned long long)kws_windows_queued,
         (unsigned long long)kws_windows_dropped,
         (unsigned int)kws_queue_peak,
         (unsigned long long)wake_prompts_played,
         (unsigned long long)wake_prompts_failed);
#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
  printf("xiaov: offline stats executed=%llu cloud=%llu failed=%llu "
         "timeouts=%llu\n",
         (unsigned long long)offline_executed,
         (unsigned long long)offline_cloud,
         (unsigned long long)offline_failed,
         (unsigned long long)offline_timeouts);
#  endif
#endif
}

static int service_run(const char *host, uint16_t port, const char *path,
                       bool use_tls, bool daemon) {
  xv_service_t *service = &g_service;
  uint64_t deadline_ms;
  int exit_code = 1;
  bool one_shot_turn_done = false;

  if (!service_claim()) {
    printf("xiaov: another service or daemon is already running\n");
    return 1;
  }
  if (host == NULL || path == NULL || port == 0U ||
      !service_init(service, host, port, path, use_tls)) {
    printf("xiaov: service initialization failed\n");
    service_deinit(service);
    service_release();
    return 1;
  }
  service->persistent = daemon;

  if (!daemon) {
    printf("xiaov: waiting for gateway session ready (timeout=%u ms)\n",
           (unsigned int)CONFIG_EXAMPLES_XIAOV_CONNECT_TIMEOUT_MS);
    if (!service_wait_network_ready(service)) {
      printf("xiaov: gateway session was not ready before capture\n");
      service_deinit(service);
      service_release();
      return 1;
    }
  }

#ifdef CONFIG_EXAMPLES_XIAOV_KWS
  if (daemon && !service_start_standby_kws(service)) {
    printf("xiaov: daemon cannot start standby KWS capture\n");
    service_deinit(service);
    service_release();
    return 1;
  }
#endif

  if (!daemon || !xv_ui_is_interactive(service->ui)) {
    if (daemon) {
      printf("xiaov: serial daemon starts one initial turn; later turns "
             "require touch-capable LVGL\n");
    }
    (void)service_post_event(service, XV_APP_EVENT_USER_TRIGGER);
  } else {
    printf("xiaov: daemon ready; wake word or touch Talk starts listening\n");
  }
  deadline_ms = daemon
                    ? service_monotonic_ms(service) +
                          (uint64_t)CONFIG_EXAMPLES_XIAOV_HEARTBEAT_MS
                    : service_monotonic_ms(service) +
                          (uint64_t)CONFIG_EXAMPLES_XIAOV_TURN_TIMEOUT_MS;
  for (;;) {
    xv_event_t event;
    xv_runtime_result_t result = XV_RUNTIME_IGNORED;
    if (!service_next_event(service, &event, deadline_ms)) {
      if (daemon) {
        /* A daemon has no turn deadline; reuse the wait timeout as a heartbeat
           so KWS counters are observable without ending the run. Wake-word
           bring-up needs to distinguish "no audio reaching KWS" from "audio
           reaching KWS but never scoring above the threshold". */
        service_print_stats(service, exit_code, true, true);
        deadline_ms = service_monotonic_ms(service) +
                      (uint64_t)CONFIG_EXAMPLES_XIAOV_HEARTBEAT_MS;
        continue;
      }
      printf("xiaov: service turn timed out\n");
      break;
    }
    if (event.type <= (uint16_t)XV_APP_EVENT_RESET) {
      xv_app_event_t app_event = (xv_app_event_t)event.type;
      if (app_event == XV_APP_EVENT_END_OF_SPEECH) {
        const char *reason = "external";
        if (event.value == XV_SERVICE_EOS_REASON_SILENCE) {
          reason = "silence";
        } else if (event.value == XV_SERVICE_EOS_REASON_MAX_DURATION) {
          reason = "max_duration";
        }
        printf("xiaov: end of speech reason=%s hangover_ms=%u "
               "max_capture_ms=%u\n",
               reason, (unsigned int)CONFIG_EXAMPLES_XIAOV_CAPTURE_HANGOVER_MS,
               (unsigned int)CONFIG_EXAMPLES_XIAOV_MAX_CAPTURE_MS);
      }
      result = service_dispatch_event(service, app_event);
      if (app_event == XV_APP_EVENT_TURN_COMPLETED) {
        one_shot_turn_done = true;
      }
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
    } else if (event.type == XV_SERVICE_EVENT_WAKE_DETECTED) {
      result = service_handle_wake_detected(service);
#  ifdef CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS
    } else if (event.type == XV_SERVICE_EVENT_OFFLINE_REQUEST) {
      result = service_handle_offline_request(service);
    } else if (event.type == XV_SERVICE_EVENT_OFFLINE_COMMAND) {
      result = service_handle_offline_command(service,
                                              (uint16_t)event.value);
    } else if (event.type == XV_SERVICE_EVENT_OFFLINE_TIMEOUT) {
      result = service_handle_offline_timeout(service);
#  endif
#endif
#ifdef CONFIG_EXAMPLES_XIAOV_KWS
    } else if (event.type == XV_SERVICE_EVENT_MUSIC_IDLE) {
      result = service_handle_music_idle(service);
    } else if (event.type == XV_SERVICE_EVENT_MEDIA_PLAY_DEFERRED) {
      service_execute_deferred_media_play(service);
      result = XV_RUNTIME_APPLIED;
    } else if (event.type == XV_SERVICE_EVENT_MUSIC_STREAM_DRAINED) {
      result = service_handle_music_stream_drained(service);
#endif
    } else {
      printf("xiaov: ignored unknown service event type=%u\n",
             (unsigned int)event.type);
    }
    if (result == XV_RUNTIME_ADAPTER_ERROR && !daemon) {
      break;
    }
    if (!daemon && one_shot_turn_done &&
        service->runtime.machine.state == XV_STATE_STANDBY) {
      exit_code = 0;
      break;
    }
  }

  service_print_stats(service, exit_code, daemon, false);
  service_deinit(service);
  service_release();
  return exit_code;
}

int xv_service_run_once(const char *host, uint16_t port, const char *path,
                        bool use_tls) {
  return service_run(host, port, path, use_tls, false);
}

int xv_service_run_daemon(const char *host, uint16_t port, const char *path,
                          bool use_tls) {
  return service_run(host, port, path, use_tls, true);
}
