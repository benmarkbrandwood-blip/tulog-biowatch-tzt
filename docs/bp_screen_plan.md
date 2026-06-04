# B.P. Screen Implementation Plan (Revised)
**Project:** tulog-biowatch (ESP32-S3 AMOLED Watch)
**Date:** 2026-05-11 (revised after implementation-readiness review)
**Target files:** `main/main.c`, `main/app_config.h`, `main/app_state.h`,
                  `main/services/svc_bp_record.c/.h` (new)

---

## Overview

The B.P. tile on the home screen already routes to `s_scr_pump`, which is declared
but never created (`NULL`). The goal is to:

1. Build a new **`svc_bp_record`** service (modelled on `svc_record`) that samples
   ECG + PPG at 1000 Hz and writes a minimal CSV to SD card.
2. Build **`ui_create_bp_screen()`** in `main.c` that lets the user choose 30 s /
   1 min / 2 min duration, counts down during recording, then reads HRV (RMSSD of
   RR intervals) and PAT variance from the CSV and displays them as text labels and
   a beat-by-beat chart.
3. Wire `s_scr_pump` to this new screen.

### Stage 1 / Stage 2 data-source note

`ECG_USE_SIMULATED_SOURCE = 1` means `s_ecg_raw[]` and `s_ppg_raw[]` are updated
at 100 Hz by `ecg_sampler_task`. The 1 kHz BP sampler reads the ring-buffer head
under spinlock every 1 ms; between 100 Hz updates, 9 of 10 reads return the same
value. This is intentional and acceptable for Stage 1 — the CSV schema, service
API, and sampler task will not change in Stage 2. When the ADS127L18 (SPI2) and
MAX86140 (SPI3) drivers are integrated, `ecg_sampler_task` will feed `s_ecg_raw[]`
and `s_ppg_raw[]` at the sensor's native rate (target 1 kHz for ECG/FCG, 100–250 Hz
for PPG); the BP sampler task immediately benefits without modification.

**`ecg_simulate_raw()` is NOT thread-safe** — it uses shared `static` variables.
The BP sampler task must never call it directly. Only `ecg_sampler_task` calls it.

---

## Context: How the Record Screen Works (Reference Pattern)

| Concern | Record screen approach |
|---|---|
| Filename | `BSP_SD_MOUNT_POINT/YYYYMMDD_HHMMSS_mmm_<label>.csv` |
| Writer task | `sd_writer_task` pinned to Core 1 (`REC_CORE_WRITER = 1`) |
| Queue | `xQueueCreate(REC_QUEUE_LEN, sizeof(rec_row_t))` |
| Flush | Every 100 rows + `fclose` on stop |
| End sentinel | `# finished,YYYY-MM-DD HH:MM:SS.mmm\r\n` |
| Drift logging | `drift_ms = actual_ms - expected_ms` each sample |

The BP service keeps signal timing (`r_peak_ms`, `expected_ms`), drift, and finish
datetime, and drops everything else (resp, nas, fcg, batt, spo2, resp_rate).

---

## Phase 1 — Data structures, constants, service stub

### 1.1 New row type — add to `main/app_state.h`

```c
typedef struct {
    uint32_t time_ms;    /* expected_ms (monotonic from sampler task start) */
    int32_t  ecg;        /* raw ECG ADC value (simulated / ADS127L18 Stage 2) */
    int32_t  ppg;        /* raw PPG ADC value (simulated / MAX86140 Stage 2) */
    int32_t  drift_ms;   /* actual_ms - expected_ms (cumulative jitter) */
    uint32_t r_peak_ms;  /* recording-relative R-peak timestamp; 0 between beats */
    int16_t  rr_ms;      /* RR interval at beat; 0 between beats */
    int16_t  pat_ms;     /* single-beat PAT; 0 between beats */
} bp_row_t;
```

### 1.2 Analysis result type — add to `main/app_state.h`

