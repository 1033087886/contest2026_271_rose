/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "xiaov/conversation.h"

#include <string.h>

static bool bounded_length(const char *text, size_t capacity,
                           size_t *length) {
  size_t index;

  if (text == NULL || length == NULL || capacity == 0U) {
    return false;
  }
  for (index = 0U; index < capacity; ++index) {
    if (text[index] == '\0') {
      *length = index;
      return true;
    }
  }
  return false;
}

static bool render_operation(xv_conversation_model_t *model,
                             const xv_conversation_op_t *operation) {
  if (!model->render(model->render_context, operation)) {
    model->render_failures += 1U;
    return false;
  }
  return true;
}

static xv_conversation_style_t markdown_style(
    xv_markdown_span_type_t type) {
  switch (type) {
  case XV_MARKDOWN_SPAN_BOLD:
    return XV_CONVERSATION_STYLE_BOLD;
  case XV_MARKDOWN_SPAN_CODE:
    return XV_CONVERSATION_STYLE_CODE;
  case XV_MARKDOWN_SPAN_UNORDERED_MARKER:
    return XV_CONVERSATION_STYLE_UNORDERED_MARKER;
  case XV_MARKDOWN_SPAN_ORDERED_MARKER:
    return XV_CONVERSATION_STYLE_ORDERED_MARKER;
  case XV_MARKDOWN_SPAN_TEXT:
  default:
    return XV_CONVERSATION_STYLE_TEXT;
  }
}

static bool markdown_emit(void *context, const xv_markdown_span_t *span) {
  xv_conversation_model_t *model = context;
  xv_conversation_op_t operation;

  memset(&operation, 0, sizeof(operation));
  operation.type = XV_CONVERSATION_OP_ASSISTANT_SPAN;
  operation.style = markdown_style(span->type);
  operation.turn_id = model->turn_id;
  operation.text = span->data;
  operation.text_length = span->length;
  operation.value = span->value;
  return render_operation(model, &operation);
}

static xv_conversation_result_t begin_turn(xv_conversation_model_t *model,
                                            const char *turn_id,
                                            size_t turn_id_length) {
  xv_conversation_op_t operation;

  xv_stream_markdown_reset(&model->markdown);
  memcpy(model->turn_id, turn_id, turn_id_length + 1U);
  model->asr_text[0] = '\0';
  model->turn_active = true;
  model->assistant_open = false;
  model->assistant_has_delta = false;
  model->assistant_finalized = false;

  memset(&operation, 0, sizeof(operation));
  operation.type = XV_CONVERSATION_OP_TURN_BEGIN;
  operation.turn_id = model->turn_id;
  return render_operation(model, &operation) ? XV_CONVERSATION_OK
                                             : XV_CONVERSATION_RENDER_ERROR;
}

static xv_conversation_result_t ensure_turn(xv_conversation_model_t *model,
                                             const char *turn_id,
                                             size_t turn_id_length) {
  if (!model->turn_active || strcmp(model->turn_id, turn_id) != 0) {
    return begin_turn(model, turn_id, turn_id_length);
  }
  return XV_CONVERSATION_OK;
}

static xv_conversation_result_t emit_asr(xv_conversation_model_t *model,
                                          const char *text,
                                          size_t text_length, bool final) {
  xv_conversation_op_t operation;

  memcpy(model->asr_text, text, text_length + 1U);
  memset(&operation, 0, sizeof(operation));
  operation.type = XV_CONVERSATION_OP_ASR_TEXT;
  operation.turn_id = model->turn_id;
  operation.text = (const uint8_t *)model->asr_text;
  operation.text_length = text_length;
  operation.final = final;
  return render_operation(model, &operation) ? XV_CONVERSATION_OK
                                             : XV_CONVERSATION_RENDER_ERROR;
}

static xv_conversation_result_t ensure_assistant(
    xv_conversation_model_t *model) {
  xv_conversation_op_t operation;

  if (model->assistant_open) {
    return XV_CONVERSATION_OK;
  }
  memset(&operation, 0, sizeof(operation));
  operation.type = XV_CONVERSATION_OP_ASSISTANT_BEGIN;
  operation.turn_id = model->turn_id;
  if (!render_operation(model, &operation)) {
    return XV_CONVERSATION_RENDER_ERROR;
  }
  model->assistant_open = true;
  return XV_CONVERSATION_OK;
}

static xv_conversation_result_t feed_assistant(
    xv_conversation_model_t *model, const char *text, size_t text_length) {
  xv_stream_markdown_result_t result = xv_stream_markdown_feed(
      &model->markdown, (const uint8_t *)text, text_length);

  if (result == XV_STREAM_MARKDOWN_CALLBACK_ERROR) {
    return XV_CONVERSATION_RENDER_ERROR;
  }
  return result == XV_STREAM_MARKDOWN_OK ? XV_CONVERSATION_OK
                                         : XV_CONVERSATION_MARKDOWN_ERROR;
}

