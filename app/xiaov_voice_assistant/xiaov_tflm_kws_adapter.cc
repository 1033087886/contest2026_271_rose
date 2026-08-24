/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "xiaov_tflm_kws_adapter.h"

#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "flatbuffers/verifier.h"
#include "tensorflow/lite/core/c/common.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace {

using KwsOpResolver = tflite::MicroMutableOpResolver<13>;

struct AdapterState {
  bool ready;
  xv_tflm_kws_config_t config;
  uint16_t wake_class_indices[XV_TFLM_KWS_MAX_WAKE_CLASSES];
  xv_kws_controller_t controller;
  uint16_t candidate_output_index;
  bool candidate_active;
  TfLiteTensor *input;
  TfLiteTensor *output;
  xv_tflm_kws_stats_t stats;
};

alignas(16) uint8_t g_tensor_arena[XV_TFLM_KWS_TENSOR_ARENA_BYTES];
alignas(KwsOpResolver) uint8_t g_resolver_storage[sizeof(KwsOpResolver)];
alignas(tflite::MicroInterpreter)
    uint8_t g_interpreter_storage[sizeof(tflite::MicroInterpreter)];

KwsOpResolver *g_resolver = nullptr;
tflite::MicroInterpreter *g_interpreter = nullptr;
AdapterState g_state = {};

struct FrontendCounters {
  uint64_t argument_errors;
  uint64_t pcm_calls;
  uint64_t frontend_need_more;
  uint64_t frontend_errors;
};

FrontendCounters g_frontend_counters = {};
std::atomic<int> g_last_status{XV_TFLM_KWS_ERROR_NOT_INITIALIZED};

/* The capture-side frontend and the inference engine are independent hot paths.
 *
 * Since inference moved to its own thread, three threads reach this adapter: the
 * capture thread (advance_frontend), the inference worker (score_window) and the
 * service thread (reset_stream / init / shutdown). Reset() and Invoke() must not
 * overlap -- a standby restart that lands mid-Invoke leaves the graph clobbered
 * and every later Invoke returns score 0 with no error status, which is exactly
 * the failure observed on 2026-08-10.
 *
 * The original race fix put both paths behind g_engine_lock. That made every
 * 377 ms Invoke block the capture callback, exactly reproducing the ALSA
 * starvation the queue was meant to fix: each Invoke was followed by one PCM
 * prepare. Keep a separate frontend lock and take both only for lifecycle calls.
 * The lock order for those calls is always frontend, then engine.
 */
pthread_mutex_t g_frontend_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_engine_lock = PTHREAD_MUTEX_INITIALIZER;

class FrontendGuard {
 public:
  FrontendGuard() { pthread_mutex_lock(&g_frontend_lock); }
  ~FrontendGuard() { pthread_mutex_unlock(&g_frontend_lock); }
  FrontendGuard(const FrontendGuard &) = delete;
  FrontendGuard &operator=(const FrontendGuard &) = delete;
};

class EngineGuard {
 public:
  EngineGuard() { pthread_mutex_lock(&g_engine_lock); }
  ~EngineGuard() { pthread_mutex_unlock(&g_engine_lock); }
  EngineGuard(const EngineGuard &) = delete;
  EngineGuard &operator=(const EngineGuard &) = delete;
};

class RuntimeGuard {
 public:
  RuntimeGuard() {
    pthread_mutex_lock(&g_frontend_lock);
    pthread_mutex_lock(&g_engine_lock);
  }
  ~RuntimeGuard() {
    pthread_mutex_unlock(&g_engine_lock);
    pthread_mutex_unlock(&g_frontend_lock);
  }
  RuntimeGuard(const RuntimeGuard &) = delete;
  RuntimeGuard &operator=(const RuntimeGuard &) = delete;
};
#ifdef CONFIG_EXAMPLES_XIAOV_KWS_TRACE_SCORES
xv_tflm_kws_feature_stats_t g_feature_stats = {};
xv_tflm_kws_score_debug_t g_score_debug = {};
#endif

