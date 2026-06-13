#include "hal_max30102.h"
#include "hal_bus.h"
#include "app_config.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "hal_max30102";

/* ── Register map ────────────────────────────────────────────────────────── */
#define REG_INTR_STATUS1    0x00
#define REG_INTR_ENABLE1    0x02
#define REG_FIFO_WR_PTR     0x04
#define REG_OVF_COUNTER     0x05
#define REG_FIFO_RD_PTR     0x06
#define REG_FIFO_DATA       0x07
#define REG_FIFO_CONFIG     0x08
#define REG_MODE_CONFIG     0x09
#define REG_SPO2_CONFIG     0x0A
#define REG_LED1_PA         0x0C   /* Red LED pulse amplitude */
#define REG_LED2_PA         0x0D   /* IR LED pulse amplitude */
#define REG_PART_ID         0xFF

#define PART_ID_EXPECTED    0x15
#define ADDR                I2C_ADDR_MAX30102

/* ── SpO2 accumulator ────────────────────────────────────────────────────── */
/* Welford online variance over SPO2_WINDOW samples then reset. */
#define SPO2_WINDOW         400    /* ~4 s at 100 SPS */
#define SPO2_MIN_DC         5000   /* reject signal if DC < this (finger not present) */

static uint32_t s_n;
static float    s_red_mean, s_ir_mean;
static float    s_red_M2, s_ir_M2;
static uint8_t  s_spo2_pct;

