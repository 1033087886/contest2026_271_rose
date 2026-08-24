#include "xiaov_protocol.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define XV_JSON_MAX_DEPTH 12U
#define XV_JSON_KEY_SIZE 32U
#define XV_JSON_TYPE_SIZE 24U

typedef struct {
  char *data;
  size_t capacity;
  size_t length;
  bool overflow;
} xv_json_writer_t;

typedef struct {
  const char *data;
  size_t length;
  size_t position;
} xv_json_parser_t;

typedef struct {
  bool version_seen;
  bool type_seen;
  bool id_seen;
  bool timestamp_seen;
  bool payload_seen;
  bool turn_id_seen;
  bool stream_id_seen;
  bool samples_seen;
  bool recoverable_seen;
  bool text_seen;
  bool timer_id_seen;
  bool label_seen;
  bool reminder_kind_seen;
  bool deadline_epoch_seen;
  bool media_action_seen;
  bool media_query_seen;
  bool volume_percent_seen;
  bool position_seconds_seen;
  bool command_id_seen;
  bool error_code_seen;
  bool media_ok_seen;
  bool display_mode_seen;
  bool display_status_seen;
  bool nonce_seen;
  uint32_t stream_id;
  uint32_t samples;
  double deadline_epoch;
  uint32_t volume_percent;
  uint32_t nonce;
  double position_seconds;
  bool media_ok;
  bool recoverable;
  char turn_id[XV_CONTROL_TURN_ID_MAX];
  char text[XV_CONTROL_TEXT_MAX];
  char timer_id[XV_CONTROL_REMINDER_TIMER_ID_MAX];
  char label[XV_CONTROL_REMINDER_LABEL_MAX];
  char reminder_kind[XV_CONTROL_REMINDER_KIND_MAX];
  char media_action[XV_CONTROL_MEDIA_ACTION_MAX];
  char media_query[XV_CONTROL_MEDIA_QUERY_MAX];
  char command_id[XV_CONTROL_COMMAND_ID_MAX];
  char error_code[XV_CONTROL_ERROR_CODE_MAX];
  char display_mode[XV_CONTROL_DISPLAY_MODE_MAX];
  char display_status[XV_CONTROL_DISPLAY_STATUS_MAX];
  char type[XV_JSON_TYPE_SIZE];
} xv_decoded_fields_t;

static bool utf8_sequence(const unsigned char *data, size_t remaining,
                          size_t *sequence_size) {
  unsigned char first;

  if (remaining == 0U || sequence_size == NULL) {
    return false;
  }
  first = data[0];
  if (first < 0x80U) {
    *sequence_size = 1U;
    return true;
  }
  if (first >= 0xc2U && first <= 0xdfU && remaining >= 2U &&
      data[1] >= 0x80U && data[1] <= 0xbfU) {
    *sequence_size = 2U;
    return true;
  }
  if (first == 0xe0U && remaining >= 3U && data[1] >= 0xa0U &&
      data[1] <= 0xbfU && data[2] >= 0x80U && data[2] <= 0xbfU) {
    *sequence_size = 3U;
    return true;
  }
  if (((first >= 0xe1U && first <= 0xecU) ||
       (first >= 0xeeU && first <= 0xefU)) &&
      remaining >= 3U && data[1] >= 0x80U && data[1] <= 0xbfU &&
      data[2] >= 0x80U && data[2] <= 0xbfU) {
    *sequence_size = 3U;
    return true;
  }
  if (first == 0xedU && remaining >= 3U && data[1] >= 0x80U &&
      data[1] <= 0x9fU && data[2] >= 0x80U && data[2] <= 0xbfU) {
    *sequence_size = 3U;
    return true;
  }
  if (first == 0xf0U && remaining >= 4U && data[1] >= 0x90U &&
      data[1] <= 0xbfU && data[2] >= 0x80U && data[2] <= 0xbfU &&
      data[3] >= 0x80U && data[3] <= 0xbfU) {
    *sequence_size = 4U;
    return true;
  }
  if (first >= 0xf1U && first <= 0xf3U && remaining >= 4U &&
      data[1] >= 0x80U && data[1] <= 0xbfU && data[2] >= 0x80U &&
      data[2] <= 0xbfU && data[3] >= 0x80U && data[3] <= 0xbfU) {
    *sequence_size = 4U;
    return true;
  }
  if (first == 0xf4U && remaining >= 4U && data[1] >= 0x80U &&
      data[1] <= 0x8fU && data[2] >= 0x80U && data[2] <= 0xbfU &&
      data[3] >= 0x80U && data[3] <= 0xbfU) {
    *sequence_size = 4U;
    return true;
  }
  return false;
}

static bool valid_utf8_string(const char *value, bool require_nonempty) {
  const unsigned char *cursor;
  size_t remaining;

  if (value == NULL || (require_nonempty && value[0] == '\0')) {
    return false;
  }
  cursor = (const unsigned char *)value;
  remaining = strlen(value);
  while (remaining > 0U) {
    size_t sequence_size;
    if (!utf8_sequence(cursor, remaining, &sequence_size)) {
      return false;
    }
    cursor += sequence_size;
    remaining -= sequence_size;
  }
  return true;
}

static bool valid_bounded_utf8_string(const char *value, size_t capacity,
                                      bool require_nonempty) {
  size_t index;

  if (value == NULL || capacity == 0U) {
    return false;
  }
  for (index = 0U; index < capacity; ++index) {
    if (value[index] == '\0') {
      return (!require_nonempty || index > 0U) &&
             valid_utf8_string(value, require_nonempty);
    }
  }
  return false;
}

static void writer_init(xv_json_writer_t *writer, char *destination,
                        size_t destination_size) {
  writer->data = destination;
  writer->capacity = destination_size;
  writer->length = 0U;
  writer->overflow = destination == NULL || destination_size == 0U;
  if (!writer->overflow) {
    destination[0] = '\0';
  }
}

