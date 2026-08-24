/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifdef __NuttX__
#  include <nuttx/config.h>
#endif

#include "xiaov_music_adapter.h"
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
#  include "xiaov_alsa_backend.h"
#  include "xiaov_local_wav.h"
#endif

#include <errno.h>
#include <media_defs.h>
#ifdef CONFIG_MEDIA_FOCUS
#  include <media_focus.h>
#endif
#include <media_player.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define XV_MUSIC_COMMAND_CAPACITY 8U
#define XV_MUSIC_ALERT_FRAME_SAMPLES 320U
#define XV_MUSIC_ALERT_FRAME_MS 20U
#define XV_MUSIC_ALERT_CYCLE_FRAMES 20U
#define XV_MUSIC_ALERT_TONE_FRAMES 8U
#define XV_MUSIC_PROGRESS_INTERVAL_MS 1000U
#define XV_MUSIC_DUCK_RATIO 0.25f
#define XV_MUSIC_ALERT_VOLUME 0.75f
#define XV_MUSIC_PCM_OPTIONS \
  "format=s16le:sample_rate=16000:ch_layout=mono"

#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
#  define XV_ALSA_MUSIC_FRAME_SAMPLES 320U
#  define XV_ALSA_MUSIC_NOTE_FRAMES 20U
#  define XV_ALSA_MUSIC_TONE_FRAMES 17U
#  define XV_ALSA_MUSIC_AMPLITUDE 12000
#  define XV_ALSA_MUSIC_GAIN_MAX 32767U
#endif

#ifndef CONFIG_EXAMPLES_XIAOV_MUSIC_WORKER_STACKSIZE
#  define CONFIG_EXAMPLES_XIAOV_MUSIC_WORKER_STACKSIZE 16384
#endif

#ifndef CONFIG_EXAMPLES_XIAOV_MUSIC_COMMAND_TIMEOUT_MS
#  define CONFIG_EXAMPLES_XIAOV_MUSIC_COMMAND_TIMEOUT_MS 5000
#endif

#ifndef CONFIG_EXAMPLES_XIAOV_MUSIC_ALSA_WORKER_STACKSIZE
#  define CONFIG_EXAMPLES_XIAOV_MUSIC_ALSA_WORKER_STACKSIZE 8192
#endif

static xv_music_interrupt_policy_t alert_interrupt_policy(void) {
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  /* The alert backend is Media Framework while voice uses ALSA.  Stop music
   * so a paused player cannot retain the shared audiocodec device. */
  return XV_MUSIC_INTERRUPT_STOP;
#else
  return XV_MUSIC_INTERRUPT_PAUSE;
#endif
}

typedef enum {
  XV_INTERNAL_MEDIA_COMMAND = 0,
  XV_INTERNAL_INTERRUPTION,
  XV_INTERNAL_ALERT_START,
  XV_INTERNAL_ALERT_STOP
} xv_internal_command_type_t;

typedef enum {
  XV_PLAYER_COOKIE_MUSIC = 0,
  XV_PLAYER_COOKIE_ALERT
} xv_player_cookie_kind_t;

typedef struct {
  xv_music_adapter_t *adapter;
  xv_player_cookie_kind_t kind;
} xv_player_cookie_t;

typedef struct {
  sem_t done;
  xv_internal_command_type_t type;
  xv_music_action_t action;
  xv_music_interrupt_reason_t reason;
  xv_music_interrupt_policy_t policy;
  char source[XV_MUSIC_SOURCE_CAPACITY];
  uint32_t value;
  xv_music_command_result_t result;
  bool active;
  bool occupied;
  bool synchronous;
  bool executing;
  bool completed;
  bool canceled;
  bool semaphore_initialized;
} xv_music_command_slot_t;

#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
typedef enum {
  XV_ALSA_MUSIC_STOPPED = 0,
  XV_ALSA_MUSIC_PAUSED,
  XV_ALSA_MUSIC_PLAYING
} xv_alsa_music_state_t;
#endif

struct xv_music_adapter {
  pthread_mutex_t lock;
  pthread_mutex_t snapshot_lock;
  sem_t wake;
  pthread_t thread;
  xv_music_callbacks_t callbacks;
  xv_music_model_t model;
  xv_music_snapshot_t published_snapshot;
  xv_music_command_slot_t commands[XV_MUSIC_COMMAND_CAPACITY];
  uint8_t command_queue[XV_MUSIC_COMMAND_CAPACITY];
  size_t command_read;
  size_t command_write;
  size_t command_count;

  void *music_player;
  void *music_focus;
  void *alert_player;
  void *alert_focus;
  char loaded_source[XV_MUSIC_SOURCE_CAPACITY];
  xv_player_cookie_t music_cookie;
  xv_player_cookie_t alert_cookie;
  int pending_music_focus;
  int pending_alert_focus;
  int pending_music_event;
  int pending_music_result;
  int pending_alert_event;
  int pending_alert_result;
  uint32_t alert_frame_index;
  int16_t alert_frame[XV_MUSIC_ALERT_FRAME_SAMPLES];
  bool music_focus_pending;
  bool alert_focus_pending;
  bool music_event_pending;
  bool alert_event_pending;
  bool music_started;
  bool music_paused;
  bool alert_active;
  pthread_cond_t alert_stop_condition;
  uint64_t alert_stop_generation;
  uint64_t alert_stop_completed_generation;
  bool alert_stop_requested;
  bool alert_stop_result;
  bool lock_initialized;
  bool snapshot_lock_initialized;
  bool alert_stop_condition_initialized;
  bool wake_initialized;
  bool thread_started;
  bool initialized;
  bool shutting_down;
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  xv_alsa_playback_backend_t alsa_music;
  pthread_t alsa_music_thread;
  sem_t alsa_music_wake;
  pthread_cond_t alsa_music_condition;
  uint64_t alsa_music_request_generation;
  uint64_t alsa_music_completed_generation;
  uint64_t alsa_music_played_samples;
  uint32_t alsa_music_phase;
  uint32_t alsa_music_note_frame;
  uint16_t alsa_music_gain_q15;
  int alsa_music_result;
  int alsa_music_failure_result;
  xv_alsa_music_state_t alsa_music_target;
  xv_local_wav_t local_wav;
  bool alsa_music_selected;
  bool alsa_music_local_file;
  bool alsa_music_reset_pending;
  bool alsa_music_failed;
  bool alsa_music_condition_initialized;
  bool alsa_music_wake_initialized;
  bool alsa_music_thread_started;
#endif
};

_Static_assert(sizeof(struct xv_music_adapter) <=
                   XV_MUSIC_ADAPTER_STORAGE_SIZE,
               "XV_MUSIC_ADAPTER_STORAGE_SIZE is too small");

static bool bounded_copy(char *destination, size_t capacity,
                         const char *source) {
  size_t index;

  if (destination == NULL || capacity == 0U || source == NULL) {
    return false;
  }
  for (index = 0U; index < capacity; ++index) {
    destination[index] = source[index];
    if (source[index] == '\0') {
      return true;
    }
  }
  destination[0] = '\0';
  return false;
}

static void result_set(xv_music_command_result_t *result, bool ok,
                       xv_music_error_t error, int platform_result) {
  result->ok = ok;
  result->error = error;
  result->platform_result = platform_result;
}

static xv_music_error_t model_error(xv_music_model_result_t result) {
  switch (result) {
  case XV_MUSIC_MODEL_OK:
    return XV_MUSIC_ERROR_NONE;
  case XV_MUSIC_MODEL_INVALID_SOURCE:
    return XV_MUSIC_ERROR_INVALID_SOURCE;
  case XV_MUSIC_MODEL_PLAYLIST_FULL:
    return XV_MUSIC_ERROR_PLAYLIST_FULL;
  case XV_MUSIC_MODEL_PLAYLIST_EMPTY:
    return XV_MUSIC_ERROR_PLAYLIST_EMPTY;
  case XV_MUSIC_MODEL_NO_TRACK:
    return XV_MUSIC_ERROR_NO_TRACK;
  case XV_MUSIC_MODEL_OUT_OF_RANGE:
    return XV_MUSIC_ERROR_OUT_OF_RANGE;
  case XV_MUSIC_MODEL_BAD_ARGUMENT:
  default:
    return XV_MUSIC_ERROR_INVALID_ARGUMENT;
  }
}

static void notify_state(xv_music_adapter_t *adapter) {
  xv_music_snapshot_t snapshot;

  xv_music_model_snapshot(&adapter->model, &snapshot);
  pthread_mutex_lock(&adapter->snapshot_lock);
  adapter->published_snapshot = snapshot;
  pthread_mutex_unlock(&adapter->snapshot_lock);
  if (adapter->callbacks.state_changed != NULL) {
    adapter->callbacks.state_changed(adapter->callbacks.context, &snapshot);
  }
}