```c
typedef struct {
    float    hrv_rmssd_ms;     /* RMSSD of successive RR differences (ms) */
    float    pat_variance_ms2; /* variance of beat-to-beat PAT (ms^2) */
    float    pat_mean_ms;      /* mean PAT (ms) */
    uint32_t beat_count;       /* total beats in file */
    bool     valid;            /* false if fewer than 4 beats or file error */
    int16_t  rr_series[64];    /* beat-by-beat RR values for chart (last 64 beats) */
    int16_t  pat_series[64];   /* beat-by-beat PAT values for chart (last 64 beats) */
} bp_analysis_t;
```

### 1.3 New config constants — add to `main/app_config.h`

```c
/* ---- B.P. recording ---------------------------------------------------- */
#define BP_SAMPLE_HZ              1000
#define BP_SAMPLE_PERIOD_US       (1000000 / BP_SAMPLE_HZ)   /* 1000 µs */
#define BP_CORE_SAMPLER           1   /* Core 1: avoids interfering with ecg_sampler on Core 0 */
#define BP_CORE_WRITER            1
#define BP_SAMPLER_PRIORITY       6   /* same as sd_writer_task; both blocked most of the time */
#define BP_QUEUE_LEN              2048 /* ~2 s at 1 kHz; allocated from PSRAM */
#define BP_WRITE_DELAY_US         200  /* tighter than REC_WRITE_DELAY_US; smaller rows */
#define BP_IOBUF_SIZE             8192 /* larger stdio buffer for 1 kHz throughput */
#define BP_FLUSH_ROWS             200  /* flush every 200 rows (~0.2 s) */
#define BP_ROW_BUF                64   /* max bytes per CSV row */
#define BP_DURATION_30S           30
#define BP_DURATION_60S           60
#define BP_DURATION_120S          120
#define BP_BATT_STOP_PCT          5
#define BP_ANALYSIS_STACK_BYTES   8192  /* file parse + sqrt; generous for stdio internals */
```

### 1.4 New service files

```
main/services/svc_bp_record.h
main/services/svc_bp_record.c
```

Add both to `main/CMakeLists.txt` under `SRCS`.

### 1.5 `svc_bp_record.h` — public API

```c
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_state.h"

esp_err_t   svc_bp_rec_init(void);
esp_err_t   svc_bp_rec_start(uint32_t duration_s);   /* 30, 60, or 120 */
void        svc_bp_rec_stop(void);
void        svc_bp_rec_enqueue(const bp_row_t *row);
bool        svc_bp_rec_is_recording(void);
uint32_t    svc_bp_rec_get_start_ms(void);
uint32_t    svc_bp_rec_get_duration_s(void);
const char *svc_bp_rec_get_filename(void);    /* path of last completed file */
```

---

## Phase 2 — Service implementation (`svc_bp_record.c`)

Model directly on `svc_record.c`. Key differences noted below.

### 2.1 CSV header

```c
fprintf(s_bp_file,
    "time_ms,ecg,ppg,drift_ms,r_peak_ms,rr_ms,pat_ms\r\n");
```

### 2.2 Filename convention (matches Record screen)

```c
snprintf(s_bp_filename, sizeof(s_bp_filename),
    BSP_SD_MOUNT_POINT "/%04d%02d%02d_%02d%02d%02d_%03d_bp.csv",
    tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
    tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
```

### 2.3 Writer task row format

```c
snprintf(line, sizeof(line),
    "%lu,%ld,%ld,%ld,%lu,%d,%d\r\n",
    (unsigned long)row.time_ms,
    (long)row.ecg, (long)row.ppg, (long)row.drift_ms,
    (unsigned long)row.r_peak_ms,
    (int)row.rr_ms, (int)row.pat_ms);
```

### 2.4 Auto-stop on duration

Duration is checked inside `svc_bp_rec_enqueue()` (or the writer task loop):

```c
uint32_t elapsed_ms = (uint32_t)(esp_timer_get_time()/1000ULL) - s_bp_start_ms;
if (elapsed_ms >= s_bp_duration_s * 1000UL) {
    s_bp_recording = false;
}
```

The sampler task checks `s_bp_recording` each iteration and self-deletes when false.

### 2.5 Queue — allocate from PSRAM

`sizeof(bp_row_t) = 24 bytes`; 2048 × 24 = 48 KB. Allocate the queue storage from
PSRAM to avoid internal SRAM pressure:

