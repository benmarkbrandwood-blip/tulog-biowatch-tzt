#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Bitmask flags for biosignal_frame_t.valid_mask */
#define BIOSIG_VALID_ECG1       (1u << 0)
#define BIOSIG_VALID_ECG2       (1u << 1)
#define BIOSIG_VALID_PPG_IR     (1u << 2)
#define BIOSIG_VALID_PPG_RED    (1u << 3)
#define BIOSIG_VALID_SPO2       (1u << 4)
#define BIOSIG_VALID_ACCEL_X    (1u << 5)
#define BIOSIG_VALID_ACCEL_Y    (1u << 6)
#define BIOSIG_VALID_ACCEL_Z    (1u << 7)
#define BIOSIG_VALID_RESP_NASAL (1u << 8)
#define BIOSIG_VALID_RESP_CHEST (1u << 9)
#define BIOSIG_VALID_ECG3       (1u << 10)

/**
 * biosignal_frame_t — unified sample from all biosensors.
 *
 * valid_mask is 0 until real sensors are wired (Stage 1+).  Consumers must
 * test the relevant BIOSIG_VALID_* bit before trusting a field value.
 */
typedef struct {
    int32_t  ecg1_raw;
    int32_t  ecg2_raw;
    int32_t  ecg3_raw;
    int32_t  ppg_ir_raw;
    int32_t  ppg_red_raw;
    int32_t  spo2_est;
    int32_t  accel_x_raw;
    int32_t  accel_y_raw;
    int32_t  accel_z_raw;
    int32_t  resp_nasal_raw;
    int32_t  resp_chest_raw;
    uint32_t sample_index;
    uint64_t timestamp_us;
    uint32_t valid_mask;
} biosignal_frame_t;

/**
 * svc_biosignal_acq_init — initialise acquisition state.
 * Call once from app_main after hal_bus_init().
 */
esp_err_t svc_biosignal_acq_init(void);

/**
 * svc_biosignal_acq_step_100hz — called every 10 ms from ecg_sampler_task.
 * Stage 0: increments sample_index and timestamps only.
 * Stage 1+: reads sensor FIFOs / DRDYs.
 */
void svc_biosignal_acq_step_100hz(void);

/**
 * svc_biosignal_acq_get_latest — thread-safe copy of the most recent frame.
 */
void svc_biosignal_acq_get_latest(biosignal_frame_t *out);

/**
 * svc_biosignal_acq_sensor_ready — returns true when valid_mask includes bit.
 */
bool svc_biosignal_acq_sensor_ready(uint32_t valid_bit);