static void player_event(void *context, int event, int result,
                         const char *extra) {
  xv_player_cookie_t *cookie = context;
  xv_music_adapter_t *adapter;

  (void)extra;
  if (cookie == NULL || cookie->adapter == NULL) {
    return;
  }
  adapter = cookie->adapter;
  pthread_mutex_lock(&adapter->lock);
  if (cookie->kind == XV_PLAYER_COOKIE_MUSIC) {
    /* A completion can be followed immediately by STOPPED.  Retain the
     * completion until the worker consumes it, otherwise playlist advance is
     * lost.  A failing event is retained for the same reason. */
    if (!adapter->music_event_pending || result < 0 ||
        (adapter->pending_music_result >= 0 &&
         adapter->pending_music_event != MEDIA_EVENT_COMPLETED)) {
      adapter->pending_music_event = event;
      adapter->pending_music_result = result;
      adapter->music_event_pending = true;
    }
  } else {
    /* Do not let the normal STOPPED notification mask an alert failure. */
    if (!adapter->alert_event_pending || result < 0) {
      adapter->pending_alert_event = event;
      adapter->pending_alert_result = result;
      adapter->alert_event_pending = true;
    }
  }
  pthread_mutex_unlock(&adapter->lock);
  (void)sem_post(&adapter->wake);
}

#ifdef CONFIG_MEDIA_FOCUS
static void focus_suggestion(int suggestion, int request_id, void *context) {
  xv_player_cookie_t *cookie = context;
  xv_music_adapter_t *adapter;

  (void)request_id;
  if (cookie == NULL || cookie->adapter == NULL) {
    return;
  }
  adapter = cookie->adapter;
  pthread_mutex_lock(&adapter->lock);
  if (cookie->kind == XV_PLAYER_COOKIE_MUSIC) {
    adapter->pending_music_focus = suggestion;
    adapter->music_focus_pending = true;
  } else {
    adapter->pending_alert_focus = suggestion;
    adapter->alert_focus_pending = true;
  }
  pthread_mutex_unlock(&adapter->lock);
  (void)sem_post(&adapter->wake);
}
#endif

static int request_focus(xv_player_cookie_t *cookie, const char *scenario,
                         void **handle, int *initial_suggestion) {
#ifdef CONFIG_MEDIA_FOCUS
  *handle = media_focus_request2(initial_suggestion, scenario,
                                 focus_suggestion, 1, cookie);
  return *handle == NULL ? -ENODEV : 0;
#else
  (void)cookie;
  (void)scenario;
  *handle = NULL;
  *initial_suggestion = MEDIA_FOCUS_PLAY;
  return 0;
#endif
}

static int abandon_focus(void **handle) {
#ifdef CONFIG_MEDIA_FOCUS
  int result;
  if (*handle == NULL) {
    return 0;
  }
  result = media_focus_abandon(*handle);
  *handle = NULL;
  return result;
#else
  *handle = NULL;
  return 0;
#endif
}

static float music_volume(const xv_music_adapter_t *adapter) {
  float volume = (float)adapter->model.volume_percent / 100.0f;
  if (adapter->model.output == XV_MUSIC_OUTPUT_DUCKED) {
    volume *= XV_MUSIC_DUCK_RATIO;
  }
  return volume;
}

#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
static void add_milliseconds(struct timespec *time, uint32_t milliseconds);

static void alsa_music_set_gain(xv_music_adapter_t *adapter, float volume) {
  uint32_t gain;

  if (volume <= 0.0f) {
    gain = 0U;
  } else if (volume >= 1.0f) {
    gain = XV_ALSA_MUSIC_GAIN_MAX;
  } else {
    gain = (uint32_t)(volume * (float)XV_ALSA_MUSIC_GAIN_MAX + 0.5f);
  }
  pthread_mutex_lock(&adapter->lock);
  adapter->alsa_music_gain_q15 = (uint16_t)gain;
  pthread_mutex_unlock(&adapter->lock);
}

static void alsa_music_generate_frame(xv_music_adapter_t *adapter,
                                      int16_t *samples) {
  static const uint16_t frequencies[] = {
      523U, 659U, 784U, 659U, 587U, 698U, 880U, 698U,
  };
  uint32_t note_index =
      (adapter->alsa_music_note_frame / XV_ALSA_MUSIC_NOTE_FRAMES) %
      (sizeof(frequencies) / sizeof(frequencies[0]));
  uint32_t frame_in_note =
      adapter->alsa_music_note_frame % XV_ALSA_MUSIC_NOTE_FRAMES;
  uint32_t increment =
      (uint32_t)(((uint64_t)frequencies[note_index] << 32) / 16000U);
  uint16_t gain;
  size_t index;

  pthread_mutex_lock(&adapter->lock);
  gain = adapter->alsa_music_gain_q15;
  pthread_mutex_unlock(&adapter->lock);
  if (frame_in_note == 0U) {
    adapter->alsa_music_phase = 0U;
  }
  for (index = 0U; index < XV_ALSA_MUSIC_FRAME_SAMPLES; ++index) {
    int32_t saw = (int32_t)(adapter->alsa_music_phase >> 16);
    int32_t triangle =
        saw < 32768 ? saw * 2 - 32768 : 98303 - saw * 2;
    int64_t scaled;

    adapter->alsa_music_phase += increment;
    if (frame_in_note >= XV_ALSA_MUSIC_TONE_FRAMES) {
      samples[index] = 0;
      continue;
    }
    scaled = (int64_t)triangle * XV_ALSA_MUSIC_AMPLITUDE * gain;
    samples[index] =
        (int16_t)(scaled / (32768LL * XV_ALSA_MUSIC_GAIN_MAX));
  }
  adapter->alsa_music_note_frame += 1U;
}

static int alsa_music_read_local_frame(xv_music_adapter_t *adapter,
                                       int16_t *samples,
                                       size_t *samples_read,
                                       bool *looped) {
  uint16_t gain;
  bool eof;
  size_t index;
  int result;

  result = xv_local_wav_read(&adapter->local_wav, samples,
                             XV_ALSA_MUSIC_FRAME_SAMPLES,
                             samples_read, &eof);
  if (result < 0) {
    return result;
  }
  memset(samples + *samples_read, 0,
         (XV_ALSA_MUSIC_FRAME_SAMPLES - *samples_read) * sizeof(*samples));
  pthread_mutex_lock(&adapter->lock);
  gain = adapter->alsa_music_gain_q15;
  pthread_mutex_unlock(&adapter->lock);
  for (index = 0U; index < *samples_read; ++index) {
    samples[index] =
        (int16_t)(((int64_t)samples[index] * gain) /
                  XV_ALSA_MUSIC_GAIN_MAX);
  }
  *looped = false;
  if (eof) {
    result = xv_local_wav_rewind(&adapter->local_wav);
    if (result < 0) {
      return result;
    }
    *looped = true;
  }
  return 0;
}

static void alsa_music_complete_request(xv_music_adapter_t *adapter,
                                        uint64_t generation, int result) {
  pthread_mutex_lock(&adapter->lock);
  adapter->alsa_music_result = result;
  adapter->alsa_music_completed_generation = generation;
  pthread_cond_broadcast(&adapter->alsa_music_condition);
  pthread_mutex_unlock(&adapter->lock);
}

static void *alsa_music_worker(void *context) {
  xv_music_adapter_t *adapter = context;
  uint64_t handled_generation = 0U;
  int16_t frame[XV_ALSA_MUSIC_FRAME_SAMPLES];

  for (;;) {
    uint64_t generation;
    xv_alsa_music_state_t target;
    bool reset;
    bool shutting_down;
    int result = 0;

    pthread_mutex_lock(&adapter->lock);
    generation = adapter->alsa_music_request_generation;
    target = adapter->alsa_music_target;
    reset = adapter->alsa_music_reset_pending;
    shutting_down = adapter->shutting_down;
    pthread_mutex_unlock(&adapter->lock);

    if (shutting_down) {
      (void)xv_alsa_playback_backend_close(&adapter->alsa_music);
      return NULL;
    }
    if (generation != handled_generation) {
      if (target != XV_ALSA_MUSIC_PLAYING) {
        result = xv_alsa_playback_backend_close(&adapter->alsa_music);
      } else if (!xv_alsa_playback_backend_is_open(&adapter->alsa_music)) {
        result = xv_alsa_playback_backend_open(&adapter->alsa_music);
      }
      if (reset) {
        adapter->alsa_music_phase = 0U;
        adapter->alsa_music_note_frame = 0U;
        if (result >= 0 && adapter->alsa_music_local_file) {
          result = xv_local_wav_rewind(&adapter->local_wav);
        }
        pthread_mutex_lock(&adapter->lock);
        adapter->alsa_music_played_samples = 0U;
        adapter->alsa_music_reset_pending = false;
        pthread_mutex_unlock(&adapter->lock);
      }
      if (result < 0) {
        pthread_mutex_lock(&adapter->lock);
        adapter->alsa_music_target = XV_ALSA_MUSIC_STOPPED;
        pthread_mutex_unlock(&adapter->lock);
      }
      handled_generation = generation;
      alsa_music_complete_request(adapter, generation, result);
      if (result < 0) {
        (void)xv_alsa_playback_backend_close(&adapter->alsa_music);
      }
      continue;
    }
    if (target != XV_ALSA_MUSIC_PLAYING ||
        !xv_alsa_playback_backend_is_open(&adapter->alsa_music)) {
      int wait_result;
      do {
        wait_result = sem_wait(&adapter->alsa_music_wake);
      } while (wait_result < 0 && errno == EINTR);
      continue;
    }

    {
      size_t samples_read = XV_ALSA_MUSIC_FRAME_SAMPLES;
      bool looped = false;

      if (adapter->alsa_music_local_file) {
        result = alsa_music_read_local_frame(adapter, frame,
                                             &samples_read, &looped);
      } else {
        alsa_music_generate_frame(adapter, frame);
      }
      if (result >= 0) {
        result = xv_alsa_playback_backend_write(
            &adapter->alsa_music, frame, XV_ALSA_MUSIC_FRAME_SAMPLES);
      }
      if (result >= 0) {
        pthread_mutex_lock(&adapter->lock);
        if (looped) {
          adapter->alsa_music_played_samples = 0U;
        } else {
          adapter->alsa_music_played_samples += samples_read;
        }
        pthread_mutex_unlock(&adapter->lock);
      }
    }
    if (result < 0) {
      (void)xv_alsa_playback_backend_close(&adapter->alsa_music);
      pthread_mutex_lock(&adapter->lock);
      adapter->alsa_music_target = XV_ALSA_MUSIC_STOPPED;
      adapter->alsa_music_failure_result = result;
      adapter->alsa_music_failed = true;
      pthread_mutex_unlock(&adapter->lock);
      (void)sem_post(&adapter->wake);
      continue;
    }
  }
}

