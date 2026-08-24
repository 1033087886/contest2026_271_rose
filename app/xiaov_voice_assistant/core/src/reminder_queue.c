/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "xiaov/reminder_queue.h"

#include <string.h>

static bool terminated(const char *text, size_t capacity, bool nonempty) {
  size_t index;

  if (text == NULL || (nonempty && text[0] == '\0')) {
    return false;
  }
  for (index = 0U; index < capacity; ++index) {
    if (text[index] == '\0') {
      return true;
    }
  }
  return false;
}

static bool reminder_valid(const xv_reminder_t *reminder) {
  return reminder != NULL &&
         terminated(reminder->timer_id, sizeof(reminder->timer_id), true) &&
         terminated(reminder->label, sizeof(reminder->label), true) &&
         terminated(reminder->kind, sizeof(reminder->kind), true);
}

bool xv_reminder_queue_init(xv_reminder_queue_t *queue,
                            xv_reminder_t *storage, size_t capacity) {
  if (queue == NULL || storage == NULL || capacity == 0U) {
    return false;
  }
  queue->storage = storage;
  queue->capacity = capacity;
  xv_reminder_queue_reset(queue);
  return true;
}

void xv_reminder_queue_reset(xv_reminder_queue_t *queue) {
  if (queue == NULL) {
    return;
  }
  queue->read_index = 0U;
  queue->write_index = 0U;
  queue->size = 0U;
  queue->rejected = 0U;
}

bool xv_reminder_queue_push(xv_reminder_queue_t *queue,
                            const xv_reminder_t *reminder) {
  if (queue == NULL || queue->storage == NULL ||
      !reminder_valid(reminder) || queue->size == queue->capacity) {
    if (queue != NULL) {
      queue->rejected += 1U;
    }
    return false;
  }
  queue->storage[queue->write_index] = *reminder;
  queue->write_index = (queue->write_index + 1U) % queue->capacity;
  queue->size += 1U;
  return true;
}

const xv_reminder_t *xv_reminder_queue_peek(
    const xv_reminder_queue_t *queue) {
  if (queue == NULL || queue->storage == NULL || queue->size == 0U) {
    return NULL;
  }
  return &queue->storage[queue->read_index];
}

bool xv_reminder_queue_pop(xv_reminder_queue_t *queue,
                           xv_reminder_t *reminder) {
  if (queue == NULL || queue->storage == NULL || queue->size == 0U) {
    return false;
  }
  if (reminder != NULL) {
    *reminder = queue->storage[queue->read_index];
  }
  memset(&queue->storage[queue->read_index], 0,
         sizeof(queue->storage[queue->read_index]));
  queue->read_index = (queue->read_index + 1U) % queue->capacity;
  queue->size -= 1U;
  return true;
}

size_t xv_reminder_queue_size(const xv_reminder_queue_t *queue) {
  return queue == NULL ? 0U : queue->size;
}

uint64_t xv_reminder_queue_rejected(const xv_reminder_queue_t *queue) {
  return queue == NULL ? 0U : queue->rejected;
}
