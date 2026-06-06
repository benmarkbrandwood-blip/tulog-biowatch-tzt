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
#include <string.h>

/* ── Constants ───────────────────────────────────────────────────────────── */

/* 320 × 16 × 2 = 10 240 bytes per buffer; two buffers = 20 480 bytes total.
 * No rotation buffer needed — orientation is handled by hardware MADCTL. */
#define LVGL_DRAW_BUF_LINES  16
#define LVGL_TASK_STACK      8192
#define LVGL_TASK_PRIORITY   4

/* Bytes per pixel for RGB565 (display native format at LV_COLOR_DEPTH=16). */
#define BYTES_PER_PIXEL      2

static const char *TAG = "Display";

/* ── Module state ────────────────────────────────────────────────────────── */

static SemaphoreHandle_t         s_lvgl_mutex = NULL;
static esp_lcd_panel_handle_t    s_panel      = NULL;
static esp_lcd_panel_io_handle_t s_io_handle  = NULL;  /* kept for explicit MADCTL writes */
static lv_display_t             *s_disp       = NULL;

/* ── DMA-done callback (called from ISR) ────────────────────────────────── */

static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    if (s_disp) lv_display_flush_ready(s_disp);
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
    /* ILI9341 over SPI expects big-endian RGB565; LVGL renders little-endian. */
    uint32_t px_count = (uint32_t)(area->x2 - area->x1 + 1)
                      * (uint32_t)(area->y2 - area->y1 + 1);
    lv_draw_sw_rgb565_swap(px_map, px_count);

    esp_lcd_panel_draw_bitmap(s_panel,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              px_map);
    /* lv_display_flush_ready() is called from on_trans_done ISR after DMA. */
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
    s_io_handle = io_handle;  /* keep for post-init MADCTL writes (Phase 1) */

    /* 3. ILI9341 panel driver */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,  /* BGR rendered red as blue; reference demo confirms RGB */
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    /* ── Phase 0: read chip ID to confirm ILI9341 vs ST7789 ─────────────────
     * 0xD3 (Read ID4): ILI9341 returns {dummy, 0x00, 0x93, 0x41}
     *                  ST7789  returns {dummy, 0x00, 0x85, 0x52} (approx)
     * 0x04 (RDDID):    ILI9341 returns {dummy, mfr, ver, 0x41}
     * Bytes are logged raw; if all zeros, MISO may be unconnected or need a
     * different spi_mode for reads — identification still comes from factory demo.
     */
    uint8_t id4[4] = {0};
    uint8_t rddid[4] = {0};
    esp_err_t id_err4   = esp_lcd_panel_io_rx_param(s_io_handle, 0xD3, id4,   sizeof(id4));
    esp_err_t id_errRDD = esp_lcd_panel_io_rx_param(s_io_handle, 0x04, rddid, sizeof(rddid));
    ESP_LOGI(TAG, "Panel ID — 0xD3: %02X %02X %02X %02X (err=%d)   "
                  "RDDID 0x04: %02X %02X %02X %02X (err=%d)",
             id4[0], id4[1], id4[2], id4[3], (int)id_err4,
             rddid[0], rddid[1], rddid[2], rddid[3], (int)id_errRDD);
    /* Expected for ILI9341: 0xD3 → XX 00 93 41  */

    /* Write MADCTL FIRST so the GRAM clear runs in the correct scan direction.
     * If clear runs before MADCTL is set, stale landscape GRAM bleeds through. */

    /* Write the full MADCTL register (0x36) in one shot.
     * This overwrites ALL bits including any scan-order bits left by the init
     * sequence — avoids the OR-only-one-bit problem of esp_lcd_panel_swap_xy /
     * mirror.  LCD_MADCTL is defined in app_config.h; change it and reflash to
     * sweep orientations without touching any other code.
     * BGR bit (0x08) is CLEAR — RGB element order + lv_draw_sw_rgb565_swap()
     * in the flush is confirmed-correct for this panel. */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io_handle, 0x36,
                    (uint8_t[]){LCD_MADCTL}, 1));
    ESP_LOGI(TAG, "MADCTL set to 0x%02X", (unsigned)LCD_MADCTL);

    /* Clear the full native GRAM now that MADCTL is set — clears in the
     * correct scan direction so no stale content bleeds through. */
    static uint16_t s_clear_line[LCD_NATIVE_W];
    memset(s_clear_line, 0x00, sizeof(s_clear_line));
    for (int row = 0; row < LCD_NATIVE_H; row++) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, row, LCD_NATIVE_W, row + 1, s_clear_line);
    }

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* 4. LVGL core + tick source */
    lv_init();
    lv_tick_set_cb(lvgl_get_tick_ms);

    /* 5. LVGL display object + flush callback.
     *    Create at the logical landscape resolution — MADCTL has already told the
     *    panel to treat its long axis as horizontal, so LVGL's 320-wide rows map
     *    directly to 320-wide GRAM rows with no software rotation needed. */
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
