/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure ALSA audio adapter for OpenVela platform.
 * Opens hw:snddmic at 48kHz stereo for capture, hw:audiocodec for playback.
 * Performs 3:1 decimation (48000→16000 Hz) and stereo→mono conversion.
 */

#ifdef __NuttX__
#  include <nuttx/config.h>
#endif

#include "xiaov_media_adapter.h"
#include "xiaov_alsa_backend.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#ifndef CONFIG_EXAMPLES_XIAOV_MEDIA_WORKER_STACKSIZE
#define XV_MEDIA_WORKER_STACKSIZE 16384U
#else
#define XV_MEDIA_WORKER_STACKSIZE CONFIG_EXAMPLES_XIAOV_MEDIA_WORKER_STACKSIZE
#endif

typedef struct {
  int16_t samples[XV_MEDIA_FRAME_SAMPLES];
  size_t sample_count;
} xv_playback_slot_t;

struct xv_media_adapter {
  pthread_mutex_t lock;
  pthread_cond_t capture_stopped;
  sem_t capture_wake;
  sem_t playback_wake;
  pthread_t capture_thread;
  pthread_t playback_thread;
  xv_media_callbacks_t callbacks;

  bool initialized;
  bool shutting_down;
  bool capture_requested;
  bool capture_active;
  bool capture_stop_pending;
  bool playback_requested;
  bool playback_active;
  bool playback_finished;
  bool playback_drain_notified;
  bool playback_eof_sent;
  bool playback_completed;
  uint64_t capture_stop_requested;
  uint64_t capture_stop_completed;

  xv_alsa_backend_t alsa;
  uint32_t capture_frame_index;
  int16_t capture_frame[XV_MEDIA_FRAME_SAMPLES];

  xv_playback_slot_t playback_queue[XV_MEDIA_PLAYBACK_QUEUE_FRAMES];
  size_t playback_read;
  size_t playback_write;
  size_t playback_count;
  int16_t playback_current[XV_MEDIA_FRAME_SAMPLES];

  uint64_t capture_dropped;
  uint64_t playback_rejected;
};

_Static_assert(sizeof(struct xv_media_adapter) <=
                   XV_MEDIA_ADAPTER_STORAGE_SIZE,
               "XV_MEDIA_ADAPTER_STORAGE_SIZE is too small");

static void xv_media_notify(xv_media_adapter_t *adapter,
                            xv_media_event_t event, int result) {
  if (adapter->callbacks.emit_event != NULL) {
    adapter->callbacks.emit_event(adapter->callbacks.context, event, result);
  }
}

static int xv_sem_wait(sem_t *semaphore) {
  int ret;

  do {
    ret = sem_wait(semaphore);
  } while (ret < 0 && errno == EINTR);
  return ret;
}

static bool xv_capture_is_requested(xv_media_adapter_t *adapter) {
  bool requested;

  pthread_mutex_lock(&adapter->lock);
  requested = adapter->capture_requested && !adapter->shutting_down;
  pthread_mutex_unlock(&adapter->lock);
  return requested;
}

static bool xv_playback_is_requested(xv_media_adapter_t *adapter) {
  bool requested;

  pthread_mutex_lock(&adapter->lock);
  requested = adapter->playback_requested && !adapter->shutting_down;
  pthread_mutex_unlock(&adapter->lock);
  return requested;
}

static int xv_capture_open(xv_media_adapter_t *adapter) {
  int ret;

  ret = xv_alsa_capture_open(&adapter->alsa);
  if (ret < 0) {
    syslog(LOG_ERR, "xiaov_alsa: capture open failed: %d\n", ret);
    return ret;
  }

  adapter->capture_frame_index = 0U;
  pthread_mutex_lock(&adapter->lock);
  adapter->capture_active = true;
  pthread_mutex_unlock(&adapter->lock);
  xv_media_notify(adapter, XV_MEDIA_EVENT_CAPTURE_STARTED, 0);
  syslog(LOG_INFO, "xiaov_alsa: capture started (hw:snddmic 48kHz stereo)\n");
  return 0;
}

