#include "hal_battery.h"
#include "app_config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_log.h"
#include "esp_err.h"
/* TODO Phase 3: replace battery_adc_init() with adc_oneshot on PIN_BAT_ADC
 * (GPIO35/ADC1_CH7) per migration plan §3.4.  Until then, battery monitoring
 * is disabled — battery_adc_init() returns immediately, read functions return -1. */

static const char *TAG = "Battery";

/* Phase 3 replacement target: AXP2101 register map preserved as reference.
 * Do not compile until Phase 3 — i2c_master API is referenced here but the
 * TZT board has no AXP2101, so this block will be deleted and replaced with
 * adc_oneshot on PIN_BAT_ADC (GPIO35). */
#if 0
#include "driver/i2c_master.h"
#define AXP2101_I2C_ADDR        0x34
#define AXP2101_I2C_HZ          100000
#define AXP2101_I2C_TIMEOUT_MS  100
#define AXP_REG_PMU_STATUS1     0x00
#define AXP_REG_PMU_STATUS2     0x01
#define AXP_REG_VBAT_H          0x34
#define AXP_REG_VBAT_L          0x35
#define AXP_REG_BATT_PERCENT    0xA4

static i2c_master_dev_handle_t s_axp_dev = NULL;
static bool                    s_axp_present = false;

static esp_err_t axp_read(uint8_t reg, uint8_t *out, size_t len)
{
    if (!s_axp_dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_axp_dev, &reg, 1, out, len,
                                        AXP2101_I2C_TIMEOUT_MS);
}
#endif /* Phase 3 */

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void battery_adc_init(void)
{
    /* Phase 3 stub — AXP2101 is absent on TZT board; replace with
     * adc_oneshot on PIN_BAT_ADC (GPIO35/ADC1_CH7) per migration plan §3.4. */
    ESP_LOGW(TAG, "Battery monitoring disabled (Phase 3 not implemented)");
}

/* Phase 3 stubs — return sentinel values until adc_oneshot is wired up. */

float battery_read_voltage(void)  { return -1.0f; }
int   battery_read_percent(void)  { return -1; }
