#include "hal_touch.h"
#include "app_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_log.h"

#include "lvgl.h"

/* XPT2046 channel-select command bytes (12-bit differential mode, PD=11). */
#define XPT2046_CMD_X   0xD3    /* measure X position (short panel axis) */
#define XPT2046_CMD_Y   0x93    /* measure Y position (long panel axis)  */
#define XPT2046_CMD_Z1  0xB3    /* Z1 pressure channel */
#define XPT2046_CMD_Z2  0xC3    /* Z2 pressure channel */

/* Touch pressure threshold.  Formula: Z = 4095 + Z1 - Z2.
 * Below this value the pen is considered up.  Matches TFT_eSPI default. */
#define XPT_Z_THRESHOLD  350

/* ADC calibration — raw range → screen pixel.
 * Derived from hardware corner-touch measurements (2026-06-06):
 *   top-left  raw_x≈2923 raw_y≈2793
 *   top-right raw_x≈857  raw_y≈2800
 *   bot-left  raw_x≈2858 raw_y≈930
 *   bot-right raw_x≈862  raw_y≈920
 * raw_x: LEFT=high, RIGHT=low → X channel, inverted.
 * raw_y: TOP=high, BOTTOM=low → Y channel, inverted.
 * Margins added beyond the corner-touch extremes to avoid edge clipping. */
#define XPT_X_MIN   650     /* raw_x at RIGHT screen edge */
#define XPT_X_MAX   3200    /* raw_x at LEFT  screen edge */
#define XPT_Y_MIN   650     /* raw_y at BOTTOM screen edge */
#define XPT_Y_MAX   3100    /* raw_y at TOP   screen edge */

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
    /* rx[0]: received during command byte — discard.
     * rx[1..2]: [null bit][B11..B5][B4..B0][3 don't-care bits]
     * Extract 12-bit result: (rx[1]<<5) | (rx[2]>>3) */
    return ((uint16_t)(rx[1] << 8 | rx[2]) >> 3) & 0x0FFF;
}

/* ── Average N samples (discard first, average remainder) ───────────────── */

#define XPT_SAMPLES 5

static uint16_t xpt2046_read(uint8_t cmd)
{
    xpt2046_read_once(cmd);          /* discard first sample — settling time */
    uint32_t sum = 0;
    for (int i = 0; i < XPT_SAMPLES; i++) sum += xpt2046_read_once(cmd);
    return (uint16_t)(sum / XPT_SAMPLES);
}

/* ── Z pressure reading ──────────────────────────────────────────────────── */

static uint16_t xpt2046_read_z(void)
{
    uint16_t z1 = xpt2046_read_once(XPT2046_CMD_Z1);
    uint16_t z2 = xpt2046_read_once(XPT2046_CMD_Z2);
    /* Z = 4095 + Z1 - Z2.  Positive and > threshold when pen is down.
     * The special case z==4095 (Z1=0, Z2=0, pen up) is caught by the
     * threshold check — no separate handling needed. */
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

    /* The LovyanGFX reference demo for this board sets pin_int=-1 — the
     * XPT2046 PENIRQ line is not routed to the ESP32.  GPIO36 floats and
     * cannot be used as an IRQ gate.  Use Z-pressure as the sole gate. */
    uint16_t z = xpt2046_read_z();
    if (z <= XPT_Z_THRESHOLD) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    uint16_t raw_x = xpt2046_read(XPT2046_CMD_X);
    uint16_t raw_y = xpt2046_read(XPT2046_CMD_Y);

    /* Log raw values for calibration — watch serial output while touching
     * screen corners and update XPT_X/Y_MIN/MAX above. */
    ESP_LOGI(TAG, "z=%d raw x=%d y=%d", z, raw_x, raw_y);

    /* Coordinate mapping for MADCTL=0x40 landscape 320×240.
     * Verified by corner-touch calibration (2026-06-06):
     *   raw_x (0xD3) = horizontal axis: LEFT=high, RIGHT=low → invert for LVGL X.
     *   raw_y (0x93) = vertical axis:   TOP=high, BOTTOM=low → invert for LVGL Y.
     * No X/Y swap needed for this display/touch overlay orientation. */
    int16_t lx = (int16_t)(LCD_H_RES - 1)
                 - xpt_map(raw_x, XPT_X_MIN, XPT_X_MAX, LCD_H_RES - 1);
    int16_t ly = (int16_t)(LCD_V_RES - 1)
                 - xpt_map(raw_y, XPT_Y_MIN, XPT_Y_MAX, LCD_V_RES - 1);

    data->point.x = lx;
    data->point.y = ly;
    data->state   = LV_INDEV_STATE_PRESSED;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void hal_touch_init(void)
{
    /* XPT2046 shares SPI2 with the display (CS=GPIO33).
     * PENIRQ (GPIO36) is NOT connected on this board — touch detection uses
     * Z-pressure measurement only, not IRQ.
     * Must be called after hal_display_init() (SPI2 bus must already exist). */
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 2 * 1000 * 1000,   /* 2 MHz — safe for XPT2046 */
        .mode           = 0,                  /* CPOL=0, CPHA=0 */
        .spics_io_num   = PIN_TP_CS,          /* GPIO33 */
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
