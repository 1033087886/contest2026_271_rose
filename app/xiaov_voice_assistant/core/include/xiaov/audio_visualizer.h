/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XIAOV_AUDIO_VISUALIZER_H
#define XIAOV_AUDIO_VISUALIZER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XV_AUDIO_VISUALIZER_SAMPLE_RATE_HZ 16000U
#define XV_AUDIO_VISUALIZER_FRAME_SAMPLES 320U
#define XV_AUDIO_VISUALIZER_BANDS 8U
#define XV_AUDIO_VISUALIZER_LEVEL_MAX 100U

typedef struct {
  uint8_t bands[XV_AUDIO_VISUALIZER_BANDS];
  uint8_t energy;
} xv_audio_visualizer_result_t;

/* Computes coarse 250/500/750/1000/1500/2000/3000/4000 Hz levels from one
 * 20 ms mono PCM frame. The result is intended for UI motion, not acoustic
 * measurement or KWS features. */
bool xv_audio_visualizer_process(
    const int16_t *samples, size_t sample_count,
    xv_audio_visualizer_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
