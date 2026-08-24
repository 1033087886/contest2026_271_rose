/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XIAOV_TFLM_KWS_ADAPTER_H
#define XIAOV_TFLM_KWS_ADAPTER_H

#include "xiaov/kws_controller.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef XV_TFLM_KWS_TENSOR_ARENA_BYTES
#  ifdef CONFIG_EXAMPLES_XIAOV_KWS_TENSOR_ARENA_BYTES
#    define XV_TFLM_KWS_TENSOR_ARENA_BYTES \
      CONFIG_EXAMPLES_XIAOV_KWS_TENSOR_ARENA_BYTES
#  else
#    define XV_TFLM_KWS_TENSOR_ARENA_BYTES (128U * 1024U)
#  endif
#endif

#define XV_TFLM_KWS_MAX_WAKE_CLASSES 32U

/* Upper bound on one feature window, used to size the queue entries that carry
 * windows between the capture thread and the inference thread. Both deployed
 * models take 99 frames x 24 bins; xv_tflm_kws_advance_frontend rejects a
 * frontend that asks for more rather than overrunning this buffer. */
#define XV_KWS_FRONTEND_MAX_FEATURE_ELEMENTS 2376U

typedef enum {
  XV_TFLM_KWS_STATUS_OK = 0,
  XV_TFLM_KWS_STATUS_NEED_MORE_AUDIO,
  XV_TFLM_KWS_STATUS_GATED,
  XV_TFLM_KWS_ERROR_NOT_INITIALIZED,
  XV_TFLM_KWS_ERROR_BAD_ARGUMENT,
  XV_TFLM_KWS_ERROR_MODEL_BUFFER,
  XV_TFLM_KWS_ERROR_SCHEMA_VERSION,
  XV_TFLM_KWS_ERROR_OP_REGISTRATION,
  XV_TFLM_KWS_ERROR_TENSOR_ALLOCATION,
  XV_TFLM_KWS_ERROR_TENSOR_CONTRACT,
  XV_TFLM_KWS_ERROR_QUANTIZATION,
  XV_TFLM_KWS_ERROR_FRONTEND,
  XV_TFLM_KWS_ERROR_INVOKE,
  XV_TFLM_KWS_ERROR_DECISION
} xv_tflm_kws_status_t;

typedef struct {
  const int8_t *data;
  size_t element_count;
  float scale;
  int32_t zero_point;
} xv_tflm_kws_features_t;

typedef struct {
  int8_t *data;
  size_t element_count;
  float scale;
  int32_t zero_point;
} xv_tflm_kws_feature_buffer_t;

typedef enum {
  XV_TFLM_KWS_FRONTEND_ERROR = -1,
  XV_TFLM_KWS_FRONTEND_NEED_MORE_AUDIO = 0,
  XV_TFLM_KWS_FRONTEND_FEATURES_READY = 1
} xv_tflm_kws_frontend_result_t;

/* A reusable frontend owns its history in context and writes a complete int8
 * feature tensor only when it returns FEATURES_READY. It may consume short PCM
 * frames over several calls. The adapter supplies the model's exact scale and
 * zero point in output and rejects callbacks that alter that contract. */
typedef xv_tflm_kws_frontend_result_t (*xv_tflm_kws_frontend_process_fn)(
    void *context, const int16_t *samples, size_t sample_count,
    uint32_t sample_rate_hz, xv_tflm_kws_feature_buffer_t *output);

typedef void (*xv_tflm_kws_frontend_reset_fn)(void *context);

typedef struct {
  void *context;
  uint32_t sample_rate_hz;
  xv_tflm_kws_frontend_process_fn process;
  xv_tflm_kws_frontend_reset_fn reset;
} xv_tflm_kws_frontend_t;

typedef struct {
  const uint8_t *model_data;
  size_t model_size;

  size_t input_elements;
  float input_scale;
  int32_t input_zero_point;
  size_t output_elements;
  float output_scale;
  int32_t output_zero_point;

  const uint16_t *wake_class_indices;
  size_t wake_class_count;
  xv_kws_controller_config_t decision;

  /* Leave process NULL for callers that provide precomputed features. */
  xv_tflm_kws_frontend_t frontend;
} xv_tflm_kws_config_t;

typedef struct {
  uint16_t score_q15;
  int8_t raw_score;
  uint16_t output_index;
  xv_kws_decision_t decision;
} xv_tflm_kws_result_t;

typedef struct {
  uint64_t init_calls;
  uint64_t init_failures;
  uint64_t argument_errors;
  uint64_t model_buffer_errors;
  uint64_t schema_version_errors;
  uint64_t op_registration_errors;
  uint64_t tensor_allocation_errors;
  uint64_t tensor_contract_errors;
  uint64_t quantization_errors;
  uint64_t feature_calls;
  uint64_t pcm_calls;
  uint64_t frontend_need_more;
  uint64_t frontend_errors;
  uint64_t inference_gated;
  uint64_t inference_attempts;
  uint64_t inference_successes;
  uint64_t invoke_errors;
  uint64_t decision_errors;
  uint64_t wake_triggers;
  uint64_t class_switch_resets;
  /* Retained across model reinitialization for boot-lifetime telemetry. */
  uint16_t max_score_q15;
  uint16_t max_output_index;
  size_t arena_used_bytes;
  xv_tflm_kws_status_t last_status;
  xv_kws_controller_stats_t controller;
} xv_tflm_kws_stats_t;