static void spo2_accumulate(int32_t red, int32_t ir)
{
    s_n++;

    float dr = (float)red - s_red_mean;
    s_red_mean += dr / (float)s_n;
    s_red_M2   += dr * ((float)red - s_red_mean);

    float di = (float)ir - s_ir_mean;
    s_ir_mean  += di / (float)s_n;
    s_ir_M2    += di * ((float)ir - s_ir_mean);

    if (s_n < SPO2_WINDOW) return;

    /* Compute SpO2 from R = (AC_red/DC_red) / (AC_ir/DC_ir) */
    if (s_ir_mean > SPO2_MIN_DC && s_red_mean > SPO2_MIN_DC) {
        float red_ac = sqrtf(s_red_M2 / (float)s_n);
        float ir_ac  = sqrtf(s_ir_M2  / (float)s_n);
        float R      = (red_ac / s_red_mean) / (ir_ac / s_ir_mean);
        float spo2   = -45.060f * R * R + 30.354f * R + 94.845f;
        if (spo2 < 50.0f) {
            s_spo2_pct = 0;  /* out of plausible range — treat as invalid */
        } else if (spo2 > 100.0f) {
            s_spo2_pct = 100;
        } else {
            s_spo2_pct = (uint8_t)spo2;
        }
        ESP_LOGI(TAG, "SpO2 %u%% (R=%.3f DC_ir=%.0f DC_red=%.0f)",
                 s_spo2_pct, (double)R, (double)s_ir_mean, (double)s_red_mean);
    } else {
        s_spo2_pct = 0;  /* finger not on sensor */
    }

    /* Reset for next window */
    s_n = 0;
    s_red_mean = s_ir_mean = 0.0f;
    s_red_M2   = s_ir_M2   = 0.0f;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t hal_max30102_init(void)
{
    /* 1. Verify Part ID */
    uint8_t part_id = 0;
    esp_err_t err = hal_i2c_reg_read(ADDR, REG_PART_ID, &part_id, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Part ID read failed: %s", esp_err_to_name(err));
        return err;
    }
    if (part_id != PART_ID_EXPECTED) {
        ESP_LOGE(TAG, "Part ID 0x%02X, expected 0x%02X", part_id, PART_ID_EXPECTED);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Part ID 0x%02X OK", part_id);

    /* 2. Reset */
    uint8_t v = 0x40;  /* RESET bit */
    err = hal_i2c_reg_write(ADDR, REG_MODE_CONFIG, &v, 1);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 3. FIFO config: no sample averaging, FIFO rollover enabled */
    v = 0x10;
    err = hal_i2c_reg_write(ADDR, REG_FIFO_CONFIG, &v, 1);
    if (err != ESP_OK) return err;

    /* 4. SpO2 mode */
    v = 0x03;
    err = hal_i2c_reg_write(ADDR, REG_MODE_CONFIG, &v, 1);
    if (err != ESP_OK) return err;

    /* 5. SpO2 config: ADC range 4096 nA, 100 SPS, 411 µs pulse (18-bit ADC)
     *    [6:5]=01 (4096 nA), [4:2]=001 (100 SPS), [1:0]=11 (411 µs) = 0x27 */
    v = 0x27;
    err = hal_i2c_reg_write(ADDR, REG_SPO2_CONFIG, &v, 1);
    if (err != ESP_OK) return err;

    /* 6. LED currents: Red (LED1) and IR (LED2) at 7.2 mA (0x24) */
    v = 0x24;
    err = hal_i2c_reg_write(ADDR, REG_LED1_PA, &v, 1);
    if (err != ESP_OK) return err;
    err = hal_i2c_reg_write(ADDR, REG_LED2_PA, &v, 1);
    if (err != ESP_OK) return err;

    /* 7. Clear FIFO pointers */
    v = 0x00;
    hal_i2c_reg_write(ADDR, REG_FIFO_WR_PTR, &v, 1);
    hal_i2c_reg_write(ADDR, REG_OVF_COUNTER,  &v, 1);
    hal_i2c_reg_write(ADDR, REG_FIFO_RD_PTR, &v, 1);

    /* Reset SpO2 accumulator */
    s_n = 0;
    s_red_mean = s_ir_mean = 0.0f;
    s_red_M2   = s_ir_M2   = 0.0f;
    s_spo2_pct = 0;

    ESP_LOGI(TAG, "Init OK — SpO2 mode, 100 SPS, 18-bit, LED 7.2 mA");
    return ESP_OK;
}

esp_err_t hal_max30102_read_fifo(int32_t *ir_out, int32_t *red_out)
{
    /* Read FIFO pointers to find available sample count. */
    uint8_t ptrs[3];
    esp_err_t err = hal_i2c_reg_read(ADDR, REG_FIFO_WR_PTR, ptrs, 3);
    if (err != ESP_OK) return err;

    uint8_t wr_ptr  = ptrs[0] & 0x1F;
    /* ptrs[1] = overflow counter — not used here */
    uint8_t rd_ptr  = ptrs[2] & 0x1F;
    uint8_t n_avail = (wr_ptr - rd_ptr) & 0x1F;

    if (n_avail == 0) return ESP_ERR_NOT_FOUND;

    /* Cap drain at 4 samples to bound I2C transaction length (24 bytes). */
    if (n_avail > 4) n_avail = 4;

    /* Read n_avail × 6 bytes from the FIFO data register.
     * SpO2 mode order: LED1 (Red) 3 bytes then LED2 (IR) 3 bytes per sample.
     * The I2C read auto-advances through FIFO entries after register 0x07. */
    uint8_t buf[24];  /* 4 samples × 6 bytes */
    err = hal_i2c_reg_read(ADDR, REG_FIFO_DATA, buf, n_avail * 6);
    if (err != ESP_OK) return err;

    /* Use the LAST sample (freshest), feed all into the SpO2 accumulator. */
    for (uint8_t i = 0; i < n_avail; i++) {
        const uint8_t *s = buf + i * 6;
        int32_t red = ((int32_t)(s[0] & 0x03) << 16) | ((int32_t)s[1] << 8) | s[2];
        int32_t ir  = ((int32_t)(s[3] & 0x03) << 16) | ((int32_t)s[4] << 8) | s[5];
        spo2_accumulate(red, ir);
        if (i == n_avail - 1) {
            *red_out = red;
            *ir_out  = ir;
        }
    }

    return ESP_OK;
}

uint8_t hal_max30102_get_spo2(void)
{
    return s_spo2_pct;
}
