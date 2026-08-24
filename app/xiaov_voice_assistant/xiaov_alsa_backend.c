/*
 * Copyright 2026 Xiao V contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifdef __NuttX__
#  include <nuttx/config.h>
#endif

#include "xiaov_alsa_backend.h"

#include <aw-alsa-lib/control.h>
#include <aw-alsa-lib/pcm.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#ifndef CONFIG_EXAMPLES_XIAOV_ALSA_CAPTURE_DEVICE
#  define CONFIG_EXAMPLES_XIAOV_ALSA_CAPTURE_DEVICE "hw:snddmic"
#endif

#ifndef CONFIG_EXAMPLES_XIAOV_ALSA_PLAYBACK_DEVICE
#  define CONFIG_EXAMPLES_XIAOV_ALSA_PLAYBACK_DEVICE "hw:audiocodec"
#endif

#define XV_ALSA_BUFFER_FRAMES (XV_ALSA_NATIVE_FRAME_SAMPLES * 4U)
#define XV_ALSA_RETRY_DELAY_US 1000U
/* Recovery attempts allowed per 20 ms transfer before the caller is told the
 * device is gone.
 *
 * Without a cap, a handle whose device has disappeared -- which is what a
 * SIGKILLed task leaves behind, because the thread never runs its cleanup --
 * makes these loops call snd_vela_pcm_prepare forever. Each attempt logs
 * "prepare:0x...", so the board prints a line a second indefinitely and the nsh
 * thread never regains the console: `reboot` and Ctrl-C both stop working and
 * only a physical reset recovers it. Observed 2026-08-10.
 *
 * A genuine transient underrun clears in one or two prepares, and one 20 ms
 * frame only needs 4 reads at 5 ms of PCM each, so 16 is far above any healthy
 * case while still terminating in well under a second.
 */
#define XV_ALSA_MAX_RECOVERIES 16U

/* R528 DShanPi capture controls used by the vendor xiaozhi demo.  Applying
 * these only from rcS is insufficient: snd_vela_pcm_open() initializes the
 * audio path later and can restore the card defaults.  The observed result on
 * RC31 was healthy 16 kHz PCM delivery but a KWS ceiling of 384 Q15 until the
 * same six controls were set manually after the daemon had opened snddmic.
 *
 * Keep the controls next to the ALSA open that depends on them.  They are
 * applied after PCM configuration and read back before the first sample is
 * accepted, so every fresh standby/voice capture has a deterministic hardware
 * gain even after a physical reset or a playback/capture route transition.
 */
#define XV_ALSA_CAPTURE_CONTROL_CARD "audiocodec"

typedef struct {
  unsigned int numid;
  unsigned int value;
} xv_alsa_control_value_t;

static const xv_alsa_control_value_t g_capture_controls[] = {
    {19U, 255U},
    {20U, 255U},
    {21U, 255U},
    {6U, 180U},
    {7U, 180U},
    {15U, 6U},
};

/* The R528 codec samples pb_audio_route while the playback PCM is opened.
 * Changing control 32 after START updates the stored enum but does not rerun
 * the DAPM route, so the external speaker PA on PD17 remains low. Select the
 * speaker route before opening hw:audiocodec.
 *
 * Do not force any gain here. RC36 left gain under the codec defaults and the
 * normal volume control (DACL/R=180 from rcS, LINEOUT=26 from the driver) and
 * produced clean speech. RC42 forced DACL/R=220 and LINEOUT=31 on every open;
 * both music and TTS then sounded clipped and hoarse on the real board. Route
 * selection is required for audibility, but gain policy is not part of open.
 */
static const xv_alsa_control_value_t g_playback_controls[] = {
    {32U, 0U},
};

static int xv_alsa_validate_control_value(const snd_ctl_info_t *info,
                                          unsigned int value) {
  if (info->type == SND_CTL_ELEM_TYPE_INTEGER) {
    return value >= (unsigned int)info->min &&
                   value <= (unsigned int)info->max
               ? 0
               : -ERANGE;
  }
  if (info->type == SND_CTL_ELEM_TYPE_ENUMERATED) {
    return value < (unsigned int)info->items ? 0 : -ERANGE;
  }
  return -EINVAL;
}

