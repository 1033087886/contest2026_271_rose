/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XIAOV_KWS_CONTROLLER_H
#define XIAOV_KWS_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XV_KWS_SCORE_Q15_ONE 32767U

typedef struct {
  uint16_t score_threshold_q15;
  uint16_t consecutive_hits;
  uint32_t cooldown_ms;
} xv_kws_controller_config_t;

typedef struct {
  uint64_t frames_seen;
  uint64_t above_threshold_frames;
  uint64_t triggers;
  uint64_t cooldown_suppressions;
  uint64_t latched_suppressions;
  uint64_t score_range_errors;
  uint64_t clock_errors;
  uint64_t temporal_resets;
} xv_kws_controller_stats_t;

typedef enum {
  XV_KWS_DECISION_IDLE = 0,
  XV_KWS_DECISION_CANDIDATE,
  XV_KWS_DECISION_TRIGGERED,
  XV_KWS_DECISION_SUPPRESSED,
  XV_KWS_DECISION_ERROR_BAD_ARGUMENT,
  XV_KWS_DECISION_ERROR_SCORE_RANGE,
  XV_KWS_DECISION_ERROR_CLOCK
} xv_kws_decision_t;

/* Caller-owned, allocation-free wake decision state. Scores are probabilities
 * in unsigned Q15: 0 is 0.0 and XV_KWS_SCORE_Q15_ONE is 1.0. */
typedef struct {
  xv_kws_controller_config_t config;
  xv_kws_controller_stats_t stats;
  uint64_t last_time_ms;
  uint64_t last_trigger_ms;
  uint16_t hit_run;
  bool initialized;
  bool has_time;
  bool has_trigger;
  bool armed;
} xv_kws_controller_t;

/* Defaults are an uncalibrated engineering starting point: 0.80 probability,
 * three consecutive frames and a two-second cooldown. */
void xv_kws_controller_config_default(xv_kws_controller_config_t *config);

bool xv_kws_controller_init(xv_kws_controller_t *controller,
                            const xv_kws_controller_config_t *config);

/* Clears candidate/cooldown/latch state while preserving lifetime statistics.
 * Use this when an audio stream is restarted or its monotonic clock changes. */
void xv_kws_controller_reset_temporal(xv_kws_controller_t *controller);

void xv_kws_controller_clear_stats(xv_kws_controller_t *controller);

/* A trigger is emitted only after consecutive_hits qualifying frames. The
 * detector must then observe a below-threshold frame before it can re-arm, and
 * cooldown_ms must also have elapsed. Monotonic time regressions are rejected
 * and reset temporal state instead of leaving a stale cooldown active. */
xv_kws_decision_t xv_kws_controller_process(xv_kws_controller_t *controller,
                                            uint16_t score_q15,
                                            uint64_t now_ms);

bool xv_kws_controller_in_cooldown(const xv_kws_controller_t *controller,
                                   uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
