#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef void (*hal_display_locked_fn_t)(void *ctx);

/* Initialise SPI2 bus, ILI9341 panel driver, LVGL 9, and launch the LVGL task.
 * Must be called once from app_main before any LVGL calls. */
void hal_display_init(void);

/* Take the LVGL mutex. timeout_ms = 0 means wait indefinitely.
 * Returns true if the lock was acquired. */
bool hal_display_lock_ms(uint32_t timeout_ms);

/* Release the LVGL mutex. */
void hal_display_unlock(void);

/* Higher-level helper: poll until total_timeout_ms elapses, run fn(ctx) under
 * the lock, then unlock. Returns false if the lock could not be acquired within
 * total_timeout_ms. */
bool hal_display_lock(uint32_t total_timeout_ms,
                      const char *who,
                      hal_display_locked_fn_t fn,
                      void *ctx);
