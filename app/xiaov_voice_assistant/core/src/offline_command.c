/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "xiaov/offline_command.h"

static const xv_offline_command_descriptor_t g_commands[] = {
    {3U, XV_OFFLINE_COMMAND_MEDIA_PLAY, XV_OFFLINE_ACTION_MEDIA_PLAY,
     XV_OFFLINE_ROUTE_LOCAL, "media_play"},
    {4U, XV_OFFLINE_COMMAND_MEDIA_PAUSE, XV_OFFLINE_ACTION_MEDIA_PAUSE,
     XV_OFFLINE_ROUTE_LOCAL, "media_pause"},
    {5U, XV_OFFLINE_COMMAND_MEDIA_RESUME, XV_OFFLINE_ACTION_MEDIA_RESUME,
     XV_OFFLINE_ROUTE_LOCAL, "media_resume"},
    {6U, XV_OFFLINE_COMMAND_MEDIA_STOP, XV_OFFLINE_ACTION_MEDIA_STOP,
     XV_OFFLINE_ROUTE_LOCAL, "media_stop"},
    {7U, XV_OFFLINE_COMMAND_MEDIA_PREVIOUS,
     XV_OFFLINE_ACTION_MEDIA_PREVIOUS, XV_OFFLINE_ROUTE_LOCAL,
     "media_previous"},
    {8U, XV_OFFLINE_COMMAND_MEDIA_NEXT, XV_OFFLINE_ACTION_MEDIA_NEXT,
     XV_OFFLINE_ROUTE_LOCAL, "media_next"},
    {9U, XV_OFFLINE_COMMAND_VOLUME_UP, XV_OFFLINE_ACTION_VOLUME_UP,
     XV_OFFLINE_ROUTE_LOCAL, "volume_up"},
    {10U, XV_OFFLINE_COMMAND_VOLUME_DOWN, XV_OFFLINE_ACTION_VOLUME_DOWN,
     XV_OFFLINE_ROUTE_LOCAL, "volume_down"},
    {11U, XV_OFFLINE_COMMAND_TIMER_SET, XV_OFFLINE_ACTION_TIMER_SET,
     XV_OFFLINE_ROUTE_REQUIRES_CLOUD, "timer_set"},
    {12U, XV_OFFLINE_COMMAND_TIMER_CANCEL, XV_OFFLINE_ACTION_TIMER_CANCEL,
     XV_OFFLINE_ROUTE_REQUIRES_CLOUD, "timer_cancel"},
    {13U, XV_OFFLINE_COMMAND_TIMER_LIST, XV_OFFLINE_ACTION_TIMER_LIST,
     XV_OFFLINE_ROUTE_REQUIRES_CLOUD, "timer_list"},
    {14U, XV_OFFLINE_COMMAND_MEDIA_MUTE, XV_OFFLINE_ACTION_MEDIA_MUTE,
     XV_OFFLINE_ROUTE_LOCAL, "media_mute"},
    {15U, XV_OFFLINE_COMMAND_MEDIA_UNMUTE, XV_OFFLINE_ACTION_MEDIA_UNMUTE,
     XV_OFFLINE_ROUTE_LOCAL, "media_unmute"},
    {16U, XV_OFFLINE_COMMAND_SCREEN_ON, XV_OFFLINE_ACTION_SCREEN_ON,
     XV_OFFLINE_ROUTE_LOCAL, "screen_on"},
    {17U, XV_OFFLINE_COMMAND_SCREEN_OFF, XV_OFFLINE_ACTION_SCREEN_OFF,
     XV_OFFLINE_ROUTE_LOCAL, "screen_off"},
    {18U, XV_OFFLINE_COMMAND_WEATHER, XV_OFFLINE_ACTION_WEATHER,
     XV_OFFLINE_ROUTE_REQUIRES_CLOUD, "weather"},
};

_Static_assert(sizeof(g_commands) / sizeof(g_commands[0]) ==
                   XV_OFFLINE_COMMAND_COUNT,
               "offline command table size must match the model contract");

const xv_offline_command_descriptor_t *xv_offline_command_lookup(
    uint16_t class_index) {
  size_t position;

  if (class_index < XV_OFFLINE_COMMAND_CLASS_FIRST ||
      class_index > XV_OFFLINE_COMMAND_CLASS_LAST) {
    return NULL;
  }
  position = (size_t)(class_index - XV_OFFLINE_COMMAND_CLASS_FIRST);
  if (g_commands[position].class_index != class_index) {
    return NULL;
  }
  return &g_commands[position];
}

size_t xv_offline_command_count(void) { return XV_OFFLINE_COMMAND_COUNT; }

const xv_offline_command_descriptor_t *xv_offline_command_at(
    size_t position) {
  if (position >= XV_OFFLINE_COMMAND_COUNT) {
    return NULL;
  }
  return &g_commands[position];
}

static xv_offline_execution_result_t execution_result(bool accepted) {
  return accepted ? XV_OFFLINE_EXECUTION_EXECUTED
                  : XV_OFFLINE_EXECUTION_FAILED;
}