```c
static StaticQueue_t s_bp_queue_struct;
static uint8_t      *s_bp_queue_storage = NULL;   /* from PSRAM */

s_bp_queue_storage = heap_caps_malloc(
    BP_QUEUE_LEN * sizeof(bp_row_t), MALLOC_CAP_SPIRAM);
s_bp_queue = xQueueCreateStatic(BP_QUEUE_LEN, sizeof(bp_row_t),
                                 s_bp_queue_storage, &s_bp_queue_struct);
```

### 2.6 End sentinel (identical to Record screen)

```c
fprintf(s_bp_file,
    "# finished,%04d-%02d-%02d %02d:%02d:%02d.%03d\r\n",
    tm.tm_year+1900, ...);
```

---

## Phase 3 — 1 kHz sampler task (`bp_sampler_task`)

**Core and priority:** Core 1, priority `BP_SAMPLER_PRIORITY` (6). This keeps
it off Core 0 and away from `ecg_sampler_task` (Core 0, priority 8). The LVGL
port task and `sd_writer_task` are also on Core 1 but are blocked most of the
time; brief preemption from the sampler is safe.

**Timing:** Use the `next_us` + `esp_rom_delay_us` pattern from `ecg_sampler_task`
(NOT `vTaskDelayUntil` — that API operates in ticks and requires separate state).

**Data source:** Read the ring-buffer head under `s_ecg_spinlock`. In Stage 1 this
returns the latest 100 Hz sample (duplicated 9×/10ms). In Stage 2, when
`ecg_sampler_task` is updated to feed ADS127L18 data at 1 kHz, distinct values
appear naturally without any change to the BP sampler.

```c
static void bp_sampler_task(void *arg)
{
    int64_t start_us = esp_timer_get_time();
    int64_t next_us  = start_us;
    const int32_t period_us = BP_SAMPLE_PERIOD_US;  /* 1000 µs */
    uint32_t bp_sample_number = 0;

    while (s_bp_recording) {
        /* Wait for next 1 ms tick using the same pattern as ecg_sampler_task */
        int64_t now_us = esp_timer_get_time();
        if (now_us < next_us) {
            int32_t sleep_us = (int32_t)(next_us - now_us);
            if (sleep_us >= 1000) {
                vTaskDelay(pdMS_TO_TICKS(sleep_us / 1000));
            } else if (sleep_us > 50) {
                esp_rom_delay_us((uint32_t)sleep_us);
            }
        }

        uint32_t expected_ms = bp_sample_number;  /* 1 ms per sample at 1 kHz */
        uint32_t actual_ms   = (uint32_t)((esp_timer_get_time() - start_us) / 1000LL);

        bp_row_t row = {
            .time_ms   = expected_ms,
            .drift_ms  = (int32_t)actual_ms - (int32_t)expected_ms,
            .r_peak_ms = 0,
            .rr_ms     = 0,
            .pat_ms    = 0,
        };

        /* Read latest ECG/PPG and beat-event fields under spinlock.
         * In Stage 1: 100 Hz values repeat 10× per 10 ms window.
         * In Stage 2: ADS127L18/MAX86140 drivers feed these buffers at sensor rate. */
        portENTER_CRITICAL(&s_ecg_spinlock);
        row.ecg = s_ecg_raw[s_ecg_write_index];
        row.ppg = s_ppg_raw[s_ecg_write_index];
        /* Capture beat-event fields if a new beat was resolved this tick.
         * Uses the same deferred-search outputs as ecg_sampler_task. */
        if (s_ecg_last_rpeak_expected > 0) {
            /* Only write beat fields on the sample closest to the R-peak */
            /* (detailed beat-sync logic mirrors ecg_sampler_task's rec_rr_ms path) */
        }
        portEXIT_CRITICAL(&s_ecg_spinlock);

        svc_bp_rec_enqueue(&row);

        bp_sample_number++;
        next_us += period_us;
    }
    vTaskDelete(NULL);
}
```

> **Beat-sync fields note:** The simplest correct approach for Phase 3 is to pass
> beat events from `ecg_sampler_task` to `bp_sampler_task` via a lightweight
> `volatile` flag + struct (not a queue). When `ecg_sampler_task` resolves a
> deferred R-peak, it writes `r_peak_ms`, `rr_ms`, and `pat_ms` to a shared
> `s_bp_pending_beat` struct and sets a `volatile bool s_bp_beat_ready = true`.
> The BP sampler reads and clears this flag under spinlock on the next tick.
> This is simpler than a queue and avoids blocking.