static void writer_bytes(xv_json_writer_t *writer, const char *data,
                         size_t length) {
  if (writer->overflow) {
    return;
  }
  if (length >= writer->capacity - writer->length) {
    writer->overflow = true;
    writer->data[0] = '\0';
    return;
  }
  memcpy(writer->data + writer->length, data, length);
  writer->length += length;
  writer->data[writer->length] = '\0';
}

static void writer_text(xv_json_writer_t *writer, const char *text) {
  writer_bytes(writer, text, strlen(text));
}

static void writer_u64(xv_json_writer_t *writer, uint64_t value) {
  char number[32];
  int length = snprintf(number, sizeof(number), "%" PRIu64, value);

  if (length <= 0 || (size_t)length >= sizeof(number)) {
    writer->overflow = true;
    if (writer->capacity > 0U) {
      writer->data[0] = '\0';
    }
    return;
  }
  writer_bytes(writer, number, (size_t)length);
}

static void writer_bool(xv_json_writer_t *writer, bool value) {
  writer_text(writer, value ? "true" : "false");
}

static void writer_string(xv_json_writer_t *writer, const char *value) {
  const unsigned char *cursor = (const unsigned char *)value;
  static const char hex[] = "0123456789abcdef";

  writer_text(writer, "\"");
  while (*cursor != '\0' && !writer->overflow) {
    if (*cursor == '"' || *cursor == '\\') {
      char escaped[2] = {'\\', (char)*cursor};
      writer_bytes(writer, escaped, sizeof(escaped));
      ++cursor;
    } else if (*cursor < 0x20U) {
      char escaped[6] = {'\\', 'u', '0', '0', hex[*cursor >> 4U],
                         hex[*cursor & 0x0fU]};
      writer_bytes(writer, escaped, sizeof(escaped));
      ++cursor;
    } else {
      size_t sequence_size;
      size_t remaining = strlen((const char *)cursor);
      if (!utf8_sequence(cursor, remaining, &sequence_size)) {
        writer->overflow = true;
        writer->data[0] = '\0';
        return;
      }
      writer_bytes(writer, (const char *)cursor, sequence_size);
      cursor += sequence_size;
    }
  }
  writer_text(writer, "\"");
}

static xv_control_result_t writer_result(const xv_json_writer_t *writer,
                                         size_t *encoded_size) {
  if (writer->overflow) {
    return XV_CONTROL_BUFFER_TOO_SMALL;
  }
  if (encoded_size != NULL) {
    *encoded_size = writer->length;
  }
  return XV_CONTROL_OK;
}

static bool valid_common_encode(char *destination, size_t destination_size,
                                const char *event_id) {
  if (destination != NULL && destination_size > 0U) {
    destination[0] = '\0';
  }
  if (destination == NULL || destination_size == 0U ||
      !valid_utf8_string(event_id, true)) {
    return false;
  }
  return true;
}

static void write_event_prefix(xv_json_writer_t *writer, const char *type,
                               const char *event_id, uint64_t timestamp_ms) {
  writer_text(writer, "{\"v\":1,\"type\":");
  writer_string(writer, type);
  writer_text(writer, ",\"id\":");
  writer_string(writer, event_id);
  writer_text(writer, ",\"ts_ms\":");
  writer_u64(writer, timestamp_ms);
}

xv_control_result_t xv_control_encode_session_start(
    char *destination, size_t destination_size, const char *event_id,
    uint64_t timestamp_ms, const char *device_id, const char *token,
    bool touch_capable, bool barge_in_capable, size_t *encoded_size) {
  xv_json_writer_t writer;

  if (encoded_size != NULL) {
    *encoded_size = 0U;
  }
  if (!valid_common_encode(destination, destination_size, event_id) ||
      !valid_utf8_string(device_id, true) ||
      (token != NULL && !valid_utf8_string(token, true))) {
    return XV_CONTROL_BAD_ARGUMENT;
  }
  writer_init(&writer, destination, destination_size);
  write_event_prefix(&writer, "session.start", event_id, timestamp_ms);
  writer_text(&writer, ",\"payload\":{\"device_id\":");
  writer_string(&writer, device_id);
  writer_text(&writer, ",\"capabilities\":{\"touch\":");
  writer_bool(&writer, touch_capable);
  writer_text(&writer, ",\"barge_in\":");
  writer_bool(&writer, barge_in_capable);
  writer_text(&writer, "}");
  if (token != NULL) {
    writer_text(&writer, ",\"token\":");
    writer_string(&writer, token);
  }
  writer_text(&writer, "}}");
  return writer_result(&writer, encoded_size);
}

xv_control_result_t xv_control_encode_listen_start(
    char *destination, size_t destination_size, const char *event_id,
    uint64_t timestamp_ms, const char *turn_id, uint32_t stream_id,
    size_t *encoded_size) {
  xv_json_writer_t writer;

  if (encoded_size != NULL) {
    *encoded_size = 0U;
  }
  if (!valid_common_encode(destination, destination_size, event_id) ||
      !valid_utf8_string(turn_id, true)) {
    return XV_CONTROL_BAD_ARGUMENT;
  }
  writer_init(&writer, destination, destination_size);
  write_event_prefix(&writer, "listen.start", event_id, timestamp_ms);
  writer_text(&writer, ",\"turn_id\":");
  writer_string(&writer, turn_id);
  writer_text(&writer, ",\"payload\":{\"stream_id\":");
  writer_u64(&writer, stream_id);
  writer_text(&writer,
              ",\"audio\":{\"encoding\":\"pcm_s16le\",\"sample_rate\":16000,"
              "\"channels\":1,\"frame_ms\":20}}}");
  return writer_result(&writer, encoded_size);
}

static xv_control_result_t encode_reason_event(
    char *destination, size_t destination_size, const char *type,
    const char *event_id, uint64_t timestamp_ms, const char *turn_id,
    const char *reason, size_t *encoded_size) {
  xv_json_writer_t writer;

  if (encoded_size != NULL) {
    *encoded_size = 0U;
  }
  if (!valid_common_encode(destination, destination_size, event_id) ||
      !valid_utf8_string(turn_id, true) ||
      !valid_utf8_string(reason, true)) {
    return XV_CONTROL_BAD_ARGUMENT;
  }
  writer_init(&writer, destination, destination_size);
  write_event_prefix(&writer, type, event_id, timestamp_ms);
  writer_text(&writer, ",\"turn_id\":");
  writer_string(&writer, turn_id);
  writer_text(&writer, ",\"payload\":{\"reason\":");
  writer_string(&writer, reason);
  writer_text(&writer, "}}");
  return writer_result(&writer, encoded_size);
}

