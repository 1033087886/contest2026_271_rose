#include "xiaov/event_queue.h"

bool xv_event_queue_init(xv_event_queue_t *queue, xv_event_t *storage,
                         size_t capacity) {
  if (queue == NULL || storage == NULL || capacity == 0U) {
    return false;
  }
  queue->storage = storage;
  queue->capacity = capacity;
  xv_event_queue_reset(queue);
  return true;
}

void xv_event_queue_reset(xv_event_queue_t *queue) {
  if (queue == NULL) {
    return;
  }
  queue->read_index = 0U;
  queue->write_index = 0U;
  queue->size = 0U;
  queue->rejected_events = 0U;
}

bool xv_event_queue_push(xv_event_queue_t *queue, const xv_event_t *event) {
  if (queue == NULL || queue->storage == NULL || event == NULL ||
      queue->size == queue->capacity) {
    if (queue != NULL) {
      queue->rejected_events += 1U;
    }
    return false;
  }
  queue->storage[queue->write_index] = *event;
  queue->write_index = (queue->write_index + 1U) % queue->capacity;
  queue->size += 1U;
  return true;
}

bool xv_event_queue_pop(xv_event_queue_t *queue, xv_event_t *event) {
  if (queue == NULL || queue->storage == NULL || event == NULL ||
      queue->size == 0U) {
    return false;
  }
  *event = queue->storage[queue->read_index];
  queue->read_index = (queue->read_index + 1U) % queue->capacity;
  queue->size -= 1U;
  return true;
}

size_t xv_event_queue_size(const xv_event_queue_t *queue) {
  return queue == NULL ? 0U : queue->size;
}

uint64_t xv_event_queue_rejected(const xv_event_queue_t *queue) {
  return queue == NULL ? 0U : queue->rejected_events;
}
