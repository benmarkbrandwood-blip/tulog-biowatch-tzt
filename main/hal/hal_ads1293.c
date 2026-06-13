#include "hal_ads1293.h"
#include "hal_ads1293_regs.h"
#include "hal_bus.h"
#include "hal_display.h"
#include "app_config.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hal_ads1293";

static spi_device_handle_t s_spi = NULL;

/* ── Low-level SPI helpers ───────────────────────────────────────────────── */

esp_err_t hal_ads1293_reg_write(uint8_t addr, uint8_t val)
{
    uint8_t tx[2] = { ADS1293_CMD_WRITE(addr), val };
    return hal_spi_txrx(s_spi, tx, NULL, 2);
}

esp_err_t hal_ads1293_reg_read(uint8_t addr, uint8_t *val_out)
{
    uint8_t tx[2] = { ADS1293_CMD_READ(addr), 0x00 };
    uint8_t rx[2] = { 0 };
    esp_err_t err = hal_spi_txrx(s_spi, tx, rx, 2);
    if (err == ESP_OK) *val_out = rx[1];
    return err;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

bool hal_ads1293_data_ready(void)
{
    /* DRDY ISR removed (F0) — GPIO4 has no external pull-up (R17 removed with LED)
     * so interrupt-based detection was unreliable at 853 Hz.  Reads are unconditional
     * in svc_biosignal_acq_step_100hz; this level check is provided for callers
     * that want a quick poll before committing to a full 10-byte SPI transfer. */
    return (gpio_get_level(PIN_ADS1293_DRDY) == 0);
}

esp_err_t hal_ads1293_read_ecg(int32_t *ch1_out, int32_t *ch2_out, int32_t *ch3_out)
{
    /* 10-byte full-duplex: cmd byte + 9 data bytes (CH1+CH2+CH3 ECG, each 3 bytes MSB-first).
     * CH_CNFG = 0x70 (E1+E2+E3) ensures DATA_LOOP returns all three channels.
     * CSB held low for the entire transaction by the SPI master driver. */
    uint8_t tx[10] = { ADS1293_CMD_READ(ADS1293_DATA_LOOP), 0,0,0,0,0,0,0,0,0 };
    uint8_t rx[10] = { 0 };

    /* Take the SPI2 gate to serialise against the display DMA flush (F3 fix).
     * The gate is given by on_trans_done ISR when DMA completes, so the wait
     * here is at most ~2 ms (one display chunk transfer at 40 MHz). */
    SemaphoreHandle_t gate = hal_display_get_spi2_gate();
    if (gate) xSemaphoreTake(gate, portMAX_DELAY);
    esp_err_t err = hal_spi_txrx(s_spi, tx, rx, 10);
    if (gate) xSemaphoreGive(gate);
    if (err != ESP_OK) return err;

    /* rx[0] = don't-care (MISO during cmd byte)
     * rx[1..3] = CH1 ECG H/M/L, rx[4..6] = CH2 ECG H/M/L, rx[7..9] = CH3 ECG H/M/L
     * Sign-extend 24-bit two's complement → int32_t via arithmetic right-shift. */
    *ch1_out = (int32_t)(((uint32_t)rx[1] << 24) |
                         ((uint32_t)rx[2] << 16) |
                         ((uint32_t)rx[3] <<  8)) >> 8;
    *ch2_out = (int32_t)(((uint32_t)rx[4] << 24) |
                         ((uint32_t)rx[5] << 16) |
                         ((uint32_t)rx[6] <<  8)) >> 8;
    *ch3_out = (int32_t)(((uint32_t)rx[7] << 24) |
                         ((uint32_t)rx[8] << 16) |
                         ((uint32_t)rx[9] <<  8)) >> 8;
    return ESP_OK;
}

/* Dump all writable init registers to the log — called when DRDY never asserts. */
static void ads1293_dump_regs(void)
{
    static const uint8_t k_dump[] = {
        ADS1293_CONFIG,       ADS1293_FLEX_CH1_CN,  ADS1293_FLEX_CH2_CN,
        ADS1293_FLEX_CH3_CN,  ADS1293_CMDET_EN,     ADS1293_RLD_CN,
        ADS1293_OSC_CN,       ADS1293_AFE_RES,      ADS1293_AFE_SHDN_CN,
        ADS1293_R2_RATE,      ADS1293_R3_RATE_CH1,  ADS1293_R3_RATE_CH2,
        ADS1293_R3_RATE_CH3,  ADS1293_DRDYB_SRC,    ADS1293_CH_CNFG,
        ADS1293_DATA_STATUS,  ADS1293_ERROR_STATUS,  ADS1293_REVID,
    };
    ESP_LOGE(TAG, "---- ADS1293 register dump (DRDY timeout) ----");
    for (size_t i = 0; i < sizeof(k_dump); i++) {
        uint8_t v = 0xFF;
        hal_ads1293_reg_read(k_dump[i], &v);
        ESP_LOGE(TAG, "  [0x%02X] = 0x%02X", k_dump[i], v);
    }
    ESP_LOGE(TAG, "  DRDY GPIO%d level = %d", PIN_ADS1293_DRDY,
             gpio_get_level(PIN_ADS1293_DRDY));
}

esp_err_t hal_ads1293_init(void)
{
    /* ── 1. Register SPI device on SPI2 (already initialised by hal_display) ──
     * Using 500 kHz and Mode 3 for bring-up:
     *   - 500 kHz gives generous setup/hold margins on signal-integrity issues.
     *   - Mode 3 (CPOL=1, CPHA=1): clock idle HIGH, data captured on falling SCLK.
     *     Forum reports for ADS1293 on Arduino specifically cite Mode 3 as the fix
     *     for DRDY never asserting.  Mode 0 was tried first (REVID reads OK with
     *     both) but did not produce DRDY.  Mode 3 is equivalent to Mode 1 from the
     *     device's sampling perspective but with clock idle HIGH, which matches the
     *     ADS1293 timing diagram more closely when SCLK is shown starting HIGH. */
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 4 * 1000 * 1000,  /* 4 MHz — well within ADS1293's 20 MHz max */
        .mode           = 3,            /* CPOL=1, CPHA=1 — forum-reported fix */
        .spics_io_num   = PIN_ADS1293_CS,
        .queue_size     = 1,
    };
    esp_err_t err = hal_spi_dev_add(SPI2_HOST, &devcfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(err));
        return err;
    }

    /* ── 2. Configure DRDYB GPIO as plain input (no ISR — F0 fix) ───────────
     * The 853 Hz DRDY ISR was causing interrupt pressure on core 0 while reads
     * are unconditional anyway.  GPIO4 also had no external pull-up (R17 removed
     * with LED footprint), making NEGEDGE detection unreliable.
     * hal_ads1293_data_ready() now does a direct level check only. */
    gpio_config_t io = {
        .pin_bit_mask  = (1ULL << PIN_ADS1293_DRDY),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    /* ── 3. Soft-stop any running conversion ─────────────────────────────── */
    err = hal_ads1293_reg_write(ADS1293_CONFIG, ADS1293_CONFIG_STOP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CONFIG stop write failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    /* ── 4. Verify REVID — confirms SPI wiring and mode are functional ───── */
    uint8_t revid = 0;
    err = hal_ads1293_reg_read(ADS1293_REVID, &revid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "REVID read failed: %s", esp_err_to_name(err));
        return err;
    }
    if (revid != ADS1293_REVID_EXPECTED) {
        ESP_LOGE(TAG, "REVID=0x%02X expected 0x%02X — SPI mode/wiring error",
                 revid, ADS1293_REVID_EXPECTED);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "REVID OK (0x%02X) — SPI Mode 3 @ 500 kHz confirmed", revid);

    /* ── 5. 3-lead ECG configuration with write-readback verification ───────
     *
     * Leads: CH1=Lead I (LA−RA), CH2=Lead II (LL−RA), CH3=Lead III (LL−LA)
     * ODR = 102400 / (R1×R2×R3) = 102400 / (4×5×6) ≈ 853 SPS per channel */
    static const uint8_t k_init[][2] = {
        { ADS1293_FLEX_CH1_CN,  0x11 },  /* CH1: Lead I  — INP=IN2(LA), INN=IN1(RA) */
        { ADS1293_FLEX_CH2_CN,  0x19 },  /* CH2: Lead II — INP=IN3(LL), INN=IN1(RA) */
        { ADS1293_FLEX_CH3_CN,  0x13 },  /* CH3: Lead III— INP=IN3(LL), INN=IN2(LA) */
        { ADS1293_CMDET_EN,     0x07 },  /* CM detect on IN1, IN2, IN3 */
        { ADS1293_RLD_CN,       0x04 },  /* RLD amp output → IN4 */
        { ADS1293_OSC_CN,       ADS1293_OSC_INTERNAL },  /* 0x04: CLKFED — RC osc → digital */
        { ADS1293_AFE_SHDN_CN,  0x00 },  /* all AFE channels active (0x24 was wrong) */
        { ADS1293_R2_RATE,      0x02 },  /* R2=5 */
        { ADS1293_R3_RATE_CH1,  0x02 },  /* R3=6 → ~853 SPS CH1 */
        { ADS1293_R3_RATE_CH2,  0x02 },  /* R3=6 → ~853 SPS CH2 */
        { ADS1293_R3_RATE_CH3,  0x02 },  /* R3=6 → ~853 SPS CH3 */
        { ADS1293_DRDYB_SRC,    ADS1293_DRDYB_CH1_ECG },  /* 0x08: CH1 ECG drives DRDYB */
        { ADS1293_CH_CNFG,      ADS1293_CHCNFG_E1_EN | ADS1293_CHCNFG_E2_EN | ADS1293_CHCNFG_E3_EN },
    };

    uint8_t readback = 0;
    bool rb_fail = false;
    for (size_t i = 0; i < sizeof(k_init) / sizeof(k_init[0]); i++) {
        uint8_t reg = k_init[i][0];
        uint8_t val = k_init[i][1];

        err = hal_ads1293_reg_write(reg, val);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Write reg 0x%02X failed: %s", reg, esp_err_to_name(err));
            return err;
        }

        /* Read back immediately to confirm the write landed. */
        err = hal_ads1293_reg_read(reg, &readback);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Readback reg 0x%02X failed: %s", reg, esp_err_to_name(err));
            return err;
        }
        if (readback != val) {
            ESP_LOGE(TAG, "Readback MISMATCH reg 0x%02X: wrote 0x%02X read 0x%02X",
                     reg, val, readback);
            rb_fail = true;
        } else {
            ESP_LOGI(TAG, "  reg 0x%02X = 0x%02X OK", reg, val);
        }
    }

    if (rb_fail) {
        ESP_LOGE(TAG, "Register write verification failed — check SPI mode/wiring");
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* ── 6. Start conversion ──────────────────────────────────────────────── */
    err = hal_ads1293_reg_write(ADS1293_CONFIG, ADS1293_CONFIG_START);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CONFIG start write failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Conversion started (CONFIG=0x01) — polling DRDY...");

    /* ── 7. DRDY diagnostic poll — wait up to 500 ms ────────────────────────
     * At ~853 SPS the first sample should appear within 2 ms.
     * If DRDY stays HIGH we do NOT abort — the GPIO4 external pull-up was
     * The ISR catches brief pulses (the ADS1293 DRDYB pulse may be much shorter
     * than 1 ms, so GPIO level-polling reliably missed it).  Wait up to 2 s
     * for the interrupt flag; if it never fires, probe DATA_LOOP unconditionally.
     * A non-zero probe response means the chip IS sampling — we continue and let
     * step_100hz catch future DRDY pulses via the ISR.  Only a zero probe (chip
     * not responding at all) is treated as a hard failure. */
    bool drdy_seen = false;
    for (int ms = 0; ms < 2000; ms++) {
        if (gpio_get_level(PIN_ADS1293_DRDY) == 0) {
            ESP_LOGI(TAG, "DRDY asserted (LOW) after %d ms — ADS1293 running", ms);
            drdy_seen = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!drdy_seen) {
        ads1293_dump_regs();
        int32_t p1 = 0, p2 = 0, p3 = 0;
        hal_ads1293_read_ecg(&p1, &p2, &p3);
        ESP_LOGW(TAG, "DRDY not seen in 2000 ms. Probe DATA_LOOP: CH1=%ld CH2=%ld CH3=%ld",
                 (long)p1, (long)p2, (long)p3);
        if (p1 == 0 && p2 == 0 && p3 == 0) {
            ESP_LOGE(TAG, "Probe returned zero — chip not running");
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGW(TAG, "Chip IS sampling (non-zero probe). Continuing — check DRDY wiring.");
    }

    ESP_LOGI(TAG, "Init OK — 3-lead ECG ~853 SPS, SPI Mode 3 4 MHz, DRDY=GPIO%d CS=GPIO%d",
             PIN_ADS1293_DRDY, PIN_ADS1293_CS);
    return ESP_OK;
}
