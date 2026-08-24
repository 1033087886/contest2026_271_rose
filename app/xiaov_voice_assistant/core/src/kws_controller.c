/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "xiaov/kws_controller.h"

#include <stddef.h>
#include <string.h>

static void increment_saturated(uint64_t *value) {
  if (*value != UINT64_MAX) {
    *value += 1U;
  }
}

void xv_kws_controller_config_default(xv_kws_controller_config_t *config) {
  if (config == NULL) {
    return;
  }
  config->score_threshold_q15 = 26214U;
  config->consecutive_hits = 3U;
  config->cooldown_ms = 2000U;
}

bool xv_kws_controller_init(xv_kws_controller_t *controller,
                            const xv_kws_controller_config_t *config) {
  xv_kws_controller_config_t selected;

  if (controller == NULL) {
    return false;
  }
  if (config == NULL) {
    xv_kws_controller_config_default(&selected);
  } else {
    selected = *config;
  }
  if (selected.score_threshold_q15 > XV_KWS_SCORE_Q15_ONE ||
      selected.consecutive_hits == 0U) {
    return false;
  }

  memset(controller, 0, sizeof(*controller));
  controller->config = selected;
  controller->initialized = true;
  controller->armed = true;
  return true;
}

void xv_kws_controller_reset_temporal(xv_kws_controller_t *controller) {
  if (controller == NULL || !controller->initialized) {
    return;
  }
  controller->last_time_ms = 0U;
  controller->last_trigger_ms = 0U;
  controller->hit_run = 0U;
  controller->has_time = false;
  controller->has_trigger = false;
  controller->armed = true;
  increment_saturated(&controller->stats.temporal_resets);
}

void xv_kws_controller_clear_stats(xv_kws_controller_t *controller) {
  if (controller == NULL || !controller->initialized) {
    return;
  }
  memset(&controller->stats, 0, sizeof(controller->stats));
}

bool xv_kws_controller_in_cooldown(const xv_kws_controller_t *controller,
                                   uint64_t now_ms) {
  if (controller == NULL || !controller->initialized ||
      !controller->has_trigger || controller->config.cooldown_ms == 0U ||
      (controller->has_time && now_ms < controller->last_time_ms) ||
      now_ms < controller->last_trigger_ms) {
    return false;
  }
  return now_ms - controller->last_trigger_ms < controller->config.cooldown_ms;
}

xv_kws_decision_t xv_kws_controller_process(xv_kws_controller_t *controller,
                                            uint16_t score_q15,
                                            uint64_t now_ms) {
  if (controller == NULL || !controller->initialized) {
    return XV_KWS_DECISION_ERROR_BAD_ARGUMENT;
  }
  if (score_q15 > XV_KWS_SCORE_Q15_ONE) {
    increment_saturated(&controller->stats.score_range_errors);
    controller->hit_run = 0U;
    return XV_KWS_DECISION_ERROR_SCORE_RANGE;
  }
  if (controller->has_time && now_ms < controller->last_time_ms) {
    increment_saturated(&controller->stats.clock_errors);
    xv_kws_controller_reset_temporal(controller);
    controller->last_time_ms = now_ms;
    controller->has_time = true;
    return XV_KWS_DECISION_ERROR_CLOCK;
  }

  controller->last_time_ms = now_ms;
  controller->has_time = true;
  increment_saturated(&controller->stats.frames_seen);

  if (score_q15 < controller->config.score_threshold_q15) {
    controller->hit_run = 0U;
    controller->armed = true;
    return XV_KWS_DECISION_IDLE;
  }

  increment_saturated(&controller->stats.above_threshold_frames);
  if (xv_kws_controller_in_cooldown(controller, now_ms)) {
    controller->hit_run = 0U;
    increment_saturated(&controller->stats.cooldown_suppressions);
    return XV_KWS_DECISION_SUPPRESSED;
  }
  if (!controller->armed) {
    controller->hit_run = 0U;
    increment_saturated(&controller->stats.latched_suppressions);
    return XV_KWS_DECISION_SUPPRESSED;
  }

  if (controller->hit_run != UINT16_MAX) {
    controller->hit_run += 1U;
  }
  if (controller->hit_run < controller->config.consecutive_hits) {
    return XV_KWS_DECISION_CANDIDATE;
  }

  controller->hit_run = 0U;
  controller->armed = false;
  controller->last_trigger_ms = now_ms;
  controller->has_trigger = true;
  increment_saturated(&controller->stats.triggers);
  return XV_KWS_DECISION_TRIGGERED;
}