static int xv_alsa_configure_capture_controls(void) {
  size_t index;

  for (index = 0U;
       index < sizeof(g_capture_controls) / sizeof(g_capture_controls[0]);
       ++index) {
    const xv_alsa_control_value_t *control = &g_capture_controls[index];
    snd_ctl_info_t info;
    int result;

    memset(&info, 0, sizeof(info));
    result = snd_ctl_get_bynum(XV_ALSA_CAPTURE_CONTROL_CARD,
                               control->numid, &info);
    if (result < 0) {
      return result;
    }
    result = xv_alsa_validate_control_value(&info, control->value);
    if (result < 0) {
      return result;
    }
    result = snd_ctl_set_bynum(XV_ALSA_CAPTURE_CONTROL_CARD,
                               control->numid, control->value);
    if (result < 0) {
      return result;
    }
    memset(&info, 0, sizeof(info));
    result = snd_ctl_get_bynum(XV_ALSA_CAPTURE_CONTROL_CARD,
                               control->numid, &info);
    if (result < 0) {
      return result;
    }
    if (info.value != (unsigned long)control->value) {
      return -EIO;
    }
  }
  return 0;
}

static int xv_alsa_configure_playback_controls(void) {
  size_t index;

  for (index = 0U;
       index < sizeof(g_playback_controls) / sizeof(g_playback_controls[0]);
       ++index) {
    const xv_alsa_control_value_t *control = &g_playback_controls[index];
    snd_ctl_info_t info;
    int result;

    memset(&info, 0, sizeof(info));
    result = snd_ctl_get_bynum(XV_ALSA_CAPTURE_CONTROL_CARD,
                               control->numid, &info);
    if (result < 0) {
      return result;
    }
    result = xv_alsa_validate_control_value(&info, control->value);
    if (result < 0) {
      return result;
    }
    result = snd_ctl_set_bynum(XV_ALSA_CAPTURE_CONTROL_CARD,
                               control->numid, control->value);
    if (result < 0) {
      return result;
    }
    memset(&info, 0, sizeof(info));
    result = snd_ctl_get_bynum(XV_ALSA_CAPTURE_CONTROL_CARD,
                               control->numid, &info);
    if (result < 0) {
      return result;
    }
    if (info.value != (unsigned long)control->value) {
      return -EIO;
    }
  }
  return 0;
}

