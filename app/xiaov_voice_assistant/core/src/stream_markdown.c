#include "xiaov/stream_markdown.h"

#include <string.h>

static bool flush_output(xv_stream_markdown_t *parser) {
  xv_markdown_span_t span;

  if (parser->output_length == 0U) {
    return true;
  }
  span.type = parser->output_type;
  span.data = parser->output;
  span.length = parser->output_length;
  span.value = parser->output_value;
  parser->output_length = 0U;
  if (!parser->emit(parser->emit_context, &span)) {
    parser->error = XV_STREAM_MARKDOWN_CALLBACK_ERROR;
    return false;
  }
  return true;
}

static bool emit_bytes(xv_stream_markdown_t *parser,
                       xv_markdown_span_type_t type, uint32_t value,
                       const uint8_t *data, size_t length) {
  size_t copied = 0U;

  while (copied < length) {
    size_t available;
    size_t count;

    if (parser->output_length > 0U &&
        (parser->output_type != type || parser->output_value != value)) {
      if (!flush_output(parser)) {
        return false;
      }
    }
    if (parser->output_length == 0U) {
      parser->output_type = type;
      parser->output_value = value;
    }
    available = XV_STREAM_MARKDOWN_OUTPUT_CAPACITY - parser->output_length;
    count = length - copied;
    if (count > available) {
      count = available;
    }
    /* Every callback payload is independently valid UTF-8. If the buffer edge
     * lands inside a code point, flush before that code point instead. */
    if (count < length - copied) {
      while (count > 0U && (data[copied + count] & 0xc0U) == 0x80U) {
        count -= 1U;
      }
      if (count == 0U) {
        if (!flush_output(parser)) {
          return false;
        }
        continue;
      }
    }
    memcpy(parser->output + parser->output_length, data + copied, count);
    parser->output_length += count;
    copied += count;
    if (parser->output_length == XV_STREAM_MARKDOWN_OUTPUT_CAPACITY &&
        !flush_output(parser)) {
      return false;
    }
  }
  return true;
}

static bool emit_repeated(xv_stream_markdown_t *parser,
                          xv_markdown_span_type_t type, uint8_t byte,
                          size_t count) {
  uint8_t bytes[XV_STREAM_MARKDOWN_MAX_FENCE_TICKS];

  if (count > sizeof(bytes)) {
    return false;
  }
  memset(bytes, (int)byte, count);
  return emit_bytes(parser, type, 0U, bytes, count);
}

static void clear_prefix(xv_stream_markdown_t *parser) {
  parser->prefix_length = 0U;
  parser->prefix_state = XV_MARKDOWN_PREFIX_NONE;
}

static bool emit_bold_literal(xv_stream_markdown_t *parser,
                              const uint8_t *extra, size_t extra_length,
                              bool continue_until_delimiter) {
  static const uint8_t opener[] = {'*', '*'};

  if (!emit_bytes(parser, XV_MARKDOWN_SPAN_TEXT, 0U, opener,
                  sizeof(opener)) ||
      !emit_bytes(parser, XV_MARKDOWN_SPAN_TEXT, 0U, parser->bold,
                  parser->bold_length) ||
      (extra_length > 0U &&
       !emit_bytes(parser, XV_MARKDOWN_SPAN_TEXT, 0U, extra,
                   extra_length))) {
    return false;
  }
  parser->bold_active = false;
  parser->bold_star_pending = false;
  parser->bold_length = 0U;
  parser->bold_literal_active = continue_until_delimiter;
  parser->bold_literal_star_seen = false;
  return true;
}

static bool append_bold(xv_stream_markdown_t *parser, const uint8_t *data,
                        size_t length) {
  if (length <= (size_t)parser->limits.max_bold_bytes - parser->bold_length) {
    memcpy(parser->bold + parser->bold_length, data, length);
    parser->bold_length += length;
    return true;
  }
  return emit_bold_literal(parser, data, length, true);
}

