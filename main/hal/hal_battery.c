#include "hal_battery.h"
#include "esp_log.h"

static const char *TAG = "Battery";

/* The TZT ESP32-2432S024C uses an IP5306 boost+charger IC that has no I2C
 * interface wired to the ESP32 in this board variant.  GPIO35 routes to the
 * external expansion connector P3, not to a battery sense node.  There is no
 * battery voltage path to any ESP32 ADC input, so monitoring is unavailable. */

void battery_adc_init(void)
{
    ESP_LOGW(TAG, "No battery sense path on this board (IP5306, no I2C/ADC)");
}

float battery_read_voltage(void)  { return -1.0f; }
int   battery_read_percent(void)  { return -1; }
