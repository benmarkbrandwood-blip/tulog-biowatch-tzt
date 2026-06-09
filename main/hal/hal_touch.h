#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Initialise XPT2046 resistive touch controller (SPI2 shared with display,
 * CS=GPIO33) and register the LVGL pointer input device.
 * Touch detection uses Z-pressure measurement — PENIRQ (GPIO36) is not
 * connected on this board and is not used.
 * Must be called after hal_display_init() (SPI2 bus must already exist). */
void hal_touch_init(void);

/* Override the axis calibration constants computed from corner touches.
 * x_min = raw_x at RIGHT screen edge (low)
 * x_max = raw_x at LEFT  screen edge (high)
 * y_min = raw_y at BOTTOM screen edge (low)
 * y_max = raw_y at TOP   screen edge (high)
 * Call after hal_touch_init(), typically with values loaded from NVS. */
void hal_touch_set_calibration(uint16_t x_min, uint16_t x_max,
                                uint16_t y_min, uint16_t y_max);

/* Read raw (uncalibrated) X/Y if pen is down.
 * Returns true and fills *x, *y when Z-pressure > threshold.
 * Used by the touch calibration routine. */
bool hal_touch_read_raw(uint16_t *x, uint16_t *y);
