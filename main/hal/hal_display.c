#include "hal_display.h"
#include "app_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
/* TODO Phase 2: bsp/esp-bsp.h removed — replace bsp_display_lock/unlock with
 * a FreeRTOS mutex in hal_display_init() and hal_display_lock() below. */

static const char *TAG = "Display";

bool hal_display_lock(uint32_t total_timeout_ms,
                      const char *who,
                      hal_display_locked_fn_t fn,
                      void *ctx)
{
    uint32_t waited_ms = 0;

    while (waited_ms < total_timeout_ms) {
        if (bsp_display_lock(DISPLAY_LOCK_SLICE_MS)) {
            if (fn) {
                fn(ctx);
            }
            bsp_display_unlock();
            return true;
        }

        waited_ms += DISPLAY_LOCK_SLICE_MS;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "%s: failed to acquire display lock after %u ms", who, total_timeout_ms);
    return false;
}