static int alsa_music_transition(xv_music_adapter_t *adapter,
                                 xv_alsa_music_state_t target, bool reset) {
  struct timespec deadline;
  uint64_t generation;
  int wait_result = 0;
  int result;

  if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
    return -errno;
  }
  add_milliseconds(&deadline, CONFIG_EXAMPLES_XIAOV_MUSIC_COMMAND_TIMEOUT_MS);
  pthread_mutex_lock(&adapter->lock);
  if (adapter->shutting_down || !adapter->alsa_music_thread_started) {
    pthread_mutex_unlock(&adapter->lock);
    return -ESHUTDOWN;
  }
  adapter->alsa_music_request_generation += 1U;
  if (adapter->alsa_music_request_generation == 0U) {
    adapter->alsa_music_request_generation = 1U;
  }
  generation = adapter->alsa_music_request_generation;
  adapter->alsa_music_target = target;
  adapter->alsa_music_reset_pending = reset;
  pthread_mutex_unlock(&adapter->lock);
  (void)sem_post(&adapter->alsa_music_wake);

  pthread_mutex_lock(&adapter->lock);
  while (adapter->alsa_music_completed_generation < generation &&
         !adapter->shutting_down && wait_result == 0) {
    wait_result = pthread_cond_timedwait(&adapter->alsa_music_condition,
                                        &adapter->lock, &deadline);
  }
  if (adapter->alsa_music_completed_generation >= generation) {
    result = adapter->alsa_music_result;
  } else if (wait_result == ETIMEDOUT) {
    result = -ETIMEDOUT;
  } else if (adapter->shutting_down) {
    result = -ESHUTDOWN;
  } else {
    result = wait_result == 0 ? -EIO : -wait_result;
  }
  pthread_mutex_unlock(&adapter->lock);
  return result;
}

static int select_alsa_music_fallback(xv_music_adapter_t *adapter,
                                      const char *source, int media_result,
                                      xv_music_error_t *error) {
  int result;

  if (media_result != -ENODEV) {
    *error = XV_MUSIC_ERROR_PREPARE_FAILED;
    return media_result;
  }
  adapter->alsa_music_local_file = false;
  xv_local_wav_close(&adapter->local_wav);
  if (source[0] != '/') {
    *error = XV_MUSIC_ERROR_PREPARE_FAILED;
    printf("xiaov-music: remote source unsupported without media backend "
           "source=%s\n",
           source);
    return -ENOTSUP;
  }
  result = xv_local_wav_open(&adapter->local_wav, source);
  if (result < 0) {
    *error = XV_MUSIC_ERROR_PREPARE_FAILED;
    printf("xiaov-music: local WAV rejected source=%s result=%d\n",
           source, result);
    return result;
  }
  adapter->alsa_music_local_file = true;
  xv_music_model_update_progress(
      &adapter->model, 0U,
      xv_local_wav_duration_ms(&adapter->local_wav));
  adapter->alsa_music_selected = true;
  pthread_mutex_lock(&adapter->lock);
  adapter->alsa_music_failed = false;
  pthread_mutex_unlock(&adapter->lock);
  adapter->music_started = false;
  adapter->music_paused = false;
  (void)bounded_copy(adapter->loaded_source,
                     sizeof(adapter->loaded_source), source);
  printf("xiaov-music: media device unavailable; using local WAV source=%s "
         "duration_ms=%u\n",
         source,
         (unsigned int)xv_local_wav_duration_ms(&adapter->local_wav));
  return 0;
}
#endif

static int close_music(xv_music_adapter_t *adapter) {
  int result = 0;
  int current;

#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  if (adapter->alsa_music_selected) {
    current = alsa_music_transition(adapter, XV_ALSA_MUSIC_STOPPED, false);
    if (current < 0 && current != -ESHUTDOWN) {
      result = current;
    }
    if (current >= 0) {
      pthread_mutex_lock(&adapter->lock);
      adapter->alsa_music_failed = false;
      pthread_mutex_unlock(&adapter->lock);
      adapter->alsa_music_selected = false;
      adapter->alsa_music_local_file = false;
      xv_local_wav_close(&adapter->local_wav);
    }
  }
#endif
  if (adapter->music_player != NULL) {
    if (adapter->music_started || adapter->music_paused) {
      current = media_player_stop(adapter->music_player);
      if (current < 0) {
        result = current;
      }
    }
    current = media_player_close(adapter->music_player, 0);
    if (current < 0 && result == 0) {
      result = current;
    }
    adapter->music_player = NULL;
  }
  current = abandon_focus(&adapter->music_focus);
  if (current < 0 && result == 0) {
    result = current;
  }
  adapter->music_started = false;
  adapter->music_paused = false;
  adapter->loaded_source[0] = '\0';
  return result;
}

static int apply_initial_music_focus(xv_music_adapter_t *adapter,
                                     int suggestion) {
  xv_music_interrupt_policy_t policy;
  bool active = true;

  if (suggestion == MEDIA_FOCUS_PLAY ||
      suggestion == MEDIA_FOCUS_PLAY_WITH_KEEP) {
    active = false;
    policy = XV_MUSIC_INTERRUPT_PAUSE;
  } else if (suggestion == MEDIA_FOCUS_PLAY_WITH_DUCK) {
    policy = XV_MUSIC_INTERRUPT_DUCK;
  } else if (suggestion == MEDIA_FOCUS_PAUSE ||
             suggestion == MEDIA_FOCUS_PLAY_BUT_SILENT) {
    policy = XV_MUSIC_INTERRUPT_PAUSE;
  } else {
    policy = XV_MUSIC_INTERRUPT_STOP;
  }
  (void)xv_music_model_set_interruption(
      &adapter->model, XV_MUSIC_REASON_PLATFORM_FOCUS, policy, active);
  return active && policy != XV_MUSIC_INTERRUPT_DUCK ? -EBUSY : 0;
}

static int open_music(xv_music_adapter_t *adapter,
                      xv_music_error_t *error) {
  const xv_music_track_t *track = xv_music_model_current(&adapter->model);
  void *player;
  int suggestion;
  int result;

  if (track == NULL) {
    *error = XV_MUSIC_ERROR_NO_TRACK;
    return -ENOENT;
  }
  result = request_focus(&adapter->music_cookie, MEDIA_SCENARIO_MUSIC,
                         &adapter->music_focus, &suggestion);
  if (result < 0) {
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
    return select_alsa_music_fallback(adapter, track->source, result, error);
#else
    *error = XV_MUSIC_ERROR_FOCUS_BLOCKED;
    return result;
#endif
  }
  result = apply_initial_music_focus(adapter, suggestion);
  if (result < 0) {
    (void)abandon_focus(&adapter->music_focus);
    *error = XV_MUSIC_ERROR_FOCUS_BLOCKED;
    return result;
  }

  player = media_player_open(MEDIA_STREAM_MUSIC);
  if (player == NULL) {
    (void)abandon_focus(&adapter->music_focus);
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
    return select_alsa_music_fallback(adapter, track->source, -ENODEV,
                                      error);
#else
    *error = XV_MUSIC_ERROR_PREPARE_FAILED;
    return -ENODEV;
#endif
  }
  result = media_player_set_event_callback(player, &adapter->music_cookie,
                                           player_event);
  if (result >= 0) {
    result = media_player_prepare(player, track->source, NULL);
  }
  if (result >= 0) {
    result = media_player_set_volume(player, music_volume(adapter));
  }
  if (result >= 0 && adapter->model.position_ms > 0U) {
    result = media_player_seek(player, adapter->model.position_ms);
  }
  if (result < 0) {
    (void)media_player_close(player, 0);
    (void)abandon_focus(&adapter->music_focus);
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
    return select_alsa_music_fallback(adapter, track->source, result, error);
#else
    *error = XV_MUSIC_ERROR_PREPARE_FAILED;
    return result;
#endif
  }
  adapter->music_player = player;
  (void)bounded_copy(adapter->loaded_source,
                     sizeof(adapter->loaded_source), track->source);
  adapter->music_started = false;
  adapter->music_paused = false;
  return 0;
}

