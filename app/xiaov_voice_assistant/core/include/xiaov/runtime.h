#ifndef XIAOV_RUNTIME_H
#define XIAOV_RUNTIME_H

#include "xiaov/state_machine.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*xv_platform_command_fn)(void *context);
typedef uint64_t (*xv_platform_monotonic_ms_fn)(void *context);
typedef void (*xv_platform_transition_fn)(void *context,
                                          const xv_transition_t *transition);

typedef struct {
  void *context;
  xv_platform_command_fn start_capture;
  xv_platform_command_fn stop_capture;
  xv_platform_command_fn start_playback;
  xv_platform_command_fn stop_playback;
  xv_platform_command_fn start_alert;
  xv_platform_command_fn stop_alert;
} xv_audio_adapter_t;

typedef struct {
  void *context;
  xv_platform_command_fn ensure_session;
  xv_platform_command_fn send_audio_end;
  xv_platform_command_fn cancel_turn;
} xv_network_adapter_t;

typedef struct {
  void *context;
  xv_platform_monotonic_ms_fn monotonic_ms;
} xv_clock_adapter_t;

typedef struct {
  void *context;
  xv_platform_command_fn show_error;
  xv_platform_command_fn clear_error;
  xv_platform_command_fn enter_low_power;
  xv_platform_transition_fn state_changed;
} xv_app_adapter_t;

typedef struct {
  xv_audio_adapter_t audio;
  xv_network_adapter_t network;
  xv_clock_adapter_t clock;
  xv_app_adapter_t app;
} xv_platform_adapters_t;

typedef enum {
  XV_RUNTIME_APPLIED = 0,
  XV_RUNTIME_IGNORED,
  XV_RUNTIME_ADAPTER_ERROR,
  XV_RUNTIME_BAD_ARGUMENT
} xv_runtime_result_t;

typedef struct {
  xv_state_machine_t machine;
  xv_platform_adapters_t adapters;
  uint32_t last_failed_action;
} xv_runtime_t;

bool xv_runtime_init(xv_runtime_t *runtime,
                     const xv_platform_adapters_t *adapters,
                     bool hands_free_barge_in);

xv_runtime_result_t xv_runtime_dispatch(xv_runtime_t *runtime,
                                        xv_app_event_t event,
                                        xv_transition_t *transition);

uint64_t xv_runtime_monotonic_ms(const xv_runtime_t *runtime);
uint32_t xv_runtime_last_failed_action(const xv_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif
