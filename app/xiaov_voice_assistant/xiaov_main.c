/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifdef __NuttX__
#  include <nuttx/config.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "xiaov/audio_frame.h"
#include "xiaov/event_queue.h"
#include "xiaov/ring_buffer.h"
#include "xiaov/runtime.h"
#include "xiaov/vad.h"
#ifdef __NuttX__
#  include "xiaov_service.h"
#  include "xiaov_skills.h"
#endif
#include "xiaov_version.h"

#define XV_PROBE_PCM_SAMPLES 32000U
#define XV_PROBE_EVENT_COUNT 16U

#ifdef __NuttX__
#  define XV_PROVISION_WIFI_PATH "/data/wifi.cfg"
#  ifdef CONFIG_EXAMPLES_XIAOV_TOKEN_PATH
#    define XV_PROVISION_TOKEN_PATH CONFIG_EXAMPLES_XIAOV_TOKEN_PATH
#  else
#    define XV_PROVISION_TOKEN_PATH "/data/xiaov.token"
#  endif
#endif

typedef struct {
  xv_runtime_t runtime;
  xv_pcm_ring_t pcm_ring;
  xv_event_queue_t event_queue;
  xv_vad_t vad;
  int16_t pcm_storage[XV_PROBE_PCM_SAMPLES];
  xv_event_t event_storage[XV_PROBE_EVENT_COUNT];
  bool capture_active;
  bool playback_active;
  bool alert_active;
  bool session_ready;
  bool error_visible;
} xv_probe_t;

typedef struct {
  const char *name;
  xv_app_event_t event;
} xv_named_event_t;

static const xv_named_event_t g_named_events[] = {
    {"trigger", XV_APP_EVENT_USER_TRIGGER},
    {"eos", XV_APP_EVENT_END_OF_SPEECH},
    {"tts", XV_APP_EVENT_TTS_STARTED},
    {"complete", XV_APP_EVENT_TURN_COMPLETED},
    {"barge-in", XV_APP_EVENT_BARGE_IN},
    {"cancel", XV_APP_EVENT_CANCEL},
    {"alarm", XV_APP_EVENT_ALARM_FIRED},
    {"ack", XV_APP_EVENT_ALARM_ACKNOWLEDGED},
    {"failure", XV_APP_EVENT_FAILURE},
    {"reset", XV_APP_EVENT_RESET},
};

static void probe_log(const char *operation) {
  printf("xiaov: adapter %s\n", operation);
}

static bool probe_start_capture(void *context) {
  xv_probe_t *probe = context;
  probe->capture_active = true;
  probe_log("start_capture");
  return true;
}

static bool probe_stop_capture(void *context) {
  xv_probe_t *probe = context;
  probe->capture_active = false;
  probe_log("stop_capture");
  return true;
}

static bool probe_start_playback(void *context) {
  xv_probe_t *probe = context;
  probe->playback_active = true;
  probe_log("start_playback");
  return true;
}

static bool probe_stop_playback(void *context) {
  xv_probe_t *probe = context;
  probe->playback_active = false;
  probe_log("stop_playback");
  return true;
}

static bool probe_start_alert(void *context) {
  xv_probe_t *probe = context;
  probe->alert_active = true;
  probe_log("start_alert");
  return true;
}

static bool probe_stop_alert(void *context) {
  xv_probe_t *probe = context;
  probe->alert_active = false;
  probe_log("stop_alert");
  return true;
}

static bool probe_ensure_session(void *context) {
  xv_probe_t *probe = context;
  probe->session_ready = true;
  probe_log("ensure_session");
  return true;
}

static bool probe_send_audio_end(void *context) {
  xv_probe_t *probe = context;
  probe_log("send_audio_end");
  return probe->session_ready;
}

static bool probe_cancel_turn(void *context) {
  xv_probe_t *probe = context;
  probe->session_ready = false;
  probe_log("cancel_turn");
  return true;
}

static uint64_t probe_monotonic_ms(void *context) {
  struct timespec value;
  (void)context;
#ifdef _WIN32
  /* Host-only selftest fallback. openvela always takes the monotonic branch. */
  if (timespec_get(&value, TIME_UTC) != TIME_UTC) {
    return 0U;
  }
#else
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
    return 0U;
  }
#endif
  return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static bool probe_show_error(void *context) {
  xv_probe_t *probe = context;
  probe->error_visible = true;
  probe_log("show_error");
  return true;
}

static bool probe_clear_error(void *context) {
  xv_probe_t *probe = context;
  probe->error_visible = false;
  probe_log("clear_error");
  return true;
}

static bool probe_enter_low_power(void *context) {
  (void)context;
  probe_log("enter_low_power");
  return true;
}

static void probe_state_changed(void *context,
                                const xv_transition_t *transition) {
  (void)context;
  printf("xiaov: %s -> %s actions=0x%08lx\n",
         xv_state_name(transition->previous),
         xv_state_name(transition->current),
         (unsigned long)transition->actions);
}

static bool probe_init(xv_probe_t *probe) {
  xv_platform_adapters_t adapters;

  memset(probe, 0, sizeof(*probe));
  memset(&adapters, 0, sizeof(adapters));

  if (!xv_pcm_ring_init(&probe->pcm_ring, probe->pcm_storage,
                        XV_PROBE_PCM_SAMPLES) ||
      !xv_event_queue_init(&probe->event_queue, probe->event_storage,
                           XV_PROBE_EVENT_COUNT) ||
      !xv_vad_init(&probe->vad, NULL)) {
    return false;
  }

  adapters.audio.context = probe;
  adapters.audio.start_capture = probe_start_capture;
  adapters.audio.stop_capture = probe_stop_capture;
  adapters.audio.start_playback = probe_start_playback;
  adapters.audio.stop_playback = probe_stop_playback;
  adapters.audio.start_alert = probe_start_alert;
  adapters.audio.stop_alert = probe_stop_alert;
  adapters.network.context = probe;
  adapters.network.ensure_session = probe_ensure_session;
  adapters.network.send_audio_end = probe_send_audio_end;
  adapters.network.cancel_turn = probe_cancel_turn;
  adapters.clock.context = probe;
  adapters.clock.monotonic_ms = probe_monotonic_ms;
  adapters.app.context = probe;
  adapters.app.show_error = probe_show_error;
  adapters.app.clear_error = probe_clear_error;
  adapters.app.enter_low_power = probe_enter_low_power;
  adapters.app.state_changed = probe_state_changed;

  /* AEC is not available on the current BSP, so hands-free barge-in is off. */
  return xv_runtime_init(&probe->runtime, &adapters, false);
}

static bool probe_dispatch(xv_probe_t *probe, xv_app_event_t event) {
  xv_transition_t transition;
  xv_runtime_result_t result;

  result = xv_runtime_dispatch(&probe->runtime, event, &transition);
  if (result == XV_RUNTIME_APPLIED) {
    return true;
  }
  if (result == XV_RUNTIME_IGNORED) {
    printf("xiaov: event ignored in state %s\n",
           xv_state_name(probe->runtime.machine.state));
  } else {
    printf("xiaov: dispatch failed result=%d action=0x%08lx\n", (int)result,
           (unsigned long)xv_runtime_last_failed_action(&probe->runtime));
  }
  return false;
}

static bool probe_core_storage(xv_probe_t *probe) {
  static const int16_t samples[] = {100, -200, 300, -400};
  int16_t output[4];
  xv_event_t input_event;
  xv_event_t output_event;
  xv_audio_header_t input_header;
  xv_audio_header_t output_header;
  const uint8_t *payload;
  uint8_t frame[XV_AUDIO_HEADER_SIZE + 2U];

  memset(&input_event, 0, sizeof(input_event));
  input_event.type = 7U;
  input_event.timestamp_ms = xv_runtime_monotonic_ms(&probe->runtime);
  if (!xv_event_queue_push(&probe->event_queue, &input_event) ||
      !xv_event_queue_pop(&probe->event_queue, &output_event) ||
      output_event.type != input_event.type) {
    return false;
  }

  if (xv_pcm_ring_write(&probe->pcm_ring, samples, 4U) != 4U ||
      xv_pcm_ring_read(&probe->pcm_ring, output, 4U) != 4U ||
      memcmp(samples, output, sizeof(samples)) != 0) {
    return false;
  }

  memset(&input_header, 0, sizeof(input_header));
  input_header.kind = XV_AUDIO_KIND_MICROPHONE;
  input_header.stream_id = 1U;
  input_header.payload_length = 2U;
  if (!xv_audio_header_encode(frame, sizeof(frame), &input_header)) {
    return false;
  }
  frame[XV_AUDIO_HEADER_SIZE] = 0x34U;
  frame[XV_AUDIO_HEADER_SIZE + 1U] = 0x12U;
  return xv_audio_frame_decode(frame, sizeof(frame), &output_header, &payload) &&
         output_header.stream_id == input_header.stream_id &&
         payload[0] == 0x34U && payload[1] == 0x12U;
}