void IncrementSaturated(uint64_t *value) {
  if (*value != UINT64_MAX) {
    *value += 1U;
  }
}

xv_tflm_kws_status_t SetStatus(xv_tflm_kws_status_t status) {
  g_last_status.store(static_cast<int>(status), std::memory_order_relaxed);
  return status;
}

uint64_t SaturatingAdd(uint64_t left, uint64_t right) {
  return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

void DestroyRuntime(void) {
  g_state.ready = false;
  g_state.input = nullptr;
  g_state.output = nullptr;
  if (g_interpreter != nullptr) {
    g_interpreter->~MicroInterpreter();
    g_interpreter = nullptr;
  }
  if (g_resolver != nullptr) {
    g_resolver->~KwsOpResolver();
    g_resolver = nullptr;
  }
}

bool PositiveFinite(float value) {
  return value == value && value > 0.0F && value <= 3.402823466e+38F;
}

float Absolute(float value) { return value < 0.0F ? -value : value; }

bool QuantScaleMatches(float actual, float expected) {
  const float tolerance = 1.0e-7F + Absolute(expected) * 1.0e-5F;
  return PositiveFinite(actual) && PositiveFinite(expected) &&
         Absolute(actual - expected) <= tolerance;
}

bool Int8ZeroPoint(int32_t value) { return value >= -128 && value <= 127; }

bool TensorElementCount(const TfLiteTensor *tensor, size_t *elements) {
  size_t count = 1U;
  int index;

  if (tensor == nullptr || elements == nullptr || tensor->dims == nullptr ||
      tensor->dims->size <= 0) {
    return false;
  }
  for (index = 0; index < tensor->dims->size; ++index) {
    const int dimension = tensor->dims->data[index];
    if (dimension <= 0 || count > SIZE_MAX / static_cast<size_t>(dimension)) {
      return false;
    }
    count *= static_cast<size_t>(dimension);
  }
  *elements = count;
  return true;
}

bool ValidateInt8Tensor(const TfLiteTensor *tensor, size_t expected_elements,
                        float expected_scale, int32_t expected_zero_point,
                        bool *quantization_error) {
  const TfLiteAffineQuantization *affine;
  size_t elements = 0U;

  *quantization_error = false;
  if (tensor == nullptr || tensor->type != kTfLiteInt8 ||
      tensor->data.int8 == nullptr ||
      !TensorElementCount(tensor, &elements) || elements != expected_elements ||
      tensor->bytes != expected_elements) {
    return false;
  }

  if (!Int8ZeroPoint(expected_zero_point) ||
      !QuantScaleMatches(tensor->params.scale, expected_scale) ||
      tensor->params.zero_point != expected_zero_point ||
      tensor->quantization.type != kTfLiteAffineQuantization ||
      tensor->quantization.params == nullptr) {
    *quantization_error = true;
    return false;
  }

  affine = static_cast<const TfLiteAffineQuantization *>(
      tensor->quantization.params);
  if (affine->scale == nullptr || affine->zero_point == nullptr ||
      affine->scale->size != 1 || affine->zero_point->size != 1 ||
      !QuantScaleMatches(affine->scale->data[0], expected_scale) ||
      affine->zero_point->data[0] != expected_zero_point) {
    *quantization_error = true;
    return false;
  }
  return true;
}

TfLiteStatus RegisterFixedOps(KwsOpResolver *resolver) {
  if (resolver->AddReshape() != kTfLiteOk ||
      resolver->AddFullyConnected() != kTfLiteOk ||
      resolver->AddDepthwiseConv2D() != kTfLiteOk ||
      resolver->AddConv2D() != kTfLiteOk ||
      resolver->AddSoftmax() != kTfLiteOk ||
      resolver->AddAveragePool2D() != kTfLiteOk ||
      resolver->AddMaxPool2D() != kTfLiteOk ||
      resolver->AddMean() != kTfLiteOk || resolver->AddAdd() != kTfLiteOk ||
      resolver->AddMul() != kTfLiteOk || resolver->AddPack() != kTfLiteOk ||
      resolver->AddShape() != kTfLiteOk ||
      resolver->AddStridedSlice() != kTfLiteOk) {
    return kTfLiteError;
  }
  return kTfLiteOk;
}

xv_tflm_kws_status_t InitFailure(xv_tflm_kws_status_t status,
                                 uint64_t *counter) {
  IncrementSaturated(&g_state.stats.init_failures);
  IncrementSaturated(counter);
  DestroyRuntime();
  return SetStatus(status);
}

bool ValidateConfig(const xv_tflm_kws_config_t *config) {
  size_t index;
  size_t other;

  if (config == nullptr || config->model_data == nullptr ||
      config->model_size < 8U || config->input_elements == 0U ||
      config->output_elements == 0U ||
      !PositiveFinite(config->input_scale) ||
      !PositiveFinite(config->output_scale) ||
      !Int8ZeroPoint(config->input_zero_point) ||
      !Int8ZeroPoint(config->output_zero_point) ||
      config->wake_class_indices == nullptr ||
      config->wake_class_count == 0U ||
      config->wake_class_count > XV_TFLM_KWS_MAX_WAKE_CLASSES ||
      config->decision.consecutive_hits == 0U ||
      config->decision.score_threshold_q15 > XV_KWS_SCORE_Q15_ONE ||
      (config->frontend.process != nullptr &&
       config->frontend.sample_rate_hz == 0U) ||
      (config->frontend.process == nullptr &&
       (config->frontend.reset != nullptr ||
        config->frontend.sample_rate_hz != 0U))) {
    return false;
  }
  if ((reinterpret_cast<uintptr_t>(config->model_data) & 0x3U) != 0U) {
    return false;
  }
  for (index = 0U; index < config->wake_class_count; ++index) {
    if (config->wake_class_indices[index] >= config->output_elements) {
      return false;
    }
    for (other = 0U; other < index; ++other) {
      if (config->wake_class_indices[index] ==
          config->wake_class_indices[other]) {
        return false;
      }
    }
  }
  return true;
}

void ClearResult(xv_tflm_kws_result_t *result) {
  if (result == nullptr) {
    return;
  }
  result->score_q15 = 0U;
  result->raw_score = -128;
  result->output_index = 0U;
  result->decision = XV_KWS_DECISION_IDLE;
}

xv_tflm_kws_status_t RunInference(uint64_t now_ms,
                                  xv_tflm_kws_result_t *result) {
  float best_probability = -1.0F;
  int8_t best_raw = -128;
  uint16_t best_index = 0U;
  size_t index;
  uint16_t score_q15;
  xv_kws_decision_t decision;

  IncrementSaturated(&g_state.stats.inference_attempts);
#ifdef CONFIG_EXAMPLES_XIAOV_KWS_TRACE_SCORES
  /* Snapshot the input window before Invoke so a missed wake can be attributed
   * to the microphone level rather than to the firmware. Taken here rather than
   * in the frontend because this is the exact buffer the interpreter reads.
   *
   * mean_q4 is the criterion: board measurement puts the firing cliff at -1929
   * in the model's input domain, so a window below that is too quiet to score
   * and nothing in the firmware is at fault. Scanning all 2376 elements per
   * inference is only worth it while diagnosing, hence the compile-time gate.
   */
  {
    const int8_t *const features = g_state.input->data.int8;
    const size_t count = g_state.config.input_elements;
    int32_t sum = 0;
    int8_t minimum = INT8_MAX;
    int8_t maximum = INT8_MIN;

    for (size_t i = 0U; i < count; ++i) {
      const int8_t value = features[i];
      if (value < minimum) {
        minimum = value;
      }
      if (value > maximum) {
        maximum = value;
      }
      sum += static_cast<int32_t>(value);
    }
    g_feature_stats.minimum = minimum;
    g_feature_stats.maximum = maximum;
    g_feature_stats.mean_q4 =
        count != 0U ? (sum * 16) / static_cast<int32_t>(count) : 0;
    g_feature_stats.valid = count != 0U;
  }
#endif
  if (g_interpreter->Invoke() != kTfLiteOk) {
    IncrementSaturated(&g_state.stats.invoke_errors);
    return SetStatus(XV_TFLM_KWS_ERROR_INVOKE);
  }
  IncrementSaturated(&g_state.stats.inference_successes);

  for (index = 0U; index < g_state.config.wake_class_count; ++index) {
    const uint16_t output_index = g_state.wake_class_indices[index];
    const int8_t raw = g_state.output->data.int8[output_index];
    float probability =
        (static_cast<int32_t>(raw) - g_state.output->params.zero_point) *
        g_state.output->params.scale;
    if (probability < 0.0F) {
      probability = 0.0F;
    } else if (probability > 1.0F) {
      probability = 1.0F;
    }
    if (probability > best_probability) {
      best_probability = probability;
      best_raw = raw;
      best_index = output_index;
    }
  }

  score_q15 = static_cast<uint16_t>(
      best_probability * static_cast<float>(XV_KWS_SCORE_Q15_ONE) + 0.5F);
#ifdef CONFIG_EXAMPLES_XIAOV_KWS_TRACE_SCORES
  /* The board reports score_q15=0 on windows whose best raw output is -1,
   * which would be probability 0.496 and score 16253. Those two numbers come
   * out of the same arithmetic, so one of the intermediate values must not be
   * what it appears. Record every step as integers -- probability scaled by
   * 10000 -- so the logging path itself cannot mask a float problem.
   */
  g_score_debug.raw_class1 = g_state.output->data.int8[1];
  g_score_debug.raw_class2 =
      g_state.config.output_elements > 2U ? g_state.output->data.int8[2] : 0;
  g_score_debug.output_zero_point = g_state.output->params.zero_point;
  g_score_debug.output_scale_q16 =
      static_cast<int32_t>(g_state.output->params.scale * 65536.0F);
  g_score_debug.best_probability_q14 =
      static_cast<int32_t>(best_probability * 16384.0F);
  g_score_debug.score_q15 = score_q15;
  g_score_debug.valid = true;
#endif
  if (score_q15 > g_state.stats.max_score_q15) {
    g_state.stats.max_score_q15 = score_q15;
    g_state.stats.max_output_index = best_index;
  }
  if (score_q15 >= g_state.config.decision.score_threshold_q15) {
    if (g_state.candidate_active &&
        g_state.candidate_output_index != best_index) {
      xv_kws_controller_reset_temporal(&g_state.controller);
      IncrementSaturated(&g_state.stats.class_switch_resets);
    }
    g_state.candidate_output_index = best_index;
    g_state.candidate_active = true;
  } else {
    g_state.candidate_active = false;
  }
  decision = xv_kws_controller_process(&g_state.controller, score_q15, now_ms);
  result->score_q15 = score_q15;
  result->raw_score = best_raw;
  result->output_index = best_index;
  result->decision = decision;
  if (decision == XV_KWS_DECISION_TRIGGERED) {
    IncrementSaturated(&g_state.stats.wake_triggers);
  } else if (decision == XV_KWS_DECISION_ERROR_BAD_ARGUMENT ||
             decision == XV_KWS_DECISION_ERROR_SCORE_RANGE ||
             decision == XV_KWS_DECISION_ERROR_CLOCK) {
    IncrementSaturated(&g_state.stats.decision_errors);
    return SetStatus(XV_TFLM_KWS_ERROR_DECISION);
  }
  return SetStatus(XV_TFLM_KWS_STATUS_OK);
}

}  // namespace