static void xv_capture_close(xv_media_adapter_t *adapter, bool notify) {
  int result = 0;
  int ret;

  if (xv_alsa_capture_is_open(&adapter->alsa)) {
    ret = xv_alsa_capture_close(&adapter->alsa);
    if (ret < 0) {
      syslog(LOG_ERR, "xiaov_alsa: capture close failed: %d\n", ret);
      result = ret;
    }
  }

  pthread_mutex_lock(&adapter->lock);
  adapter->capture_active = false;
  adapter->capture_stop_pending = false;
  adapter->capture_stop_completed = adapter->capture_stop_requested;
  pthread_cond_broadcast(&adapter->capture_stopped);
  pthread_mutex_unlock(&adapter->lock);
  if (notify) {
    xv_media_notify(adapter, XV_MEDIA_EVENT_CAPTURE_STOPPED, result);
    syslog(LOG_INFO, "xiaov_alsa: capture stopped\n");
  }
}

static void xv_capture_fail(xv_media_adapter_t *adapter, int result) {
  syslog(LOG_ERR, "xiaov_alsa: capture error: %d\n", result);
  pthread_mutex_lock(&adapter->lock);
  adapter->capture_requested = false;
  pthread_mutex_unlock(&adapter->lock);
  xv_capture_close(adapter, false);
  xv_media_notify(adapter, XV_MEDIA_EVENT_CAPTURE_ERROR, result);
}

static void xv_capture_deliver(xv_media_adapter_t *adapter) {
  bool accepted = false;
  uint32_t timestamp_ms =
      adapter->capture_frame_index * XV_MEDIA_FRAME_DURATION_MS;

  if (adapter->callbacks.capture_sink != NULL) {
    accepted = adapter->callbacks.capture_sink(
        adapter->callbacks.context, adapter->capture_frame,
        XV_MEDIA_FRAME_SAMPLES, timestamp_ms);
  }
  adapter->capture_frame_index += 1U;
  if (!accepted) {
    pthread_mutex_lock(&adapter->lock);
    adapter->capture_dropped += 1U;
    pthread_mutex_unlock(&adapter->lock);
  }
}

static void *xv_capture_worker(void *argument) {
  xv_media_adapter_t *adapter = argument;

  for (;;) {
    bool shutting_down;
    bool requested;
    int ret;

    pthread_mutex_lock(&adapter->lock);
    shutting_down = adapter->shutting_down;
    requested = adapter->capture_requested;
    pthread_mutex_unlock(&adapter->lock);

    if (shutting_down) {
      xv_capture_close(adapter, false);
      return NULL;
    }

    if (!requested) {
      if (xv_alsa_capture_is_open(&adapter->alsa)) {
        xv_capture_close(adapter, true);
      } else {
        bool notify_stopped;

        pthread_mutex_lock(&adapter->lock);
        notify_stopped = adapter->capture_stop_pending;
        adapter->capture_stop_pending = false;
        if (notify_stopped) {
          adapter->capture_stop_completed =
              adapter->capture_stop_requested;
          pthread_cond_broadcast(&adapter->capture_stopped);
        }
        pthread_mutex_unlock(&adapter->lock);
        if (notify_stopped) {
          xv_media_notify(adapter, XV_MEDIA_EVENT_CAPTURE_STOPPED, 0);
        }
      }
      if (xv_sem_wait(&adapter->capture_wake) < 0) {
        xv_capture_fail(adapter, -errno);
      }
      continue;
    }

    if (!xv_alsa_capture_is_open(&adapter->alsa)) {
      ret = xv_capture_open(adapter);
      if (ret < 0) {
        xv_capture_fail(adapter, ret);
        continue;
      }
      if (!xv_capture_is_requested(adapter)) {
        continue;
      }
    }

    /* Read 20ms frame: 48kHz stereo → 16kHz mono with decimation */
    ret = xv_alsa_capture_read_20ms(
        &adapter->alsa, adapter->capture_frame, XV_MEDIA_FRAME_SAMPLES);
    if (ret < 0) {
      syslog(LOG_ERR, "xiaov_alsa: capture read failed: %d\n", ret);
      xv_capture_fail(adapter, ret);
      continue;
    }
    xv_capture_deliver(adapter);
  }
}