static bool process_inline_atom(xv_stream_markdown_t *parser,
                                const uint8_t *data, size_t length) {
  static const uint8_t star = '*';
  bool is_star = length == 1U && data[0] == '*';

  if (parser->bold_literal_active) {
    if (!emit_bytes(parser, XV_MARKDOWN_SPAN_TEXT, 0U, data, length)) {
      return false;
    }
    if (is_star) {
      if (parser->bold_literal_star_seen) {
        parser->bold_literal_active = false;
        parser->bold_literal_star_seen = false;
      } else {
        parser->bold_literal_star_seen = true;
      }
    } else {
      parser->bold_literal_star_seen = false;
    }
    return true;
  }

  if (parser->bold_active) {
    if (is_star) {
      if (parser->bold_star_pending) {
        parser->bold_star_pending = false;
        if (parser->bold_length == 0U) {
          static const uint8_t empty_bold[] = {'*', '*', '*', '*'};
          if (!emit_bytes(parser, XV_MARKDOWN_SPAN_TEXT, 0U, empty_bold,
                          sizeof(empty_bold))) {
            return false;
          }
        } else {
          if (!emit_bytes(parser, XV_MARKDOWN_SPAN_BOLD, 0U, parser->bold,
                          parser->bold_length)) {
            return false;
          }
        }
        parser->bold_active = false;
        parser->bold_length = 0U;
      } else {
        parser->bold_star_pending = true;
      }
      return true;
    }
    if (parser->bold_star_pending) {
      parser->bold_star_pending = false;
      if (parser->bold_length + 1U + length >
          (size_t)parser->limits.max_bold_bytes) {
        uint8_t extra[5];
        extra[0] = star;
        memcpy(extra + 1U, data, length);
        return emit_bold_literal(parser, extra, length + 1U, true);
      }
      parser->bold[parser->bold_length++] = star;
    }
    return append_bold(parser, data, length);
  }

  if (is_star) {
    if (parser->text_star_pending) {
      parser->text_star_pending = false;
      parser->bold_active = true;
      parser->bold_length = 0U;
    } else {
      parser->text_star_pending = true;
    }
    return true;
  }
  if (parser->text_star_pending) {
    parser->text_star_pending = false;
    if (!emit_bytes(parser, XV_MARKDOWN_SPAN_TEXT, 0U, &star, 1U)) {
      return false;
    }
  }
  return emit_bytes(parser, XV_MARKDOWN_SPAN_TEXT, 0U, data, length);
}

static bool replay_prefix(xv_stream_markdown_t *parser) {
  size_t index;
  size_t length = parser->prefix_length;
  uint8_t bytes[XV_STREAM_MARKDOWN_MAX_ORDERED_DIGITS + 2U];

  memcpy(bytes, parser->prefix, length);
  clear_prefix(parser);
  parser->at_line_start = false;
  for (index = 0U; index < length; ++index) {
    if (!process_inline_atom(parser, bytes + index, 1U)) {
      return false;
    }
  }
  return true;
}

static uint32_t ordered_value(const xv_stream_markdown_t *parser) {
  size_t index;
  uint32_t value = 0U;

  for (index = 0U; index + 1U < parser->prefix_length; ++index) {
    value = value * 10U + (uint32_t)(parser->prefix[index] - (uint8_t)'0');
  }
  return value;
}

static bool process_prefix_atom(xv_stream_markdown_t *parser,
                                const uint8_t *data, size_t length) {
  uint8_t byte;

  if (length != 1U) {
    if (!replay_prefix(parser)) {
      return false;
    }
    return process_inline_atom(parser, data, length);
  }
  byte = data[0];
  switch (parser->prefix_state) {
  case XV_MARKDOWN_PREFIX_UNORDERED:
    if (byte == ' ' || byte == '\t') {
      uint8_t marker[2] = {parser->prefix[0], byte};
      clear_prefix(parser);
      parser->at_line_start = false;
      return emit_bytes(parser, XV_MARKDOWN_SPAN_UNORDERED_MARKER, 0U,
                        marker, sizeof(marker));
    }
    break;
  case XV_MARKDOWN_PREFIX_ORDERED_DIGITS:
    if (byte >= '0' && byte <= '9' &&
        parser->prefix_length < XV_STREAM_MARKDOWN_MAX_ORDERED_DIGITS) {
      parser->prefix[parser->prefix_length++] = byte;
      return true;
    }
    if (byte == '.') {
      parser->prefix[parser->prefix_length++] = byte;
      parser->prefix_state = XV_MARKDOWN_PREFIX_ORDERED_DOT;
      return true;
    }
    break;
  case XV_MARKDOWN_PREFIX_ORDERED_DOT:
    if (byte == ' ' || byte == '\t') {
      uint8_t marker[XV_STREAM_MARKDOWN_MAX_ORDERED_DIGITS + 2U];
      size_t marker_length = parser->prefix_length + 1U;
      uint32_t value = ordered_value(parser);
      memcpy(marker, parser->prefix, parser->prefix_length);
      marker[parser->prefix_length] = byte;
      clear_prefix(parser);
      parser->at_line_start = false;
      return emit_bytes(parser, XV_MARKDOWN_SPAN_ORDERED_MARKER, value,
                        marker, marker_length);
    }
    break;
  case XV_MARKDOWN_PREFIX_FENCE:
    if (byte == '`') {
      parser->prefix[parser->prefix_length++] = byte;
      if (parser->prefix_length == 3U) {
        clear_prefix(parser);
        parser->opening_fence_line = true;
        parser->at_line_start = false;
      }
      return true;
    }
    break;
  case XV_MARKDOWN_PREFIX_NONE:
  default:
    break;
  }
  if (!replay_prefix(parser)) {
    return false;
  }
  return process_inline_atom(parser, data, length);
}