extern "C" xv_tflm_kws_status_t
xv_tflm_kws_init(const xv_tflm_kws_config_t *config) {
  RuntimeGuard guard;
  const tflite::Model *model;
  bool input_quantization_error = false;
  bool output_quantization_error = false;
  size_t index;

  IncrementSaturated(&g_state.stats.init_calls);
  DestroyRuntime();
  std::memset(&g_state.controller, 0, sizeof(g_state.controller));
  g_state.candidate_output_index = 0U;
  g_state.candidate_active = false;
  g_state.stats.arena_used_bytes = 0U;

  if (!ValidateConfig(config)) {
    return InitFailure(XV_TFLM_KWS_ERROR_BAD_ARGUMENT,
                       &g_state.stats.argument_errors);
  }

  flatbuffers::Verifier verifier(config->model_data, config->model_size);
  if (!tflite::VerifyModelBuffer(verifier)) {
    return InitFailure(XV_TFLM_KWS_ERROR_MODEL_BUFFER,
                       &g_state.stats.model_buffer_errors);
  }
  model = tflite::GetModel(config->model_data);
  if (model == nullptr || model->version() != TFLITE_SCHEMA_VERSION) {
    return InitFailure(XV_TFLM_KWS_ERROR_SCHEMA_VERSION,
                       &g_state.stats.schema_version_errors);
  }

  std::memset(g_tensor_arena, 0, sizeof(g_tensor_arena));
  g_resolver = new (g_resolver_storage) KwsOpResolver();
  if (RegisterFixedOps(g_resolver) != kTfLiteOk) {
    return InitFailure(XV_TFLM_KWS_ERROR_OP_REGISTRATION,
                       &g_state.stats.op_registration_errors);
  }
  g_interpreter = new (g_interpreter_storage) tflite::MicroInterpreter(
      model, *g_resolver, g_tensor_arena, sizeof(g_tensor_arena));
  if (g_interpreter->initialization_status() != kTfLiteOk ||
      g_interpreter->AllocateTensors() != kTfLiteOk) {
    return InitFailure(XV_TFLM_KWS_ERROR_TENSOR_ALLOCATION,
                       &g_state.stats.tensor_allocation_errors);
  }
  g_state.stats.arena_used_bytes = g_interpreter->arena_used_bytes();

  if (g_interpreter->inputs_size() != 1U ||
      g_interpreter->outputs_size() != 1U) {
    return InitFailure(XV_TFLM_KWS_ERROR_TENSOR_CONTRACT,
                       &g_state.stats.tensor_contract_errors);
  }
  g_state.input = g_interpreter->input(0);
  g_state.output = g_interpreter->output(0);
  if (!ValidateInt8Tensor(g_state.input, config->input_elements,
                          config->input_scale, config->input_zero_point,
                          &input_quantization_error) ||
      !ValidateInt8Tensor(g_state.output, config->output_elements,
                          config->output_scale, config->output_zero_point,
                          &output_quantization_error)) {
    if (input_quantization_error || output_quantization_error) {
      return InitFailure(XV_TFLM_KWS_ERROR_QUANTIZATION,
                         &g_state.stats.quantization_errors);
    }
    return InitFailure(XV_TFLM_KWS_ERROR_TENSOR_CONTRACT,
                       &g_state.stats.tensor_contract_errors);
  }
  if (!xv_kws_controller_init(&g_state.controller, &config->decision)) {
    return InitFailure(XV_TFLM_KWS_ERROR_BAD_ARGUMENT,
                       &g_state.stats.argument_errors);
  }

  g_state.config = *config;
  for (index = 0U; index < config->wake_class_count; ++index) {
    g_state.wake_class_indices[index] = config->wake_class_indices[index];
  }
  g_state.config.wake_class_indices = g_state.wake_class_indices;
  g_state.ready = true;
  return SetStatus(XV_TFLM_KWS_STATUS_OK);
}

