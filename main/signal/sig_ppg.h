#pragma once

#include <stdint.h>

/*
 * Beat-synchronised PPG simulator — two-Gaussian model.
 *
 * All functions are called from ecg_sampler_task under s_ecg_spinlock
 * (portENTER_CRITICAL / portEXIT_CRITICAL). Do not call from LVGL context.
 */

/* Initialise module state. Call once from app_main before ecg_sampler_task. */
void    ppg_sim_init(void);

/*
 * Notify the PPG generator that an R-peak was detected.
 * r_peak_ms : expected_ms of the R-peak (same timeline as s_ecg_last_peak_ms).
 * rr_ms     : RR interval of this beat in ms; ignored if zero.
 */
void    ppg_sim_on_beat(uint32_t r_peak_ms, uint32_t rr_ms);

/*
 * Generate one PPG sample at the given expected_ms tick.
 * Returns int16_t in the range (REC_SIM_CENTRE ± REC_SIM_AMP), matching
 * all other simulated channels. Returns REC_SIM_CENTRE if no beat yet.
 */
int16_t ppg_sim_get_sample(uint32_t current_ms);

/*
 * Ground-truth PAT for the most recent beat in ms (simulation mode).
 * Returns -1 if no beat has been received yet.
 */
int32_t ppg_sim_get_pat_ms(void);

/*
 * Feed one PPG sample into the foot detector.
 * Call every sample inside portENTER_CRITICAL, immediately after storing the
 * sample in s_ppg_raw[]. Uses first-derivative zero-crossing to locate the
 * systolic foot; updates the internal PAT running average on detection.
 *
 * TIMESTAMP CONTRACT: current_ms must be (uint32_t)(esp_timer_get_time()/1000),
 * the same monotonic clock and epoch used by ecg_sampler_task's expected_ms.
 * When integrating a real MAX86140 driver (Stage 2), timestamp each FIFO read
 * with esp_timer_get_time() at the moment of the SPI read, then pass
 * (uint32_t)(read_us / 1000) here. Mixing clocks or epochs silently corrupts PAT.
 */
void ppg_det_update_sample(int32_t ppg_sample, uint32_t current_ms);

/*
 * Most recently detected single-beat PAT (ms), before EMA smoothing.
 * Lags the true beat by one beat (foot detected ~PAT ms after R-peak).
 * Returns -1 until the first foot has been detected.
 * Use this for beat-by-beat CSV logging; use ppg_det_get_pat_avg_ms() for display.
 */
int32_t ppg_det_get_pat_last_ms(void);

/*
 * Filtered PAT estimate from the foot detector (ms), EMA over PPG_PAT_AVG_BEATS.
 * Returns -1 until a valid foot has been detected.
 */
int32_t ppg_det_get_pat_avg_ms(void);
