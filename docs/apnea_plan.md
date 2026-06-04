# Onboard Apnea Analysis — Implementation Plan
**Project:** Tulog Biowatch (ESP32-S3 AMOLED, ESP-IDF)  
**Managed with:** Claude Code  
**Date:** 2026-05-13  

---

## 1. Objective

Implement real-time, onboard sleep apnea analysis that:
- Detects and classifies **Central Apnea (CA)**, **Obstructive Apnea (OA)**, **Mixed Apnea (MA)**, and **Hypopnea** events.
- Computes and displays a live **Apnea-Hypopnea Index (AHI)** during and at the end of a recording session.
- Integrates cleanly into the existing **Record Screen** UI.
- Logs timestamped events to the SD/flash file system for PC-side post-processing.
- Extends the existing MATLAB analysis pipeline (`Apnea_code_all_databases_plot_NASCorr-2.m`) to include PAT, SpO2, and HR contextual tracking.

---

## 2. Sensor Inputs & Signal Mapping

| Signal | Source | Existing Code Status | Role in Apnea |
|--------|--------|----------------------|---------------|
| **NAS** | Nasal airflow sensor (simulated respiration) | ✅ Respiration rate running | Primary airflow cessation detector |
| **ERB** | Electro-resistive band (chest/abdo effort) | ❌ To be added | Differentiates CA vs OA vs MA |
| **ECG R-peaks / RRI** | AD8232 / ADS127L18 | ✅ Pan-Tompkins running | RSA-derived respiration; HR & HRV |
| **PPG** | MAX86140 | ✅ Running | SpO2 desaturation; PTT leg |
| **PAT** (PTT from ECG→PPG) | Computed | ✅ Calculated | Arousal/effort marker; BP proxy |
| **FCG ch1 & ch2** | Wrist force cardiography | ❌ To be added | Thoracic effort (substitute THOR/ABDO); CA vs OA differentiation |

### Signal Priority for Detection
```
Airflow cessation  →  NAS (primary) + ERB (secondary)
Respiratory effort →  ERB + FCG ch1/ch2 (wrist thoracic proxy)
Oxygenation        →  PPG → SpO2 (3% desaturation criterion)
Cardiac markers    →  ECG RRI (RSA), PAT, HR
```

---

## 3. Apnea Classification Logic

### 3.1 Event Detection Criteria (AASM-aligned)

| Event | Airflow (NAS/ERB) | Effort (ERB/FCG) | Duration | SpO2 |
|-------|-------------------|------------------|----------|------|
| **Apnea (any)** | ≥90% reduction | — | ≥10 s | — |
| **Hypopnea** | 30–90% reduction | — | ≥10 s | ≥3% drop OR arousal |
| **Central Apnea** | ≥90% reduction | **absent** | ≥10 s | — |
| **Obstructive Apnea** | ≥90% reduction | **present** | ≥10 s | — |
| **Mixed Apnea** | ≥90% reduction | Absent then present | ≥10 s | — |

### 3.2 C Classification Algorithm (firmware pseudocode)

```c
// Apnea type classifier
// Inputs: nas_flow_reduced (bool), erb_effort (bool), fcg_effort (bool),
//         duration_s (float), spo2_drop (float)

typedef enum {
    APNEA_NONE = 0,
    APNEA_CENTRAL,
    APNEA_OBSTRUCTIVE,
    APNEA_MIXED,
    APNEA_HYPOPNEA
} apnea_type_t;

apnea_type_t classify_apnea(bool airflow_reduced_90,
                             bool airflow_reduced_30,
                             bool effort_present,
                             bool effort_initially_absent,
                             float duration_s,
                             float spo2_drop) {
    if (duration_s < 10.0f) return APNEA_NONE;

    if (airflow_reduced_90) {
        if (!effort_present) return APNEA_CENTRAL;
        if (effort_initially_absent && effort_present) return APNEA_MIXED;
        return APNEA_OBSTRUCTIVE;
    }
    if (airflow_reduced_30 && (spo2_drop >= 3.0f)) return APNEA_HYPOPNEA;

    return APNEA_NONE;
}
```

### 3.3 Effort Detection via FCG + ERB

- **ERB**: Band amplitude <10% of 30-second rolling baseline → effort absent.
- **FCG ch1/ch2**: Frequency content in 0.1–0.5 Hz band. If power drops >80% → effort absent. FCG acts as wrist-worn substitute for thoracic belt (THOR/ABDO channels in the MATLAB code).
- Mixed apnea: effort absent for first half of event, then resumes → hallmark of MA.

---

## 4. Signal Processing Pipeline (Onboard C)

### 4.1 Pre-processing (already exists or minimal addition needed)