xv_control_result_t xv_control_encode_listen_stop(
    char *destination, size_t destination_size, const char *event_id,
    uint64_t timestamp_ms, const char *turn_id, const char *reason,
    size_t *encoded_size) {
  return encode_reason_event(destination, destination_size, "listen.stop",
                             event_id, timestamp_ms, turn_id, reason,
                             encoded_size);
}

xv_control_result_t xv_control_encode_turn_cancel(
    char *destination, size_t destination_size, const char *event_id,
    uint64_t timestamp_ms, const char *turn_id, const char *reason,
    size_t *encoded_size) {
  return encode_reason_event(destination, destination_size, "turn.cancel",
                             event_id, timestamp_ms, turn_id, reason,
                             encoded_size);
}

xv_control_result_t xv_control_encode_ping(
    char *destination, size_t destination_size, const char *event_id,
    uint64_t timestamp_ms, bool include_nonce, uint32_t nonce,
    size_t *encoded_size) {
  xv_json_writer_t writer;

  if (encoded_size != NULL) {
    *encoded_size = 0U;
  }
  if (!valid_common_encode(destination, destination_size, event_id)) {
    return XV_CONTROL_BAD_ARGUMENT;
  }
  writer_init(&writer, destination, destination_size);
  write_event_prefix(&writer, "ping", event_id, timestamp_ms);
  writer_text(&writer, ",\"payload\":{");
  if (include_nonce) {
    writer_text(&writer, "\"nonce\":");
    writer_u64(&writer, nonce);
  }
  writer_text(&writer, "}}");
  return writer_result(&writer, encoded_size);
}

xv_control_result_t xv_control_encode_media_result(
    char *destination, size_t destination_size, const char *event_id,
    uint64_t timestamp_ms, const char *command_id, bool ok,
    const char *error_code, size_t *encoded_size) {
  xv_json_writer_t writer;

  if (encoded_size != NULL) {
    *encoded_size = 0U;
  }
  if (!valid_common_encode(destination, destination_size, event_id) ||
      !valid_bounded_utf8_string(command_id, XV_CONTROL_COMMAND_ID_MAX,
                                 true) ||
      (ok && error_code != NULL) || (!ok && error_code == NULL) ||
      (error_code != NULL &&
       !valid_bounded_utf8_string(error_code, XV_CONTROL_ERROR_CODE_MAX,
                                  true))) {
    return XV_CONTROL_BAD_ARGUMENT;
  }
  writer_init(&writer, destination, destination_size);
  write_event_prefix(&writer, "media.result", event_id, timestamp_ms);
  writer_text(&writer, ",\"payload\":{\"command_id\":");
  writer_string(&writer, command_id);
  writer_text(&writer, ",\"ok\":");
  writer_bool(&writer, ok);
  if (error_code != NULL) {
    writer_text(&writer, ",\"error_code\":");
    writer_string(&writer, error_code);
  }
  writer_text(&writer, "}}");
  return writer_result(&writer, encoded_size);
}

xv_control_result_t xv_control_encode_media_ready(
    char *destination, size_t destination_size, const char *event_id,
    uint64_t timestamp_ms, const char *command_id, bool ok,
    const char *error_code, size_t *encoded_size) {
  xv_json_writer_t writer;

  if (encoded_size != NULL) {
    *encoded_size = 0U;
  }
  if (!valid_common_encode(destination, destination_size, event_id) ||
      !valid_bounded_utf8_string(command_id, XV_CONTROL_COMMAND_ID_MAX,
                                 true) ||
      (ok && error_code != NULL) || (!ok && error_code == NULL) ||
      (error_code != NULL &&
       !valid_bounded_utf8_string(error_code, XV_CONTROL_ERROR_CODE_MAX,
                                  true))) {
    return XV_CONTROL_BAD_ARGUMENT;
  }
  writer_init(&writer, destination, destination_size);
  write_event_prefix(&writer, "media.ready", event_id, timestamp_ms);
  writer_text(&writer, ",\"payload\":{\"command_id\":");
  writer_string(&writer, command_id);
  writer_text(&writer, ",\"ok\":");
  writer_bool(&writer, ok);
  if (error_code != NULL) {
    writer_text(&writer, ",\"error_code\":");
    writer_string(&writer, error_code);
  }
  writer_text(&writer, "}}");
  return writer_result(&writer, encoded_size);
}

xv_control_result_t xv_control_encode_display_command(
    char *destination, size_t destination_size, const char *event_id,
    uint64_t timestamp_ms, const char *action, const char *source,
    size_t *encoded_size) {
  xv_json_writer_t writer;

  if (encoded_size != NULL) {
    *encoded_size = 0U;
  }
  if (!valid_common_encode(destination, destination_size, event_id) ||
      !valid_bounded_utf8_string(action, XV_CONTROL_MEDIA_ACTION_MAX, true) ||
      (source != NULL &&
       !valid_bounded_utf8_string(source, XV_CONTROL_MEDIA_QUERY_MAX, true))) {
    return XV_CONTROL_BAD_ARGUMENT;
  }
  writer_init(&writer, destination, destination_size);
  write_event_prefix(&writer, "display.command", event_id, timestamp_ms);
  writer_text(&writer, ",\"payload\":{\"action\":");
  writer_string(&writer, action);
  if (source != NULL) {
    writer_text(&writer, ",\"source\":");
    writer_string(&writer, source);
  }
  writer_text(&writer, "}}");
  return writer_result(&writer, encoded_size);
}

static void parser_space(xv_json_parser_t *parser) {
  while (parser->position < parser->length) {
    char value = parser->data[parser->position];
    if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
      break;
    }
    ++parser->position;
  }
}

