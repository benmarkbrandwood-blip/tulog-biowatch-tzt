#include "hal_display.h"
#include "app_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_io_spi.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "lvgl.h"

/* ── Constants ───────────────────────────────────────────────────────────── */

/* 240 × 32 × 2 bytes = 15 360 bytes per buffer (~15 KB each, two buffers ~30 KB). */
#define LVGL_DRAW_BUF_LINES  32
#define LVGL_TASK_STACK      4096
#define LVGL_TASK_PRIORITY   4

/* Bytes per pixel for RGB565 (display native format at LV_COLOR_DEPTH=16). */
#define BYTES_PER_PIXEL      2

static const char *TAG = "Display";

/* ── Module state ────────────────────────────────────────────────────────── */

static SemaphoreHandle_t      s_lvgl_mutex  = NULL;
static esp_lcd_panel_handle_t s_panel       = NULL;
static lv_display_t          *s_disp        = NULL;

/* ── DMA-done callback (called from ISR) ────────────────────────────────── */

static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    lv_display_flush_ready(s_disp);
    return false;
}

/* ── Tick source ─────────────────────────────────────────────────────────── */

static uint32_t lvgl_get_tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* ── Flush callback (called by LVGL timer handler under mutex) ───────────── */

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* ILI9341 over SPI expects big-endian RGB565.  LVGL 9 renders in
     * little-endian native byte order, so swap each pair of bytes in-place
     * before the DMA transfer.  If colours appear inverted (blue/red swapped),
     * remove this call — some board revisions do not need it. */
    uint32_t px_count = (uint32_t)(area->x2 - area->x1 + 1)
                      * (uint32_t)(area->y2 - area->y1 + 1);
    lv_draw_sw_rgb565_swap(px_map, px_count);

    esp_lcd_panel_draw_bitmap(s_panel,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              px_map);
    /* lv_display_flush_ready() is called from the on_trans_done ISR callback
     * once the DMA transfer completes.  Do NOT call it here. */
}

/* ── LVGL task ───────────────────────────────────────────────────────────── */

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task running on core %d", (int)xPortGetCoreID());
    while (1) {
        if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            uint32_t next_ms = lv_timer_handler();
            xSemaphoreGive(s_lvgl_mutex);
            if (next_ms < 1)   next_ms = 1;
            if (next_ms > 100) next_ms = 100;
            vTaskDelay(pdMS_TO_TICKS(next_ms));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void hal_display_init(void)
{
    /* 1. SPI2 bus (display only — SD card uses SPI3/VSPI in hal_storage.c) */
    spi_bus_config_t buscfg = {
        .mosi_io_num     = PIN_LCD_MOSI,
        .miso_io_num     = PIN_LCD_MISO,
        .sclk_io_num     = PIN_LCD_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * BYTES_PER_PIXEL,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* 2. SPI panel IO — wire the DMA-done event to our ISR callback so
     *    lv_display_flush_ready() is signalled only after the transfer ends. */
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num          = PIN_LCD_CS,
        .dc_gpio_num          = PIN_LCD_DC,
        .spi_mode             = 0,
        .pclk_hz              = 40 * 1000 * 1000,
        .lcd_cmd_bits         = 8,
        .lcd_param_bits       = 8,
        .trans_queue_depth    = 10,
        .on_color_trans_done  = on_trans_done,
        .user_ctx             = NULL,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    /* 3. ILI9341 panel driver */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* 4. LVGL core + tick source */
    lv_init();
    lv_tick_set_cb(lvgl_get_tick_ms);

    /* 5. LVGL display object + flush callback */
    s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);

    /* 6. Draw buffers — DMA-capable internal SRAM; 2 bytes/pixel (RGB565).
     *    Do NOT use sizeof(lv_color_t) — in LVGL 9 that struct is 3 bytes. */
    const size_t buf_bytes = (size_t)LCD_H_RES * LVGL_DRAW_BUF_LINES * BYTES_PER_PIXEL;
    void *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "LVGL draw buffer allocation failed (need %u bytes each)", (unsigned)buf_bytes);
        abort();
    }
    lv_display_set_buffers(s_disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* 7. LVGL mutex + task (pinned to UI_AUX_CORE, same core as clock/button tasks) */
    s_lvgl_mutex = xSemaphoreCreateMutex();
    configASSERT(s_lvgl_mutex);
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL,
                            LVGL_TASK_PRIORITY, NULL, UI_AUX_CORE);

    ESP_LOGI(TAG, "Init OK — ILI9341 %d×%d, 2×%u-byte draw buffers",
             LCD_H_RES, LCD_V_RES, (unsigned)buf_bytes);
}

bool hal_display_lock_ms(uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY
                                         : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_lvgl_mutex, ticks) == pdTRUE;
}

void hal_display_unlock(void)
{
    xSemaphoreGive(s_lvgl_mutex);
}

bool hal_display_lock(uint32_t total_timeout_ms,
                      const char *who,
                      hal_display_locked_fn_t fn,
                      void *ctx)
{
    uint32_t waited_ms = 0;

    while (waited_ms < total_timeout_ms) {
        if (hal_display_lock_ms(DISPLAY_LOCK_SLICE_MS)) {
            if (fn) fn(ctx);
            hal_display_unlock();
            return true;
        }
        waited_ms += DISPLAY_LOCK_SLICE_MS;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "%s: failed to acquire display lock after %u ms", who, total_timeout_ms);
    return false;
}