extern "C" void xv_tflm_kws_shutdown(void) {
  RuntimeGuard guard;

  DestroyRuntime();
}

extern "C" bool xv_tflm_kws_is_ready(void) {
  EngineGuard guard;
  return g_state.ready;
}

extern "C" xv_tflm_kws_status_t xv_tflm_kws_reset_stream(void) {
  RuntimeGuard guard;

  if (!g_state.ready || g_interpreter == nullptr) {
    return SetStatus(XV_TFLM_KWS_ERROR_NOT_INITIALIZED);
  }
  if (g_interpreter->Reset() != kTfLiteOk) {
    IncrementSaturated(&g_state.stats.invoke_errors);
    return SetStatus(XV_TFLM_KWS_ERROR_INVOKE);
  }
  xv_kws_controller_reset_temporal(&g_state.controller);
  g_state.candidate_output_index = 0U;
  g_state.candidate_active = false;
  if (g_state.config.frontend.reset != nullptr) {
    g_state.config.frontend.reset(g_state.config.frontend.context);
  }
  return SetStatus(XV_TFLM_KWS_STATUS_OK);
}

extern "C" xv_tflm_kws_status_t xv_tflm_kws_process_features(
    const xv_tflm_kws_features_t *features, uint64_t now_ms,
    xv_tflm_kws_result_t *result) {
  EngineGuard guard;

  ClearResult(result);
  IncrementSaturated(&g_state.stats.feature_calls);
  if (!g_state.ready) {
    return SetStatus(XV_TFLM_KWS_ERROR_NOT_INITIALIZED);
  }
  if (features == nullptr || result == nullptr || features->data == nullptr ||
      features->element_count != g_state.config.input_elements) {
    IncrementSaturated(&g_state.stats.argument_errors);
    return SetStatus(XV_TFLM_KWS_ERROR_BAD_ARGUMENT);
  }
  if (!QuantScaleMatches(features->scale, g_state.input->params.scale) ||
      features->zero_point != g_state.input->params.zero_point) {
    IncrementSaturated(&g_state.stats.quantization_errors);
    return SetStatus(XV_TFLM_KWS_ERROR_QUANTIZATION);
  }

  std::memcpy(g_state.input->data.int8, features->data,
              features->element_count);
  return RunInference(now_ms, result);
}

