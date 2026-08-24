#ifndef XIAOV_RING_BUFFER_H
#define XIAOV_RING_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int16_t *storage;
  size_t capacity;
  size_t read_index;
  size_t write_index;
  size_t size;
  uint64_t overwritten_samples;
} xv_pcm_ring_t;

bool xv_pcm_ring_init(xv_pcm_ring_t *ring, int16_t *storage, size_t capacity);
void xv_pcm_ring_reset(xv_pcm_ring_t *ring);

/* Keeps the newest audio by overwriting the oldest samples when full. */
size_t xv_pcm_ring_write(xv_pcm_ring_t *ring, const int16_t *samples,
                         size_t count);
size_t xv_pcm_ring_read(xv_pcm_ring_t *ring, int16_t *samples, size_t count);
size_t xv_pcm_ring_available(const xv_pcm_ring_t *ring);
size_t xv_pcm_ring_free(const xv_pcm_ring_t *ring);
uint64_t xv_pcm_ring_overwritten(const xv_pcm_ring_t *ring);

#ifdef __cplusplus
}
#endif

#endif
