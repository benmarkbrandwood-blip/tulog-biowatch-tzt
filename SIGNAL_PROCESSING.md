# Signal Processing Methods

This document describes all signal acquisition, filtering, detection, and derived-metric algorithms implemented in the tulog-biowatch firmware. It covers the current Stage 1 (simulated) implementation and the intended behaviour when real sensor hardware is integrated in Stage 2.

All constants referenced below are defined in [main/app_config.h](file:///home/benbrandwood/Documents/dev/tulog-biowatch/main/app_config.h). Algorithm implementation lives in [main/main.c](file:///home/benbrandwood/Documents/dev/tulog-biowatch/main/main.c) (`ecg_sampler_task`) and [main/signal/](file:///home/benbrandwood/Documents/dev/tulog-biowatch/main/signal/).


## 1. Sampling Architecture

### 1.1 Common timeline

All signals share a single monotonic timeline rooted at `esp_timer_get_time()`:

```
expected_ms = total_samples × ECG_SAMPLE_PERIOD_MS
```

`expected_ms` is the ideal timestamp each sample *should* occur at on a perfect 100 Hz grid. The measured wall-clock time at each sample is `actual_ms = (esp_timer_get_time() − start_us) / 1000`. The difference `actual_ms − expected_ms` is recorded as `drift_ms` in the CSV and grows as a cumulative representation of jitter relative to the ideal grid.

This single-clock model is critical for PAT accuracy: both the ECG R-peak timestamp and the PPG foot timestamp must come from `esp_timer_get_time()` — the same origin. Any mixing of clocks or origins silently corrupts PAT. See §4 for details.

### 1.2 Sampling rate

| Channel | Stage 1 | Stage 2 target |
| - | - | - |
| ECG | 100 Hz (simulated) | 1000 Hz via ADS127L18 |
| PPG | 100 Hz (simulated, beat-synchronised) | 100–400 Hz via MAX86140 |
| RESP / NAS / ERB | 100 Hz (sine placeholders) | 50–100 Hz via ADS127L18 |
| FCG | 100 Hz (sine placeholder) | 1000 Hz via ADS127L18 |



## 2. ECG Processing Pipeline

### 2.1 Signal simulation (Stage 1)

`ecg_simulate_raw()` in [main/signal/sig_pipeline.c](file:///home/benbrandwood/Documents/dev/tulog-biowatch/main/signal/sig_pipeline.c) generates a deterministic 12-bit (0–4095) P-QRS-T waveform at ~75 BPM (80 samples per beat at 100 Hz):

| Component | Samples within beat | Amplitude |
| - | - | - |
| P wave | t = 12–18 (half-sine) | +90 counts |
| Q dip | t = 22 | −180 counts |
| R spike | t = 23–25 (asymmetric triangle) | peak +1300 counts |
| S dip | t = 26–27 | −260 / −120 counts |
| T wave | t = 36–52 (half-sine) | +240 counts |
| Baseline | DC offset 2048 + slow respiratory wander (~60 counts, sinusoidal over ~150 beats) | — |
| Noise | Galois LFSR 16-bit, ±16 counts | — |


The waveform is biased around mid-scale (2048) so that the autoscale and QRS detector see realistic amplitude ratios immediately on startup.

### 2.2 Bandpass filter

Each sample passes through `signal_bandpass_step()` ([main/signal/sig_pipeline.c](file:///home/benbrandwood/Documents/dev/tulog-biowatch/main/signal/sig_pipeline.c)):

**High-pass filter (removes baseline wander and P-wave energy)**

First-order IIR high-pass with cutoff `ECG_QRS_BP_LOW_HZ` = 10 Hz:

```
α_HP = RC_HP / (RC_HP + dt),  RC_HP = 1 / (2π × 10)  
hp[n] = α_HP × (hp[n−1] + x[n] − x[n−1])
```

The 10 Hz lower cutoff was chosen to suppress P-wave energy. The P-wave fundamental lies in the 2–8 Hz band (P-wave duration ~60–80 ms); a 5 Hz cutoff passes enough of this energy to occasionally trigger the MWI before the true QRS arrives. Raising the cutoff to 10 Hz attenuates P-wave energy by approximately 6 dB while preserving the QRS complex, which concentrates its energy above 10 Hz.

**Low-pass filter (removes high-frequency noise)**

First-order IIR low-pass with cutoff `ECG_QRS_BP_HIGH_HZ` = 15 Hz:

```
α_LP = dt / (RC_LP + dt),  RC_LP = 1 / (2π × 15)  
lp[n] = lp[n−1] + α_LP × (hp[n] − lp[n−1])
```

The combined 10–15 Hz passband targets the dominant QRS energy. Filter states (`s_ecg_hp_state`, `s_ecg_lp_state`, `s_ecg_prev_input`) are held in module-level variables and re-initialised at task start. The bandpassed output `band` is stored sample-by-sample in a parallel ring buffer `s_ecg_band[]` (float, `ECG_WINDOW_SAMPLES` entries) alongside the raw `s_ecg_raw[]` buffer; this bandpassed history is used by the R-peak search (§2.4).

### 2.3 QRS detection — Pan-Tompkins pipeline

After bandpass filtering, each sample follows:

**Step 1 — Differentiation**

```
diff[n] = band[n] − band[n−1]
```

Emphasises the steep slopes of the QRS complex; suppresses the slowly-varying P and T waves.

**Step 2 — Squaring**

```
squared[n] = diff[n]²
```

Makes all values positive and non-linearly amplifies large slopes relative to small ones, improving the signal-to-noise ratio of the QRS energy.

**Step 3 — Moving window integration (MWI)**

```
integrated[n] = (1 / N) × Σ squared[n−N+1 .. n],  N = ECG_MWI_SAMPLES = 15
```

A 15-sample (150 ms) rectangular integrator smooths the squared derivative into a broad energy envelope centred over each QRS complex. The window length is matched to the typical QRS duration.

**Step 4 — Local peak detection**

A local maximum in the MWI output is declared when:

```
integrated[n−2] ≥ integrated[n]  AND  integrated[n−2] ≥ integrated[n−3]
```

The two-sample look-back (`prev_int`) introduces a 20 ms confirmation delay, ensuring the peak has passed before triggering.

**Step 5 — Adaptive threshold**

The threshold separating signal (QRS) from noise is:

```
threshold = noise_level + 0.25 × (signal_level − noise_level)
```

Signal and noise levels are exponential moving averages updated after each detection:

```
signal_level[n] = 0.875 × signal_level[n−1] + 0.125 × integrated_peak   (on QRS detection)  
noise_level[n]  = 0.875 × noise_level[n−1]  + 0.125 × integrated         (on non-detection)
```

The 0.25 weighting places the threshold at the first quartile between noise and signal floors — this is the standard Pan-Tompkins ratio. A hard floor of `ECG_MIN_THRESHOLD_FLOOR` = 25 prevents false triggers when signal levels are very low at startup.

**Step 6 — Refractory period**

After any confirmed QRS event, detections are suppressed for `ECG_REFACTORY_MS` = 250 ms. At the maximum valid rate (`ECG_MAX_BPM` = 220 BPM, RR ≈ 272 ms) this guard does not block genuine beats.

### 2.4 R-peak refinement — deferred bidirectional bandpassed search

The MWI pipeline introduces two sources of timing error relative to the true R-peak:

1. **MWI envelope lag**: the 150 ms integration window centres on the QRS energy, placing its peak ~75 ms after the true R-peak apex.
2. **Early triggering on P-wave**: with any finite bandpass lower cutoff, residual P-wave energy (fundamental ~4–8 Hz) can raise the MWI above threshold before the QRS arrives. With the 10 Hz cutoff this is greatly reduced; the deferred search provides a second line of defence for cases where it still occurs.

The firmware implements a **deferred bidirectional search on the bandpassed signal** rather than an immediate backward search on raw ADC values. Using the bandpassed buffer is essential: in the raw ADC domain the P-wave amplitude and R-peak amplitude are on the same scale (both depend on absolute ADC counts), whereas in the bandpassed domain the QRS response is typically 5–10× larger than the P-wave response because QRS slopes are much steeper.

**Mechanism**

When the MWI fires (local peak above threshold, outside refractory period):

1. Signal level is updated; `ppg_sim_on_beat(expected_ms, rr_estimate)` is called immediately so the PPG simulation waveform generation stays synchronised.
2. A pending beat state is armed: `s_ecg_pending_beat = true`, and the ring buffer index and sample count at MWI fire are recorded (`s_ecg_beat_detect_idx`, `s_ecg_beat_detect_sample`).
3. The refractory timer is set from the MWI timestamp: `s_ecg_last_peak_ms = expected_ms`.

On each subsequent sample, a deferred check fires once `ECG_RPEAK_FORWARD_SAMPLES` = 20 ticks (200 ms) have elapsed since MWI fire. At that point the true R-peak is guaranteed to be in the ring buffer regardless of whether detection fired early or late. The search then runs:

```
for k in [−ECG_RPEAK_BACKWARD_SAMPLES .. +ECG_RPEAK_FORWARD_SAMPLES]:
    idx = (detect_idx + ECG_WINDOW_SAMPLES + k) % ECG_WINDOW_SAMPLES
    track argmax s_ecg_band[idx]

r_peak_expected = s_ecg_expected_ms[rpeak_idx]
```

`ECG_RPEAK_BACKWARD_SAMPLES` = 10 (100 ms back from detection) and `ECG_RPEAK_FORWARD_SAMPLES` = 20 (200 ms forward) give a 300 ms total search window centred slightly ahead of detection.

**Recording-relative timestamp**

`r_peak_expected` is in the `expected_ms` domain (milliseconds since `ecg_sampler_task` started). To produce a recording-relative timestamp comparable to `time_ms` in the CSV:

```
r_peak_ms (CSV) = r_peak_expected + s_ecg_task_start_ms − svc_rec_get_start_ms()
```

where `s_ecg_task_start_ms` = `esp_timer_get_time() / 1000` captured once at task startup. This converts the sample-count timeline to the same wall-clock epoch as `time_ms`.

**CSV timing**: because the deferred check fires 200 ms after MWI, the CSV row carrying non-zero `rr_ms`, `pat_ms`, and `r_peak_ms` values appears ~200 ms after the actual R-peak. `r_peak_ms` itself points back to the true apex; `time_ms` on that row is ~200 ms later. This is by design.

### 2.5 Heart rate estimation

HR is computed in the deferred R-peak search (§2.4), from refined R-peak timestamps stored in `s_ecg_last_rpeak_expected`. Using the deferred refined timestamps rather than the MWI fire timestamps gives a more accurate RR interval, particularly when the MWI fires early (P-wave case).

```
rr_ms     = r_peak_expected[N] − s_ecg_last_rpeak_expected[N−1]  
bpm_inst  = 60000 / rr_ms
```

Accepted only when `ECG_MIN_BPM` ≤ `bpm_inst` ≤ `ECG_MAX_BPM` (35–220 BPM). Applied to the running estimate with a 3:1 smoothing ratio:

```
hr_bpm = (hr_bpm × 3 + bpm_inst) / 4     (once initialised)  
hr_bpm = bpm_inst                          (on first valid beat)
```

This EMA (effective time constant ≈ 3 beats) rejects single-beat artefacts while still tracking genuine rate changes within a few seconds. `hr_bpm` is protected by `s_ecg_spinlock` and published to the topbar and CSV. The display update is delayed by `ECG_RPEAK_FORWARD_SAMPLES` × 10 ms = 200 ms relative to the beat, which is imperceptible at normal resting heart rates.

### 2.6 Respiration rate estimation

Respiration modulates the ECG baseline at a much slower rate than the heartbeat. The firmware exploits this via **delayed-signal intersection counting**:

1. Maintain a ring buffer `s_resp_history[]` of `RESP_RATE_WINDOW_SECONDS × ECG_SAMPLE_HZ` = 2000 samples (20 s).

2. Count samples where the current signal crosses the delayed signal (a zero-crossing of the difference), subject to:

   - A minimum inter-crossing gap of `RESP_DUP_DELAY_MS` = 200 ms (prevents rapid re-crossing artefacts).

   - A minimum crossing elevation of `RESP_DUP_ELEVATION` = 25 counts (suppresses crossings near the flat baseline).

3. One crossing per breath cycle → `resp_bpm = crossings × (60 / RESP_RATE_WINDOW_SECONDS)`.

Valid range: `RESP_MIN_BPM`–`RESP_MAX_BPM` = 4–60 BPM.

> **Stage 2 note**: the nasal thermistor (NTC on ADS127L18 ch3) and ERB chest band (ch4) will provide direct respiratory signals. The intersection-counting algorithm will be applied to those channels; the ECG-derived respiration estimate may be retained as a fallback.


## 3. PPG Processing Pipeline

### 3.1 Two-Gaussian waveform model (Stage 1 simulation)

The simulated PPG is generated by `ppg_sim_get_sample()` in [main/signal/sig_ppg.c](file:///home/benbrandwood/Documents/dev/tulog-biowatch/main/signal/sig_ppg.c). The model is based on the two-Gaussian decomposition of the photoplethysmogram described in Charlton *et al.*, *Scientific Reports* 2020:

```
ppg(t) = A₁ · exp(−(t − μ₁)² / (2σ₁²)) + A₂ · exp(−(t − μ₂)² / (2σ₂²))
```

Where `t` is time in ms measured from the PPG foot (onset of the systolic upstroke), and the parameters are expressed as fractions of the RR interval:

| Parameter | Value | Physiological interpretation |
| - | - | - |
| A₁ | 1.0 | Systolic peak amplitude (normalised) |
| μ₁ | `PPG_SIM_MU1_FRAC` × RR = 0.18 × RR | Systolic peak centre |
| σ₁ | `PPG_SIM_SIGMA1_FRAC` × RR = 0.08 × RR | Systolic peak width |
| A₂ | 0.35 | Diastolic/reflected wave amplitude |
| μ₂ | `PPG_SIM_MU2_FRAC` × RR = 0.45 × RR | Diastolic peak centre |
| σ₂ | `PPG_SIM_SIGMA2_FRAC` × RR = 0.12 × RR | Diastolic peak width |


The sum is normalised by `1 / (A₁ + A₂)` to the range [0, 1], then scaled to `REC_SIM_CENTRE ± REC_SIM_AMP` = 500 ± 420 (ADC counts) to match the other simulated channels.

The denominators `1 / (2σ²)` are pre-computed in `ppg_sim_on_beat()` (per beat, not per sample) to avoid repeated floating-point division in the 100 Hz sample loop.

### 3.2 PPG bandpass filter

Before the PPG sample is stored in `s_ppg_raw[]` or passed to the foot detector, it passes through the same `signal_bandpass_step()` function used for the ECG, with cutoffs tuned for PPG:

| Parameter | Value | Purpose |
| - | - | - |
| `PPG_BP_LOW_HZ` | 0.5 Hz | Removes DC baseline and respiratory baseline wander |
| `PPG_BP_HIGH_HZ` | 16 Hz | Removes high-frequency noise above the PPG bandwidth |

The filter states (`s_ppg_hp_state`, `s_ppg_lp_state`, `s_ppg_prev_input`) are separate from the ECG filter states and re-initialised at task start.

The filtered output is cast to `int32_t` and stored as `ppg_sample`. All downstream consumers — `s_ppg_raw[]` (chart display), `ppg_det_update_sample()` (foot detector), and `rec_row_t.ppg` (CSV) — receive the bandpass-filtered value. For Stage 2, the same filter is applied to raw MAX86140 FIFO reads before passing them to the detector, removing the DC pedestal inherent in optical PPG signals.

### 3.3 Beat synchronisation and PAT offset

Each QRS detection triggers `ppg_sim_on_beat(r_peak_ms, rr_ms)`, which:

1. Advances a Galois 16-bit LFSR (polynomial 0xB400) to produce pseudo-random beat-to-beat PAT jitter uniformly distributed over ±`PPG_SIM_PAT_JITTER_MS` = ±10 ms, simulating the physiological variability of vascular transit time.

2. Computes the foot timestamp: `beat_foot_ms = r_peak_ms + pat_ms_applied`.

3. Pre-computes `inv_2sig1sq` and `inv_2sig2sq` from the new RR interval.

4. Opens the foot-detector search window (§3.3).

`ppg_sim_get_sample(current_ms)` evaluates the two-Gaussian sum for `t = current_ms − beat_foot_ms`. Outside the beat window (t < 0 or t > RR), it returns `REC_SIM_CENTRE` (baseline).

### 3.4 PPG foot detection — first-derivative zero-crossing

The systolic foot (the onset of the rapid upstroke that begins each PPG pulse) is the physiologically correct reference point for PAT. It corresponds to the arrival of the pressure pulse at the measurement site.

The detector in `ppg_det_update_sample()` implements a first-derivative zero-crossing method:

**Search window**: opened by `ppg_sim_on_beat()` each R-peak; active from `r_peak_ms + 50 ms` to `r_peak_ms + 500 ms`. The 50 ms pre-delay excludes the ECG signal region; the 500 ms upper bound corresponds to the maximum physiological PAT (wrist, supine).

**Phase 1 — Upstroke tracking**

```
d[n] = ppg[n] − ppg[n−1]
```

While searching and `d > 0`, track the running maximum derivative `det_peak_deriv`. While `d ≤ 0`, record the current timestamp as `det_last_nonpos_ms` (a candidate for the last pre-upstroke zero-crossing). Declare the derivative peaked once `d < 0.5 × det_peak_deriv` (past the inflection).

**Phase 2 — Foot timestamp**

After the derivative has peaked, the next sample where `d ≤ 0` (derivative returning to non-positive) marks the systolic peak crossing. The foot is then:

```
foot_ms = det_last_nonpos_ms + ECG_SAMPLE_PERIOD_MS
```

This places the foot one sample after the last non-positive derivative — the sample immediately before the upstroke began. A minimum peak derivative guard (`det_peak_deriv > 2.0`) prevents triggering on noise when no PPG signal has been received.

**Validation**: detected PAT is accepted only when `50 ≤ pat_raw ≤ 500 ms` (physiological plausible range for wrist PPG).

> **Stage 2 note**: the guard threshold `2.0` is calibrated to the simulated amplitude range (~420 counts). Real MAX86140 19-bit samples will produce derivatives orders of magnitude larger — the guard will remain non-blocking but can be tightened after characterising the real sensor. The algorithm itself is amplitude-independent.

### 3.5 PPG chart display

`rec_fill_ppg_plot()` copies `s_ppg_raw[]` (400 × int32_t ring buffer) under spinlock and applies dynamic autoscale:

```
y = (ppg[i] − ppg_min) × 1000 / (ppg_max − ppg_min + 1)
```

This mirrors the ECG chart normalisation exactly. Both `s_ppg_min` and `s_ppg_max` are tracked per-sample under spinlock in `ecg_sampler_task` and reset every `ECG_WINDOW_SAMPLES × 4` = 1600 samples (~16 s). The autoscale adapts to any sensor amplitude range, from the simulated 80–920 counts to MAX86140's 19-bit values, without code changes.


## 4. Pulse Arrival Time (PAT)

### 4.1 Definitions

| Term | Definition |
| - | - |
| **PAT** | Pulse Arrival Time — ECG R-peak → PPG foot at the measurement site |
| **PTT** | Pulse Transit Time — onset of left ventricular ejection → PPG foot. PTT = PAT − PEP |
| **PEP** | Pre-Ejection Period — onset of electrical systole (Q-wave) to onset of mechanical ejection (aortic valve opening). Measurable from FCG (Forcecardiography). |


PAT conflates PTT and PEP. Since PEP varies with cardiac contractility (sympathetic tone, heart rate, inotropic state), PAT-derived BP estimates require PEP correction to be accurate across physiological states. This is the motivation for including FCG in the Stage 2 sensor suite: `PTT = PAT − PEP`.

### 4.2 Haemodynamic basis

The Moens–Korteweg equation relates PTT to blood pressure:

```
v = √(Eh / 2ρR)
```

where `v` is pulse wave velocity, `E` is arterial wall Young's modulus, `h` is wall thickness, `ρ` is blood density, and `R` is arterial radius. Since PTT = path length / v:

```
PTT ∝ 1 / √(BP)  →  BP ≈ a / PTT² + b
```

The constants `a` and `b` are subject-specific and require periodic cuff calibration. Beat-to-beat *changes* in PTT (and therefore PAT) are clinically useful without calibration, particularly for detecting the large nocturnal BP surges that accompany OSA termination events (surges of 40–80 mmHg SBP).

### 4.3 Typical values

| Condition | Approximate PAT (wrist) |
| - | - |
| Normal resting, young adult | 180–250 ms |
| Hypertensive surge | 120–160 ms |
| Simulated default (`PPG_SIM_PAT_DEFAULT_MS`) | 200 ms |
| Simulated jitter (`PPG_SIM_PAT_JITTER_MS`) | ±10 ms |


### 4.4 PAT implementation — two outputs

The firmware maintains two separate PAT values at runtime:

**Topbar (averaged)**

`ppg_det_get_pat_avg_ms()` — an EMA over the last `PPG_PAT_AVG_BEATS` = 4 beats (~4 s at 75 BPM):

```
pat_avg[N] = (pat_avg[N−1] × (K−1) + pat_raw[N]) / K,  K = PPG_PAT_AVG_BEATS
```

This is displayed on the record screen topbar as "PAT Nms". The running average reduces beat-to-beat noise in the display without losing the multi-second trend, which is the clinically relevant timescale for PAT changes during apnoea events.

**CSV (beat-by-beat)**

`ppg_det_get_pat_last_ms()` — the most recently detected single-beat instantaneous PAT, written to `rec_row_t.pat_ms` at the R-peak timestamp of the next beat (1-beat lag). Between beats, `pat_ms` is zero. This provides the full beat-to-beat PAT sequence for offline analysis.

`rec_row_t.rr_ms` is written at the same R-peak event (also zero between beats), so PAT and RR interval for each beat are colocated in the CSV and can be directly associated.

### 4.5 PAT accuracy requirements

The deferred bidirectional R-peak search (§2.4) is essential for accurate absolute PAT. Without it, two failure modes corrupt the timestamp:

- **Detection lag** (MWI fires after R-peak): the timestamp trails the true R-peak by ~85 ms, causing a true PAT of 200 ms to measure as ~115 ms — a 43% underestimate.
- **Early detection** (MWI fires on P-wave before R-peak): the timestamp precedes the true R-peak; combined with the PPG foot that follows the real R-peak, PAT appears artificially long and inconsistent.

Both cases are addressed by searching the bandpassed ring buffer `s_ecg_band[]` bidirectionally ±300 ms around the detection point after waiting for the R-peak to enter the buffer. Beat-to-beat PAT *variability* is preserved in all cases (timing biases are per-beat), but absolute accuracy requires correct R-peak timestamps for cuff-calibrated BP estimation.

With the deferred bandpassed search, R-peak timestamp accuracy at 100 Hz is ±10 ms (±1 sample). At Stage 2's 1000 Hz ADS127L18 sampling this improves to ±1 ms. PPG foot detection accuracy using first-derivative zero-crossing is ±1 sample period of the PPG sampling rate.

### 4.6 Clock domain contract

For accurate PAT, R-peak and PPG foot timestamps must both use `esp_timer_get_time()`, the same monotonic clock at the same origin:

```
r_peak_expected  = s_ecg_expected_ms[rpeak_idx]             (ECG ring buffer, expected_ms domain)  
foot_ms          = det_last_nonpos_ms + ECG_SAMPLE_PERIOD_MS  (PPG detector, same domain)  
PAT              = foot_ms − r_peak_expected
```

The CSV column `r_peak_ms` is recording-relative (same epoch as `time_ms`), not in the `expected_ms` domain. The conversion is:

```
r_peak_ms (CSV) = r_peak_expected + s_ecg_task_start_ms − svc_rec_get_start_ms()
```

`s_ecg_task_start_ms` is captured once as `esp_timer_get_time() / 1000` when `ecg_sampler_task` starts. This ensures that `r_peak_ms` in the CSV refers to the same wall-clock epoch as `time_ms`, allowing direct comparison: on the row where `r_peak_ms` is non-zero, the R-peak occurred at `r_peak_ms` ms from recording start, and the row itself was written at `time_ms` ms (~200 ms later due to the deferred search).

When integrating the Stage 2 MAX86140 driver, each SPI FIFO read must be timestamped with `esp_timer_get_time()` at the moment of the read, and that value (divided by 1000) passed as `current_ms` to `ppg_det_update_sample()`. Mixing the MAX86140's internal oscillator timestamp with the ECG `expected_ms` timeline would produce PAT values that are systematically wrong and drift over time.


## 5. Stage 2 Signal Paths (Planned)

### 5.1 ECG — ADS127L18 via SPI2

The ADS127L18 is a 24-bit signed twos-complement output (range −8,388,608 to +8,388,607). The ECG raw ring buffer `s_ecg_raw[]` stores `int32_t` and the CSV column `ecg` is `int32_t` — both hold the full 24-bit value without truncation. The autoscale chart normalisation handles any amplitude range dynamically.

The `ecg_adc_read_raw()` abstraction (`#if ECG_USE_SIMULATED_SOURCE`) is the integration seam: replace the simulated path with a read from the ADS127L18 SPI buffer. The QRS detection pipeline, R-peak search, HR, and respiration algorithms are signal-level agnostic.

### 5.2 PPG — MAX86140 via SPI3

MAX86140 outputs 19-bit unsigned samples (0–524,287). `s_ppg_raw[]` stores `int32_t` and `rec_row_t.ppg` is `int32_t` — both accommodate the full range.

Integration changes required:

1. Replace `ppg_sim_get_sample(expected_ms)` with a read from the MAX86140 FIFO — likely in a separate SPI interrupt handler or task rather than inline in `ecg_sampler_task`.

2. Timestamp each FIFO read with `esp_timer_get_time()` and write `(uint32_t)(read_us / 1000)` as `current_ms` to `ppg_det_update_sample()`.

3. Keep calling `ppg_sim_on_beat(r_peak_ms, rr_ms)` from the QRS detector — this opens the foot-detector search window (the waveform generation part of the function becomes irrelevant once `ppg_sim_get_sample()` is no longer called, but the window-opening logic is still used by the detector).

4. The PPG chart autoscale and foot detector will work without modification.

5. `ppg_sim_get_pat_ms()` (simulation ground truth) can be retired; the validation log comparing it to the detector can be removed or repurposed for MAX86140 SNR monitoring.

### 5.3 FCG — ADS127L18 ch0/ch1

FCG channels are stored as `int16_t` in `rec_row_t` (±32,767). ADS127L18 24-bit values should be scaled to 16-bit at the driver layer (right-shift by 8). If full 24-bit FCG resolution is required for PEP extraction, `fcg1`/`fcg2` fields should be widened to `int32_t` following the same pattern as `ecg` and `ppg`.

PEP extraction from FCG involves detecting the onset of the low-frequency cardiomechanical signal relative to the ECG Q-wave. This algorithm is not yet implemented.

### 5.4 SpO₂ — MAX86140

SpO₂ is derived from the ratio of red to infrared (IR) photoplethysmographic signals:

```
R = (AC_red / DC_red) / (AC_IR / DC_IR)  
SpO₂ ≈ a − b × R    (empirical calibration, typically a ≈ 110, b ≈ 25)
```

The `spo2` field in `rec_row_t` is `uint8_t` (percentage 0–100) and the topbar label `s_lbl_rec_spo2` is already present and initialised to "SpO2 --%". Wiring in the real value requires computing the AC and DC components from the MAX86140 FIFO and writing the result to `row.spo2`.


## 6. Data Recording Format

### 6.1 CSV columns

| Column | Type | Units | Description |
| - | - | - | - |
| `time_ms` | uint32 | ms | Recording elapsed time from `svc_rec_start()` |
| `ecg` | int32 | ADC counts | Raw ECG sample (12-bit sim; 24-bit ADS127L18 in Stage 2) |
| `ppg` | int32 | ADC counts | Raw PPG sample (simulated; 19-bit MAX86140 in Stage 2) |
| `resp` | int16 | ADC counts | Respiratory signal (simulated sine; ERB/NAS in Stage 2) |
| `nas` | int16 | ADC counts | Nasal thermistor (simulated; ADS127L18 ch3 in Stage 2) |
| `fcg1` | int16 | ADC counts | FCG channel 1 (simulated; ADS127L18 ch0 in Stage 2) |
| `fcg2` | int16 | ADC counts | FCG channel 2 (simulated; ADS127L18 ch1 in Stage 2) |
| `drift_ms` | int32 | ms | `actual_ms − expected_ms` cumulative wall-clock jitter |
| `batt_v` | float | V | Battery voltage from AXP2101 |
| `spo2` | uint8 | % | SpO₂ (always 0 in Stage 1) |
| `resp_rate` | uint8 | BPM | Estimated respiration rate |
| `hr_bpm` | uint8 | BPM | Estimated heart rate |
| `rr_ms` | int16 | ms | RR interval (refined R-peak to refined R-peak); 0 between beats |
| `pat_ms` | int16 | ms | Detected PAT (previous beat's foot; 1-beat lag); 0 between beats |
| `r_peak_ms` | uint32 | ms | Recording-relative timestamp of the detected R-peak apex; 0 between beats |


### 6.2 Beat-synchronised fields

`rr_ms`, `pat_ms`, and `r_peak_ms` are non-zero only on beat-event rows. All other rows carry zero. Because of the deferred R-peak search (§2.4), the beat-event row appears ~200 ms after the actual R-peak in the recording; `r_peak_ms` gives the true R-peak time regardless.

```
beats = df[df['rr_ms'] != 0]       # isolate beat-event rows  
pat_series    = beats['pat_ms']      # beat-by-beat PAT (ms)  
rr_series     = beats['rr_ms']       # beat-by-beat RR interval (ms)  
rpeak_times   = beats['r_peak_ms']   # true R-peak time from recording start (ms)
```

To verify R-peak detection accuracy in a recording, compare `r_peak_ms` against the `time_ms` of the surrounding row with the highest `ecg` value — they should align within ±10 ms (±1 sample at 100 Hz).

### 6.3 Drift interpretation

`drift_ms` is a cumulative quantity: it grows monotonically if the actual sample rate is slightly above the target, and shrinks if below. It does not represent instantaneous jitter. Use the first-difference of `drift_ms` to recover the per-sample timing error:

```
jitter_ms = df['drift_ms'].diff()  # per-sample deviation from 10 ms ideal
```


## 7. Key References

- Pan, J. & Tompkins, W.J. (1985). A real-time QRS detection algorithm. *IEEE Transactions on Biomedical Engineering*, 32(3), 230–236.

- Charlton, P.H. *et al.* (2020). Breathing rate estimation from the electrocardiogram and photoplethysmogram: A review. *IEEE Reviews in Biomedical Engineering*, 11, 2–20.

- *Charlton, P.H. et al. (2019). Modeling arterial pulse waves in healthy aging: a database for in silico evaluation of haemodynamics and pulse wave indexes.* American Journal of Physiology-Heart and Circulatory Physiology, *317(5), H1062–H1085.

- Moens, A.I. (1878). Die Pulskurve. Leiden: E.J. Brill.

- Allen, J. (2007). Photoplethysmography and its application in clinical physiological measurement. *Physiological Measurement*, 28(3), R1–R39.

- Pinheiro, N. *et al.* (2010). Can PTT be used to measure blood pressure? *The Proceedings of World Congress on Medical Physics and Biomedical Engineering*, 491–494.