static int reconcile_music(xv_music_adapter_t *adapter, bool force_reload,
                           xv_music_error_t *error) {
  const xv_music_track_t *track = xv_music_model_current(&adapter->model);
  xv_music_output_state_t output = adapter->model.output;
  bool backend_missing;
  int result;

  *error = XV_MUSIC_ERROR_NONE;
  if (output == XV_MUSIC_OUTPUT_ERROR) {
    *error = XV_MUSIC_ERROR_PLAYBACK_FAILED;
    return -EIO;
  }
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  if (adapter->alsa_music_selected) {
    alsa_music_set_gain(adapter, music_volume(adapter));
  }
#endif
  if (output == XV_MUSIC_OUTPUT_STOPPED) {
    result = close_music(adapter);
    if (result < 0) {
      *error = XV_MUSIC_ERROR_PLAYBACK_FAILED;
    }
    return result;
  }
  if (output == XV_MUSIC_OUTPUT_PAUSED) {
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
    if (adapter->alsa_music_selected && adapter->music_started &&
        !adapter->music_paused) {
      result = alsa_music_transition(adapter, XV_ALSA_MUSIC_PAUSED, false);
      if (result < 0) {
        *error = XV_MUSIC_ERROR_PLAYBACK_FAILED;
        return result;
      }
      adapter->music_paused = true;
      return 0;
    }
#endif
    if (adapter->music_player != NULL && adapter->music_started &&
        !adapter->music_paused) {
      result = media_player_pause(adapter->music_player);
      if (result < 0) {
        *error = XV_MUSIC_ERROR_PLAYBACK_FAILED;
        return result;
      }
      adapter->music_paused = true;
    }
    return 0;
  }
  if (track == NULL) {
    *error = XV_MUSIC_ERROR_NO_TRACK;
    return -ENOENT;
  }
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  backend_missing =
      !adapter->alsa_music_selected && adapter->music_player == NULL;
#else
  backend_missing = adapter->music_player == NULL;
#endif
  if (force_reload || backend_missing ||
      strcmp(adapter->loaded_source, track->source) != 0) {
    result = close_music(adapter);
    if (result < 0) {
      *error = XV_MUSIC_ERROR_PLAYBACK_FAILED;
      return result;
    }
    result = open_music(adapter, error);
    if (result < 0) {
      return result;
    }
  }
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  if (adapter->alsa_music_selected) {
    alsa_music_set_gain(adapter, music_volume(adapter));
    if (!adapter->music_started || adapter->music_paused) {
      result = alsa_music_transition(adapter, XV_ALSA_MUSIC_PLAYING,
                                     !adapter->music_started);
      if (result < 0) {
        *error = XV_MUSIC_ERROR_PLAYBACK_FAILED;
        return result;
      }
      adapter->music_started = true;
      adapter->music_paused = false;
    }
    return 0;
  }
#endif
  result = media_player_set_volume(adapter->music_player,
                                   music_volume(adapter));
  if (result < 0) {
    *error = XV_MUSIC_ERROR_PLAYBACK_FAILED;
    return result;
  }
  if (!adapter->music_started || adapter->music_paused) {
    result = media_player_start(adapter->music_player);
    if (result < 0) {
      *error = XV_MUSIC_ERROR_PLAYBACK_FAILED;
      return result;
    }
    adapter->music_started = true;
    adapter->music_paused = false;
  }
  return 0;
}

static bool music_is_locally_blocked(const xv_music_adapter_t *adapter) {
  uint32_t platform = (uint32_t)XV_MUSIC_REASON_PLATFORM_FOCUS;
  return (adapter->model.pause_reasons & ~platform) != 0U ||
         (adapter->model.stop_reasons & ~platform) != 0U;
}

static void command_model_failure(xv_music_command_result_t *result,
                                  xv_music_model_result_t model_result) {
  result_set(result, false, model_error(model_result), 0);
}

static void execute_media_command(xv_music_adapter_t *adapter,
                                  const xv_music_command_slot_t *command,
                                  xv_music_command_result_t *result) {
  xv_music_model_result_t model_result = XV_MUSIC_MODEL_OK;
  xv_music_error_t error = XV_MUSIC_ERROR_NONE;
  int platform_result = 0;
  bool force_reload = false;

  xv_music_model_set_error(&adapter->model, false);
  if (command->action == XV_MUSIC_ACTION_PLAY) {
    if (music_is_locally_blocked(adapter)) {
      result_set(result, false, XV_MUSIC_ERROR_FOCUS_BLOCKED, -EBUSY);
      return;
    }
    model_result = xv_music_model_play(&adapter->model, command->source,
                                       NULL, NULL);
    force_reload = true;
  } else if (command->action == XV_MUSIC_ACTION_PAUSE) {
    model_result = xv_music_model_pause(&adapter->model);
  } else if (command->action == XV_MUSIC_ACTION_RESUME) {
    if (music_is_locally_blocked(adapter)) {
      result_set(result, false, XV_MUSIC_ERROR_FOCUS_BLOCKED, -EBUSY);
      return;
    }
    (void)xv_music_model_set_interruption(
        &adapter->model, XV_MUSIC_REASON_PLATFORM_FOCUS,
        XV_MUSIC_INTERRUPT_PAUSE, false);
    model_result = xv_music_model_resume(&adapter->model);
  } else if (command->action == XV_MUSIC_ACTION_STOP) {
    model_result = xv_music_model_stop(&adapter->model);
  } else if (command->action == XV_MUSIC_ACTION_NEXT ||
             command->action == XV_MUSIC_ACTION_PREVIOUS) {
    if (music_is_locally_blocked(adapter)) {
      result_set(result, false, XV_MUSIC_ERROR_FOCUS_BLOCKED, -EBUSY);
      return;
    }
    model_result = command->action == XV_MUSIC_ACTION_NEXT
                       ? xv_music_model_next(&adapter->model)
                       : xv_music_model_previous(&adapter->model);
    force_reload = true;
  } else if (command->action == XV_MUSIC_ACTION_SET_VOLUME) {
    model_result = xv_music_model_set_volume(&adapter->model, command->value);
  } else if (command->action == XV_MUSIC_ACTION_SEEK) {
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
    if (adapter->alsa_music_selected) {
      result_set(result, false, XV_MUSIC_ERROR_PLAYBACK_FAILED, -ENOTSUP);
      return;
    }
#endif
    if (adapter->music_player == NULL) {
      result_set(result, false, XV_MUSIC_ERROR_NO_TRACK, -ENOENT);
      return;
    }
    if (adapter->model.duration_ms != 0U &&
        command->value > adapter->model.duration_ms) {
      result_set(result, false, XV_MUSIC_ERROR_OUT_OF_RANGE, -ERANGE);
      return;
    }
    platform_result = media_player_seek(adapter->music_player, command->value);
    if (platform_result < 0) {
      result_set(result, false, XV_MUSIC_ERROR_PLAYBACK_FAILED,
                 platform_result);
      return;
    }
    model_result = xv_music_model_seek(&adapter->model, command->value);
  } else {
    result_set(result, false, XV_MUSIC_ERROR_INVALID_ACTION, -EINVAL);
    return;
  }

  if (model_result != XV_MUSIC_MODEL_OK) {
    command_model_failure(result, model_result);
    return;
  }
  if (command->action != XV_MUSIC_ACTION_SEEK) {
    platform_result = reconcile_music(adapter, force_reload, &error);
  }
  if (platform_result < 0) {
    xv_music_model_set_error(&adapter->model, true);
    result_set(result, false, error, platform_result);
  } else {
    result_set(result, true, XV_MUSIC_ERROR_NONE, 0);
  }
  notify_state(adapter);
}

static int close_alert(xv_music_adapter_t *adapter) {
  int result = 0;
  int current;

  if (adapter->alert_player != NULL) {
    media_player_close_socket(adapter->alert_player);
    current = media_player_stop(adapter->alert_player);
    if (current < 0) {
      result = current;
    }
    current = media_player_close(adapter->alert_player, 0);
    if (current < 0 && result == 0) {
      result = current;
    }
    adapter->alert_player = NULL;
  }
  current = abandon_focus(&adapter->alert_focus);
  if (current < 0 && result == 0) {
    result = current;
  }
  adapter->alert_active = false;
  adapter->alert_frame_index = 0U;
  return result;
}

