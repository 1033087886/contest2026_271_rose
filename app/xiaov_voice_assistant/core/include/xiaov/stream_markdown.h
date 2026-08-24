#ifndef XIAOV_STREAM_MARKDOWN_H
#define XIAOV_STREAM_MARKDOWN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hard limits keep parser storage and per-call work explicit on the device. */
#define XV_STREAM_MARKDOWN_OUTPUT_CAPACITY 128U
#define XV_STREAM_MARKDOWN_MAX_BOLD_BYTES 256U
#define XV_STREAM_MARKDOWN_MAX_CHUNK_BYTES 4096U
#define XV_STREAM_MARKDOWN_DEFAULT_DOCUMENT_BYTES (1024U * 1024U)
#define XV_STREAM_MARKDOWN_MAX_ORDERED_DIGITS 9U
#define XV_STREAM_MARKDOWN_MAX_FENCE_TICKS 16U
#define XV_STREAM_MARKDOWN_MAX_FENCE_TRAILING_BYTES 16U

typedef enum {
  XV_MARKDOWN_SPAN_TEXT = 0,
  XV_MARKDOWN_SPAN_BOLD,
  XV_MARKDOWN_SPAN_CODE,
  XV_MARKDOWN_SPAN_UNORDERED_MARKER,
  XV_MARKDOWN_SPAN_ORDERED_MARKER
} xv_markdown_span_type_t;

typedef struct {
  xv_markdown_span_type_t type;
  const uint8_t *data;
  size_t length;
  /* Set to the parsed list number for XV_MARKDOWN_SPAN_ORDERED_MARKER. */
  uint32_t value;
} xv_markdown_span_t;

/* span->data is valid only for the duration of the callback. */
typedef bool (*xv_stream_markdown_emit_fn)(
    void *context, const xv_markdown_span_t *span);

typedef struct {
  uint32_t max_chunk_bytes;
  uint32_t max_document_bytes;
  uint16_t max_bold_bytes;
} xv_stream_markdown_limits_t;

typedef enum {
  XV_STREAM_MARKDOWN_OK = 0,
  XV_STREAM_MARKDOWN_BAD_ARGUMENT,
  XV_STREAM_MARKDOWN_INPUT_TOO_LARGE,
  XV_STREAM_MARKDOWN_DOCUMENT_TOO_LARGE,
  XV_STREAM_MARKDOWN_INVALID_UTF8,
  XV_STREAM_MARKDOWN_CALLBACK_ERROR,
  XV_STREAM_MARKDOWN_FINISHED
} xv_stream_markdown_result_t;

typedef enum {
  XV_MARKDOWN_PREFIX_NONE = 0,
  XV_MARKDOWN_PREFIX_UNORDERED,
  XV_MARKDOWN_PREFIX_ORDERED_DIGITS,
  XV_MARKDOWN_PREFIX_ORDERED_DOT,
  XV_MARKDOWN_PREFIX_FENCE
} xv_markdown_prefix_state_t;

/*
 * The structure is public so callers can allocate it statically. Treat fields
 * after limits as private implementation state and use reset() for reuse.
 */
typedef struct {
  xv_stream_markdown_emit_fn emit;
  void *emit_context;
  xv_stream_markdown_limits_t limits;
  uint32_t document_bytes;
  xv_stream_markdown_result_t error;

  uint8_t output[XV_STREAM_MARKDOWN_OUTPUT_CAPACITY];
  size_t output_length;
  xv_markdown_span_type_t output_type;
  uint32_t output_value;

  uint8_t bold[XV_STREAM_MARKDOWN_MAX_BOLD_BYTES];
  size_t bold_length;
  bool bold_active;
  bool bold_star_pending;
  bool bold_literal_active;
  bool bold_literal_star_seen;
  bool text_star_pending;

  uint8_t prefix[XV_STREAM_MARKDOWN_MAX_ORDERED_DIGITS + 2U];
  size_t prefix_length;
  xv_markdown_prefix_state_t prefix_state;

  uint8_t utf8[4];
  uint8_t utf8_length;
  uint8_t utf8_expected;
  uint8_t utf8_second_min;
  uint8_t utf8_second_max;

  uint8_t fence_ticks;
  uint8_t fence_trailing[XV_STREAM_MARKDOWN_MAX_FENCE_TRAILING_BYTES];
  uint8_t fence_trailing_length;
  bool at_line_start;
  bool opening_fence_line;
  bool in_code;
  bool cr_pending;
  bool finished;
} xv_stream_markdown_t;

void xv_stream_markdown_limits_default(xv_stream_markdown_limits_t *limits);

bool xv_stream_markdown_init(xv_stream_markdown_t *parser,
                             xv_stream_markdown_emit_fn emit,
                             void *emit_context,
                             const xv_stream_markdown_limits_t *limits);

/* Reset parsing state while retaining the configured callback and limits. */
void xv_stream_markdown_reset(xv_stream_markdown_t *parser);

/*
 * Input may end at any byte boundary, including inside a UTF-8 code point or a
 * Markdown delimiter. A successful feed flushes complete, decidable spans.
 * Invalid UTF-8 and callback rejection are sticky until reset().
 *
 * This intentionally small subset recognizes column-zero "- ", "+ ", and
 * "* " unordered markers; 1-9 decimal digits followed by ". " for ordered
 * markers; **bold**; and column-zero backtick fences. Tabs may replace the
 * marker/fence trailing spaces, and CR, LF, and CRLF are accepted line endings.
 * Escapes, nesting, indentation, inline code, links, and other Markdown syntax
 * are emitted as text. Opening-fence info is discarded and code stays literal.
 */
xv_stream_markdown_result_t xv_stream_markdown_feed(
    xv_stream_markdown_t *parser, const uint8_t *data, size_t length);

/*
 * Finalizes the stream. Unclosed bold is emitted literally as TEXT; an open
 * fenced block is implicitly closed and its content remains CODE. An incomplete
 * UTF-8 code point is an error. A second successful finish returns FINISHED.
 */
xv_stream_markdown_result_t
xv_stream_markdown_finish(xv_stream_markdown_t *parser);

#ifdef __cplusplus
}
#endif

#endif
