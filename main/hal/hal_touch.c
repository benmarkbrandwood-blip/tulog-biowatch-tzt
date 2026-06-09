#include "hal_touch.h"
#include "app_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_log.h"

#include "lvgl.h"

/* XPT2046 channel-select command bytes (12-bit differential mode, PD=11). */
#define XPT2046_CMD_X   0xD3    /* measure X position */
#define XPT2046_CMD_Y   0x93    /* measure Y position */
#define XPT2046_CMD_Z1  0xB3    /* Z1 pressure channel */
#define XPT2046_CMD_Z2  0xC3    /* Z2 pressure channel */

/* Pressure threshold — pen down when Z > this. */
#define XPT_Z_THRESHOLD  350

/* Calibration: raw ADC range → screen pixel.
 * Defaults from corner-touch session (2026-06-06).
 * hal_touch_set_calibration() overwrites these at runtime from NVS. */
static uint16_t s_x_min = 650;    /* raw_x at RIGHT screen edge */
static uint16_t s_x_max = 3200;   /* raw_x at LEFT  screen edge */
static uint16_t s_y_min = 650;    /* raw_y at BOTTOM screen edge */
static uint16_t s_y_max = 3100;   /* raw_y at TOP   screen edge */

static const char *TAG = "Touch";
static spi_device_handle_t s_xpt = NULL;

/* ── Read one 12-bit ADC channel (single sample) ────────────────────────── */

static uint16_t xpt2046_read_once(uint8_t cmd)
{
    uint8_t tx[3] = {cmd, 0x00, 0x00};
    uint8_t rx[3] = {0x00, 0x00, 0x00};
    spi_transaction_t t = {
        .length    = 24,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_polling_transmit(s_xpt, &t);
    return ((uint16_t)(rx[1] << 8 | rx[2]) >> 3) & 0x0FFF;
}

/* ── Average N samples (discard first, average remainder) ───────────────── */

#define XPT_SAMPLES 5

static uint16_t xpt2046_read(uint8_t cmd)
{
    xpt2046_read_once(cmd);          /* discard — settling time */
    uint32_t sum = 0;
    for (int i = 0; i < XPT_SAMPLES; i++) sum += xpt2046_read_once(cmd);
    return (uint16_t)(sum / XPT_SAMPLES);
}

/* ── Z pressure reading ──────────────────────────────────────────────────── */

static uint16_t xpt2046_read_z(void)
{
    uint16_t z1 = xpt2046_read_once(XPT2046_CMD_Z1);
    uint16_t z2 = xpt2046_read_once(XPT2046_CMD_Z2);
    int32_t z = (int32_t)4095 + (int32_t)z1 - (int32_t)z2;
    return (z > 0) ? (uint16_t)z : 0;
}

/* ── Map raw ADC value to pixel coordinate ───────────────────────────────── */

static inline int16_t xpt_map(uint16_t raw, int raw_min, int raw_max, int px_max)
{
    int v = ((int)raw - raw_min) * px_max / (raw_max - raw_min);
    if (v < 0)       v = 0;
    if (v > px_max)  v = px_max;
    return (int16_t)v;
}

/* ── LVGL read callback ──────────────────────────────────────────────────── */

static void xpt2046_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    uint16_t z = xpt2046_read_z();
    if (z <= XPT_Z_THRESHOLD) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    uint16_t raw_x = xpt2046_read(XPT2046_CMD_X);
    uint16_t raw_y = xpt2046_read(XPT2046_CMD_Y);

    /* raw_x: LEFT=high, RIGHT=low → invert.
     * raw_y: TOP=high, BOTTOM=low → invert.
     * Calibration values set by hal_touch_set_calibration() (loaded from NVS). */
    int16_t lx = (int16_t)(LCD_H_RES - 1)
                 - xpt_map(raw_x, s_x_min, s_x_max, LCD_H_RES - 1);
    int16_t ly = (int16_t)(LCD_V_RES - 1)
                 - xpt_map(raw_y, s_y_min, s_y_max, LCD_V_RES - 1);

    data->point.x = lx;
    data->point.y = ly;
    data->state   = LV_INDEV_STATE_PRESSED;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void hal_touch_set_calibration(uint16_t x_min, uint16_t x_max,
                                uint16_t y_min, uint16_t y_max)
{
    s_x_min = x_min;
    s_x_max = x_max;
    s_y_min = y_min;
    s_y_max = y_max;
    ESP_LOGI(TAG, "Cal: x %u-%u  y %u-%u", x_min, x_max, y_min, y_max);
}

bool hal_touch_read_raw(uint16_t *x, uint16_t *y)
{
    if (xpt2046_read_z() <= XPT_Z_THRESHOLD) return false;
    *x = xpt2046_read(XPT2046_CMD_X);
    *y = xpt2046_read(XPT2046_CMD_Y);
    return true;
}

void hal_touch_init(void)
{
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 2 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = PIN_TP_CS,
        .queue_size     = 1,
        .pre_cb         = NULL,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &s_xpt));

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, xpt2046_read_cb);

    ESP_LOGI(TAG, "XPT2046 OK — SPI2, CS=GPIO%d, Z-pressure mode (no IRQ)",
             PIN_TP_CS);
}
