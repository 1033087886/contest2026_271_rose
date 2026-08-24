/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XIAOV_CONVERSATION_H
#define XIAOV_CONVERSATION_H

#include "xiaov/state_machine.h"
#include "xiaov/stream_markdown.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XV_CONVERSATION_TURN_ID_CAPACITY 64U
#define XV_CONVERSATION_ASR_CAPACITY 2048U

typedef enum {
  XV_CONVERSATION_TEXT_ASR_PARTIAL = 0,
  XV_CONVERSATION_TEXT_ASR_FINAL,
  XV_CONVERSATION_TEXT_ASSISTANT_DELTA,
  XV_CONVERSATION_TEXT_ASSISTANT_FINAL
} xv_conversation_text_kind_t;

typedef enum {
  XV_CONVERSATION_STYLE_TEXT = 0,
  XV_CONVERSATION_STYLE_BOLD,
  XV_CONVERSATION_STYLE_CODE,
  XV_CONVERSATION_STYLE_UNORDERED_MARKER,
  XV_CONVERSATION_STYLE_ORDERED_MARKER
} xv_conversation_style_t;

typedef enum {
  XV_CONVERSATION_OP_TURN_BEGIN = 0,
  XV_CONVERSATION_OP_ASR_TEXT,
  XV_CONVERSATION_OP_ASSISTANT_BEGIN,
  XV_CONVERSATION_OP_ASSISTANT_SPAN,
  XV_CONVERSATION_OP_ASSISTANT_FINAL,
  XV_CONVERSATION_OP_STATE
} xv_conversation_op_type_t;

typedef struct {
  xv_conversation_op_type_t type;
  xv_conversation_style_t style;
  xv_app_state_t state;
  const char *turn_id;
  const uint8_t *text;
  size_t text_length;
  uint32_t value;
  bool final;
} xv_conversation_op_t;

/* All pointers in an operation are valid only for the callback duration. */
typedef bool (*xv_conversation_render_fn)(
    void *context, const xv_conversation_op_t *operation);

typedef enum {
  XV_CONVERSATION_OK = 0,
  XV_CONVERSATION_BAD_ARGUMENT,
  XV_CONVERSATION_TURN_ID_TOO_LARGE,
  XV_CONVERSATION_TEXT_TOO_LARGE,
  XV_CONVERSATION_RENDER_ERROR,
  XV_CONVERSATION_MARKDOWN_ERROR
} xv_conversation_result_t;

/* Public so applications can place the complete model in static storage. */
typedef struct {
  xv_conversation_render_fn render;
  void *render_context;
  xv_stream_markdown_t markdown;
  char turn_id[XV_CONVERSATION_TURN_ID_CAPACITY];
  char asr_text[XV_CONVERSATION_ASR_CAPACITY];
  xv_app_state_t state;
  uint64_t completed_turns;
  uint64_t render_failures;
  bool turn_active;
  bool assistant_open;
  bool assistant_has_delta;
  bool assistant_finalized;
} xv_conversation_model_t;

bool xv_conversation_init(xv_conversation_model_t *model,
                          xv_conversation_render_fn render,
                          void *render_context);
void xv_conversation_reset(xv_conversation_model_t *model);

xv_conversation_result_t xv_conversation_set_state(
    xv_conversation_model_t *model, xv_app_state_t state);

xv_conversation_result_t xv_conversation_push_text(
    xv_conversation_model_t *model, xv_conversation_text_kind_t kind,
    const char *turn_id, const char *text);

#ifdef __cplusplus
}
#endif

#endif
