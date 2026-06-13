#include "hal_ads1293.h"
#include "hal_ads1293_regs.h"
#include "hal_bus.h"
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
    return gpio_get_level(PIN_ADS1293_DRDY) == 0;
}

esp_err_t hal_ads1293_read_ecg(int32_t *ch1_out, int32_t *ch2_out, int32_t *ch3_out)
{
    /* 10-byte full-duplex: cmd byte + 9 data bytes (CH1+CH2+CH3 ECG, each 3 bytes MSB-first).
     * CH_CNFG = 0x70 (E1+E2+E3) ensures DATA_LOOP returns all three channels.
     * CSB held low for the entire transaction by the SPI master driver. */
    uint8_t tx[10] = { ADS1293_CMD_READ(ADS1293_DATA_LOOP), 0,0,0,0,0,0,0,0,0 };
    uint8_t rx[10] = { 0 };

    esp_err_t err = hal_spi_txrx(s_spi, tx, rx, 10);
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

esp_err_t hal_ads1293_init(void)
{
    /* ── 1. Register SPI device on SPI2 (already initialised by hal_display) ── */
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 4 * 1000 * 1000,  /* 4 MHz — well below 20 MHz max */
        .mode           = 0,                  /* CPOL=0, CPHA=0 — confirmed by Protocentral Arduino ref */
        .spics_io_num   = PIN_ADS1293_CS,
        .queue_size     = 1,
    };
    esp_err_t err = hal_spi_dev_add(SPI2_HOST, &devcfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(err));
        return err;
    }

    /* ── 2. Configure DRDYB GPIO as input with weak pull-up ───────────────── */
    gpio_config_t io = {
        .pin_bit_mask  = (1ULL << PIN_ADS1293_DRDY),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    /* ── 3. Soft-stop any running conversion (also resets START_CON) ──────── */
    err = hal_ads1293_reg_write(ADS1293_CONFIG, ADS1293_CONFIG_STOP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CONFIG stop write failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2));  /* allow ADC pipeline to flush */

    /* ── 4. Verify REVID ──────────────────────────────────────────────────── */
    uint8_t revid = 0;
    err = hal_ads1293_reg_read(ADS1293_REVID, &revid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "REVID read failed: %s", esp_err_to_name(err));
        return err;
    }
    if (revid != ADS1293_REVID_EXPECTED) {
        ESP_LOGE(TAG, "REVID=0x%02X expected 0x%02X — chip absent or SPI wiring error",
                 revid, ADS1293_REVID_EXPECTED);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "REVID OK (0x%02X)", revid);

    /* ── 5. Two-lead ECG configuration (TI datasheet §9.2.1 sequence) ──────
     *
     * Leads: CH1 = Lead I  (LA–RA): INP=IN2, INN=IN1
     *        CH2 = Lead II (LL–RA): INP=IN3, INN=IN1
     *
     * ODR = fs / (R1 × R2 × R3)
     *     = 102400 / (4 × 5 × 6) ≈ 853 SPS per channel.
     * ecg_sampler_task polls at 100 Hz and reads the latest sample each tick.
     *
     * OSC_CN=0x00: internal RC oscillator (no external crystal required).
     * To use an external 4.096 MHz crystal, change OSC_CN to 0x04. */
    static const uint8_t k_init[][2] = {
        { ADS1293_FLEX_CH1_CN,  0x11 },  /* CH1: Lead I  — INP=IN2(LA), INN=IN1(RA) */
        { ADS1293_FLEX_CH2_CN,  0x19 },  /* CH2: Lead II — INP=IN3(LL), INN=IN1(RA) */
        { ADS1293_FLEX_CH3_CN,  0x13 },  /* CH3: Lead III— INP=IN3(LL), INN=IN2(LA) */
        { ADS1293_CMDET_EN,     0x07 },  /* CM detect on IN1, IN2, IN3 */
        { ADS1293_RLD_CN,       0x04 },  /* RLD amp output → IN4 */
        { ADS1293_OSC_CN,       ADS1293_OSC_INTERNAL },  /* 0x04: feed RC osc to digital */
        { ADS1293_AFE_SHDN_CN,  0x00 },  /* all AFE channels active — confirmed by Protocentral ref */
        { ADS1293_R2_RATE,      0x02 },  /* R2 = 5 */
        { ADS1293_R3_RATE_CH1,  0x02 },  /* R3 = 6 → ~853 SPS, CH1 */
        { ADS1293_R3_RATE_CH2,  0x02 },  /* R3 = 6 → ~853 SPS, CH2 */
        { ADS1293_R3_RATE_CH3,  0x02 },  /* R3 = 6 → ~853 SPS, CH3 */
        { ADS1293_DRDYB_SRC,    ADS1293_DRDYB_CH1_ECG },  /* 0x08: CH1 ECG drives DRDYB */
        { ADS1293_CH_CNFG,      ADS1293_CHCNFG_E1_EN | ADS1293_CHCNFG_E2_EN | ADS1293_CHCNFG_E3_EN },  /* 0x70 */
    };

    for (size_t i = 0; i < sizeof(k_init) / sizeof(k_init[0]); i++) {
        err = hal_ads1293_reg_write(k_init[i][0], k_init[i][1]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Init reg 0x%02X write failed: %s",
                     k_init[i][0], esp_err_to_name(err));
            return err;
        }
    }

    /* ── 6. Start conversion ──────────────────────────────────────────────── */
    err = hal_ads1293_reg_write(ADS1293_CONFIG, ADS1293_CONFIG_START);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CONFIG start write failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Init OK — 3-lead ECG, ~853 SPS, internal RC osc, DRDY=GPIO%d CS=GPIO%d",
             PIN_ADS1293_DRDY, PIN_ADS1293_CS);
    return ESP_OK;
}
