/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "xiaov/music_player.h"

#include <string.h>

static bool bounded_copy(char *destination, size_t capacity,
                         const char *source) {
  size_t index;

  if (destination == NULL || source == NULL || capacity == 0U) {
    return false;
  }
  for (index = 0U; index < capacity; ++index) {
    destination[index] = source[index];
    if (source[index] == '\0') {
      return true;
    }
  }
  destination[0] = '\0';
  return false;
}

static char ascii_lower(char value) {
  if (value >= 'A' && value <= 'Z') {
    return (char)(value + ('a' - 'A'));
  }
  return value;
}

static bool starts_with_ci(const char *text, const char *prefix) {
  while (*prefix != '\0') {
    if (*text == '\0' || ascii_lower(*text) != ascii_lower(*prefix)) {
      return false;
    }
    ++text;
    ++prefix;
  }
  return true;
}

static size_t utf8_prefix_length(const char *text, size_t length,
                                 size_t maximum) {
  size_t prefix;

  if (length <= maximum) {
    return length;
  }
  prefix = maximum;
  while (prefix > 0U &&
         ((unsigned char)text[prefix] & 0xc0U) == 0x80U) {
    --prefix;
  }
  /* Returning maximum here would retain continuation bytes when the first
   * code point itself does not fit.  An empty title is preferable to invalid
   * UTF-8; derive_title replaces that with a stable fallback below. */
  return prefix;
}

static bool valid_reason(xv_music_interrupt_reason_t reason) {
  uint32_t value = (uint32_t)reason;
  uint32_t all = (uint32_t)XV_MUSIC_REASON_VOICE_TURN |
                 (uint32_t)XV_MUSIC_REASON_TTS |
                 (uint32_t)XV_MUSIC_REASON_ALERT |
                 (uint32_t)XV_MUSIC_REASON_PLATFORM_FOCUS;
  return value != 0U && (value & (value - 1U)) == 0U &&
         (value & ~all) == 0U;
}

static void recompute_output(xv_music_model_t *model) {
  if (!model->has_current || model->desired == XV_MUSIC_DESIRED_STOPPED ||
      model->stop_reasons != 0U) {
    model->output = XV_MUSIC_OUTPUT_STOPPED;
  } else if (model->desired == XV_MUSIC_DESIRED_PAUSED ||
             model->pause_reasons != 0U) {
    model->output = XV_MUSIC_OUTPUT_PAUSED;
  } else if (model->duck_reasons != 0U) {
    model->output = XV_MUSIC_OUTPUT_DUCKED;
  } else {
    model->output = XV_MUSIC_OUTPUT_PLAYING;
  }
}

static void derive_title(const char *source, char *title, size_t capacity) {
  const char *begin = source;
  const char *cursor;
  size_t length;

  for (cursor = source; *cursor != '\0' && *cursor != '?' && *cursor != '#';
       ++cursor) {
    if (*cursor == '/' || *cursor == '\\') {
      begin = cursor + 1;
    }
  }
  length = (size_t)(cursor - begin);
  if (length == 0U) {
    (void)bounded_copy(title, capacity,
                       starts_with_ci(source, "http://") ||
                               starts_with_ci(source, "https://")
                           ? "Network stream"
                           : "Local audio");
    return;
  }
  if (length >= capacity) {
    length = utf8_prefix_length(begin, length, capacity - 1U);
  }
  if (length == 0U) {
    (void)bounded_copy(title, capacity, "Audio track");
    return;
  }
  memcpy(title, begin, length);
  title[length] = '\0';
}

void xv_music_model_init(xv_music_model_t *model) {
  if (model == NULL) {
    return;
  }
  memset(model, 0, sizeof(*model));
  model->desired = XV_MUSIC_DESIRED_STOPPED;
  model->output = XV_MUSIC_OUTPUT_STOPPED;
  model->volume_percent = XV_MUSIC_DEFAULT_VOLUME_PERCENT;
}

