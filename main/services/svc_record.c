#include "svc_record.h"
#include "app_config.h"
#include "hal_storage.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
/* SD_MOUNT_POINT removed — SD_MOUNT_POINT from app_config.h used instead */

static const char *TAG = "Rec";

static QueueHandle_t     s_rec_queue       = NULL;
static TaskHandle_t      s_rec_writer_task = NULL;
static volatile bool     s_recording       = false;
static FILE             *s_rec_file        = NULL;
static uint32_t          s_rec_start_ms    = 0;
static char              s_rec_label[REC_LABEL_MAX]     = {0};
static char              s_rec_filename[REC_FILENAME_MAX] = {0};
static SemaphoreHandle_t s_rec_mutex       = NULL;

/* -------------------------------------------------------------------------- */
/* Private helpers                                                            */
/* -------------------------------------------------------------------------- */

static esp_err_t rec_open_file(void)
{
    /* Lazy-mount: the user may have inserted the card after boot. */
    if (!hal_storage_is_mounted()) {
        if (hal_storage_mount() != ESP_OK) {
            ESP_LOGE(TAG, "rec_open_file: SD not mounted");
            return ESP_FAIL;
        }
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_now;
    localtime_r(&tv.tv_sec, &tm_now);
    int ms = (int)(tv.tv_usec / 1000);

    if (s_rec_label[0])
        snprintf(s_rec_filename, sizeof(s_rec_filename),
                 SD_MOUNT_POINT "/%04d%02d%02d_%02d%02d%02d_%03d_%s.csv",
                 tm_now.tm_year+1900, tm_now.tm_mon+1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, ms,
                 s_rec_label);
    else
        snprintf(s_rec_filename, sizeof(s_rec_filename),
                 SD_MOUNT_POINT "/%04d%02d%02d_%02d%02d%02d_%03d.csv",
                 tm_now.tm_year+1900, tm_now.tm_mon+1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, ms);

    s_rec_file = fopen(s_rec_filename, "w");
    if (!s_rec_file) {
        ESP_LOGE(TAG, "Failed to open %s (errno=%d %s)",
                 s_rec_filename, errno, strerror(errno));
        return ESP_FAIL;
    }
    /* Larger stdio buffer reduces FATFS write churn at 100 Hz. */
    static char rec_iobuf[4096];
    setvbuf(s_rec_file, rec_iobuf, _IOFBF, sizeof(rec_iobuf));

    fprintf(s_rec_file,
        "time_ms,ecg,ecg2,ecg3,ppg,resp,nas,fcg1,accel_x,accel_y,accel_z,drift_ms,batt_pct,spo2,resp_rate,hr_bpm,rr_ms,pat_ms,r_peak_ms\r\n");
    fflush(s_rec_file);

    return ESP_OK;
}

/*
 * SD writer task — core 1.
 * Reads rec_row_t items from s_rec_queue and writes CSV rows.
 * 0.5 ms delay between rows throttles SD bus.
 * Self-deletes when s_recording==false and queue is drained.
 */
static void sd_writer_task(void *arg)
{
    (void)arg;
    rec_row_t row;
    char line[REC_ROW_BUF];
    uint32_t rows_since_flush = 0;

    while (1) {
        if (xQueueReceive(s_rec_queue, &row, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (s_rec_file) {
                int n = snprintf(line, sizeof(line),
                    "%lu,%ld,%ld,%ld,%ld,%d,%d,%d,%d,%d,%d,%ld,%u,%u,%u,%u,%d,%d,%lu\r\n",
                    (unsigned long)row.time_ms,
                    (long)row.ecg, (long)row.ecg2, (long)row.ecg3,
                    (long)row.ppg, row.resp,
                    row.nas, row.fcg1,
                    row.accel_x, row.accel_y, row.accel_z,
                    (long)row.drift_ms, row.batt_pct,
                    row.spo2, row.resp_rate, row.hr_bpm,
                    row.rr_ms, row.pat_ms,
                    (unsigned long)row.r_peak_ms);
                if (n > 0) {
                    if (fputs(line, s_rec_file) == EOF) {
                        ESP_LOGE(TAG, "fputs failed (errno=%d %s)",
                                 errno, strerror(errno));
                    }
                }
                if (++rows_since_flush >= 100) {
                    fflush(s_rec_file);
                    rows_since_flush = 0;
                }
                esp_rom_delay_us(REC_WRITE_DELAY_US);
            }
        } else {
            if (!s_recording &&
                uxQueueMessagesWaiting(s_rec_queue) == 0)
                break;
        }
    }

    /* drain */
    while (xQueueReceive(s_rec_queue, &row, 0) == pdTRUE) {
        if (s_rec_file) {
            snprintf(line, sizeof(line),
                "%lu,%ld,%ld,%ld,%ld,%d,%d,%d,%d,%d,%d,%ld,%u,%u,%u,%u,%d,%d,%lu\r\n",
                (unsigned long)row.time_ms,
                (long)row.ecg, (long)row.ecg2, (long)row.ecg3,
                (long)row.ppg, row.resp,
                row.nas, row.fcg1,
                row.accel_x, row.accel_y, row.accel_z,
                (long)row.drift_ms, row.batt_pct,
                row.spo2, row.resp_rate, row.hr_bpm,
                row.rr_ms, row.pat_ms,
                (unsigned long)row.r_peak_ms);
            fputs(line, s_rec_file);
            esp_rom_delay_us(REC_WRITE_DELAY_US);
        }
    }

    if (s_rec_file) {
        struct timeval tv; gettimeofday(&tv, NULL);
        struct tm tm_end; localtime_r(&tv.tv_sec, &tm_end);
        fprintf(s_rec_file,
            "# finished,%04d-%02d-%02d %02d:%02d:%02d.%03d\r\n",
            tm_end.tm_year+1900, tm_end.tm_mon+1, tm_end.tm_mday,
            tm_end.tm_hour, tm_end.tm_min, tm_end.tm_sec,
            (int)(tv.tv_usec/1000));
        fflush(s_rec_file);
        fclose(s_rec_file);
        s_rec_file = NULL;

    }
    s_rec_writer_task = NULL;
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t svc_rec_init(void)
{
    s_rec_mutex = xSemaphoreCreateMutex();
    if (!s_rec_mutex) {
        ESP_LOGE(TAG, "Failed to create rec_mutex");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t svc_rec_start(const char *label)
{
    strncpy(s_rec_label, label ? label : "", sizeof(s_rec_label) - 1);
    s_rec_label[sizeof(s_rec_label) - 1] = '\0';

    if (!s_rec_queue) {
        s_rec_queue = xQueueCreate(REC_QUEUE_LEN, sizeof(rec_row_t));
        if (!s_rec_queue) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        xQueueReset(s_rec_queue);
    }

    s_rec_start_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    if (rec_open_file() != ESP_OK) {
        return ESP_FAIL;
    }

    s_recording = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        sd_writer_task, "sd_writer", 4096, NULL, 6,
        &s_rec_writer_task, REC_CORE_WRITER);
    if (ret != pdPASS) {
        s_recording = false;
        fclose(s_rec_file); s_rec_file = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

void svc_rec_stop(void)
{
    if (!s_recording) return;
    s_recording = false;
}

void svc_rec_enqueue(const rec_row_t *row)
{
    if (s_recording && s_rec_queue) {
        xQueueSend(s_rec_queue, row, 0);
    }
}

bool svc_rec_is_recording(void)
{
    return s_recording;
}

uint32_t svc_rec_get_start_ms(void)
{
    return s_rec_start_ms;
}
