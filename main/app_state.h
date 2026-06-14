#pragma once

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Recording                                                                  */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t time_ms;
    int32_t  ecg;              /* Lead I  (ADS1293 CH1) */
    int32_t  ecg2;             /* Lead II (ADS1293 CH2) */
    int32_t  ecg3;             /* Lead III (ADS1293 CH3) */
    int32_t  ppg;              /* int32_t: holds 19-bit MAX86140 samples without truncation */
    int16_t  resp, nas, fcg1;
    int16_t  accel_x, accel_y, accel_z;  /* MPU-6050 raw; divide by 16384 × 9.81 for m/s² */
    int32_t  drift_ms;
    uint8_t  batt_pct;  /* E-Gauge percent, written every ~30 s, 0 between updates */
    uint8_t  spo2;
    uint8_t  resp_rate;
    uint8_t  hr_bpm;
    int16_t  rr_ms;       /* RR interval at beat detection, 0 between beats */
    int16_t  pat_ms;      /* detected PAT from previous beat's foot, 0 between beats */
    uint32_t r_peak_ms;   /* expected_ms of the detected R-peak, 0 between beats */
} rec_row_t;

/* -------------------------------------------------------------------------- */
/* B.P. recording                                                             */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t time_ms;    /* expected_ms (monotonic from bp_sampler_task start) */
    int32_t  ecg;        /* raw ECG value (ring-buffer head; real sensor in Stage 2) */
    int32_t  ppg;        /* raw PPG value (ring-buffer head; real sensor in Stage 2) */
    int32_t  drift_ms;   /* actual_ms - expected_ms */
    uint32_t r_peak_ms;  /* recording-relative R-peak timestamp; 0 between beats */
    int32_t  rr_us;      /* RR interval at beat (µs); 0 between beats */
    int32_t  pat_us;     /* single-beat PAT (µs); 0 between beats */
} bp_row_t;

typedef struct {
    float    hrv_rmssd_us;     /* RMSSD of successive RR differences (µs) */
    float    pat_variance_us2; /* variance of beat-to-beat PAT (µs^2) */
    float    pat_mean_us;      /* mean PAT (µs) */
    uint32_t beat_count;       /* total beats found in file */
    bool     valid;            /* false if fewer than 4 beats or file error */
    int32_t  rr_series[64];    /* last up to 64 beat-by-beat RR values (µs) for chart */
    int32_t  pat_series[64];   /* last up to 64 beat-by-beat PAT values (µs) for chart */
} bp_analysis_t;

typedef enum {
    REC_TAB_ECG = 0, REC_TAB_PPG, REC_TAB_RESP,
    REC_TAB_NAS, REC_TAB_FCG1, REC_TAB_ACCEL,
    REC_TAB_COUNT
} rec_tab_t;

/* -------------------------------------------------------------------------- */
/* Health / signal tabs                                                       */
/* -------------------------------------------------------------------------- */

typedef enum {
    HEALTH_TAB_ECG = 0,
    HEALTH_TAB_PPG,
    HEALTH_TAB_RESP,
    HEALTH_TAB_NAS,
    HEALTH_TAB_FCG,
    HEALTH_TAB_COUNT
} health_tab_t;

/* -------------------------------------------------------------------------- */
/* Files / Wi-Fi transfer                                                     */
/* -------------------------------------------------------------------------- */

typedef enum {
    FILE_KIND_UNKNOWN = 0,
    FILE_KIND_RECORD,
    FILE_KIND_BP
} file_kind_t;

typedef struct {
    char        name[64];
    char        path[128];
    uint32_t    size_bytes;
    file_kind_t kind;
} watch_file_entry_t;

typedef struct {
    watch_file_entry_t items[64];
    uint16_t           count;
    bool               sd_ok;
} watch_file_list_t;

typedef struct {
    bool        active;
    bool        done;
    bool        success;
    uint32_t    bytes_sent;
    uint32_t    bytes_total;
    char        filename[64];
    char        message[96];
} file_tx_status_t;
