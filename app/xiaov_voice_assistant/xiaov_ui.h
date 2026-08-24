/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XIAOV_OPENVELA_UI_H
#define XIAOV_OPENVELA_UI_H

#include "xiaov/conversation.h"
#include "xiaov/music_player.h"
#include "xiaov_music_adapter.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XV_UI_STORAGE_SIZE 131072U

typedef void (*xv_ui_action_fn)(void *context, xv_app_event_t event);
typedef void (*xv_ui_offline_command_fn)(void *context);
typedef void (*xv_ui_media_action_fn)(void *context, xv_music_action_t action,
                                      uint32_t value);
typedef void (*xv_ui_display_action_fn)(void *context, const char *action);

typedef struct {
  void *context;
  xv_ui_action_fn request_action;
  xv_ui_offline_command_fn request_offline_command;
  xv_ui_media_action_fn request_media;
  xv_ui_display_action_fn request_display;
} xv_ui_callbacks_t;

typedef struct xv_ui xv_ui_t;

typedef union {
  uint64_t alignment;
  uint8_t bytes[XV_UI_STORAGE_SIZE];
} xv_ui_storage_t;

typedef struct {
  uint64_t queue_rejected;
  uint64_t render_rejected;
  size_t queue_depth;
  bool graphical;
  bool interactive;
  uint64_t video_frames;
  uint64_t video_replaced;
} xv_ui_stats_t;

xv_ui_t *xv_ui_init(xv_ui_storage_t *storage,
                     const xv_ui_callbacks_t *callbacks);
bool xv_ui_start(xv_ui_t *ui);
void xv_ui_stop(xv_ui_t *ui);
void xv_ui_deinit(xv_ui_t *ui);

bool xv_ui_post_state(xv_ui_t *ui, xv_app_state_t state);
bool xv_ui_post_text(xv_ui_t *ui, xv_conversation_text_kind_t kind,
                     const char *turn_id, const char *text);
bool xv_ui_post_reminder(xv_ui_t *ui, const char *label, bool active);
bool xv_ui_post_media_command(xv_ui_t *ui, const char *action,
                              const char *query);
bool xv_ui_post_music_state(xv_ui_t *ui,
                            const xv_music_snapshot_t *snapshot);
bool xv_ui_post_low_power(xv_ui_t *ui, bool low_power);
bool xv_ui_post_display_state(xv_ui_t *ui, const char *mode,
                              const char *status);
bool xv_ui_update_video_frame(xv_ui_t *ui, const uint8_t *rgb565,
                              size_t byte_count, uint32_t sequence,
                              uint32_t timestamp_ms);

/* This is intentionally not queued: audio producers only replace the latest
 * level snapshot and the LVGL worker consumes it from its animation timer. */
bool xv_ui_update_audio_levels(xv_ui_t *ui, const uint8_t levels[8],
                               uint8_t mouth_level);
bool xv_ui_post_error(xv_ui_t *ui, const char *message);

bool xv_ui_is_graphical(xv_ui_t *ui);
bool xv_ui_is_interactive(xv_ui_t *ui);
void xv_ui_get_stats(xv_ui_t *ui, xv_ui_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