extern "C" xv_tflm_kws_status_t xv_tflm_kws_process_pcm_gated(
    const int16_t *samples, size_t sample_count, uint32_t sample_rate_hz,
    uint64_t now_ms, bool inference_enabled,
    xv_tflm_kws_result_t *result) {
  RuntimeGuard guard;
  xv_tflm_kws_feature_buffer_t output;
  xv_tflm_kws_frontend_result_t frontend_result;

  ClearResult(result);
  IncrementSaturated(&g_state.stats.pcm_calls);
  if (!g_state.ready) {
    return SetStatus(XV_TFLM_KWS_ERROR_NOT_INITIALIZED);
  }
  if (samples == nullptr || sample_count == 0U || result == nullptr ||
      g_state.config.frontend.process == nullptr ||
      sample_rate_hz != g_state.config.frontend.sample_rate_hz) {
    IncrementSaturated(&g_state.stats.argument_errors);
    return SetStatus(XV_TFLM_KWS_ERROR_BAD_ARGUMENT);
  }

  output.data = g_state.input->data.int8;
  output.element_count = g_state.config.input_elements;
  output.scale = g_state.input->params.scale;
  output.zero_point = g_state.input->params.zero_point;
  frontend_result = g_state.config.frontend.process(
      g_state.config.frontend.context, samples, sample_count, sample_rate_hz,
      &output);

  if (frontend_result == XV_TFLM_KWS_FRONTEND_NEED_MORE_AUDIO) {
    IncrementSaturated(&g_state.stats.frontend_need_more);
    return SetStatus(XV_TFLM_KWS_STATUS_NEED_MORE_AUDIO);
  }
  if (frontend_result != XV_TFLM_KWS_FRONTEND_FEATURES_READY ||
      output.data != g_state.input->data.int8 ||
      output.element_count != g_state.config.input_elements ||
      !QuantScaleMatches(output.scale, g_state.input->params.scale) ||
      output.zero_point != g_state.input->params.zero_point) {
    IncrementSaturated(&g_state.stats.frontend_errors);
    return SetStatus(XV_TFLM_KWS_ERROR_FRONTEND);
  }
  if (!inference_enabled) {
    const xv_kws_decision_t decision =
        xv_kws_controller_process(&g_state.controller, 0U, now_ms);
    IncrementSaturated(&g_state.stats.inference_gated);
    g_state.candidate_output_index = 0U;
    g_state.candidate_active = false;
    result->decision = decision;
    if (decision == XV_KWS_DECISION_ERROR_BAD_ARGUMENT ||
        decision == XV_KWS_DECISION_ERROR_SCORE_RANGE ||
        decision == XV_KWS_DECISION_ERROR_CLOCK) {
      IncrementSaturated(&g_state.stats.decision_errors);
      return SetStatus(XV_TFLM_KWS_ERROR_DECISION);
    }
    return SetStatus(XV_TFLM_KWS_STATUS_GATED);
  }
  return RunInference(now_ms, result);
}