/* This adapter is a single, serialized device instance. It performs no heap
 * allocation and keeps the TFLM interpreter in a fixed static arena. Model
 * bytes and frontend context must remain alive until shutdown. */
xv_tflm_kws_status_t xv_tflm_kws_init(const xv_tflm_kws_config_t *config);
void xv_tflm_kws_shutdown(void);
bool xv_tflm_kws_is_ready(void);

xv_tflm_kws_status_t xv_tflm_kws_reset_stream(void);

xv_tflm_kws_status_t xv_tflm_kws_process_features(
    const xv_tflm_kws_features_t *features, uint64_t now_ms,
    xv_tflm_kws_result_t *result);

xv_tflm_kws_status_t xv_tflm_kws_process_pcm(
    const int16_t *samples, size_t sample_count, uint32_t sample_rate_hz,
    uint64_t now_ms, xv_tflm_kws_result_t *result);

/* Always advances the streaming frontend. When inference_enabled is false,
 * complete feature windows are retained but Invoke is skipped, allowing VAD
 * to reduce idle CPU without truncating wake-word context. */
xv_tflm_kws_status_t xv_tflm_kws_process_pcm_gated(
    const int16_t *samples, size_t sample_count, uint32_t sample_rate_hz,
    uint64_t now_ms, bool inference_enabled,
    xv_tflm_kws_result_t *result);

/* Split frontend from inference so the two can run on different threads.
 *
 * Board measurement: one Invoke costs ~377 ms while the frontend emits a window
 * every 200 ms. Running both on the capture thread makes it 1.9x slower than
 * audio arrives, the 80 ms ALSA ring overruns, and only ~35% of the audio ever
 * reaches the detector -- standby never wakes.
 *
 * Windows cannot simply be skipped to catch up: the decision rule needs two
 * *consecutive* qualifying windows, and dropping to the newest window costs
 * 30-38 points of recall (build/rc17/decimate_windows.py). The detector must
 * therefore lag rather than skip, which means every emitted window is queued in
 * order and scored later.
 *
 * xv_tflm_kws_advance_frontend runs on the capture thread and only touches the
 * frontend; it copies the finished window out to `window` instead of leaving it
 * in the interpreter's input tensor, so the consumer can score the previous
 * window concurrently. xv_tflm_kws_score_window runs on the consumer thread and
 * owns the interpreter, the controller and the stats.
 */
typedef struct {
  int8_t data[XV_KWS_FRONTEND_MAX_FEATURE_ELEMENTS];
  size_t element_count;
  bool inference_enabled;
  uint64_t now_ms;
} xv_tflm_kws_window_t;

/* Returns STATUS_OK when *window is filled, NEED_MORE_AUDIO when the frontend
 * needs more samples, and an error otherwise. Never invokes the model. */
xv_tflm_kws_status_t xv_tflm_kws_advance_frontend(
    const int16_t *samples, size_t sample_count, uint32_t sample_rate_hz,
    uint64_t now_ms, bool inference_enabled, xv_tflm_kws_window_t *window);

/* Scores one queued window. Pass contiguous=false when windows were dropped
 * between this one and the previous call, so a broken run cannot be counted as
 * consecutive. */
xv_tflm_kws_status_t xv_tflm_kws_score_window(
    const xv_tflm_kws_window_t *window, bool contiguous,
    xv_tflm_kws_result_t *result);

void xv_tflm_kws_get_stats(xv_tflm_kws_stats_t *stats);

/* Spread of the int8 feature window handed to the most recent Invoke.
 *
 * Populated only under CONFIG_EXAMPLES_XIAOV_KWS_TRACE_SCORES; otherwise valid
 * is false. mean_q4 is the mean scaled by 16 so no floating point is needed on
 * the logging path.
 *
 * mean_q4 separates "too quiet to score" from a firmware fault. Board
 * measurement puts the wake model's firing cliff at -1929 in the model's input
 * domain: a window one step below that collapses from 32383 to 0, so a miss
 * below -1929 is a microphone level problem. Comparing against host numbers
 * requires requantising the host frontend's output (scale 1.0, zero point 0)
 * into the model's domain first.
 */
typedef struct {
  int8_t minimum;
  int8_t maximum;
  int32_t mean_q4;
  bool valid;
} xv_tflm_kws_feature_stats_t;

void xv_tflm_kws_get_feature_stats(xv_tflm_kws_feature_stats_t *stats);

/* Every integer step behind score_q15, for the case where the score disagrees
 * with the raw output it was derived from. Populated only under
 * CONFIG_EXAMPLES_XIAOV_KWS_TRACE_SCORES; probability is carried as Q14 and
 * scale as Q16 so reading these values needs no float.
 */
typedef struct {
  int8_t raw_class1;
  int8_t raw_class2;
  int32_t output_zero_point;
  int32_t output_scale_q16;
  int32_t best_probability_q14;
  uint16_t score_q15;
  bool valid;
} xv_tflm_kws_score_debug_t;

void xv_tflm_kws_get_score_debug(xv_tflm_kws_score_debug_t *debug);

#ifdef __cplusplus
}
#endif

#endif
