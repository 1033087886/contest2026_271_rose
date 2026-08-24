/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XIAOV_AUDIO_RATE_CONVERTER_H
#define XIAOV_AUDIO_RATE_CONVERTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XV_AUDIO_NATIVE_SAMPLE_RATE_HZ 48000U
#define XV_AUDIO_APPLICATION_SAMPLE_RATE_HZ 16000U
#define XV_AUDIO_NATIVE_CHANNELS 2U
#define XV_AUDIO_RATE_RATIO 3U
#define XV_AUDIO_DECIMATOR_TAPS 63U

typedef struct {
  int16_t history[XV_AUDIO_DECIMATOR_TAPS];
  size_t write_index;
  uint8_t phase;
} xv_audio_downsampler_t;

typedef struct {
  int16_t previous;
  bool has_previous;
} xv_audio_upsampler_t;

void xv_audio_downsampler_reset(xv_audio_downsampler_t *state);

/* Converts interleaved 48 kHz stereo to 16 kHz mono. The output count is
 * floor((pending_native_frames + input_frames) / 3). A fixed-point low-pass
 * filter runs before decimation so ultrasonic input does not alias into the
 * KWS/ASR band. Returns 0 without changing state for invalid arguments or an
 * undersized output buffer. */
size_t xv_audio_downsample_48k_stereo_to_16k_mono(
    xv_audio_downsampler_t *state, const int16_t *input,
    size_t input_frames, int16_t *output, size_t output_capacity);

void xv_audio_upsampler_reset(xv_audio_upsampler_t *state);

/* Converts 16 kHz mono to interleaved 48 kHz stereo. Each input sample emits
 * three native frames. Linear interpolation is continuous across calls and
 * both output channels are identical. */
size_t xv_audio_upsample_16k_mono_to_48k_stereo(
    xv_audio_upsampler_t *state, const int16_t *input,
    size_t input_samples, int16_t *output, size_t output_frame_capacity);

#ifdef __cplusplus
}
#endif

#endif