static int open_alert(xv_music_adapter_t *adapter) {
  void *player;
  int suggestion;
  int result;

  result = request_focus(&adapter->alert_cookie, MEDIA_SCENARIO_ALARM,
                         &adapter->alert_focus, &suggestion);
  if (result < 0) {
    return result;
  }
  if (suggestion == MEDIA_FOCUS_STOP || suggestion == MEDIA_FOCUS_PAUSE ||
      suggestion == MEDIA_FOCUS_PLAY_BUT_SILENT) {
    (void)abandon_focus(&adapter->alert_focus);
    return -EBUSY;
  }
  player = media_player_open(MEDIA_STREAM_ALARM);
  if (player == NULL) {
    (void)abandon_focus(&adapter->alert_focus);
    return -ENODEV;
  }
  result = media_player_set_event_callback(player, &adapter->alert_cookie,
                                           player_event);
  if (result >= 0) {
    result = media_player_prepare(player, NULL, XV_MUSIC_PCM_OPTIONS);
  }
  if (result >= 0) {
    result = media_player_set_volume(player, XV_MUSIC_ALERT_VOLUME);
  }
  if (result >= 0) {
    result = media_player_start(player);
  }
  if (result < 0) {
    (void)media_player_close(player, 0);
    (void)abandon_focus(&adapter->alert_focus);
    return result;
  }
  adapter->alert_player = player;
  adapter->alert_active = true;
  adapter->alert_frame_index = 0U;
  return 0;
}

static int write_alert_frame(xv_music_adapter_t *adapter) {
  const uint8_t *data;
  size_t remaining;
  size_t index;
  bool tone = adapter->alert_frame_index % XV_MUSIC_ALERT_CYCLE_FRAMES <
              XV_MUSIC_ALERT_TONE_FRAMES;

  for (index = 0U; index < XV_MUSIC_ALERT_FRAME_SAMPLES; ++index) {
    adapter->alert_frame[index] =
        !tone ? 0 : ((index / 8U) % 2U == 0U ? 4200 : -4200);
  }
  adapter->alert_frame_index += 1U;
  data = (const uint8_t *)adapter->alert_frame;
  remaining = sizeof(adapter->alert_frame);
  while (remaining > 0U) {
    ssize_t written =
        media_player_write_data(adapter->alert_player, data, remaining);
    if (written <= 0) {
      return written < 0 ? (int)written : -EIO;
    }
    data += (size_t)written;
    remaining -= (size_t)written;
  }
  return 0;
}

static void execute_interruption(xv_music_adapter_t *adapter,
                                 const xv_music_command_slot_t *command,
                                 xv_music_command_result_t *result) {
  xv_music_error_t error;
  int platform_result;

  if (xv_music_model_set_interruption(&adapter->model, command->reason,
                                      command->policy,
                                      command->active) != XV_MUSIC_MODEL_OK) {
    result_set(result, false, XV_MUSIC_ERROR_INVALID_ARGUMENT, -EINVAL);
    return;
  }
  platform_result = reconcile_music(adapter, false, &error);
  result_set(result, platform_result >= 0,
             platform_result >= 0 ? XV_MUSIC_ERROR_NONE : error,
             platform_result);
  notify_state(adapter);
}

static void execute_alert(xv_music_adapter_t *adapter, bool start,
                          xv_music_command_result_t *result) {
  xv_music_error_t error = XV_MUSIC_ERROR_NONE;
  int platform_result = 0;

  if (start && adapter->alert_active) {
    result_set(result, true, XV_MUSIC_ERROR_NONE, 0);
    return;
  }
  if (!start && !adapter->alert_active && adapter->alert_player == NULL) {
    (void)xv_music_model_set_interruption(
        &adapter->model, XV_MUSIC_REASON_ALERT, alert_interrupt_policy(),
        false);
    platform_result = reconcile_music(adapter, false, &error);
    result_set(result, platform_result >= 0,
               platform_result >= 0 ? XV_MUSIC_ERROR_NONE : error,
               platform_result);
    notify_state(adapter);
    return;
  }
  if (start) {
    (void)xv_music_model_set_interruption(
        &adapter->model, XV_MUSIC_REASON_ALERT, alert_interrupt_policy(),
        true);
    platform_result = reconcile_music(adapter, false, &error);
    if (platform_result >= 0) {
      platform_result = open_alert(adapter);
      if (platform_result < 0) {
        error = XV_MUSIC_ERROR_PREPARE_FAILED;
      }
    }
    if (platform_result < 0) {
      (void)xv_music_model_set_interruption(
          &adapter->model, XV_MUSIC_REASON_ALERT, alert_interrupt_policy(),
          false);
      (void)reconcile_music(adapter, false, &error);
    }
  } else {
    platform_result = close_alert(adapter);
    (void)xv_music_model_set_interruption(
        &adapter->model, XV_MUSIC_REASON_ALERT, alert_interrupt_policy(),
        false);
    if (reconcile_music(adapter, false, &error) < 0 &&
        platform_result >= 0) {
      platform_result = -EIO;
    }
  }
  result_set(result, platform_result >= 0,
             platform_result >= 0 ? XV_MUSIC_ERROR_NONE : error,
             platform_result);
  notify_state(adapter);
}

/* Alert stop bypasses the bounded media command queue.  A queue-full or
 * stalled media command must not leave an already-visible reminder playing. */
static bool process_alert_stop_request(xv_music_adapter_t *adapter) {
  xv_music_command_result_t result;
  uint64_t generation;
  bool requested;

  pthread_mutex_lock(&adapter->lock);
  requested = adapter->alert_stop_requested;
  generation = adapter->alert_stop_generation;
  pthread_mutex_unlock(&adapter->lock);
  if (!requested) {
    return false;
  }

  result_set(&result, false, XV_MUSIC_ERROR_INTERNAL, -EIO);
  execute_alert(adapter, false, &result);

  pthread_mutex_lock(&adapter->lock);
  adapter->alert_stop_result =
      result.ok && !adapter->alert_active && adapter->alert_player == NULL;
  adapter->alert_stop_requested = false;
  adapter->alert_stop_completed_generation = generation;
  pthread_cond_broadcast(&adapter->alert_stop_condition);
  pthread_mutex_unlock(&adapter->lock);
  return true;
}

static void execute_slot(xv_music_adapter_t *adapter,
                         xv_music_command_slot_t *command,
                         xv_music_command_result_t *result) {
  if (command->type == XV_INTERNAL_MEDIA_COMMAND) {
    execute_media_command(adapter, command, result);
  } else if (command->type == XV_INTERNAL_INTERRUPTION) {
    execute_interruption(adapter, command, result);
  } else if (command->type == XV_INTERNAL_ALERT_START) {
    execute_alert(adapter, true, result);
  } else {
    execute_alert(adapter, false, result);
  }
}

static void complete_slot(xv_music_adapter_t *adapter, size_t index,
                          const xv_music_command_result_t *result) {
  xv_music_command_slot_t *slot = &adapter->commands[index];
  bool signal = false;

  pthread_mutex_lock(&adapter->lock);
  slot->result = *result;
  slot->executing = false;
  if (slot->synchronous && !slot->canceled) {
    slot->completed = true;
    signal = true;
  } else {
    slot->occupied = false;
  }
  pthread_mutex_unlock(&adapter->lock);
  if (signal) {
    (void)sem_post(&slot->done);
  }
}

static bool pop_command(xv_music_adapter_t *adapter, size_t *index) {
  xv_music_command_slot_t *slot;

  pthread_mutex_lock(&adapter->lock);
  while (adapter->command_count > 0U) {
    *index = adapter->command_queue[adapter->command_read];
    adapter->command_read =
        (adapter->command_read + 1U) % XV_MUSIC_COMMAND_CAPACITY;
    adapter->command_count -= 1U;
    slot = &adapter->commands[*index];
    if (slot->canceled) {
      slot->occupied = false;
      continue;
    }
    slot->executing = true;
    pthread_mutex_unlock(&adapter->lock);
    return true;
  }
  pthread_mutex_unlock(&adapter->lock);
  return false;
}

static void handle_music_focus(xv_music_adapter_t *adapter,
                               int suggestion) {
  xv_music_interrupt_policy_t policy = XV_MUSIC_INTERRUPT_PAUSE;
  bool active = true;
  xv_music_error_t error;

  if (suggestion == MEDIA_FOCUS_PLAY ||
      suggestion == MEDIA_FOCUS_PLAY_WITH_KEEP) {
    active = false;
  } else if (suggestion == MEDIA_FOCUS_PLAY_WITH_DUCK) {
    policy = XV_MUSIC_INTERRUPT_DUCK;
  } else if (suggestion == MEDIA_FOCUS_STOP) {
    policy = XV_MUSIC_INTERRUPT_STOP;
  }
  (void)xv_music_model_set_interruption(
      &adapter->model, XV_MUSIC_REASON_PLATFORM_FOCUS, policy, active);
  if (reconcile_music(adapter, false, &error) < 0) {
    xv_music_model_set_error(&adapter->model, true);
  }
  notify_state(adapter);
}