static bool parser_take(xv_json_parser_t *parser, char expected) {
  parser_space(parser);
  if (parser->position >= parser->length ||
      parser->data[parser->position] != expected) {
    return false;
  }
  ++parser->position;
  return true;
}

static int hex_value(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

static bool parser_hex_quad(xv_json_parser_t *parser, uint32_t *result) {
  uint32_t codepoint = 0U;
  size_t index;

  for (index = 0U; index < 4U; ++index) {
    int digit;
    if (parser->position >= parser->length ||
        (digit = hex_value(parser->data[parser->position++])) < 0) {
      return false;
    }
    codepoint = codepoint * 16U + (uint32_t)digit;
  }
  *result = codepoint;
  return true;
}

static size_t encode_utf8_codepoint(uint32_t codepoint, char output[4]) {
  if (codepoint <= 0x7fU) {
    output[0] = (char)codepoint;
    return 1U;
  }
  if (codepoint <= 0x7ffU) {
    output[0] = (char)(0xc0U | (codepoint >> 6U));
    output[1] = (char)(0x80U | (codepoint & 0x3fU));
    return 2U;
  }
  if (codepoint <= 0xffffU) {
    output[0] = (char)(0xe0U | (codepoint >> 12U));
    output[1] = (char)(0x80U | ((codepoint >> 6U) & 0x3fU));
    output[2] = (char)(0x80U | (codepoint & 0x3fU));
    return 3U;
  }
  output[0] = (char)(0xf0U | (codepoint >> 18U));
  output[1] = (char)(0x80U | ((codepoint >> 12U) & 0x3fU));
  output[2] = (char)(0x80U | ((codepoint >> 6U) & 0x3fU));
  output[3] = (char)(0x80U | (codepoint & 0x3fU));
  return 4U;
}

static void capture_bytes(char *capture, size_t capture_size,
                          size_t *output_length, bool *complete,
                          const char *bytes, size_t byte_count) {
  if (capture != NULL) {
    if (*output_length + byte_count < capture_size) {
      memcpy(capture + *output_length, bytes, byte_count);
    } else {
      *complete = false;
    }
  }
  *output_length += byte_count;
}

static bool parser_string(xv_json_parser_t *parser, char *capture,
                          size_t capture_size, size_t *decoded_length,
                          bool *capture_complete) {
  size_t output_length = 0U;
  bool complete = true;

  parser_space(parser);
  if (parser->position >= parser->length ||
      parser->data[parser->position++] != '"') {
    return false;
  }
  while (parser->position < parser->length) {
    unsigned char value = (unsigned char)parser->data[parser->position++];
    if (value == '"') {
      if (capture != NULL && capture_size > 0U) {
        size_t terminator =
            output_length < capture_size ? output_length : capture_size - 1U;
        capture[terminator] = '\0';
      }
      if (decoded_length != NULL) {
        *decoded_length = output_length;
      }
      if (capture_complete != NULL) {
        *capture_complete = complete;
      }
      return true;
    }
    if (value < 0x20U) {
      return false;
    }
    if (value == '\\') {
      unsigned char escaped;
      if (parser->position >= parser->length) {
        return false;
      }
      escaped = (unsigned char)parser->data[parser->position++];
      if (escaped == 'u') {
        char encoded[4];
        uint32_t codepoint;
        size_t encoded_size;

        if (!parser_hex_quad(parser, &codepoint)) {
          return false;
        }
        if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
          uint32_t low_surrogate;
          if (parser->position + 2U > parser->length ||
              parser->data[parser->position] != '\\' ||
              parser->data[parser->position + 1U] != 'u') {
            return false;
          }
          parser->position += 2U;
          if (!parser_hex_quad(parser, &low_surrogate) ||
              low_surrogate < 0xdc00U || low_surrogate > 0xdfffU) {
            return false;
          }
          codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) +
                      (low_surrogate - 0xdc00U);
        } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
          return false;
        }
        if (codepoint == 0U || codepoint > 0x10ffffU) {
          return false;
        }
        encoded_size = encode_utf8_codepoint(codepoint, encoded);
        capture_bytes(capture, capture_size, &output_length, &complete,
                      encoded, encoded_size);
        continue;
      } else {
        switch (escaped) {
        case '"':
        case '\\':
        case '/':
          value = escaped;
          break;
        case 'b':
          value = '\b';
          break;
        case 'f':
          value = '\f';
          break;
        case 'n':
          value = '\n';
          break;
        case 'r':
          value = '\r';
          break;
        case 't':
          value = '\t';
          break;
        default:
          return false;
        }
      }
    } else if (value >= 0x80U) {
      size_t sequence_size;
      size_t start = parser->position - 1U;
      if (!utf8_sequence((const unsigned char *)parser->data + start,
                         parser->length - start, &sequence_size)) {
        return false;
      }
      capture_bytes(capture, capture_size, &output_length, &complete,
                    parser->data + start, sequence_size);
      parser->position = start + sequence_size;
      continue;
    }
    capture_bytes(capture, capture_size, &output_length, &complete,
                  (const char *)&value, 1U);
  }
  return false;
}

static bool parser_literal(xv_json_parser_t *parser, const char *literal) {
  size_t length = strlen(literal);
  parser_space(parser);
  if (length > parser->length - parser->position ||
      memcmp(parser->data + parser->position, literal, length) != 0) {
    return false;
  }
  parser->position += length;
  return true;
}

static bool parser_unsigned(xv_json_parser_t *parser, uint64_t maximum,
                            uint64_t *result) {
  uint64_t value = 0U;
  size_t start;

  parser_space(parser);
  start = parser->position;
  if (start >= parser->length || parser->data[start] < '0' ||
      parser->data[start] > '9') {
    return false;
  }
  if (parser->data[start] == '0' && start + 1U < parser->length &&
      parser->data[start + 1U] >= '0' && parser->data[start + 1U] <= '9') {
    return false;
  }
  while (parser->position < parser->length &&
         parser->data[parser->position] >= '0' &&
         parser->data[parser->position] <= '9') {
    uint32_t digit = (uint32_t)(parser->data[parser->position] - '0');
    if (value > (maximum - digit) / 10U) {
      return false;
    }
    value = value * 10U + digit;
    ++parser->position;
  }
  if (result != NULL) {
    *result = value;
  }
  return true;
}