bool xv_music_source_is_supported(const char *source) {
  size_t length;

  if (source == NULL || source[0] == '\0') {
    return false;
  }
  for (length = 0U; length < XV_MUSIC_SOURCE_CAPACITY; ++length) {
    unsigned char value = (unsigned char)source[length];
    if (value == '\0') {
      break;
    }
    if (value < 0x20U || value == 0x7fU) {
      return false;
    }
  }
  if (length == 0U || length == XV_MUSIC_SOURCE_CAPACITY) {
    return false;
  }
  return source[0] == '/' || starts_with_ci(source, "http://") ||
         starts_with_ci(source, "https://");
}

xv_music_model_result_t xv_music_model_play(xv_music_model_t *model,
                                             const char *source,
                                             size_t *selected_index,
                                             bool *track_added) {
  size_t index;

  if (model == NULL || source == NULL) {
    return XV_MUSIC_MODEL_BAD_ARGUMENT;
  }
  if (!xv_music_source_is_supported(source)) {
    return XV_MUSIC_MODEL_INVALID_SOURCE;
  }
  for (index = 0U; index < model->track_count; ++index) {
    if (strcmp(model->tracks[index].source, source) == 0) {
      break;
    }
  }
  if (index == model->track_count) {
    xv_music_track_t *track;
    if (model->track_count == XV_MUSIC_PLAYLIST_CAPACITY) {
      return XV_MUSIC_MODEL_PLAYLIST_FULL;
    }
    track = &model->tracks[model->track_count];
    if (!bounded_copy(track->source, sizeof(track->source), source)) {
      return XV_MUSIC_MODEL_INVALID_SOURCE;
    }
    derive_title(source, track->title, sizeof(track->title));
    index = model->track_count++;
    if (track_added != NULL) {
      *track_added = true;
    }
  } else if (track_added != NULL) {
    *track_added = false;
  }
  model->current_index = index;
  model->has_current = true;
  model->desired = XV_MUSIC_DESIRED_PLAYING;
  model->position_ms = 0U;
  model->duration_ms = 0U;
  recompute_output(model);
  if (selected_index != NULL) {
    *selected_index = index;
  }
  return XV_MUSIC_MODEL_OK;
}

xv_music_model_result_t xv_music_model_pause(xv_music_model_t *model) {
  if (model == NULL) {
    return XV_MUSIC_MODEL_BAD_ARGUMENT;
  }
  if (!model->has_current) {
    return XV_MUSIC_MODEL_NO_TRACK;
  }
  model->desired = XV_MUSIC_DESIRED_PAUSED;
  recompute_output(model);
  return XV_MUSIC_MODEL_OK;
}

xv_music_model_result_t xv_music_model_resume(xv_music_model_t *model) {
  if (model == NULL) {
    return XV_MUSIC_MODEL_BAD_ARGUMENT;
  }
  if (!model->has_current) {
    return XV_MUSIC_MODEL_NO_TRACK;
  }
  model->desired = XV_MUSIC_DESIRED_PLAYING;
  recompute_output(model);
  return XV_MUSIC_MODEL_OK;
}

xv_music_model_result_t xv_music_model_stop(xv_music_model_t *model) {
  if (model == NULL) {
    return XV_MUSIC_MODEL_BAD_ARGUMENT;
  }
  model->desired = XV_MUSIC_DESIRED_STOPPED;
  model->position_ms = 0U;
  recompute_output(model);
  return XV_MUSIC_MODEL_OK;
}

static xv_music_model_result_t move_track(xv_music_model_t *model,
                                          bool forward) {
  if (model == NULL) {
    return XV_MUSIC_MODEL_BAD_ARGUMENT;
  }
  if (model->track_count == 0U) {
    return XV_MUSIC_MODEL_PLAYLIST_EMPTY;
  }
  if (!model->has_current) {
    model->current_index = 0U;
  } else if (forward) {
    model->current_index = (model->current_index + 1U) % model->track_count;
  } else {
    model->current_index =
        model->current_index == 0U ? model->track_count - 1U
                                   : model->current_index - 1U;
  }
  model->has_current = true;
  model->desired = XV_MUSIC_DESIRED_PLAYING;
  model->position_ms = 0U;
  model->duration_ms = 0U;
  recompute_output(model);
  return XV_MUSIC_MODEL_OK;
}

xv_music_model_result_t xv_music_model_next(xv_music_model_t *model) {
  return move_track(model, true);
}

xv_music_model_result_t xv_music_model_previous(xv_music_model_t *model) {
  return move_track(model, false);
}