static int xv_playback_open(xv_media_adapter_t *adapter) {
  int ret;

  ret = xv_alsa_playback_open(&adapter->alsa);
  if (ret < 0) {
    syslog(LOG_ERR, "xiaov_alsa: playback open failed: %d\n", ret);
    return ret;
  }

  pthread_mutex_lock(&adapter->lock);
  adapter->playback_active = true;
  pthread_mutex_unlock(&adapter->lock);
  xv_media_notify(adapter, XV_MEDIA_EVENT_PLAYBACK_STARTED, 0);
  syslog(LOG_INFO, "xiaov_alsa: playback started (hw:audiocodec 48kHz stereo)\n");
  return 0;
}

static void xv_playback_close(xv_media_adapter_t *adapter, bool notify) {
  int result = 0;
  int ret;

  if (xv_alsa_playback_is_open(&adapter->alsa)) {
    ret = xv_alsa_playback_close(&adapter->alsa);
    if (ret < 0) {
      syslog(LOG_ERR, "xiaov_alsa: playback close failed: %d\n", ret);
      result = ret;
    }
  }

  pthread_mutex_lock(&adapter->lock);
  adapter->playback_active = false;
  pthread_mutex_unlock(&adapter->lock);
  if (notify) {
    xv_media_notify(adapter, XV_MEDIA_EVENT_PLAYBACK_STOPPED, result);
    syslog(LOG_INFO, "xiaov_alsa: playback stopped\n");
  }
}

static void xv_playback_fail(xv_media_adapter_t *adapter, int result) {
  syslog(LOG_ERR, "xiaov_alsa: playback error: %d\n", result);
  pthread_mutex_lock(&adapter->lock);
  adapter->playback_requested = false;
  adapter->playback_read = 0U;
  adapter->playback_write = 0U;
  adapter->playback_count = 0U;
  pthread_mutex_unlock(&adapter->lock);
  xv_playback_close(adapter, false);
  xv_media_notify(adapter, XV_MEDIA_EVENT_PLAYBACK_ERROR, result);
}

static size_t xv_playback_pop(xv_media_adapter_t *adapter) {
  size_t sample_count = 0U;

  pthread_mutex_lock(&adapter->lock);
  if (adapter->playback_count > 0U) {
    xv_playback_slot_t *slot =
        &adapter->playback_queue[adapter->playback_read];
    sample_count = slot->sample_count;
    memcpy(adapter->playback_current, slot->samples,
           sample_count * sizeof(int16_t));
    adapter->playback_read =
        (adapter->playback_read + 1U) % XV_MEDIA_PLAYBACK_QUEUE_FRAMES;
    adapter->playback_count -= 1U;
  }
  pthread_mutex_unlock(&adapter->lock);
  return sample_count;
}

static bool xv_playback_should_close_input(xv_media_adapter_t *adapter) {
  bool close_input = false;

  pthread_mutex_lock(&adapter->lock);
  if (adapter->playback_requested && adapter->playback_finished &&
      adapter->playback_count == 0U && !adapter->playback_eof_sent) {
    adapter->playback_eof_sent = true;
    close_input = true;
  }
  pthread_mutex_unlock(&adapter->lock);
  return close_input;
}