static bool parser_bounded_decimal(xv_json_parser_t *parser, double maximum,
                                   double *result) {
  double value = 0.0;
  double fraction_scale = 0.1;
  uint32_t exponent = 0U;
  bool exponent_negative = false;
  size_t digits = 0U;
  uint32_t index;

  parser_space(parser);
  if (parser->position >= parser->length ||
      parser->data[parser->position] < '0' ||
      parser->data[parser->position] > '9') {
    return false;
  }
  if (parser->data[parser->position] == '0') {
    parser->position += 1U;
    if (parser->position < parser->length &&
        parser->data[parser->position] >= '0' &&
        parser->data[parser->position] <= '9') {
      return false;
    }
  } else {
    while (parser->position < parser->length &&
           parser->data[parser->position] >= '0' &&
           parser->data[parser->position] <= '9') {
      value = value * 10.0 +
              (double)(parser->data[parser->position] - '0');
      parser->position += 1U;
    }
  }
  if (parser->position < parser->length &&
      parser->data[parser->position] == '.') {
    parser->position += 1U;
    while (parser->position < parser->length &&
           parser->data[parser->position] >= '0' &&
           parser->data[parser->position] <= '9') {
      value += (double)(parser->data[parser->position] - '0') *
               fraction_scale;
      fraction_scale *= 0.1;
      parser->position += 1U;
      digits += 1U;
    }
    if (digits == 0U) {
      return false;
    }
  }
  if (parser->position < parser->length &&
      (parser->data[parser->position] == 'e' ||
       parser->data[parser->position] == 'E')) {
    parser->position += 1U;
    if (parser->position < parser->length &&
        (parser->data[parser->position] == '+' ||
         parser->data[parser->position] == '-')) {
      exponent_negative = parser->data[parser->position] == '-';
      parser->position += 1U;
    }
    digits = 0U;
    while (parser->position < parser->length &&
           parser->data[parser->position] >= '0' &&
           parser->data[parser->position] <= '9') {
      uint32_t digit = (uint32_t)(parser->data[parser->position] - '0');
      if (exponent > 308U / 10U ||
          exponent * 10U + digit > 308U) {
        return false;
      }
      exponent = exponent * 10U + digit;
      parser->position += 1U;
      digits += 1U;
    }
    if (digits == 0U) {
      return false;
    }
    for (index = 0U; index < exponent; ++index) {
      value = exponent_negative ? value / 10.0 : value * 10.0;
      if (value > maximum) {
        return false;
      }
    }
  }
  if (value > maximum) {
    return false;
  }
  *result = value;
  return true;
}

static bool parser_number(xv_json_parser_t *parser) {
  parser_space(parser);
  if (parser->position < parser->length &&
      parser->data[parser->position] == '-') {
    ++parser->position;
  }
  if (parser->position >= parser->length) {
    return false;
  }
  if (parser->data[parser->position] == '0') {
    ++parser->position;
    if (parser->position < parser->length &&
        parser->data[parser->position] >= '0' &&
        parser->data[parser->position] <= '9') {
      return false;
    }
  } else {
    size_t digits = 0U;
    while (parser->position < parser->length &&
           parser->data[parser->position] >= '0' &&
           parser->data[parser->position] <= '9') {
      ++parser->position;
      ++digits;
    }
    if (digits == 0U) {
      return false;
    }
  }
  if (parser->position < parser->length &&
      parser->data[parser->position] == '.') {
    size_t digits = 0U;
    ++parser->position;
    while (parser->position < parser->length &&
           parser->data[parser->position] >= '0' &&
           parser->data[parser->position] <= '9') {
      ++parser->position;
      ++digits;
    }
    if (digits == 0U) {
      return false;
    }
  }
  if (parser->position < parser->length &&
      (parser->data[parser->position] == 'e' ||
       parser->data[parser->position] == 'E')) {
    size_t digits = 0U;
    ++parser->position;
    if (parser->position < parser->length &&
        (parser->data[parser->position] == '+' ||
         parser->data[parser->position] == '-')) {
      ++parser->position;
    }
    while (parser->position < parser->length &&
           parser->data[parser->position] >= '0' &&
           parser->data[parser->position] <= '9') {
      ++parser->position;
      ++digits;
    }
    if (digits == 0U) {
      return false;
    }
  }
  return true;
}

static bool parser_value(xv_json_parser_t *parser, unsigned int depth);

static bool parser_array(xv_json_parser_t *parser, unsigned int depth) {
  if (depth >= XV_JSON_MAX_DEPTH || !parser_take(parser, '[')) {
    return false;
  }
  parser_space(parser);
  if (parser_take(parser, ']')) {
    return true;
  }
  for (;;) {
    if (!parser_value(parser, depth + 1U)) {
      return false;
    }
    if (parser_take(parser, ']')) {
      return true;
    }
    if (!parser_take(parser, ',')) {
      return false;
    }
  }
}

static bool parser_object(xv_json_parser_t *parser, unsigned int depth) {
  if (depth >= XV_JSON_MAX_DEPTH || !parser_take(parser, '{')) {
    return false;
  }
  parser_space(parser);
  if (parser_take(parser, '}')) {
    return true;
  }
  for (;;) {
    if (!parser_string(parser, NULL, 0U, NULL, NULL) ||
        !parser_take(parser, ':') || !parser_value(parser, depth + 1U)) {
      return false;
    }
    if (parser_take(parser, '}')) {
      return true;
    }
    if (!parser_take(parser, ',')) {
      return false;
    }
  }
}