```
NAS signal:
  → LP filter @ 0.4 Hz   (matches MATLAB: LP_filter(fs, NAF1, 0.4))
  → DC remove             (simple_dc)
  → Rolling peak detection (5-peak window baseline, same as MATLAB)
  → Amplitude % vs baseline → airflow_reduction float

ERB signal:
  → LP filter @ 0.4 Hz
  → DC remove
  → Rolling RMS or peak amplitude
  → effort_score float (0–1)

FCG ch1 & ch2:
  → BP filter 0.1–3 Hz (resp + cardiac)
  → 0.1–0.5 Hz band power → resp_effort_fcg
  → 0.8–3 Hz band power  → cardiac_component_fcg

ECG-derived respiration (already implemented):
  → RSA via R-peak amplitude variation (cubic spline interpolation)
  → Amplitude modulation component
  → Sum → ECG_resp signal
  → LP filter @ 0.4 Hz

SpO2 (PPG):
  → Rolling max peak (10 s window)
  → Drop detection: current < (last_peak - 3%) sustained > 3 s
```

### 4.2 Apnea Detection State Machine

```c
typedef enum {
    STATE_NORMAL,
    STATE_CANDIDATE,   // airflow reduced, timing
    STATE_CONFIRMED,   // ≥10s confirmed event
    STATE_RECOVERY
} apnea_state_t;

// Run every 1-second epoch
void apnea_fsm_tick(apnea_context_t *ctx) {
    float nas_reduction = compute_nas_reduction(ctx);
    bool  effort        = compute_effort(ctx);  // ERB + FCG
    float spo2_drop     = compute_spo2_drop(ctx);

    switch (ctx->state) {
        case STATE_NORMAL:
            if (nas_reduction >= 0.90f) {
                ctx->state = STATE_CANDIDATE;
                ctx->event_start_s = ctx->elapsed_s;
                ctx->initial_effort = effort;
            } else if (nas_reduction >= 0.30f && spo2_drop >= 3.0f) {
                ctx->state = STATE_CANDIDATE;  // hypopnea candidate
                ctx->event_start_s = ctx->elapsed_s;
            }
            break;

        case STATE_CANDIDATE:
            ctx->duration_s = ctx->elapsed_s - ctx->event_start_s;
            if (nas_reduction < 0.30f) {       // event ended
                if (ctx->duration_s >= 10.0f) {
                    apnea_type_t type = classify_apnea(
                        nas_reduction_peak >= 0.90f,
                        nas_reduction_peak >= 0.30f,
                        effort,
                        !ctx->initial_effort && effort,
                        ctx->duration_s,
                        spo2_drop);
                    log_event(ctx, type);
                    update_ahi(ctx, type);
                }
                ctx->state = STATE_RECOVERY;
            } else if (ctx->duration_s >= 10.0f) {
                ctx->state = STATE_CONFIRMED;  // ongoing event
            }
            break;

        case STATE_CONFIRMED:
            // Update live display every tick
            update_display_live_event(ctx);
            if (nas_reduction < 0.30f) {
                apnea_type_t type = classify_apnea(...);
                log_event(ctx, type);
                update_ahi(ctx, type);
                ctx->state = STATE_RECOVERY;
            }
            break;

        case STATE_RECOVERY:
            // Minimum 3 s before new event detection
            if (ctx->elapsed_s - ctx->event_end_s > 3.0f)
                ctx->state = STATE_NORMAL;
            break;
    }
}
```

---

## 5. AHI Computation

\[
\text{AHI} = \frac{N_{\text{CA}} + N_{\text{OA}} + N_{\text{MA}} + N_{\text{Hypopnea}}}{T_{\text{recording\_hours}}}
\]

```c
float compute_ahi(apnea_context_t *ctx) {
    float hours = ctx->elapsed_s / 3600.0f;
    if (hours < 0.001f) return 0.0f;
    return (float)(ctx->count_CA + ctx->count_OA +
                   ctx->count_MA + ctx->count_HYP) / hours;
}
```

**AHI severity thresholds** (for display colour coding):
| AHI | Severity | Display Colour |
|-----|----------|----------------|
| < 5 | Normal | Green |
| 5–14 | Mild | Yellow |
| 15–29 | Moderate | Orange |
| ≥ 30 | Severe | Red |

---

## 6. Record Screen UI Integration

### 6.1 Layout Additions to Record Screen

```
┌──────────────────────────────────────┐
│  ● REC  00:47:23          SpO2: 97% │
│  HR: 68 bpm    PAT: 142 ms          │
│  RespRate: 14 /min                  │
├──────────────────────────────────────┤
│  AHI: 4.2   [MILD]  ████░░░░░░  □  │
│                                      │
│  Events (last 3):                   │
│  ◆ 00:32:14  OA  18s               │
│  ◆ 00:28:45  HYP 12s  ▼SpO2 3.1%  │
│  ◆ 00:15:02  CA  22s               │
│                                      │
│  [  STOP  ]    [  MARK  ]          │
└──────────────────────────────────────┘
```

