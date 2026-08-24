/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "xiaov_capture_gain.h"

#include <limits.h>

static int16_t saturate_pcm16(int32_t value) {
  if (value > INT16_MAX) {
    return INT16_MAX;
  }
  if (value < INT16_MIN) {
    return INT16_MIN;
  }
  return (int16_t)value;
}

bool xv_capture_gain_apply(const xv_capture_gain_config_t *config,
                           const int16_t *input, size_t sample_count,
                           int16_t *output) {
  uint32_t peak = 0U;
  uint32_t gain_q8;
  size_t index;

  if (config == NULL || input == NULL || output == NULL || sample_count == 0U ||
      config->target_peak == 0U || config->target_peak > INT16_MAX ||
      config->max_gain_q8 < XV_CAPTURE_GAIN_Q8_ONE) {
    return false;
  }

  for (index = 0U; index < sample_count; ++index) {
    const int32_t sample = input[index];
    const uint32_t magnitude =
        (uint32_t)(sample < 0 ? -sample : sample);
    if (magnitude > peak) {
      peak = magnitude;
    }
  }

  if (peak == 0U) {
    gain_q8 = config->max_gain_q8;
  } else {
    gain_q8 = ((uint32_t)config->target_peak * XV_CAPTURE_GAIN_Q8_ONE +
               peak / 2U) /
              peak;
    if (gain_q8 < XV_CAPTURE_GAIN_Q8_ONE) {
      gain_q8 = XV_CAPTURE_GAIN_Q8_ONE;
    } else if (gain_q8 > config->max_gain_q8) {
      gain_q8 = config->max_gain_q8;
    }
  }

  for (index = 0U; index < sample_count; ++index) {
    int32_t scaled = (int32_t)input[index] * (int32_t)gain_q8;
    if (scaled >= 0) {
      scaled = (scaled + (int32_t)XV_CAPTURE_GAIN_Q8_ONE / 2) /
               (int32_t)XV_CAPTURE_GAIN_Q8_ONE;
    } else {
      scaled = -((-scaled + (int32_t)XV_CAPTURE_GAIN_Q8_ONE / 2) /
                 (int32_t)XV_CAPTURE_GAIN_Q8_ONE);
    }
    output[index] = saturate_pcm16(scaled);
  }
  return true;
}
