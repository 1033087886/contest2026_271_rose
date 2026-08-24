#include "xiaov/ring_buffer.h"

bool xv_pcm_ring_init(xv_pcm_ring_t *ring, int16_t *storage, size_t capacity) {
  if (ring == NULL || storage == NULL || capacity == 0U) {
    return false;
  }
  ring->storage = storage;
  ring->capacity = capacity;
  xv_pcm_ring_reset(ring);
  return true;
}

void xv_pcm_ring_reset(xv_pcm_ring_t *ring) {
  if (ring == NULL) {
    return;
  }
  ring->read_index = 0U;
  ring->write_index = 0U;
  ring->size = 0U;
  ring->overwritten_samples = 0U;
}

size_t xv_pcm_ring_write(xv_pcm_ring_t *ring, const int16_t *samples,
                         size_t count) {
  size_t index;
  if (ring == NULL || ring->storage == NULL || samples == NULL ||
      ring->capacity == 0U) {
    return 0U;
  }

  for (index = 0U; index < count; ++index) {
    ring->storage[ring->write_index] = samples[index];
    ring->write_index = (ring->write_index + 1U) % ring->capacity;
    if (ring->size == ring->capacity) {
      ring->read_index = ring->write_index;
      ring->overwritten_samples += 1U;
    } else {
      ring->size += 1U;
    }
  }
  return count;
}

size_t xv_pcm_ring_read(xv_pcm_ring_t *ring, int16_t *samples, size_t count) {
  size_t index;
  size_t readable;
  if (ring == NULL || ring->storage == NULL || samples == NULL) {
    return 0U;
  }

  readable = count < ring->size ? count : ring->size;
  for (index = 0U; index < readable; ++index) {
    samples[index] = ring->storage[ring->read_index];
    ring->read_index = (ring->read_index + 1U) % ring->capacity;
  }
  ring->size -= readable;
  return readable;
}

size_t xv_pcm_ring_available(const xv_pcm_ring_t *ring) {
  return ring == NULL ? 0U : ring->size;
}

size_t xv_pcm_ring_free(const xv_pcm_ring_t *ring) {
  return ring == NULL ? 0U : ring->capacity - ring->size;
}

uint64_t xv_pcm_ring_overwritten(const xv_pcm_ring_t *ring) {
  return ring == NULL ? 0U : ring->overwritten_samples;
}
