#ifndef XIAOV_STATE_MACHINE_H
#define XIAOV_STATE_MACHINE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  XV_STATE_STANDBY = 0,
  XV_STATE_LISTENING,
  XV_STATE_THINKING,
  XV_STATE_SPEAKING,
  XV_STATE_ALERTING,
  XV_STATE_ERROR
} xv_app_state_t;

typedef enum {
  XV_APP_EVENT_USER_TRIGGER = 0,
  XV_APP_EVENT_END_OF_SPEECH,
  XV_APP_EVENT_TTS_STARTED,
  XV_APP_EVENT_TURN_COMPLETED,
  XV_APP_EVENT_BARGE_IN,
  XV_APP_EVENT_CANCEL,
  XV_APP_EVENT_ALARM_FIRED,
  XV_APP_EVENT_ALARM_ACKNOWLEDGED,
  XV_APP_EVENT_FAILURE,
  XV_APP_EVENT_RESET
} xv_app_event_t;

typedef enum {
  XV_ACTION_NONE = 0,
  XV_ACTION_START_CAPTURE = 1U << 0,
  XV_ACTION_STOP_CAPTURE = 1U << 1,
  XV_ACTION_ENSURE_NETWORK = 1U << 2,
  XV_ACTION_SEND_AUDIO_END = 1U << 3,
  XV_ACTION_START_PLAYBACK = 1U << 4,
  XV_ACTION_STOP_PLAYBACK = 1U << 5,
  XV_ACTION_CANCEL_TURN = 1U << 6,
  XV_ACTION_SHOW_ERROR = 1U << 7,
  XV_ACTION_CLEAR_ERROR = 1U << 8,
  XV_ACTION_START_ALERT = 1U << 9,
  XV_ACTION_STOP_ALERT = 1U << 10,
  XV_ACTION_ENTER_LOW_POWER = 1U << 11
} xv_app_action_t;

typedef struct {
  xv_app_state_t state;
  bool hands_free_barge_in;
} xv_state_machine_t;

typedef struct {
  xv_app_state_t previous;
  xv_app_state_t current;
  uint32_t actions;
} xv_transition_t;

void xv_state_machine_init(xv_state_machine_t *machine,
                           bool hands_free_barge_in);
bool xv_state_machine_dispatch(xv_state_machine_t *machine,
                               xv_app_event_t event,
                               xv_transition_t *transition);
const char *xv_state_name(xv_app_state_t state);

#ifdef __cplusplus
}
#endif

#endif
