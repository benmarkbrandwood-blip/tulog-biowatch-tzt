#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * hal_ads1293_init — register SPI2 device, configure DRDY GPIO, run the
 *   two-lead ECG initialisation sequence, and verify REVID == 0x01.
 *
 * Must be called after hal_display_init() (which initialises SPI2).
 * Returns ESP_OK on success, ESP_ERR_NOT_FOUND if REVID mismatches.
 */
esp_err_t hal_ads1293_init(void);

/**
 * hal_ads1293_data_ready — true when DRDYB is asserted (active-low GPIO level 0).
 * Non-blocking; safe to call at task rate.
 */
bool hal_ads1293_data_ready(void);

/**
 * hal_ads1293_read_ecg — read CH1 and CH2 ECG via DATA_LOOP streaming.
 * Outputs: 24-bit sign-extended values in *ch1_out and *ch2_out.
 * Call only when hal_ads1293_data_ready() returns true.
 */
esp_err_t hal_ads1293_read_ecg(int32_t *ch1_out, int32_t *ch2_out, int32_t *ch3_out);

/**
 * hal_ads1293_reg_read / hal_ads1293_reg_write — low-level register access.
 * Exposed for diagnostics / future calibration from the acquisition service.
 */
esp_err_t hal_ads1293_reg_read(uint8_t addr, uint8_t *val_out);
esp_err_t hal_ads1293_reg_write(uint8_t addr, uint8_t val);