static int xv_alsa_configure(snd_pcm_t *handle) {
  snd_pcm_hw_params_t *hardware;
  snd_pcm_sw_params_t *software;
  int result;

  snd_pcm_hw_params_alloca(&hardware);
  result = snd_vela_pcm_hw_params_any(handle, hardware);
  if (result < 0) {
    return result;
  }
  result = snd_vela_pcm_hw_params_set_access(
      handle, hardware, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (result < 0) {
    return result;
  }
  result = snd_vela_pcm_hw_params_set_format(
      handle, hardware, SND_PCM_FORMAT_S16_LE);
  if (result < 0) {
    return result;
  }
  result = snd_vela_pcm_hw_params_set_channels(
      handle, hardware, XV_AUDIO_NATIVE_CHANNELS);
  if (result < 0) {
    return result;
  }
  result = snd_vela_pcm_hw_params_set_rate(
      handle, hardware, XV_AUDIO_NATIVE_SAMPLE_RATE_HZ, 0);
  if (result < 0) {
    return result;
  }
  result = snd_vela_pcm_hw_params_set_period_size(
      handle, hardware, XV_ALSA_NATIVE_FRAME_SAMPLES, 0);
  if (result < 0) {
    return result;
  }
  result = snd_vela_pcm_hw_params_set_buffer_size(
      handle, hardware, XV_ALSA_BUFFER_FRAMES);
  if (result < 0) {
    return result;
  }
  result = snd_vela_pcm_hw_params(handle, hardware);
  if (result < 0) {
    return result;
  }

  snd_pcm_sw_params_alloca(&software);
  result = snd_vela_pcm_sw_params_current(handle, software);
  if (result < 0) {
    return result;
  }
  result = snd_vela_pcm_sw_params_set_start_threshold(
      handle, software,
      snd_vela_pcm_stream(handle) == SND_VELA_PCM_STREAM_CAPTURE
          ? 1U
          : XV_ALSA_NATIVE_FRAME_SAMPLES);
  if (result < 0) {
    return result;
  }
  result = snd_vela_pcm_sw_params_set_stop_threshold(
      handle, software, XV_ALSA_BUFFER_FRAMES);
  if (result < 0) {
    return result;
  }
  result = snd_vela_pcm_sw_params_set_avail_min(
      handle, software, XV_ALSA_NATIVE_FRAME_SAMPLES);
  if (result < 0) {
    return result;
  }
  return snd_vela_pcm_sw_params(handle, software);
}

void xv_alsa_backend_reset(xv_alsa_backend_t *backend) {
  if (backend != NULL) {
    memset(backend, 0, sizeof(*backend));
    xv_audio_downsampler_reset(&backend->downsampler);
    xv_audio_upsampler_reset(&backend->upsampler);
  }
}

static int xv_alsa_open(void **destination, const char *device, int stream) {
  snd_pcm_t *handle = NULL;
  int result;

  result = snd_vela_pcm_open(&handle, device, stream, 0);
  if (result < 0) {
    return result;
  }
  result = xv_alsa_configure(handle);
  if (result < 0) {
    (void)snd_vela_pcm_close(handle);
    return result;
  }
  *destination = handle;
  return 0;
}

static int xv_alsa_transfer_recover(snd_pcm_t *handle, int result) {
  if (result == -EPIPE || result == -ESTRPIPE) {
    return snd_vela_pcm_prepare(handle);
  }
  if (result == -EAGAIN) {
    (void)usleep(XV_ALSA_RETRY_DELAY_US);
    return 0;
  }
  return result;
}

void xv_alsa_playback_backend_reset(xv_alsa_playback_backend_t *backend) {
  if (backend != NULL) {
    memset(backend, 0, sizeof(*backend));
    xv_audio_upsampler_reset(&backend->upsampler);
  }
}

bool xv_alsa_playback_backend_is_open(
    const xv_alsa_playback_backend_t *backend) {
  return backend != NULL && backend->handle != NULL;
}

int xv_alsa_playback_backend_open(xv_alsa_playback_backend_t *backend) {
  int result;

  if (backend == NULL || backend->handle != NULL) {
    return -EINVAL;
  }
  xv_audio_upsampler_reset(&backend->upsampler);
  result = xv_alsa_configure_playback_controls();
  if (result < 0) {
    return result;
  }
  result = xv_alsa_open(&backend->handle,
                        CONFIG_EXAMPLES_XIAOV_ALSA_PLAYBACK_DEVICE,
                        SND_VELA_PCM_STREAM_PLAYBACK);
  if (result < 0) {
    return result;
  }
  result = xv_alsa_configure_playback_controls();
  if (result < 0) {
    (void)xv_alsa_playback_backend_close(backend);
    return result;
  }
  return 0;
}

int xv_alsa_playback_backend_write(xv_alsa_playback_backend_t *backend,
                                   const int16_t *samples,
                                   size_t sample_count) {
  snd_pcm_t *handle;
  size_t native_frames;
  size_t frames_written = 0U;
  unsigned int recoveries = 0U;

  if (backend == NULL || backend->handle == NULL || samples == NULL ||
      sample_count == 0U ||
      sample_count > XV_ALSA_NATIVE_FRAME_SAMPLES / XV_AUDIO_RATE_RATIO) {
    return -EINVAL;
  }
  native_frames = xv_audio_upsample_16k_mono_to_48k_stereo(
      &backend->upsampler, samples, sample_count, backend->native,
      XV_ALSA_NATIVE_FRAME_SAMPLES);
  if (native_frames != sample_count * XV_AUDIO_RATE_RATIO) {
    return -EIO;
  }
  handle = (snd_pcm_t *)backend->handle;
  while (frames_written < native_frames) {
    snd_pcm_sframes_t result = snd_vela_pcm_writei(
        handle,
        backend->native + frames_written * XV_AUDIO_NATIVE_CHANNELS,
        native_frames - frames_written);
    if (result <= 0) {
      int recovery = xv_alsa_transfer_recover(handle, (int)result);
      if (recovery < 0) {
        return recovery;
      }
      recoveries += 1U;
      if (recoveries >= XV_ALSA_MAX_RECOVERIES) {
        return -EIO;
      }
      continue;
    }
    frames_written += (size_t)result;
  }
  return 0;
}

int xv_alsa_playback_backend_drain(xv_alsa_playback_backend_t *backend) {
  if (backend == NULL || backend->handle == NULL) {
    return -EINVAL;
  }
  return snd_vela_pcm_drain((snd_pcm_t *)backend->handle);
}

int xv_alsa_playback_backend_close(xv_alsa_playback_backend_t *backend) {
  snd_pcm_t *handle;
  int result;

  if (backend == NULL || backend->handle == NULL) {
    return 0;
  }
  handle = (snd_pcm_t *)backend->handle;
  backend->handle = NULL;
  result = snd_vela_pcm_drop(handle);
  if (snd_vela_pcm_close(handle) < 0 && result >= 0) {
    return -EIO;
  }
  xv_audio_upsampler_reset(&backend->upsampler);
  return result;
}

bool xv_alsa_capture_is_open(const xv_alsa_backend_t *backend) {
  return backend != NULL && backend->capture_handle != NULL;
}

int xv_alsa_capture_open(xv_alsa_backend_t *backend) {
  int result;

  if (backend == NULL || backend->capture_handle != NULL) {
    return -EINVAL;
  }
  xv_audio_downsampler_reset(&backend->downsampler);
  result = xv_alsa_open(&backend->capture_handle,
                        CONFIG_EXAMPLES_XIAOV_ALSA_CAPTURE_DEVICE,
                        SND_VELA_PCM_STREAM_CAPTURE);
  if (result < 0) {
    return result;
  }
  result = xv_alsa_configure_capture_controls();
  if (result < 0) {
    (void)xv_alsa_capture_close(backend);
    return result;
  }
  return 0;
}

int xv_alsa_capture_read_20ms(xv_alsa_backend_t *backend,
                              int16_t *output, size_t output_samples) {
  snd_pcm_t *handle;
  size_t frames_read = 0U;
  unsigned int recoveries = 0U;

  if (backend == NULL || backend->capture_handle == NULL || output == NULL ||
      output_samples != XV_ALSA_NATIVE_FRAME_SAMPLES / XV_AUDIO_RATE_RATIO) {
    return -EINVAL;
  }
  handle = (snd_pcm_t *)backend->capture_handle;
  while (frames_read < XV_ALSA_NATIVE_FRAME_SAMPLES) {
    snd_pcm_sframes_t result = snd_vela_pcm_readi(
        handle,
        backend->capture_native + frames_read * XV_AUDIO_NATIVE_CHANNELS,
        XV_ALSA_NATIVE_FRAME_SAMPLES - frames_read);
    if (result <= 0) {
      int recovery = xv_alsa_transfer_recover(handle, (int)result);
      if (recovery < 0) {
        return recovery;
      }
      recoveries += 1U;
      if (recoveries >= XV_ALSA_MAX_RECOVERIES) {
        /* Report the device as gone instead of retrying forever. The caller
         * (xv_capture_worker) treats this as a capture error, closes the handle
         * and stops, which is what lets the console stay usable. */
        return -EIO;
      }
      continue;
    }
    frames_read += (size_t)result;
  }
  if (xv_audio_downsample_48k_stereo_to_16k_mono(
          &backend->downsampler, backend->capture_native,
          XV_ALSA_NATIVE_FRAME_SAMPLES, output, output_samples) !=
      output_samples) {
    return -EIO;
  }
  return 0;
}

int xv_alsa_capture_close(xv_alsa_backend_t *backend) {
  snd_pcm_t *handle;
  int result;

  if (backend == NULL || backend->capture_handle == NULL) {
    return 0;
  }
  handle = (snd_pcm_t *)backend->capture_handle;
  backend->capture_handle = NULL;
  result = snd_vela_pcm_drop(handle);
  if (snd_vela_pcm_close(handle) < 0 && result >= 0) {
    return -EIO;
  }
  xv_audio_downsampler_reset(&backend->downsampler);
  return result;
}

bool xv_alsa_playback_is_open(const xv_alsa_backend_t *backend) {
  return backend != NULL && backend->playback_handle != NULL;
}

int xv_alsa_playback_open(xv_alsa_backend_t *backend) {
  int result;

  if (backend == NULL || backend->playback_handle != NULL) {
    return -EINVAL;
  }
  xv_audio_upsampler_reset(&backend->upsampler);
  result = xv_alsa_configure_playback_controls();
  if (result < 0) {
    return result;
  }
  result = xv_alsa_open(&backend->playback_handle,
                        CONFIG_EXAMPLES_XIAOV_ALSA_PLAYBACK_DEVICE,
                        SND_VELA_PCM_STREAM_PLAYBACK);
  if (result < 0) {
    return result;
  }
  result = xv_alsa_configure_playback_controls();
  if (result < 0) {
    (void)xv_alsa_playback_close(backend);
    return result;
  }
  return 0;
}

int xv_alsa_playback_write(xv_alsa_backend_t *backend,
                           const int16_t *samples, size_t sample_count) {
  snd_pcm_t *handle;
  size_t native_frames;
  size_t frames_written = 0U;
  unsigned int recoveries = 0U;

  if (backend == NULL || backend->playback_handle == NULL || samples == NULL ||
      sample_count == 0U ||
      sample_count > XV_ALSA_NATIVE_FRAME_SAMPLES / XV_AUDIO_RATE_RATIO) {
    return -EINVAL;
  }
  native_frames = xv_audio_upsample_16k_mono_to_48k_stereo(
      &backend->upsampler, samples, sample_count, backend->playback_native,
      XV_ALSA_NATIVE_FRAME_SAMPLES);
  if (native_frames != sample_count * XV_AUDIO_RATE_RATIO) {
    return -EIO;
  }
  handle = (snd_pcm_t *)backend->playback_handle;
  while (frames_written < native_frames) {
    snd_pcm_sframes_t result = snd_vela_pcm_writei(
        handle,
        backend->playback_native + frames_written * XV_AUDIO_NATIVE_CHANNELS,
        native_frames - frames_written);
    if (result <= 0) {
      int recovery = xv_alsa_transfer_recover(handle, (int)result);
      if (recovery < 0) {
        return recovery;
      }
      recoveries += 1U;
      if (recoveries >= XV_ALSA_MAX_RECOVERIES) {
        return -EIO;
      }
      continue;
    }
    frames_written += (size_t)result;
  }
  return 0;
}

int xv_alsa_playback_drain(xv_alsa_backend_t *backend) {
  if (backend == NULL || backend->playback_handle == NULL) {
    return -EINVAL;
  }
  return snd_vela_pcm_drain((snd_pcm_t *)backend->playback_handle);
}

int xv_alsa_playback_close(xv_alsa_backend_t *backend) {
  snd_pcm_t *handle;
  int result;

  if (backend == NULL || backend->playback_handle == NULL) {
    return 0;
  }
  handle = (snd_pcm_t *)backend->playback_handle;
  backend->playback_handle = NULL;
  result = snd_vela_pcm_drop(handle);
  if (snd_vela_pcm_close(handle) < 0 && result >= 0) {
    return -EIO;
  }
  xv_audio_upsampler_reset(&backend->upsampler);
  return result;
}
