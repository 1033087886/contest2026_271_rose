/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XIAOV_OFFLINE_COMMAND_H
#define XIAOV_OFFLINE_COMMAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* These values are part of the xiaov-kws-dscnn-v1 output contract. They are
 * intentionally numeric: callers pass the acoustic model output index, never
 * decoded or typed text. */
#define XV_OFFLINE_COMMAND_CLASS_FIRST 3U
#define XV_OFFLINE_COMMAND_CLASS_LAST 18U
#define XV_OFFLINE_COMMAND_COUNT 16U

typedef enum {
  XV_OFFLINE_COMMAND_MEDIA_PLAY = 3,
  XV_OFFLINE_COMMAND_MEDIA_PAUSE = 4,
  XV_OFFLINE_COMMAND_MEDIA_RESUME = 5,
  XV_OFFLINE_COMMAND_MEDIA_STOP = 6,
  XV_OFFLINE_COMMAND_MEDIA_PREVIOUS = 7,
  XV_OFFLINE_COMMAND_MEDIA_NEXT = 8,
  XV_OFFLINE_COMMAND_VOLUME_UP = 9,
  XV_OFFLINE_COMMAND_VOLUME_DOWN = 10,
  XV_OFFLINE_COMMAND_TIMER_SET = 11,
  XV_OFFLINE_COMMAND_TIMER_CANCEL = 12,
  XV_OFFLINE_COMMAND_TIMER_LIST = 13,
  XV_OFFLINE_COMMAND_MEDIA_MUTE = 14,
  XV_OFFLINE_COMMAND_MEDIA_UNMUTE = 15,
  XV_OFFLINE_COMMAND_SCREEN_ON = 16,
  XV_OFFLINE_COMMAND_SCREEN_OFF = 17,
  XV_OFFLINE_COMMAND_WEATHER = 18
} xv_offline_command_id_t;

typedef enum {
  XV_OFFLINE_ACTION_INVALID = 0,
  XV_OFFLINE_ACTION_MEDIA_PLAY,
  XV_OFFLINE_ACTION_MEDIA_PAUSE,
  XV_OFFLINE_ACTION_MEDIA_RESUME,
  XV_OFFLINE_ACTION_MEDIA_STOP,
  XV_OFFLINE_ACTION_MEDIA_PREVIOUS,
  XV_OFFLINE_ACTION_MEDIA_NEXT,
  XV_OFFLINE_ACTION_VOLUME_UP,
  XV_OFFLINE_ACTION_VOLUME_DOWN,
  XV_OFFLINE_ACTION_TIMER_SET,
  XV_OFFLINE_ACTION_TIMER_CANCEL,
  XV_OFFLINE_ACTION_TIMER_LIST,
  XV_OFFLINE_ACTION_MEDIA_MUTE,
  XV_OFFLINE_ACTION_MEDIA_UNMUTE,
  XV_OFFLINE_ACTION_SCREEN_ON,
  XV_OFFLINE_ACTION_SCREEN_OFF,
  XV_OFFLINE_ACTION_WEATHER
} xv_offline_command_action_t;

typedef enum {
  XV_OFFLINE_ROUTE_LOCAL = 0,
  XV_OFFLINE_ROUTE_REQUIRES_CLOUD
} xv_offline_command_route_t;

typedef struct {
  uint16_t class_index;
  xv_offline_command_id_t command;
  xv_offline_command_action_t action;
  xv_offline_command_route_t route;
  const char *label;
} xv_offline_command_descriptor_t;

typedef enum {
  XV_OFFLINE_MEDIA_PLAY = 0,
  XV_OFFLINE_MEDIA_PAUSE,
  XV_OFFLINE_MEDIA_RESUME,
  XV_OFFLINE_MEDIA_STOP,
  XV_OFFLINE_MEDIA_PREVIOUS,
  XV_OFFLINE_MEDIA_NEXT
} xv_offline_media_action_t;

typedef bool (*xv_offline_media_fn)(void *context,
                                    xv_offline_media_action_t action);
typedef bool (*xv_offline_adjust_volume_fn)(void *context,
                                            int16_t delta_percent);
typedef bool (*xv_offline_set_muted_fn)(void *context, bool muted);
typedef bool (*xv_offline_set_screen_active_fn)(void *context, bool active);

typedef struct {
  void *context;
  xv_offline_media_fn media;
  xv_offline_adjust_volume_fn adjust_volume;
  xv_offline_set_muted_fn set_muted;
  xv_offline_set_screen_active_fn set_screen_active;
  uint8_t volume_step_percent;
} xv_offline_command_executor_t;

typedef enum {
  XV_OFFLINE_EXECUTION_UNSUPPORTED = 0,
  XV_OFFLINE_EXECUTION_REQUIRES_CLOUD,
  XV_OFFLINE_EXECUTION_UNAVAILABLE,
  XV_OFFLINE_EXECUTION_FAILED,
  XV_OFFLINE_EXECUTION_EXECUTED,
  XV_OFFLINE_EXECUTION_BAD_ARGUMENT
} xv_offline_execution_result_t;

/* Returns NULL for unknown, wake (indices 1/2), and background (index 0)
 * classes. The returned descriptor has static lifetime. */
const xv_offline_command_descriptor_t *xv_offline_command_lookup(
    uint16_t class_index);
size_t xv_offline_command_count(void);
const xv_offline_command_descriptor_t *xv_offline_command_at(size_t position);

/* Executes callbacks synchronously and therefore belongs on the application
 * task, never in the 20 ms capture callback. The capture path should queue the
 * acoustic class index first. Only actions whose descriptor route is LOCAL
 * invoke callbacks; missing callbacks and callback failures are observable
 * and never become success. Timer set and cancel lack duration/identity in the
 * class output; the timer list is authoritative at the gateway; weather
 * requires fresh remote data. Those four actions return REQUIRES_CLOUD without
 * invoking a callback. */
xv_offline_execution_result_t xv_offline_command_execute(
    uint16_t class_index, const xv_offline_command_executor_t *executor);

const char *xv_offline_command_route_name(xv_offline_command_route_t route);
const char *xv_offline_execution_result_name(
    xv_offline_execution_result_t result);

#ifdef __cplusplus
}
#endif

#endif