static bool flush_code_fence_candidate(xv_stream_markdown_t *parser) {
  if (parser->fence_ticks > 0U &&
      !emit_repeated(parser, XV_MARKDOWN_SPAN_CODE, '`',
                     parser->fence_ticks)) {
    return false;
  }
  if (parser->fence_trailing_length > 0U &&
      !emit_bytes(parser, XV_MARKDOWN_SPAN_CODE, 0U,
                  parser->fence_trailing, parser->fence_trailing_length)) {
    return false;
  }
  parser->fence_ticks = 0U;
  parser->fence_trailing_length = 0U;
  return true;
}

static bool process_code_atom(xv_stream_markdown_t *parser,
                              const uint8_t *data, size_t length) {
  uint8_t byte = length == 1U ? data[0] : 0U;

  if (!parser->at_line_start) {
    return emit_bytes(parser, XV_MARKDOWN_SPAN_CODE, 0U, data, length);
  }
  if (parser->fence_ticks < 3U) {
    if (length == 1U && byte == '`') {
      parser->fence_ticks += 1U;
      return true;
    }
    if (!flush_code_fence_candidate(parser)) {
      return false;
    }
    parser->at_line_start = false;
    return emit_bytes(parser, XV_MARKDOWN_SPAN_CODE, 0U, data, length);
  }

  if (parser->fence_trailing_length == 0U && length == 1U && byte == '`') {
    if (parser->fence_ticks < XV_STREAM_MARKDOWN_MAX_FENCE_TICKS) {
      parser->fence_ticks += 1U;
      return true;
    }
  } else if (length == 1U && (byte == ' ' || byte == '\t') &&
             parser->fence_trailing_length <
                 XV_STREAM_MARKDOWN_MAX_FENCE_TRAILING_BYTES) {
    parser->fence_trailing[parser->fence_trailing_length++] = byte;
    return true;
  }

  if (!flush_code_fence_candidate(parser)) {
    return false;
  }
  parser->at_line_start = false;
  return emit_bytes(parser, XV_MARKDOWN_SPAN_CODE, 0U, data, length);
}

static bool process_regular_atom(xv_stream_markdown_t *parser,
                                 const uint8_t *data, size_t length) {
  uint8_t byte = length == 1U ? data[0] : 0U;

  if (parser->opening_fence_line) {
    return true;
  }
  if (parser->in_code) {
    return process_code_atom(parser, data, length);
  }
  if (parser->bold_active || parser->bold_literal_active) {
    parser->at_line_start = false;
    return process_inline_atom(parser, data, length);
  }
  if (parser->prefix_state != XV_MARKDOWN_PREFIX_NONE) {
    return process_prefix_atom(parser, data, length);
  }
  if (parser->at_line_start && length == 1U) {
    if (byte == '-' || byte == '+' || byte == '*') {
      parser->prefix[0] = byte;
      parser->prefix_length = 1U;
      parser->prefix_state = XV_MARKDOWN_PREFIX_UNORDERED;
      return true;
    }
    if (byte >= '0' && byte <= '9') {
      parser->prefix[0] = byte;
      parser->prefix_length = 1U;
      parser->prefix_state = XV_MARKDOWN_PREFIX_ORDERED_DIGITS;
      return true;
    }
    if (byte == '`') {
      parser->prefix[0] = byte;
      parser->prefix_length = 1U;
      parser->prefix_state = XV_MARKDOWN_PREFIX_FENCE;
      return true;
    }
  }
  parser->at_line_start = false;
  return process_inline_atom(parser, data, length);
}

