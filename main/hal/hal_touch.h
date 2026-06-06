#pragma once

/* Initialise XPT2046 resistive touch controller (SPI2 shared with display,
 * CS=GPIO33) and register the LVGL pointer input device.
 * Touch detection uses Z-pressure measurement — PENIRQ (GPIO36) is not
 * connected on this board and is not used.
 * Must be called after hal_display_init() (SPI2 bus must already exist). */
void hal_touch_init(void);