static bool probe_find_event(const char *name, xv_app_event_t *event) {
  size_t index;
  for (index = 0U;
       index < sizeof(g_named_events) / sizeof(g_named_events[0]); ++index) {
    if (strcmp(name, g_named_events[index].name) == 0) {
      *event = g_named_events[index].event;
      return true;
    }
  }
  return false;
}

static int probe_selftest(xv_probe_t *probe) {
  static const xv_app_event_t flow[] = {
      XV_APP_EVENT_USER_TRIGGER,
      XV_APP_EVENT_END_OF_SPEECH,
      XV_APP_EVENT_TTS_STARTED,
      XV_APP_EVENT_TURN_COMPLETED,
      XV_APP_EVENT_ALARM_FIRED,
      XV_APP_EVENT_ALARM_ACKNOWLEDGED,
  };
  size_t index;

  if (!probe_core_storage(probe)) {
    printf("xiaov: core storage/codec probe failed\n");
    return 1;
  }
  for (index = 0U; index < sizeof(flow) / sizeof(flow[0]); ++index) {
    if (!probe_dispatch(probe, flow[index])) {
      return 1;
    }
  }
  if (probe->runtime.machine.state != XV_STATE_STANDBY ||
      probe->capture_active || probe->playback_active || probe->alert_active ||
      probe->error_visible) {
    printf("xiaov: final state check failed\n");
    return 1;
  }
  printf("xiaov: selftest PASS firmware=%s target=%s pcm_capacity=%u "
         "event_capacity=%u now_ms=%llu\n",
         XIAOV_FIRMWARE_VERSION, XIAOV_FIRMWARE_TARGET,
         (unsigned int)XV_PROBE_PCM_SAMPLES,
         (unsigned int)XV_PROBE_EVENT_COUNT,
         (unsigned long long)xv_runtime_monotonic_ms(&probe->runtime));
  return 0;
}

#ifdef __NuttX__
static bool probe_provision_value_valid(const char *value, size_t minimum,
                                        size_t maximum, bool config_quoted) {
  size_t index;
  size_t length;

  if (value == NULL) {
    return false;
  }
  length = strlen(value);
  if (length < minimum || length > maximum) {
    return false;
  }
  for (index = 0U; index < length; ++index) {
    unsigned char current = (unsigned char)value[index];
    if (current < 0x20U || current > 0x7eU ||
        (config_quoted && (current == '"' || current == '\\'))) {
      return false;
    }
  }
  return true;
}

static bool probe_provision_write_wifi(const char *ssid,
                                       const char *password) {
  FILE *output = fopen(XV_PROVISION_WIFI_PATH, "w");
  bool ok;

  if (output == NULL) {
    return false;
  }
  ok = fprintf(output, "SSID=\"%s\"\nPASSWORD=\"%s\"\n", ssid,
               password) >= 0;
  if (fclose(output) != 0) {
    ok = false;
  }
  return ok;
}

static bool probe_provision_write_token(const char *token) {
  FILE *output = fopen(XV_PROVISION_TOKEN_PATH, "w");
  bool ok;

  if (output == NULL) {
    return false;
  }
  ok = fprintf(output, "%s\n", token) >= 0;
  if (fclose(output) != 0) {
    ok = false;
  }
  return ok;
}

static bool probe_config_file_has_payload(const char *path) {
  FILE *input = fopen(path, "r");
  int value;

  if (input == NULL) {
    return false;
  }
  do {
    value = fgetc(input);
  } while (value == '\r' || value == '\n' || value == ' ' || value == '\t');
  (void)fclose(input);
  return value != EOF;
}

