#ifndef XIAOV_AUDIO_FRAME_H
#define XIAOV_AUDIO_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XV_AUDIO_HEADER_SIZE 24U
#define XV_AUDIO_MAX_PAYLOAD 32768U
#define XV_AUDIO_PROTOCOL_VERSION 1U

typedef enum {
  XV_AUDIO_KIND_MICROPHONE = 1,
  XV_AUDIO_KIND_TTS = 2,
  XV_AUDIO_KIND_VIDEO_RGB565 = 3,
  XV_AUDIO_KIND_MUSIC = 4
} xv_audio_kind_t;

typedef struct {
  uint8_t kind;
  uint16_t flags;
  uint32_t stream_id;
  uint32_t sequence;
  uint32_t timestamp_ms;
  uint32_t payload_length;
} xv_audio_header_t;

bool xv_audio_header_encode(uint8_t *destination, size_t destination_size,
                            const xv_audio_header_t *header);
bool xv_audio_frame_decode(const uint8_t *frame, size_t frame_size,
                           xv_audio_header_t *header,
                           const uint8_t **payload);

#ifdef __cplusplus
}
#endif

#endif
