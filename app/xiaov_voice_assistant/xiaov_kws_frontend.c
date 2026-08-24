/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "xiaov_kws_frontend.h"

#include <limits.h>
#include <string.h>

#define XV_KWS_FRONTEND_LOG_OFFSET_Q3 192
#define XV_KWS_FRONTEND_GOERTZEL_Q 14
#define XV_KWS_FRONTEND_PCM_SHIFT 23

static const int16_t g_goertzel_coefficient_q14
    [XV_KWS_FRONTEND_FEATURE_BINS] = {
        32703, 32623, 32570, 32441, 32280, 31979, 31739, 31319,
        31001, 30467, 29865, 28959, 27939, 26809, 25573, 23887,
        22053, 20084, 17558, 14876, 11583, 8149,  4107,  0,
};

static int64_t trunc_div_power_of_two(int64_t value, unsigned int shift) {
  const int64_t divisor = (int64_t)1 << shift;
  return value / divisor;
}

static int32_t triangle_q15(size_t index) {
  if (index < XV_KWS_FRONTEND_FRAME_SAMPLES / 2U) {
    return (int32_t)((index * 32767U) /
                     (XV_KWS_FRONTEND_FRAME_SAMPLES / 2U));
  }
  return (int32_t)(((XV_KWS_FRONTEND_FRAME_SAMPLES - 1U - index) * 32767U) /
                   (XV_KWS_FRONTEND_FRAME_SAMPLES - 1U -
                    XV_KWS_FRONTEND_FRAME_SAMPLES / 2U));
}

static int8_t log_power_code(uint64_t power) {
  uint64_t base;
  unsigned int exponent = 0U;
  unsigned int fraction;
  int code;

  if (power == 0U) {
    return INT8_MIN;
  }
  {
    uint64_t scan = power;
    while (scan > 1U) {
      scan >>= 1U;
      exponent += 1U;
    }
  }
  base = (uint64_t)1U << exponent;
  if (exponent >= 3U) {
    fraction = (unsigned int)((power >> (exponent - 3U)) & 7U);
  } else {
    fraction = (unsigned int)(((power - base) << 3U) / base);
  }
  code = (int)(exponent * 8U + fraction) -
         XV_KWS_FRONTEND_LOG_OFFSET_Q3;
  if (code < INT8_MIN) {
    return INT8_MIN;
  }
  if (code > INT8_MAX) {
    return INT8_MAX;
  }
  return (int8_t)code;
}

bool xv_kws_frontend_extract_frame(const int16_t *samples, size_t sample_count,
                                   int8_t *features,
                                   size_t feature_count) {
  size_t bin;

  if (samples == NULL || features == NULL ||
      sample_count != XV_KWS_FRONTEND_FRAME_SAMPLES ||
      feature_count != XV_KWS_FRONTEND_FEATURE_BINS) {
    return false;
  }

  for (bin = 0U; bin < XV_KWS_FRONTEND_FEATURE_BINS; ++bin) {
    const int32_t coefficient = g_goertzel_coefficient_q14[bin];
    int32_t previous = 0;
    int32_t previous2 = 0;
    size_t index;

    for (index = 0U; index < XV_KWS_FRONTEND_FRAME_SAMPLES; ++index) {
      const int32_t windowed = (int32_t)trunc_div_power_of_two(
          (int64_t)samples[index] * triangle_q15(index),
          XV_KWS_FRONTEND_PCM_SHIFT);
      const int64_t recurrence = trunc_div_power_of_two(
          (int64_t)coefficient * previous, XV_KWS_FRONTEND_GOERTZEL_Q);
      const int64_t current = (int64_t)windowed + recurrence - previous2;

      if (current < INT32_MIN || current > INT32_MAX) {
        return false;
      }
      previous2 = previous;
      previous = (int32_t)current;
    }

    {
      const int64_t cross = trunc_div_power_of_two(
          (int64_t)coefficient * previous * previous2,
          XV_KWS_FRONTEND_GOERTZEL_Q);
      const int64_t signed_power =
          (int64_t)previous * previous + (int64_t)previous2 * previous2 -
          cross;
      features[bin] =
          log_power_code(signed_power > 0 ? (uint64_t)signed_power : 0U);
    }
  }
  return true;
}

void xv_kws_frontend_init(xv_kws_pcm_frontend_t *frontend) {
  xv_kws_frontend_reset(frontend);
}

void xv_kws_frontend_reset(void *context) {
  xv_kws_pcm_frontend_t *frontend = (xv_kws_pcm_frontend_t *)context;

  if (frontend == NULL) {
    return;
  }
  memset(frontend, 0, sizeof(*frontend));
  memset(frontend->features, INT8_MIN, sizeof(frontend->features));
  frontend->samples_until_frame = XV_KWS_FRONTEND_FRAME_SAMPLES;
}

