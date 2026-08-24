/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifdef __NuttX__
#  include <nuttx/config.h>
#endif

#include "xiaov_ui.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_EXAMPLES_XIAOV_LVGL
#  include <lvgl/lvgl.h>
#  include "assets/xiaov_cjk_font.h"
#endif

#define XV_UI_EVENT_CAPACITY 12U
#define XV_UI_EVENT_TEXT_CAPACITY XV_CONVERSATION_ASR_CAPACITY
#define XV_UI_MESSAGE_CAPACITY 6U
#define XV_UI_SPANS_PER_MESSAGE 24U
#define XV_UI_SPAN_TEXT_CAPACITY \
  (XV_STREAM_MARKDOWN_OUTPUT_CAPACITY + 1U)
#define XV_UI_VIDEO_WIDTH 320U
#define XV_UI_VIDEO_HEIGHT 240U
#define XV_UI_VIDEO_FRAME_BYTES \
  (XV_UI_VIDEO_WIDTH * XV_UI_VIDEO_HEIGHT * 2U)

#ifndef CONFIG_EXAMPLES_XIAOV_UI_WORKER_STACKSIZE
#  define CONFIG_EXAMPLES_XIAOV_UI_WORKER_STACKSIZE 36864
#endif

#ifndef CONFIG_EXAMPLES_XIAOV_INPUT_DEVPATH
#  define CONFIG_EXAMPLES_XIAOV_INPUT_DEVPATH "/dev/input0"
#endif

typedef enum {
  XV_UI_EVENT_STATE = 0,
  XV_UI_EVENT_TEXT,
  XV_UI_EVENT_REMINDER,
  XV_UI_EVENT_MEDIA_COMMAND,
  XV_UI_EVENT_MUSIC_STATE,
  XV_UI_EVENT_LOW_POWER,
  XV_UI_EVENT_DISPLAY_STATE,
  XV_UI_EVENT_ERROR
} xv_ui_event_type_t;

typedef struct {
  xv_ui_event_type_t type;
  xv_conversation_text_kind_t text_kind;
  xv_app_state_t state;
  bool active;
  xv_music_snapshot_t music;
  char turn_id[XV_CONVERSATION_TURN_ID_CAPACITY];
  char text[XV_UI_EVENT_TEXT_CAPACITY];
} xv_ui_event_t;

#ifdef CONFIG_EXAMPLES_XIAOV_LVGL
typedef struct {
  struct xv_ui *ui;
  xv_music_action_t action;
  uint32_t value;
} xv_ui_music_button_t;

typedef enum {
  XV_UI_PAGE_CONVERSATION = 0,
  XV_UI_PAGE_CONTROL,
  XV_UI_PAGE_MONITOR,
  XV_UI_PAGE_MEDIA,
  XV_UI_PAGE_COUNT
} xv_ui_page_t;

typedef struct {
  struct xv_ui *ui;
  xv_ui_page_t page;
} xv_ui_nav_button_t;
#endif

#ifdef CONFIG_EXAMPLES_XIAOV_LVGL
typedef struct {
  lv_obj_t *container;
  lv_obj_t *role;
  lv_obj_t *body;
  lv_obj_t *spans[XV_UI_SPANS_PER_MESSAGE];
  char text[XV_UI_SPANS_PER_MESSAGE][XV_UI_SPAN_TEXT_CAPACITY];
  char asr_text[XV_UI_EVENT_TEXT_CAPACITY];
  xv_conversation_style_t styles[XV_UI_SPANS_PER_MESSAGE];
  size_t span_count;
} xv_ui_message_t;
#endif

struct xv_ui {
  pthread_mutex_t lock;
  sem_t wake;
  sem_t ready_wake;
  pthread_t thread;
  xv_ui_callbacks_t callbacks;
  xv_conversation_model_t conversation;
  xv_ui_event_t events[XV_UI_EVENT_CAPACITY];
  size_t read_index;
  size_t write_index;
  size_t event_count;
  uint64_t queue_rejected;
  uint64_t render_rejected;
  bool lock_initialized;
  bool wake_initialized;
  bool ready_wake_initialized;
  bool thread_started;
  bool running;
  bool ready;
  bool graphical;
  bool interactive;
  bool input_ready;
  uint8_t audio_levels[8];
  uint8_t mouth_level;
  bool audio_levels_valid;
  uint8_t video_display_index;
  uint8_t video_pending_index;
  bool video_pending;
  uint32_t video_pending_sequence;
  uint32_t video_pending_timestamp_ms;
  uint64_t video_frames;
  uint64_t video_replaced;
#ifdef CONFIG_EXAMPLES_XIAOV_LVGL
  lv_nuttx_result_t lv_result;
  lv_obj_t *status_text;
  lv_obj_t *queue_text;
  lv_obj_t *page_host;
  lv_obj_t *pages[XV_UI_PAGE_COUNT];
  lv_obj_t *nav_buttons[XV_UI_PAGE_COUNT];
  lv_obj_t *nav_labels[XV_UI_PAGE_COUNT];
  lv_obj_t *conversation_view;
  lv_obj_t *reminder_banner;
  lv_obj_t *talk_button;
  lv_obj_t *talk_button_text;
  lv_obj_t *control_talk_button;
  lv_obj_t *control_talk_button_text;
  lv_obj_t *command_button;
  lv_obj_t *monitor_button;
  lv_obj_t *video_button;
  lv_obj_t *music_title;
  lv_obj_t *music_detail;
  lv_obj_t *face_panel;
  lv_obj_t *state_orb;
  lv_obj_t *face_eyes[2];
  lv_obj_t *face_mouth;
  lv_obj_t *spectrum[8];
  lv_timer_t *animation_timer;
  lv_obj_t *video_layer;
  lv_obj_t *video_image;
  lv_obj_t *video_status;
  lv_obj_t *video_exit;
  lv_image_dsc_t video_descriptors[2];
  xv_ui_music_button_t music_buttons[8];
  xv_ui_nav_button_t nav_button_data[XV_UI_PAGE_COUNT];
  char queue_status[40];
  char banner_text[XV_UI_EVENT_TEXT_CAPACITY];
  xv_ui_message_t messages[XV_UI_MESSAGE_CAPACITY];
  size_t next_message;
  size_t message_count;
  int current_user;
  int current_assistant;
  bool lv_owned;
  bool reminder_visible;
  bool error_visible;
  bool low_power;
  bool music_visual_active;
  xv_app_state_t visual_state;
  xv_ui_page_t current_page;
  uint32_t animation_step;
  bool video_visible;
  bool video_monitor_mode;
#endif
};

static uint8_t g_video_buffers[2][XV_UI_VIDEO_FRAME_BYTES]
    __attribute__((aligned(64)));

_Static_assert(sizeof(struct xv_ui) <= XV_UI_STORAGE_SIZE,
               "XV_UI_STORAGE_SIZE is too small");

static bool bounded_copy(char *destination, size_t capacity,
                         const char *source) {
  size_t index;

  if (destination == NULL || capacity == 0U || source == NULL) {
    return false;
  }
  for (index = 0U; index < capacity; ++index) {
    destination[index] = source[index];
    if (source[index] == '\0') {
      return true;
    }
  }
  destination[0] = '\0';
  return false;
}

static bool ui_running(xv_ui_t *ui) {
  bool running;

  pthread_mutex_lock(&ui->lock);
  running = ui->running;
  pthread_mutex_unlock(&ui->lock);
  return running;
}

static bool ui_push(xv_ui_t *ui, const xv_ui_event_t *event) {
  bool accepted = false;
  uint64_t rejected;

  if (ui == NULL || event == NULL || !ui->lock_initialized) {
    return false;
  }
  pthread_mutex_lock(&ui->lock);
  if (ui->running && ui->event_count < XV_UI_EVENT_CAPACITY) {
    ui->events[ui->write_index] = *event;
    ui->write_index = (ui->write_index + 1U) % XV_UI_EVENT_CAPACITY;
    ui->event_count += 1U;
    accepted = true;
  } else {
    ui->queue_rejected += 1U;
  }
  rejected = ui->queue_rejected;
  pthread_mutex_unlock(&ui->lock);
  if (accepted) {
    sem_post(&ui->wake);
  } else {
    printf("xiaov: UI event rejected type=%u total=%llu\n",
           (unsigned int)event->type, (unsigned long long)rejected);
  }
  return accepted;
}

static bool ui_pop(xv_ui_t *ui, xv_ui_event_t *event) {
  bool available = false;

  pthread_mutex_lock(&ui->lock);
  if (ui->event_count > 0U) {
    *event = ui->events[ui->read_index];
    ui->read_index = (ui->read_index + 1U) % XV_UI_EVENT_CAPACITY;
    ui->event_count -= 1U;
    available = true;
  }
  pthread_mutex_unlock(&ui->lock);
  return available;
}

static void wait_for_event(xv_ui_t *ui, uint32_t milliseconds) {
  struct timespec deadline;
  int result;

  if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
    return;
  }
  deadline.tv_sec += (time_t)(milliseconds / 1000U);
  deadline.tv_nsec += (long)(milliseconds % 1000U) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec += 1;
    deadline.tv_nsec -= 1000000000L;
  }
  do {
    result = sem_timedwait(&ui->wake, &deadline);
  } while (result < 0 && errno == EINTR);
}

