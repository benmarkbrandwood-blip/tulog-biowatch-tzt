#include "svc_biosignal_acq.h"
#include "hal_ads1293.h"
#include "hal_max30102.h"
#include "app_config.h"

#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "svc_biosig";

static biosignal_frame_t s_latest;
static SemaphoreHandle_t s_lock;
static bool              s_ads1293_ok  = false;
static bool              s_max30102_ok = false;

esp_err_t svc_biosignal_acq_init(void)
{
    memset(&s_latest, 0, sizeof(s_latest));
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    esp_err_t err = hal_ads1293_init();
    if (err == ESP_OK) {
        s_ads1293_ok = true;
        ESP_LOGI(TAG, "ADS1293 online — ECG1/ECG2/ECG3 valid");
    } else {
        ESP_LOGW(TAG, "ADS1293 init failed (%s) — ECG channels remain simulated",
                 esp_err_to_name(err));
    }

    err = hal_max30102_init();
    if (err == ESP_OK) {
        s_max30102_ok = true;
        ESP_LOGI(TAG, "MAX30102 online — PPG IR/Red + SpO2 valid");
    } else {
        ESP_LOGW(TAG, "MAX30102 init failed (%s) — PPG remains simulated",
                 esp_err_to_name(err));
    }

    return ESP_OK;  /* non-fatal: system continues with simulated signals */
}

void svc_biosignal_acq_step_100hz(void)
{
    uint32_t idx = s_latest.sample_index + 1;
    uint64_t ts  = (uint64_t)esp_timer_get_time();

    int32_t ch1 = 0, ch2 = 0, ch3 = 0;
    int32_t ppg_ir = 0, ppg_red = 0;
    uint32_t new_valid = 0;

    if (s_ads1293_ok) {
        /* DRDY ISR removed (F0): reads unconditionally at 100 Hz.
         * ADS1293 at ~853 SPS always has fresh data. */
        static uint32_t s_ecg_log_ctr = 0;
        esp_err_t ecg_err = hal_ads1293_read_ecg(&ch1, &ch2, &ch3);
        if (ecg_err == ESP_OK) {
            new_valid |= BIOSIG_VALID_ECG1 | BIOSIG_VALID_ECG2 | BIOSIG_VALID_ECG3;
            if (++s_ecg_log_ctr % 100 == 0) {
                ESP_LOGI(TAG, "ECG raw: CH1=%ld CH2=%ld CH3=%ld",
                         (long)ch1, (long)ch2, (long)ch3);
            }
        }
    }

    if (s_max30102_ok) {
        if (hal_max30102_read_fifo(&ppg_ir, &ppg_red) == ESP_OK) {
            new_valid |= BIOSIG_VALID_PPG_IR | BIOSIG_VALID_PPG_RED | BIOSIG_VALID_SPO2;
        }
    }

    if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
        s_latest.sample_index = idx;
        s_latest.timestamp_us = ts;
        if (new_valid & (BIOSIG_VALID_ECG1 | BIOSIG_VALID_ECG2 | BIOSIG_VALID_ECG3)) {
            s_latest.ecg1_raw    = ch1;
            s_latest.ecg2_raw    = ch2;
            s_latest.ecg3_raw    = ch3;
        }
        if (new_valid & BIOSIG_VALID_PPG_IR) {
            s_latest.ppg_ir_raw  = ppg_ir;
            s_latest.ppg_red_raw = ppg_red;
            s_latest.spo2_est    = (int32_t)hal_max30102_get_spo2();
        }
        s_latest.valid_mask |= new_valid;
        xSemaphoreGive(s_lock);
    }
}

void svc_biosignal_acq_get_latest(biosignal_frame_t *out)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
        *out = s_latest;
        xSemaphoreGive(s_lock);
    } else {
        memset(out, 0, sizeof(*out));
    }
}

bool svc_biosignal_acq_sensor_ready(uint32_t valid_bit)
{
    return (s_latest.valid_mask & valid_bit) != 0;
}
