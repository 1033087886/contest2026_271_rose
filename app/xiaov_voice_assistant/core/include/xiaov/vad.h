#ifndef XIAOV_VAD_H
#define XIAOV_VAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-point energy VAD used as the compute gate in front of KWS.
 *
 * The detector is intentionally integer-only: the standby path runs on the A7
 * for every 20 ms frame, and no floating point, allocation or libm call is
 * acceptable there. Callers own all storage.
 *
 * Hangover keeps the gate open after speech falls below the threshold so a
 * wake word is never truncated at its tail; the pre-roll ring buffer covers
 * the head. See docs/architecture.md for the standby data flow.
 */

typedef struct {
  /* Frame is speech when its mean square exceeds noise * ratio, scaled by
   * XV_VAD_RATIO_SCALE. 2048 == 2.0x the tracked noise floor. */
  uint32_t speech_ratio;
  /* Absolute mean-square floor; frames below this are always silence. Keeps a
   * dead-silent room from driving the adaptive floor to zero. */
  uint32_t noise_floor_min;
  /* Consecutive speech frames required to open the gate. */
  uint16_t attack_frames;
  /* Frames the gate stays open after speech stops. */
  uint16_t hangover_frames;
} xv_vad_config_t;

typedef struct {
  xv_vad_config_t config;
  uint32_t noise_mean_square;
  uint16_t speech_run;
  uint16_t hangover_left;
  bool active;
  uint64_t speech_frames;
  uint64_t total_frames;
} xv_vad_t;

#define XV_VAD_RATIO_SCALE 1024U

/* Populates cfg with the phase-1 starting point: 2.0x noise floor, 2-frame
 * attack, 25-frame (500 ms at 20 ms/frame) hangover. */
void xv_vad_config_default(xv_vad_config_t *cfg);

bool xv_vad_init(xv_vad_t *vad, const xv_vad_config_t *cfg);
void xv_vad_reset(xv_vad_t *vad);

/* Feeds one frame of 16-bit mono PCM. Returns true while the gate is open,
 * i.e. the frame should reach the KWS stage. */
bool xv_vad_process(xv_vad_t *vad, const int16_t *samples, size_t count);

/* Mean square of the frame, the same value xv_vad_process thresholds. Exposed
 * so the host tuner and on-device metrics report identical numbers. */
uint32_t xv_vad_frame_energy(const int16_t *samples, size_t count);

bool xv_vad_active(const xv_vad_t *vad);
uint32_t xv_vad_noise_floor(const xv_vad_t *vad);

#ifdef __cplusplus
}
#endif

#endif
