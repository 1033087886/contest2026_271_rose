/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifdef __NuttX__
#  include <nuttx/config.h>
#  include <nuttx/video/fb.h>
#  include <fcntl.h>
#  include <sys/ioctl.h>
#  include <unistd.h>
#endif

#include "xiaov_power_adapter.h"

#include <errno.h>
#include <string.h>

struct xv_power_adapter {
  xv_power_config_t config;
  xv_power_stats_t stats;
  char fb_path[XV_POWER_FB_PATH_CAPACITY];
  int fd;
};

_Static_assert(sizeof(struct xv_power_adapter) <=
                   XV_POWER_ADAPTER_STORAGE_SIZE,
               "XV_POWER_ADAPTER_STORAGE_SIZE is too small");

static void increment_saturated(uint64_t *value) {
  if (*value != UINT64_MAX) {
    *value += 1U;
  }
}

static bool bounded_copy(char *destination, size_t capacity,
                         const char *source) {
  size_t index;

  if (destination == NULL || capacity == 0U || source == NULL) {
    return false;
  }
  for (index = 0U; index < capacity; ++index) {
    destination[index] = source[index];
    if (source[index] == '\0') {
      return index > 0U;
    }
  }
  destination[0] = '\0';
  return false;
}

static int system_open(void *context, const char *path) {
  (void)context;
#ifdef __NuttX__
  return open(path, O_RDWR);
#else
  (void)path;
  errno = ENOTSUP;
  return -1;
#endif
}

static int system_set_brightness(void *context, int fd,
                                 uint8_t brightness) {
  (void)context;
#ifdef __NuttX__
  return ioctl(fd, FBIOSET_POWER, (unsigned long)brightness);
#else
  (void)fd;
  (void)brightness;
  errno = ENOTSUP;
  return -1;
#endif
}

static int system_close(void *context, int fd) {
  (void)context;
#ifdef __NuttX__
  return close(fd);
#else
  (void)fd;
  errno = ENOTSUP;
  return -1;
#endif
}

static bool valid_io(const xv_power_io_t *io) {
  bool none = io->open_fb == NULL && io->set_brightness == NULL &&
              io->close_fb == NULL;
  bool complete = io->open_fb != NULL && io->set_brightness != NULL &&
                  io->close_fb != NULL;
  return none || complete;
}

static bool ensure_open(xv_power_adapter_t *adapter) {
  int fd;

  if (adapter->fd >= 0) {
    return true;
  }
  increment_saturated(&adapter->stats.open_attempts);
  fd = adapter->config.io.open_fb(adapter->config.io.context,
                                  adapter->fb_path);
  if (fd < 0) {
    increment_saturated(&adapter->stats.open_failures);
    adapter->stats.framebuffer_open = false;
    return false;
  }
  adapter->fd = fd;
  adapter->stats.framebuffer_open = true;
  return true;
}

static bool apply_brightness(xv_power_adapter_t *adapter,
                             uint8_t brightness) {
  increment_saturated(&adapter->stats.brightness_attempts);
  if (!ensure_open(adapter) ||
      adapter->config.io.set_brightness(adapter->config.io.context,
                                        adapter->fd, brightness) < 0) {
    increment_saturated(&adapter->stats.brightness_failures);
    return false;
  }
  return true;
}

static bool apply_animation(xv_power_adapter_t *adapter, bool enabled) {
  if (adapter->config.set_animation_enabled == NULL) {
    return true;
  }
  increment_saturated(&adapter->stats.animation_attempts);
  if (!adapter->config.set_animation_enabled(
          adapter->config.animation_context, enabled)) {
    increment_saturated(&adapter->stats.animation_failures);
    return false;
  }
  return true;
}

xv_power_adapter_t *xv_power_init(xv_power_adapter_storage_t *storage,
                                  const xv_power_config_t *config) {
  xv_power_adapter_t *adapter;

  if (storage == NULL || config == NULL || !valid_io(&config->io)) {
    return NULL;
  }
  memset(storage, 0, sizeof(*storage));
  adapter = (xv_power_adapter_t *)storage->bytes;
  adapter->fd = -1;
  adapter->config = *config;
  if (!bounded_copy(adapter->fb_path, sizeof(adapter->fb_path),
                    config->fb_path)) {
    return NULL;
  }
  adapter->config.fb_path = adapter->fb_path;
  if (adapter->config.io.open_fb == NULL) {
    adapter->config.io.open_fb = system_open;
    adapter->config.io.set_brightness = system_set_brightness;
    adapter->config.io.close_fb = system_close;
  }
  (void)ensure_open(adapter);
  return adapter;
}

void xv_power_deinit(xv_power_adapter_t *adapter) {
  if (adapter == NULL) {
    return;
  }
  if (adapter->fd >= 0) {
    (void)adapter->config.io.close_fb(adapter->config.io.context,
                                      adapter->fd);
    adapter->fd = -1;
    adapter->stats.framebuffer_open = false;
  }
}

bool xv_power_set_active(xv_power_adapter_t *adapter, bool active) {
  bool brightness_ok;
  bool animation_ok;
  bool applied;

  if (adapter == NULL) {
    return false;
  }
  if (adapter->stats.state_known && adapter->stats.active == active) {
    increment_saturated(&adapter->stats.noops);
    return true;
  }

  if (active) {
    brightness_ok = apply_brightness(adapter,
                                     adapter->config.active_brightness);
    animation_ok = apply_animation(adapter, true);
  } else {
    animation_ok = apply_animation(adapter, false);
    brightness_ok = apply_brightness(adapter,
                                     adapter->config.standby_brightness);
  }
  applied = brightness_ok && animation_ok;
  adapter->stats.state_known = applied;
  adapter->stats.active = active;
  increment_saturated(&adapter->stats.transitions);
  return applied;
}

void xv_power_get_stats(const xv_power_adapter_t *adapter,
                        xv_power_stats_t *stats) {
  if (adapter == NULL || stats == NULL) {
    return;
  }
  *stats = adapter->stats;
}