### 6.2 Display Update Intervals

| Element | Update Rate |
|---------|-------------|
| HR, SpO2, RespRate | Every 1 s |
| PAT | Every R-peak cycle |
| AHI | Every 60 s (recalculate) |
| Live event indicator | Every 1 s during active event |
| Event log (last 3) | On new event confirmation |

### 6.3 LVGL Widget Approach

- **AHI value**: `lv_label` with colour mapped to severity, updated every 60 s.
- **Live event badge**: `lv_obj` (rounded rect) shown during STATE_CONFIRMED, flashing, labelled `CA` / `OA` / `MA` / `HYP` with elapsed duration.
- **Event list**: Circular buffer of last 10 events; display last 3 in a `lv_list` or stacked `lv_label` rows.
- **PAT/SpO2/HR**: Existing metric labels — add PAT display alongside SpO2.
- **Session-end screen**: On STOP, full-screen AHI summary showing counts per type, total recording time, and min SpO2.

---

## 7. Data Structures

```c
// apnea_event.h

#define APNEA_EVENT_LOG_SIZE 256

typedef struct {
    uint32_t    timestamp_s;     // seconds from recording start
    apnea_type_t type;           // CA, OA, MA, HYP
    float       duration_s;
    float       spo2_nadir;      // lowest SpO2 during event
    float       pat_ms;          // PAT at event start
    float       hr_bpm;          // mean HR during event
} apnea_event_t;

typedef struct {
    apnea_event_t events[APNEA_EVENT_LOG_SIZE];
    uint16_t      count;
    uint16_t      count_CA;
    uint16_t      count_OA;
    uint16_t      count_MA;
    uint16_t      count_HYP;
    float         ahi_current;
    float         spo2_min;
    uint32_t      elapsed_s;
    apnea_state_t state;
    float         event_start_s;
    bool          initial_effort;
} apnea_context_t;
```

---

## 8. File System Logging Format

Append to session CSV on the SD card / SPIFFS:

```
# tulog_session_YYYYMMDD_HHMMSS.csv
timestamp_s,type,duration_s,spo2_nadir,pat_ms,hr_bpm,nas_reduction,erb_effort
0032.14,OA,18.3,95.2,148.2,72.1,0.97,1
0028.45,HYP,12.1,94.9,155.0,68.4,0.55,1
0015.02,CA,22.0,93.1,161.3,65.2,0.96,0
```

This feeds directly into the PC receiver and MATLAB post-processing pipeline.

---

## 9. MATLAB Pipeline Extension (PAT + SpO2 + HR)

Extend `Apnea_code_all_databases_plot_NASCorr-2.m` to add:

### 9.1 PAT Tracking

```matlab
% PAT = time from ECG R-peak to PPG systolic foot
% locs = R-peak sample indices (already computed)
% ppg_foot_locs = detect PPG feet (negative peaks after each R-peak)

PAT_ms = zeros(1, length(locs)-1);
for i = 1:length(locs)-1
    search_start = locs(i);
    search_end   = min(locs(i) + round(0.4*fs), length(PPG));
    [~, foot_idx] = min(PPG(search_start:search_end));
    PAT_ms(i) = (foot_idx / fs) * 1000;  % ms
end
PAT_time = tm(locs(1:end-1));

% Add subplot for PAT
subplot(5,1,5);
plot(PAT_time, PAT_ms, 'm');
title('Pulse Arrival Time (PAT)');
ylabel('PAT (ms)'); xlabel('Time (s)');
% Overlay apnea event markers (same xline logic as other subplots)
```

### 9.2 HR Overlay on SpO2 Plot

```matlab
% RRI already available from locs
RRI_s    = diff(locs) / fs;
HR_bpm   = 60 ./ RRI_s;
HR_times = tm(locs(2:end));

% Add to SAO2 subplot (right y-axis)
yyaxis right
plot(HR_times, HR_bpm, 'r--', 'LineWidth', 1);
ylabel('HR (bpm)');
```

### 9.3 AHI Post-hoc Calculation from Annotations File

```matlab
% Load scored events from Visual_scoring file (already parsed)
% Filter to apnea/hypopnea events with duration >= 10 s
valid_events = apnea_durations(apnea_durations >= 10);
recording_hours = tm(end) / 3600;
AHI_posthoc = length(valid_events) / recording_hours;
fprintf('Post-hoc AHI: %.1f events/hour\n', AHI_posthoc);
```