static bool xv_playback_mark_drained(xv_media_adapter_t *adapter) {
  bool notify = false;

  pthread_mutex_lock(&adapter->lock);
  if (adapter->playback_requested && adapter->playback_completed &&
      !adapter->playback_drain_notified) {
    adapter->playback_drain_notified = true;
    notify = true;
  }
  pthread_mutex_unlock(&adapter->lock);
  return notify;
}

static int xv_playback_write_current(xv_media_adapter_t *adapter,
                                     size_t sample_count) {
  if (!xv_playback_is_requested(adapter)) {
    return -ECANCELED;
  }
  return xv_alsa_playback_write(&adapter->alsa,
                                adapter->playback_current,
                                sample_count);
}

static void *xv_playback_worker(void *argument) {
  xv_media_adapter_t *adapter = argument;

  for (;;) {
    bool shutting_down;
    bool requested;
    size_t sample_count;
    int ret;

    pthread_mutex_lock(&adapter->lock);
    shutting_down = adapter->shutting_down;
    requested = adapter->playback_requested;
    pthread_mutex_unlock(&adapter->lock);

    if (shutting_down) {
      xv_playback_close(adapter, false);
      return NULL;
    }

    if (!requested) {
      if (xv_alsa_playback_is_open(&adapter->alsa)) {
        xv_playback_close(adapter, true);
      }
      if (xv_sem_wait(&adapter->playback_wake) < 0) {
        xv_playback_fail(adapter, -errno);
      }
      continue;
    }

    if (!xv_alsa_playback_is_open(&adapter->alsa)) {
      ret = xv_playback_open(adapter);
      if (ret < 0) {
        xv_playback_fail(adapter, ret);
        continue;
      }
      if (!xv_playback_is_requested(adapter)) {
        continue;
      }
    }

    sample_count = xv_playback_pop(adapter);
    if (sample_count > 0U &&
        xv_media_playback_queued(adapter) ==
            XV_MEDIA_PLAYBACK_QUEUE_FRAMES / 2U) {
      xv_media_notify(adapter, XV_MEDIA_EVENT_PLAYBACK_SPACE_AVAILABLE, 0);
    }
    if (sample_count == 0U) {
      if (xv_playback_should_close_input(adapter)) {
        ret = xv_alsa_playback_drain(&adapter->alsa);
        if (ret < 0) {
          syslog(LOG_ERR, "xiaov_alsa: playback drain failed: %d\n", ret);
          xv_playback_fail(adapter, ret);
          continue;
        }
        pthread_mutex_lock(&adapter->lock);
        adapter->playback_completed = true;
        pthread_mutex_unlock(&adapter->lock);
      }
      if (xv_playback_mark_drained(adapter)) {
        xv_media_notify(adapter, XV_MEDIA_EVENT_PLAYBACK_DRAINED, 0);
      }
      if (xv_sem_wait(&adapter->playback_wake) < 0) {
        xv_playback_fail(adapter, -errno);
      }
      continue;
    }

    /* Write upsampled 16kHz mono → 48kHz stereo */
    ret = xv_playback_write_current(adapter, sample_count);
    if (ret < 0 && ret != -ECANCELED) {
      xv_playback_fail(adapter, ret);
    }
  }
}

static int xv_create_worker(pthread_t *thread, void *(*entry)(void *),
                            void *argument) {
  pthread_attr_t attributes;
  int ret;

  ret = pthread_attr_init(&attributes);
  if (ret != 0) {
    return ret;
  }
  ret = pthread_attr_setstacksize(&attributes, XV_MEDIA_WORKER_STACKSIZE);
  if (ret == 0) {
    ret = pthread_create(thread, &attributes, entry, argument);
  }
  (void)pthread_attr_destroy(&attributes);
  return ret;
}