extern "C" xv_tflm_kws_status_t xv_tflm_kws_process_pcm(
    const int16_t *samples, size_t sample_count, uint32_t sample_rate_hz,
    uint64_t now_ms, xv_tflm_kws_result_t *result) {
  return xv_tflm_kws_process_pcm_gated(
      samples, sample_count, sample_rate_hz, now_ms, true, result);
}

extern "C" xv_tflm_kws_status_t xv_tflm_kws_advance_frontend(
    const int16_t *samples, size_t sample_count, uint32_t sample_rate_hz,
    uint64_t now_ms, bool inference_enabled, xv_tflm_kws_window_t *window) {
  FrontendGuard guard;
  xv_tflm_kws_feature_buffer_t output;
  xv_tflm_kws_frontend_result_t frontend_result;

  if (window == nullptr) {
    IncrementSaturated(&g_frontend_counters.argument_errors);
    return SetStatus(XV_TFLM_KWS_ERROR_BAD_ARGUMENT);
  }
  window->element_count = 0U;
  window->inference_enabled = inference_enabled;
  window->now_ms = now_ms;

  IncrementSaturated(&g_frontend_counters.pcm_calls);
  if (!g_state.ready) {
    return SetStatus(XV_TFLM_KWS_ERROR_NOT_INITIALIZED);
  }
  if (samples == nullptr || sample_count == 0U ||
      g_state.config.frontend.process == nullptr ||
      sample_rate_hz != g_state.config.frontend.sample_rate_hz ||
      g_state.config.input_elements > XV_KWS_FRONTEND_MAX_FEATURE_ELEMENTS) {
    IncrementSaturated(&g_frontend_counters.argument_errors);
    return SetStatus(XV_TFLM_KWS_ERROR_BAD_ARGUMENT);
  }

  /* The frontend writes straight into the queue entry, never into the
   * interpreter's input tensor: the consumer thread may be inside Invoke on the
   * previous window, and overwriting that tensor would corrupt it mid-inference.
   */
  output.data = window->data;
  output.element_count = g_state.config.input_elements;
  output.scale = g_state.input->params.scale;
  output.zero_point = g_state.input->params.zero_point;
  frontend_result = g_state.config.frontend.process(
      g_state.config.frontend.context, samples, sample_count, sample_rate_hz,
      &output);

  if (frontend_result == XV_TFLM_KWS_FRONTEND_NEED_MORE_AUDIO) {
    IncrementSaturated(&g_frontend_counters.frontend_need_more);
    return SetStatus(XV_TFLM_KWS_STATUS_NEED_MORE_AUDIO);
  }
  if (frontend_result != XV_TFLM_KWS_FRONTEND_FEATURES_READY ||
      output.data != window->data ||
      output.element_count != g_state.config.input_elements ||
      !QuantScaleMatches(output.scale, g_state.input->params.scale) ||
      output.zero_point != g_state.input->params.zero_point) {
    IncrementSaturated(&g_frontend_counters.frontend_errors);
    return SetStatus(XV_TFLM_KWS_ERROR_FRONTEND);
  }
  window->element_count = output.element_count;
  return SetStatus(XV_TFLM_KWS_STATUS_OK);
}

