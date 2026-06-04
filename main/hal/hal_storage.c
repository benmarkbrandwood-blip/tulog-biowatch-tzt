#include "hal_storage.h"
#include "app_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
/* TODO Phase 3: replace bsp_sdcard_mount() with sdspi_host + esp_vfs_fat_sdspi_mount()
 * per migration plan §3.5 (SPI3/VSPI, GPIO18/19/23/5). */

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

    /* Phase 3 stub — sdspi_host mount not yet implemented.
     * SD card recording will be unavailable until Phase 3 is complete. */
    ESP_LOGW(TAG, "SD card mount not available (Phase 3 not implemented)");
    return ESP_ERR_NOT_SUPPORTED;
}

bool hal_storage_is_mounted(void)
{
    return s_sd_mounted;
}