static xv_conversation_result_t finish_assistant(
    xv_conversation_model_t *model) {
  xv_stream_markdown_result_t result;
  xv_conversation_op_t operation;

  result = xv_stream_markdown_finish(&model->markdown);
  if (result == XV_STREAM_MARKDOWN_CALLBACK_ERROR) {
    return XV_CONVERSATION_RENDER_ERROR;
  }
  if (result != XV_STREAM_MARKDOWN_OK &&
      result != XV_STREAM_MARKDOWN_FINISHED) {
    return XV_CONVERSATION_MARKDOWN_ERROR;
  }
  memset(&operation, 0, sizeof(operation));
  operation.type = XV_CONVERSATION_OP_ASSISTANT_FINAL;
  operation.turn_id = model->turn_id;
  operation.final = true;
  if (!render_operation(model, &operation)) {
    return XV_CONVERSATION_RENDER_ERROR;
  }
  model->assistant_open = false;
  model->assistant_finalized = true;
  model->completed_turns += 1U;
  return XV_CONVERSATION_OK;
}

bool xv_conversation_init(xv_conversation_model_t *model,
                          xv_conversation_render_fn render,
                          void *render_context) {
  if (model == NULL || render == NULL) {
    return false;
  }
  memset(model, 0, sizeof(*model));
  model->render = render;
  model->render_context = render_context;
  model->state = XV_STATE_STANDBY;
  return xv_stream_markdown_init(&model->markdown, markdown_emit, model, NULL);
}

void xv_conversation_reset(xv_conversation_model_t *model) {
  xv_conversation_render_fn render;
  void *render_context;

  if (model == NULL) {
    return;
  }
  render = model->render;
  render_context = model->render_context;
  memset(model, 0, sizeof(*model));
  model->render = render;
  model->render_context = render_context;
  model->state = XV_STATE_STANDBY;
  (void)xv_stream_markdown_init(&model->markdown, markdown_emit, model, NULL);
}

xv_conversation_result_t xv_conversation_set_state(
    xv_conversation_model_t *model, xv_app_state_t state) {
  xv_conversation_op_t operation;

  if (model == NULL || state > XV_STATE_ERROR) {
    return XV_CONVERSATION_BAD_ARGUMENT;
  }
  model->state = state;
  memset(&operation, 0, sizeof(operation));
  operation.type = XV_CONVERSATION_OP_STATE;
  operation.state = state;
  return render_operation(model, &operation) ? XV_CONVERSATION_OK
                                             : XV_CONVERSATION_RENDER_ERROR;
}

xv_conversation_result_t xv_conversation_push_text(
    xv_conversation_model_t *model, xv_conversation_text_kind_t kind,
    const char *turn_id, const char *text) {
  xv_conversation_result_t result;
  size_t turn_id_length;
  size_t text_length;

  if (model == NULL || turn_id == NULL || text == NULL ||
      kind > XV_CONVERSATION_TEXT_ASSISTANT_FINAL) {
    return XV_CONVERSATION_BAD_ARGUMENT;
  }
  if (!bounded_length(turn_id, XV_CONVERSATION_TURN_ID_CAPACITY,
                      &turn_id_length)) {
    return XV_CONVERSATION_TURN_ID_TOO_LARGE;
  }
  if (turn_id_length == 0U) {
    return XV_CONVERSATION_BAD_ARGUMENT;
  }
  if (!bounded_length(text, XV_CONVERSATION_ASR_CAPACITY, &text_length)) {
    return text == NULL ? XV_CONVERSATION_BAD_ARGUMENT
                        : XV_CONVERSATION_TEXT_TOO_LARGE;
  }
  result = ensure_turn(model, turn_id, turn_id_length);
  if (result != XV_CONVERSATION_OK) {
    return result;
  }

  if (kind == XV_CONVERSATION_TEXT_ASR_PARTIAL ||
      kind == XV_CONVERSATION_TEXT_ASR_FINAL) {
    return emit_asr(model, text, text_length,
                    kind == XV_CONVERSATION_TEXT_ASR_FINAL);
  }

  if (model->assistant_finalized) {
    return kind == XV_CONVERSATION_TEXT_ASSISTANT_FINAL
               ? XV_CONVERSATION_OK
               : XV_CONVERSATION_MARKDOWN_ERROR;
  }

  result = ensure_assistant(model);
  if (result != XV_CONVERSATION_OK) {
    return result;
  }
  if (kind == XV_CONVERSATION_TEXT_ASSISTANT_DELTA) {
    model->assistant_has_delta = true;
    return feed_assistant(model, text, text_length);
  }

  /* assistant.final carries the complete answer. It is a fallback when no
   * delta arrived, but must not duplicate an already streamed answer. */
  if (!model->assistant_has_delta && text_length > 0U) {
    result = feed_assistant(model, text, text_length);
    if (result != XV_CONVERSATION_OK) {
      return result;
    }
  }
  return finish_assistant(model);
}
