#pragma once
#include "esp_err.h"
#include <stdint.h>

/**
 * hal_max30102_init — configure MAX30102 for SpO2 mode at 100 SPS.
 * Verifies Part ID (0x15), resets the chip, and enables both Red and IR LEDs.
 * Call once after hal_bus_init().
 */
esp_err_t hal_max30102_init(void);

/**
 * hal_max30102_read_fifo — drain any pending FIFO samples.
 *
 * Reads FIFO write/read pointers to determine how many samples are available,
 * drains them all, and returns the latest IR and Red values in the out pointers.
 * Each sample is 18-bit (0–262143).
 *
 * Returns ESP_ERR_NOT_FOUND when the FIFO is empty (no new sample since last call).
 */
esp_err_t hal_max30102_read_fifo(int32_t *ir_out, int32_t *red_out);

/**
 * hal_max30102_get_spo2 — latest SpO2 estimate (0–100 %).
 * Updated every ~4 s from the Welford online variance accumulator.
 * Returns 0 until enough samples have accumulated or signal is too weak.
 */
uint8_t hal_max30102_get_spo2(void);