#ifdef CONFIG_EXAMPLES_XIAOV_LVGL
static const char *state_label(xv_app_state_t state) {
  switch (state) {
  case XV_STATE_STANDBY:
    return "待机";
  case XV_STATE_LISTENING:
    return "聆听中";
  case XV_STATE_THINKING:
    return "思考中";
  case XV_STATE_SPEAKING:
    return "说话中";
  case XV_STATE_ALERTING:
    return "提醒";
  case XV_STATE_ERROR:
    return "出错了";
  }
  return "未知";
}

static void style_span(lv_obj_t *label, xv_conversation_style_t style) {
  lv_obj_set_style_text_color(label, lv_color_hex(0xf2f4f8), 0);
  lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(label, 0, 0);
  lv_obj_set_style_radius(label, 6, 0);
  if (style == XV_CONVERSATION_STYLE_BOLD) {
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_color(label, lv_color_hex(0x164a7b), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(label, 3, 0);
  } else if (style == XV_CONVERSATION_STYLE_CODE) {
    lv_obj_set_style_text_color(label, lv_color_hex(0x8dbbff), 0);
    lv_obj_set_style_bg_color(label, lv_color_hex(0x1b212b), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(label, 4, 0);
  } else if (style == XV_CONVERSATION_STYLE_UNORDERED_MARKER ||
             style == XV_CONVERSATION_STYLE_ORDERED_MARKER) {
    lv_obj_set_style_text_color(label, lv_color_hex(0x0a84ff), 0);
  }
}

static void reset_message(xv_ui_message_t *message) {
  size_t index;

  message->span_count = 0U;
  message->asr_text[0] = '\0';
  for (index = 0U; index < XV_UI_SPANS_PER_MESSAGE; ++index) {
    message->text[index][0] = '\0';
    message->styles[index] = XV_CONVERSATION_STYLE_TEXT;
    lv_obj_add_flag(message->spans[index], LV_OBJ_FLAG_HIDDEN);
  }
}

static int acquire_message(xv_ui_t *ui, const char *role, bool assistant) {
  size_t slot = ui->next_message;
  xv_ui_message_t *message = &ui->messages[slot];

  ui->next_message = (ui->next_message + 1U) % XV_UI_MESSAGE_CAPACITY;
  if (ui->message_count < XV_UI_MESSAGE_CAPACITY) {
    ui->message_count += 1U;
  }
  reset_message(message);
  lv_label_set_text_static(message->role, role);
  lv_obj_set_style_text_color(
      message->role,
      assistant ? lv_color_hex(0x64d2ff) : lv_color_hex(0x0a84ff), 0);
  lv_obj_remove_flag(message->container, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(message->container);
  lv_obj_scroll_to_view(message->container, LV_ANIM_OFF);
  return (int)slot;
}

static bool append_span(xv_ui_t *ui, int message_index,
                        xv_conversation_style_t style,
                        const uint8_t *text, size_t length) {
  xv_ui_message_t *message;
  size_t slot;

  if (message_index < 0 || (size_t)message_index >= XV_UI_MESSAGE_CAPACITY ||
      text == NULL || length == 0U ||
      length >= XV_UI_SPAN_TEXT_CAPACITY) {
    return length == 0U;
  }
  message = &ui->messages[message_index];
  if (message->span_count > 0U) {
    size_t previous = message->span_count - 1U;
    size_t used = strlen(message->text[previous]);
    if (message->styles[previous] == style &&
        length < XV_UI_SPAN_TEXT_CAPACITY - used) {
      memcpy(message->text[previous] + used, text, length);
      message->text[previous][used + length] = '\0';
      lv_label_set_text_static(message->spans[previous],
                               message->text[previous]);
      return true;
    }
  }
  if (message->span_count == XV_UI_SPANS_PER_MESSAGE) {
    return false;
  }
  slot = message->span_count++;
  memcpy(message->text[slot], text, length);
  message->text[slot][length] = '\0';
  message->styles[slot] = style;
  style_span(message->spans[slot], style);
  lv_label_set_text_static(message->spans[slot], message->text[slot]);
  lv_obj_remove_flag(message->spans[slot], LV_OBJ_FLAG_HIDDEN);
  lv_obj_scroll_to_view(message->container, LV_ANIM_OFF);
  return true;
}

static void refresh_animation(xv_ui_t *ui) {
  bool animate = !ui->low_power &&
                 (ui->visual_state != XV_STATE_STANDBY ||
                  ui->music_visual_active);

  if (ui->animation_timer == NULL) {
    return;
  }
  if (animate) {
    lv_timer_resume(ui->animation_timer);
  } else {
    lv_timer_pause(ui->animation_timer);
  }
}

static void update_expression(xv_ui_t *ui) {
  lv_color_t color = lv_color_hex(0x0a84ff);
  int eye_height = 7;
  int mouth_height = 3;

  if (ui->visual_state == XV_STATE_LISTENING) {
    color = lv_color_hex(0x0a84ff);
    eye_height = 11;
  } else if (ui->visual_state == XV_STATE_THINKING) {
    color = lv_color_hex(0x5e5ce6);
    eye_height = 5;
  } else if (ui->visual_state == XV_STATE_SPEAKING) {
    color = lv_color_hex(0x30d158);
    mouth_height = 8;
  } else if (ui->visual_state == XV_STATE_ALERTING) {
    color = lv_color_hex(0xff9f0a);
    eye_height = 11;
    mouth_height = 10;
  } else if (ui->visual_state == XV_STATE_ERROR) {
    color = lv_color_hex(0xff453a);
    eye_height = 3;
    mouth_height = 2;
  }
  lv_obj_set_style_bg_color(ui->state_orb, color, 0);
  lv_obj_set_style_bg_color(ui->face_panel, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_color(ui->face_eyes[0], lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_color(ui->face_eyes[1], lv_color_hex(0xffffff), 0);
  lv_obj_set_height(ui->face_eyes[0], eye_height);
  lv_obj_set_height(ui->face_eyes[1], eye_height);
  lv_obj_set_style_bg_color(ui->face_mouth, lv_color_hex(0xffffff), 0);
  lv_obj_set_height(ui->face_mouth, mouth_height);
  refresh_animation(ui);
}

static void animation_tick(lv_timer_t *timer) {
  xv_ui_t *ui = lv_timer_get_user_data(timer);
  uint8_t levels[8];
  uint8_t mouth_level;
  size_t index;
  bool levels_valid;
  bool spectrum_active;

  if (ui == NULL || ui->low_power) {
    return;
  }
  pthread_mutex_lock(&ui->lock);
  memcpy(levels, ui->audio_levels, sizeof(levels));
  mouth_level = ui->mouth_level;
  levels_valid = ui->audio_levels_valid;
  pthread_mutex_unlock(&ui->lock);
  ui->animation_step += 1U;
  spectrum_active = ui->visual_state == XV_STATE_LISTENING ||
                    ui->music_visual_active;
  if (ui->visual_state == XV_STATE_SPEAKING) {
    int height = levels_valid ? 3 + (int)mouth_level * 9 / 100
                              : ((ui->animation_step & 1U) == 0U ? 4 : 10);
    lv_obj_set_height(ui->face_mouth, height);
  }
  for (index = 0U; index < sizeof(ui->spectrum) / sizeof(ui->spectrum[0]);
       ++index) {
    if (spectrum_active) {
      int height;
      if (levels_valid) {
        height = 5 + (int)levels[index] * 28 / 100;
      } else if (ui->music_visual_active) {
        /* HTTP/local decoder PCM is not exposed by Media Framework yet. */
        height = 8 + (int)((ui->animation_step + index * 3U) % 3U) * 4;
      } else {
        height = 5;
      }
      lv_obj_set_height(ui->spectrum[index], height);
      lv_obj_remove_flag(ui->spectrum[index], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(ui->spectrum[index], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void set_low_power(xv_ui_t *ui, bool low_power) {
  ui->low_power = low_power;
  if (low_power) {
    lv_obj_add_flag(ui->face_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->state_orb, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(ui->face_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui->state_orb, LV_OBJ_FLAG_HIDDEN);
  }
  refresh_animation(ui);
}

static void update_status(xv_ui_t *ui, xv_app_state_t state) {
  lv_color_t button_color = lv_color_hex(0x0a84ff);
  const char *button_text = "唤醒";

  lv_label_set_text_static(ui->status_text, state_label(state));
  if (state == XV_STATE_LISTENING) {
    button_color = lv_color_hex(0x0a84ff);
    button_text = "完成";
  } else if (state == XV_STATE_THINKING) {
    button_color = lv_color_hex(0x5e5ce6);
    button_text = "重新说";
  } else if (state == XV_STATE_SPEAKING) {
    button_color = lv_color_hex(0x30d158);
    button_text = "打断";
  } else if (state == XV_STATE_ALERTING) {
    button_color = lv_color_hex(0xff9f0a);
    button_text = "关闭";
  } else if (state == XV_STATE_ERROR) {
    button_color = lv_color_hex(0xff453a);
    button_text = "重置";
  }
  lv_obj_set_style_bg_color(ui->talk_button, button_color, 0);
  lv_obj_set_style_bg_color(ui->control_talk_button, button_color, 0);
  lv_label_set_text_static(ui->talk_button_text, button_text);
  lv_label_set_text_static(ui->control_talk_button_text, button_text);
  if (state == XV_STATE_STANDBY &&
      ui->callbacks.request_offline_command != NULL) {
    lv_obj_remove_state(ui->command_button, LV_STATE_DISABLED);
  } else {
    lv_obj_add_state(ui->command_button, LV_STATE_DISABLED);
  }
  ui->visual_state = state;
  update_expression(ui);
  if (state != XV_STATE_ERROR) {
    ui->error_visible = false;
    if (!ui->reminder_visible) {
      lv_obj_add_flag(ui->reminder_banner, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void talk_clicked(lv_event_t *event) {
  xv_ui_t *ui = lv_event_get_user_data(event);
  xv_app_event_t action = XV_APP_EVENT_USER_TRIGGER;

  if (ui->conversation.state == XV_STATE_LISTENING) {
    action = XV_APP_EVENT_END_OF_SPEECH;
  } else if (ui->conversation.state == XV_STATE_ALERTING) {
    action = XV_APP_EVENT_ALARM_ACKNOWLEDGED;
  } else if (ui->conversation.state == XV_STATE_ERROR) {
    action = XV_APP_EVENT_RESET;
  }
  if (ui->callbacks.request_action != NULL) {
    ui->callbacks.request_action(ui->callbacks.context, action);
  }
}

static void command_clicked(lv_event_t *event) {
  xv_ui_t *ui = lv_event_get_user_data(event);

  if (ui->conversation.state == XV_STATE_STANDBY &&
      ui->callbacks.request_offline_command != NULL) {
    ui->callbacks.request_offline_command(ui->callbacks.context);
  }
}

static void set_page(xv_ui_t *ui, xv_ui_page_t page) {
  size_t index;

  if (ui == NULL || page >= XV_UI_PAGE_COUNT) {
    return;
  }
  for (index = 0U; index < XV_UI_PAGE_COUNT; ++index) {
    bool selected = index == (size_t)page;
    if (selected) {
      lv_obj_remove_flag(ui->pages[index], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(ui->pages[index], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_text_color(
        ui->nav_labels[index],
        selected ? lv_color_hex(0x0a84ff) : lv_color_hex(0x8e8e93), 0);
    lv_obj_set_style_bg_color(
        ui->nav_buttons[index],
        selected ? lv_color_hex(0x102a43) : lv_color_hex(0x111318), 0);
  }
  ui->current_page = page;
}

static void nav_clicked(lv_event_t *event) {
  xv_ui_nav_button_t *button = lv_event_get_user_data(event);

  if (button != NULL && button->ui != NULL) {
    set_page(button->ui, button->page);
  }
}

static void monitor_clicked(lv_event_t *event) {
  xv_ui_t *ui = lv_event_get_user_data(event);
  if (ui != NULL && ui->callbacks.request_display != NULL) {
    ui->callbacks.request_display(ui->callbacks.context, "open_monitor");
  }
}

static void video_clicked(lv_event_t *event) {
  xv_ui_t *ui = lv_event_get_user_data(event);
  if (ui != NULL && ui->callbacks.request_display != NULL) {
    ui->callbacks.request_display(ui->callbacks.context, "play_video");
  }
}

static void video_exit_clicked(lv_event_t *event) {
  xv_ui_t *ui = lv_event_get_user_data(event);
  if (ui != NULL && ui->callbacks.request_display != NULL) {
    ui->callbacks.request_display(
        ui->callbacks.context,
        ui->video_monitor_mode ? "close_monitor" : "stop_video");
  }
}

static void music_clicked(lv_event_t *event) {
  xv_ui_music_button_t *button = lv_event_get_user_data(event);

  if (button != NULL && button->ui != NULL &&
      button->ui->callbacks.request_media != NULL) {
    button->ui->callbacks.request_media(button->ui->callbacks.context,
                                        button->action, button->value);
  }
}

static lv_obj_t *add_music_button(xv_ui_t *ui, lv_obj_t *parent,
                                  size_t slot, const char *text,
                                  xv_music_action_t action, uint32_t value) {
  lv_obj_t *button;
  lv_obj_t *label;

  if (slot >= sizeof(ui->music_buttons) / sizeof(ui->music_buttons[0])) {
    return NULL;
  }
  ui->music_buttons[slot].ui = ui;
  ui->music_buttons[slot].action = action;
  ui->music_buttons[slot].value = value;
  button = lv_button_create(parent);
  lv_obj_set_height(button, 32);
  lv_obj_set_flex_grow(button, 1);
  lv_obj_set_style_radius(button, 7, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x2c2c2e), 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_add_event_cb(button, music_clicked, LV_EVENT_CLICKED,
                      &ui->music_buttons[slot]);
  label = lv_label_create(button);
  lv_label_set_text_static(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(0xf5f5f7), 0);
  lv_obj_center(label);
  return button;
}

static void update_music(xv_ui_t *ui, const xv_music_snapshot_t *snapshot) {
  char detail[96];
  const char *title = snapshot->has_current ? snapshot->title : "No track";

  lv_label_set_text(ui->music_title, title);
  (void)snprintf(detail, sizeof(detail), "%s  %u:%02u / %u:%02u  %u%%",
                 xv_music_output_name(snapshot->output),
                 (unsigned int)(snapshot->position_ms / 60000U),
                 (unsigned int)((snapshot->position_ms / 1000U) % 60U),
                 (unsigned int)(snapshot->duration_ms / 60000U),
                 (unsigned int)((snapshot->duration_ms / 1000U) % 60U),
                 (unsigned int)snapshot->volume_percent);
  lv_label_set_text(ui->music_detail, detail);
  ui->music_visual_active = snapshot->has_current &&
                            (snapshot->output == XV_MUSIC_OUTPUT_PLAYING ||
                             snapshot->output == XV_MUSIC_OUTPUT_DUCKED);
  refresh_animation(ui);
}

static bool build_graphical_ui(xv_ui_t *ui) {
  lv_nuttx_dsc_t descriptor;
#ifdef CONFIG_INPUT_TOUCHSCREEN
  int input_fd;
#endif
  lv_obj_t *screen;
  lv_obj_t *status_bar;
  lv_obj_t *brand;
  lv_obj_t *music_panel;
  lv_obj_t *music_controls;
  lv_obj_t *music_seek;
  lv_obj_t *action_controls;
  lv_obj_t *control_header;
  lv_obj_t *control_grid;
  lv_obj_t *control_tile;
  lv_obj_t *control_label;
  lv_obj_t *monitor_header;
  lv_obj_t *monitor_preview;
  lv_obj_t *monitor_hint;
  lv_obj_t *monitor_controls;
  lv_obj_t *media_header;
  lv_obj_t *nav_bar;
  lv_obj_t *command_label;
  lv_obj_t *monitor_label;
  lv_obj_t *video_label;
  lv_obj_t *video_exit_label;
  static const char *const nav_text[XV_UI_PAGE_COUNT] = {
      "对话", "控制", "监控", "媒体"};
  size_t message_index;
  size_t span_index;
  size_t nav_index;

  if (lv_is_initialized()) {
    printf("xiaov: LVGL is already owned by another application; using serial UI\n");
    return false;
  }
  lv_init();
  ui->lv_owned = true;
  lv_nuttx_dsc_init(&descriptor);
#ifdef CONFIG_INPUT_TOUCHSCREEN
  descriptor.input_path = CONFIG_EXAMPLES_XIAOV_INPUT_DEVPATH;
  input_fd = open(descriptor.input_path, O_RDONLY | O_NONBLOCK);
  if (input_fd >= 0) {
    close(input_fd);
    ui->input_ready = true;
    printf("xiaov: touch probe ready path=%s\n", descriptor.input_path);
  } else {
    printf("xiaov: touch probe unavailable path=%s errno=%d\n",
           descriptor.input_path, errno);
  }
#endif
  lv_nuttx_init(&descriptor, &ui->lv_result);
  if (ui->lv_result.disp == NULL) {
    printf("xiaov: LVGL display initialization failed; using serial UI\n");
    lv_nuttx_deinit(&ui->lv_result);
    lv_deinit();
    ui->lv_owned = false;
    return false;
  }

  screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
  lv_obj_set_style_text_color(screen, lv_color_hex(0xf5f5f7), 0);
  lv_obj_set_style_text_font(screen, &xiaov_cjk_font_16, 0);
  lv_obj_set_style_pad_all(screen, 0, 0);
  lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  status_bar = lv_obj_create(screen);
  lv_obj_set_size(status_bar, LV_PCT(100), 44);
  lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x08090b), 0);
  lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(status_bar, 0, 0);
  lv_obj_set_style_radius(status_bar, 0, 0);
  lv_obj_set_style_pad_hor(status_bar, 14, 0);
  lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);

  brand = lv_label_create(status_bar);
  lv_label_set_text_static(brand, "小 V");
  lv_obj_set_style_text_color(brand, lv_color_hex(0xffffff), 0);
  ui->status_text = lv_label_create(status_bar);
  lv_obj_set_style_text_color(ui->status_text, lv_color_hex(0x0a84ff), 0);
  ui->queue_text = lv_label_create(status_bar);
  ui->queue_status[0] = '\0';
  lv_label_set_text_static(ui->queue_text, ui->queue_status);
  lv_obj_set_style_text_color(ui->queue_text, lv_color_hex(0xff9f0a), 0);

  ui->page_host = lv_obj_create(screen);
  lv_obj_set_width(ui->page_host, LV_PCT(100));
  lv_obj_set_flex_grow(ui->page_host, 1);
  lv_obj_set_style_bg_opa(ui->page_host, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ui->page_host, 0, 0);
  lv_obj_set_style_radius(ui->page_host, 0, 0);
  lv_obj_set_style_pad_all(ui->page_host, 0, 0);
  lv_obj_remove_flag(ui->page_host, LV_OBJ_FLAG_SCROLLABLE);
  for (nav_index = 0U; nav_index < XV_UI_PAGE_COUNT; ++nav_index) {
    ui->pages[nav_index] = lv_obj_create(ui->page_host);
    lv_obj_set_size(ui->pages[nav_index], LV_PCT(100), LV_PCT(100));
    lv_obj_center(ui->pages[nav_index]);
    lv_obj_set_style_bg_color(ui->pages[nav_index], lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ui->pages[nav_index], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui->pages[nav_index], 0, 0);
    lv_obj_set_style_radius(ui->pages[nav_index], 0, 0);
    lv_obj_set_style_pad_all(ui->pages[nav_index], 0, 0);
    lv_obj_set_flex_flow(ui->pages[nav_index], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui->pages[nav_index], LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(ui->pages[nav_index], LV_OBJ_FLAG_SCROLLABLE);
  }

  ui->face_panel = lv_obj_create(ui->pages[XV_UI_PAGE_CONVERSATION]);
  lv_obj_set_size(ui->face_panel, LV_PCT(100), 64);
  lv_obj_set_style_bg_opa(ui->face_panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ui->face_panel, 0, 0);
  lv_obj_set_style_radius(ui->face_panel, 0, 0);
  lv_obj_set_style_pad_all(ui->face_panel, 0, 0);
  lv_obj_remove_flag(ui->face_panel, LV_OBJ_FLAG_SCROLLABLE);
  ui->state_orb = lv_obj_create(ui->face_panel);
  lv_obj_set_size(ui->state_orb, 18, 18);
  lv_obj_align(ui->state_orb, LV_ALIGN_LEFT_MID, 20, 0);
  lv_obj_set_style_border_width(ui->state_orb, 0, 0);
  lv_obj_set_style_radius(ui->state_orb, LV_RADIUS_CIRCLE, 0);
  ui->face_eyes[0] = lv_obj_create(ui->face_panel);
  ui->face_eyes[1] = lv_obj_create(ui->face_panel);
  lv_obj_set_size(ui->face_eyes[0], 10, 7);
  lv_obj_set_size(ui->face_eyes[1], 10, 7);
  lv_obj_set_pos(ui->face_eyes[0], LV_PCT(40), 15);
  lv_obj_set_pos(ui->face_eyes[1], LV_PCT(52), 15);
  lv_obj_set_style_border_width(ui->face_eyes[0], 0, 0);
  lv_obj_set_style_border_width(ui->face_eyes[1], 0, 0);
  lv_obj_set_style_radius(ui->face_eyes[0], 5, 0);
  lv_obj_set_style_radius(ui->face_eyes[1], 5, 0);
  ui->face_mouth = lv_obj_create(ui->face_panel);
  lv_obj_set_size(ui->face_mouth, 24, 3);
  lv_obj_set_pos(ui->face_mouth, LV_PCT(44), 38);
  lv_obj_set_style_border_width(ui->face_mouth, 0, 0);
  lv_obj_set_style_radius(ui->face_mouth, 3, 0);
  for (span_index = 0U;
       span_index < sizeof(ui->spectrum) / sizeof(ui->spectrum[0]);
       ++span_index) {
    ui->spectrum[span_index] = lv_obj_create(ui->face_panel);
    lv_obj_set_size(ui->spectrum[span_index], 4, 8);
    lv_obj_set_pos(ui->spectrum[span_index],
                   LV_PCT(68 + (int)span_index * 3), 28);
    lv_obj_set_style_bg_color(ui->spectrum[span_index],
                               lv_color_hex(0x0a84ff), 0);
    lv_obj_set_style_border_width(ui->spectrum[span_index], 0, 0);
    lv_obj_set_style_radius(ui->spectrum[span_index], 2, 0);
    lv_obj_add_flag(ui->spectrum[span_index], LV_OBJ_FLAG_HIDDEN);
  }
  ui->animation_timer = lv_timer_create(animation_tick, 180U, ui);
  if (ui->animation_timer == NULL) {
    return false;
  }
  lv_timer_pause(ui->animation_timer);

  ui->conversation_view = lv_obj_create(ui->pages[XV_UI_PAGE_CONVERSATION]);
  lv_obj_set_width(ui->conversation_view, LV_PCT(100));
  lv_obj_set_flex_grow(ui->conversation_view, 1);
  lv_obj_set_flex_flow(ui->conversation_view, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_bg_opa(ui->conversation_view, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ui->conversation_view, 0, 0);
  lv_obj_set_style_radius(ui->conversation_view, 0, 0);
  lv_obj_set_style_pad_hor(ui->conversation_view, 12, 0);
  lv_obj_set_style_pad_ver(ui->conversation_view, 8, 0);
  lv_obj_set_style_pad_row(ui->conversation_view, 8, 0);
  lv_obj_set_scroll_dir(ui->conversation_view, LV_DIR_VER);

  for (message_index = 0U; message_index < XV_UI_MESSAGE_CAPACITY;
       ++message_index) {
    xv_ui_message_t *message = &ui->messages[message_index];
    message->container = lv_obj_create(ui->conversation_view);
    lv_obj_set_width(message->container, LV_PCT(100));
    lv_obj_set_height(message->container, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(message->container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_radius(message->container, 8, 0);
    lv_obj_set_style_border_width(message->container, 0, 0);
    lv_obj_set_style_bg_color(message->container, lv_color_hex(0x1c1c1e), 0);
    lv_obj_set_style_bg_opa(message->container, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(message->container, 8, 0);
    lv_obj_remove_flag(message->container, LV_OBJ_FLAG_SCROLLABLE);
    message->role = lv_label_create(message->container);
    message->body = lv_obj_create(message->container);
    lv_obj_set_width(message->body, LV_PCT(100));
    lv_obj_set_height(message->body, LV_SIZE_CONTENT);
    /* A flex-row label is measured at its intrinsic text width, so
     * LV_LABEL_LONG_WRAP cannot constrain a long streaming response. Keep
     * each styled span in a bounded block instead. */
    lv_obj_set_flex_flow(message->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(message->body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(message->body, 0, 0);
    lv_obj_set_style_pad_all(message->body, 0, 0);
    lv_obj_set_style_pad_column(message->body, 0, 0);
    lv_obj_set_style_pad_row(message->body, 2, 0);
    lv_obj_remove_flag(message->body, LV_OBJ_FLAG_SCROLLABLE);
    for (span_index = 0U; span_index < XV_UI_SPANS_PER_MESSAGE;
         ++span_index) {
      message->spans[span_index] = lv_label_create(message->body);
      lv_label_set_long_mode(message->spans[span_index], LV_LABEL_LONG_WRAP);
      lv_obj_set_width(message->spans[span_index], LV_PCT(100));
      lv_obj_add_flag(message->spans[span_index], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(message->container, LV_OBJ_FLAG_HIDDEN);
  }

  ui->reminder_banner = lv_label_create(ui->pages[XV_UI_PAGE_CONVERSATION]);
  lv_obj_set_width(ui->reminder_banner, LV_PCT(100));
  lv_obj_set_style_bg_color(ui->reminder_banner, lv_color_hex(0x382b0d), 0);
  lv_obj_set_style_bg_opa(ui->reminder_banner, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(ui->reminder_banner, lv_color_hex(0xffd60a), 0);
  lv_obj_set_style_pad_all(ui->reminder_banner, 8, 0);
  lv_obj_add_flag(ui->reminder_banner, LV_OBJ_FLAG_HIDDEN);

  action_controls = lv_obj_create(ui->pages[XV_UI_PAGE_CONVERSATION]);
  lv_obj_set_size(action_controls, LV_PCT(86), 62);
  lv_obj_set_flex_flow(action_controls, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_bg_opa(action_controls, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(action_controls, 0, 0);
  lv_obj_set_style_pad_all(action_controls, 0, 0);
  lv_obj_set_style_pad_column(action_controls, 8, 0);
  lv_obj_set_style_margin_bottom(action_controls, 8, 0);
  lv_obj_remove_flag(action_controls, LV_OBJ_FLAG_SCROLLABLE);

  ui->talk_button = lv_button_create(action_controls);
  lv_obj_set_height(ui->talk_button, LV_PCT(100));
  lv_obj_set_flex_grow(ui->talk_button, 3);
  lv_obj_set_style_radius(ui->talk_button, 8, 0);
  lv_obj_add_event_cb(ui->talk_button, talk_clicked, LV_EVENT_CLICKED, ui);
  ui->talk_button_text = lv_label_create(ui->talk_button);
  lv_obj_center(ui->talk_button_text);

  ui->command_button = lv_button_create(action_controls);
  lv_obj_set_height(ui->command_button, LV_PCT(100));
  lv_obj_set_flex_grow(ui->command_button, 2);
  lv_obj_set_style_radius(ui->command_button, 8, 0);
  lv_obj_set_style_bg_color(ui->command_button, lv_color_hex(0x2c2c2e), 0);
  lv_obj_add_event_cb(ui->command_button, command_clicked,
                      LV_EVENT_CLICKED, ui);
  command_label = lv_label_create(ui->command_button);
  lv_label_set_text_static(command_label, "命令");
  lv_obj_center(command_label);

  control_header = lv_label_create(ui->pages[XV_UI_PAGE_CONTROL]);
  lv_label_set_text_static(control_header, "语音控制");
  lv_obj_set_style_text_color(control_header, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_margin_left(control_header, 16, 0);
  lv_obj_set_style_margin_top(control_header, 16, 0);

  control_grid = lv_obj_create(ui->pages[XV_UI_PAGE_CONTROL]);
  lv_obj_set_width(control_grid, LV_PCT(90));
  lv_obj_set_height(control_grid, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(control_grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_bg_opa(control_grid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(control_grid, 0, 0);
  lv_obj_set_style_pad_all(control_grid, 0, 0);
  lv_obj_set_style_pad_row(control_grid, 8, 0);
  lv_obj_set_style_pad_column(control_grid, 8, 0);
  lv_obj_remove_flag(control_grid, LV_OBJ_FLAG_SCROLLABLE);
  for (nav_index = 0U; nav_index < 4U; ++nav_index) {
    static const char *const tile_text[4] = {
        "空调", "灯光", "提醒", "更多设备"};
    control_tile = lv_obj_create(control_grid);
    lv_obj_set_size(control_tile, 136, 70);
    lv_obj_set_style_bg_color(
        control_tile,
        nav_index == 0U ? lv_color_hex(0x0a84ff) : lv_color_hex(0x1c1c1e), 0);
    lv_obj_set_style_bg_opa(control_tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(control_tile, 0, 0);
    lv_obj_set_style_radius(control_tile, 8, 0);
    lv_obj_set_style_pad_all(control_tile, 10, 0);
    lv_obj_remove_flag(control_tile, LV_OBJ_FLAG_SCROLLABLE);
    control_label = lv_label_create(control_tile);
    lv_label_set_text_static(control_label, tile_text[nav_index]);
    lv_label_set_long_mode(control_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(control_label, LV_PCT(100));
    lv_obj_set_style_text_color(control_label, lv_color_hex(0xffffff), 0);
  }
  control_label = lv_label_create(ui->pages[XV_UI_PAGE_CONTROL]);
  lv_label_set_text_static(control_label,
                           "唤醒小 V，然后说出设备和操作。");
  lv_label_set_long_mode(control_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(control_label, LV_PCT(88));
  lv_obj_set_style_text_color(control_label, lv_color_hex(0x8e8e93), 0);

  ui->control_talk_button = lv_button_create(ui->pages[XV_UI_PAGE_CONTROL]);
  lv_obj_set_size(ui->control_talk_button, LV_PCT(88), 58);
  lv_obj_set_style_radius(ui->control_talk_button, 8, 0);
  lv_obj_set_style_margin_bottom(ui->control_talk_button, 10, 0);
  lv_obj_add_event_cb(ui->control_talk_button, talk_clicked,
                      LV_EVENT_CLICKED, ui);
  ui->control_talk_button_text = lv_label_create(ui->control_talk_button);
  lv_obj_center(ui->control_talk_button_text);

  monitor_header = lv_label_create(ui->pages[XV_UI_PAGE_MONITOR]);
  lv_label_set_text_static(monitor_header, "摄像头");
  lv_obj_set_style_text_color(monitor_header, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_margin_left(monitor_header, 16, 0);
  lv_obj_set_style_margin_top(monitor_header, 16, 0);

  monitor_preview = lv_obj_create(ui->pages[XV_UI_PAGE_MONITOR]);
  lv_obj_set_size(monitor_preview, LV_PCT(90), 190);
  lv_obj_set_style_bg_color(monitor_preview, lv_color_hex(0x101216), 0);
  lv_obj_set_style_bg_opa(monitor_preview, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(monitor_preview, 1, 0);
  lv_obj_set_style_border_color(monitor_preview, lv_color_hex(0x2c2c2e), 0);
  lv_obj_set_style_radius(monitor_preview, 8, 0);
  lv_obj_remove_flag(monitor_preview, LV_OBJ_FLAG_SCROLLABLE);
  monitor_hint = lv_label_create(monitor_preview);
  lv_label_set_text_static(monitor_hint, "点击下方按钮进入实时监控");
  lv_obj_set_style_text_color(monitor_hint, lv_color_hex(0x8e8e93), 0);
  lv_obj_center(monitor_hint);

  monitor_controls = lv_obj_create(ui->pages[XV_UI_PAGE_MONITOR]);
  lv_obj_set_size(monitor_controls, LV_PCT(90), 64);
  lv_obj_set_flex_flow(monitor_controls, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_bg_opa(monitor_controls, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(monitor_controls, 0, 0);
  lv_obj_set_style_pad_all(monitor_controls, 0, 0);
  lv_obj_set_style_pad_column(monitor_controls, 8, 0);
  lv_obj_remove_flag(monitor_controls, LV_OBJ_FLAG_SCROLLABLE);
  ui->monitor_button = lv_button_create(monitor_controls);
  lv_obj_set_height(ui->monitor_button, LV_PCT(100));
  lv_obj_set_flex_grow(ui->monitor_button, 1);
  lv_obj_set_style_radius(ui->monitor_button, 8, 0);
  lv_obj_set_style_bg_color(ui->monitor_button, lv_color_hex(0x0a84ff), 0);
  lv_obj_add_event_cb(ui->monitor_button, monitor_clicked,
                      LV_EVENT_CLICKED, ui);
  monitor_label = lv_label_create(ui->monitor_button);
  lv_label_set_text_static(monitor_label, "实时监控");
  lv_obj_center(monitor_label);
  ui->video_button = lv_button_create(monitor_controls);
  lv_obj_set_height(ui->video_button, LV_PCT(100));
  lv_obj_set_flex_grow(ui->video_button, 1);
  lv_obj_set_style_radius(ui->video_button, 8, 0);
  lv_obj_set_style_bg_color(ui->video_button, lv_color_hex(0x2c2c2e), 0);
  lv_obj_add_event_cb(ui->video_button, video_clicked,
                      LV_EVENT_CLICKED, ui);
  video_label = lv_label_create(ui->video_button);
  lv_label_set_text_static(video_label, "播放视频");
  lv_obj_center(video_label);

  media_header = lv_label_create(ui->pages[XV_UI_PAGE_MEDIA]);
  lv_label_set_text_static(media_header, "正在播放");
  lv_obj_set_style_text_color(media_header, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_margin_left(media_header, 16, 0);
  lv_obj_set_style_margin_top(media_header, 16, 0);

  music_panel = lv_obj_create(ui->pages[XV_UI_PAGE_MEDIA]);
  lv_obj_set_width(music_panel, LV_PCT(90));
  lv_obj_set_flex_grow(music_panel, 1);
  lv_obj_set_flex_flow(music_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_radius(music_panel, 8, 0);
  lv_obj_set_style_border_width(music_panel, 0, 0);
  lv_obj_set_style_bg_color(music_panel, lv_color_hex(0x1c1c1e), 0);
  lv_obj_set_style_bg_opa(music_panel, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(music_panel, 12, 0);
  lv_obj_set_style_margin_bottom(music_panel, 12, 0);
  lv_obj_remove_flag(music_panel, LV_OBJ_FLAG_SCROLLABLE);
  ui->music_title = lv_label_create(music_panel);
  lv_label_set_long_mode(ui->music_title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(ui->music_title, LV_PCT(100));
  lv_obj_set_style_text_color(ui->music_title, lv_color_hex(0xffffff), 0);
  ui->music_detail = lv_label_create(music_panel);
  lv_obj_set_style_text_color(ui->music_detail, lv_color_hex(0x8e8e93), 0);
  music_controls = lv_obj_create(music_panel);
  lv_obj_set_width(music_controls, LV_PCT(100));
  lv_obj_set_height(music_controls, 34);
  lv_obj_set_flex_flow(music_controls, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_bg_opa(music_controls, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(music_controls, 0, 0);
  lv_obj_set_style_pad_all(music_controls, 0, 0);
  lv_obj_set_style_pad_column(music_controls, 4, 0);
  (void)add_music_button(ui, music_controls, 0U, "上一首",
                         XV_MUSIC_ACTION_PREVIOUS, 0U);
  (void)add_music_button(ui, music_controls, 1U, "暂停",
                         XV_MUSIC_ACTION_PAUSE, 0U);
  (void)add_music_button(ui, music_controls, 2U, "播放",
                         XV_MUSIC_ACTION_PLAY, 0U);
  (void)add_music_button(ui, music_controls, 3U, "下一首",
                         XV_MUSIC_ACTION_NEXT, 0U);
  (void)add_music_button(ui, music_controls, 4U, "停止",
                         XV_MUSIC_ACTION_STOP, 0U);
  music_seek = lv_obj_create(music_panel);
  lv_obj_set_width(music_seek, LV_PCT(100));
  lv_obj_set_height(music_seek, 34);
  lv_obj_set_flex_flow(music_seek, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_bg_opa(music_seek, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(music_seek, 0, 0);
  lv_obj_set_style_pad_all(music_seek, 0, 0);
  lv_obj_set_style_pad_column(music_seek, 4, 0);
  (void)add_music_button(ui, music_seek, 5U, "音量 40%",
                         XV_MUSIC_ACTION_SET_VOLUME, 40U);
  (void)add_music_button(ui, music_seek, 6U, "音量 80%",
                         XV_MUSIC_ACTION_SET_VOLUME, 80U);

  nav_bar = lv_obj_create(screen);
  lv_obj_set_size(nav_bar, LV_PCT(100), 56);
  lv_obj_set_flex_flow(nav_bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_bg_color(nav_bar, lv_color_hex(0x08090b), 0);
  lv_obj_set_style_bg_opa(nav_bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(nav_bar, 0, 0);
  lv_obj_set_style_radius(nav_bar, 0, 0);
  lv_obj_set_style_pad_all(nav_bar, 6, 0);
  lv_obj_set_style_pad_column(nav_bar, 5, 0);
  lv_obj_remove_flag(nav_bar, LV_OBJ_FLAG_SCROLLABLE);
  for (nav_index = 0U; nav_index < XV_UI_PAGE_COUNT; ++nav_index) {
    ui->nav_button_data[nav_index].ui = ui;
    ui->nav_button_data[nav_index].page = (xv_ui_page_t)nav_index;
    ui->nav_buttons[nav_index] = lv_button_create(nav_bar);
    lv_obj_set_height(ui->nav_buttons[nav_index], LV_PCT(100));
    lv_obj_set_flex_grow(ui->nav_buttons[nav_index], 1);
    lv_obj_set_style_radius(ui->nav_buttons[nav_index], 7, 0);
    lv_obj_set_style_shadow_width(ui->nav_buttons[nav_index], 0, 0);
    lv_obj_add_event_cb(ui->nav_buttons[nav_index], nav_clicked,
                        LV_EVENT_CLICKED, &ui->nav_button_data[nav_index]);
    ui->nav_labels[nav_index] = lv_label_create(ui->nav_buttons[nav_index]);
    lv_label_set_text_static(ui->nav_labels[nav_index], nav_text[nav_index]);
    lv_obj_center(ui->nav_labels[nav_index]);
  }

  ui->video_layer = lv_obj_create(screen);
  lv_obj_set_size(ui->video_layer, LV_PCT(100), LV_PCT(100));
  lv_obj_center(ui->video_layer);
  lv_obj_add_flag(ui->video_layer, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_style_bg_color(ui->video_layer, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(ui->video_layer, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ui->video_layer, 0, 0);
  lv_obj_set_style_radius(ui->video_layer, 0, 0);
  lv_obj_set_style_pad_all(ui->video_layer, 0, 0);
  lv_obj_remove_flag(ui->video_layer, LV_OBJ_FLAG_SCROLLABLE);
  ui->video_image = lv_image_create(ui->video_layer);
  lv_obj_center(ui->video_image);
  lv_image_set_scale(ui->video_image, LV_SCALE_NONE);
  ui->video_status = lv_label_create(ui->video_layer);
  lv_label_set_text_static(ui->video_status, "Connecting");
  lv_obj_set_style_text_color(ui->video_status, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_color(ui->video_status, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(ui->video_status, LV_OPA_70, 0);
  lv_obj_set_style_pad_all(ui->video_status, 6, 0);
  lv_obj_align(ui->video_status, LV_ALIGN_TOP_LEFT, 10, 10);
  ui->video_exit = lv_button_create(ui->video_layer);
  lv_obj_set_size(ui->video_exit, 44, 44);
  lv_obj_set_style_radius(ui->video_exit, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(ui->video_exit, lv_color_hex(0x2c2c2e), 0);
  lv_obj_align(ui->video_exit, LV_ALIGN_TOP_RIGHT, -10, 10);
  lv_obj_add_event_cb(ui->video_exit, video_exit_clicked,
                      LV_EVENT_CLICKED, ui);
  video_exit_label = lv_label_create(ui->video_exit);
  lv_label_set_text_static(video_exit_label, LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_font(video_exit_label, LV_FONT_DEFAULT, 0);
  lv_obj_center(video_exit_label);
  lv_obj_add_flag(ui->video_layer, LV_OBJ_FLAG_HIDDEN);
  set_page(ui, XV_UI_PAGE_CONVERSATION);
  update_status(ui, XV_STATE_STANDBY);
  {
    xv_music_snapshot_t initial;
    memset(&initial, 0, sizeof(initial));
    initial.output = XV_MUSIC_OUTPUT_STOPPED;
    initial.volume_percent = XV_MUSIC_DEFAULT_VOLUME_PERCENT;
    update_music(ui, &initial);
  }
  return true;
}

static void destroy_graphical_ui(xv_ui_t *ui) {
  if (!ui->lv_owned) {
    return;
  }
  lv_nuttx_deinit(&ui->lv_result);
  lv_deinit();
  ui->lv_owned = false;
}

static bool render_graphical(xv_ui_t *ui,
                             const xv_conversation_op_t *operation) {
  xv_ui_message_t *message;

  switch (operation->type) {
  case XV_CONVERSATION_OP_TURN_BEGIN:
    ui->current_user = -1;
    ui->current_assistant = -1;
    return true;
  case XV_CONVERSATION_OP_ASR_TEXT:
    if (ui->current_user < 0) {
      ui->current_user = acquire_message(ui, "You", false);
    }
    message = &ui->messages[ui->current_user];
    if (operation->text_length >= XV_UI_EVENT_TEXT_CAPACITY) {
      return false;
    }
    reset_message(message);
    memcpy(message->asr_text, operation->text, operation->text_length);
    message->asr_text[operation->text_length] = '\0';
    message->span_count = 1U;
    style_span(message->spans[0], XV_CONVERSATION_STYLE_TEXT);
    lv_label_set_text_static(message->spans[0], message->asr_text);
    lv_obj_remove_flag(message->spans[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view(message->container, LV_ANIM_OFF);
    return true;
  case XV_CONVERSATION_OP_ASSISTANT_BEGIN:
    ui->current_assistant = acquire_message(ui, "Xiao V", true);
    return true;
  case XV_CONVERSATION_OP_ASSISTANT_SPAN:
    return append_span(ui, ui->current_assistant, operation->style,
                       operation->text, operation->text_length);
  case XV_CONVERSATION_OP_ASSISTANT_FINAL:
    if (ui->current_assistant >= 0) {
      lv_obj_scroll_to_view(ui->messages[ui->current_assistant].container,
                            LV_ANIM_OFF);
    }
    return true;
  case XV_CONVERSATION_OP_STATE:
    update_status(ui, operation->state);
    return true;
  }
  return false;
}
#endif

static bool render_serial(const xv_conversation_op_t *operation) {
  switch (operation->type) {
  case XV_CONVERSATION_OP_TURN_BEGIN:
    printf("xiaov-ui: turn %s\n", operation->turn_id);
    break;
  case XV_CONVERSATION_OP_ASR_TEXT:
    printf("xiaov-ui: user%s %.*s\n", operation->final ? " final" : "",
           (int)operation->text_length, (const char *)operation->text);
    break;
  case XV_CONVERSATION_OP_ASSISTANT_BEGIN:
    printf("xiaov-ui: assistant begin\n");
    break;
  case XV_CONVERSATION_OP_ASSISTANT_SPAN:
    printf("xiaov-ui: assistant style=%u %.*s\n",
           (unsigned int)operation->style, (int)operation->text_length,
           (const char *)operation->text);
    break;
  case XV_CONVERSATION_OP_ASSISTANT_FINAL:
    printf("xiaov-ui: assistant final\n");
    break;
  case XV_CONVERSATION_OP_STATE:
    printf("xiaov-ui: state %s\n", xv_state_name(operation->state));
    break;
  }
  return true;
}

static bool conversation_render(void *context,
                                const xv_conversation_op_t *operation) {
  xv_ui_t *ui = context;
#ifdef CONFIG_EXAMPLES_XIAOV_LVGL
  if (ui->graphical) {
    return render_graphical(ui, operation);
  }
#else
  (void)ui;
#endif
  return render_serial(operation);
}

static void show_reminder(xv_ui_t *ui, const char *label, bool active) {
#ifdef CONFIG_EXAMPLES_XIAOV_LVGL
  if (ui->graphical) {
    ui->reminder_visible = active;
    if (active) {
      if (!bounded_copy(ui->banner_text, sizeof(ui->banner_text), label)) {
        return;
      }
      lv_label_set_text_static(ui->reminder_banner, ui->banner_text);
      lv_obj_remove_flag(ui->reminder_banner, LV_OBJ_FLAG_HIDDEN);
    } else if (!ui->error_visible) {
      lv_obj_add_flag(ui->reminder_banner, LV_OBJ_FLAG_HIDDEN);
    }
    return;
  }
#else
  (void)ui;
#endif
  printf("xiaov-ui: reminder %s%s%s\n", active ? "active" : "cleared",
         active ? " label=" : "", active ? label : "");
}

static void show_error(xv_ui_t *ui, const char *message) {
#ifdef CONFIG_EXAMPLES_XIAOV_LVGL
  if (ui->graphical) {
    ui->error_visible = true;
    if (!bounded_copy(ui->banner_text, sizeof(ui->banner_text), message)) {
      return;
    }
    lv_label_set_text_static(ui->reminder_banner, ui->banner_text);
    lv_obj_remove_flag(ui->reminder_banner, LV_OBJ_FLAG_HIDDEN);
    return;
  }
#else
  (void)ui;
#endif
  printf("xiaov-ui: error %s\n", message);
}

static void show_media_command(xv_ui_t *ui, const char *action,
                               const char *query) {
#ifdef CONFIG_EXAMPLES_XIAOV_LVGL
  if (ui->graphical) {
    int index = acquire_message(ui, "System", false);
    xv_ui_message_t *message = &ui->messages[index];
    int length;

    reset_message(message);
    length = snprintf(message->asr_text, sizeof(message->asr_text),
                      "Media backend unavailable: %s", action);
    if (length < 0 || (size_t)length >= sizeof(message->asr_text)) {
      return;
    }
    message->span_count = 1U;
    style_span(message->spans[0], XV_CONVERSATION_STYLE_TEXT);
    lv_label_set_text_static(message->spans[0], message->asr_text);
    lv_obj_set_style_text_color(message->spans[0], lv_color_hex(0xc62828), 0);
    lv_obj_remove_flag(message->spans[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view(message->container, LV_ANIM_OFF);
    return;
  }
#else
  (void)ui;
#endif
  printf("xiaov-ui: media backend unavailable action=%s query=%s\n", action,
         query);
}

static void show_music_state(xv_ui_t *ui,
                             const xv_music_snapshot_t *snapshot) {
#ifdef CONFIG_EXAMPLES_XIAOV_LVGL
  if (ui->graphical) {
    update_music(ui, snapshot);
    return;
  }
#else
  (void)ui;
#endif
  printf("xiaov-ui: music state=%s title=%s position=%u duration=%u volume=%u\n",
         xv_music_output_name(snapshot->output), snapshot->title,
         (unsigned int)snapshot->position_ms,
         (unsigned int)snapshot->duration_ms,
         (unsigned int)snapshot->volume_percent);
}

static void show_low_power(xv_ui_t *ui, bool low_power) {
#ifdef CONFIG_EXAMPLES_XIAOV_LVGL
  if (ui->graphical) {
    set_low_power(ui, low_power);
    return;
  }
#else
  (void)ui;
#endif
  printf("xiaov-ui: low_power=%u\n", low_power ? 1U : 0U);
}

static void show_display_state(xv_ui_t *ui, const char *mode,
                               const char *status) {
#ifdef CONFIG_EXAMPLES_XIAOV_LVGL
  if (ui->graphical) {
    char detail[32];
    if (strcmp(mode, "closed") == 0 || strcmp(status, "closed") == 0) {
      ui->video_visible = false;
      lv_obj_add_flag(ui->video_layer, LV_OBJ_FLAG_HIDDEN);
      return;
    }
    ui->video_visible = true;
    ui->video_monitor_mode = strcmp(mode, "monitor") == 0;
    if (strcmp(status, "streaming") == 0 ||
        strcmp(status, "playing") == 0) {
      (void)snprintf(detail, sizeof(detail), "%s  %ux%u",
                     ui->video_monitor_mode ? "Live" : "Video",
                     XV_UI_VIDEO_WIDTH, XV_UI_VIDEO_HEIGHT);
      lv_label_set_text(ui->video_status, detail);
    } else if (strcmp(status, "error") == 0) {
      lv_label_set_text_static(ui->video_status, "Video unavailable");
    } else {
      lv_label_set_text_static(ui->video_status, "Connecting");
    }
    lv_obj_remove_flag(ui->video_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui->video_layer);
    return;
  }
#else
  (void)ui;
#endif
  printf("xiaov-ui: display mode=%s status=%s\n", mode, status);
}

static void process_event(xv_ui_t *ui, const xv_ui_event_t *event) {
  xv_conversation_result_t result = XV_CONVERSATION_OK;

  if (event->type == XV_UI_EVENT_STATE) {
    result = xv_conversation_set_state(&ui->conversation, event->state);
  } else if (event->type == XV_UI_EVENT_TEXT) {
    result = xv_conversation_push_text(&ui->conversation, event->text_kind,
                                       event->turn_id, event->text);
  } else if (event->type == XV_UI_EVENT_REMINDER) {
    show_reminder(ui, event->text, event->active);
  } else if (event->type == XV_UI_EVENT_MEDIA_COMMAND) {
    show_media_command(ui, event->turn_id, event->text);
  } else if (event->type == XV_UI_EVENT_MUSIC_STATE) {
    show_music_state(ui, &event->music);
  } else if (event->type == XV_UI_EVENT_LOW_POWER) {
    show_low_power(ui, event->active);
  } else if (event->type == XV_UI_EVENT_DISPLAY_STATE) {
    show_display_state(ui, event->turn_id, event->text);
  } else if (event->type == XV_UI_EVENT_ERROR) {
    show_error(ui, event->text);
  }
  if (result != XV_CONVERSATION_OK) {
    uint64_t rejected;
    pthread_mutex_lock(&ui->lock);
    ui->render_rejected += 1U;
    rejected = ui->render_rejected;
    pthread_mutex_unlock(&ui->lock);
    printf("xiaov: UI render rejected result=%u total=%llu\n",
           (unsigned int)result,
           (unsigned long long)rejected);
  }
}

#ifdef CONFIG_EXAMPLES_XIAOV_LVGL
static void refresh_queue_status(xv_ui_t *ui) {
  uint64_t queue_rejected;
  uint64_t render_rejected;

  pthread_mutex_lock(&ui->lock);
  queue_rejected = ui->queue_rejected;
  render_rejected = ui->render_rejected;
  pthread_mutex_unlock(&ui->lock);
  if (queue_rejected == 0U && render_rejected == 0U) {
    ui->queue_status[0] = '\0';
  } else {
    if (queue_rejected > 999U || render_rejected > 999U) {
      snprintf(ui->queue_status, sizeof(ui->queue_status), "!999+");
    } else {
      snprintf(ui->queue_status, sizeof(ui->queue_status), "!%llu/%llu",
               (unsigned long long)queue_rejected,
               (unsigned long long)render_rejected);
    }
  }
  lv_label_set_text_static(ui->queue_text, ui->queue_status);
}

static void refresh_video_frame(xv_ui_t *ui) {
  uint8_t index;
  bool available = false;

  pthread_mutex_lock(&ui->lock);
  if (ui->video_pending) {
    index = ui->video_pending_index;
    ui->video_display_index = index;
    ui->video_pending = false;
    available = true;
  }
  pthread_mutex_unlock(&ui->lock);
  if (!available) {
    return;
  }
  ui->video_descriptors[index].header.magic = LV_IMAGE_HEADER_MAGIC;
  ui->video_descriptors[index].header.cf = LV_COLOR_FORMAT_RGB565;
  ui->video_descriptors[index].header.flags = 0U;
  ui->video_descriptors[index].header.w = XV_UI_VIDEO_WIDTH;
  ui->video_descriptors[index].header.h = XV_UI_VIDEO_HEIGHT;
  ui->video_descriptors[index].header.stride = XV_UI_VIDEO_WIDTH * 2U;
  ui->video_descriptors[index].data_size = XV_UI_VIDEO_FRAME_BYTES;
  ui->video_descriptors[index].data = g_video_buffers[index];
  lv_image_set_src(ui->video_image, &ui->video_descriptors[index]);
  lv_obj_invalidate(ui->video_image);
}
#endif

static void *ui_worker(void *context) {
  xv_ui_t *ui = context;

#ifdef CONFIG_EXAMPLES_XIAOV_LVGL
  ui->graphical = build_graphical_ui(ui);
#  ifdef CONFIG_INPUT_TOUCHSCREEN
  ui->interactive = ui->graphical && ui->input_ready;
#  else
  ui->interactive = false;
#  endif
#else
  printf("xiaov: LVGL disabled; using serial conversation UI\n");
  ui->graphical = false;
  ui->interactive = false;
#endif
  (void)xv_conversation_set_state(&ui->conversation, XV_STATE_STANDBY);
  pthread_mutex_lock(&ui->lock);
  ui->ready = true;
  pthread_mutex_unlock(&ui->lock);
  sem_post(&ui->ready_wake);

  while (ui_running(ui)) {
    xv_ui_event_t event;
    bool processed = false;
    while (ui_pop(ui, &event)) {
      process_event(ui, &event);
      processed = true;
    }
#ifdef CONFIG_EXAMPLES_XIAOV_LVGL
    if (ui->graphical) {
      uint32_t idle = lv_timer_handler();
      refresh_queue_status(ui);
      refresh_video_frame(ui);
      if (idle == 0U || idle > 20U) {
        idle = 20U;
      }
      wait_for_event(ui, idle);
      continue;
    }
#endif
    if (!processed) {
      wait_for_event(ui, 1000U);
    }
  }
#ifdef CONFIG_EXAMPLES_XIAOV_LVGL
  destroy_graphical_ui(ui);
#endif
  return NULL;
}

xv_ui_t *xv_ui_init(xv_ui_storage_t *storage,
                     const xv_ui_callbacks_t *callbacks) {
  xv_ui_t *ui;

  if (storage == NULL || callbacks == NULL) {
    return NULL;
  }
  memset(storage, 0, sizeof(*storage));
  ui = (xv_ui_t *)storage->bytes;
  ui->callbacks = *callbacks;
#ifdef CONFIG_EXAMPLES_XIAOV_LVGL
  ui->current_user = -1;
  ui->current_assistant = -1;
#endif
  if (pthread_mutex_init(&ui->lock, NULL) != 0) {
    return NULL;
  }
  ui->lock_initialized = true;
  if (sem_init(&ui->wake, 0, 0) < 0) {
    xv_ui_deinit(ui);
    return NULL;
  }
  ui->wake_initialized = true;
  if (sem_init(&ui->ready_wake, 0, 0) < 0) {
    xv_ui_deinit(ui);
    return NULL;
  }
  ui->ready_wake_initialized = true;
  if (!xv_conversation_init(&ui->conversation, conversation_render, ui)) {
    xv_ui_deinit(ui);
    return NULL;
  }
  return ui;
}

bool xv_ui_start(xv_ui_t *ui) {
  pthread_attr_t attributes;
  int result;

  if (ui == NULL || !ui->lock_initialized || ui->thread_started) {
    return false;
  }
  pthread_mutex_lock(&ui->lock);
  ui->running = true;
  ui->ready = false;
  pthread_mutex_unlock(&ui->lock);
  if (pthread_attr_init(&attributes) != 0) {
    pthread_mutex_lock(&ui->lock);
    ui->running = false;
    pthread_mutex_unlock(&ui->lock);
    return false;
  }
  if (pthread_attr_setstacksize(&attributes,
                                CONFIG_EXAMPLES_XIAOV_UI_WORKER_STACKSIZE) !=
      0) {
    pthread_attr_destroy(&attributes);
    pthread_mutex_lock(&ui->lock);
    ui->running = false;
    pthread_mutex_unlock(&ui->lock);
    return false;
  }
  result = pthread_create(&ui->thread, &attributes, ui_worker, ui);
  pthread_attr_destroy(&attributes);
  if (result != 0) {
    pthread_mutex_lock(&ui->lock);
    ui->running = false;
    pthread_mutex_unlock(&ui->lock);
    return false;
  }
  ui->thread_started = true;
  do {
    result = sem_wait(&ui->ready_wake);
  } while (result < 0 && errno == EINTR);
  return result == 0;
}

void xv_ui_stop(xv_ui_t *ui) {
  if (ui == NULL || !ui->thread_started) {
    return;
  }
  pthread_mutex_lock(&ui->lock);
  ui->running = false;
  pthread_mutex_unlock(&ui->lock);
  sem_post(&ui->wake);
  pthread_join(ui->thread, NULL);
  ui->thread_started = false;
}

void xv_ui_deinit(xv_ui_t *ui) {
  if (ui == NULL) {
    return;
  }
  xv_ui_stop(ui);
  if (ui->ready_wake_initialized) {
    sem_destroy(&ui->ready_wake);
    ui->ready_wake_initialized = false;
  }
  if (ui->wake_initialized) {
    sem_destroy(&ui->wake);
    ui->wake_initialized = false;
  }
  if (ui->lock_initialized) {
    pthread_mutex_destroy(&ui->lock);
    ui->lock_initialized = false;
  }
}

bool xv_ui_post_state(xv_ui_t *ui, xv_app_state_t state) {
  xv_ui_event_t event;
  if (state > XV_STATE_ERROR) {
    return false;
  }
  memset(&event, 0, sizeof(event));
  event.type = XV_UI_EVENT_STATE;
  event.state = state;
  return ui_push(ui, &event);
}

bool xv_ui_post_text(xv_ui_t *ui, xv_conversation_text_kind_t kind,
                     const char *turn_id, const char *text) {
  xv_ui_event_t event;

  if (kind > XV_CONVERSATION_TEXT_ASSISTANT_FINAL) {
    return false;
  }
  memset(&event, 0, sizeof(event));
  event.type = XV_UI_EVENT_TEXT;
  event.text_kind = kind;
  if (!bounded_copy(event.turn_id, sizeof(event.turn_id), turn_id) ||
      event.turn_id[0] == '\0' ||
      !bounded_copy(event.text, sizeof(event.text), text)) {
    return false;
  }
  return ui_push(ui, &event);
}

bool xv_ui_post_reminder(xv_ui_t *ui, const char *label, bool active) {
  xv_ui_event_t event;

  memset(&event, 0, sizeof(event));
  event.type = XV_UI_EVENT_REMINDER;
  event.active = active;
  if (active && (!bounded_copy(event.text, sizeof(event.text), label) ||
                 event.text[0] == '\0')) {
    return false;
  }
  return ui_push(ui, &event);
}

bool xv_ui_post_media_command(xv_ui_t *ui, const char *action,
                              const char *query) {
  xv_ui_event_t event;

  memset(&event, 0, sizeof(event));
  event.type = XV_UI_EVENT_MEDIA_COMMAND;
  if (!bounded_copy(event.turn_id, sizeof(event.turn_id), action) ||
      event.turn_id[0] == '\0' ||
      !bounded_copy(event.text, sizeof(event.text),
                    query == NULL ? "" : query)) {
    return false;
  }
  return ui_push(ui, &event);
}

bool xv_ui_post_music_state(xv_ui_t *ui,
                            const xv_music_snapshot_t *snapshot) {
  xv_ui_event_t event;

  if (ui == NULL || snapshot == NULL) {
    return false;
  }
  memset(&event, 0, sizeof(event));
  event.type = XV_UI_EVENT_MUSIC_STATE;
  event.music = *snapshot;
  return ui_push(ui, &event);
}

bool xv_ui_post_low_power(xv_ui_t *ui, bool low_power) {
  xv_ui_event_t event;

  if (ui == NULL) {
    return false;
  }
  memset(&event, 0, sizeof(event));
  event.type = XV_UI_EVENT_LOW_POWER;
  event.active = low_power;
  return ui_push(ui, &event);
}

bool xv_ui_post_display_state(xv_ui_t *ui, const char *mode,
                              const char *status) {
  xv_ui_event_t event;

  if (ui == NULL || mode == NULL || status == NULL) {
    return false;
  }
  memset(&event, 0, sizeof(event));
  event.type = XV_UI_EVENT_DISPLAY_STATE;
  if (!bounded_copy(event.turn_id, sizeof(event.turn_id), mode) ||
      !bounded_copy(event.text, sizeof(event.text), status)) {
    return false;
  }
  return ui_push(ui, &event);
}

bool xv_ui_update_video_frame(xv_ui_t *ui, const uint8_t *rgb565,
                              size_t byte_count, uint32_t sequence,
                              uint32_t timestamp_ms) {
  uint8_t index;

  if (ui == NULL || rgb565 == NULL || !ui->lock_initialized ||
      byte_count != XV_UI_VIDEO_FRAME_BYTES) {
    return false;
  }
  pthread_mutex_lock(&ui->lock);
  index = ui->video_display_index == 0U ? 1U : 0U;
  if (ui->video_pending) {
    ui->video_replaced += 1U;
  }
  memcpy(g_video_buffers[index], rgb565, byte_count);
  ui->video_pending_index = index;
  ui->video_pending_sequence = sequence;
  ui->video_pending_timestamp_ms = timestamp_ms;
  ui->video_pending = true;
  ui->video_frames += 1U;
  pthread_mutex_unlock(&ui->lock);
  sem_post(&ui->wake);
  return true;
}

bool xv_ui_update_audio_levels(xv_ui_t *ui, const uint8_t levels[8],
                               uint8_t mouth_level) {
  size_t index;

  if (ui == NULL || levels == NULL || !ui->lock_initialized) {
    return false;
  }
  pthread_mutex_lock(&ui->lock);
  for (index = 0U; index < sizeof(ui->audio_levels); ++index) {
    ui->audio_levels[index] = levels[index] > 100U ? 100U : levels[index];
  }
  ui->mouth_level = mouth_level > 100U ? 100U : mouth_level;
  ui->audio_levels_valid = true;
  pthread_mutex_unlock(&ui->lock);
  return true;
}

bool xv_ui_post_error(xv_ui_t *ui, const char *message) {
  xv_ui_event_t event;

  memset(&event, 0, sizeof(event));
  event.type = XV_UI_EVENT_ERROR;
  if (!bounded_copy(event.text, sizeof(event.text), message) ||
      event.text[0] == '\0') {
    return false;
  }
  return ui_push(ui, &event);
}

bool xv_ui_is_graphical(xv_ui_t *ui) {
  bool graphical;

  if (ui == NULL || !ui->lock_initialized) {
    return false;
  }
  pthread_mutex_lock(&ui->lock);
  graphical = ui->ready && ui->graphical;
  pthread_mutex_unlock(&ui->lock);
  return graphical;
}

bool xv_ui_is_interactive(xv_ui_t *ui) {
  bool interactive;

  if (ui == NULL || !ui->lock_initialized) {
    return false;
  }
  pthread_mutex_lock(&ui->lock);
  interactive = ui->ready && ui->interactive;
  pthread_mutex_unlock(&ui->lock);
  return interactive;
}

void xv_ui_get_stats(xv_ui_t *ui, xv_ui_stats_t *stats) {
  if (ui == NULL || stats == NULL || !ui->lock_initialized) {
    return;
  }
  pthread_mutex_lock(&ui->lock);
  stats->queue_rejected = ui->queue_rejected;
  stats->render_rejected = ui->render_rejected;
  stats->queue_depth = ui->event_count;
  stats->graphical = ui->graphical;
  stats->interactive = ui->interactive;
  stats->video_frames = ui->video_frames;
  stats->video_replaced = ui->video_replaced;
  pthread_mutex_unlock(&ui->lock);
}
