#include "hal_backlight.h"
#include "app_config.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_err.h"

#define BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER    LEDC_TIMER_0
#define BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BL_LEDC_BITS     LEDC_TIMER_8_BIT
#define BL_LEDC_FREQ_HZ  5000

static const char *TAG = "Backlight";
static bool s_init = false;

void hal_backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = BL_LEDC_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_BITS,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t ch = {
        .gpio_num   = PIN_LCD_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    s_init = true;
    ESP_LOGI(TAG, "Init OK, GPIO%d", PIN_LCD_BL);
}

void hal_backlight_set_percent(uint8_t pct)
{
    if (!s_init) return;
    if (pct > 100) pct = 100;
    uint32_t duty = ((uint32_t)pct * 255u) / 100u;
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

void hal_backlight_on(void)  { hal_backlight_set_percent(100); }
void hal_backlight_off(void) { hal_backlight_set_percent(0); }
