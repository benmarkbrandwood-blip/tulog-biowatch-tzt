#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

/* Returns the FreeRTOS handle for the LVGL port task (for stack HWM monitoring).
 * May return NULL before hal_display_init() completes. */
TaskHandle_t hal_display_get_lvgl_task(void);

/* Returns a binary semaphore that gates SPI2 bus access.
 *
 * Problem (H1, confirmed 2026-06-14): esp_lcd DMA flush (core 1) and ADS1293
 * blocking spi_device_transmit (core 0) simultaneously contend the SPI2 bus
 * lock.  The ESP-IDF SPI driver's own arbitration deadlocks in this scenario
 * (flush=1 ads=1 for 8+ s, Task WDT fires on LVGL task).
 *
 * Solution (F3): gate both callers through this binary semaphore so only one
 * can hold SPI2 at a time.  hal_ads1293_read_ecg() takes it before transmit
 * and gives it after.  lvgl_flush_cb() takes it before esp_lcd_panel_draw_bitmap()
 * and on_trans_done ISR gives it back (xSemaphoreGiveFromISR).
 *
 * Initialised and given (available) in hal_display_init(). */
SemaphoreHandle_t hal_display_get_spi2_gate(void);