xv_media_adapter_t *xv_media_init(xv_media_adapter_storage_t *storage,
                                  const xv_media_callbacks_t *callbacks) {
  xv_media_adapter_t *adapter;
  bool lock_initialized = false;
  bool capture_condition_initialized = false;
  bool capture_sem_initialized = false;
  bool playback_sem_initialized = false;
  bool capture_thread_created = false;
  int ret;

  if (storage == NULL || callbacks == NULL ||
      callbacks->capture_sink == NULL || XV_MEDIA_PLAYBACK_QUEUE_FRAMES == 0U) {
    return NULL;
  }

  memset(storage, 0, sizeof(*storage));
  adapter = (xv_media_adapter_t *)(void *)storage->bytes;
  adapter->callbacks = *callbacks;
  xv_alsa_backend_reset(&adapter->alsa);

  ret = pthread_mutex_init(&adapter->lock, NULL);
  if (ret != 0) {
    goto fail;
  }
  lock_initialized = true;
  if (pthread_cond_init(&adapter->capture_stopped, NULL) != 0) {
    goto fail;
  }
  capture_condition_initialized = true;
  if (sem_init(&adapter->capture_wake, 0, 0) < 0) {
    goto fail;
  }
  capture_sem_initialized = true;
  if (sem_init(&adapter->playback_wake, 0, 0) < 0) {
    goto fail;
  }
  playback_sem_initialized = true;

  ret = xv_create_worker(&adapter->capture_thread, xv_capture_worker, adapter);
  if (ret != 0) {
    goto fail;
  }
  capture_thread_created = true;
  ret = xv_create_worker(&adapter->playback_thread, xv_playback_worker,
                         adapter);
  if (ret != 0) {
    goto fail;
  }

  adapter->initialized = true;
  syslog(LOG_INFO, "xiaov_alsa: adapter initialized\n");
  return adapter;

fail:
  if (capture_thread_created) {
    pthread_mutex_lock(&adapter->lock);
    adapter->shutting_down = true;
    pthread_mutex_unlock(&adapter->lock);
    (void)sem_post(&adapter->capture_wake);
    (void)pthread_join(adapter->capture_thread, NULL);
  }
  if (playback_sem_initialized) {
    (void)sem_destroy(&adapter->playback_wake);
  }
  if (capture_sem_initialized) {
    (void)sem_destroy(&adapter->capture_wake);
  }
  if (capture_condition_initialized) {
    (void)pthread_cond_destroy(&adapter->capture_stopped);
  }
  if (lock_initialized) {
    (void)pthread_mutex_destroy(&adapter->lock);
  }
  memset(storage, 0, sizeof(*storage));
  return NULL;
}

void xv_media_deinit(xv_media_adapter_t *adapter) {
  if (adapter == NULL || !adapter->initialized) {
    return;
  }

  pthread_mutex_lock(&adapter->lock);
  adapter->initialized = false;
  adapter->shutting_down = true;
  adapter->capture_requested = false;
  adapter->playback_requested = false;
  adapter->playback_count = 0U;
  pthread_cond_broadcast(&adapter->capture_stopped);
  pthread_mutex_unlock(&adapter->lock);

  (void)sem_post(&adapter->capture_wake);
  (void)sem_post(&adapter->playback_wake);
  (void)pthread_join(adapter->capture_thread, NULL);
  (void)pthread_join(adapter->playback_thread, NULL);
  (void)sem_destroy(&adapter->capture_wake);
  (void)sem_destroy(&adapter->playback_wake);
  (void)pthread_cond_destroy(&adapter->capture_stopped);
  (void)pthread_mutex_destroy(&adapter->lock);
  syslog(LOG_INFO, "xiaov_alsa: adapter deinitialized\n");
}

