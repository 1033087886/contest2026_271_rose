/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XIAOV_OPENVELA_LOCAL_WAV_H
#define XIAOV_OPENVELA_LOCAL_WAV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XV_LOCAL_WAV_SAMPLE_RATE_HZ 16000U
#define XV_LOCAL_WAV_FRAME_SAMPLES 320U

typedef struct {
  FILE *file;
  long data_offset;
  uint32_t data_bytes;
  uint32_t consumed_bytes;
} xv_local_wav_t;

void xv_local_wav_reset(xv_local_wav_t *reader);

/* Opens a PCM16, mono, 16 kHz RIFF/WAVE file and validates its data extent. */
int xv_local_wav_open(xv_local_wav_t *reader, const char *path);
void xv_local_wav_close(xv_local_wav_t *reader);
bool xv_local_wav_is_open(const xv_local_wav_t *reader);

int xv_local_wav_rewind(xv_local_wav_t *reader);
int xv_local_wav_read(xv_local_wav_t *reader, int16_t *samples,
                      size_t capacity, size_t *samples_read, bool *eof);

uint32_t xv_local_wav_duration_ms(const xv_local_wav_t *reader);
uint32_t xv_local_wav_position_ms(const xv_local_wav_t *reader);

#ifdef __cplusplus
}
#endif

#endif