static bool parser_value(xv_json_parser_t *parser, unsigned int depth) {
  char value;
  parser_space(parser);
  if (parser->position >= parser->length) {
    return false;
  }
  value = parser->data[parser->position];
  if (value == '"') {
    return parser_string(parser, NULL, 0U, NULL, NULL);
  }
  if (value == '{') {
    return parser_object(parser, depth);
  }
  if (value == '[') {
    return parser_array(parser, depth);
  }
  if (value == 't') {
    return parser_literal(parser, "true");
  }
  if (value == 'f') {
    return parser_literal(parser, "false");
  }
  if (value == 'n') {
    return parser_literal(parser, "null");
  }
  return parser_number(parser);
}

static bool key_equals(const char *key, bool complete, const char *expected) {
  return complete && strcmp(key, expected) == 0;
}

static bool parse_payload(xv_json_parser_t *parser,
                          xv_decoded_fields_t *fields) {
  if (!parser_take(parser, '{')) {
    return false;
  }
  parser_space(parser);
  if (parser_take(parser, '}')) {
    return true;
  }
  for (;;) {
    char key[XV_JSON_KEY_SIZE];
    bool complete;
    uint64_t number;

    if (!parser_string(parser, key, sizeof(key), NULL, &complete) ||
        !parser_take(parser, ':')) {
      return false;
    }
    if (key_equals(key, complete, "nonce")) {
      if (fields->nonce_seen ||
          !parser_unsigned(parser, UINT32_MAX, &number)) {
        return false;
      }
      fields->nonce_seen = true;
      fields->nonce = (uint32_t)number;
    } else if (key_equals(key, complete, "stream_id")) {
      if (fields->stream_id_seen ||
          !parser_unsigned(parser, UINT32_MAX, &number)) {
        return false;
      }
      fields->stream_id_seen = true;
      fields->stream_id = (uint32_t)number;
    } else if (key_equals(key, complete, "samples")) {
      if (fields->samples_seen ||
          !parser_unsigned(parser, UINT32_MAX, &number)) {
        return false;
      }
      fields->samples_seen = true;
      fields->samples = (uint32_t)number;
    } else if (key_equals(key, complete, "recoverable")) {
      if (fields->recoverable_seen) {
        return false;
      }
      if (parser_literal(parser, "true")) {
        fields->recoverable = true;
      } else if (parser_literal(parser, "false")) {
        fields->recoverable = false;
      } else {
        return false;
      }
      fields->recoverable_seen = true;
    } else if (key_equals(key, complete, "text")) {
      bool text_complete;
      size_t text_length;
      if (fields->text_seen ||
          !parser_string(parser, fields->text, sizeof(fields->text),
                         &text_length, &text_complete) ||
          !text_complete) {
        return false;
      }
      fields->text_seen = true;
    } else if (key_equals(key, complete, "timer_id")) {
      bool value_complete;
      size_t value_length;
      if (fields->timer_id_seen ||
          !parser_string(parser, fields->timer_id, sizeof(fields->timer_id),
                         &value_length, &value_complete) ||
          !value_complete || value_length == 0U) {
        return false;
      }
      fields->timer_id_seen = true;
    } else if (key_equals(key, complete, "label")) {
      bool value_complete;
      size_t value_length;
      if (fields->label_seen ||
          !parser_string(parser, fields->label, sizeof(fields->label),
                         &value_length, &value_complete) ||
          !value_complete || value_length == 0U) {
        return false;
      }
      fields->label_seen = true;
    } else if (key_equals(key, complete, "kind")) {
      bool value_complete;
      size_t value_length;
      if (fields->reminder_kind_seen ||
          !parser_string(parser, fields->reminder_kind,
                         sizeof(fields->reminder_kind), &value_length,
                         &value_complete) ||
          !value_complete || value_length == 0U) {
        return false;
      }
      fields->reminder_kind_seen = true;
    } else if (key_equals(key, complete, "deadline_epoch")) {
      if (fields->deadline_epoch_seen ||
          !parser_bounded_decimal(parser, 9007199254740991.0,
                                  &fields->deadline_epoch)) {
        return false;
      }
      fields->deadline_epoch_seen = true;
    } else if (key_equals(key, complete, "action")) {
      bool value_complete;
      size_t value_length;
      if (fields->media_action_seen ||
          !parser_string(parser, fields->media_action,
                         sizeof(fields->media_action), &value_length,
                         &value_complete) ||
          !value_complete || value_length == 0U) {
        return false;
      }
      fields->media_action_seen = true;
    } else if (key_equals(key, complete, "query")) {
      bool value_complete;
      size_t value_length;
      if (fields->media_query_seen ||
          !parser_string(parser, fields->media_query,
                         sizeof(fields->media_query), &value_length,
                         &value_complete) ||
          !value_complete || value_length == 0U) {
        return false;
      }
      fields->media_query_seen = true;
    } else if (key_equals(key, complete, "volume_percent")) {
      if (fields->volume_percent_seen ||
          !parser_unsigned(parser, 100U, &number)) {
        return false;
      }
      fields->volume_percent_seen = true;
      fields->volume_percent = (uint32_t)number;
    } else if (key_equals(key, complete, "position_seconds")) {
      if (fields->position_seconds_seen ||
          !parser_bounded_decimal(parser, 86400.0,
                                  &fields->position_seconds)) {
        return false;
      }
      fields->position_seconds_seen = true;
    } else if (key_equals(key, complete, "command_id")) {
      bool value_complete;
      size_t value_length;
      if (fields->command_id_seen ||
          !parser_string(parser, fields->command_id,
                         sizeof(fields->command_id), &value_length,
                         &value_complete) ||
          !value_complete || value_length == 0U) {
        return false;
      }
      fields->command_id_seen = true;
    } else if (key_equals(key, complete, "error_code") ||
               key_equals(key, complete, "code")) {
      /* The gateway's error event uses payload.code; error_code is retained
       * for media.result and for compatibility with older peers. */
      bool value_complete;
      size_t value_length;
      if (fields->error_code_seen ||
          !parser_string(parser, fields->error_code,
                         sizeof(fields->error_code), &value_length,
                         &value_complete) ||
          !value_complete || value_length == 0U) {
        return false;
      }
      fields->error_code_seen = true;
    } else if (key_equals(key, complete, "ok")) {
      if (fields->media_ok_seen) {
        return false;
      }
      if (parser_literal(parser, "true")) {
        fields->media_ok = true;
      } else if (parser_literal(parser, "false")) {
        fields->media_ok = false;
      } else {
        return false;
      }
      fields->media_ok_seen = true;
    } else if (key_equals(key, complete, "mode")) {
      bool value_complete;
      size_t value_length;
      if (fields->display_mode_seen ||
          !parser_string(parser, fields->display_mode,
                         sizeof(fields->display_mode), &value_length,
                         &value_complete) ||
          !value_complete || value_length == 0U) {
        return false;
      }
      fields->display_mode_seen = true;
    } else if (key_equals(key, complete, "status")) {
      bool value_complete;
      size_t value_length;
      if (fields->display_status_seen ||
          !parser_string(parser, fields->display_status,
                         sizeof(fields->display_status), &value_length,
                         &value_complete) ||
          !value_complete || value_length == 0U) {
        return false;
      }
      fields->display_status_seen = true;
    } else if (!parser_value(parser, 1U)) {
      return false;
    }
    if (parser_take(parser, '}')) {
      return true;
    }
    if (!parser_take(parser, ',')) {
      return false;
    }
  }
}