static int probe_config_status(void) {
  bool wifi_present = probe_config_file_has_payload(XV_PROVISION_WIFI_PATH);
  bool token_present = probe_config_file_has_payload(XV_PROVISION_TOKEN_PATH);

  printf("xiaov: config wifi=%s token=%s\n",
         wifi_present ? "present" : "missing",
         token_present ? "present" : "missing");
  return token_present ? 0 : 1;
}

static int probe_provision(int argc, char *argv[]) {
  bool token_requested = argc == 5;

  if ((argc != 4 && argc != 5) ||
      !probe_provision_value_valid(argv[2], 1U, 32U, true) ||
      !probe_provision_value_valid(argv[3], 8U, 64U, true) ||
      (token_requested &&
       !probe_provision_value_valid(argv[4], 1U, 255U, false))) {
    printf("xiaov: provision rejected invalid arguments\n");
    return 2;
  }
  if (!probe_provision_write_wifi(argv[2], argv[3])) {
    printf("xiaov: provision failed writing Wi-Fi configuration\n");
    return 1;
  }
  if (token_requested && !probe_provision_write_token(argv[4])) {
    printf("xiaov: provision failed writing gateway token\n");
    return 1;
  }
  printf("xiaov: provision PASS token=%s; reset board to connect\n",
         token_requested ? "written" : "unchanged");
  return 0;
}
#endif

static void probe_usage(void) {
  printf("usage: xiaov [version | selftest | EVENT...]\n");
  printf("       xiaov service HOST PORT PATH [tls]\n");
  printf("       xiaov daemon HOST PORT PATH [tls]\n");
#ifdef __NuttX__
  printf("       xiaov config\n");
  printf("       xiaov provision SSID PASSWORD [GATEWAY_TOKEN]\n");
  printf("       xiaov skills install\n");
#endif
  printf("events: trigger eos tts complete barge-in cancel alarm ack failure reset\n");
  printf("example: xiaov trigger eos tts complete\n");
}

int main(int argc, char *argv[]) {
  static xv_probe_t probe;
  int index;

#ifdef __NuttX__
  if (argc == 3 && strcmp(argv[1], "skills") == 0 &&
      strcmp(argv[2], "install") == 0) {
    return xv_skills_install() ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "config") == 0) {
    return probe_config_status();
  }
  if (argc >= 2 && strcmp(argv[1], "provision") == 0) {
    return probe_provision(argc, argv);
  }
  if (argc >= 5 && (strcmp(argv[1], "service") == 0 ||
                    strcmp(argv[1], "daemon") == 0)) {
    char *end = NULL;
    unsigned long port = strtoul(argv[3], &end, 10);
    bool use_tls = argc == 6 && strcmp(argv[5], "tls") == 0;
    if (end == argv[3] || *end != '\0' || port == 0U || port > UINT16_MAX ||
        argc > 6 || (argc == 6 && !use_tls)) {
      probe_usage();
      return 2;
    }
    (void)xv_skills_install();
    if (strcmp(argv[1], "daemon") == 0) {
      return xv_service_run_daemon(argv[2], (uint16_t)port, argv[4], use_tls);
    }
    return xv_service_run_once(argv[2], (uint16_t)port, argv[4], use_tls);
  }
#endif

  if (argc == 2 && strcmp(argv[1], "version") == 0) {
    printf("xiaov: firmware version=%s target=%s\n",
           XIAOV_FIRMWARE_VERSION, XIAOV_FIRMWARE_TARGET);
    return 0;
  }

  if (!probe_init(&probe)) {
    printf("xiaov: initialization failed\n");
    return 1;
  }
  if (argc == 1 || (argc == 2 && strcmp(argv[1], "selftest") == 0)) {
    return probe_selftest(&probe);
  }

  for (index = 1; index < argc; ++index) {
    xv_app_event_t event;
    if (!probe_find_event(argv[index], &event)) {
      probe_usage();
      return 2;
    }
    if (!probe_dispatch(&probe, event)) {
      return 1;
    }
  }

  printf("xiaov: state=%s pcm_overwritten=%llu events_rejected=%llu\n",
         xv_state_name(probe.runtime.machine.state),
         (unsigned long long)xv_pcm_ring_overwritten(&probe.pcm_ring),
         (unsigned long long)xv_event_queue_rejected(&probe.event_queue));
  return 0;
}