extern "C" xv_tflm_kws_status_t xv_tflm_kws_score_window(
    const xv_tflm_kws_window_t *window, bool contiguous,
    xv_tflm_kws_result_t *result) {
  EngineGuard guard;

  ClearResult(result);
  if (!g_state.ready) {
    return SetStatus(XV_TFLM_KWS_ERROR_NOT_INITIALIZED);
  }
  if (window == nullptr || result == nullptr ||
      window->element_count != g_state.config.input_elements) {
    IncrementSaturated(&g_state.stats.argument_errors);
    return SetStatus(XV_TFLM_KWS_ERROR_BAD_ARGUMENT);
  }

  /* A gap in the window sequence means the two windows either side of it are not
   * adjacent in time. Letting the temporal run survive that gap would let the
   * rule accept two seconds apart as "consecutive", which is a false-accept
   * path, so a discontinuity resets the run exactly like a closed VAD gate.
   */
  if (!contiguous) {
    xv_kws_controller_reset_temporal(&g_state.controller);
    g_state.candidate_output_index = 0U;
    g_state.candidate_active = false;
  }

  if (!window->inference_enabled) {
    const xv_kws_decision_t decision =
        xv_kws_controller_process(&g_state.controller, 0U, window->now_ms);
    IncrementSaturated(&g_state.stats.inference_gated);
    g_state.candidate_output_index = 0U;
    g_state.candidate_active = false;
    result->decision = decision;
    if (decision == XV_KWS_DECISION_ERROR_BAD_ARGUMENT ||
        decision == XV_KWS_DECISION_ERROR_SCORE_RANGE ||
        decision == XV_KWS_DECISION_ERROR_CLOCK) {
      IncrementSaturated(&g_state.stats.decision_errors);
      return SetStatus(XV_TFLM_KWS_ERROR_DECISION);
    }
    return SetStatus(XV_TFLM_KWS_STATUS_GATED);
  }

  std::memcpy(g_state.input->data.int8, window->data, window->element_count);
  return RunInference(window->now_ms, result);
}

