#pragma once

#include "app_config.h"   /* ECG_USE_SIMULATED_SOURCE */

/* -------------------------------------------------------------------------- */
/* ECG simulation                                                             */
/* -------------------------------------------------------------------------- */

#if ECG_USE_SIMULATED_SOURCE
/*
 * Generate one 12-bit ECG sample (0..4095). Models a P-QRS-T complex at
 * ~75 BPM with slow respiratory baseline wander and light pseudo-random
 * noise. Maintains internal state across calls using function-local statics.
 */
int ecg_simulate_raw(void);
#endif

/* -------------------------------------------------------------------------- */
/* Signal processing                                                          */
/* -------------------------------------------------------------------------- */

/*
 * Single-sample first-order bandpass filter step (high-pass then low-pass).
 * All filter state is kept externally so the function is re-entrant.
 */
float signal_bandpass_step(float x,
                           float sample_hz,
                           float low_hz,
                           float high_hz,
                           float *hp_state,
                           float *lp_state,
                           float *prev_input);
