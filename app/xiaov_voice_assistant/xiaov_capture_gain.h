/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XIAOV_CAPTURE_GAIN_H
#define XIAOV_CAPTURE_GAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XV_CAPTURE_GAIN_Q8_ONE 256U

typedef struct {
  uint16_t target_peak;
  uint16_t max_gain_q8;
} xv_capture_gain_config_t;

/* Raise one capture block toward target_peak, but never attenuate a loud block.
 * Gain is selected from the block peak, capped at max_gain_q8, and applied with
 * signed rounding and PCM16 saturation. Input and output may alias.
 */
bool xv_capture_gain_apply(const xv_capture_gain_config_t *config,
                           const int16_t *input, size_t sample_count,
                           int16_t *output);

#ifdef __cplusplus
}
#endif

#endif