static void handle_pending_callbacks(xv_music_adapter_t *adapter) {
  int music_focus_value = 0;
  int alert_focus_value = 0;
  int music_event_value = 0;
  int music_event_result = 0;
  int alert_event_value = 0;
  int alert_event_result = 0;
  bool music_focus_pending;
  bool alert_focus_pending;
  bool music_event_pending;
  bool alert_event_pending;
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  bool alsa_music_failed;
  int alsa_music_failure_result = 0;
#endif

  pthread_mutex_lock(&adapter->lock);
  music_focus_pending = adapter->music_focus_pending;
  alert_focus_pending = adapter->alert_focus_pending;
  music_event_pending = adapter->music_event_pending;
  alert_event_pending = adapter->alert_event_pending;
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  alsa_music_failed = adapter->alsa_music_failed;
  if (alsa_music_failed) {
    alsa_music_failure_result = adapter->alsa_music_failure_result;
    adapter->alsa_music_failed = false;
  }
#endif
  if (music_focus_pending) {
    music_focus_value = adapter->pending_music_focus;
    adapter->music_focus_pending = false;
  }
  if (alert_focus_pending) {
    alert_focus_value = adapter->pending_alert_focus;
    adapter->alert_focus_pending = false;
  }
  if (music_event_pending) {
    music_event_value = adapter->pending_music_event;
    music_event_result = adapter->pending_music_result;
    adapter->music_event_pending = false;
  }
  if (alert_event_pending) {
    alert_event_value = adapter->pending_alert_event;
    alert_event_result = adapter->pending_alert_result;
    adapter->alert_event_pending = false;
  }
  pthread_mutex_unlock(&adapter->lock);

  if (music_focus_pending) {
    handle_music_focus(adapter, music_focus_value);
  }
  if (alert_focus_pending &&
      (alert_focus_value == MEDIA_FOCUS_STOP ||
       alert_focus_value == MEDIA_FOCUS_PAUSE ||
       alert_focus_value == MEDIA_FOCUS_PLAY_BUT_SILENT)) {
    xv_music_error_t ignored;
    (void)close_alert(adapter);
    (void)xv_music_model_set_interruption(
        &adapter->model, XV_MUSIC_REASON_ALERT, XV_MUSIC_INTERRUPT_PAUSE,
        false);
    (void)reconcile_music(adapter, false, &ignored);
    if (adapter->callbacks.alert_error != NULL) {
      adapter->callbacks.alert_error(adapter->callbacks.context, -EINTR);
    }
  }
  if (music_event_pending) {
    xv_music_error_t error;

    if (music_event_result < 0) {
      xv_music_model_set_error(&adapter->model, true);
    } else if (music_event_value == MEDIA_EVENT_COMPLETED &&
               adapter->model.desired == XV_MUSIC_DESIRED_PLAYING &&
               xv_music_model_next(&adapter->model) == XV_MUSIC_MODEL_OK) {
      adapter->music_started = false;
      (void)reconcile_music(adapter, true, &error);
    }
    notify_state(adapter);
  }
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  if (alsa_music_failed) {
    adapter->music_started = false;
    adapter->music_paused = false;
    (void)close_music(adapter);
    xv_music_model_set_error(&adapter->model, true);
    printf("xiaov-music: ALSA fallback playback failed: %d\n",
           alsa_music_failure_result);
    notify_state(adapter);
  }
#endif
  if (alert_event_pending && alert_event_result < 0 &&
      alert_event_value != MEDIA_EVENT_STOPPED &&
      adapter->callbacks.alert_error != NULL) {
    adapter->callbacks.alert_error(adapter->callbacks.context,
                                   alert_event_result);
  }
}

static void poll_progress(xv_music_adapter_t *adapter) {
  unsigned int position;
  unsigned int duration;

#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  if (adapter->alsa_music_selected) {
    uint64_t played_samples;

    if (!adapter->music_started || adapter->music_paused) {
      return;
    }
    pthread_mutex_lock(&adapter->lock);
    played_samples = adapter->alsa_music_played_samples;
    pthread_mutex_unlock(&adapter->lock);
    played_samples =
        (played_samples * 1000U) / XV_LOCAL_WAV_SAMPLE_RATE_HZ;
    position = played_samples > UINT32_MAX ? UINT32_MAX
                                           : (unsigned int)played_samples;
    duration = adapter->alsa_music_local_file
                   ? xv_local_wav_duration_ms(&adapter->local_wav)
                   : 0U;
    xv_music_model_update_progress(&adapter->model, position, duration);
    notify_state(adapter);
    return;
  }
#endif
  if (adapter->music_player == NULL || !adapter->music_started ||
      adapter->music_paused) {
    return;
  }
  if (media_player_get_position(adapter->music_player, &position) < 0) {
    return;
  }
  if (media_player_get_duration(adapter->music_player, &duration) < 0) {
    duration = adapter->model.duration_ms;
  }
  xv_music_model_update_progress(&adapter->model, position, duration);
  notify_state(adapter);
}

static void add_milliseconds(struct timespec *time, uint32_t milliseconds) {
  time->tv_sec += (time_t)(milliseconds / 1000U);
  time->tv_nsec += (long)(milliseconds % 1000U) * 1000000L;
  if (time->tv_nsec >= 1000000000L) {
    time->tv_sec += 1;
    time->tv_nsec -= 1000000000L;
  }
}

static void wait_worker(xv_music_adapter_t *adapter, uint32_t milliseconds) {
  struct timespec deadline;
  int result;

  if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
    return;
  }
  add_milliseconds(&deadline, milliseconds);
  do {
    result = sem_timedwait(&adapter->wake, &deadline);
  } while (result < 0 && errno == EINTR);
}

static bool is_shutting_down(xv_music_adapter_t *adapter) {
  bool shutting_down;
  pthread_mutex_lock(&adapter->lock);
  shutting_down = adapter->shutting_down;
  pthread_mutex_unlock(&adapter->lock);
  return shutting_down;
}

static void alert_failed(xv_music_adapter_t *adapter, int result) {
  xv_music_error_t ignored;

  (void)close_alert(adapter);
  (void)xv_music_model_set_interruption(
      &adapter->model, XV_MUSIC_REASON_ALERT, XV_MUSIC_INTERRUPT_PAUSE,
      false);
  (void)reconcile_music(adapter, false, &ignored);
  notify_state(adapter);
  if (adapter->callbacks.alert_error != NULL) {
    adapter->callbacks.alert_error(adapter->callbacks.context, result);
  }
}

static void *music_worker(void *context) {
  xv_music_adapter_t *adapter = context;
  uint32_t progress_elapsed = 0U;

  notify_state(adapter);
  while (!is_shutting_down(adapter)) {
    size_t index;
    bool processed = false;

    if (process_alert_stop_request(adapter)) {
      processed = true;
    }
    while (pop_command(adapter, &index)) {
      xv_music_command_result_t result;
      execute_slot(adapter, &adapter->commands[index], &result);
      complete_slot(adapter, index, &result);
      processed = true;
    }
    handle_pending_callbacks(adapter);
    if (adapter->alert_active) {
      int result = write_alert_frame(adapter);
      if (result < 0) {
        alert_failed(adapter, result);
      }
      progress_elapsed += XV_MUSIC_ALERT_FRAME_MS;
      if (progress_elapsed >= XV_MUSIC_PROGRESS_INTERVAL_MS) {
        poll_progress(adapter);
        progress_elapsed = 0U;
      }
      wait_worker(adapter, XV_MUSIC_ALERT_FRAME_MS);
    } else {
      poll_progress(adapter);
      progress_elapsed = 0U;
      wait_worker(adapter,
                  processed ? 1U : XV_MUSIC_PROGRESS_INTERVAL_MS);
    }
  }
  (void)close_alert(adapter);
  (void)close_music(adapter);
  return NULL;
}

static bool prepare_slot(xv_music_command_slot_t *slot,
                         xv_internal_command_type_t type,
                         const xv_music_command_t *command,
                         xv_music_interrupt_reason_t reason,
                         xv_music_interrupt_policy_t policy, bool active,
                         bool synchronous) {
  while (sem_trywait(&slot->done) == 0) {
  }
  slot->type = type;
  slot->action = command == NULL ? XV_MUSIC_ACTION_STOP : command->action;
  slot->reason = reason;
  slot->policy = policy;
  slot->value = 0U;
  slot->source[0] = '\0';
  if (command != NULL) {
    slot->value = command->action == XV_MUSIC_ACTION_SET_VOLUME
                      ? command->volume_percent
                      : command->position_ms;
    if (command->action == XV_MUSIC_ACTION_PLAY &&
        !bounded_copy(slot->source, sizeof(slot->source), command->source)) {
      return false;
    }
  }
  result_set(&slot->result, false, XV_MUSIC_ERROR_INTERNAL, 0);
  slot->active = active;
  slot->occupied = true;
  slot->synchronous = synchronous;
  slot->executing = false;
  slot->completed = false;
  slot->canceled = false;
  return true;
}

