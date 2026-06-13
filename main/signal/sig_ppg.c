#include "sig_ppg.h"
#include "app_config.h"

#include <stdbool.h>
#include <math.h>

/* -------------------------------------------------------------------------- */
/* Module state                                                               */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t beat_foot_ms;     /* expected_ms when the PPG foot occurs */
    uint32_t rr_ms;            /* RR interval of the current beat in ms */
    bool     beat_valid;       /* false until first valid beat received */
    float    pat_ms_applied;   /* PAT for this beat including jitter */
    uint32_t lfsr;             /* Galois LFSR for beat-to-beat PAT jitter */

    /* Pre-computed per beat: 1/(2*sigma^2) in ms^-2, avoids per-sample division */
    float inv_2sig1sq;
    float inv_2sig2sq;
    float amplitude_scale;     /* 1 / (A1 + A2) for normalising to 0..1 */

    /* Foot detector — first-derivative zero-crossing method */
    bool     det_searching;        /* true while within the search window */
    uint32_t last_r_peak_ms;       /* r_peak_ms from most recent beat */
    uint32_t det_window_open_ms;   /* earliest expected_ms to start searching */
    uint32_t det_window_close_ms;  /* latest expected_ms; abort if not found */
    int32_t  det_prev_ppg;         /* PPG sample from the previous tick (int32_t for real sensor range) */
    float    det_peak_deriv;       /* running maximum of d[n] seen this search */
    bool     det_deriv_peaked;     /* true once d has started declining */
    uint32_t det_last_nonpos_ms;   /* expected_ms of last d≤0 before peak */
    int32_t  pat_det_last_ms;      /* most recently detected single-beat PAT, -1 until first */
    int32_t  pat_det_avg_ms;       /* filtered PAT from foot detector, -1 until first */
} ppg_sim_t;

static ppg_sim_t s_ppg;

/* -------------------------------------------------------------------------- */
/* Private helper                                                             */
/* -------------------------------------------------------------------------- */