static bool xv_media_request(xv_media_adapter_t *adapter, bool capture,
                             bool start) {
  sem_t *wake;

  if (adapter == NULL) {
    return false;
  }

  pthread_mutex_lock(&adapter->lock);
  if (!adapter->initialized || adapter->shutting_down) {
    pthread_mutex_unlock(&adapter->lock);
    return false;
  }
  if (capture) {
    if (start) {
      adapter->capture_stop_pending = false;
    } else {
      adapter->capture_stop_requested += 1U;
      if (adapter->capture_stop_requested == 0U) {
        adapter->capture_stop_requested = 1U;
      }
      if (adapter->capture_requested || adapter->capture_active) {
        adapter->capture_stop_pending = true;
      } else {
        adapter->capture_stop_pending = false;
        adapter->capture_stop_completed =
            adapter->capture_stop_requested;
        pthread_cond_broadcast(&adapter->capture_stopped);
      }
    }
    adapter->capture_requested = start;
  } else {
    adapter->playback_requested = start;
    if (start) {
      adapter->playback_drain_notified = false;
      adapter->playback_eof_sent = false;
      adapter->playback_completed = false;
    } else {
      adapter->playback_finished = false;
      adapter->playback_drain_notified = false;
      adapter->playback_eof_sent = false;
      adapter->playback_completed = false;
      adapter->playback_read = 0U;
      adapter->playback_write = 0U;
      adapter->playback_count = 0U;
    }
  }
  wake = capture ? &adapter->capture_wake : &adapter->playback_wake;
  pthread_mutex_unlock(&adapter->lock);
  return sem_post(wake) == 0;
}

bool xv_media_start_capture(void *context) {
  return xv_media_request((xv_media_adapter_t *)context, true, true);
}

bool xv_media_stop_capture(void *context) {
  return xv_media_request((xv_media_adapter_t *)context, true, false);
}

static void xv_add_milliseconds(struct timespec *time,
                                uint32_t milliseconds) {
  time->tv_sec += (time_t)(milliseconds / 1000U);
  time->tv_nsec += (long)(milliseconds % 1000U) * 1000000L;
  if (time->tv_nsec >= 1000000000L) {
    time->tv_sec += 1;
    time->tv_nsec -= 1000000000L;
  }
}

bool xv_media_stop_capture_and_wait(xv_media_adapter_t *adapter,
                                    uint32_t timeout_ms) {
  struct timespec deadline;
  uint64_t request;
  int wait_result = 0;
  bool stopped;

  if (adapter == NULL || timeout_ms == 0U ||
      clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
    return false;
  }
  xv_add_milliseconds(&deadline, timeout_ms);
  if (!xv_media_stop_capture(adapter)) {
    return false;
  }

  pthread_mutex_lock(&adapter->lock);
  request = adapter->capture_stop_requested;
  while (adapter->initialized && !adapter->shutting_down &&
         adapter->capture_stop_completed < request && wait_result == 0) {
    wait_result = pthread_cond_timedwait(&adapter->capture_stopped,
                                        &adapter->lock, &deadline);
  }
  stopped = adapter->initialized && !adapter->shutting_down &&
            adapter->capture_stop_completed >= request &&
            !adapter->capture_active && !adapter->capture_requested;
  pthread_mutex_unlock(&adapter->lock);
  return stopped;
}

bool xv_media_start_playback(void *context) {
  return xv_media_request((xv_media_adapter_t *)context, false, true);
}

bool xv_media_stop_playback(void *context) {
  return xv_media_request((xv_media_adapter_t *)context, false, false);
}

bool xv_media_enqueue_playback(xv_media_adapter_t *adapter,
                               const int16_t *samples,
                               size_t sample_count) {
  xv_playback_slot_t *slot;

  if (adapter == NULL || samples == NULL || sample_count == 0U ||
      sample_count > XV_MEDIA_FRAME_SAMPLES) {
    return false;
  }

  pthread_mutex_lock(&adapter->lock);
  if (!adapter->initialized || adapter->shutting_down ||
      adapter->playback_finished ||
      adapter->playback_count == XV_MEDIA_PLAYBACK_QUEUE_FRAMES) {
    adapter->playback_rejected += 1U;
    pthread_mutex_unlock(&adapter->lock);
    return false;
  }

  slot = &adapter->playback_queue[adapter->playback_write];
  memcpy(slot->samples, samples, sample_count * sizeof(int16_t));
  slot->sample_count = sample_count;
  adapter->playback_write =
      (adapter->playback_write + 1U) % XV_MEDIA_PLAYBACK_QUEUE_FRAMES;
  adapter->playback_count += 1U;
  adapter->playback_drain_notified = false;
  pthread_mutex_unlock(&adapter->lock);
  return sem_post(&adapter->playback_wake) == 0;
}

