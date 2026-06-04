#pragma once
#include <stdint.h>

void hal_backlight_init(void);
void hal_backlight_set_percent(uint8_t pct);
void hal_backlight_on(void);
void hal_backlight_off(void);