static void precompute_denominators(float rr_ms_f)
{
    float sig1 = PPG_SIM_SIGMA1_FRAC * rr_ms_f;
    float sig2 = PPG_SIM_SIGMA2_FRAC * rr_ms_f;
    s_ppg.inv_2sig1sq = 1.0f / (2.0f * sig1 * sig1);
    s_ppg.inv_2sig2sq = 1.0f / (2.0f * sig2 * sig2);
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void ppg_sim_init(void)
{
    s_ppg.beat_foot_ms       = 0;
    s_ppg.rr_ms              = 0;
    s_ppg.beat_valid         = false;
    s_ppg.pat_ms_applied     = PPG_SIM_PAT_DEFAULT_MS;
    s_ppg.lfsr               = 0xACE1u;
    s_ppg.amplitude_scale    = 1.0f / (PPG_SIM_A1 + PPG_SIM_A2);
    s_ppg.det_searching      = false;
    s_ppg.last_r_peak_ms     = 0;
    s_ppg.det_window_open_ms = 0;
    s_ppg.det_window_close_ms= 0;
    s_ppg.det_prev_ppg       = (int32_t)REC_SIM_CENTRE;
    s_ppg.det_peak_deriv     = 0.0f;
    s_ppg.det_deriv_peaked   = false;
    s_ppg.det_last_nonpos_ms = 0;
    s_ppg.pat_det_last_ms    = -1;
    s_ppg.pat_det_avg_ms     = -1;
    precompute_denominators(800.0f);   /* nominal 75 BPM until first beat */
}

void ppg_sim_on_beat(uint32_t r_peak_ms, uint32_t rr_ms)
{
    if (rr_ms == 0) return;

    /* Galois LFSR step for ±PAT_JITTER_MS beat-to-beat variation */
    uint32_t lsb = s_ppg.lfsr & 1u;
    s_ppg.lfsr >>= 1;
    if (lsb) s_ppg.lfsr ^= 0xB400u;

    float jitter = ((float)(int)(s_ppg.lfsr & 0x3Fu) - 32.0f)
                   * (PPG_SIM_PAT_JITTER_MS / 32.0f);   /* range ≈ ±PPG_SIM_PAT_JITTER_MS */

    s_ppg.pat_ms_applied = PPG_SIM_PAT_DEFAULT_MS + jitter;
    s_ppg.beat_foot_ms   = r_peak_ms + (uint32_t)s_ppg.pat_ms_applied;
    s_ppg.rr_ms          = rr_ms;
    s_ppg.beat_valid     = true;

    /* Open foot-detector search window: 50 ms after R-peak, close at 500 ms */
    s_ppg.last_r_peak_ms      = r_peak_ms;
    s_ppg.det_window_open_ms  = r_peak_ms + 50u;
    s_ppg.det_window_close_ms = r_peak_ms + 500u;
    s_ppg.det_searching       = true;
    s_ppg.det_peak_deriv      = 0.0f;
    s_ppg.det_deriv_peaked    = false;
    s_ppg.det_last_nonpos_ms  = 0;

    precompute_denominators((float)rr_ms);
}

int16_t ppg_sim_get_sample(uint32_t current_ms)
{
    /* Bootstrap on first call (no ECG beat received yet). */
    if (!s_ppg.beat_valid || s_ppg.rr_ms == 0) {
        s_ppg.beat_foot_ms = current_ms;
        s_ppg.rr_ms        = 800;   /* default 75 BPM */
        s_ppg.beat_valid   = true;
        precompute_denominators(800.0f);
    }

    /* When ECG is not driving beats, auto-advance the window each period so
     * the simulation keeps running rather than going flat. */
    int32_t elapsed = (int32_t)current_ms - (int32_t)s_ppg.beat_foot_ms;
    if (elapsed > (int32_t)s_ppg.rr_ms) {
        s_ppg.beat_foot_ms += s_ppg.rr_ms;
        elapsed -= (int32_t)s_ppg.rr_ms;
    }

    /* Time within the beat window (ms from the foot) */
    float t = (float)elapsed;
    if (t < 0.0f)
        return (int16_t)REC_SIM_CENTRE;

    float rr_f = (float)s_ppg.rr_ms;
    float mu1  = PPG_SIM_MU1_FRAC * rr_f;   /* systolic centre in ms */
    float mu2  = PPG_SIM_MU2_FRAC * rr_f;   /* diastolic centre in ms */
    float dt1  = t - mu1;
    float dt2  = t - mu2;

    /* Two-Gaussian model: systolic + diastolic/reflected wave */
    float ppg = PPG_SIM_A1 * expf(-dt1 * dt1 * s_ppg.inv_2sig1sq)
              + PPG_SIM_A2 * expf(-dt2 * dt2 * s_ppg.inv_2sig2sq);
    ppg *= s_ppg.amplitude_scale;   /* normalise to 0..1 */

    return (int16_t)((float)REC_SIM_CENTRE + (float)REC_SIM_AMP * ppg);
}

int32_t ppg_sim_get_pat_ms(void)
{
    if (!s_ppg.beat_valid) return -1;
    return (int32_t)s_ppg.pat_ms_applied;
}

void ppg_det_update_sample(int32_t ppg_sample, uint32_t current_ms)
{
    /* First-derivative zero-crossing foot detector.
     * d[n] = ppg[n] - ppg[n-1] (proportional to derivative at 100 Hz).
     * The foot is the last zero-crossing of d before the systolic upstroke peak:
     *   1. Track the running maximum of d (det_peak_deriv).
     *   2. Mark det_deriv_peaked once d drops back below zero after the peak.
     *   3. The foot timestamp is det_last_nonpos_ms + ECG_SAMPLE_PERIOD_MS
     *      (one sample before d first went positive). */
    float d = (float)ppg_sample - (float)s_ppg.det_prev_ppg;
    s_ppg.det_prev_ppg = ppg_sample;

    if (!s_ppg.det_searching) return;

    /* Abort if we have passed the close time without finding a foot */
    if (current_ms > s_ppg.det_window_close_ms) {
        s_ppg.det_searching = false;
        return;
    }

    /* Not yet in the window — do nothing but keep updating det_prev_ppg */
    if (current_ms < s_ppg.det_window_open_ms) return;

    if (!s_ppg.det_deriv_peaked) {
        /* Phase 1: rising edge — track the maximum derivative */
        if (d > s_ppg.det_peak_deriv) {
            s_ppg.det_peak_deriv = d;
        }
        if (d <= 0.0f) {
            /* Still pre-peak: record as candidate last-zero-crossing */
            s_ppg.det_last_nonpos_ms = current_ms;
        } else if (s_ppg.det_peak_deriv > 0.0f && d < s_ppg.det_peak_deriv * 0.5f) {
            /* d has fallen to half its peak — consider the derivative peaked */
            s_ppg.det_deriv_peaked = true;
        }
    } else {
        /* Phase 2: look for d returning to ≤ 0 after the upstroke */
        if (d <= 0.0f && s_ppg.det_peak_deriv > 2.0f) {
            /* Found the systolic peak crossing — compute foot */
            uint32_t foot_ms = s_ppg.det_last_nonpos_ms + ECG_SAMPLE_PERIOD_MS;
            int32_t  pat_raw = (int32_t)foot_ms - (int32_t)s_ppg.last_r_peak_ms;

            /* Physiologically plausible range: 50–500 ms */
            if (pat_raw >= 50 && pat_raw <= 500) {
                s_ppg.pat_det_last_ms = pat_raw;
                if (s_ppg.pat_det_avg_ms < 0) {
                    s_ppg.pat_det_avg_ms = pat_raw;
                } else {
                    s_ppg.pat_det_avg_ms =
                        (s_ppg.pat_det_avg_ms * (PPG_PAT_AVG_BEATS - 1) + pat_raw)
                        / PPG_PAT_AVG_BEATS;
                }
            }

            s_ppg.det_searching = false;
        }
    }
}

int32_t ppg_det_get_pat_avg_ms(void)
{
    return s_ppg.pat_det_avg_ms;
}

int32_t ppg_det_get_pat_last_ms(void)
{
    return s_ppg.pat_det_last_ms;
}