xv_offline_execution_result_t xv_offline_command_execute(
    uint16_t class_index, const xv_offline_command_executor_t *executor) {
  const xv_offline_command_descriptor_t *descriptor =
      xv_offline_command_lookup(class_index);

  if (descriptor == NULL) {
    return XV_OFFLINE_EXECUTION_UNSUPPORTED;
  }
  if (descriptor->route == XV_OFFLINE_ROUTE_REQUIRES_CLOUD) {
    return XV_OFFLINE_EXECUTION_REQUIRES_CLOUD;
  }
  if (executor == NULL) {
    return XV_OFFLINE_EXECUTION_UNAVAILABLE;
  }

  switch (descriptor->action) {
    case XV_OFFLINE_ACTION_MEDIA_PLAY:
    case XV_OFFLINE_ACTION_MEDIA_PAUSE:
    case XV_OFFLINE_ACTION_MEDIA_RESUME:
    case XV_OFFLINE_ACTION_MEDIA_STOP:
    case XV_OFFLINE_ACTION_MEDIA_PREVIOUS:
    case XV_OFFLINE_ACTION_MEDIA_NEXT: {
      static const xv_offline_media_action_t media_actions[] = {
          XV_OFFLINE_MEDIA_PLAY,     XV_OFFLINE_MEDIA_PAUSE,
          XV_OFFLINE_MEDIA_RESUME,   XV_OFFLINE_MEDIA_STOP,
          XV_OFFLINE_MEDIA_PREVIOUS, XV_OFFLINE_MEDIA_NEXT,
      };
      size_t position;
      if (executor->media == NULL) {
        return XV_OFFLINE_EXECUTION_UNAVAILABLE;
      }
      position = (size_t)(descriptor->action - XV_OFFLINE_ACTION_MEDIA_PLAY);
      return execution_result(
          executor->media(executor->context, media_actions[position]));
    }

    case XV_OFFLINE_ACTION_VOLUME_UP:
    case XV_OFFLINE_ACTION_VOLUME_DOWN: {
      int16_t delta;
      if (executor->volume_step_percent == 0U ||
          executor->volume_step_percent > 100U) {
        return XV_OFFLINE_EXECUTION_BAD_ARGUMENT;
      }
      if (executor->adjust_volume == NULL) {
        return XV_OFFLINE_EXECUTION_UNAVAILABLE;
      }
      delta = (int16_t)executor->volume_step_percent;
      if (descriptor->action == XV_OFFLINE_ACTION_VOLUME_DOWN) {
        delta = (int16_t)-delta;
      }
      return execution_result(
          executor->adjust_volume(executor->context, delta));
    }

    case XV_OFFLINE_ACTION_MEDIA_MUTE:
    case XV_OFFLINE_ACTION_MEDIA_UNMUTE:
      if (executor->set_muted == NULL) {
        return XV_OFFLINE_EXECUTION_UNAVAILABLE;
      }
      return execution_result(executor->set_muted(
          executor->context,
          descriptor->action == XV_OFFLINE_ACTION_MEDIA_MUTE));

    case XV_OFFLINE_ACTION_SCREEN_ON:
    case XV_OFFLINE_ACTION_SCREEN_OFF:
      if (executor->set_screen_active == NULL) {
        return XV_OFFLINE_EXECUTION_UNAVAILABLE;
      }
      return execution_result(executor->set_screen_active(
          executor->context,
          descriptor->action == XV_OFFLINE_ACTION_SCREEN_ON));

    case XV_OFFLINE_ACTION_TIMER_SET:
    case XV_OFFLINE_ACTION_TIMER_CANCEL:
    case XV_OFFLINE_ACTION_TIMER_LIST:
    case XV_OFFLINE_ACTION_WEATHER:
      return XV_OFFLINE_EXECUTION_REQUIRES_CLOUD;

    case XV_OFFLINE_ACTION_INVALID:
    default:
      return XV_OFFLINE_EXECUTION_UNSUPPORTED;
  }
}

const char *xv_offline_command_route_name(xv_offline_command_route_t route) {
  switch (route) {
    case XV_OFFLINE_ROUTE_LOCAL:
      return "local";
    case XV_OFFLINE_ROUTE_REQUIRES_CLOUD:
      return "requires_cloud";
    default:
      return "invalid";
  }
}

const char *xv_offline_execution_result_name(
    xv_offline_execution_result_t result) {
  switch (result) {
    case XV_OFFLINE_EXECUTION_UNSUPPORTED:
      return "unsupported";
    case XV_OFFLINE_EXECUTION_REQUIRES_CLOUD:
      return "requires_cloud";
    case XV_OFFLINE_EXECUTION_UNAVAILABLE:
      return "unavailable";
    case XV_OFFLINE_EXECUTION_FAILED:
      return "failed";
    case XV_OFFLINE_EXECUTION_EXECUTED:
      return "executed";
    case XV_OFFLINE_EXECUTION_BAD_ARGUMENT:
      return "bad_argument";
    default:
      return "invalid";
  }
}
