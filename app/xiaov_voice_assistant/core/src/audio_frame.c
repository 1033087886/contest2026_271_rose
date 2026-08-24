#include "xiaov/audio_frame.h"

static uint16_t read_u16_be(const uint8_t *source) {
  return (uint16_t)(((uint16_t)source[0] << 8U) | (uint16_t)source[1]);
}

static uint32_t read_u32_be(const uint8_t *source) {
  return ((uint32_t)source[0] << 24U) | ((uint32_t)source[1] << 16U) |
         ((uint32_t)source[2] << 8U) | (uint32_t)source[3];
}

static void write_u16_be(uint8_t *destination, uint16_t value) {
  destination[0] = (uint8_t)(value >> 8U);
  destination[1] = (uint8_t)value;
}

static void write_u32_be(uint8_t *destination, uint32_t value) {
  destination[0] = (uint8_t)(value >> 24U);
  destination[1] = (uint8_t)(value >> 16U);
  destination[2] = (uint8_t)(value >> 8U);
  destination[3] = (uint8_t)value;
}

static bool valid_kind(uint8_t kind) {
  return kind == (uint8_t)XV_AUDIO_KIND_MICROPHONE ||
         kind == (uint8_t)XV_AUDIO_KIND_TTS ||
         kind == (uint8_t)XV_AUDIO_KIND_VIDEO_RGB565 ||
         kind == (uint8_t)XV_AUDIO_KIND_MUSIC;
}

bool xv_audio_header_encode(uint8_t *destination, size_t destination_size,
                            const xv_audio_header_t *header) {
  if (destination == NULL || header == NULL ||
      destination_size < XV_AUDIO_HEADER_SIZE || !valid_kind(header->kind) ||
      header->payload_length > XV_AUDIO_MAX_PAYLOAD) {
    return false;
  }
  destination[0] = (uint8_t)'X';
  destination[1] = (uint8_t)'V';
  destination[2] = (uint8_t)'A';
  destination[3] = (uint8_t)'F';
  destination[4] = XV_AUDIO_PROTOCOL_VERSION;
  destination[5] = header->kind;
  write_u16_be(&destination[6], header->flags);
  write_u32_be(&destination[8], header->stream_id);
  write_u32_be(&destination[12], header->sequence);
  write_u32_be(&destination[16], header->timestamp_ms);
  write_u32_be(&destination[20], header->payload_length);
  return true;
}

bool xv_audio_frame_decode(const uint8_t *frame, size_t frame_size,
                           xv_audio_header_t *header,
                           const uint8_t **payload) {
  uint32_t payload_length;
  if (frame == NULL || header == NULL || payload == NULL ||
      frame_size < XV_AUDIO_HEADER_SIZE || frame[0] != (uint8_t)'X' ||
      frame[1] != (uint8_t)'V' || frame[2] != (uint8_t)'A' ||
      frame[3] != (uint8_t)'F' || frame[4] != XV_AUDIO_PROTOCOL_VERSION ||
      !valid_kind(frame[5])) {
    return false;
  }
  payload_length = read_u32_be(&frame[20]);
  if (payload_length > XV_AUDIO_MAX_PAYLOAD ||
      frame_size != XV_AUDIO_HEADER_SIZE + (size_t)payload_length) {
    return false;
  }
  header->kind = frame[5];
  header->flags = read_u16_be(&frame[6]);
  header->stream_id = read_u32_be(&frame[8]);
  header->sequence = read_u32_be(&frame[12]);
  header->timestamp_ms = read_u32_be(&frame[16]);
  header->payload_length = payload_length;
  *payload = &frame[XV_AUDIO_HEADER_SIZE];
  return true;
}
