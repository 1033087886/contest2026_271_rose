/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "xiaov_local_wav.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#define XV_WAV_PCM_FORMAT 1U
#define XV_WAV_CHANNELS 1U
#define XV_WAV_BITS_PER_SAMPLE 16U
#define XV_WAV_BLOCK_ALIGN 2U
#define XV_WAV_BYTES_PER_SECOND 32000U

static uint16_t read_u16_le(const uint8_t *bytes) {
  return (uint16_t)((uint16_t)bytes[0] |
                    ((uint16_t)bytes[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static bool fourcc_equals(const uint8_t *bytes, const char *fourcc) {
  return memcmp(bytes, fourcc, 4U) == 0;
}

void xv_local_wav_reset(xv_local_wav_t *reader) {
  if (reader != NULL) {
    memset(reader, 0, sizeof(*reader));
  }
}

void xv_local_wav_close(xv_local_wav_t *reader) {
  if (reader == NULL) {
    return;
  }
  if (reader->file != NULL) {
    (void)fclose(reader->file);
  }
  xv_local_wav_reset(reader);
}

bool xv_local_wav_is_open(const xv_local_wav_t *reader) {
  return reader != NULL && reader->file != NULL;
}

int xv_local_wav_open(xv_local_wav_t *reader, const char *path) {
  uint8_t header[12];
  uint8_t chunk_header[8];
  uint8_t format[16];
  FILE *file;
  long data_offset = 0L;
  long file_size;
  long riff_end;
  uint32_t data_bytes = 0U;
  bool format_found = false;
  bool data_found = false;
  int result = 0;

  if (reader == NULL || path == NULL || path[0] == '\0') {
    return -EINVAL;
  }
  xv_local_wav_close(reader);
  errno = 0;
  file = fopen(path, "rb");
  if (file == NULL) {
    return errno == 0 ? -ENOENT : -errno;
  }
  if (fread(header, 1U, sizeof(header), file) != sizeof(header) ||
      !fourcc_equals(header, "RIFF") ||
      !fourcc_equals(header + 8U, "WAVE")) {
    result = -EINVAL;
    goto fail;
  }
  {
    uint32_t riff_size = read_u32_le(header + 4U);
    uint64_t declared_end = 8U + (uint64_t)riff_size;

    if (riff_size < 4U || declared_end > (uint64_t)LONG_MAX ||
        fseek(file, 0L, SEEK_END) != 0 ||
        (file_size = ftell(file)) < 0L ||
        declared_end > (uint64_t)file_size ||
        fseek(file, (long)sizeof(header), SEEK_SET) != 0) {
      result = -EINVAL;
      goto fail;
    }
    riff_end = (long)declared_end;
  }

  while (!format_found || !data_found) {
    uint32_t chunk_size;
    long chunk_offset;
    long chunk_end;
    long payload_offset;
    uint64_t declared_chunk_end;

    chunk_offset = ftell(file);
    if (chunk_offset < 0L ||
        (uint64_t)chunk_offset + sizeof(chunk_header) >
            (uint64_t)riff_end) {
      result = -EINVAL;
      goto fail;
    }

    if (fread(chunk_header, 1U, sizeof(chunk_header), file) !=
        sizeof(chunk_header)) {
      result = -EINVAL;
      goto fail;
    }
    chunk_size = read_u32_le(chunk_header + 4U);
    payload_offset = ftell(file);
    if (payload_offset < 0L) {
      result = -EIO;
      goto fail;
    }
    declared_chunk_end =
        (uint64_t)payload_offset + chunk_size + (uint64_t)(chunk_size & 1U);
    if (declared_chunk_end > (uint64_t)riff_end ||
        declared_chunk_end > (uint64_t)LONG_MAX) {
      result = -EINVAL;
      goto fail;
    }
    chunk_end = (long)declared_chunk_end;
    if (fourcc_equals(chunk_header, "fmt ")) {
      if (chunk_size < sizeof(format) ||
          fread(format, 1U, sizeof(format), file) != sizeof(format)) {
        result = -EINVAL;
        goto fail;
      }
      if (read_u16_le(format) != XV_WAV_PCM_FORMAT ||
          read_u16_le(format + 2U) != XV_WAV_CHANNELS ||
          read_u32_le(format + 4U) != XV_LOCAL_WAV_SAMPLE_RATE_HZ ||
          read_u32_le(format + 8U) != XV_WAV_BYTES_PER_SECOND ||
          read_u16_le(format + 12U) != XV_WAV_BLOCK_ALIGN ||
          read_u16_le(format + 14U) != XV_WAV_BITS_PER_SAMPLE) {
        result = -ENOTSUP;
        goto fail;
      }
      if (fseek(file, payload_offset, SEEK_SET) != 0) {
        result = -EIO;
        goto fail;
      }
      format_found = true;
    } else if (fourcc_equals(chunk_header, "data")) {
      if (chunk_size == 0U || (chunk_size % XV_WAV_BLOCK_ALIGN) != 0U) {
        result = -EINVAL;
        goto fail;
      }
      data_offset = payload_offset;
      data_bytes = chunk_size;
      data_found = true;
    }
    if (fseek(file, chunk_end, SEEK_SET) != 0) {
      result = -EIO;
      goto fail;
    }
  }

  if (fseek(file, data_offset, SEEK_SET) != 0) {
    result = -EINVAL;
    goto fail;
  }
  reader->file = file;
  reader->data_offset = data_offset;
  reader->data_bytes = data_bytes;
  reader->consumed_bytes = 0U;
  return 0;

fail:
  (void)fclose(file);
  return result;
}

int xv_local_wav_rewind(xv_local_wav_t *reader) {
  if (!xv_local_wav_is_open(reader)) {
    return -EBADF;
  }
  if (fseek(reader->file, reader->data_offset, SEEK_SET) != 0) {
    return -EIO;
  }
  clearerr(reader->file);
  reader->consumed_bytes = 0U;
  return 0;
}

int xv_local_wav_read(xv_local_wav_t *reader, int16_t *samples,
                      size_t capacity, size_t *samples_read, bool *eof) {
  uint8_t bytes[XV_LOCAL_WAV_FRAME_SAMPLES * XV_WAV_BLOCK_ALIGN];
  uint32_t remaining_bytes;
  size_t count;
  size_t index;

  if (!xv_local_wav_is_open(reader) || samples == NULL ||
      samples_read == NULL || eof == NULL || capacity == 0U ||
      capacity > XV_LOCAL_WAV_FRAME_SAMPLES) {
    return -EINVAL;
  }
  *samples_read = 0U;
  *eof = false;
  remaining_bytes = reader->data_bytes - reader->consumed_bytes;
  count = remaining_bytes / XV_WAV_BLOCK_ALIGN;
  if (count > capacity) {
    count = capacity;
  }
  if (count == 0U) {
    *eof = true;
    return 0;
  }
  if (fread(bytes, XV_WAV_BLOCK_ALIGN, count, reader->file) != count) {
    return -EIO;
  }
  for (index = 0U; index < count; ++index) {
    samples[index] = (int16_t)read_u16_le(bytes + index * 2U);
  }
  reader->consumed_bytes += (uint32_t)(count * XV_WAV_BLOCK_ALIGN);
  *samples_read = count;
  *eof = reader->consumed_bytes == reader->data_bytes;
  return 0;
}

static uint32_t bytes_to_ms(uint32_t bytes) {
  return (uint32_t)(((uint64_t)bytes * 1000U) /
                    XV_WAV_BYTES_PER_SECOND);
}

uint32_t xv_local_wav_duration_ms(const xv_local_wav_t *reader) {
  return reader == NULL ? 0U : bytes_to_ms(reader->data_bytes);
}

uint32_t xv_local_wav_position_ms(const xv_local_wav_t *reader) {
  return reader == NULL ? 0U : bytes_to_ms(reader->consumed_bytes);
}