---

## 10. New Source Files to Create

| File | Location | Description |
|------|----------|-------------|
| `apnea_detector.h` | `main/include/` | Structs, enums, function declarations |
| `apnea_detector.c` | `main/` | FSM, classification, AHI, logging |
| `apnea_display.c` | `main/ui/` | LVGL widgets, Record screen integration |
| `apnea_display.h` | `main/include/ui/` | Display function declarations |
| `erb_signal.c` | `main/sensors/` | ERB ADC read, filtering, effort scoring |
| `fcg_signal.c` | `main/sensors/` | FCG ch1/ch2 ADC read, band power |
| `apnea_plan.md` | project root | This file |

---

## 11. Integration Points in Existing Code

- **`record_screen.c`**: Add `apnea_fsm_tick()` call in the 1-second timer callback; add LVGL widget init in screen init function.
- **`ppg_task.c`**: After SpO2 update, call `apnea_update_spo2(ctx, spo2_val)`.
- **`ecg_task.c`**: After R-peak detection, call `apnea_update_rpeak(ctx, rr_interval_ms)`.
- **`pat_calc.c`** (existing): Pass PAT value to `apnea_update_pat(ctx, pat_ms)`.
- **File logger**: Extend session file writer to include `apnea_event_t` serialisation.
- **CMakeLists.txt**: Add `apnea_detector.c`, `apnea_display.c`, `erb_signal.c`, `fcg_signal.c` to SRCS.

---

## 12. Development Phases

### Phase 1 — Sensor & Signal Foundation
- [ ] Add ERB ADC channel and `erb_signal.c` (LP filter, rolling baseline, effort score)
- [ ] Add FCG ch1/ch2 ADC channels and `fcg_signal.c` (band power 0.1–0.5 Hz)
- [ ] Validate both signals on Record screen debug overlay

### Phase 2 — Detection Engine
- [ ] Implement `apnea_detector.c` with FSM and classification logic
- [ ] Unit test classifier with synthetic and replay data
- [ ] Validate CA/OA/MA/HYP classification against MATLAB ground truth (excerpt2.mat annotations)

### Phase 3 — Display Integration
- [ ] Create LVGL widgets in `apnea_display.c`
- [ ] Integrate into Record screen: live event badge, AHI label, event log
- [ ] Session-end summary screen

### Phase 4 — Logging & Validation
- [ ] Extend CSV logger with apnea event columns
- [ ] Cross-validate onboard AHI vs MATLAB post-hoc AHI on same recording
- [ ] Threshold and sensitivity tuning based on validation results

### Phase 5 — MATLAB Extension
- [ ] Add PAT subplot to `Apnea_code_all_databases_plot_NASCorr-2.m`
- [ ] Add HR overlay on SpO2 subplot
- [ ] Add AHI post-hoc calculation from annotation file
- [ ] Add post-hoc CA/OA/MA classification using ERB/FCG proxy (THOR/ABDO channels)

---

## 13. Key Thresholds & Parameters (Tunable)

```c
// apnea_config.h
#define APNEA_AIRFLOW_CESSATION_THRESH   0.90f   // 90% reduction = apnea
#define APNEA_AIRFLOW_REDUCTION_THRESH   0.30f   // 30% reduction = hypopnea candidate
#define APNEA_SPO2_DROP_THRESH           3.0f    // % drop for hypopnea criterion
#define APNEA_MIN_DURATION_S             10.0f   // minimum event duration (AASM)
#define APNEA_MIN_RECOVERY_S             3.0f    // inter-event dead-zone
#define APNEA_EFFORT_ABSENT_THRESH       0.10f   // <10% effort baseline = absent
#define NAS_BASELINE_PEAKS               5       // rolling window for NAS baseline
#define NAS_LP_CUTOFF_HZ                 0.4f    // matches MATLAB LP_filter
#define ERB_LP_CUTOFF_HZ                 0.4f
#define FCG_RESP_BAND_LOW_HZ             0.1f
#define FCG_RESP_BAND_HIGH_HZ            0.5f
#define SAO2_EVENT_MIN_DURATION_S        3.0f
#define SAO2_PEAK_WINDOW_S               10.0f
```

---

## 14. References

- AASM Manual for Scoring Sleep (current edition) — event duration and reduction thresholds
- MATLAB prototype: `Apnea_code_all_databases_plot_NASCorr-2.m` — NAS correlation, ECG-derived respiration, SpO2 desaturation detection
- Existing firmware: Pan-Tompkins R-peak detection, PPG SpO2, PAT calculation (ECG→PPG foot)
- PhysioNet APNEA-ECG / UCDDB datasets — validation reference for CA/OA/MA classification
