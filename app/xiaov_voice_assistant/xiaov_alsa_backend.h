/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XIAOV_ALSA_BACKEND_H
#define XIAOV_ALSA_BACKEND_H

#include "xiaov/audio_rate_converter.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XV_ALSA_NATIVE_FRAME_SAMPLES 960U
#define XV_ALSA_NATIVE_STEREO_SAMPLES \
  (XV_ALSA_NATIVE_FRAME_SAMPLES * XV_AUDIO_NATIVE_CHANNELS)

typedef struct {
  void *handle;
  xv_audio_upsampler_t upsampler;
  int16_t native[XV_ALSA_NATIVE_STEREO_SAMPLES];
} xv_alsa_playback_backend_t;

typedef struct {
  void *capture_handle;
  void *playback_handle;
  xv_audio_downsampler_t downsampler;
  xv_audio_upsampler_t upsampler;
  int16_t capture_native[XV_ALSA_NATIVE_STEREO_SAMPLES];
  int16_t playback_native[XV_ALSA_NATIVE_STEREO_SAMPLES];
} xv_alsa_backend_t;

void xv_alsa_playback_backend_reset(xv_alsa_playback_backend_t *backend);
bool xv_alsa_playback_backend_is_open(
    const xv_alsa_playback_backend_t *backend);
int xv_alsa_playback_backend_open(xv_alsa_playback_backend_t *backend);
int xv_alsa_playback_backend_write(xv_alsa_playback_backend_t *backend,
                                   const int16_t *samples,
                                   size_t sample_count);
int xv_alsa_playback_backend_drain(xv_alsa_playback_backend_t *backend);
int xv_alsa_playback_backend_close(xv_alsa_playback_backend_t *backend);

void xv_alsa_backend_reset(xv_alsa_backend_t *backend);

bool xv_alsa_capture_is_open(const xv_alsa_backend_t *backend);
int xv_alsa_capture_open(xv_alsa_backend_t *backend);
int xv_alsa_capture_read_20ms(xv_alsa_backend_t *backend,
                              int16_t *output, size_t output_samples);
int xv_alsa_capture_close(xv_alsa_backend_t *backend);

bool xv_alsa_playback_is_open(const xv_alsa_backend_t *backend);
int xv_alsa_playback_open(xv_alsa_backend_t *backend);
int xv_alsa_playback_write(xv_alsa_backend_t *backend,
                           const int16_t *samples, size_t sample_count);
int xv_alsa_playback_drain(xv_alsa_backend_t *backend);
int xv_alsa_playback_close(xv_alsa_backend_t *backend);

#ifdef __cplusplus
}
#endif

#endif
