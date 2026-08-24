/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XIAOV_OPENVELA_POWER_ADAPTER_H
#define XIAOV_OPENVELA_POWER_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XV_POWER_FB_PATH_CAPACITY 64U
#define XV_POWER_BRIGHTNESS_MAX 255U
#define XV_POWER_ADAPTER_STORAGE_SIZE 256U

typedef int (*xv_power_open_fn)(void *context, const char *path);
typedef int (*xv_power_set_brightness_fn)(void *context, int fd,
                                          uint8_t brightness);
typedef int (*xv_power_close_fn)(void *context, int fd);
typedef bool (*xv_power_animation_fn)(void *context, bool enabled);

typedef struct {
  void *context;
  xv_power_open_fn open_fb;
  xv_power_set_brightness_fn set_brightness;
  xv_power_close_fn close_fb;
} xv_power_io_t;

typedef struct {
  const char *fb_path;
  uint8_t active_brightness;
  uint8_t standby_brightness;
  xv_power_animation_fn set_animation_enabled;
  void *animation_context;

  /* Leave all three callbacks NULL to use open/ioctl/close on NuttX. */
  xv_power_io_t io;
} xv_power_config_t;

typedef struct {
  uint64_t transitions;
  uint64_t noops;
  uint64_t open_attempts;
  uint64_t open_failures;
  uint64_t brightness_attempts;
  uint64_t brightness_failures;
  uint64_t animation_attempts;
  uint64_t animation_failures;
  bool state_known;
  bool active;
  bool framebuffer_open;
} xv_power_stats_t;

typedef struct xv_power_adapter xv_power_adapter_t;

typedef union {
  uint64_t alignment;
  uint8_t bytes[XV_POWER_ADAPTER_STORAGE_SIZE];
} xv_power_adapter_storage_t;

/* Initialization remains successful when the framebuffer is not present.
 * The first transition retries it so the service can start before /dev/fb0. */
xv_power_adapter_t *xv_power_init(xv_power_adapter_storage_t *storage,
                                  const xv_power_config_t *config);
void xv_power_deinit(xv_power_adapter_t *adapter);

/* Returns true only when both the requested brightness and animation state
 * were applied. Callers should log false as a capability failure, not turn a
 * voice session into an application error. */
bool xv_power_set_active(xv_power_adapter_t *adapter, bool active);
void xv_power_get_stats(const xv_power_adapter_t *adapter,
                        xv_power_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