extern "C" void xv_tflm_kws_get_stats(xv_tflm_kws_stats_t *stats) {
  if (stats == nullptr) {
    return;
  }
  {
    EngineGuard guard;
    *stats = g_state.stats;
    stats->controller = g_state.controller.stats;
  }
  {
    FrontendGuard guard;
    stats->argument_errors = SaturatingAdd(
        stats->argument_errors, g_frontend_counters.argument_errors);
    stats->pcm_calls =
        SaturatingAdd(stats->pcm_calls, g_frontend_counters.pcm_calls);
    stats->frontend_need_more = SaturatingAdd(
        stats->frontend_need_more, g_frontend_counters.frontend_need_more);
    stats->frontend_errors = SaturatingAdd(
        stats->frontend_errors, g_frontend_counters.frontend_errors);
  }
  stats->last_status = static_cast<xv_tflm_kws_status_t>(
      g_last_status.load(std::memory_order_relaxed));
}

extern "C" void xv_tflm_kws_get_feature_stats(
    xv_tflm_kws_feature_stats_t *stats) {
  EngineGuard guard;

  if (stats == nullptr) {
    return;
  }
#ifdef CONFIG_EXAMPLES_XIAOV_KWS_TRACE_SCORES
  *stats = g_feature_stats;
#else
  /* valid stays false so a caller cannot mistake an unpopulated snapshot for a
   * real all-zero feature window.
   */
  xv_tflm_kws_feature_stats_t empty = {};
  *stats = empty;
#endif
}

extern "C" void xv_tflm_kws_get_score_debug(
    xv_tflm_kws_score_debug_t *debug) {
  EngineGuard guard;

  if (debug == nullptr) {
    return;
  }
#ifdef CONFIG_EXAMPLES_XIAOV_KWS_TRACE_SCORES
  *debug = g_score_debug;
#else
  xv_tflm_kws_score_debug_t empty = {};
  *debug = empty;
#endif
}
