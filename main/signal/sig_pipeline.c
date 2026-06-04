#include "sig_pipeline.h"

#include <stdint.h>
#include <math.h>

/* -------------------------------------------------------------------------- */
/* ECG simulation                                                             */
/* -------------------------------------------------------------------------- */

#if ECG_USE_SIMULATED_SOURCE

/*
 * Generate one 12-bit ECG sample (0..4095) at the next call. Models a
 * P-QRS-T complex at ~75 BPM with slow respiratory baseline wander and
 * light pseudo-random noise so the autoscale and beat detector see
 * realistic input. The waveform is biased around mid-scale (~2048) so
 * autoscale shrink-then-grow converges quickly.
 */
int ecg_simulate_raw(void)
{
    /* 100 samples / second × 60 / 75 BPM ≈ 80 samples per beat. */
    static uint32_t s_phase = 0;
    static uint32_t s_beat  = 0;
    static uint32_t s_lfsr  = 0xACE1u;

    const uint32_t period_samples = 80;  /* ~75 BPM at 100 Hz */
    uint32_t t = s_beat;                 /* 0..period_samples-1 */

    /* Components, all in raw ADC counts (0..4095). */
    int baseline = 2048;

    /* Slow respiratory wander: ~15 cycles/min over the recording. */
    float resp = 60.0f * sinf((float)s_phase * 2.0f * (float)M_PI / (15.0f * 100.0f));

    /* P wave: small bump around t = 12..18 */
    float p = 0.0f;
    if (t >= 12 && t <= 18) {
        float u = (float)(t - 12) / 6.0f;
        p = 90.0f * sinf(u * (float)M_PI);
    }

    /* QRS complex: Q dip at t=22, R spike at t=24, S dip at t=26 */
    float qrs = 0.0f;
    if (t == 22) qrs = -180.0f;
    else if (t == 23) qrs = 220.0f;
    else if (t == 24) qrs = 1300.0f;   /* tall R — dominates running_max */
    else if (t == 25) qrs = 600.0f;
    else if (t == 26) qrs = -260.0f;
    else if (t == 27) qrs = -120.0f;

    /* T wave: broad bump around t = 36..52 */
    float twave = 0.0f;
    if (t >= 36 && t <= 52) {
        float u = (float)(t - 36) / 16.0f;
        twave = 240.0f * sinf(u * (float)M_PI);
    }

    /* Galois LFSR for cheap pseudo-random noise (0..~30 counts). */
    uint32_t lsb = s_lfsr & 1u;
    s_lfsr >>= 1;
    if (lsb) s_lfsr ^= 0xB400u;
    int noise = (int)(s_lfsr & 0x1Fu) - 16;

    int raw = baseline + (int)resp + (int)p + (int)qrs + (int)twave + noise;
    if (raw < 0) raw = 0;
    if (raw > 4095) raw = 4095;

    s_phase++;
    s_beat = (s_beat + 1u) % period_samples;
    return raw;
}

#endif /* ECG_USE_SIMULATED_SOURCE */

/* -------------------------------------------------------------------------- */
/* Signal processing                                                          */
/* -------------------------------------------------------------------------- */

float signal_bandpass_step(float x,
                           float sample_hz,
                           float low_hz,
                           float high_hz,
                           float *hp_state,
                           float *lp_state,
                           float *prev_input)
{
    if (!hp_state || !lp_state || !prev_input || sample_hz <= 0.0f) {
        return x;
    }

    if (low_hz < 0.01f) low_hz = 0.01f;
    if (high_hz <= low_hz) high_hz = low_hz + 0.5f;
    if (high_hz > (sample_hz * 0.45f)) high_hz = sample_hz * 0.45f;

    const float dt = 1.0f / sample_hz;

    const float rc_hp = 1.0f / (2.0f * (float)M_PI * low_hz);
    const float alpha_hp = rc_hp / (rc_hp + dt);
    float hp = alpha_hp * ((*hp_state) + x - (*prev_input));
    *hp_state = hp;
    *prev_input = x;

    const float rc_lp = 1.0f / (2.0f * (float)M_PI * high_hz);
    const float alpha_lp = dt / (rc_lp + dt);
    float lp = (*lp_state) + alpha_lp * (hp - (*lp_state));
    *lp_state = lp;

    return lp;
}
