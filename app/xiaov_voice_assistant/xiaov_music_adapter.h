/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XIAOV_OPENVELA_MUSIC_ADAPTER_H
#define XIAOV_OPENVELA_MUSIC_ADAPTER_H

#include "xiaov/music_player.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  XV_MUSIC_ACTION_PLAY = 0,
  XV_MUSIC_ACTION_PAUSE,
  XV_MUSIC_ACTION_RESUME,
  XV_MUSIC_ACTION_STOP,
  XV_MUSIC_ACTION_NEXT,
  XV_MUSIC_ACTION_PREVIOUS,
  XV_MUSIC_ACTION_SET_VOLUME,
  XV_MUSIC_ACTION_SEEK
} xv_music_action_t;

typedef enum {
  XV_MUSIC_ERROR_NONE = 0,
  XV_MUSIC_ERROR_INVALID_ACTION,
  XV_MUSIC_ERROR_INVALID_SOURCE,
  XV_MUSIC_ERROR_INVALID_ARGUMENT,
  XV_MUSIC_ERROR_OUT_OF_RANGE,
  XV_MUSIC_ERROR_PLAYLIST_FULL,
  XV_MUSIC_ERROR_PLAYLIST_EMPTY,
  XV_MUSIC_ERROR_NO_TRACK,
  XV_MUSIC_ERROR_FOCUS_BLOCKED,
  XV_MUSIC_ERROR_PREPARE_FAILED,
  XV_MUSIC_ERROR_PLAYBACK_FAILED,
  XV_MUSIC_ERROR_QUEUE_FULL,
  XV_MUSIC_ERROR_TIMED_OUT,
  XV_MUSIC_ERROR_SHUTTING_DOWN,
  XV_MUSIC_ERROR_INTERNAL
} xv_music_error_t;

typedef struct {
  xv_music_action_t action;
  const char *source;
  uint32_t volume_percent;
  uint32_t position_ms;
} xv_music_command_t;

typedef struct {
  bool ok;
  xv_music_error_t error;
  int platform_result;
} xv_music_command_result_t;

typedef void (*xv_music_state_fn)(void *context,
                                  const xv_music_snapshot_t *snapshot);
typedef void (*xv_music_alert_error_fn)(void *context, int result);

typedef struct {
  void *context;
  xv_music_state_fn state_changed;
  xv_music_alert_error_fn alert_error;
} xv_music_callbacks_t;

typedef struct xv_music_adapter xv_music_adapter_t;

#define XV_MUSIC_ADAPTER_STORAGE_SIZE 32768U

typedef union {
  uint64_t alignment;
  uint8_t bytes[XV_MUSIC_ADAPTER_STORAGE_SIZE];
} xv_music_adapter_storage_t;

xv_music_adapter_t *xv_music_init(xv_music_adapter_storage_t *storage,
                                  const xv_music_callbacks_t *callbacks);
void xv_music_deinit(xv_music_adapter_t *adapter);

/* Execute waits for the single media worker and is used by media.command so
 * its result reflects the real Media Framework call. Submit is non-blocking
 * and is intended for local LVGL controls. */
bool xv_music_execute(xv_music_adapter_t *adapter,
                      const xv_music_command_t *command,
                      xv_music_command_result_t *result);
bool xv_music_submit(xv_music_adapter_t *adapter,
                     const xv_music_command_t *command);
bool xv_music_get_snapshot(xv_music_adapter_t *adapter,
                           xv_music_snapshot_t *snapshot);

bool xv_music_set_interruption(xv_music_adapter_t *adapter,
                               xv_music_interrupt_reason_t reason,
                               xv_music_interrupt_policy_t policy,
                               bool active);
bool xv_music_start_alert(void *context);
bool xv_music_stop_alert(void *context);

bool xv_music_action_from_string(const char *text,
                                 xv_music_action_t *action);
const char *xv_music_error_code(xv_music_error_t error);

#ifdef __cplusplus
}
#endif

#endif
