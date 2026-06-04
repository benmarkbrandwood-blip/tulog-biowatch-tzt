#pragma once

/* Initialise CST820 touch controller (I2C bus on GPIO33/32, reset via GPIO25/21)
 * and register the LVGL pointer input device.
 * Must be called after hal_display_init() (requires lv_init() to have run). */
void hal_touch_init(void);
