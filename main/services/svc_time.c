#include "svc_time.h"
#include "app_config.h"

#include <stdbool.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "Time";

#define SNTP_VALID_EPOCH    1704067200      /* 2024-01-01 UTC */
#define SNTP_TIMEOUT_MS     15000
#define SNTP_POLL_MS        250

static bool s_time_synced = false;

/*
 * SNTP synchronisation.
 *
 * Issues with the previous implementation:
 *   1. esp_sntp_init() was being called every time the user reconnected to
 *      WiFi without first calling esp_sntp_stop().  After the first call,
 *      subsequent invocations either no-op'd or returned an error and the
 *      sync silently never completed.
 *   2. The wait-loop only spun while status == SNTP_SYNC_STATUS_RESET.  As
 *      soon as the daemon entered SNTP_SYNC_STATUS_IN_PROGRESS the loop
 *      exited and reported "timeout" even when a valid response was about to
 *      arrive.
 *   3. TZ was set *after* the wait — so any localtime_r() during the wait
 *      used UTC.  TZ should be set up-front.
 *   4. There was no sanity-check on the returned epoch; if the SNTP server
 *      is unreachable but a stale 1970 timestamp was already in the RTC, the
 *      wait loop would exit immediately.
 *
 * starts SNTP, then polls until the system clock crosses a sane epoch
 * (1 Jan 2024 ≡ 1704067200) or a hard timeout fires.
 */
void svc_time_sync(void)
{
    /* TZ must be set before any localtime_r() / strftime() call. */
    setenv("TZ", POSIX_TZ, 1);
    tzset();

    /* Stop any previous instance before re-initialising. */
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, SNTP_SERVER);
    esp_sntp_init();

    const TickType_t deadline = xTaskGetTickCount() +
                                pdMS_TO_TICKS(SNTP_TIMEOUT_MS);

    bool got_time = false;
    while (xTaskGetTickCount() < deadline) {
        time_t now = 0;
        time(&now);
        if (now > SNTP_VALID_EPOCH &&
            sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            got_time = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(SNTP_POLL_MS));
    }

    if (got_time) {
        time_t now = 0;
        time(&now);
        struct tm tm_local;
        localtime_r(&now, &tm_local);
        char buf[40];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm_local);

        s_time_synced = true;

        nvs_handle_t nvshdl;
        if (nvs_open(NVS_TIME_NAMESPACE, NVS_READWRITE, &nvshdl) == ESP_OK) {
            nvs_set_i64(nvshdl, NVS_TIME_KEY_LAST_SYNC, (int64_t)now);
            nvs_commit(nvshdl);
            nvs_close(nvshdl);
        }
    } else {
        ESP_LOGW(TAG, "SNTP sync timeout (status=%d)",
                 (int)sntp_get_sync_status());
        s_time_synced = false;
        /* Leave the daemon running so a delayed response can still update
         * the clock in the background. */
    }
}

bool svc_time_is_synced(void)
{
    return s_time_synced;
}

/*
 * Restores the last NTP-synced Unix timestamp from NVS and calls
 * settimeofday() so the clock shows a plausible stale time rather than
 * epoch 0 after a reboot without WiFi.  Does NOT set s_time_synced —
 * the restored time is stale, so the UI correctly shows "Time may be stale".
 * Safe to call before display init; NVS must already be initialised.
 */
void svc_time_restore_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_TIME_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return;
    }

    int64_t saved_ts = 0;
    err = nvs_get_i64(handle, NVS_TIME_KEY_LAST_SYNC, &saved_ts);
    nvs_close(handle);

    if (err != ESP_OK || saved_ts <= SNTP_VALID_EPOCH) {
        return;
    }

    setenv("TZ", POSIX_TZ, 1);
    tzset();

    struct timeval tv = { .tv_sec = (time_t)saved_ts, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    ESP_LOGI(TAG, "Restored last-known time from NVS (stale)");
}