static int wait_for_slot(xv_music_adapter_t *adapter, size_t index,
                         xv_music_command_result_t *result) {
  xv_music_command_slot_t *slot = &adapter->commands[index];
  struct timespec deadline;
  int wait_result;

  if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
    wait_result = sem_wait(&slot->done);
  } else {
    add_milliseconds(&deadline,
                     CONFIG_EXAMPLES_XIAOV_MUSIC_COMMAND_TIMEOUT_MS);
    do {
      wait_result = sem_timedwait(&slot->done, &deadline);
    } while (wait_result < 0 && errno == EINTR);
  }
  if (wait_result < 0 && errno == ETIMEDOUT) {
    bool completed;

    pthread_mutex_lock(&adapter->lock);
    completed = slot->completed;
    if (!completed) {
      slot->canceled = true;
    }
    pthread_mutex_unlock(&adapter->lock);

    if (completed) {
      /* The worker completed as the timeout expired.  Consume its signal and
       * return the actual Media Framework result instead of reporting a
       * spurious timeout. */
      do {
        wait_result = sem_wait(&slot->done);
      } while (wait_result < 0 && errno == EINTR);
      if (wait_result == 0) {
        pthread_mutex_lock(&adapter->lock);
        *result = slot->result;
        slot->occupied = false;
        slot->completed = false;
        pthread_mutex_unlock(&adapter->lock);
        return 0;
      }
      result_set(result, false, XV_MUSIC_ERROR_INTERNAL, -errno);
      return -errno;
    }

    /* Do not wait forever when a Media Framework call is stalled.  Keeping
     * the canceled slot occupied lets the worker finish safely without a
     * later command reusing its storage. */
    result_set(result, false, XV_MUSIC_ERROR_TIMED_OUT, -ETIMEDOUT);
    return -ETIMEDOUT;
  }
  if (wait_result < 0) {
    result_set(result, false, XV_MUSIC_ERROR_INTERNAL, -errno);
    return -errno;
  }
  pthread_mutex_lock(&adapter->lock);
  *result = slot->result;
  slot->occupied = false;
  slot->completed = false;
  pthread_mutex_unlock(&adapter->lock);
  return 0;
}

static bool enqueue(xv_music_adapter_t *adapter,
                    xv_internal_command_type_t type,
                    const xv_music_command_t *command,
                    xv_music_interrupt_reason_t reason,
                    xv_music_interrupt_policy_t policy, bool active,
                    bool synchronous, xv_music_command_result_t *result) {
  size_t index;
  bool found = false;

  if (adapter == NULL) {
    if (result != NULL) {
      result_set(result, false, XV_MUSIC_ERROR_SHUTTING_DOWN, -ESHUTDOWN);
    }
    return false;
  }
  /* State and alert callbacks run on the music worker.  A synchronous
   * command from either callback would otherwise wait for that same worker
   * to dequeue it. */
  if (synchronous && pthread_equal(pthread_self(), adapter->thread)) {
    if (result != NULL) {
      result_set(result, false, XV_MUSIC_ERROR_INTERNAL, -EDEADLK);
    }
    return false;
  }
  pthread_mutex_lock(&adapter->lock);
  if (!adapter->initialized || adapter->shutting_down ||
      adapter->command_count == XV_MUSIC_COMMAND_CAPACITY) {
    pthread_mutex_unlock(&adapter->lock);
    if (result != NULL) {
      result_set(result, false,
                 adapter->shutting_down ? XV_MUSIC_ERROR_SHUTTING_DOWN
                                        : XV_MUSIC_ERROR_QUEUE_FULL,
                 adapter->shutting_down ? -ESHUTDOWN : -EAGAIN);
    }
    return false;
  }
  for (index = 0U; index < XV_MUSIC_COMMAND_CAPACITY; ++index) {
    if (!adapter->commands[index].occupied) {
      found = true;
      break;
    }
  }
  if (!found || !prepare_slot(&adapter->commands[index], type, command,
                              reason, policy, active, synchronous)) {
    pthread_mutex_unlock(&adapter->lock);
    if (result != NULL) {
      result_set(result, false,
                 found ? XV_MUSIC_ERROR_INVALID_ARGUMENT
                       : XV_MUSIC_ERROR_QUEUE_FULL,
                 found ? -EINVAL : -EAGAIN);
    }
    return false;
  }
  adapter->command_queue[adapter->command_write] = (uint8_t)index;
  adapter->command_write =
      (adapter->command_write + 1U) % XV_MUSIC_COMMAND_CAPACITY;
  adapter->command_count += 1U;
  pthread_mutex_unlock(&adapter->lock);
  (void)sem_post(&adapter->wake);
  if (!synchronous) {
    return true;
  }
  return wait_for_slot(adapter, index, result) == 0;
}

xv_music_adapter_t *xv_music_init(xv_music_adapter_storage_t *storage,
                                  const xv_music_callbacks_t *callbacks) {
  xv_music_adapter_t *adapter;
  pthread_attr_t attributes;
  size_t index;
  int result;

  if (storage == NULL || callbacks == NULL) {
    return NULL;
  }
  memset(storage, 0, sizeof(*storage));
  adapter = (xv_music_adapter_t *)(void *)storage->bytes;
  adapter->callbacks = *callbacks;
  xv_music_model_init(&adapter->model);
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  xv_alsa_playback_backend_reset(&adapter->alsa_music);
  xv_local_wav_reset(&adapter->local_wav);
  adapter->alsa_music_gain_q15 =
      (uint16_t)((XV_MUSIC_DEFAULT_VOLUME_PERCENT *
                  XV_ALSA_MUSIC_GAIN_MAX) /
                 100U);
#endif
  adapter->music_cookie.adapter = adapter;
  adapter->music_cookie.kind = XV_PLAYER_COOKIE_MUSIC;
  adapter->alert_cookie.adapter = adapter;
  adapter->alert_cookie.kind = XV_PLAYER_COOKIE_ALERT;

  if (pthread_mutex_init(&adapter->lock, NULL) != 0) {
    return NULL;
  }
  adapter->lock_initialized = true;
  if (pthread_mutex_init(&adapter->snapshot_lock, NULL) != 0) {
    xv_music_deinit(adapter);
    return NULL;
  }
  adapter->snapshot_lock_initialized = true;
  xv_music_model_snapshot(&adapter->model, &adapter->published_snapshot);
  if (pthread_cond_init(&adapter->alert_stop_condition, NULL) != 0) {
    xv_music_deinit(adapter);
    return NULL;
  }
  adapter->alert_stop_condition_initialized = true;
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  if (pthread_cond_init(&adapter->alsa_music_condition, NULL) != 0) {
    xv_music_deinit(adapter);
    return NULL;
  }
  adapter->alsa_music_condition_initialized = true;
  if (sem_init(&adapter->alsa_music_wake, 0, 0) < 0) {
    xv_music_deinit(adapter);
    return NULL;
  }
  adapter->alsa_music_wake_initialized = true;
#endif
  if (sem_init(&adapter->wake, 0, 0) < 0) {
    xv_music_deinit(adapter);
    return NULL;
  }
  adapter->wake_initialized = true;
  for (index = 0U; index < XV_MUSIC_COMMAND_CAPACITY; ++index) {
    if (sem_init(&adapter->commands[index].done, 0, 0) < 0) {
      xv_music_deinit(adapter);
      return NULL;
    }
    adapter->commands[index].semaphore_initialized = true;
  }
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  if (pthread_attr_init(&attributes) != 0) {
    xv_music_deinit(adapter);
    return NULL;
  }
  result = pthread_attr_setstacksize(
      &attributes, CONFIG_EXAMPLES_XIAOV_MUSIC_ALSA_WORKER_STACKSIZE);
  if (result == 0) {
    result = pthread_create(&adapter->alsa_music_thread, &attributes,
                            alsa_music_worker, adapter);
  }
  (void)pthread_attr_destroy(&attributes);
  if (result != 0) {
    xv_music_deinit(adapter);
    return NULL;
  }
  adapter->alsa_music_thread_started = true;
#endif
  if (pthread_attr_init(&attributes) != 0) {
    xv_music_deinit(adapter);
    return NULL;
  }
  result = pthread_attr_setstacksize(
      &attributes, CONFIG_EXAMPLES_XIAOV_MUSIC_WORKER_STACKSIZE);
  if (result == 0) {
    result = pthread_create(&adapter->thread, &attributes, music_worker,
                            adapter);
  }
  (void)pthread_attr_destroy(&attributes);
  if (result != 0) {
    xv_music_deinit(adapter);
    return NULL;
  }
  adapter->thread_started = true;
  adapter->initialized = true;
  return adapter;
}