bool xv_media_finish_playback(xv_media_adapter_t *adapter) {
  if (adapter == NULL) {
    return false;
  }

  pthread_mutex_lock(&adapter->lock);
  if (!adapter->initialized || adapter->shutting_down) {
    pthread_mutex_unlock(&adapter->lock);
    return false;
  }
  adapter->playback_finished = true;
  pthread_mutex_unlock(&adapter->lock);
  return sem_post(&adapter->playback_wake) == 0;
}

void xv_media_clear_playback(xv_media_adapter_t *adapter) {
  if (adapter == NULL) {
    return;
  }

  pthread_mutex_lock(&adapter->lock);
  if (adapter->initialized && !adapter->shutting_down) {
    adapter->playback_read = 0U;
    adapter->playback_write = 0U;
    adapter->playback_count = 0U;
    adapter->playback_finished = false;
    adapter->playback_drain_notified = false;
    adapter->playback_eof_sent = false;
    adapter->playback_completed = false;
  }
  pthread_mutex_unlock(&adapter->lock);
}

size_t xv_media_playback_queued(const xv_media_adapter_t *adapter) {
  xv_media_adapter_t *mutable_adapter = (xv_media_adapter_t *)(uintptr_t)adapter;
  size_t count;

  if (mutable_adapter == NULL) {
    return 0U;
  }
  pthread_mutex_lock(&mutable_adapter->lock);
  count = mutable_adapter->playback_count;
  pthread_mutex_unlock(&mutable_adapter->lock);
  return count;
}

uint64_t xv_media_capture_dropped(const xv_media_adapter_t *adapter) {
  xv_media_adapter_t *mutable_adapter = (xv_media_adapter_t *)(uintptr_t)adapter;
  uint64_t count;

  if (mutable_adapter == NULL) {
    return 0U;
  }
  pthread_mutex_lock(&mutable_adapter->lock);
  count = mutable_adapter->capture_dropped;
  pthread_mutex_unlock(&mutable_adapter->lock);
  return count;
}

uint64_t xv_media_playback_rejected(const xv_media_adapter_t *adapter) {
  xv_media_adapter_t *mutable_adapter = (xv_media_adapter_t *)(uintptr_t)adapter;
  uint64_t count;

  if (mutable_adapter == NULL) {
    return 0U;
  }
  pthread_mutex_lock(&mutable_adapter->lock);
  count = mutable_adapter->playback_rejected;
  pthread_mutex_unlock(&mutable_adapter->lock);
  return count;
}

bool xv_media_capture_active(const xv_media_adapter_t *adapter) {
  xv_media_adapter_t *mutable_adapter = (xv_media_adapter_t *)(uintptr_t)adapter;
  bool active;

  if (mutable_adapter == NULL) {
    return false;
  }
  pthread_mutex_lock(&mutable_adapter->lock);
  active = mutable_adapter->capture_active;
  pthread_mutex_unlock(&mutable_adapter->lock);
  return active;
}

bool xv_media_playback_active(const xv_media_adapter_t *adapter) {
  xv_media_adapter_t *mutable_adapter = (xv_media_adapter_t *)(uintptr_t)adapter;
  bool active;

  if (mutable_adapter == NULL) {
    return false;
  }
  pthread_mutex_lock(&mutable_adapter->lock);
  active = mutable_adapter->playback_active;
  pthread_mutex_unlock(&mutable_adapter->lock);
  return active;
}
