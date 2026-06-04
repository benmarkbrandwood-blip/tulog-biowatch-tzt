#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_state.h"

/*
 * B.P. recording service — 1 kHz ECG/PPG snapshot with beat-synchronous fields.
 *
 * Architecture:
 *   bp_sampler_task  — Core BP_CORE_SAMPLER, priority BP_SAMPLER_PRIORITY.
 *                      Reads s_ecg_raw[]/s_ppg_raw[] ring-buffer head under
 *                      s_ecg_spinlock every 1 ms. In Stage 1 (simulated 100 Hz
 *                      source) values repeat 10x per 10 ms window; Stage 2 sensor
 *                      drivers will fill the ring buffer at sensor rate.
 *   sd_writer_task   — Core BP_CORE_WRITER. Dequeues bp_row_t and writes CSV.
 *
 * Queue storage is allocated from PSRAM (48 KB at BP_QUEUE_LEN = 2048).
 */

esp_err_t   svc_bp_rec_init(void);

/* duration_s: BP_DURATION_30S, BP_DURATION_60S, or BP_DURATION_120S */
esp_err_t   svc_bp_rec_start(uint32_t duration_s);

void        svc_bp_rec_stop(void);
void        svc_bp_rec_enqueue(const bp_row_t *row);
bool        svc_bp_rec_is_recording(void);
uint32_t    svc_bp_rec_get_start_ms(void);
uint32_t    svc_bp_rec_get_duration_s(void);

/* Returns path of last completed file, or empty string if none yet. */
const char *svc_bp_rec_get_filename(void);

/* Post-recording analysis — call after svc_bp_rec_is_recording() is false.
 * Reads rr_ms/pat_ms columns from the completed CSV and computes RMSSD and
 * PAT variance. Populates out->rr_series[] / pat_series[] for chart display. */
esp_err_t bp_analyse_file(const char *path, bp_analysis_t *out);