---

## Phase 4 — Post-recording analysis

Runs on-device after `fclose`. Reads the completed CSV, extracts `rr_ms ≠ 0` rows,
computes RMSSD and PAT variance, stores results for UI display.

### 4.1 Function signature

```c
esp_err_t bp_analyse_file(const char *path, bp_analysis_t *out);
```

Place in `svc_bp_record.c` (or `signal/sig_bp_analysis.c` if preferred later).

### 4.2 Algorithm

```
1. Open file, skip header.
2. Scan rows: sscanf each line for 7 fields.
3. Collect rr_ms != 0 rows → rr[], pat[] (up to 512 entries; truncate if more).
4. RMSSD = sqrt( mean( (rr[i+1] - rr[i])^2 ) ), i = 0..N-2
5. PAT mean = sum(pat) / N
6. PAT variance = sum( (pat[i] - mean)^2 ) / N
7. Copy last min(N, 64) beats into out->rr_series[], out->pat_series[].
8. Set out->beat_count, out->valid (valid if N >= 4).
```

Memory: stack buffers `int16_t rr[512]`, `int16_t pat[512]` (2 KB total).
At 75 BPM over 120 s ≈ 150 beats; 512 is well over the maximum.

### 4.3 Analysis task

```c
static void bp_analysis_task(void *arg)
{
    static bp_analysis_t s_result;   /* static: keeps off stack */
    esp_err_t err = bp_analyse_file(svc_bp_rec_get_filename(), &s_result);
    if (err == ESP_OK && s_result.valid) {
        s_bp_last_result    = s_result;
        s_bp_analysis_ready = true;   /* polled by bp_ui_timer_cb */
    }
    vTaskDelete(NULL);
}
```

Spawned after recording stops; stack = `BP_ANALYSIS_STACK_BYTES` (8192).

---

## Phase 5 — B.P. Screen UI (`ui_create_bp_screen`)

### 5.1 Screen lifecycle and global objects (add to `main.c`)

```c
static lv_obj_t       *s_scr_bp            = NULL;
static lv_obj_t       *s_lbl_bp_status     = NULL;
static lv_obj_t       *s_lbl_bp_countdown  = NULL;
static lv_obj_t       *s_btn_bp_start      = NULL;
static lv_obj_t       *s_lbl_bp_btn        = NULL;
static lv_obj_t       *s_bp_chart          = NULL;
static lv_obj_t       *s_bp_chart_card     = NULL;
static lv_obj_t       *s_lbl_bp_hrv        = NULL;
static lv_obj_t       *s_lbl_bp_pat        = NULL;
static lv_timer_t     *s_bp_ui_timer       = NULL;
static int64_t         s_nav_bp_us         = 0;
static uint32_t        s_bp_chosen_dur_s   = BP_DURATION_60S;
static lv_chart_series_t *s_bp_rr_series   = NULL;
static lv_chart_series_t *s_bp_pat_series  = NULL;
static lv_coord_t      s_bp_chart_rr[64]   = {0};
static lv_coord_t      s_bp_chart_pat[64]  = {0};
static volatile bool   s_bp_analysis_ready = false;
static bp_analysis_t   s_bp_last_result    = {0};
```

### 5.2 Screen layout (410 × 502 px)

```
y=10   Title "B.P. Measurement"   (Montserrat 18, COLOUR_ACCENT)
y=44   Status label               (Montserrat 14, COLOUR_SUBTEXT)
y=70   Duration selector row      (3 buttons 110×52: 30s | 1min | 2min)
y=140  Countdown label            (Montserrat 48, COLOUR_TEXT, centred)
y=210  START/STOP button          (370×52, green→red on start)
y=280  Results card (400×80)      HRV RMSSD text + PAT mean / variance text
y=374  Chart card (410×100)       lv_chart: RR series (cyan) + PAT series (orange)
y=480  BOOT hint label            (Montserrat 12, COLOUR_SUBTEXT)
```

