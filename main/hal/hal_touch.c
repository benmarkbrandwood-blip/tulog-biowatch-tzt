#include "hal_touch.h"
#include "app_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"

#include "lvgl.h"

/* CST820 I2C address (fixed, matches CST816S — same chip family). */
#define CST820_ADDR    0x15
#define CST820_I2C_HZ  400000
#define CST820_TIMEOUT pdMS_TO_TICKS(50)

static const char *TAG = "Touch";
static i2c_master_dev_handle_t s_cst820 = NULL;

/* ── LVGL read callback (polled each LVGL timer cycle) ──────────────────── */

static void cst820_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    /* Burst read 6 bytes starting at register 0x01:
     *   [0] gesture ID   [1] finger count
     *   [2] x high (4b)  [3] x low (8b)
     *   [4] y high (4b)  [5] y low (8b) */
    uint8_t reg = 0x01;
    uint8_t raw[6] = {0};
    esp_err_t err = i2c_master_transmit_receive(s_cst820, &reg, 1,
                                                raw, sizeof(raw), CST820_TIMEOUT);
    if (err != ESP_OK || raw[1] == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    data->point.x = (int16_t)(((raw[2] & 0x0F) << 8) | raw[3]);
    data->point.y = (int16_t)(((raw[4] & 0x0F) << 8) | raw[5]);
    data->state   = LV_INDEV_STATE_PRESSED;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void hal_touch_init(void)
{
    /* Reset sequence per CST820 datasheet and factory Arduino sketch:
     * drive INT low briefly before asserting RST, so the chip boots in I2C mode
     * rather than SPI mode. */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_TP_INT) | (1ULL << PIN_TP_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    gpio_set_level(PIN_TP_INT, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(PIN_TP_INT, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(300));

    /* I2C master bus (I2C0, SDA=GPIO33, SCL=GPIO32) */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port              = I2C_NUM_0,
        .sda_io_num            = PIN_TP_SDA,
        .scl_io_num            = PIN_TP_SCL,
        .clk_source            = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt     = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    /* CST820 device */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = CST820_ADDR,
        .scl_speed_hz    = CST820_I2C_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_cst820));

    /* Disable auto-sleep (register 0xFE = 0xFF) so the chip stays responsive. */
    uint8_t nosleep[] = {0xFE, 0xFF};
    esp_err_t err = i2c_master_transmit(s_cst820, nosleep, sizeof(nosleep), CST820_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Auto-sleep disable failed: %s (non-fatal)", esp_err_to_name(err));
    }

    /* Register LVGL pointer input device (polled by lv_timer_handler). */
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, cst820_read_cb);

    ESP_LOGI(TAG, "CST820 OK (I2C0, SDA=GPIO%d, SCL=GPIO%d)",
             PIN_TP_SDA, PIN_TP_SCL);
}
