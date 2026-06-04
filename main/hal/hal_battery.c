#include "hal_battery.h"
#include "app_config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
/* TODO Phase 3: bsp_i2c_get_handle() removed — replace battery_adc_init() with
 * adc_oneshot on PIN_BAT_ADC (GPIO35/ADC1_CH7) per migration plan §3.4. */

static const char *TAG = "Battery";

/* AXP2101 I2C address (TWSI). */
#define AXP2101_I2C_ADDR        0x34
#define AXP2101_I2C_HZ          100000
#define AXP2101_I2C_TIMEOUT_MS  100

/* Register addresses (datasheet §6.13.1). */
#define AXP_REG_PMU_STATUS1     0x00  /* battery present (bit 3) etc. */
#define AXP_REG_PMU_STATUS2     0x01  /* charge state, current direction */
#define AXP_REG_VBAT_H          0x34  /* VBAT ADC high 6 bits */
#define AXP_REG_VBAT_L          0x35  /* VBAT ADC low 8 bits */
#define AXP_REG_BATT_PERCENT    0xA4  /* fuel-gauge percentage 0..100 */

static i2c_master_dev_handle_t s_axp_dev = NULL;
static bool                    s_axp_present = false;

/* -------------------------------------------------------------------------- */
/* I2C helpers                                                                */
/* -------------------------------------------------------------------------- */

static esp_err_t axp_read(uint8_t reg, uint8_t *out, size_t len)
{
    if (!s_axp_dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_axp_dev, &reg, 1, out, len,
                                        AXP2101_I2C_TIMEOUT_MS);
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void battery_adc_init(void)
{
    if (s_axp_dev) return;

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGW(TAG, "I2C bus handle unavailable; battery monitoring disabled");
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_I2C_ADDR,
        .scl_speed_hz    = AXP2101_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_axp_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c_master_bus_add_device failed: %s",
                 esp_err_to_name(err));
        s_axp_dev = NULL;
        return;
    }

    /* Probe — read PMU status1. If the chip is present, this returns OK. */
    uint8_t status1 = 0;
    err = axp_read(AXP_REG_PMU_STATUS1, &status1, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 probe failed at 0x%02X: %s",
                 AXP2101_I2C_ADDR, esp_err_to_name(err));
        i2c_master_bus_rm_device(s_axp_dev);
        s_axp_dev = NULL;
        return;
    }

    s_axp_present = true;
    bool batt_present = (status1 & (1 << 3)) != 0;
    ESP_LOGI(TAG, "AXP2101 detected at 0x%02X (status1=0x%02X, battery %s)",
             AXP2101_I2C_ADDR, status1, batt_present ? "present" : "absent");
}

float battery_read_voltage(void)
{
    if (!s_axp_present) return -1.0f;

    /* Datasheet: read high 6 bits FIRST, then low 8 bits. */
    uint8_t hi = 0, lo = 0;
    if (axp_read(AXP_REG_VBAT_H, &hi, 1) != ESP_OK) return -1.0f;
    if (axp_read(AXP_REG_VBAT_L, &lo, 1) != ESP_OK) return -1.0f;

    /* 14-bit value, LSB = 1 mV. */
    uint16_t mv = ((uint16_t)(hi & 0x3F) << 8) | lo;
    return (float)mv / 1000.0f;
}

int battery_read_percent(void)
{
    if (!s_axp_present) return -1;

    uint8_t pct = 0;
    if (axp_read(AXP_REG_BATT_PERCENT, &pct, 1) != ESP_OK) return -1;

    /* Fuel gauge sometimes reports 0 before it has learned the cell. Fall
     * back to a linear voltage estimate in that case so the UI shows
     * something reasonable. */
    if (pct == 0) {
        float v = battery_read_voltage();
        if (v <= 0.0f) return -1;
        float frac = (v - BATTERY_VOLTAGE_EMPTY) /
                     (BATTERY_VOLTAGE_FULL - BATTERY_VOLTAGE_EMPTY);
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        return (int)(frac * 100.0f + 0.5f);
    }

    if (pct > 100) pct = 100;
    return (int)pct;
}