### 5.3 Chart description (clarified)

The chart plots two **beat-index series** (x = beat number):
- **Cyan series:** RR interval in ms per beat (typically 600–1000 ms)
- **Orange series:** PAT in ms per beat (typically 150–350 ms)

The scalar results (RMSSD, PAT variance, PAT mean) appear as **text labels** in
the results card above the chart. They are NOT plotted as a series. The chart
provides visual inspection of beat-to-beat variability; the labels give the
derived statistics.

Point count: `min(beat_count, 64)` — last 64 beats maximum.

Both series use `lv_coord_t` arrays (`s_bp_chart_rr[]`, `s_bp_chart_pat[]`).
When populating from `bp_analysis_t.rr_series[]` / `pat_series[]` (which are
`int16_t`), explicitly promote each element:

```c
for (uint32_t i = 0; i < n; i++) {
    s_bp_chart_rr[i]  = (lv_coord_t)s_bp_last_result.rr_series[i];
    s_bp_chart_pat[i] = (lv_coord_t)s_bp_last_result.pat_series[i];
}
```

### 5.4 UI timer guard

`bp_ui_timer_cb` fires at 1 Hz. Add an early-return guard so LVGL objects are
only touched while the BP screen is active:

```c
static void bp_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
#if LVGL_VERSION_MAJOR >= 9
    if (lv_screen_active() != s_scr_bp) return;
#else
    if (lv_scr_act() != s_scr_bp) return;
#endif
    /* ... countdown update, auto-stop check, analysis-ready check ... */
}
```

### 5.5 START / STOP button callback (`bp_startstop_btn_cb`)

```
ON START:
  1. svc_bp_rec_start(s_bp_chosen_dur_s) — returns ESP_OK or error
  2. Change button label to STOP (red)
  3. Hide duration selector buttons
  4. Show countdown label
  5. s_lbl_bp_status = "Recording…"
  6. Clear chart and result labels

ON STOP (manual):
  1. svc_bp_rec_stop()
  2. Change button label to START (green)
  3. Show duration selector buttons
  4. s_lbl_bp_status = "Analysing…"
  5. Spawn bp_analysis_task (stack BP_ANALYSIS_STACK_BYTES)
  6. s_bp_analysis_ready = false

ON ANALYSIS COMPLETE (polled by bp_ui_timer_cb via s_bp_analysis_ready):
  1. Display "HRV RMSSD: XX.X ms" in s_lbl_bp_hrv
  2. Display "PAT: XXX ms  σ²: XX.X ms²" in s_lbl_bp_pat
  3. Populate s_bp_chart_rr[] and s_bp_chart_pat[] from result
  4. Build/refresh chart (same deferred pattern as rec_build_chart)
  5. s_lbl_bp_status = "Saved: <filename>"
  6. s_bp_analysis_ready = false
```

### 5.6 Auto-stop from `bp_ui_timer_cb`

```c
if (svc_bp_rec_is_recording()) {
    uint32_t elapsed_s = (uint32_t)(
        (esp_timer_get_time()/1000ULL - svc_bp_rec_get_start_ms()) / 1000UL);
    /* update countdown label */
    if (elapsed_s >= s_bp_chosen_dur_s) {
        svc_bp_rec_stop();
        /* trigger analysis — same as manual stop path */
    }
    /* battery check */
    if (s_cached_batt_pct >= 0 && s_cached_batt_pct <= BP_BATT_STOP_PCT) {
        svc_bp_rec_stop();
        lv_label_set_text(s_lbl_bp_status, "Stopped: low battery");
    }
}
```

---

## Phase 6 — Wiring into `main.c`

### 6.1 Screen creation order (already correct)

`ui_create_bp_screen()` is called from `ui_create_app_screens()`, which runs
before `ui_create_home_screen()`. This ensures `s_scr_pump` is set before
`home_btn_event` user_data is registered:

```c
static void ui_create_app_screens(void)
{
    ui_create_settings_screen();
    ui_create_record_screen();
    ui_create_bp_screen();    /* ADD — must be before ui_create_home_screen() */
}
```

In `ui_create_bp_screen()`:
```c
s_scr_bp   = lv_obj_create(NULL);
s_scr_pump = s_scr_bp;   /* home_btn_event already uses s_scr_pump as user_data */
```