static bool process_newline(xv_stream_markdown_t *parser,
                            const uint8_t *data, size_t length) {
  if (parser->opening_fence_line) {
    parser->opening_fence_line = false;
    parser->in_code = true;
    parser->at_line_start = true;
    return true;
  }
  if (parser->in_code) {
    if (parser->at_line_start && parser->fence_ticks >= 3U) {
      parser->fence_ticks = 0U;
      parser->fence_trailing_length = 0U;
      parser->in_code = false;
      parser->at_line_start = true;
      return true;
    }
    if (parser->at_line_start && !flush_code_fence_candidate(parser)) {
      return false;
    }
    if (!emit_bytes(parser, XV_MARKDOWN_SPAN_CODE, 0U, data, length)) {
      return false;
    }
    parser->at_line_start = true;
    return true;
  }
  if (parser->prefix_state != XV_MARKDOWN_PREFIX_NONE &&
      !replay_prefix(parser)) {
    return false;
  }
  if (!process_inline_atom(parser, data, length)) {
    return false;
  }
  parser->at_line_start = true;
  return true;
}

static bool process_atom(xv_stream_markdown_t *parser, const uint8_t *data,
                         size_t length) {
  static const uint8_t cr = '\r';

  if (length == 1U && data[0] == '\r') {
    if (parser->cr_pending && !process_newline(parser, &cr, 1U)) {
      return false;
    }
    parser->cr_pending = true;
    return true;
  }
  if (length == 1U && data[0] == '\n') {
    static const uint8_t crlf[] = {'\r', '\n'};
    if (parser->cr_pending) {
      parser->cr_pending = false;
      return process_newline(parser, crlf, sizeof(crlf));
    }
    return process_newline(parser, data, length);
  }
  if (parser->cr_pending) {
    parser->cr_pending = false;
    if (!process_newline(parser, &cr, 1U)) {
      return false;
    }
  }
  return process_regular_atom(parser, data, length);
}

static bool consume_utf8_byte(xv_stream_markdown_t *parser, uint8_t byte) {
  if (parser->utf8_expected == 0U) {
    if (byte <= 0x7fU) {
      return process_atom(parser, &byte, 1U);
    }
    parser->utf8[0] = byte;
    parser->utf8_length = 1U;
    parser->utf8_second_min = 0x80U;
    parser->utf8_second_max = 0xbfU;
    if (byte >= 0xc2U && byte <= 0xdfU) {
      parser->utf8_expected = 2U;
    } else if (byte >= 0xe0U && byte <= 0xefU) {
      parser->utf8_expected = 3U;
      if (byte == 0xe0U) {
        parser->utf8_second_min = 0xa0U;
      } else if (byte == 0xedU) {
        parser->utf8_second_max = 0x9fU;
      }
    } else if (byte >= 0xf0U && byte <= 0xf4U) {
      parser->utf8_expected = 4U;
      if (byte == 0xf0U) {
        parser->utf8_second_min = 0x90U;
      } else if (byte == 0xf4U) {
        parser->utf8_second_max = 0x8fU;
      }
    } else {
      return false;
    }
    return true;
  }

  if (byte < 0x80U || byte > 0xbfU ||
      (parser->utf8_length == 1U &&
       (byte < parser->utf8_second_min || byte > parser->utf8_second_max))) {
    return false;
  }
  parser->utf8[parser->utf8_length++] = byte;
  if (parser->utf8_length == parser->utf8_expected) {
    size_t length = parser->utf8_length;
    parser->utf8_length = 0U;
    parser->utf8_expected = 0U;
    return process_atom(parser, parser->utf8, length);
  }
  return true;
}

void xv_stream_markdown_limits_default(xv_stream_markdown_limits_t *limits) {
  if (limits == NULL) {
    return;
  }
  limits->max_chunk_bytes = XV_STREAM_MARKDOWN_MAX_CHUNK_BYTES;
  limits->max_document_bytes = XV_STREAM_MARKDOWN_DEFAULT_DOCUMENT_BYTES;
  limits->max_bold_bytes = XV_STREAM_MARKDOWN_MAX_BOLD_BYTES;
}

static bool limits_valid(const xv_stream_markdown_limits_t *limits) {
  return limits->max_chunk_bytes > 0U &&
         limits->max_chunk_bytes <= XV_STREAM_MARKDOWN_MAX_CHUNK_BYTES &&
         limits->max_document_bytes > 0U && limits->max_bold_bytes > 0U &&
         limits->max_bold_bytes <= XV_STREAM_MARKDOWN_MAX_BOLD_BYTES;
}

