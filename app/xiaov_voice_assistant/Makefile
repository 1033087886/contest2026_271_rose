include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_EXAMPLES_XIAOV_PROGNAME)
PRIORITY  = $(CONFIG_EXAMPLES_XIAOV_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_XIAOV_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_XIAOV)

MAINSRC = xiaov_main.c

CSRCS = xiaov_protocol.c
ifeq ($(CONFIG_EXAMPLES_XIAOV_AUDIO_ALSA),y)
CSRCS += xiaov_alsa_adapter.c
CSRCS += xiaov_alsa_backend.c
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/vendor/allwinnertech/chips/r528/drivers/rtos-hal/include/hal
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/vendor/allwinnertech/chips/r528/drivers/rtos-hal/include/osal
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source
else
CSRCS += xiaov_media_adapter.c
endif
CSRCS += xiaov_local_wav.c
CSRCS += xiaov_skills.c
CSRCS += xiaov_music_adapter.c
CSRCS += xiaov_network_adapter.c
CSRCS += xiaov_power_adapter.c
CSRCS += xiaov_service.c
CSRCS += xiaov_ui.c

ifeq ($(CONFIG_EXAMPLES_XIAOV_KWS),y)
CXXEXT := .cc
CSRCS += xiaov_capture_gain.c
CSRCS += xiaov_kws_frontend.c
CSRCS += core/src/kws_controller.c
CXXSRCS += xiaov_tflm_kws_adapter.cc
CXXSRCS += xiaov_kws_model.cc
CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/mlearning/tflite-micro/tflite-micro
CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/math/gemmlowp/gemmlowp
CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/math/kissfft/kissfft
CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/math/ruy/ruy
CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/system/flatbuffers/flatbuffers/include
CXXFLAGS += -DTFLITE_EMULATE_FLOAT
CXXFLAGS += -DTF_LITE_DISABLE_X86_NEON
CXXFLAGS += -DTF_LITE_STRIP_ERROR_STRINGS
endif

ifeq ($(CONFIG_EXAMPLES_XIAOV_OFFLINE_COMMANDS),y)
CSRCS += core/src/offline_command.c
CXXSRCS += xiaov_command_model.cc
endif

ifeq ($(CONFIG_EXAMPLES_XIAOV_LVGL),y)
CSRCS += assets/xiaov_cjk_font_16.c
endif
CSRCS += core/src/audio_frame.c
CSRCS += core/src/audio_rate_converter.c
CSRCS += core/src/audio_visualizer.c
CSRCS += core/src/conversation.c
CSRCS += core/src/event_queue.c
CSRCS += core/src/music_player.c
CSRCS += core/src/reminder_queue.c
CSRCS += core/src/ring_buffer.c
CSRCS += core/src/runtime.c
CSRCS += core/src/state_machine.c
CSRCS += core/src/stream_markdown.c
CSRCS += core/src/vad.c

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/demos/contest2026_271_xiaov_voice_assistant/core/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../frameworks/multimedia/media/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/libwebsockets/libwebsockets/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/libwebsockets
CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/demos/contest2026_271_xiaov_voice_assistant
CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/demos/contest2026_271_xiaov_voice_assistant/core/include

include $(APPDIR)/Application.mk