### 6.2 `home_btn_event` — add BP branch

```c
if (target == s_scr_bp) {
    s_nav_bp_us = esp_timer_get_time();
    bp_destroy_chart();
}
```

### 6.3 `app_main` — add init call after `svc_rec_init()`

```c
svc_bp_rec_init();
```

### 6.4 Forward declarations (near line 298)

```c
static void ui_create_bp_screen(void);
static void bp_startstop_btn_cb(lv_event_t *e);
static void bp_dur_btn_cb(lv_event_t *e);
static void bp_ui_timer_cb(lv_timer_t *timer);
static void bp_build_chart(void);
static void bp_destroy_chart(void);
```

---

## Phase 7 — CMakeLists.txt

```cmake
"services/svc_bp_record.c"
```

---

## Implementation Order

| Step | Phase | Action | Test |
|---|---|---|---|
| 1 | 1 | Add `bp_row_t`, `bp_analysis_t` to `app_state.h` | Build only |
| 2 | 1 | Add `BP_*` constants to `app_config.h` | Build only |
| 3 | 1 | Create `svc_bp_record.h` + stub `svc_bp_record.c` | Build only |
| 4 | 2 | Implement `svc_bp_rec_start/stop/enqueue/writer_task` | Flash — verify CSV on SD |
| 5 | 3 | Implement `bp_sampler_task` in `main.c` | Flash — verify 1 kHz rows in CSV |
| 6 | 4 | Implement `bp_analyse_file()` | Flash — verify RMSSD/PAT logged |
| 7 | 5 | Build `ui_create_bp_screen()` (layout only) | Flash — verify screen renders |
| 8 | 5 | Wire START/STOP button + countdown | Flash — verify countdown |
| 9 | 5/4 | Wire analysis task + result labels | Flash — verify metrics displayed |
| 10 | 5 | Add chart with dual series | Flash — verify plot renders |

---

## Key Risks and Mitigations (Updated)

| Risk | Mitigation |
|---|---|
| 100 Hz data repeated 10× at 1 kHz | Accepted in Stage 1. CSV schema unchanged for Stage 2 sensors. |
| `ecg_simulate_raw()` not thread-safe | BP sampler reads ring buffer under spinlock only — never calls simulate functions directly. |
| 48 KB queue in internal SRAM | Allocate via `heap_caps_malloc(…, MALLOC_CAP_SPIRAM)`; use `xQueueCreateStatic`. |
| bp_sampler_task interfering with ecg_sampler_task | Pinned to Core 1 (not Core 0). ecg_sampler_task owns Core 0 exclusively. |
| bp_ui_timer_cb touching LVGL on wrong screen | Early return: `if (lv_screen_active() != s_scr_bp) return;` |
| `lv_coord_t` vs `int16_t` mismatch in chart data | Explicit element-by-element cast when copying from `bp_analysis_t` to chart arrays. |
| bp_analysis_task stack overflow parsing 4 MB CSV | Stack = `BP_ANALYSIS_STACK_BYTES` = 8192; specified explicitly in `xTaskCreate`. |
| SD throughput at 1 kHz (~36 KB/s) | Within 1-bit SDMMC capability; 8 KB `setvbuf` absorbs bursts; 2-second queue buffer. |

---

## CSV Schema Reference

| Column | Type | Description |
|---|---|---|
| `time_ms` | uint32 | Expected sample timestamp (ms, monotonic from bp task start) |
| `ecg` | int32 | Raw ECG value (ring buffer head; distinct at Stage 2 sensor rate) |
| `ppg` | int32 | Raw PPG value (ring buffer head; distinct at Stage 2 sensor rate) |
| `drift_ms` | int32 | actual_ms − expected_ms (cumulative jitter at 1 kHz) |
| `r_peak_ms` | uint32 | Recording-relative R-peak timestamp; 0 between beats |
| `rr_ms` | int16 | RR interval at this beat (ms); 0 between beats |
| `pat_ms` | int16 | Single-beat PAT (ms); 0 between beats |
| *(footer)* | — | `# finished,YYYY-MM-DD HH:MM:SS.mmm` |
