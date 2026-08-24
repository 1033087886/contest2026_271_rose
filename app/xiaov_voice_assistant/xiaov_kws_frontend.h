/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XIAOV_KWS_FRONTEND_H
#define XIAOV_KWS_FRONTEND_H

#include "xiaov_tflm_kws_adapter.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XV_KWS_FRONTEND_SAMPLE_RATE_HZ 16000U
#define XV_KWS_FRONTEND_FRAME_SAMPLES 400U
#define XV_KWS_FRONTEND_FRAME_STEP_SAMPLES 320U
#define XV_KWS_FRONTEND_FEATURE_BINS 24U
#define XV_KWS_FRONTEND_CONTEXT_FRAMES 99U
#define XV_KWS_FRONTEND_CONTEXT_SAMPLES                                    \
  (XV_KWS_FRONTEND_FRAME_SAMPLES +                                        \
   (XV_KWS_FRONTEND_CONTEXT_FRAMES - 1U) *                                \
       XV_KWS_FRONTEND_FRAME_STEP_SAMPLES)
#define XV_KWS_FRONTEND_FEATURE_ELEMENTS \
  (XV_KWS_FRONTEND_FEATURE_BINS * XV_KWS_FRONTEND_CONTEXT_FRAMES)
#define XV_KWS_FRONTEND_EMIT_STRIDE_FRAMES 10U

typedef struct {
  int16_t audio[XV_KWS_FRONTEND_FRAME_SAMPLES];
  int16_t ordered_frame[XV_KWS_FRONTEND_FRAME_SAMPLES];
  int8_t features[XV_KWS_FRONTEND_CONTEXT_FRAMES]
                 [XV_KWS_FRONTEND_FEATURE_BINS];
  size_t audio_write;
  size_t audio_count;
  size_t samples_until_frame;
  size_t feature_write;
  size_t feature_count;
  size_t frames_since_emit;
  bool has_emitted;
} xv_kws_pcm_frontend_t;

void xv_kws_frontend_init(xv_kws_pcm_frontend_t *frontend);
void xv_kws_frontend_reset(void *context);

/* Command mode starts immediately after an explicit touch. Seed the context
 * with silence so short commands can be evaluated after one normal stride
 * instead of waiting almost two seconds for a cold context. */
void xv_kws_frontend_prefill_silence(void *context);

xv_tflm_kws_frontend_result_t xv_kws_frontend_process(
    void *context, const int16_t *samples, size_t sample_count,
    uint32_t sample_rate_hz, xv_tflm_kws_feature_buffer_t *output);

/* Exposed for host golden-vector verification and deterministic training
 * probes. Production streaming code should call xv_kws_frontend_process. */
bool xv_kws_frontend_extract_frame(const int16_t *samples, size_t sample_count,
                                   int8_t *features, size_t feature_count);

#ifdef __cplusplus
}
#endif

#endif
