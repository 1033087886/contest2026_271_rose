/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "xiaov/audio_rate_converter.h"

#include <limits.h>
#include <string.h>

/* 63-tap Kaiser-windowed low-pass, 7.5 kHz cutoff at 48 kHz, Q15. The
 * coefficients sum to 32768. Stop-band rejection starts above the 8 kHz
 * output Nyquist frequency. */
static const int16_t g_decimator_coefficients[XV_AUDIO_DECIMATOR_TAPS] = {
    -2,   -4,   -1,   8,    16,   9,    -18,  -42,  -31,  28,   90,
    81,   -28,  -162, -178, 0,    259,  344,  86,   -372, -611, -283,
    487,  1047, 700,  -589, -1880, -1747, 658, 4754, 8643, 10244,
    8643, 4754, 658,  -1747, -1880, -589, 700, 1047, 487,  -283,
    -611, -372, 86,   344,  259,  0,    -178, -162, -28,  81,   90,
    28,   -31,  -42,  -18,  9,    16,   8,    -1,   -4,   -2,
};

_Static_assert(sizeof(g_decimator_coefficients) /
                       sizeof(g_decimator_coefficients[0]) ==
                   XV_AUDIO_DECIMATOR_TAPS,
               "decimator coefficient count mismatch");

static int16_t clamp_i16(int64_t value) {
  if (value > INT16_MAX) {
    return INT16_MAX;
  }
  if (value < INT16_MIN) {
    return INT16_MIN;
  }
  return (int16_t)value;
}

void xv_audio_downsampler_reset(xv_audio_downsampler_t *state) {
  if (state != NULL) {
    memset(state, 0, sizeof(*state));
  }
}

static int16_t downsampler_output(const xv_audio_downsampler_t *state) {
  int64_t accumulator = 0;
  size_t history_index = state->write_index;
  size_t tap;

  for (tap = 0U; tap < XV_AUDIO_DECIMATOR_TAPS; ++tap) {
    history_index = history_index == 0U
                        ? XV_AUDIO_DECIMATOR_TAPS - 1U
                        : history_index - 1U;
    accumulator += (int64_t)state->history[history_index] *
                   (int64_t)g_decimator_coefficients[tap];
  }
  accumulator += accumulator >= 0 ? 16384 : -16384;
  return clamp_i16(accumulator / 32768);
}

size_t xv_audio_downsample_48k_stereo_to_16k_mono(
    xv_audio_downsampler_t *state, const int16_t *input,
    size_t input_frames, int16_t *output, size_t output_capacity) {
  size_t expected;
  size_t input_index;
  size_t produced = 0U;

  if (state == NULL || input == NULL || output == NULL ||
      state->write_index >= XV_AUDIO_DECIMATOR_TAPS ||
      state->phase >= XV_AUDIO_RATE_RATIO) {
    return 0U;
  }
  expected = ((size_t)state->phase + input_frames) / XV_AUDIO_RATE_RATIO;
  if (output_capacity < expected) {
    return 0U;
  }

  for (input_index = 0U; input_index < input_frames; ++input_index) {
    int32_t left = input[input_index * XV_AUDIO_NATIVE_CHANNELS];
    int32_t right = input[input_index * XV_AUDIO_NATIVE_CHANNELS + 1U];
    int32_t mono = (left + right) / 2;

    state->history[state->write_index] = (int16_t)mono;
    state->write_index =
        (state->write_index + 1U) % XV_AUDIO_DECIMATOR_TAPS;
    state->phase += 1U;
    if (state->phase == XV_AUDIO_RATE_RATIO) {
      state->phase = 0U;
      output[produced++] = downsampler_output(state);
    }
  }
  return produced;
}

void xv_audio_upsampler_reset(xv_audio_upsampler_t *state) {
  if (state != NULL) {
    memset(state, 0, sizeof(*state));
  }
}

static int16_t interpolate(int32_t from, int32_t to, int32_t to_weight) {
  int32_t value = from * (3 - to_weight) + to * to_weight;
  value += value >= 0 ? 1 : -1;
  return clamp_i16(value / 3);
}

size_t xv_audio_upsample_16k_mono_to_48k_stereo(
    xv_audio_upsampler_t *state, const int16_t *input,
    size_t input_samples, int16_t *output, size_t output_frame_capacity) {
  size_t input_index;
  size_t output_frame = 0U;

  if (state == NULL || input == NULL || output == NULL ||
      input_samples > SIZE_MAX / XV_AUDIO_RATE_RATIO ||
      output_frame_capacity < input_samples * XV_AUDIO_RATE_RATIO) {
    return 0U;
  }
  for (input_index = 0U; input_index < input_samples; ++input_index) {
    int16_t current = input[input_index];
    int16_t values[XV_AUDIO_RATE_RATIO];
    size_t phase;

    if (!state->has_previous) {
      state->previous = current;
      state->has_previous = true;
    }
    values[0] = state->previous;
    values[1] = interpolate(state->previous, current, 1);
    values[2] = interpolate(state->previous, current, 2);
    for (phase = 0U; phase < XV_AUDIO_RATE_RATIO; ++phase) {
      output[output_frame * XV_AUDIO_NATIVE_CHANNELS] = values[phase];
      output[output_frame * XV_AUDIO_NATIVE_CHANNELS + 1U] = values[phase];
      output_frame += 1U;
    }
    state->previous = current;
  }
  return output_frame;
}