static bool parse_top_level(xv_json_parser_t *parser,
                            xv_decoded_fields_t *fields) {
  if (!parser_take(parser, '{')) {
    return false;
  }
  parser_space(parser);
  if (parser_take(parser, '}')) {
    return true;
  }
  for (;;) {
    char key[XV_JSON_KEY_SIZE];
    bool complete;
    uint64_t number;
    size_t string_length;

    if (!parser_string(parser, key, sizeof(key), NULL, &complete) ||
        !parser_take(parser, ':')) {
      return false;
    }
    if (key_equals(key, complete, "v")) {
      if (fields->version_seen ||
          !parser_unsigned(parser, UINT32_MAX, &number) ||
          number != XV_CONTROL_PROTOCOL_VERSION) {
        return false;
      }
      fields->version_seen = true;
    } else if (key_equals(key, complete, "type")) {
      bool type_complete;
      if (fields->type_seen ||
          !parser_string(parser, fields->type, sizeof(fields->type),
                         &string_length, &type_complete) ||
          !type_complete || string_length == 0U) {
        return false;
      }
      fields->type_seen = true;
    } else if (key_equals(key, complete, "id")) {
      if (fields->id_seen ||
          !parser_string(parser, NULL, 0U, &string_length, NULL) ||
          string_length == 0U) {
        return false;
      }
      fields->id_seen = true;
    } else if (key_equals(key, complete, "ts_ms")) {
      if (fields->timestamp_seen ||
          !parser_unsigned(parser, UINT64_MAX, &number)) {
        return false;
      }
      fields->timestamp_seen = true;
    } else if (key_equals(key, complete, "turn_id")) {
      bool turn_complete;
      if (fields->turn_id_seen ||
          !parser_string(parser, fields->turn_id, sizeof(fields->turn_id),
                         &string_length, &turn_complete) ||
          !turn_complete || string_length == 0U) {
        return false;
      }
      fields->turn_id_seen = true;
    } else if (key_equals(key, complete, "payload")) {
      if (fields->payload_seen || !parse_payload(parser, fields)) {
        return false;
      }
      fields->payload_seen = true;
    } else if (!parser_value(parser, 0U)) {
      return false;
    }
    if (parser_take(parser, '}')) {
      return true;
    }
    if (!parser_take(parser, ',')) {
      return false;
    }
  }
}

