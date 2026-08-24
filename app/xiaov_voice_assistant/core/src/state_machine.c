#include "xiaov/state_machine.h"

#include <stddef.h>

static bool apply_normal_event(const xv_state_machine_t *machine,
                               xv_app_event_t event,
                               xv_app_state_t *next,
                               uint32_t *actions) {
  switch (machine->state) {
  case XV_STATE_STANDBY:
    if (event == XV_APP_EVENT_USER_TRIGGER) {
      *next = XV_STATE_LISTENING;
      *actions = XV_ACTION_START_CAPTURE | XV_ACTION_ENSURE_NETWORK;
      return true;
    }
    if (event == XV_APP_EVENT_ALARM_FIRED) {
      *next = XV_STATE_ALERTING;
      *actions = XV_ACTION_START_ALERT;
      return true;
    }
    break;

  case XV_STATE_LISTENING:
    if (event == XV_APP_EVENT_END_OF_SPEECH) {
      *next = XV_STATE_THINKING;
      *actions = XV_ACTION_STOP_CAPTURE | XV_ACTION_SEND_AUDIO_END;
      return true;
    }
    if (event == XV_APP_EVENT_CANCEL) {
      *next = XV_STATE_STANDBY;
      *actions = XV_ACTION_STOP_CAPTURE | XV_ACTION_CANCEL_TURN |
                 XV_ACTION_ENTER_LOW_POWER;
      return true;
    }
    break;

  case XV_STATE_THINKING:
    if (event == XV_APP_EVENT_TTS_STARTED) {
      *next = XV_STATE_SPEAKING;
      *actions = XV_ACTION_START_PLAYBACK;
      return true;
    }
    if (event == XV_APP_EVENT_TURN_COMPLETED) {
      *next = XV_STATE_STANDBY;
      *actions = XV_ACTION_ENTER_LOW_POWER;
      return true;
    }
    if (event == XV_APP_EVENT_USER_TRIGGER) {
      *next = XV_STATE_LISTENING;
      *actions = XV_ACTION_CANCEL_TURN | XV_ACTION_START_CAPTURE |
                 XV_ACTION_ENSURE_NETWORK;
      return true;
    }
    if (event == XV_APP_EVENT_CANCEL) {
      *next = XV_STATE_STANDBY;
      *actions = XV_ACTION_CANCEL_TURN | XV_ACTION_ENTER_LOW_POWER;
      return true;
    }
    break;

  case XV_STATE_SPEAKING:
    if (event == XV_APP_EVENT_TURN_COMPLETED) {
      *next = XV_STATE_STANDBY;
      *actions = XV_ACTION_STOP_PLAYBACK | XV_ACTION_ENTER_LOW_POWER;
      return true;
    }
    if (event == XV_APP_EVENT_CANCEL) {
      *next = XV_STATE_STANDBY;
      *actions = XV_ACTION_STOP_PLAYBACK | XV_ACTION_CANCEL_TURN |
                 XV_ACTION_ENTER_LOW_POWER;
      return true;
    }
    if (event == XV_APP_EVENT_USER_TRIGGER ||
        (event == XV_APP_EVENT_BARGE_IN && machine->hands_free_barge_in)) {
      *next = XV_STATE_LISTENING;
      *actions = XV_ACTION_STOP_PLAYBACK | XV_ACTION_CANCEL_TURN |
                 XV_ACTION_START_CAPTURE | XV_ACTION_ENSURE_NETWORK;
      return true;
    }
    break;

  case XV_STATE_ALERTING:
    if (event == XV_APP_EVENT_ALARM_ACKNOWLEDGED ||
        event == XV_APP_EVENT_CANCEL) {
      *next = XV_STATE_STANDBY;
      *actions = XV_ACTION_STOP_ALERT | XV_ACTION_ENTER_LOW_POWER;
      return true;
    }
    if (event == XV_APP_EVENT_USER_TRIGGER) {
      *next = XV_STATE_LISTENING;
      *actions = XV_ACTION_STOP_ALERT | XV_ACTION_START_CAPTURE |
                 XV_ACTION_ENSURE_NETWORK;
      return true;
    }
    break;

  case XV_STATE_ERROR:
    if (event == XV_APP_EVENT_RESET) {
      *next = XV_STATE_STANDBY;
      *actions = XV_ACTION_CLEAR_ERROR | XV_ACTION_ENTER_LOW_POWER;
      return true;
    }
    break;
  }
  return false;
}

void xv_state_machine_init(xv_state_machine_t *machine,
                           bool hands_free_barge_in) {
  if (machine == NULL) {
    return;
  }
  machine->state = XV_STATE_STANDBY;
  machine->hands_free_barge_in = hands_free_barge_in;
}

bool xv_state_machine_dispatch(xv_state_machine_t *machine,
                               xv_app_event_t event,
                               xv_transition_t *transition) {
  xv_app_state_t next;
  uint32_t actions = XV_ACTION_NONE;
  bool handled;
  if (machine == NULL || transition == NULL) {
    return false;
  }

  next = machine->state;
  if (event == XV_APP_EVENT_FAILURE) {
    if (machine->state == XV_STATE_ERROR) {
      return false;
    }
    next = XV_STATE_ERROR;
    actions = XV_ACTION_STOP_CAPTURE | XV_ACTION_STOP_PLAYBACK |
              XV_ACTION_STOP_ALERT | XV_ACTION_CANCEL_TURN |
              XV_ACTION_SHOW_ERROR;
    handled = true;
  } else {
    handled = apply_normal_event(machine, event, &next, &actions);
  }

  if (!handled) {
    return false;
  }
  transition->previous = machine->state;
  transition->current = next;
  transition->actions = actions;
  machine->state = next;
  return true;
}

const char *xv_state_name(xv_app_state_t state) {
  switch (state) {
  case XV_STATE_STANDBY:
    return "standby";
  case XV_STATE_LISTENING:
    return "listening";
  case XV_STATE_THINKING:
    return "thinking";
  case XV_STATE_SPEAKING:
    return "speaking";
  case XV_STATE_ALERTING:
    return "alerting";
  case XV_STATE_ERROR:
    return "error";
  }
  return "unknown";
}