xv_music_model_result_t xv_music_model_set_volume(xv_music_model_t *model,
                                                   uint32_t volume_percent) {
  if (model == NULL) {
    return XV_MUSIC_MODEL_BAD_ARGUMENT;
  }
  if (volume_percent > 100U) {
    return XV_MUSIC_MODEL_OUT_OF_RANGE;
  }
  model->volume_percent = (uint8_t)volume_percent;
  return XV_MUSIC_MODEL_OK;
}

xv_music_model_result_t xv_music_model_seek(xv_music_model_t *model,
                                             uint32_t position_ms) {
  if (model == NULL) {
    return XV_MUSIC_MODEL_BAD_ARGUMENT;
  }
  if (!model->has_current) {
    return XV_MUSIC_MODEL_NO_TRACK;
  }
  if (model->duration_ms != 0U && position_ms > model->duration_ms) {
    return XV_MUSIC_MODEL_OUT_OF_RANGE;
  }
  model->position_ms = position_ms;
  return XV_MUSIC_MODEL_OK;
}

xv_music_model_result_t xv_music_model_set_interruption(
    xv_music_model_t *model, xv_music_interrupt_reason_t reason,
    xv_music_interrupt_policy_t policy, bool active) {
  uint32_t bit = (uint32_t)reason;

  if (model == NULL || !valid_reason(reason) ||
      policy > XV_MUSIC_INTERRUPT_STOP) {
    return XV_MUSIC_MODEL_BAD_ARGUMENT;
  }
  model->pause_reasons &= ~bit;
  model->duck_reasons &= ~bit;
  model->stop_reasons &= ~bit;
  if (active) {
    if (policy == XV_MUSIC_INTERRUPT_PAUSE) {
      model->pause_reasons |= bit;
    } else if (policy == XV_MUSIC_INTERRUPT_DUCK) {
      model->duck_reasons |= bit;
    } else {
      model->stop_reasons |= bit;
    }
  }
  recompute_output(model);
  return XV_MUSIC_MODEL_OK;
}

void xv_music_model_set_error(xv_music_model_t *model, bool error) {
  if (model == NULL) {
    return;
  }
  if (error) {
    model->output = XV_MUSIC_OUTPUT_ERROR;
  } else {
    recompute_output(model);
  }
}

void xv_music_model_update_progress(xv_music_model_t *model,
                                    uint32_t position_ms,
                                    uint32_t duration_ms) {
  if (model == NULL || !model->has_current) {
    return;
  }
  model->duration_ms = duration_ms;
  model->position_ms =
      duration_ms != 0U && position_ms > duration_ms ? duration_ms
                                                     : position_ms;
}

const xv_music_track_t *xv_music_model_current(
    const xv_music_model_t *model) {
  if (model == NULL || !model->has_current ||
      model->current_index >= model->track_count) {
    return NULL;
  }
  return &model->tracks[model->current_index];
}

void xv_music_model_snapshot(const xv_music_model_t *model,
                             xv_music_snapshot_t *snapshot) {
  const xv_music_track_t *track;

  if (model == NULL || snapshot == NULL) {
    return;
  }
  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->track_count = model->track_count;
  snapshot->current_index = model->current_index;
  snapshot->desired = model->desired;
  snapshot->output = model->output;
  snapshot->position_ms = model->position_ms;
  snapshot->duration_ms = model->duration_ms;
  snapshot->volume_percent = model->volume_percent;
  snapshot->has_current = model->has_current;
  track = xv_music_model_current(model);
  if (track != NULL) {
    (void)bounded_copy(snapshot->source, sizeof(snapshot->source),
                       track->source);
    (void)bounded_copy(snapshot->title, sizeof(snapshot->title), track->title);
  }
}

const char *xv_music_output_name(xv_music_output_state_t state) {
  switch (state) {
  case XV_MUSIC_OUTPUT_STOPPED:
    return "stopped";
  case XV_MUSIC_OUTPUT_PAUSED:
    return "paused";
  case XV_MUSIC_OUTPUT_PLAYING:
    return "playing";
  case XV_MUSIC_OUTPUT_DUCKED:
    return "ducked";
  case XV_MUSIC_OUTPUT_ERROR:
    return "error";
  }
  return "unknown";
}
