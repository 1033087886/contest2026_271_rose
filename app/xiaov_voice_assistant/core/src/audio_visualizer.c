/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "xiaov/audio_visualizer.h"

#include <limits.h>
#include <string.h>

#define XV_VISUALIZER_COEFFICIENT_Q 14U
#define XV_VISUALIZER_INPUT_SHIFT 5U
#define XV_VISUALIZER_LEVEL_FLOOR_EXPONENT 14U

static const int16_t g_coefficients_q14[XV_AUDIO_VISUALIZER_BANDS] = {
    32610, 32138, 31357, 30274, 27246, 23170, 12540, 0,
};

static uint8_t log_level(uint64_t power) {
  unsigned int exponent = 0U;
  unsigned int fraction;
  unsigned int level;
  uint64_t base;

  if (power == 0U) {
    return 0U;
  }
  {
    uint64_t scan = power;
    while (scan > 1U) {
      scan >>= 1U;
      exponent += 1U;
    }
  }
  if (exponent <= XV_VISUALIZER_LEVEL_FLOOR_EXPONENT) {
    return 0U;
  }
  base = (uint64_t)1U << exponent;
  fraction = (unsigned int)(((power - base) << 2U) / base);
  level = (exponent - XV_VISUALIZER_LEVEL_FLOOR_EXPONENT) * 4U + fraction;
  return level > XV_AUDIO_VISUALIZER_LEVEL_MAX
             ? XV_AUDIO_VISUALIZER_LEVEL_MAX
             : (uint8_t)level;
}

static int32_t triangular_window(size_t index) {
  if (index < XV_AUDIO_VISUALIZER_FRAME_SAMPLES / 2U) {
    return (int32_t)((index * 32767U) /
                     (XV_AUDIO_VISUALIZER_FRAME_SAMPLES / 2U));
  }
  return (int32_t)(
      ((XV_AUDIO_VISUALIZER_FRAME_SAMPLES - 1U - index) * 32767U) /
      (XV_AUDIO_VISUALIZER_FRAME_SAMPLES - 1U -
       XV_AUDIO_VISUALIZER_FRAME_SAMPLES / 2U));
}

bool xv_audio_visualizer_process(
    const int16_t *samples, size_t sample_count,
    xv_audio_visualizer_result_t *result) {
  int64_t sample_sum = 0;
  uint64_t absolute_sum = 0U;
  int32_t mean;
  size_t band;
  size_t index;

  if (samples == NULL || result == NULL ||
      sample_count != XV_AUDIO_VISUALIZER_FRAME_SAMPLES) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  for (index = 0U; index < sample_count; ++index) {
    int32_t value = samples[index];
    sample_sum += value;
    absolute_sum += (uint64_t)(value < 0 ? -value : value);
  }
  mean = (int32_t)(sample_sum / (int64_t)sample_count);
  {
    uint64_t average = absolute_sum / sample_count;
    uint64_t scaled = (average * XV_AUDIO_VISUALIZER_LEVEL_MAX + 16383U) /
                      32767U;
    result->energy = scaled > XV_AUDIO_VISUALIZER_LEVEL_MAX
                         ? XV_AUDIO_VISUALIZER_LEVEL_MAX
                         : (uint8_t)scaled;
  }

  for (band = 0U; band < XV_AUDIO_VISUALIZER_BANDS; ++band) {
    int32_t previous = 0;
    int32_t previous2 = 0;
    const int32_t coefficient = g_coefficients_q14[band];

    for (index = 0U; index < sample_count; ++index) {
      int64_t centered = (int32_t)samples[index] - mean;
      int64_t windowed =
          (centered * triangular_window(index)) >> (15U +
                                                     XV_VISUALIZER_INPUT_SHIFT);
      int64_t recurrence =
          ((int64_t)coefficient * previous) >> XV_VISUALIZER_COEFFICIENT_Q;
      int64_t current = windowed + recurrence - previous2;
      if (current < INT32_MIN || current > INT32_MAX) {
        return false;
      }
      previous2 = previous;
      previous = (int32_t)current;
    }
    {
      int64_t cross = ((int64_t)coefficient * previous * previous2) >>
                      XV_VISUALIZER_COEFFICIENT_Q;
      int64_t signed_power = (int64_t)previous * previous +
                             (int64_t)previous2 * previous2 - cross;
      result->bands[band] =
          log_level(signed_power > 0 ? (uint64_t)signed_power : 0U);
    }
  }
  return true;
}
