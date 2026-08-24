#include "xiaov/runtime.h"

#include <stddef.h>

static const uint32_t ACTION_ORDER[] = {
    XV_ACTION_STOP_CAPTURE,   XV_ACTION_STOP_PLAYBACK,
    XV_ACTION_STOP_ALERT,     XV_ACTION_CANCEL_TURN,
    XV_ACTION_START_CAPTURE,  XV_ACTION_ENSURE_NETWORK,
    XV_ACTION_SEND_AUDIO_END, XV_ACTION_START_PLAYBACK,
    XV_ACTION_START_ALERT,    XV_ACTION_CLEAR_ERROR,
    XV_ACTION_SHOW_ERROR,     XV_ACTION_ENTER_LOW_POWER,
};

static xv_platform_command_fn action_callback(const xv_runtime_t *runtime,
                                               uint32_t action,
                                               void **context) {
  switch (action) {
  case XV_ACTION_START_CAPTURE:
    *context = runtime->adapters.audio.context;
    return runtime->adapters.audio.start_capture;
  case XV_ACTION_STOP_CAPTURE:
    *context = runtime->adapters.audio.context;
    return runtime->adapters.audio.stop_capture;
  case XV_ACTION_ENSURE_NETWORK:
    *context = runtime->adapters.network.context;
    return runtime->adapters.network.ensure_session;
  case XV_ACTION_SEND_AUDIO_END:
    *context = runtime->adapters.network.context;
    return runtime->adapters.network.send_audio_end;
  case XV_ACTION_START_PLAYBACK:
    *context = runtime->adapters.audio.context;
    return runtime->adapters.audio.start_playback;
  case XV_ACTION_STOP_PLAYBACK:
    *context = runtime->adapters.audio.context;
    return runtime->adapters.audio.stop_playback;
  case XV_ACTION_CANCEL_TURN:
    *context = runtime->adapters.network.context;
    return runtime->adapters.network.cancel_turn;
  case XV_ACTION_SHOW_ERROR:
    *context = runtime->adapters.app.context;
    return runtime->adapters.app.show_error;
  case XV_ACTION_CLEAR_ERROR:
    *context = runtime->adapters.app.context;
    return runtime->adapters.app.clear_error;
  case XV_ACTION_START_ALERT:
    *context = runtime->adapters.audio.context;
    return runtime->adapters.audio.start_alert;
  case XV_ACTION_STOP_ALERT:
    *context = runtime->adapters.audio.context;
    return runtime->adapters.audio.stop_alert;
  case XV_ACTION_ENTER_LOW_POWER:
    *context = runtime->adapters.app.context;
    return runtime->adapters.app.enter_low_power;
  default:
    *context = NULL;
    return NULL;
  }
}

static uint32_t execute_actions(xv_runtime_t *runtime, uint32_t actions,
                                bool stop_on_failure) {
  size_t index;
  uint32_t first_failure = XV_ACTION_NONE;

  for (index = 0U; index < sizeof(ACTION_ORDER) / sizeof(ACTION_ORDER[0]);
       ++index) {
    uint32_t action = ACTION_ORDER[index];
    xv_platform_command_fn callback;
    void *context = NULL;

    if ((actions & action) == 0U) {
      continue;
    }
    callback = action_callback(runtime, action, &context);
    if (callback == NULL || !callback(context)) {
      if (first_failure == XV_ACTION_NONE) {
        first_failure = action;
      }
      if (stop_on_failure) {
        break;
      }
    }
  }
  return first_failure;
}

static void notify_transition(xv_runtime_t *runtime,
                              const xv_transition_t *transition) {
  if (runtime->adapters.app.state_changed != NULL) {
    runtime->adapters.app.state_changed(runtime->adapters.app.context,
                                        transition);
  }
}

bool xv_runtime_init(xv_runtime_t *runtime,
                     const xv_platform_adapters_t *adapters,
                     bool hands_free_barge_in) {
  if (runtime == NULL || adapters == NULL ||
      adapters->clock.monotonic_ms == NULL) {
    return false;
  }
  xv_state_machine_init(&runtime->machine, hands_free_barge_in);
  runtime->adapters = *adapters;
  runtime->last_failed_action = XV_ACTION_NONE;
  return true;
}

xv_runtime_result_t xv_runtime_dispatch(xv_runtime_t *runtime,
                                        xv_app_event_t event,
                                        xv_transition_t *transition) {
  xv_transition_t current;
  uint32_t failed_action;

  if (runtime == NULL) {
    return XV_RUNTIME_BAD_ARGUMENT;
  }
  runtime->last_failed_action = XV_ACTION_NONE;
  if (!xv_state_machine_dispatch(&runtime->machine, event, &current)) {
    if (transition != NULL) {
      transition->previous = runtime->machine.state;
      transition->current = runtime->machine.state;
      transition->actions = XV_ACTION_NONE;
    }
    return XV_RUNTIME_IGNORED;
  }

  notify_transition(runtime, &current);
  failed_action = execute_actions(runtime, current.actions, true);
  if (failed_action == XV_ACTION_NONE) {
    if (transition != NULL) {
      *transition = current;
    }
    return XV_RUNTIME_APPLIED;
  }

  runtime->last_failed_action = failed_action;
  if (runtime->machine.state != XV_STATE_ERROR &&
      xv_state_machine_dispatch(&runtime->machine, XV_APP_EVENT_FAILURE,
                                &current)) {
    notify_transition(runtime, &current);
    (void)execute_actions(runtime, current.actions, false);
  }
  if (transition != NULL) {
    *transition = current;
  }
  return XV_RUNTIME_ADAPTER_ERROR;
}

uint64_t xv_runtime_monotonic_ms(const xv_runtime_t *runtime) {
  if (runtime == NULL || runtime->adapters.clock.monotonic_ms == NULL) {
    return 0U;
  }
  return runtime->adapters.clock.monotonic_ms(
      runtime->adapters.clock.context);
}

uint32_t xv_runtime_last_failed_action(const xv_runtime_t *runtime) {
  if (runtime == NULL) {
    return XV_ACTION_NONE;
  }
  return runtime->last_failed_action;
}
