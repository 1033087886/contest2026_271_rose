#ifndef XIAOV_EVENT_QUEUE_H
#define XIAOV_EVENT_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint16_t type;
  uint16_t flags;
  uint32_t value;
  uint64_t timestamp_ms;
  void *data;
} xv_event_t;

typedef struct {
  xv_event_t *storage;
  size_t capacity;
  size_t read_index;
  size_t write_index;
  size_t size;
  uint64_t rejected_events;
} xv_event_queue_t;

bool xv_event_queue_init(xv_event_queue_t *queue, xv_event_t *storage,
                         size_t capacity);
void xv_event_queue_reset(xv_event_queue_t *queue);
bool xv_event_queue_push(xv_event_queue_t *queue, const xv_event_t *event);
bool xv_event_queue_pop(xv_event_queue_t *queue, xv_event_t *event);
size_t xv_event_queue_size(const xv_event_queue_t *queue);
uint64_t xv_event_queue_rejected(const xv_event_queue_t *queue);

#ifdef __cplusplus
}
#endif

#endif
