#include "svc_bp_record.h"
#include "app_config.h"
#include "hal_storage.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>   /* fsync() */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
/* SD_MOUNT_POINT removed — SD_MOUNT_POINT from app_config.h used instead */

static const char *TAG = "BPRec";

static QueueHandle_t     s_bp_queue        = NULL;
static StaticQueue_t     s_bp_queue_struct;
static uint8_t          *s_bp_queue_storage = NULL;

static TaskHandle_t      s_bp_writer_task  = NULL;
static volatile bool     s_bp_recording    = false;
static FILE             *s_bp_file         = NULL;
static uint32_t          s_bp_start_ms     = 0;
static uint32_t          s_bp_duration_s   = BP_DURATION_60S;
static char              s_bp_filename[128] = {0};

/* -------------------------------------------------------------------------- */
/* Private helpers                                                            */
/* -------------------------------------------------------------------------- */

static esp_err_t bp_open_file(void)
{
    if (!hal_storage_is_mounted()) {
        if (hal_storage_mount() != ESP_OK) {
            ESP_LOGE(TAG, "bp_open_file: SD not mounted");
            return ESP_FAIL;
        }
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_now;
    localtime_r(&tv.tv_sec, &tm_now);
    int ms = (int)(tv.tv_usec / 1000);

    snprintf(s_bp_filename, sizeof(s_bp_filename),
             SD_MOUNT_POINT "/%04d%02d%02d_%02d%02d%02d_%03d_bp.csv",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, ms);

    s_bp_file = fopen(s_bp_filename, "w");
    if (!s_bp_file) {
        ESP_LOGE(TAG, "Failed to open %s (errno=%d %s)",
                 s_bp_filename, errno, strerror(errno));
        return ESP_FAIL;
    }

    static char bp_iobuf[BP_IOBUF_SIZE];
    setvbuf(s_bp_file, bp_iobuf, _IOFBF, sizeof(bp_iobuf));

    fprintf(s_bp_file,
            "time_ms,ecg,ppg,drift_ms,r_peak_ms,rr_us,pat_us\r\n");
    fflush(s_bp_file);
    fsync(fileno(s_bp_file));   /* commit header to SD so file isn't 0 bytes */

    return ESP_OK;
}

static void bp_writer_task(void *arg)
{
    (void)arg;
    bp_row_t row;
    char line[BP_ROW_BUF];
    uint32_t rows_since_flush = 0;

    while (1) {
        if (xQueueReceive(s_bp_queue, &row, pdMS_TO_TICKS(200)) == pdTRUE) {
            /* Auto-stop on duration */
            if (s_bp_recording) {
                uint32_t elapsed_ms =
                    (uint32_t)(esp_timer_get_time() / 1000ULL) - s_bp_start_ms;
                if (elapsed_ms >= s_bp_duration_s * 1000UL) {
                    s_bp_recording = false;
                }
            }

            if (s_bp_file) {
                int n = snprintf(line, sizeof(line),
                                 "%lu,%ld,%ld,%ld,%lu,%ld,%ld\r\n",
                                 (unsigned long)row.time_ms,
                                 (long)row.ecg, (long)row.ppg,
                                 (long)row.drift_ms,
                                 (unsigned long)row.r_peak_ms,
                                 (long)row.rr_us, (long)row.pat_us);
                if (n > 0) {
                    if (fputs(line, s_bp_file) == EOF) {
                        ESP_LOGE(TAG, "fputs failed (errno=%d %s)",
                                 errno, strerror(errno));
                    }
                }
                if (++rows_since_flush >= BP_FLUSH_ROWS) {
                    fflush(s_bp_file);
                    /* fsync() commits the FATFS directory entry to SD card so
                     * the file size on disk is non-zero even if the system
                     * reboots before fclose() runs. */
                    fsync(fileno(s_bp_file));
                    rows_since_flush = 0;
                }
                esp_rom_delay_us(BP_WRITE_DELAY_US);
            }
        } else {
            if (!s_bp_recording &&
                uxQueueMessagesWaiting(s_bp_queue) == 0)
                break;
        }
    }

    /* Drain any remaining rows */
    while (xQueueReceive(s_bp_queue, &row, 0) == pdTRUE) {
        if (s_bp_file) {
            snprintf(line, sizeof(line),
                     "%lu,%ld,%ld,%ld,%lu,%ld,%ld\r\n",
                     (unsigned long)row.time_ms,
                     (long)row.ecg, (long)row.ppg,
                     (long)row.drift_ms,
                     (unsigned long)row.r_peak_ms,
                     (long)row.rr_us, (long)row.pat_us);
            fputs(line, s_bp_file);
            esp_rom_delay_us(BP_WRITE_DELAY_US);
        }
    }

    if (s_bp_file) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm tm_end;
        localtime_r(&tv.tv_sec, &tm_end);
        int end_ms = (int)(tv.tv_usec / 1000);
        fprintf(s_bp_file,
                "# finished,%04d-%02d-%02d %02d:%02d:%02d.%03d\r\n",
                tm_end.tm_year + 1900, tm_end.tm_mon + 1, tm_end.tm_mday,
                tm_end.tm_hour, tm_end.tm_min, tm_end.tm_sec, end_ms);
        fclose(s_bp_file);
        s_bp_file = NULL;
        ESP_LOGI(TAG, "Closed: %s", s_bp_filename);
    }

    s_bp_writer_task = NULL;
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t svc_bp_rec_init(void)
{
    if (s_bp_queue) return ESP_OK;   /* already initialised */

    s_bp_queue_storage = heap_caps_malloc(
        BP_QUEUE_LEN * sizeof(bp_row_t), MALLOC_CAP_SPIRAM);
    if (!s_bp_queue_storage) {
        ESP_LOGE(TAG, "PSRAM alloc failed (%u bytes)",
                 (unsigned)(BP_QUEUE_LEN * sizeof(bp_row_t)));
        return ESP_ERR_NO_MEM;
    }

    s_bp_queue = xQueueCreateStatic(BP_QUEUE_LEN, sizeof(bp_row_t),
                                     s_bp_queue_storage, &s_bp_queue_struct);
    if (!s_bp_queue) {
        ESP_LOGE(TAG, "xQueueCreateStatic failed");
        heap_caps_free(s_bp_queue_storage);
        s_bp_queue_storage = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Init OK (queue %u bytes PSRAM)",
             (unsigned)(BP_QUEUE_LEN * sizeof(bp_row_t)));
    return ESP_OK;
}

esp_err_t svc_bp_rec_start(uint32_t duration_s)
{
    if (s_bp_recording) return ESP_ERR_INVALID_STATE;
    if (!s_bp_queue)    return ESP_ERR_INVALID_STATE;

    if (bp_open_file() != ESP_OK) return ESP_FAIL;

    xQueueReset(s_bp_queue);
    s_bp_duration_s = duration_s;
    s_bp_start_ms   = (uint32_t)(esp_timer_get_time() / 1000ULL);
    s_bp_recording  = true;

    BaseType_t ok = xTaskCreatePinnedToCore(
        bp_writer_task, "bp_writer",
        4096, NULL, BP_SAMPLER_PRIORITY,
        &s_bp_writer_task, BP_CORE_WRITER);

    if (ok != pdPASS) {
        s_bp_recording = false;
        fclose(s_bp_file);
        s_bp_file = NULL;
        ESP_LOGE(TAG, "Failed to create writer task");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Started: %s  duration=%lus", s_bp_filename,
             (unsigned long)duration_s);
    return ESP_OK;
}

void svc_bp_rec_stop(void)
{
    s_bp_recording = false;
}

void svc_bp_rec_enqueue(const bp_row_t *row)
{
    if (!s_bp_recording || !s_bp_queue) return;
    if (xQueueSend(s_bp_queue, row, 0) != pdTRUE) {
        /* Queue full: drop sample rather than block the sampler task */
    }
}

bool svc_bp_rec_is_recording(void)
{
    return s_bp_recording;
}

uint32_t svc_bp_rec_get_start_ms(void)
{
    return s_bp_start_ms;
}

uint32_t svc_bp_rec_get_duration_s(void)
{
    return s_bp_duration_s;
}

const char *svc_bp_rec_get_filename(void)
{
    return s_bp_filename;
}

/* -------------------------------------------------------------------------- */
/* Post-recording analysis                                                    */
/* -------------------------------------------------------------------------- */

esp_err_t bp_analyse_file(const char *path, bp_analysis_t *out)
{
    if (!path || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "analyse: cannot open %s", path);
        return ESP_FAIL;
    }

    static int32_t rr[512];
    static int32_t pat[512];
    uint32_t n = 0;
    char line[128];

    /* Skip header */
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return ESP_FAIL;
    }

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;

        unsigned long time_ms;
        long ecg, ppg, drift;
        unsigned long r_peak;
        long rr_us, pat_us;

        int parsed = sscanf(line, "%lu,%ld,%ld,%ld,%lu,%ld,%ld",
                            &time_ms, &ecg, &ppg, &drift,
                            &r_peak, &rr_us, &pat_us);
        if (parsed != 7) continue;
        if (rr_us == 0) continue;

        if (n < 512) {
            rr[n]  = (int32_t)rr_us;
            pat[n] = (int32_t)pat_us;
            n++;
        }
    }
    fclose(f);

    out->beat_count = n;
    if (n < 4) {
        ESP_LOGW(TAG, "analyse: only %lu beats — result invalid", (unsigned long)n);
        out->valid = false;
        return ESP_OK;
    }

    /* RMSSD */
    float sum_sq = 0.0f;
    for (uint32_t i = 0; i + 1 < n; i++) {
        float diff = (float)(rr[i + 1] - rr[i]);
        sum_sq += diff * diff;
    }
    out->hrv_rmssd_us = sqrtf(sum_sq / (float)(n - 1));

    /* PAT mean and variance */
    float pat_sum = 0.0f;
    for (uint32_t i = 0; i < n; i++) pat_sum += (float)pat[i];
    out->pat_mean_us = pat_sum / (float)n;

    float var_sum = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float d = (float)pat[i] - out->pat_mean_us;
        var_sum += d * d;
    }
    out->pat_variance_us2 = var_sum / (float)n;

    /* Copy last 64 beats into series arrays for chart */
    uint32_t start = (n > 64) ? (n - 64) : 0;
    uint32_t count = n - start;
    for (uint32_t i = 0; i < count; i++) {
        out->rr_series[i]  = rr[start + i];
        out->pat_series[i] = pat[start + i];
    }

    out->valid = true;
    ESP_LOGI(TAG, "analyse: %lu beats  RMSSD=%.1f us  PAT mean=%.1f us  PAT var=%.1f us^2",
             (unsigned long)n, (double)out->hrv_rmssd_us,
             (double)out->pat_mean_us, (double)out->pat_variance_us2);
    return ESP_OK;
}