bool xv_stream_markdown_init(xv_stream_markdown_t *parser,
                             xv_stream_markdown_emit_fn emit,
                             void *emit_context,
                             const xv_stream_markdown_limits_t *limits) {
  xv_stream_markdown_limits_t selected;

  if (parser == NULL || emit == NULL) {
    return false;
  }
  if (limits == NULL) {
    xv_stream_markdown_limits_default(&selected);
  } else {
    selected = *limits;
  }
  if (!limits_valid(&selected)) {
    return false;
  }
  memset(parser, 0, sizeof(*parser));
  parser->emit = emit;
  parser->emit_context = emit_context;
  parser->limits = selected;
  parser->error = XV_STREAM_MARKDOWN_OK;
  parser->at_line_start = true;
  return true;
}

void xv_stream_markdown_reset(xv_stream_markdown_t *parser) {
  xv_stream_markdown_emit_fn emit;
  xv_stream_markdown_limits_t limits;
  void *context;

  if (parser == NULL) {
    return;
  }
  emit = parser->emit;
  context = parser->emit_context;
  limits = parser->limits;
  memset(parser, 0, sizeof(*parser));
  parser->emit = emit;
  parser->emit_context = context;
  parser->limits = limits;
  parser->error = XV_STREAM_MARKDOWN_OK;
  parser->at_line_start = true;
}

xv_stream_markdown_result_t xv_stream_markdown_feed(
    xv_stream_markdown_t *parser, const uint8_t *data, size_t length) {
  size_t index;

  if (parser == NULL || parser->emit == NULL ||
      (data == NULL && length > 0U)) {
    return XV_STREAM_MARKDOWN_BAD_ARGUMENT;
  }
  if (parser->error != XV_STREAM_MARKDOWN_OK) {
    return parser->error;
  }
  if (parser->finished) {
    return XV_STREAM_MARKDOWN_FINISHED;
  }
  if (length > parser->limits.max_chunk_bytes) {
    return XV_STREAM_MARKDOWN_INPUT_TOO_LARGE;
  }
  if (length >
      (size_t)(parser->limits.max_document_bytes - parser->document_bytes)) {
    return XV_STREAM_MARKDOWN_DOCUMENT_TOO_LARGE;
  }
  parser->document_bytes += (uint32_t)length;

  for (index = 0U; index < length; ++index) {
    if (!consume_utf8_byte(parser, data[index])) {
      if (parser->error == XV_STREAM_MARKDOWN_OK) {
        parser->error = XV_STREAM_MARKDOWN_INVALID_UTF8;
      }
      return parser->error;
    }
  }
  if (!flush_output(parser)) {
    return parser->error;
  }
  return XV_STREAM_MARKDOWN_OK;
}

xv_stream_markdown_result_t
xv_stream_markdown_finish(xv_stream_markdown_t *parser) {
  static const uint8_t cr = '\r';
  static const uint8_t star = '*';

  if (parser == NULL || parser->emit == NULL) {
    return XV_STREAM_MARKDOWN_BAD_ARGUMENT;
  }
  if (parser->error != XV_STREAM_MARKDOWN_OK) {
    return parser->error;
  }
  if (parser->finished) {
    return XV_STREAM_MARKDOWN_FINISHED;
  }
  if (parser->utf8_expected != 0U) {
    parser->error = XV_STREAM_MARKDOWN_INVALID_UTF8;
    return parser->error;
  }
  if (parser->cr_pending) {
    parser->cr_pending = false;
    if (!process_newline(parser, &cr, 1U)) {
      return parser->error;
    }
  }

  if (parser->opening_fence_line) {
    parser->opening_fence_line = false;
    parser->in_code = true;
  }
  if (parser->in_code) {
    if (!(parser->at_line_start && parser->fence_ticks >= 3U) &&
        !flush_code_fence_candidate(parser)) {
      return parser->error;
    }
    parser->fence_ticks = 0U;
    parser->fence_trailing_length = 0U;
    parser->in_code = false;
  } else {
    if (parser->prefix_state != XV_MARKDOWN_PREFIX_NONE &&
        !replay_prefix(parser)) {
      return parser->error;
    }
    if (parser->bold_active) {
      if (parser->bold_star_pending) {
        if (!emit_bold_literal(parser, &star, 1U, false)) {
          return parser->error;
        }
      } else if (!emit_bold_literal(parser, NULL, 0U, false)) {
        return parser->error;
      }
    }
    parser->bold_literal_active = false;
    parser->bold_literal_star_seen = false;
    if (parser->text_star_pending) {
      parser->text_star_pending = false;
      if (!emit_bytes(parser, XV_MARKDOWN_SPAN_TEXT, 0U, &star, 1U)) {
        return parser->error;
      }
    }
  }
  if (!flush_output(parser)) {
    return parser->error;
  }
  parser->finished = true;
  return XV_STREAM_MARKDOWN_OK;
}