static xv_control_result_t validate_server_fields(
    const xv_decoded_fields_t *fields, xv_server_event_t *event) {
  if (!fields->version_seen || !fields->type_seen || !fields->id_seen ||
      !fields->timestamp_seen || !fields->payload_seen) {
    return XV_CONTROL_INVALID_MESSAGE;
  }
  memset(event, 0, sizeof(*event));
  if (strcmp(fields->type, "session.ready") == 0) {
    event->type = XV_SERVER_EVENT_SESSION_READY;
  } else if (strcmp(fields->type, "asr.partial") == 0 ||
             strcmp(fields->type, "asr.final") == 0 ||
             strcmp(fields->type, "assistant.delta") == 0 ||
             strcmp(fields->type, "assistant.final") == 0) {
    if (!fields->turn_id_seen || !fields->text_seen) {
      return XV_CONTROL_INVALID_MESSAGE;
    }
    if (strcmp(fields->type, "asr.partial") == 0) {
      event->type = XV_SERVER_EVENT_ASR_PARTIAL;
    } else if (strcmp(fields->type, "asr.final") == 0) {
      event->type = XV_SERVER_EVENT_ASR_FINAL;
    } else if (strcmp(fields->type, "assistant.delta") == 0) {
      event->type = XV_SERVER_EVENT_ASSISTANT_DELTA;
    } else {
      event->type = XV_SERVER_EVENT_ASSISTANT_FINAL;
    }
  } else if (strcmp(fields->type, "reminder") == 0) {
    if (!fields->timer_id_seen || !fields->label_seen ||
        !fields->reminder_kind_seen || !fields->deadline_epoch_seen) {
      return XV_CONTROL_INVALID_MESSAGE;
    }
    event->type = XV_SERVER_EVENT_REMINDER;
  } else if (strcmp(fields->type, "media.command") == 0) {
    if (!fields->command_id_seen || !fields->media_action_seen) {
      return XV_CONTROL_INVALID_MESSAGE;
    }
    if ((strcmp(fields->media_action, "play") == 0 &&
         !fields->media_query_seen) ||
        (strcmp(fields->media_action, "set_volume") == 0 &&
         !fields->volume_percent_seen) ||
        (strcmp(fields->media_action, "seek") == 0 &&
         !fields->position_seconds_seen)) {
      return XV_CONTROL_INVALID_MESSAGE;
    }
    if (strcmp(fields->media_action, "play") != 0 &&
        strcmp(fields->media_action, "pause") != 0 &&
        strcmp(fields->media_action, "resume") != 0 &&
        strcmp(fields->media_action, "stop") != 0 &&
        strcmp(fields->media_action, "next") != 0 &&
        strcmp(fields->media_action, "previous") != 0 &&
        strcmp(fields->media_action, "set_volume") != 0 &&
        strcmp(fields->media_action, "seek") != 0) {
      return XV_CONTROL_INVALID_MESSAGE;
    }
    event->type = XV_SERVER_EVENT_MEDIA_COMMAND;
  } else if (strcmp(fields->type, "display.state") == 0) {
    if (!fields->display_mode_seen || !fields->display_status_seen) {
      return XV_CONTROL_INVALID_MESSAGE;
    }
    event->type = XV_SERVER_EVENT_DISPLAY_STATE;
  } else if (strcmp(fields->type, "tts.start") == 0) {
    if (!fields->turn_id_seen || !fields->stream_id_seen) {
      return XV_CONTROL_INVALID_MESSAGE;
    }
    event->type = XV_SERVER_EVENT_TTS_START;
  } else if (strcmp(fields->type, "tts.end") == 0) {
    if (!fields->turn_id_seen || !fields->stream_id_seen ||
        !fields->samples_seen) {
      return XV_CONTROL_INVALID_MESSAGE;
    }
    event->type = XV_SERVER_EVENT_TTS_END;
  } else if (strcmp(fields->type, "music.end") == 0) {
    if (!fields->stream_id_seen || !fields->samples_seen) {
      return XV_CONTROL_INVALID_MESSAGE;
    }
    event->type = XV_SERVER_EVENT_MUSIC_END;
  } else if (strcmp(fields->type, "error") == 0) {
    if (!fields->recoverable_seen) {
      return XV_CONTROL_INVALID_MESSAGE;
    }
    event->type = XV_SERVER_EVENT_ERROR;
  } else if (strcmp(fields->type, "turn.cancelled") == 0) {
    if (!fields->turn_id_seen) {
      return XV_CONTROL_INVALID_MESSAGE;
    }
    event->type = XV_SERVER_EVENT_TURN_CANCELLED;
  } else if (strcmp(fields->type, "pong") == 0) {
    if (!fields->nonce_seen) {
      return XV_CONTROL_INVALID_MESSAGE;
    }
    event->type = XV_SERVER_EVENT_PONG;
  } else {
    return XV_CONTROL_UNSUPPORTED_EVENT;
  }
  event->stream_id = fields->stream_id;
  event->samples = fields->samples;
  event->deadline_epoch = fields->deadline_epoch;
  event->volume_percent = fields->volume_percent;
  event->nonce = fields->nonce;
  event->position_seconds = fields->position_seconds;
  event->media_ok = fields->media_ok;
  if (fields->turn_id_seen) {
    memcpy(event->turn_id, fields->turn_id, sizeof(event->turn_id));
  }
  if (fields->text_seen) {
    memcpy(event->text, fields->text, sizeof(event->text));
  }
  if (fields->timer_id_seen) {
    memcpy(event->timer_id, fields->timer_id, sizeof(event->timer_id));
  }
  if (fields->label_seen) {
    memcpy(event->label, fields->label, sizeof(event->label));
  }
  if (fields->reminder_kind_seen) {
    memcpy(event->reminder_kind, fields->reminder_kind,
           sizeof(event->reminder_kind));
  }
  if (fields->media_action_seen) {
    memcpy(event->media_action, fields->media_action,
           sizeof(event->media_action));
  }
  if (fields->media_query_seen) {
    memcpy(event->media_query, fields->media_query,
           sizeof(event->media_query));
  }
  if (fields->command_id_seen) {
    memcpy(event->command_id, fields->command_id,
           sizeof(event->command_id));
  }
  if (fields->error_code_seen) {
    memcpy(event->error_code, fields->error_code,
           sizeof(event->error_code));
  }
  if (fields->display_mode_seen) {
    memcpy(event->display_mode, fields->display_mode,
           sizeof(event->display_mode));
  }
  if (fields->display_status_seen) {
    memcpy(event->display_status, fields->display_status,
           sizeof(event->display_status));
  }
  event->recoverable = fields->recoverable;
  event->has_turn_id = fields->turn_id_seen;
  event->has_text = fields->text_seen;
  event->has_stream_id = fields->stream_id_seen;
  event->has_samples = fields->samples_seen;
  event->has_recoverable = fields->recoverable_seen;
  event->has_timer_id = fields->timer_id_seen;
  event->has_label = fields->label_seen;
  event->has_reminder_kind = fields->reminder_kind_seen;
  event->has_deadline_epoch = fields->deadline_epoch_seen;
  event->has_media_action = fields->media_action_seen;
  event->has_media_query = fields->media_query_seen;
  event->has_volume_percent = fields->volume_percent_seen;
  event->has_position_seconds = fields->position_seconds_seen;
  event->has_command_id = fields->command_id_seen;
  event->has_error_code = fields->error_code_seen;
  event->has_media_ok = fields->media_ok_seen;
  event->has_display_mode = fields->display_mode_seen;
  event->has_display_status = fields->display_status_seen;
  event->has_nonce = fields->nonce_seen;
  return XV_CONTROL_OK;
}

xv_control_result_t xv_control_decode_server_event(
    const char *json, size_t json_size, xv_server_event_t *event) {
  xv_json_parser_t parser;
  xv_decoded_fields_t fields;

  if (json == NULL || json_size == 0U || event == NULL) {
    return XV_CONTROL_BAD_ARGUMENT;
  }
  memset(event, 0, sizeof(*event));
  parser.data = json;
  parser.length = json_size;
  parser.position = 0U;
  memset(&fields, 0, sizeof(fields));
  if (!parse_top_level(&parser, &fields)) {
    return XV_CONTROL_MALFORMED_JSON;
  }
  parser_space(&parser);
  if (parser.position != parser.length) {
    return XV_CONTROL_MALFORMED_JSON;
  }
  return validate_server_fields(&fields, event);
}
