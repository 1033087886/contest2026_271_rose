#include "xiaov/vad.h"

#include <stddef.h>

#define XV_VAD_NOISE_RISE_SHIFT 8U   /* slow adaptation while speech is held */
#define XV_VAD_NOISE_TRACK_SHIFT 4U  /* faster adaptation while idle */

void xv_vad_config_default(xv_vad_config_t *cfg) {
  if (cfg == NULL) {
    return;
  }
  cfg->speech_ratio = 2U * XV_VAD_RATIO_SCALE;
  cfg->noise_floor_min = 40000U;
  cfg->attack_frames = 2U;
  cfg->hangover_frames = 25U;
}

bool xv_vad_init(xv_vad_t *vad, const xv_vad_config_t *cfg) {
  if (vad == NULL) {
    return false;
  }
  if (cfg == NULL) {
    xv_vad_config_default(&vad->config);
  } else {
    if (cfg->speech_ratio < XV_VAD_RATIO_SCALE || cfg->attack_frames == 0U) {
      return false;
    }
    vad->config = *cfg;
  }
  xv_vad_reset(vad);
  return true;
}

void xv_vad_reset(xv_vad_t *vad) {
  if (vad == NULL) {
    return;
  }
  vad->noise_mean_square = vad->config.noise_floor_min;
  vad->speech_run = 0U;
  vad->hangover_left = 0U;
  vad->active = false;
  vad->speech_frames = 0U;
  vad->total_frames = 0U;
}

uint32_t xv_vad_frame_energy(const int16_t *samples, size_t count) {
  uint64_t accumulator = 0U;
  size_t index;
  if (samples == NULL || count == 0U) {
    return 0U;
  }
  for (index = 0U; index < count; ++index) {
    int32_t sample = samples[index];
    accumulator += (uint64_t)(sample * sample);
  }
  return (uint32_t)(accumulator / count);
}

static void track_noise(xv_vad_t *vad, uint32_t energy, bool speech) {
  uint32_t noise = vad->noise_mean_square;
  uint32_t shift = speech ? XV_VAD_NOISE_RISE_SHIFT : XV_VAD_NOISE_TRACK_SHIFT;

  if (energy > noise) {
    noise += (energy - noise) >> shift;
  } else if (!speech) {
    /* Only fall back toward quiet while the gate believes the room is idle,
     * so a pause inside a sentence does not reset the floor. */
    noise -= (noise - energy) >> shift;
  }

  if (noise < vad->config.noise_floor_min) {
    noise = vad->config.noise_floor_min;
  }
  vad->noise_mean_square = noise;
}

bool xv_vad_process(xv_vad_t *vad, const int16_t *samples, size_t count) {
  uint32_t energy;
  uint64_t threshold;
  bool speech;

  if (vad == NULL || samples == NULL || count == 0U) {
    return false;
  }

  energy = xv_vad_frame_energy(samples, count);
  threshold = ((uint64_t)vad->noise_mean_square * vad->config.speech_ratio) /
              XV_VAD_RATIO_SCALE;
  speech = (uint64_t)energy > threshold;

  vad->total_frames += 1U;
  if (speech) {
    vad->speech_frames += 1U;
    if (vad->speech_run < UINT16_MAX) {
      vad->speech_run += 1U;
    }
    if (vad->speech_run >= vad->config.attack_frames) {
      vad->active = true;
      vad->hangover_left = vad->config.hangover_frames;
    }
  } else {
    vad->speech_run = 0U;
    if (vad->active) {
      if (vad->hangover_left > 0U) {
        vad->hangover_left -= 1U;
      }
      if (vad->hangover_left == 0U) {
        vad->active = false;
      }
    }
  }

  track_noise(vad, energy, speech);
  return vad->active;
}

bool xv_vad_active(const xv_vad_t *vad) {
  return vad == NULL ? false : vad->active;
}

uint32_t xv_vad_noise_floor(const xv_vad_t *vad) {
  return vad == NULL ? 0U : vad->noise_mean_square;
}