void xv_music_deinit(xv_music_adapter_t *adapter) {
  size_t index;

  if (adapter == NULL) {
    return;
  }
  if (adapter->lock_initialized) {
    pthread_mutex_lock(&adapter->lock);
    adapter->initialized = false;
    adapter->shutting_down = true;
    if (adapter->alert_stop_condition_initialized) {
      pthread_cond_broadcast(&adapter->alert_stop_condition);
    }
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
    if (adapter->alsa_music_condition_initialized) {
      pthread_cond_broadcast(&adapter->alsa_music_condition);
    }
#endif
    pthread_mutex_unlock(&adapter->lock);
  }
  if (adapter->wake_initialized) {
    (void)sem_post(&adapter->wake);
  }
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  if (adapter->alsa_music_wake_initialized) {
    (void)sem_post(&adapter->alsa_music_wake);
  }
#endif
  if (adapter->thread_started) {
    (void)pthread_join(adapter->thread, NULL);
    adapter->thread_started = false;
  }
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  if (adapter->alsa_music_thread_started) {
    (void)pthread_join(adapter->alsa_music_thread, NULL);
    adapter->alsa_music_thread_started = false;
  }
  xv_local_wav_close(&adapter->local_wav);
  adapter->alsa_music_selected = false;
  adapter->alsa_music_local_file = false;
#endif
  for (index = 0U; index < XV_MUSIC_COMMAND_CAPACITY; ++index) {
    if (adapter->commands[index].semaphore_initialized) {
      (void)sem_destroy(&adapter->commands[index].done);
      adapter->commands[index].semaphore_initialized = false;
    }
  }
  if (adapter->wake_initialized) {
    (void)sem_destroy(&adapter->wake);
    adapter->wake_initialized = false;
  }
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  if (adapter->alsa_music_wake_initialized) {
    (void)sem_destroy(&adapter->alsa_music_wake);
    adapter->alsa_music_wake_initialized = false;
  }
#endif
  if (adapter->lock_initialized) {
    (void)pthread_mutex_destroy(&adapter->lock);
    adapter->lock_initialized = false;
  }
  if (adapter->snapshot_lock_initialized) {
    (void)pthread_mutex_destroy(&adapter->snapshot_lock);
    adapter->snapshot_lock_initialized = false;
  }
  if (adapter->alert_stop_condition_initialized) {
    (void)pthread_cond_destroy(&adapter->alert_stop_condition);
    adapter->alert_stop_condition_initialized = false;
  }
#ifdef CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA
  if (adapter->alsa_music_condition_initialized) {
    (void)pthread_cond_destroy(&adapter->alsa_music_condition);
    adapter->alsa_music_condition_initialized = false;
  }
#endif
}

bool xv_music_execute(xv_music_adapter_t *adapter,
                      const xv_music_command_t *command,
                      xv_music_command_result_t *result) {
  if (result == NULL) {
    return false;
  }
  result_set(result, false, XV_MUSIC_ERROR_INVALID_ARGUMENT, -EINVAL);
  if (command == NULL || command->action > XV_MUSIC_ACTION_SEEK ||
      (command->action == XV_MUSIC_ACTION_PLAY && command->source == NULL)) {
    return false;
  }
  return enqueue(adapter, XV_INTERNAL_MEDIA_COMMAND, command, 0,
                 XV_MUSIC_INTERRUPT_PAUSE, false, true, result);
}

bool xv_music_submit(xv_music_adapter_t *adapter,
                     const xv_music_command_t *command) {
  if (command == NULL || command->action > XV_MUSIC_ACTION_SEEK ||
      (command->action == XV_MUSIC_ACTION_PLAY && command->source == NULL)) {
    return false;
  }
  return enqueue(adapter, XV_INTERNAL_MEDIA_COMMAND, command, 0,
                 XV_MUSIC_INTERRUPT_PAUSE, false, false, NULL);
}

bool xv_music_get_snapshot(xv_music_adapter_t *adapter,
                           xv_music_snapshot_t *snapshot) {
  if (adapter == NULL || snapshot == NULL) {
    return false;
  }
  pthread_mutex_lock(&adapter->lock);
  if (!adapter->initialized || adapter->shutting_down) {
    pthread_mutex_unlock(&adapter->lock);
    return false;
  }
  pthread_mutex_lock(&adapter->snapshot_lock);
  *snapshot = adapter->published_snapshot;
  pthread_mutex_unlock(&adapter->snapshot_lock);
  pthread_mutex_unlock(&adapter->lock);
  return true;
}

bool xv_music_set_interruption(xv_music_adapter_t *adapter,
                               xv_music_interrupt_reason_t reason,
                               xv_music_interrupt_policy_t policy,
                               bool active) {
  xv_music_command_result_t result;
  return enqueue(adapter, XV_INTERNAL_INTERRUPTION, NULL, reason, policy,
                 active, true, &result) && result.ok;
}

bool xv_music_start_alert(void *context) {
  xv_music_command_result_t result;
  return enqueue((xv_music_adapter_t *)context, XV_INTERNAL_ALERT_START, NULL,
                 0, XV_MUSIC_INTERRUPT_PAUSE, true, true, &result) &&
         result.ok;
}

bool xv_music_stop_alert(void *context) {
  xv_music_adapter_t *adapter = context;
  struct timespec deadline;
  uint64_t generation;
  int wait_result = 0;
  bool success = false;

  if (adapter == NULL || !adapter->lock_initialized ||
      !adapter->alert_stop_condition_initialized) {
    return false;
  }
  pthread_mutex_lock(&adapter->lock);
  if (!adapter->initialized || adapter->shutting_down) {
    pthread_mutex_unlock(&adapter->lock);
    return false;
  }
  if (!adapter->alert_stop_requested) {
    adapter->alert_stop_generation += 1U;
    if (adapter->alert_stop_generation == 0U) {
      adapter->alert_stop_generation = 1U;
    }
    adapter->alert_stop_requested = true;
    adapter->alert_stop_result = false;
  }
  generation = adapter->alert_stop_generation;
  pthread_mutex_unlock(&adapter->lock);
  (void)sem_post(&adapter->wake);

  if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
    return false;
  }
  add_milliseconds(&deadline, CONFIG_EXAMPLES_XIAOV_MUSIC_COMMAND_TIMEOUT_MS);

  pthread_mutex_lock(&adapter->lock);
  while (adapter->alert_stop_completed_generation < generation &&
         adapter->initialized && !adapter->shutting_down) {
    wait_result = pthread_cond_timedwait(&adapter->alert_stop_condition,
                                         &adapter->lock, &deadline);
    if (wait_result == ETIMEDOUT) {
      break;
    }
    if (wait_result != 0 && wait_result != EINTR) {
      break;
    }
  }
  success = adapter->alert_stop_completed_generation >= generation &&
            adapter->alert_stop_result;
  pthread_mutex_unlock(&adapter->lock);
  return success;
}

bool xv_music_action_from_string(const char *text,
                                 xv_music_action_t *action) {
  static const char *const names[] = {
      "play", "pause", "resume", "stop", "next", "previous",
      "set_volume", "seek",
  };
  size_t index;

  if (text == NULL || action == NULL) {
    return false;
  }
  for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
    if (strcmp(text, names[index]) == 0) {
      *action = (xv_music_action_t)index;
      return true;
    }
  }
  return false;
}

const char *xv_music_error_code(xv_music_error_t error) {
  switch (error) {
  case XV_MUSIC_ERROR_NONE:
    return NULL;
  case XV_MUSIC_ERROR_INVALID_ACTION:
    return "invalid_action";
  case XV_MUSIC_ERROR_INVALID_SOURCE:
    return "invalid_source";
  case XV_MUSIC_ERROR_INVALID_ARGUMENT:
    return "invalid_argument";
  case XV_MUSIC_ERROR_OUT_OF_RANGE:
    return "out_of_range";
  case XV_MUSIC_ERROR_PLAYLIST_FULL:
    return "playlist_full";
  case XV_MUSIC_ERROR_PLAYLIST_EMPTY:
    return "playlist_empty";
  case XV_MUSIC_ERROR_NO_TRACK:
    return "no_track";
  case XV_MUSIC_ERROR_FOCUS_BLOCKED:
    return "focus_blocked";
  case XV_MUSIC_ERROR_PREPARE_FAILED:
    return "prepare_failed";
  case XV_MUSIC_ERROR_PLAYBACK_FAILED:
    return "playback_failed";
  case XV_MUSIC_ERROR_QUEUE_FULL:
    return "queue_full";
  case XV_MUSIC_ERROR_TIMED_OUT:
    return "timed_out";
  case XV_MUSIC_ERROR_SHUTTING_DOWN:
    return "shutting_down";
  case XV_MUSIC_ERROR_INTERNAL:
  default:
    return "internal_error";
  }
}
