#include "hal_storage.h"
#include "app_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "sdmmc_cmd.h"

static const char    *TAG         = "Storage";
static volatile bool  s_sd_mounted = false;
static sdmmc_card_t  *s_card       = NULL;

esp_err_t hal_storage_mount(void)
{
    if (s_sd_mounted) return ESP_OK;

    /* SPI3 (VSPI) bus — separate from display (SPI2/HSPI). */
    spi_bus_config_t bus = {
        .mosi_io_num   = PIN_SD_MOSI,
        .miso_io_num   = PIN_SD_MISO,
        .sclk_io_num   = PIN_SD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI3 bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI3_HOST;
    slot.gpio_cs = PIN_SD_CS;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    for (int attempt = 1; attempt <= SD_MOUNT_RETRY_MAX; ++attempt) {
        err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot,
                                      &mount_cfg, &s_card);
        if (err == ESP_OK) {
            s_sd_mounted = true;
            ESP_LOGI(TAG, "SD card mounted at %s (%s)", SD_MOUNT_POINT,
                     s_card ? s_card->cid.name : "?");
            return ESP_OK;
        }
        if (err == ESP_ERR_INVALID_STATE) {
            s_sd_mounted = true;
            ESP_LOGW(TAG, "SD already mounted");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Mount attempt %d/%d failed: %s",
                 attempt, SD_MOUNT_RETRY_MAX, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ESP_LOGE(TAG, "SD card mount giving up: %s", esp_err_to_name(err));
    return err;
}

bool hal_storage_is_mounted(void)
{
    return s_sd_mounted;
}
