#include "hal_storage.h"
#include "app_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
/* TODO Phase 3: bsp/esp-bsp.h removed — replace bsp_sdcard_mount()/bsp_sdcard
 * with sdspi_host + esp_vfs_fat_sdspi_mount() per migration plan §3.5. */

static const char *TAG = "Storage";

/*
 * Mount the on-board micro-SD card via the Waveshare BSP.
 *
 * The Waveshare ESP32-S3-Touch-AMOLED-2.06 wires the TF slot to the SDMMC
 * peripheral in 1-bit mode (CLK=GPIO2, CMD=GPIO1, D0=GPIO3) — see
 * components/waveshare__esp32_s3_touch_amoled_2_06/include/bsp/config.h and
 * esp32_s3_touch_amoled_2_06.c.
 *
 * The reference video player example (examples/ESP-IDF-v5.4.2/06_videoplayer)
 * mounts the card by calling bsp_sdcard_mount() with retries; we follow the
 * same pattern.  Without this call, fopen("/sdcard/...", "w") returns NULL
 * (which is exactly what was producing "SD card write failed").
 */

static volatile bool s_sd_mounted = false;

esp_err_t hal_storage_mount(void)
{
    if (s_sd_mounted) return ESP_OK;

    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= SD_MOUNT_RETRY_MAX; ++attempt) {
        err = bsp_sdcard_mount();
        if (err == ESP_OK) {
            s_sd_mounted = true;
            if (bsp_sdcard) {

            } else {

            }
            return ESP_OK;
        }
        if (err == ESP_ERR_INVALID_STATE) {
            /* Already mounted by something else — treat as success. */
            s_sd_mounted = true;
            ESP_LOGW(TAG, "SD card was already mounted");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "bsp_sdcard_mount attempt %d/%d failed: %s",
                 attempt, SD_MOUNT_RETRY_MAX, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGE(TAG, "SD card mount giving up: %s", esp_err_to_name(err));
    s_sd_mounted = false;
    return err;
}

bool hal_storage_is_mounted(void)
{
    return s_sd_mounted;
}
