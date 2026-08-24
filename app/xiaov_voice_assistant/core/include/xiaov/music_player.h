/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XIAOV_MUSIC_PLAYER_H
#define XIAOV_MUSIC_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XV_MUSIC_PLAYLIST_CAPACITY 8U
#define XV_MUSIC_SOURCE_CAPACITY 1024U
#define XV_MUSIC_TITLE_CAPACITY 128U
#define XV_MUSIC_DEFAULT_VOLUME_PERCENT 60U

typedef enum {
  XV_MUSIC_DESIRED_STOPPED = 0,
  XV_MUSIC_DESIRED_PAUSED,
  XV_MUSIC_DESIRED_PLAYING
} xv_music_desired_state_t;

typedef enum {
  XV_MUSIC_OUTPUT_STOPPED = 0,
  XV_MUSIC_OUTPUT_PAUSED,
  XV_MUSIC_OUTPUT_PLAYING,
  XV_MUSIC_OUTPUT_DUCKED,
  XV_MUSIC_OUTPUT_ERROR
} xv_music_output_state_t;

typedef enum {
  XV_MUSIC_INTERRUPT_PAUSE = 0,
  XV_MUSIC_INTERRUPT_DUCK,
  XV_MUSIC_INTERRUPT_STOP
} xv_music_interrupt_policy_t;

typedef enum {
  XV_MUSIC_REASON_VOICE_TURN = 1U << 0,
  XV_MUSIC_REASON_TTS = 1U << 1,
  XV_MUSIC_REASON_ALERT = 1U << 2,
  XV_MUSIC_REASON_PLATFORM_FOCUS = 1U << 3
} xv_music_interrupt_reason_t;

typedef enum {
  XV_MUSIC_MODEL_OK = 0,
  XV_MUSIC_MODEL_BAD_ARGUMENT,
  XV_MUSIC_MODEL_INVALID_SOURCE,
  XV_MUSIC_MODEL_PLAYLIST_FULL,
  XV_MUSIC_MODEL_PLAYLIST_EMPTY,
  XV_MUSIC_MODEL_NO_TRACK,
  XV_MUSIC_MODEL_OUT_OF_RANGE
} xv_music_model_result_t;

typedef struct {
  char source[XV_MUSIC_SOURCE_CAPACITY];
  char title[XV_MUSIC_TITLE_CAPACITY];
} xv_music_track_t;

typedef struct {
  xv_music_track_t tracks[XV_MUSIC_PLAYLIST_CAPACITY];
  size_t track_count;
  size_t current_index;
  xv_music_desired_state_t desired;
  xv_music_output_state_t output;
  uint32_t pause_reasons;
  uint32_t duck_reasons;
  uint32_t stop_reasons;
  uint32_t position_ms;
  uint32_t duration_ms;
  uint8_t volume_percent;
  bool has_current;
} xv_music_model_t;

typedef struct {
  char source[XV_MUSIC_SOURCE_CAPACITY];
  char title[XV_MUSIC_TITLE_CAPACITY];
  size_t track_count;
  size_t current_index;
  xv_music_desired_state_t desired;
  xv_music_output_state_t output;
  uint32_t position_ms;
  uint32_t duration_ms;
  uint8_t volume_percent;
  bool has_current;
} xv_music_snapshot_t;

void xv_music_model_init(xv_music_model_t *model);

/* Sources are absolute local paths or HTTP(S) URLs. Relative paths and
 * opaque search terms are rejected because the device has no catalogue
 * resolver. */
bool xv_music_source_is_supported(const char *source);

/* Selects an existing source or appends it to the bounded playlist. A
 * successful call changes the desired state to PLAYING. */
xv_music_model_result_t xv_music_model_play(xv_music_model_t *model,
                                             const char *source,
                                             size_t *selected_index,
                                             bool *track_added);
xv_music_model_result_t xv_music_model_pause(xv_music_model_t *model);
xv_music_model_result_t xv_music_model_resume(xv_music_model_t *model);
xv_music_model_result_t xv_music_model_stop(xv_music_model_t *model);
xv_music_model_result_t xv_music_model_next(xv_music_model_t *model);
xv_music_model_result_t xv_music_model_previous(xv_music_model_t *model);
xv_music_model_result_t xv_music_model_set_volume(xv_music_model_t *model,
                                                   uint32_t volume_percent);
xv_music_model_result_t xv_music_model_seek(xv_music_model_t *model,
                                             uint32_t position_ms);

/* Each reason owns one policy. Reapplying a reason replaces its prior policy;
 * clearing it removes that reason from every policy. Desired user state is
 * retained so a playing track resumes only after the last blocker clears. */
xv_music_model_result_t xv_music_model_set_interruption(
    xv_music_model_t *model, xv_music_interrupt_reason_t reason,
    xv_music_interrupt_policy_t policy, bool active);

void xv_music_model_set_error(xv_music_model_t *model, bool error);
void xv_music_model_update_progress(xv_music_model_t *model,
                                    uint32_t position_ms,
                                    uint32_t duration_ms);
const xv_music_track_t *xv_music_model_current(
    const xv_music_model_t *model);
void xv_music_model_snapshot(const xv_music_model_t *model,
                             xv_music_snapshot_t *snapshot);
const char *xv_music_output_name(xv_music_output_state_t state);

#ifdef __cplusplus
}
#endif

#endif