void xv_kws_frontend_prefill_silence(void *context) {
  xv_kws_pcm_frontend_t *frontend = (xv_kws_pcm_frontend_t *)context;

  if (frontend == NULL) {
    return;
  }
  xv_kws_frontend_reset(frontend);
  frontend->feature_count = XV_KWS_FRONTEND_CONTEXT_FRAMES;
  frontend->has_emitted = true;
}

static bool append_feature_frame(xv_kws_pcm_frontend_t *frontend) {
  size_t index;

  for (index = 0U; index < XV_KWS_FRONTEND_FRAME_SAMPLES; ++index) {
    frontend->ordered_frame[index] = frontend->audio[
        (frontend->audio_write + index) % XV_KWS_FRONTEND_FRAME_SAMPLES];
  }
  if (!xv_kws_frontend_extract_frame(
          frontend->ordered_frame, XV_KWS_FRONTEND_FRAME_SAMPLES,
          frontend->features[frontend->feature_write],
          XV_KWS_FRONTEND_FEATURE_BINS)) {
    return false;
  }
  frontend->feature_write =
      (frontend->feature_write + 1U) % XV_KWS_FRONTEND_CONTEXT_FRAMES;
  if (frontend->feature_count < XV_KWS_FRONTEND_CONTEXT_FRAMES) {
    frontend->feature_count += 1U;
  }
  if (frontend->frames_since_emit < SIZE_MAX) {
    frontend->frames_since_emit += 1U;
  }
  return true;
}

static int8_t quantize_feature(int8_t feature, float scale,
                               int32_t zero_point) {
  const float scaled = (float)feature / scale;
  int32_t quantized = (int32_t)(scaled < 0.0F ? scaled - 0.5F : scaled + 0.5F) +
                     zero_point;

  if (quantized < INT8_MIN) {
    quantized = INT8_MIN;
  } else if (quantized > INT8_MAX) {
    quantized = INT8_MAX;
  }
  return (int8_t)quantized;
}

static void copy_ordered_features(const xv_kws_pcm_frontend_t *frontend,
                                  xv_tflm_kws_feature_buffer_t *output) {
  size_t row;
  size_t column;

  for (row = 0U; row < XV_KWS_FRONTEND_CONTEXT_FRAMES; ++row) {
    const size_t source =
        (frontend->feature_write + row) % XV_KWS_FRONTEND_CONTEXT_FRAMES;
    for (column = 0U; column < XV_KWS_FRONTEND_FEATURE_BINS; ++column) {
      output->data[row * XV_KWS_FRONTEND_FEATURE_BINS + column] =
          quantize_feature(frontend->features[source][column], output->scale,
                           output->zero_point);
    }
  }
}

xv_tflm_kws_frontend_result_t xv_kws_frontend_process(
    void *context, const int16_t *samples, size_t sample_count,
    uint32_t sample_rate_hz, xv_tflm_kws_feature_buffer_t *output) {
  xv_kws_pcm_frontend_t *frontend = (xv_kws_pcm_frontend_t *)context;
  size_t index;

  if (frontend == NULL || samples == NULL || sample_count == 0U ||
      sample_rate_hz != XV_KWS_FRONTEND_SAMPLE_RATE_HZ || output == NULL ||
      output->data == NULL ||
      output->element_count != XV_KWS_FRONTEND_FEATURE_ELEMENTS ||
      output->scale <= 0.0F || output->zero_point < INT8_MIN ||
      output->zero_point > INT8_MAX) {
    return XV_TFLM_KWS_FRONTEND_ERROR;
  }

  for (index = 0U; index < sample_count; ++index) {
    frontend->audio[frontend->audio_write] = samples[index];
    frontend->audio_write =
        (frontend->audio_write + 1U) % XV_KWS_FRONTEND_FRAME_SAMPLES;
    if (frontend->audio_count < XV_KWS_FRONTEND_FRAME_SAMPLES) {
      frontend->audio_count += 1U;
    }
    if (frontend->samples_until_frame == 0U) {
      return XV_TFLM_KWS_FRONTEND_ERROR;
    }
    frontend->samples_until_frame -= 1U;
    if (frontend->samples_until_frame == 0U) {
      if (frontend->audio_count != XV_KWS_FRONTEND_FRAME_SAMPLES ||
          !append_feature_frame(frontend)) {
        return XV_TFLM_KWS_FRONTEND_ERROR;
      }
      frontend->samples_until_frame = XV_KWS_FRONTEND_FRAME_STEP_SAMPLES;
    }
  }

  if (frontend->feature_count != XV_KWS_FRONTEND_CONTEXT_FRAMES ||
      (frontend->has_emitted &&
       frontend->frames_since_emit < XV_KWS_FRONTEND_EMIT_STRIDE_FRAMES)) {
    return XV_TFLM_KWS_FRONTEND_NEED_MORE_AUDIO;
  }
  copy_ordered_features(frontend, output);
  frontend->has_emitted = true;
  frontend->frames_since_emit = 0U;
  return XV_TFLM_KWS_FRONTEND_FEATURES_READY;
}
