/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XIAOV_REMINDER_QUEUE_H
#define XIAOV_REMINDER_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XV_REMINDER_TIMER_ID_CAPACITY 65U
#define XV_REMINDER_LABEL_CAPACITY 401U
#define XV_REMINDER_KIND_CAPACITY 32U

typedef struct {
  char timer_id[XV_REMINDER_TIMER_ID_CAPACITY];
  char label[XV_REMINDER_LABEL_CAPACITY];
  char kind[XV_REMINDER_KIND_CAPACITY];
  double deadline_epoch;
} xv_reminder_t;

typedef struct {
  xv_reminder_t *storage;
  size_t capacity;
  size_t read_index;
  size_t write_index;
  size_t size;
  uint64_t rejected;
} xv_reminder_queue_t;

bool xv_reminder_queue_init(xv_reminder_queue_t *queue,
                            xv_reminder_t *storage, size_t capacity);
void xv_reminder_queue_reset(xv_reminder_queue_t *queue);
bool xv_reminder_queue_push(xv_reminder_queue_t *queue,
                            const xv_reminder_t *reminder);
const xv_reminder_t *xv_reminder_queue_peek(
    const xv_reminder_queue_t *queue);
bool xv_reminder_queue_pop(xv_reminder_queue_t *queue,
                           xv_reminder_t *reminder);
size_t xv_reminder_queue_size(const xv_reminder_queue_t *queue);
uint64_t xv_reminder_queue_rejected(const xv_reminder_queue_t *queue);

#ifdef __cplusplus
}
#endif

#endif
