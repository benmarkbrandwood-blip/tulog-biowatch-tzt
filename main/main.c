/**
 * @file    main.c
 * @brief   Waveshare ESP32-S3-Touch-AMOLED-2.06 Watch
 *
 * Features preserved:
 * - boots to WiFi screen
 * - WiFi screen has Main Menu button
 * - WiFi also available from Settings
 * - remembers WiFi passwords in NVS per SSID
 * - password field is masked, so user can reconnect without retyping
 * - battery percentage shown
 * - ESP-IDF 5/6 compatible ADC battery code
 *
 * New Health screen features:
 * - custom ECG screen, no back button, no placeholder text
 * - top bar shared across tabs: HR, SpO2 placeholder, RESP placeholder, battery, SD
 * - bottom tabs: ECG, PPG, RESP, NAS, FCG
 * - ECG tab active now; others are placeholders using same top bar
 * - 100 Hz ECG sampling task on a separate core from UI work
 * - 4 second repeating ECG waveform
 * - sample timing drift display
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "lvgl.h"

#include "hal_backlight.h"
#include "hal_touch.h"

/* SD card / filesystem */
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "freertos/queue.h"
#include <sys/time.h>
#include <math.h>
#include <errno.h>

#include "app_config.h"
#include "app_state.h"
#include "ui_common.h"
#include "hal_battery.h"
#include "sig_pipeline.h"
#include "sig_ppg.h"
#include "svc_nvs.h"
#include "svc_time.h"
#include "svc_record.h"
#include "svc_bp_record.h"
#include "hal_display.h"
#include "hal_storage.h"
#include "svc_files.h"

static const char *TAG = "WatchApp";

/* -------------------------------------------------------------------------- */
/* Config (hardware-specific; numeric/string constants are in app_config.h)  */
/* -------------------------------------------------------------------------- */

/* GPIO */
#define BOOT_BTN_GPIO           GPIO_NUM_0

/* ECG ADC: keep onboard ADC sampling on ECG channel only for now */
#define ECG_ADC_UNIT             ADC_UNIT_2
#define ECG_ADC_CHANNEL          ADC_CHANNEL_3   // GPIO14
#define ECG_GPIO                 GPIO_NUM_14
#define ECG_ADC_ATTEN            ADC_ATTEN_DB_12
#define ECG_ADC_BITWIDTH         ADC_BITWIDTH_DEFAULT

/* SD mount point is now defined in app_config.h as SD_MOUNT_POINT "/sdcard" */

static float s_ecg_hp_state = 0.0f;
static float s_ecg_lp_state = 0.0f;
static float s_ecg_prev_input = 0.0f;
static float s_ecg_prev_band = 0.0f;
static float s_ecg_mwi_sum = 0.0f;
static float s_ecg_mwi_buf[ECG_WINDOW_SAMPLES] = {0};
static uint32_t s_ecg_mwi_index = 0;
static float s_ecg_signal_level = 0.0f;
static float s_ecg_noise_level = 0.0f;
static float s_ecg_detect_threshold = 0.0f;

static uint32_t              s_sim_phase        = 0;
static int                   s_resp_rate_bpm    = 0;

/* -------------------------------------------------------------------------- */
/* WiFi state                                                                 */
/* -------------------------------------------------------------------------- */

static wifi_ap_record_t   s_ap_records[MAX_SCAN_RESULTS];
static uint16_t           s_ap_count = 0;
static char               s_selected_ssid[33] = {0};
static char               s_password[65]      = {0};
static EventGroupHandle_t s_wifi_event_group  = NULL;

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static int  s_retry_count = 0;
static bool s_wifi_opened_from_settings = false;
static bool s_wifi_opened_from_files    = false;
static bool s_files_wifi_kept_alive     = false; /* true while Wi-Fi is held up for file upload */
static bool s_files_was_active          = false; /* true once Files screen has been the active screen */
static bool s_wifi_started = false;
static bool s_wifi_shutdown_requested = false;
static bool s_wifi_connecting = false;
static bool s_wifi_connected = false;

/* -------------------------------------------------------------------------- */
/* ECG state                                                                  */
/* -------------------------------------------------------------------------- */

static adc_oneshot_unit_handle_t s_ecg_adc_handle = NULL;

static portMUX_TYPE s_ecg_spinlock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_ecg_task = NULL;
static bool s_ecg_sampling_enabled = true;

static int32_t  s_ecg_raw[ECG_WINDOW_SAMPLES];    /* int32_t: holds 24-bit ADS127L18 */
static float    s_ecg_band[ECG_WINDOW_SAMPLES];   /* bandpassed ECG — used for R-peak search */
static uint32_t s_ecg_expected_ms[ECG_WINDOW_SAMPLES];
static uint32_t s_ecg_actual_ms[ECG_WINDOW_SAMPLES];

static uint32_t s_ecg_write_index = 0;
static uint32_t s_ecg_total_samples = 0;
static int s_ecg_last_raw = 0;
static int s_ecg_min = INT32_MAX;
static int s_ecg_max = 0;
static int s_ecg_hr_bpm = 0;
static int32_t s_ecg_sample_drift_ms = 0;
static int32_t  s_ecg_last_peak_ms      = -10000;
static int32_t  s_ecg_last_rpeak_expected = -1; /* last refined R-peak in expected_ms; for RR */
static int      s_ecg_threshold          = 2200;
static uint32_t s_ecg_task_start_ms     = 0;   /* wall-clock ms when sampler task started */

/* Deferred R-peak search state */
static bool     s_ecg_pending_beat       = false;
static uint32_t s_ecg_beat_detect_idx   = 0;
static uint32_t s_ecg_beat_detect_sample = 0;
static int16_t s_resp_history[RESP_RATE_WINDOW_SECONDS * ECG_SAMPLE_HZ];
static uint32_t s_resp_hist_index = 0;
static uint32_t s_resp_hist_count = 0;
static int s_resp_dup_delay_samples = (RESP_DUP_DELAY_MS * ECG_SAMPLE_HZ) / 1000;
static int s_resp_dup_elevation = RESP_DUP_ELEVATION;

/* PPG — protected by s_ecg_spinlock */
static int32_t s_ppg_raw[ECG_WINDOW_SAMPLES];     /* int32_t: holds 19-bit MAX86140 */
static int32_t s_ppg_min = INT32_MAX;
static int32_t s_ppg_max = INT32_MIN;

/* PPG bandpass filter state (0.5–16 Hz first-order IIR, mirrors ECG filter) */
static float s_ppg_hp_state   = 0.0f;
static float s_ppg_lp_state   = 0.0f;
static float s_ppg_prev_input = 0.0f;

/* Staggered SD writes: spread hr and resp across 2 samples after each beat */
static int     s_rec_stagger      = 0;   /* counts down 2→1→0 after beat */
static uint8_t s_rec_pending_hr   = 0;
static uint8_t s_rec_pending_resp = 0;

/* Battery percent cached by clock_update_task (1 Hz); read by ecg_sampler_task. */
static volatile int s_cached_batt_pct = -1;

static health_tab_t s_active_health_tab = HEALTH_TAB_ECG;

/* -------------------------------------------------------------------------- */
/* Tasks & power state                                                        */
/* -------------------------------------------------------------------------- */

static TaskHandle_t s_clock_task = NULL;

static bool    s_screen_on     = true;
static int     s_brightness    = DEFAULT_BRIGHTNESS;
static int     s_timeout_s     = DEFAULT_TIMEOUT_S;
static int64_t s_last_activity = 0;
static int64_t s_nav_rec_us    = 0;  /* set when user navigates to record screen */

/* -------------------------------------------------------------------------- */
/* LVGL objects                                                               */
/* -------------------------------------------------------------------------- */

static lv_obj_t *s_scr_home         = NULL;
static lv_obj_t *s_scr_wifi         = NULL;
static lv_obj_t *s_scr_pass         = NULL;
static lv_obj_t *s_scr_conn         = NULL;
static lv_obj_t *s_scr_health       = NULL;
/* s_scr_sleep retained as a stub: the Sleep tile was removed from the main
 * menu, but other code paths (BOOT button, inactivity timer) still drive
 * the screen-sleep behaviour without needing a dedicated screen object. */
static lv_obj_t *s_scr_sleep        __attribute__((unused)) = NULL;
static lv_obj_t *s_scr_pump         = NULL;
static lv_obj_t *s_scr_files        = NULL;
static lv_obj_t *s_scr_about        = NULL;

/* Files screen */
static lv_obj_t   *s_files_list_cont   = NULL;
static lv_obj_t   *s_lbl_files_status  = NULL;
static lv_obj_t   *s_lbl_files_xfer    = NULL;
static lv_obj_t   *s_lbl_files_detail  = NULL;
static lv_obj_t   *s_lbl_files_wifi    = NULL;
static lv_obj_t   *s_btn_files_send    = NULL;
static lv_obj_t   *s_btn_files_delete  = NULL;
static lv_timer_t *s_files_ui_timer    = NULL;
static watch_file_list_t s_files_model = {0};
static int         s_files_selected    = -1;
static lv_obj_t   *s_files_row_btns[FILES_MAX_LIST_COUNT];
static lv_obj_t *s_scr_settings     = NULL;

static lv_obj_t *s_lbl_time         = NULL;
static lv_obj_t *s_lbl_date         = NULL;
static lv_obj_t *s_lbl_time_hint    = NULL;
static lv_obj_t *s_lbl_battery      = NULL;

static lv_obj_t *s_wifi_container   = NULL;
static lv_obj_t *s_wifi_status      = NULL;

static lv_obj_t *s_ta_pass          = NULL;
static lv_obj_t *s_lbl_ssid         = NULL;
static lv_obj_t *s_keyboard         = NULL;
static lv_obj_t *s_lbl_saved_hint   = NULL;

static lv_obj_t *s_slider_bright    = NULL;
static lv_obj_t *s_lbl_bright_val   = NULL;
static lv_obj_t *s_popup            = NULL;

/* Record screen LVGL objects */
static lv_obj_t          *s_scr_record          = NULL;
static lv_obj_t          *s_rec_topbar           = NULL;
static lv_obj_t          *s_rec_plot_card        = NULL;
static lv_obj_t          *s_rec_plot             = NULL;
static lv_obj_t          *s_lbl_rec_tab_name    = NULL;   /* cycle nav label */
static lv_obj_t          *s_lbl_rec_rr           = NULL;
static lv_obj_t          *s_lbl_rec_hr           = NULL;
static lv_obj_t          *s_lbl_rec_spo2         = NULL;
static lv_obj_t          *s_lbl_rec_batt         = NULL;
static lv_obj_t          *s_lbl_rec_pat          = NULL;
static lv_obj_t          *s_lbl_rec_sd           = NULL;
static lv_obj_t          *s_lbl_rec_status       = NULL;
static lv_obj_t          *s_btn_rec_startstop    = NULL;
static lv_obj_t          *s_lbl_rec_btn          = NULL;
static lv_obj_t          *s_rec_name_ta          = NULL;
static lv_obj_t          *s_rec_keyboard         = NULL;
static lv_timer_t        *s_rec_ui_timer         = NULL;
static lv_chart_series_t *s_rec_series           = NULL;
static lv_coord_t         s_rec_chart_points[ECG_WINDOW_SAMPLES];
static rec_tab_t          s_active_rec_tab       = REC_TAB_ECG;

/* B.P. screen LVGL objects */
static lv_obj_t          *s_scr_bp              = NULL;
static lv_obj_t          *s_lbl_bp_status       = NULL;
static lv_obj_t          *s_lbl_bp_countdown    = NULL;
static lv_obj_t          *s_btn_bp_start        = NULL;
static lv_obj_t          *s_lbl_bp_btn          = NULL;
static lv_obj_t          *s_bp_chart_card       = NULL;
static lv_obj_t          *s_bp_chart            = NULL;
static lv_obj_t          *s_lbl_bp_hrv          = NULL;
static lv_obj_t          *s_lbl_bp_pat_stat     = NULL;
static lv_obj_t          *s_btn_bp_dur[3]       = {NULL, NULL, NULL};
static lv_timer_t        *s_bp_ui_timer         = NULL;
static lv_chart_series_t *s_bp_rr_series        = NULL;
static lv_chart_series_t *s_bp_pat_series       = NULL;
static lv_coord_t         s_bp_chart_rr[64]     = {0};
static lv_coord_t         s_bp_chart_pat[64]    = {0};

/* B.P. screen state */
static TaskHandle_t       s_bp_sampler_task     = NULL;
static uint32_t           s_bp_chosen_dur_s     = BP_DURATION_60S;
static int64_t            s_nav_bp_us           = 0;
static volatile bool      s_bp_analysis_ready   = false;
static bp_analysis_t      s_bp_last_result      = {0};
static volatile bool      s_bp_was_recording    = false; /* tracks recording→idle transition */

/* -------------------------------------------------------------------------- */
/* Forward declarations                                                       */
/* -------------------------------------------------------------------------- */

static void ui_create_home_screen(void);
static void ui_create_wifi_screen(void);
static void ui_create_password_screen(void);
static void ui_create_connecting_screen(void);
static void ui_create_app_screens(void);
static void ui_create_settings_screen(void);

static void wifi_driver_init(void);
static esp_err_t wifi_ensure_started(void);
static void wifi_shutdown_after_time_sync(void);
static void wifi_scan_task(void *arg);
static void wifi_connect_task(void *arg);
static void clock_update_task(void *arg);
static void boot_btn_task(void *arg);
static void ecg_sampler_task(void *arg);

static void screen_sleep(void);
static void screen_wake(void);
static void reset_activity(void);

static void wifi_button_list_populate(void);
static void clear_wifi_container(void);
static void update_home_time_labels(void);
static void ui_show_error_popup(const char *msg);
static void ui_show_success_popup(const char *msg);

static void ecg_adc_init(void);
static int ecg_adc_read_raw(void);

static void ui_create_record_screen(void);
static void rec_update_topbar(void);
static void rec_update_plot(void);
static void rec_apply_active_tab(void);
static const char *rec_tab_name(rec_tab_t tab);
static void rec_build_chart(void);
static void rec_destroy_chart(void);

static void ui_create_bp_screen(void);
static void bp_build_chart(void);
static void bp_destroy_chart(void);
static void bp_startstop_btn_cb(lv_event_t *e);
static void bp_dur_btn_cb(lv_event_t *e);
static void bp_ui_timer_cb(lv_timer_t *timer);
static void bp_sampler_task(void *arg);
static void bp_analysis_task(void *arg);
static void bp_trigger_analysis(void);

/* -------------------------------------------------------------------------- */
/* ECG ADC helpers                                                            */
/* -------------------------------------------------------------------------- */
/*
 * IMPORTANT — ECG source switched to firmware simulation.
 *
 * Why this change is required (not optional):
 *   On the TZT ESP32-2432S024C, GPIO14 is the display SPI clock (PIN_LCD_SCLK)
 *   and must never be used as ADC. ADC2 is also blocked during WiFi on the
 *   classic ESP32. ADC1 channels GPIO32–39 are available but no analog ECG
 *   front-end is wired to the board yet. Until an external SPI/I²C ADC
 *   (e.g. ADS1292, MAX86140) is connected, the safest and most informative
 *   ECG source is a deterministic waveform generated in firmware. The waveform shape and amplitude are
 *   compatible with the existing Pan-Tompkins QRS detector, autoscale,
 *   and chart pipeline — those code paths are unchanged.
 *
 * Public surface preserved:
 *   ecg_adc_init() and ecg_adc_read_raw() keep the same names and
 *   signatures. Nothing else in the codebase needs to change — the
 *   sampler, recorder, autoscale, beat detector, and chart all keep
 *   working byte-for-byte as before.
 *
 * Switching back to a real ADC later:
 *   When MAX86140 / ADS1292 (or any SPI/I²C ADC) is wired in, replace
 *   the body of ecg_adc_read_raw() to read from that driver. The init
 *   path and the rest of the pipeline do not need to change.
 */

#if ECG_USE_SIMULATED_SOURCE

static void ecg_adc_init(void)
{
    /* No real ADC — keep the handle NULL so any stray reference falls
     * back to s_ecg_last_raw without touching hardware. */
    s_ecg_adc_handle = NULL;
    s_ecg_last_raw   = 2048;

}

static int ecg_adc_read_raw(void)
{
    int raw = ecg_simulate_raw();
    s_ecg_last_raw = raw;
    return raw;
}

#else  /* ECG_USE_SIMULATED_SOURCE == 0 — legacy ADC2 path, do not use */

static void ecg_adc_init(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ECG_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t err = adc_oneshot_new_unit(&init_config, &s_ecg_adc_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE) {

        } else {
            ESP_LOGW(TAG, "ECG ADC unit init failed: %s", esp_err_to_name(err));
        }
    }

    if (!s_ecg_adc_handle) {
        ESP_LOGW(TAG, "ECG ADC handle unavailable");
        return;
    }

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ECG_ADC_BITWIDTH,
        .atten = ECG_ADC_ATTEN,
    };

    err = adc_oneshot_config_channel(s_ecg_adc_handle, ECG_ADC_CHANNEL, &config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ECG ADC channel config failed: %s", esp_err_to_name(err));
        return;
    }

}

static int ecg_adc_read_raw(void)
{
    if (!s_ecg_adc_handle) {
        return 0;
    }

    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_ecg_adc_handle, ECG_ADC_CHANNEL, &raw);
    if (err != ESP_OK) {
        return s_ecg_last_raw;
    }

    s_ecg_last_raw = raw;
    return raw;
}

#endif /* ECG_USE_SIMULATED_SOURCE */

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static void reset_activity(void)
{
    s_last_activity = esp_timer_get_time();
}

/* LVGL event wrapper: reset inactivity timer (used on keyboard VALUE_CHANGED). */
static void reset_activity_cb(lv_event_t *e) { (void)e; reset_activity(); }

static void screen_sleep(void)
{
    s_screen_on = false;
    hal_backlight_off();
}

static void screen_wake(void)
{
    s_screen_on = true;
    hal_backlight_set_percent(s_brightness);
    reset_activity();
}

/* -------------------------------------------------------------------------- */
/* Navigation helpers                                                         */
/* -------------------------------------------------------------------------- */

static void nav_to_home_locked(void *ctx)
{
    (void)ctx;
#if LVGL_VERSION_MAJOR >= 9
    lv_screen_load_anim(s_scr_home, LV_SCR_LOAD_ANIM_FADE_IN, 240, 0, false);
#else
    lv_scr_load_anim(s_scr_home, LV_SCR_LOAD_ANIM_FADE_IN, 240, 0, false);
#endif
}

static void nav_to_settings_locked(void *ctx)
{
    (void)ctx;
#if LVGL_VERSION_MAJOR >= 9
    lv_screen_load_anim(s_scr_settings, LV_SCR_LOAD_ANIM_FADE_IN, 240, 0, false);
#else
    lv_scr_load_anim(s_scr_settings, LV_SCR_LOAD_ANIM_FADE_IN, 240, 0, false);
#endif
}

static void nav_to_wifi_locked(void *ctx)
{
    (void)ctx;
#if LVGL_VERSION_MAJOR >= 9
    lv_screen_load_anim(s_scr_wifi, LV_SCR_LOAD_ANIM_FADE_IN, 240, 0, false);
#else
    lv_scr_load_anim(s_scr_wifi, LV_SCR_LOAD_ANIM_FADE_IN, 240, 0, false);
#endif
}

static void nav_to_files_locked(void *ctx)
{
    (void)ctx;
#if LVGL_VERSION_MAJOR >= 9
    lv_screen_load_anim(s_scr_files, LV_SCR_LOAD_ANIM_FADE_IN, 240, 0, false);
#else
    lv_scr_load_anim(s_scr_files, LV_SCR_LOAD_ANIM_FADE_IN, 240, 0, false);
#endif
}

static void nav_to_pass_locked(void *ctx)
{
    (void)ctx;
#if LVGL_VERSION_MAJOR >= 9
    lv_screen_load_anim(s_scr_pass, LV_SCR_LOAD_ANIM_FADE_IN, 240, 0, false);
#else
    lv_scr_load_anim(s_scr_pass, LV_SCR_LOAD_ANIM_FADE_IN, 240, 0, false);
#endif
}

static void nav_to_conn_locked(void *ctx)
{
    (void)ctx;
#if LVGL_VERSION_MAJOR >= 9
    lv_screen_load_anim(s_scr_conn, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
#else
    lv_scr_load_anim(s_scr_conn, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
#endif
}

static void app_back_btn_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();
    nav_to_home_locked(NULL);
}

static void add_back_button(lv_obj_t *scr)
{
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 90, 36);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -8, 8);
    style_button(btn, COLOUR_SURFACE2, COLOUR_SUBTEXT);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Back");
    lv_obj_set_style_text_color(lbl, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, app_back_btn_cb, LV_EVENT_CLICKED, NULL);
}

/* -------------------------------------------------------------------------- */
/* Popup                                                                      */
/* -------------------------------------------------------------------------- */

static void popup_close_timer_cb(lv_timer_t *timer)
{
    lv_obj_t *obj = (lv_obj_t *)lv_timer_get_user_data(timer);
    if (obj) {
        lv_obj_del(obj);
    }
    if (obj == s_popup) {
        s_popup = NULL;
    }
    lv_timer_del(timer);
}

static void show_popup_internal(const char *title, const char *msg, lv_color_t accent)
{
    if (s_popup) {
        lv_obj_del(s_popup);
        s_popup = NULL;
    }

    lv_obj_t *parent =
#if LVGL_VERSION_MAJOR >= 9
        lv_screen_active();
#else
        lv_scr_act();
#endif

    s_popup = lv_obj_create(parent);
    lv_obj_remove_style_all(s_popup);
    lv_obj_set_size(s_popup, LCD_H_RES, LCD_V_RES);
    lv_obj_center(s_popup);
    lv_obj_set_style_bg_color(s_popup, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_popup, LV_OPA_50, LV_PART_MAIN);

    lv_obj_t *panel = lv_obj_create(s_popup);
    lv_obj_set_size(panel, 260, 130);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, COLOUR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, accent, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 14, LV_PART_MAIN);

    lv_obj_t *ttl = lv_label_create(panel);
    lv_label_set_text(ttl, title);
    lv_obj_set_style_text_color(ttl, accent, LV_PART_MAIN);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_t *body = lv_label_create(panel);
    lv_label_set_text(body, msg);
    lv_obj_set_width(body, 175);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(body, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 14);

    lv_timer_create(popup_close_timer_cb, 1800, s_popup);
}

static void popup_error_locked(void *ctx)
{
    const char *msg = (const char *)ctx;
    show_popup_internal("Error", msg, COLOUR_ERROR);
}

static void popup_success_locked(void *ctx)
{
    const char *msg = (const char *)ctx;
    show_popup_internal("Done", msg, COLOUR_SUCCESS);
}

static void ui_show_error_popup(const char *msg)
{
    hal_display_lock(DISPLAY_LOCK_UI_TIMEOUT_MS, "ui_show_error_popup", popup_error_locked, (void *)msg);
}

static void ui_show_success_popup(const char *msg)
{
    hal_display_lock(DISPLAY_LOCK_UI_TIMEOUT_MS, "ui_show_success_popup", popup_success_locked, (void *)msg);
}

/* -------------------------------------------------------------------------- */
/* Time + battery                                                             */
/* -------------------------------------------------------------------------- */

static void update_home_time_labels(void)
{
    int batt = battery_read_percent();
    s_cached_batt_pct = batt;   /* refresh cache for topbar and CSV path */
    if (s_lbl_battery) {
        char batt_buf[16];
        if (batt >= 0) {
            snprintf(batt_buf, sizeof(batt_buf), "%d%%", batt);
        } else {
            snprintf(batt_buf, sizeof(batt_buf), "--%%");
        }
        lv_label_set_text(s_lbl_battery, batt_buf);
    }

    time_t now = time(NULL);

    if (now < 100000) {
        safe_set_label(s_lbl_time, "--:--");
        safe_set_label(s_lbl_date, "Time not synced");
        safe_set_label(s_lbl_time_hint, "Connect WiFi in Settings");
        return;
    }

    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char time_buf[16];
    char date_buf[32];
    strftime(time_buf, sizeof(time_buf), "%H:%M", &tm_now);
    strftime(date_buf, sizeof(date_buf), "%a %d %b", &tm_now);

    safe_set_label(s_lbl_time, time_buf);
    safe_set_label(s_lbl_date, date_buf);
    safe_set_label(s_lbl_time_hint, svc_time_is_synced() ? "Time synced" : "Time may be stale");
}

static void rec_update_topbar(void)
{
    char buf[40];

    if (s_lbl_rec_rr) {
        int rr_bpm;
        portENTER_CRITICAL(&s_ecg_spinlock);
        rr_bpm = s_resp_rate_bpm;
        portEXIT_CRITICAL(&s_ecg_spinlock);
        snprintf(buf, sizeof(buf), rr_bpm > 0 ? "RR %d bpm" : "RR -- bpm", rr_bpm);
        lv_label_set_text(s_lbl_rec_rr, buf);
    }
    if (s_lbl_rec_hr) {
        int bpm;
        portENTER_CRITICAL(&s_ecg_spinlock);
        bpm = s_ecg_hr_bpm;
        portEXIT_CRITICAL(&s_ecg_spinlock);
        snprintf(buf, sizeof(buf), bpm > 0 ? "HR %d" : "HR --", bpm);
        lv_label_set_text(s_lbl_rec_hr, buf);
    }
    if (s_lbl_rec_spo2)
        lv_label_set_text(s_lbl_rec_spo2, "SpO2 --%");
    if (s_lbl_rec_pat) {
        int32_t pat;
        portENTER_CRITICAL(&s_ecg_spinlock);
        pat = ppg_det_get_pat_avg_ms();
        portEXIT_CRITICAL(&s_ecg_spinlock);
        if (pat > 0)
            snprintf(buf, sizeof(buf), "PAT %ldms", (long)pat);
        else
            snprintf(buf, sizeof(buf), "PAT --");
        lv_label_set_text(s_lbl_rec_pat, buf);
    }
    if (s_lbl_rec_batt) {
        int batt = s_cached_batt_pct;
        snprintf(buf, sizeof(buf), batt >= 0 ? "BAT %d%%" : "BAT --%%", batt);
        lv_label_set_text(s_lbl_rec_batt, buf);
    }
    if (s_lbl_rec_sd) {
        char drift_buf[32];
        int32_t drift;
        portENTER_CRITICAL(&s_ecg_spinlock);
        drift = s_ecg_sample_drift_ms;
        portEXIT_CRITICAL(&s_ecg_spinlock);
        format_drift_text(drift_buf, sizeof(drift_buf), drift);
        lv_label_set_text(s_lbl_rec_sd, drift_buf);
    }
}

/* alias keeps existing clock_update_task call compiling unchanged */
static inline void health_update_topbar(void) { rec_update_topbar(); }

static void update_time_locked(void *ctx)
{
    (void)ctx;
    update_home_time_labels();
    health_update_topbar();
}

static void clock_update_task(void *arg)
{
    (void)arg;

    while (1) {
        if (s_screen_on) {
            hal_display_lock(DISPLAY_LOCK_SHORT_MS, "clock_update_task", update_time_locked, NULL);

            int64_t idle_us = esp_timer_get_time() - s_last_activity;
            if (s_timeout_s > 0 && idle_us > ((int64_t)s_timeout_s * 1000000LL)) {
                screen_sleep();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* -------------------------------------------------------------------------- */
/* BOOT side button                                                           */
/* -------------------------------------------------------------------------- */

static void boot_btn_task(void *arg)
{
    (void)arg;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOOT_BTN_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    while (1) {
        if (gpio_get_level(BOOT_BTN_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (gpio_get_level(BOOT_BTN_GPIO) != 0) {
                continue;
            }

            int64_t t0 = esp_timer_get_time() / 1000;
            while (gpio_get_level(BOOT_BTN_GPIO) == 0 &&
                   (esp_timer_get_time() / 1000 - t0) < 2000) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            int64_t dur = esp_timer_get_time() / 1000 - t0;

            if (dur <= BOOT_SHORT_PRESS_MAX_MS) {
                if (!s_screen_on) {
                    screen_wake();
                } else if (s_scr_home) {
                    reset_activity();
                    hal_display_lock(DISPLAY_LOCK_SHORT_MS, "boot_btn_task", nav_to_home_locked, NULL);
                }
            }

            vTaskDelay(pdMS_TO_TICKS(200));
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* -------------------------------------------------------------------------- */
/* WiFi events                                                                */
/* -------------------------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "WiFi disconnected, reason=%d", disc->reason);
            s_wifi_connected = false;

            if (s_wifi_shutdown_requested) {

                xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
                return;
            }

            if (!s_wifi_connecting) {

                xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
                return;
            }

            s_retry_count++;
            if (s_retry_count < 3) {
                esp_err_t err = esp_wifi_connect();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "esp_wifi_connect retry failed: %s", esp_err_to_name(err));
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
            } else {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        } else if (event_id == WIFI_EVENT_SCAN_DONE) {

        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;

        s_retry_count = 0;
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_driver_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        abort();
    }
}

static esp_err_t wifi_ensure_started(void)
{
    if (s_wifi_started) {
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK || err == ESP_ERR_WIFI_CONN) {
        s_wifi_started = true;

        return ESP_OK;
    }

    if (err == ESP_ERR_WIFI_STATE) {
        s_wifi_started = true;

        return ESP_OK;
    }

    ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
    return err;
}

static void wifi_shutdown_after_time_sync(void)
{
    if (!s_wifi_started) {
        s_wifi_shutdown_requested = false;
        s_wifi_connecting = false;
        s_wifi_connected = false;
        return;
    }

    s_wifi_shutdown_requested = true;
    s_wifi_connecting = false;

    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED &&
        err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_CONNECT &&
        err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_disconnect failed during shutdown: %s", esp_err_to_name(err));
    }

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_stop failed during shutdown: %s", esp_err_to_name(err));
        s_wifi_shutdown_requested = false;
        return;
    }

    s_wifi_started = false;
    s_wifi_connected = false;
    s_retry_count = 0;
    s_wifi_shutdown_requested = false;

}

/* -------------------------------------------------------------------------- */
/* WiFi screen                                                                */
/* -------------------------------------------------------------------------- */

static void clear_wifi_container(void)
{
    if (!s_wifi_container) return;

    while (lv_obj_get_child_cnt(s_wifi_container) > 0) {
        lv_obj_t *child = lv_obj_get_child(s_wifi_container, 0);
        void *ud = lv_obj_get_user_data(child);
        if (ud) {
            free(ud);
        }
        lv_obj_del(child);
    }
}

static const char *auth_to_text(wifi_auth_mode_t auth)
{
    switch (auth) {
        case WIFI_AUTH_OPEN: return "Open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Ent";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "Secured";
    }
}

static void wifi_ap_btn_cb(lv_event_t *e)
{
    reset_activity();

    const char *ssid = (const char *)lv_event_get_user_data(e);
    if (!ssid || ssid[0] == '\0') {
        ui_show_error_popup("Invalid SSID");
        return;
    }

    strlcpy(s_selected_ssid, ssid, sizeof(s_selected_ssid));
    s_password[0] = '\0';

    if (nvs_load_password_for_ssid(s_selected_ssid, s_password, sizeof(s_password))) {

    }

    if (s_lbl_ssid) {
        lv_label_set_text(s_lbl_ssid, s_selected_ssid);
    }
    if (s_ta_pass) {
        lv_textarea_set_text(s_ta_pass, s_password);
    }
    if (s_lbl_saved_hint) {
        if (s_password[0] != '\0') {
            lv_label_set_text(s_lbl_saved_hint, "Saved password loaded");
            lv_obj_set_style_text_color(s_lbl_saved_hint, COLOUR_SUCCESS, LV_PART_MAIN);
        } else {
            lv_label_set_text(s_lbl_saved_hint, "Enter password");
            lv_obj_set_style_text_color(s_lbl_saved_hint, COLOUR_SUBTEXT, LV_PART_MAIN);
        }
    }

    nav_to_pass_locked(NULL);
}

static void wifi_button_list_populate(void)
{
    if (!s_wifi_container) return;

    clear_wifi_container();

    if (s_ap_count == 0) {
        lv_obj_t *lbl = lv_label_create(s_wifi_container);
        lv_label_set_text(lbl, "No WiFi networks found");
        lv_obj_set_style_text_color(lbl, COLOUR_SUBTEXT, LV_PART_MAIN);
        lv_obj_set_width(lbl, 320);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        return;
    }

    for (uint16_t i = 0; i < s_ap_count; i++) {
        const char *ssid = (const char *)s_ap_records[i].ssid;
        if (!ssid || ssid[0] == '\0') {
            continue;
        }

        lv_obj_t *btn = lv_btn_create(s_wifi_container);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, LV_SIZE_CONTENT);
        style_button(btn, COLOUR_SURFACE2, COLOUR_SUBTEXT);
        lv_obj_set_style_pad_all(btn, 12, LV_PART_MAIN);

        char *ssid_copy = strdup(ssid);
        if (!ssid_copy) {
            continue;
        }
        lv_obj_set_user_data(btn, ssid_copy);
        lv_obj_add_event_cb(btn, wifi_ap_btn_cb, LV_EVENT_CLICKED, ssid_copy);

        lv_obj_t *lbl_name = lv_label_create(btn);
        lv_label_set_text(lbl_name, ssid);
        lv_obj_set_style_text_color(lbl_name, COLOUR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_18, LV_PART_MAIN);
        lv_obj_align(lbl_name, LV_ALIGN_TOP_LEFT, 0, 0);

        char meta[96];
        snprintf(meta, sizeof(meta), "RSSI %d dBm   %s",
                 s_ap_records[i].rssi, auth_to_text(s_ap_records[i].authmode));

        lv_obj_t *lbl_meta = lv_label_create(btn);
        lv_label_set_text(lbl_meta, meta);
        lv_obj_set_style_text_color(lbl_meta, COLOUR_SUBTEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl_meta, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align_to(lbl_meta, lbl_name, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    }
}

static void wifi_status_scanning_locked(void *ctx)
{
    (void)ctx;
    safe_set_label(s_wifi_status, "Scanning...");
}

static void wifi_status_done_locked(void *ctx)
{
    (void)ctx;
    safe_set_label(s_wifi_status, "Tap a network to connect");
    wifi_button_list_populate();
}

static void wifi_status_error_locked(void *ctx)
{
    const char *msg = (const char *)ctx;
    safe_set_label(s_wifi_status, msg ? msg : "Scan failed");
}

static void wifi_scan_task(void *arg)
{
    (void)arg;

    reset_activity();
    hal_display_lock(DISPLAY_LOCK_SHORT_MS, "wifi_scan_task(scanning)", wifi_status_scanning_locked, NULL);

    s_wifi_shutdown_requested = false;
    s_wifi_connecting = true;
    s_wifi_connected = false;

    if (wifi_ensure_started() != ESP_OK) {
        hal_display_lock(DISPLAY_LOCK_SHORT_MS, "wifi_scan_task(startfail)", wifi_status_error_locked, "WiFi start failed");
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(700));

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        hal_display_lock(DISPLAY_LOCK_SHORT_MS, "wifi_scan_task(scanfail)", wifi_status_error_locked, "Scan failed");
        vTaskDelete(NULL);
        return;
    }

    uint16_t count = MAX_SCAN_RESULTS;
    err = esp_wifi_scan_get_ap_records(&count, s_ap_records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(err));
        hal_display_lock(DISPLAY_LOCK_SHORT_MS, "wifi_scan_task(getfail)", wifi_status_error_locked, "Could not read results");
        vTaskDelete(NULL);
        return;
    }

    s_ap_count = count;

    hal_display_lock(DISPLAY_LOCK_UI_TIMEOUT_MS, "wifi_scan_task(done)", wifi_status_done_locked, NULL);
    vTaskDelete(NULL);
}

static void wifi_main_btn_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();
    if (s_scr_home) nav_to_home_locked(NULL);
}

static void wifi_back_btn_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();

    if (s_wifi_opened_from_files) {
        s_wifi_opened_from_files = false;
        nav_to_files_locked(NULL);
    } else if (s_wifi_opened_from_settings) {
        s_wifi_opened_from_settings = false;
        nav_to_settings_locked(NULL);
    } else if (s_scr_home) {
        nav_to_home_locked(NULL);
    }
}

static void wifi_rescan_btn_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();
    xTaskCreate(wifi_scan_task, "wifi_scan", 6144, NULL, 5, NULL);
}

static void settings_wifi_btn_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();
    s_wifi_opened_from_settings = true;
    nav_to_wifi_locked(NULL);
    xTaskCreate(wifi_scan_task, "wifi_scan", 6144, NULL, 5, NULL);
}

/* -------------------------------------------------------------------------- */
/* Password / connect                                                         */
/* -------------------------------------------------------------------------- */

static void pass_back_btn_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();
    nav_to_wifi_locked(NULL);
}

static void pass_connect_btn_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();

    if (s_ta_pass) {
        const char *pw = lv_textarea_get_text(s_ta_pass);
        strlcpy(s_password, pw ? pw : "", sizeof(s_password));
    }

    nav_to_conn_locked(NULL);
    xTaskCreate(wifi_connect_task, "wifi_connect", 8192, NULL, 5, NULL);
}

static void pass_kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        if (s_keyboard) {
            lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void pass_ta_event_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();
    if (s_keyboard) {
        lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_keyboard, s_ta_pass);
    }
}

static void conn_status_locked(void *ctx)
{
    const char *msg = (const char *)ctx;
    lv_obj_t *scr =
#if LVGL_VERSION_MAJOR >= 9
        lv_screen_active();
#else
        lv_scr_act();
#endif

    lv_obj_t *lbl = lv_obj_get_child(scr, 1);
    if (lbl) {
        lv_label_set_text(lbl, msg);
    }
}

static void wifi_connect_task(void *arg)
{
    (void)arg;

    if (wifi_ensure_started() != ESP_OK) {
        s_wifi_connecting = false;
        ui_show_error_popup("WiFi start failed");
        hal_display_lock(DISPLAY_LOCK_SHORT_MS, "wifi_connect_task(back1)", nav_to_wifi_locked, NULL);
        vTaskDelete(NULL);
        return;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, s_selected_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, s_password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "esp_wifi_disconnect: %s", esp_err_to_name(err));
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        s_wifi_connecting = false;
        ui_show_error_popup("Config failed");
        hal_display_lock(DISPLAY_LOCK_SHORT_MS, "wifi_connect_task(back2)", nav_to_wifi_locked, NULL);
        vTaskDelete(NULL);
        return;
    }

    s_retry_count = 0;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        s_wifi_connecting = false;
        ui_show_error_popup("Connect start failed");
        hal_display_lock(DISPLAY_LOCK_SHORT_MS, "wifi_connect_task(back3)", nav_to_wifi_locked, NULL);
        vTaskDelete(NULL);
        return;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_S * 1000)
    );

    if (bits & WIFI_CONNECTED_BIT) {
        nvs_save_password_for_ssid(s_selected_ssid, s_password);
        nvs_save_last_ssid(s_selected_ssid);

        hal_display_lock(DISPLAY_LOCK_SHORT_MS, "wifi_connect_task(status)", conn_status_locked, "Connected. Syncing time...");
        svc_time_sync();

        /* Keep Wi-Fi alive when opened from the Files screen — the user
         * needs the connection to upload files.  Only shut down for the
         * normal time-sync flow (Settings → WiFi). */
        if (svc_time_is_synced() && !s_wifi_opened_from_files) {
            wifi_shutdown_after_time_sync();
        } else if (s_wifi_opened_from_files) {
            s_files_wifi_kept_alive = true;
        }

        s_wifi_connecting = false;
        hal_display_lock(DISPLAY_LOCK_SHORT_MS, "wifi_connect_task(time)", update_time_locked, NULL);
        ui_show_success_popup("WiFi connected");
        vTaskDelay(pdMS_TO_TICKS(1200));

        if (s_wifi_opened_from_files) {
            s_wifi_opened_from_files = false;
            hal_display_lock(DISPLAY_LOCK_SHORT_MS, "wifi_connect_task(files)", nav_to_files_locked, NULL);
        } else if (s_scr_home) {
            hal_display_lock(DISPLAY_LOCK_SHORT_MS, "wifi_connect_task(home)", nav_to_home_locked, NULL);
        }
    } else {
        s_wifi_connecting = false;
        ui_show_error_popup("WiFi connect failed");
        vTaskDelay(pdMS_TO_TICKS(1000));
        hal_display_lock(DISPLAY_LOCK_SHORT_MS, "wifi_connect_task(back4)", nav_to_wifi_locked, NULL);
    }

    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/* ECG sampling                                                               */
/* -------------------------------------------------------------------------- */

static int resp_rate_from_intersections_locked(void)
{
    const uint32_t needed = RESP_RATE_WINDOW_SECONDS * ECG_SAMPLE_HZ;
    if (s_resp_hist_count < needed) {
        return 0;
    }

    int delay = s_resp_dup_delay_samples;
    if (delay < 1) delay = 1;
    if ((uint32_t)delay >= needed) delay = (int)needed - 1;

    const uint32_t valid_len = needed - (uint32_t)delay;
    if (valid_len < 2) {
        return 0;
    }

    int intersections = 0;
    bool prev_real_gt_delayed = false;
    bool prev_state_valid = false;

    for (uint32_t i = 0; i < valid_len; i++) {
        uint32_t idx = (s_resp_hist_index + i) % needed;
        uint32_t didx = (s_resp_hist_index + i + delay) % needed;

        float real_val = (float)s_resp_history[idx];
        float delayed_val = (float)s_resp_history[didx] + (float)s_resp_dup_elevation;

        if (real_val == delayed_val) {
            continue;
        }

        bool real_gt_delayed = (real_val > delayed_val);
        if (prev_state_valid && prev_real_gt_delayed && !real_gt_delayed) {
            intersections++;
        }

        prev_real_gt_delayed = real_gt_delayed;
        prev_state_valid = true;
    }

    int bpm = intersections * 3;
    if (bpm < RESP_MIN_BPM || bpm > RESP_MAX_BPM) {
        return 0;
    }
    return bpm;
}

static void ecg_sampler_task(void *arg)
{
    (void)arg;

    const int32_t period_us = 1000000 / ECG_SAMPLE_HZ;
    const uint32_t mwi_len = (ECG_MWI_SAMPLES < 1) ? 1 : ECG_MWI_SAMPLES;
    const uint32_t resp_hist_len = RESP_RATE_WINDOW_SECONDS * ECG_SAMPLE_HZ;

    int64_t start_us = esp_timer_get_time();
    int64_t next_us = start_us;
    s_ecg_task_start_ms = (uint32_t)(start_us / 1000);

    memset(s_ecg_mwi_buf, 0, sizeof(s_ecg_mwi_buf));
    memset(s_resp_history, 0, sizeof(s_resp_history));
    s_ecg_hp_state = 0.0f;
    s_ecg_lp_state = 0.0f;
    s_ecg_prev_input = 0.0f;
    s_ecg_prev_band = 0.0f;
    s_ppg_hp_state   = 0.0f;
    s_ppg_lp_state   = 0.0f;
    s_ppg_prev_input = 0.0f;
    s_ecg_mwi_sum = 0.0f;
    s_ecg_mwi_index = 0;
    s_ecg_signal_level = 200.0f;
    s_ecg_noise_level = 50.0f;
    s_ecg_detect_threshold = 120.0f;
    s_ecg_last_peak_ms       = -10000;
    s_ecg_last_rpeak_expected = -1;
    s_ecg_pending_beat        = false;
    s_ecg_beat_detect_idx     = 0;
    s_ecg_beat_detect_sample  = 0;
    s_ecg_hr_bpm = 0;
    s_rec_stagger      = 0;
    s_rec_pending_hr   = 0;
    s_rec_pending_resp = 0;
    s_cached_batt_pct  = -1;
    s_resp_hist_index = 0;
    s_resp_hist_count = 0;
    s_resp_rate_bpm = 0;

    int running_min = INT32_MAX;   /* overridden immediately by first sample */
    int running_max = 0;
    int32_t running_ppg_min = INT32_MAX;
    int32_t running_ppg_max = INT32_MIN;

    while (1) {
        if (!s_ecg_sampling_enabled) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us < next_us) {
            int32_t sleep_us = (int32_t)(next_us - now_us);
            if (sleep_us >= 1000) {
                vTaskDelay(pdMS_TO_TICKS(sleep_us / 1000));
            } else if (sleep_us > 50) {
                esp_rom_delay_us((uint32_t)sleep_us);
            }
            now_us = esp_timer_get_time();
        }

        uint32_t actual_ms = (uint32_t)((now_us - start_us) / 1000);
        int raw = ecg_adc_read_raw();

        float band = signal_bandpass_step((float)raw,
                                          (float)ECG_SAMPLE_HZ,
                                          ECG_QRS_BP_LOW_HZ,
                                          ECG_QRS_BP_HIGH_HZ,
                                          &s_ecg_hp_state,
                                          &s_ecg_lp_state,
                                          &s_ecg_prev_input);

        float diff = band - s_ecg_prev_band;
        s_ecg_prev_band = band;
        float squared = diff * diff;

        float old_mwi = s_ecg_mwi_buf[s_ecg_mwi_index];
        s_ecg_mwi_sum -= old_mwi;
        s_ecg_mwi_buf[s_ecg_mwi_index] = squared;
        s_ecg_mwi_sum += squared;
        s_ecg_mwi_index = (s_ecg_mwi_index + 1) % mwi_len;

        float integrated = s_ecg_mwi_sum / (float)mwi_len;

        float phase = 2.0f * (float)M_PI * (float)s_sim_phase / (float)REC_SAMPLE_HZ;
        int16_t resp_sample = (int16_t)(REC_SIM_CENTRE + REC_SIM_AMP * sinf(phase * 0.3f));
        int16_t  rec_rr_ms     = 0;  /* RR interval this sample; non-zero only at beat detection */
        int16_t  rec_pat_ms    = 0;  /* instantaneous PAT this sample; non-zero only at beat detection */
        uint32_t rec_r_peak_ms = 0;  /* expected_ms of the R-peak; non-zero only at beat detection */

        portENTER_CRITICAL(&s_ecg_spinlock);

        uint32_t sample_number = s_ecg_total_samples;
        uint32_t expected_ms = sample_number * ECG_SAMPLE_PERIOD_MS;
        uint32_t index = s_ecg_write_index;

        s_ecg_raw[index]  = (int32_t)raw;
        s_ecg_band[index] = band;   /* store bandpassed ECG for R-peak backward search */

        int16_t ppg_raw = ppg_sim_get_sample(expected_ms);
        float ppg_filt = signal_bandpass_step((float)ppg_raw,
                                              (float)ECG_SAMPLE_HZ,
                                              PPG_BP_LOW_HZ,
                                              PPG_BP_HIGH_HZ,
                                              &s_ppg_hp_state,
                                              &s_ppg_lp_state,
                                              &s_ppg_prev_input);
        int32_t ppg_sample = (int32_t)ppg_filt;
        s_ppg_raw[index] = ppg_sample;
        ppg_det_update_sample(ppg_sample, expected_ms);
        s_ecg_expected_ms[index] = expected_ms;
        s_ecg_actual_ms[index] = actual_ms;
        s_ecg_sample_drift_ms = (int32_t)actual_ms - (int32_t)expected_ms;

        if (raw < running_min) running_min = raw;
        if (raw > running_max) running_max = raw;
        s_ecg_min = running_min;
        s_ecg_max = running_max;
        if (ppg_sample < running_ppg_min) running_ppg_min = ppg_sample;
        if (ppg_sample > running_ppg_max) running_ppg_max = ppg_sample;
        s_ppg_min = running_ppg_min;
        s_ppg_max = running_ppg_max;

        if (s_ecg_total_samples > mwi_len + 2) {
            bool local_peak = false;
            float prev_int = s_ecg_mwi_buf[(s_ecg_mwi_index + mwi_len - 2) % mwi_len];
            float prev2_int = s_ecg_mwi_buf[(s_ecg_mwi_index + mwi_len - 3) % mwi_len];

            if (prev_int >= integrated && prev_int >= prev2_int) {
                local_peak = true;
            }

            float threshold = s_ecg_noise_level +
                              0.25f * (s_ecg_signal_level - s_ecg_noise_level);
            if (threshold < ECG_MIN_THRESHOLD_FLOOR) {
                threshold = ECG_MIN_THRESHOLD_FLOOR;
            }
            s_ecg_detect_threshold = threshold;
            s_ecg_threshold = (int)threshold;

            if (local_peak && prev_int > threshold &&
                (int32_t)expected_ms - s_ecg_last_peak_ms > ECG_REFACTORY_MS) {

                s_ecg_signal_level = 0.875f * s_ecg_signal_level + 0.125f * prev_int;

                /* Notify PPG simulator immediately so the waveform generation
                 * stays synchronised with the detected beat (simulation only). */
                uint32_t mwi_rr = (s_ecg_last_peak_ms > 0)
                    ? (expected_ms - (uint32_t)s_ecg_last_peak_ms) : 0;
                ppg_sim_on_beat(expected_ms, mwi_rr);

                /* Arm the deferred R-peak search. The true peak is found
                 * ECG_RPEAK_FORWARD_SAMPLES ticks later once it is in the
                 * ring buffer. r_peak_ms/rr_ms/pat_ms are written to CSV then. */
                s_ecg_pending_beat        = true;
                s_ecg_beat_detect_idx     = index;
                s_ecg_beat_detect_sample  = sample_number;

                s_ecg_last_peak_ms = (int32_t)expected_ms; /* refractory from MWI time */
            } else {
                s_ecg_noise_level = 0.875f * s_ecg_noise_level + 0.125f * integrated;
            }
        }

        /* Deferred R-peak search: runs ECG_RPEAK_FORWARD_SAMPLES ticks after the
         * MWI fires, so the true R-peak is guaranteed to be in the ring buffer
         * even when detection fires early (e.g., on P-wave energy). */
        if (s_ecg_pending_beat &&
            (sample_number - s_ecg_beat_detect_sample) >= ECG_RPEAK_FORWARD_SAMPLES) {

            uint32_t rpeak_idx  = s_ecg_beat_detect_idx;
            float    rpeak_band = s_ecg_band[s_ecg_beat_detect_idx];

            for (int k = -(int)ECG_RPEAK_BACKWARD_SAMPLES;
                 k <= (int)ECG_RPEAK_FORWARD_SAMPLES; k++) {
                /* Adding ECG_WINDOW_SAMPLES before modulo keeps the index positive
                 * when k is negative (backward search). */
                uint32_t idx = (s_ecg_beat_detect_idx
                                + ECG_WINDOW_SAMPLES + (uint32_t)k)
                               % ECG_WINDOW_SAMPLES;
                if (s_ecg_band[idx] > rpeak_band) {
                    rpeak_band = s_ecg_band[idx];
                    rpeak_idx  = idx;
                }
            }

            uint32_t r_peak_expected = s_ecg_expected_ms[rpeak_idx];

            /* Refined beat-to-beat RR and HR from true R-peak timestamps */
            if (s_ecg_last_rpeak_expected > 0) {
                int32_t rr = (int32_t)r_peak_expected - s_ecg_last_rpeak_expected;
                if (rr > 0) {
                    int bpm = 60000 / rr;
                    if (bpm >= ECG_MIN_BPM && bpm <= ECG_MAX_BPM) {
                        s_ecg_hr_bpm = (s_ecg_hr_bpm == 0) ? bpm
                                       : (s_ecg_hr_bpm * 3 + bpm) / 4;
                        rec_rr_ms = (int16_t)rr;
                    }
                }
            }
            s_ecg_last_rpeak_expected = (int32_t)r_peak_expected;

            int32_t last_pat = ppg_det_get_pat_last_ms();
            rec_pat_ms    = (last_pat > 0) ? (int16_t)last_pat : 0;

            /* r_peak_ms in recording-relative time (same epoch as time_ms) */
            rec_r_peak_ms = r_peak_expected + s_ecg_task_start_ms
                            - svc_rec_get_start_ms();

            /* Arm staggered writes: hr captured here (just updated above),
             * resp captured after resp_rate_from_intersections_locked() below. */
            s_rec_stagger    = 2;
            s_rec_pending_hr = (uint8_t)((s_ecg_hr_bpm > 0 && s_ecg_hr_bpm < 255)
                                         ? s_ecg_hr_bpm : 0);
            s_ecg_pending_beat = false;
        }

        s_resp_history[s_resp_hist_index] = resp_sample;
        s_resp_hist_index = (s_resp_hist_index + 1) % resp_hist_len;
        if (s_resp_hist_count < resp_hist_len) {
            s_resp_hist_count++;
        }
        s_resp_rate_bpm = resp_rate_from_intersections_locked();
        if (s_rec_stagger == 2) {
            s_rec_pending_resp = (uint8_t)((s_resp_rate_bpm > 0 && s_resp_rate_bpm < 255)
                                           ? s_resp_rate_bpm : 0);
        }

        s_ecg_write_index = (s_ecg_write_index + 1) % ECG_WINDOW_SAMPLES;
        s_ecg_total_samples++;

        portEXIT_CRITICAL(&s_ecg_spinlock);

        if (svc_rec_is_recording()) {
            rec_row_t row;
            uint32_t elapsed = (uint32_t)(
                (esp_timer_get_time() - (int64_t)svc_rec_get_start_ms() * 1000LL) / 1000LL);
            row.time_ms   = elapsed;
            row.ecg       = (int32_t)raw;
            row.ppg       = ppg_sample;
            row.resp      = resp_sample;
            row.nas       = (int16_t)(REC_SIM_CENTRE + REC_SIM_AMP * sinf(phase + 0.5f));
            row.fcg1      = (int16_t)(REC_SIM_CENTRE + REC_SIM_AMP * sinf(phase * 2.0f));
            row.fcg2      = (int16_t)(REC_SIM_CENTRE + REC_SIM_AMP * sinf(phase * 2.0f + 0.3f));
            row.drift_ms  = s_ecg_sample_drift_ms;
            row.spo2      = 0;
            /* Staggered writes: spread hr and resp across 2 consecutive samples
             * after each beat to avoid clustering overhead in one sample period. */
            switch (s_rec_stagger) {
                case 2:
                    row.hr_bpm    = s_rec_pending_hr;
                    row.resp_rate = 0;
                    s_rec_stagger--;
                    break;
                case 1:
                    row.hr_bpm    = 0;
                    row.resp_rate = s_rec_pending_resp;
                    s_rec_stagger--;
                    break;
                default:
                    row.hr_bpm    = 0;
                    row.resp_rate = 0;
                    break;
            }
            /* Battery percent: written once every ~30 s from the cached value
             * updated at 1 Hz by clock_update_task; zero on all other rows. */
            if ((sample_number % REC_BATT_UPDATE_SAMPLES) == 0) {
                int pct = s_cached_batt_pct;
                row.batt_pct = (pct >= 0 && pct <= 100) ? (uint8_t)pct : 0;
            } else {
                row.batt_pct = 0;
            }
            row.rr_ms      = rec_rr_ms;
            row.pat_ms     = rec_pat_ms;
            row.r_peak_ms  = rec_r_peak_ms;
            svc_rec_enqueue(&row);
        }
        s_sim_phase++;
        next_us += period_us;

        /* Every 500 samples (~5 s) log ground-truth vs detected PAT for validation */
        if ((sample_number % 500) == 0) {
            int32_t pat_gt  = ppg_sim_get_pat_ms();
            int32_t pat_det = ppg_det_get_pat_avg_ms();
            if (pat_gt > 0 && pat_det > 0) {
                ESP_LOGI("PPG_DET", "GT=%ldms  DET=%ldms  err=%ldms",
                         (long)pat_gt, (long)pat_det, (long)(pat_det - pat_gt));
            }
        }

        if ((sample_number % (ECG_WINDOW_SAMPLES * 4)) == 0) {
            running_min     = raw;
            running_max     = raw;
            running_ppg_min = ppg_sample;
            running_ppg_max = ppg_sample;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* B.P. sampler task (1 kHz)                                                  */
/* -------------------------------------------------------------------------- */

static void bp_sampler_task(void *arg)
{
    (void)arg;
    int64_t  start_us   = esp_timer_get_time();
    int64_t  next_us    = start_us;
    uint32_t bp_start_ms = svc_bp_rec_get_start_ms();
    const int32_t period_us = BP_SAMPLE_PERIOD_US;   /* 1000 µs */
    uint32_t bp_sample_number = 0;

    /* Snapshot the current R-peak so we don't report it as a new beat on the
     * first tick (ecg_sampler_task may have already detected beats before BP
     * recording started). */
    int32_t prev_rpeak_expected;
    portENTER_CRITICAL(&s_ecg_spinlock);
    prev_rpeak_expected = s_ecg_last_rpeak_expected;
    portEXIT_CRITICAL(&s_ecg_spinlock);

    while (svc_bp_rec_is_recording()) {
        int64_t now_us = esp_timer_get_time();
        if (now_us < next_us) {
            vTaskDelay(1);   /* yield 1 tick (1 ms at HZ=1000) so IDLE1 can run */
            now_us = esp_timer_get_time();
        }

        /* Catchup loop: if we woke up behind schedule (missed ticks), fire up to
         * 4 samples back-to-back without re-yielding to fill the gap in the CSV.
         * Beat data (RR/PAT) is correctly zero for fill samples because
         * cur_rpeak == prev_rpeak_expected within the same heartbeat period. */
        int catchup_rem = 4;
        do {
            uint32_t expected_ms = bp_sample_number;
            uint32_t actual_ms   = (uint32_t)((now_us - start_us) / 1000LL);

            bp_row_t row = {
                .time_ms   = expected_ms,
                .drift_ms  = (int32_t)actual_ms - (int32_t)expected_ms,
                .ecg       = 0,
                .ppg       = 0,
                .r_peak_ms = 0,
                .rr_us     = 0,
                .pat_us    = 0,
            };

            portENTER_CRITICAL(&s_ecg_spinlock);
            row.ecg = s_ecg_raw[s_ecg_write_index];
            row.ppg = s_ppg_raw[s_ecg_write_index];
            int32_t cur_rpeak = s_ecg_last_rpeak_expected;
            if (cur_rpeak > 0 && cur_rpeak != prev_rpeak_expected) {
                if (prev_rpeak_expected > 0) {
                    int32_t rr = cur_rpeak - prev_rpeak_expected;
                    if (rr > 0 && rr < 2000)
                        row.rr_us = (int32_t)rr * 1000;   /* ms → µs */
                }
                int32_t pat = ppg_det_get_pat_last_ms();
                row.pat_us    = (pat > 0) ? (int32_t)pat * 1000 : 0;   /* ms → µs */
                /* Convert ECG-relative expected_ms to BP-recording-relative time */
                row.r_peak_ms = (uint32_t)((int32_t)cur_rpeak + (int32_t)s_ecg_task_start_ms
                                            - (int32_t)bp_start_ms);
                prev_rpeak_expected = cur_rpeak;
            }
            portEXIT_CRITICAL(&s_ecg_spinlock);

            svc_bp_rec_enqueue(&row);
            bp_sample_number++;
            next_us += period_us;
            now_us = esp_timer_get_time();
        } while (--catchup_rem > 0 && now_us >= next_us && svc_bp_rec_is_recording());
    }

    s_bp_sampler_task = NULL;
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/* Health UI                                                                  */
/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */
/* Record UI helpers                                                          */
/* -------------------------------------------------------------------------- */

static const char *rec_tab_name(rec_tab_t tab)
{
    switch (tab) {
        case REC_TAB_ECG:  return "ECG";
        case REC_TAB_PPG:  return "PPG";
        case REC_TAB_RESP: return "RESP";
        case REC_TAB_NAS:  return "NAS";
        case REC_TAB_FCG1: return "FCG1";
        case REC_TAB_FCG2: return "FCG2";
        default:           return "?";
    }
}

static lv_color_t rec_tab_colour(rec_tab_t tab)
{
    switch (tab) {
        case REC_TAB_ECG:  return COLOUR_ECG;
        case REC_TAB_PPG:  return COLOUR_PPG;
        case REC_TAB_RESP: return COLOUR_RESP;
        case REC_TAB_NAS:  return COLOUR_WARN;
        case REC_TAB_FCG1: return COLOUR_ACCENT;
        case REC_TAB_FCG2: return lv_color_hex(0xAD65FF);
        default:           return COLOUR_ACCENT;
    }
}

static void rec_fill_sim_plot(rec_tab_t tab)
{
    static const float freqs[REC_TAB_COUNT] = {
        1.0f, 1.0f, 0.3f, 1.0f, 1.0f, 1.0f
    };
    float freq_hz = (tab < REC_TAB_COUNT) ? freqs[tab] : 1.0f;

    uint32_t total_samples;
    portENTER_CRITICAL(&s_ecg_spinlock);
    total_samples = s_ecg_total_samples;
    portEXIT_CRITICAL(&s_ecg_spinlock);

    float phase_offset = 2.0f * (float)M_PI *
        ((float)(total_samples % ECG_WINDOW_SAMPLES) /
         (float)ECG_WINDOW_SAMPLES);

    for (int i = 0; i < ECG_WINDOW_SAMPLES; i++) {
        float t = (float)i / (float)ECG_SAMPLE_HZ;
        float y = REC_SIM_CENTRE + REC_SIM_AMP *
                  sinf(2.0f * (float)M_PI * freq_hz * t + phase_offset);

        if (tab == REC_TAB_PPG)
            y += 90.0f * sinf(2.0f*(float)M_PI*freq_hz*2.0f*t + phase_offset*1.2f);
        else if (tab == REC_TAB_NAS || tab == REC_TAB_FCG1)
            y += 50.0f * sinf(2.0f*(float)M_PI*freq_hz*3.0f*t + phase_offset*0.8f);
        else if (tab == REC_TAB_FCG2)
            y += 50.0f * sinf(2.0f*(float)M_PI*freq_hz*3.0f*t + phase_offset*0.9f);
        else if (tab == REC_TAB_RESP)
            y += 30.0f * sinf(2.0f*(float)M_PI*0.6f*t + phase_offset*0.5f);

        if (y < 0.0f)    y = 0.0f;
        if (y > 1000.0f) y = 1000.0f;
        s_rec_chart_points[i] = (lv_coord_t)y;
    }
}

static void rec_fill_ppg_plot(void)
{
    static int32_t ppg_copy[ECG_WINDOW_SAMPLES]; /* static: keeps 1600 bytes off LVGL stack */
    uint32_t write_index, total_samples;
    int32_t  ppg_min, ppg_max;

    portENTER_CRITICAL(&s_ecg_spinlock);
    memcpy(ppg_copy, s_ppg_raw, sizeof(ppg_copy));
    write_index   = s_ecg_write_index;
    total_samples = s_ecg_total_samples;
    ppg_min       = s_ppg_min;
    ppg_max       = s_ppg_max;
    portEXIT_CRITICAL(&s_ecg_spinlock);

    if (ppg_max <= ppg_min) { ppg_min = (int32_t)REC_SIM_CENTRE - 1; ppg_max = (int32_t)REC_SIM_CENTRE + 1; }

    const int chart_pts = REC_CHART_POINTS;
    const int stride = ECG_WINDOW_SAMPLES / chart_pts;
    for (int i = 0; i < chart_pts; i++) {
        int raw_offset = i * stride;
        uint32_t src = (write_index + (uint32_t)raw_offset) % ECG_WINDOW_SAMPLES;
        int y;
        if (total_samples < (uint32_t)ECG_WINDOW_SAMPLES &&
            raw_offset >= (int)total_samples) {
            y = 500;
        } else {
            y = (int)((int64_t)(ppg_copy[src] - ppg_min) * 1000
                      / (ppg_max - ppg_min + 1));
            if (y < 0)    y = 0;
            if (y > 1000) y = 1000;
        }
        s_rec_chart_points[i] = (lv_coord_t)y;
    }
}

static void rec_update_plot(void)
{
    /* Lazy chart construction: build the chart 800ms after navigating to
     * the Record screen. Creating it during ui_create_record_screen() (or
     * earlier than 800ms post-nav) causes LVGL to hang on the third render
     * cycle of the screen-load animation. By 800ms the animation is long
     * complete and the screen is stable, so adding the chart now triggers
     * only a partial repaint of the chart's bounding box — no full-screen
     * render is needed. */
    if (!s_rec_plot) {
        if (s_nav_rec_us == 0)
            return;
        if (esp_timer_get_time() - s_nav_rec_us < 800000LL)
            return;
        rec_build_chart();
        if (!s_rec_plot) return;  /* build failed for some reason */
        return;  /* let it render once before populating data */
    }
    if (!s_rec_series) return;

    if (s_active_rec_tab == REC_TAB_PPG) {
        rec_fill_ppg_plot();
        lv_chart_set_series_ext_y_array(s_rec_plot, s_rec_series,
                                         s_rec_chart_points);
        lv_chart_refresh(s_rec_plot);
        return;
    }
    if (s_active_rec_tab != REC_TAB_ECG) {
        rec_fill_sim_plot(s_active_rec_tab);
        lv_chart_set_series_ext_y_array(s_rec_plot, s_rec_series,
                                         s_rec_chart_points);
        lv_chart_refresh(s_rec_plot);
        return;
    }

    static int32_t raw_copy[ECG_WINDOW_SAMPLES]; /* static: keeps 1600 bytes off LVGL stack */
    uint32_t write_index, total_samples;
    int raw_min, raw_max;

    int64_t t0 = esp_timer_get_time();
    portENTER_CRITICAL(&s_ecg_spinlock);
    memcpy(raw_copy, s_ecg_raw, sizeof(raw_copy));
    write_index   = s_ecg_write_index;
    total_samples = s_ecg_total_samples;
    raw_min       = s_ecg_min;
    raw_max       = s_ecg_max;
    portEXIT_CRITICAL(&s_ecg_spinlock);
    int64_t t1 = esp_timer_get_time();

    if (raw_max <= raw_min) { raw_min = 0; raw_max = 4095; }  /* sim fallback; real ADC autoscales after 1st window */

    /* Subsample the 400 raw samples down to REC_CHART_POINTS chart slots.
     * Stride = ECG_WINDOW_SAMPLES / REC_CHART_POINTS. Each chart point i
     * shows raw sample (write_index + i*stride) in the circular buffer. */
    const int chart_pts = REC_CHART_POINTS;
    const int stride = ECG_WINDOW_SAMPLES / chart_pts;
    for (int i = 0; i < chart_pts; i++) {
        int raw_offset = i * stride;
        uint32_t src = (write_index + (uint32_t)raw_offset) % ECG_WINDOW_SAMPLES;
        int y;
        if (total_samples < (uint32_t)ECG_WINDOW_SAMPLES &&
            raw_offset >= (int)total_samples) {
            y = 500;
        } else {
            y = (int)((int64_t)(raw_copy[src] - raw_min) * 1000
                      / (raw_max - raw_min + 1));
            if (y < 0)    y = 0;
            if (y > 1000) y = 1000;
        }
        s_rec_chart_points[i] = (lv_coord_t)y;
    }
    int64_t t2 = esp_timer_get_time();

    lv_chart_set_series_ext_y_array(s_rec_plot, s_rec_series, s_rec_chart_points);
    int64_t t3 = esp_timer_get_time();
    lv_chart_refresh(s_rec_plot);
    int64_t t4 = esp_timer_get_time();

    static uint32_t s_plot_cnt = 0;
    s_plot_cnt++;
    if (s_plot_cnt % 200 == 1 || (t4 - t0) > 5000) {
        ESP_LOGI(TAG, "plot#%lu lock=%lld fill=%lld arr=%lld ref=%lld us",
                 (unsigned long)s_plot_cnt, (long long)(t1 - t0),
                 (long long)(t2 - t1), (long long)(t3 - t2),
                 (long long)(t4 - t3));
    }
}

static void rec_apply_active_tab(void)
{
    if (s_lbl_rec_tab_name) {
        lv_label_set_text(s_lbl_rec_tab_name, rec_tab_name(s_active_rec_tab));
        lv_obj_set_style_text_color(s_lbl_rec_tab_name,
                                     rec_tab_colour(s_active_rec_tab),
                                     LV_PART_MAIN);
    }
    if (s_rec_series) {
#if LVGL_VERSION_MAJOR >= 9
        lv_chart_set_series_color(s_rec_plot, s_rec_series,
                                   rec_tab_colour(s_active_rec_tab));
#else
        s_rec_series->color = rec_tab_colour(s_active_rec_tab);
        lv_chart_refresh(s_rec_plot);
#endif
    }

    /*
     * Keep the LV_PART_ITEMS line colour in sync with the active tab.
     *
     * lv_chart_set_series_color() only updates the per-series colour metadata,
     * but the actual line is rendered using the LV_PART_ITEMS style on the
     * chart object (see lv_chart.c:1039 / 1366 — the renderer reads the line
     * descriptor from LV_PART_ITEMS, not from the series). Without this update
     * the line would always render in the colour we set at chart-creation
     * time (ECG green) regardless of which tab is active.
     */
    if (s_rec_plot) {
        lv_obj_set_style_line_color(s_rec_plot,
                                     rec_tab_colour(s_active_rec_tab),
                                     LV_PART_ITEMS);
    }

    rec_update_plot();
}

/* -------------------------------------------------------------------------- */
/* Record screen callbacks                                                    */
/* -------------------------------------------------------------------------- */

static void rec_prev_tab_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();
    s_active_rec_tab = (rec_tab_t)(((int)s_active_rec_tab - 1 + REC_TAB_COUNT)
                                    % REC_TAB_COUNT);
    rec_apply_active_tab();
    rec_update_topbar();
}

static void rec_next_tab_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();
    s_active_rec_tab = (rec_tab_t)(((int)s_active_rec_tab + 1) % REC_TAB_COUNT);
    rec_apply_active_tab();
    rec_update_topbar();
}

static void rec_kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if ((code == LV_EVENT_READY || code == LV_EVENT_CANCEL) && s_rec_keyboard)
        lv_obj_add_flag(s_rec_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void rec_ta_event_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();
    if (s_rec_keyboard) {
        lv_obj_clear_flag(s_rec_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_rec_keyboard, s_rec_name_ta);
    }
}

static void rec_startstop_btn_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();

    if (!svc_rec_is_recording()) {
        /* hide keyboard */
        if (s_rec_keyboard)
            lv_obj_add_flag(s_rec_keyboard, LV_OBJ_FLAG_HIDDEN);

        /* sanitise label into a local buffer */
        char label[REC_LABEL_MAX] = {0};
        if (s_rec_name_ta) {
            const char *txt = lv_textarea_get_text(s_rec_name_ta);
            size_t j = 0;
            if (txt && txt[0]) {
                for (size_t i = 0; txt[i] && j < REC_LABEL_MAX-1; i++) {
                    char c = txt[i];
                    if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
                        (c>='0'&&c<='9')||c=='_')
                        label[j++] = c;
                    else if (c==' ')
                        label[j++] = '_';
                }
            }
            label[j] = '\0';
        }

        esp_err_t err = svc_rec_start(label);
        if (err == ESP_ERR_NO_MEM) {
            if (s_lbl_rec_status)
                lv_label_set_text(s_lbl_rec_status, "ERROR: no memory");
            return;
        }
        if (err == ESP_ERR_INVALID_STATE) {
            if (s_lbl_rec_status)
                lv_label_set_text(s_lbl_rec_status, "ERROR: task failed");
            return;
        }
        if (err != ESP_OK) {
            if (s_lbl_rec_status)
                lv_label_set_text(s_lbl_rec_status, "ERROR: SD write failed");
            return;
        }

        if (s_lbl_rec_btn)
            lv_label_set_text(s_lbl_rec_btn, LV_SYMBOL_STOP "  STOP");
        if (s_btn_rec_startstop)
            style_button(s_btn_rec_startstop, COLOUR_ERROR, COLOUR_ERROR);
        if (s_rec_name_ta)
            lv_obj_add_flag(s_rec_name_ta, LV_OBJ_FLAG_HIDDEN);

    } else {
        svc_rec_stop();
        if (s_lbl_rec_btn)
            lv_label_set_text(s_lbl_rec_btn, LV_SYMBOL_PLAY "  REC");
        if (s_btn_rec_startstop)
            style_button(s_btn_rec_startstop, COLOUR_SURFACE2,
                         COLOUR_SUCCESS);
        if (s_rec_name_ta)
            lv_obj_clear_flag(s_rec_name_ta, LV_OBJ_FLAG_HIDDEN);
        if (s_lbl_rec_status)
            lv_label_set_text(s_lbl_rec_status, "Saved to SD");
    }
}

static void rec_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_screen_on) return;

    int64_t t_cb = esp_timer_get_time();
    rec_update_topbar();
    rec_update_plot();
    int64_t dt = esp_timer_get_time() - t_cb;
    if (dt > 20000)
        ESP_LOGW(TAG, "rec_timer: slow %.1f ms", (double)dt / 1000.0);

    if (svc_rec_is_recording()) {
        int batt = s_cached_batt_pct;   /* updated at 1 Hz by clock_update_task */
        if (batt >= 0 && batt <= REC_BATT_STOP_PCT) {
            ESP_LOGW(TAG, "Battery low (%d%%), stopping", batt);
            svc_rec_stop();
            if (hal_display_lock_ms(DISPLAY_LOCK_SLICE_MS)) {
                if (s_lbl_rec_btn)
                    lv_label_set_text(s_lbl_rec_btn,
                                      LV_SYMBOL_PLAY "  REC");
                if (s_btn_rec_startstop)
                    style_button(s_btn_rec_startstop, COLOUR_SURFACE2,
                                 COLOUR_SUCCESS);
                if (s_rec_name_ta)
                    lv_obj_clear_flag(s_rec_name_ta, LV_OBJ_FLAG_HIDDEN);
                if (s_lbl_rec_status)
                    lv_label_set_text(s_lbl_rec_status,
                                      "Stopped: low battery");
                hal_display_unlock();
            }
        }
        if (s_lbl_rec_status) {
            uint32_t elapsed_s = (uint32_t)(
                (esp_timer_get_time()/1000LL - svc_rec_get_start_ms()) / 1000UL);
            char rbuf[32];
            snprintf(rbuf, sizeof(rbuf),
                     LV_SYMBOL_AUDIO "  REC  %02lu:%02lu",
                     (unsigned long)(elapsed_s/60),
                     (unsigned long)(elapsed_s%60));
            lv_label_set_text(s_lbl_rec_status, rbuf);
        }
    }
}

static lv_obj_t *rec_make_metric(lv_obj_t *parent, const char *initial,
                                   int x, int y, lv_color_t colour)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, initial);
    lv_obj_set_style_text_color(lbl, colour, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

/* -------------------------------------------------------------------------- */
/* ui_create_record_screen                                                    */
/* -------------------------------------------------------------------------- */

/*
 * Layout (320 × 240 px):
 *   y=  4   topbar card  310×44  – RR | HR | SpO2 / BAT | PAT | drift
 *   y= 49   tab row  6×38 px     – ECG | PPG | RESP | NAS | FCG1 | FCG2
 *   y= 73   plot card 320×76     – 4 s rolling chart
 *   y=152   name textarea 280×28 – optional recording label
 *   y=182   REC/STOP button 280×34
 *   y=218   status label
 *   bottom  keyboard (hidden until textarea tapped)
 */
/*
 * Build the ECG chart inside s_rec_plot_card. Called only AFTER ~800 ms
 * have elapsed since navigating to the Record screen (gated in
 * rec_update_plot). Building it during ui_create_record_screen() and
 * having it present during the screen-load animation reliably hangs the
 * LVGL 9.5 render pipeline on this build, even at REC_CHART_POINTS=200.
 */
static void rec_build_chart(void)
{
    if (s_rec_plot || !s_rec_plot_card) return;

    s_rec_plot = lv_chart_create(s_rec_plot_card);
    lv_obj_set_size(s_rec_plot, LCD_H_RES, ECG_PLOT_H);
    lv_obj_align(s_rec_plot, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_rec_plot, COLOUR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_rec_plot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_rec_plot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_rec_plot, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_rec_plot, 0, LV_PART_MAIN);
    lv_chart_set_type(s_rec_plot, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_rec_plot, REC_CHART_POINTS);
    lv_chart_set_range(s_rec_plot, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    lv_chart_set_div_line_count(s_rec_plot, 0, 0);
    lv_chart_set_update_mode(s_rec_plot, LV_CHART_UPDATE_MODE_CIRCULAR);
    lv_obj_set_style_size(s_rec_plot, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(s_rec_plot, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_color(s_rec_plot, rec_tab_colour(s_active_rec_tab),
                                LV_PART_ITEMS);
    lv_obj_set_style_line_opa(s_rec_plot, LV_OPA_COVER, LV_PART_ITEMS);
    s_rec_series = lv_chart_add_series(s_rec_plot,
                                       rec_tab_colour(s_active_rec_tab),
                                       LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_series_ext_y_array(s_rec_plot, s_rec_series, s_rec_chart_points);
}

/*
 * Tear down the chart so the next navigation to Record gets a fresh build.
 * Safe from event handlers (e.g. home_btn_event when leaving Record); not
 * safe while LVGL is mid-render.
 */
static void rec_destroy_chart(void)
{
    if (!s_rec_plot) return;
    lv_obj_del(s_rec_plot);
    s_rec_plot = NULL;
    s_rec_series = NULL;
}

static void ui_create_record_screen(void)
{
    s_scr_record = lv_obj_create(NULL);
    /* alias so existing menu navigation code (which holds &s_scr_health)
       still resolves to the live object */
    s_scr_health = s_scr_record;
    style_screen(s_scr_record);

    /* ── Top bar ─────────────────────────────────────────────────── */
    s_rec_topbar = lv_obj_create(s_scr_record);
    lv_obj_set_size(s_rec_topbar, LCD_H_RES - 10, 44);
    lv_obj_align(s_rec_topbar, LV_ALIGN_TOP_MID, 0, 0);
    style_card(s_rec_topbar, 14);
    lv_obj_set_style_pad_all(s_rec_topbar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_rec_topbar, 0, LV_PART_MAIN);   /* belt-and-braces: override theme top pad */
    lv_obj_clear_flag(s_rec_topbar, LV_OBJ_FLAG_SCROLLABLE);

    /* Row 1 — y=0: at the very top of the content area. */
    s_lbl_rec_rr   = rec_make_metric(s_rec_topbar, "RR -- bpm",  10,  0,
                                      COLOUR_ECG);
    s_lbl_rec_hr   = rec_make_metric(s_rec_topbar, "HR --",      112,  0,
                                      COLOUR_PPG);
    s_lbl_rec_spo2 = rec_make_metric(s_rec_topbar, "SpO2 --%",   206,  0,
                                      COLOUR_ACCENT);
    /* Row 2 — y=22: 3/4 character below row 1. */
    s_lbl_rec_batt = rec_make_metric(s_rec_topbar, "BAT --%",    10, 22,
                                      COLOUR_SUCCESS);
    s_lbl_rec_pat  = rec_make_metric(s_rec_topbar, "PAT --",     112, 22,
                                      COLOUR_PPG);
    s_lbl_rec_sd   = rec_make_metric(s_rec_topbar, "SD 0 ms",    206, 22,
                                      COLOUR_TEXT);

    /* ── Plot strip ──────────────────────────────────────────────── */
    s_rec_plot_card = lv_obj_create(s_scr_record);
    lv_obj_set_size(s_rec_plot_card, LCD_H_RES, ECG_PLOT_H + 6);
    lv_obj_align(s_rec_plot_card, LV_ALIGN_TOP_MID, 0, 70);
    style_card(s_rec_plot_card, 0);
    lv_obj_clear_flag(s_rec_plot_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_rec_plot_card, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_rec_plot_card, 0, LV_PART_MAIN);
    /* Chart itself is built lazily — see rec_build_chart() — to avoid the
     * full-screen render pipeline hang triggered by the 400-point chart
     * during the screen-load animation. */

    /* ── Name text-area ──────────────────────────────────────────── */
    s_rec_name_ta = lv_textarea_create(s_scr_record);
    lv_obj_set_size(s_rec_name_ta, 280, 28);
    lv_obj_set_pos(s_rec_name_ta, 20, 149);
    lv_textarea_set_one_line(s_rec_name_ta, true);
    lv_textarea_set_placeholder_text(s_rec_name_ta, "e.g. rest_baseline");
    lv_textarea_set_max_length(s_rec_name_ta, REC_LABEL_MAX - 1);
    lv_obj_set_style_bg_color(s_rec_name_ta, COLOUR_SURFACE2,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_rec_name_ta, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_rec_name_ta, COLOUR_SUBTEXT,
                                   LV_PART_MAIN);
    lv_obj_set_style_border_width(s_rec_name_ta, 1, LV_PART_MAIN);
    lv_obj_add_event_cb(s_rec_name_ta, rec_ta_event_cb,
                        LV_EVENT_CLICKED, NULL);

    /* ── REC/STOP button ─────────────────────────────────────────── */
    s_btn_rec_startstop = lv_btn_create(s_scr_record);
    lv_obj_set_size(s_btn_rec_startstop, 280, 34);
    lv_obj_set_pos(s_btn_rec_startstop, 20, 179);
    style_button(s_btn_rec_startstop, COLOUR_SURFACE2, COLOUR_SUCCESS);
    lv_obj_add_event_cb(s_btn_rec_startstop, rec_startstop_btn_cb,
                        LV_EVENT_CLICKED, NULL);

    s_lbl_rec_btn = lv_label_create(s_btn_rec_startstop);
    lv_label_set_text(s_lbl_rec_btn, LV_SYMBOL_PLAY "  REC");
    lv_obj_set_style_text_color(s_lbl_rec_btn, COLOUR_SUCCESS,
                                 LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_rec_btn, &lv_font_montserrat_18,
                                LV_PART_MAIN);
    lv_obj_center(s_lbl_rec_btn);

    /* ── Status label ────────────────────────────────────────────── */
    s_lbl_rec_status = lv_label_create(s_scr_record);
    lv_label_set_text(s_lbl_rec_status, "Ready – press REC to start");
    lv_obj_set_style_text_color(s_lbl_rec_status, COLOUR_SUBTEXT,
                                 LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_rec_status, &lv_font_montserrat_14,
                                LV_PART_MAIN);
    lv_obj_set_pos(s_lbl_rec_status, 8, 215);

    /* ── Signal cycle row: [< Prev]  ECG  [Next >] ──────────────── */
    /* Two 100 px buttons flanking a centred signal-name label.
     * Much easier to hit on the resistive screen than 6 × 38 px tabs. */
    {
        const int tab_y = 46;

        lv_obj_t *btn_prev = lv_btn_create(s_scr_record);
        lv_obj_set_size(btn_prev, 100, 22);
        lv_obj_set_pos(btn_prev, 20, tab_y);
        style_button(btn_prev, COLOUR_SURFACE2, COLOUR_SUBTEXT);
        lv_obj_t *lp = lv_label_create(btn_prev);
        lv_label_set_text(lp, LV_SYMBOL_LEFT "  Prev");
        lv_obj_set_style_text_color(lp, COLOUR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(lp, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_center(lp);
        lv_obj_add_event_cb(btn_prev, rec_prev_tab_cb, LV_EVENT_CLICKED, NULL);

        s_lbl_rec_tab_name = lv_label_create(s_scr_record);
        lv_obj_set_style_text_font(s_lbl_rec_tab_name,
                                    &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_lbl_rec_tab_name, LV_ALIGN_TOP_MID, 0, tab_y + 4);

        lv_obj_t *btn_next = lv_btn_create(s_scr_record);
        lv_obj_set_size(btn_next, 100, 22);
        lv_obj_set_pos(btn_next, 200, tab_y);
        style_button(btn_next, COLOUR_SURFACE2, COLOUR_SUBTEXT);
        lv_obj_t *ln = lv_label_create(btn_next);
        lv_label_set_text(ln, "Next  " LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(ln, COLOUR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(ln, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_center(ln);
        lv_obj_add_event_cb(btn_next, rec_next_tab_cb, LV_EVENT_CLICKED, NULL);
    }

    /* ── On-screen keyboard ──────────────────────────────────────── */
    s_rec_keyboard = lv_keyboard_create(s_scr_record);
    lv_obj_set_size(s_rec_keyboard, LCD_H_RES, 200);
    lv_obj_align(s_rec_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_rec_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_rec_keyboard, s_rec_name_ta);
    lv_obj_add_event_cb(s_rec_keyboard, rec_kb_event_cb,
                        LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_rec_keyboard, rec_kb_event_cb,
                        LV_EVENT_CANCEL, NULL);
    /* Reset inactivity timer on every key press so typing never triggers sleep */
    lv_obj_add_event_cb(s_rec_keyboard, reset_activity_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Bottom hint ─────────────────────────────────────────────── */
    lv_obj_t *hint = lv_label_create(s_scr_record);
    lv_label_set_text(hint, "Bk = Home");
    lv_obj_set_style_text_color(hint, COLOUR_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, -8, -8);

    /* ── Initial state ───────────────────────────────────────────── */
    s_active_rec_tab = REC_TAB_ECG;
    rec_apply_active_tab();
    rec_update_topbar();
    rec_update_plot();

    if (!s_rec_ui_timer)
        s_rec_ui_timer = lv_timer_create(rec_ui_timer_cb,
                                          ECG_UI_REFRESH_MS, NULL);
}

/* -------------------------------------------------------------------------- */
/* Settings                                                                   */
/* -------------------------------------------------------------------------- */

static void brightness_event_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();

    if (!s_slider_bright || !s_lbl_bright_val) {
        return;
    }

    s_brightness = lv_slider_get_value(s_slider_bright);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", s_brightness);
    lv_label_set_text(s_lbl_bright_val, buf);

    hal_backlight_set_percent(s_brightness);
}

static void timeout_dd_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    reset_activity();

    uint16_t idx = lv_dropdown_get_selected(dd);
    switch (idx) {
        case 0: s_timeout_s = 15; break;
        case 1: s_timeout_s = 30; break;
        case 2: s_timeout_s = 60; break;
        case 3: s_timeout_s = 120; break;
        default: s_timeout_s = DEFAULT_TIMEOUT_S; break;
    }
}

static lv_obj_t *settings_row(lv_obj_t *parent, int y, const char *title, const char *subtitle)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, LCD_H_RES - 14, 46);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(panel, COLOUR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, COLOUR_SURFACE2, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 12, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(panel);
    lv_label_set_text(ttl, title);
    lv_obj_set_style_text_color(ttl, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 12, 5);

    lv_obj_t *sub = lv_label_create(panel);
    lv_label_set_text(sub, subtitle);
    lv_obj_set_style_text_color(sub, COLOUR_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 12, 28);

    return panel;
}

/* -------------------------------------------------------------------------- */
/* UI creation                                                                */
/* -------------------------------------------------------------------------- */

static void home_btn_event(lv_event_t *e)
{
    reset_activity();
    lv_obj_t *target = (lv_obj_t *)lv_event_get_user_data(e);
    if (!target) return;

    if (target == s_scr_record) {
        /* Defer chart construction until 800 ms after this navigation; the
         * 400-point LVGL chart hangs the render pipeline if present during
         * the screen-load animation. See rec_build_chart() / rec_update_plot. */
        s_nav_rec_us = esp_timer_get_time();
        rec_destroy_chart();
    }
    if (target == s_scr_bp) {
        s_nav_bp_us = esp_timer_get_time();
        bp_destroy_chart();
    }
#if LVGL_VERSION_MAJOR >= 9
    lv_screen_load_anim(target, LV_SCR_LOAD_ANIM_FADE_IN, 240, 0, false);
#else
    lv_scr_load_anim(target, LV_SCR_LOAD_ANIM_FADE_IN, 240, 0, false);
#endif
}

static lv_obj_t *make_home_tile(lv_obj_t *parent, int x, int y, const char *text)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 74, 52);
    lv_obj_set_pos(btn, x, y);
    style_button(btn, COLOUR_SURFACE, COLOUR_SUBTEXT);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_center(lbl);

    return btn;
}

static void ui_create_home_screen(void)
{
    s_scr_home = lv_obj_create(NULL);
    style_screen(s_scr_home);

    lv_obj_t *title = lv_label_create(s_scr_home);
    lv_label_set_text(title, "Watch");
    lv_obj_set_style_text_color(title, COLOUR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 10);

    s_lbl_battery = lv_label_create(s_scr_home);
    lv_label_set_text(s_lbl_battery, "--%");
    lv_obj_set_style_text_color(s_lbl_battery, COLOUR_SUCCESS, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_battery, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_lbl_battery, LV_ALIGN_TOP_RIGHT, -10, 12);

    s_lbl_time = lv_label_create(s_scr_home);
    lv_label_set_text(s_lbl_time, "--:--");
    lv_obj_set_style_text_color(s_lbl_time, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_time, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_align(s_lbl_time, LV_ALIGN_TOP_LEFT, 10, 30);

    s_lbl_date = lv_label_create(s_scr_home);
    lv_label_set_text(s_lbl_date, "Time not synced");
    lv_obj_set_style_text_color(s_lbl_date, COLOUR_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_date, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_lbl_date, LV_ALIGN_TOP_LEFT, 10, 92);

    s_lbl_time_hint = lv_label_create(s_scr_home);
    lv_label_set_text(s_lbl_time_hint, "Connect WiFi in Settings");
    lv_obj_set_style_text_color(s_lbl_time_hint, COLOUR_WARN, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_time_hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_lbl_time_hint, LV_ALIGN_TOP_LEFT, 10, 112);

    /*
     * 320×240 landscape layout: clock/date on left, 2×3 tile grid on right.
     *   tile w=74, h=52, gap=8
     *   col 1 x=162, col 2 x=244  (right of 320)
     *   row 1 y=18, row 2 y=78, row 3 y=138
     */
    lv_obj_t *b1 = make_home_tile(s_scr_home,  162, 18, "Record");
    lv_obj_t *b2 = make_home_tile(s_scr_home, 244, 18, "B.P.");
    lv_obj_t *b3 = make_home_tile(s_scr_home,  162, 78, "Files");
    lv_obj_t *b4 = make_home_tile(s_scr_home, 244, 78, "Settings");
    lv_obj_t *b5 = make_home_tile(s_scr_home,  162, 138, "About");

    lv_obj_add_event_cb(b1, home_btn_event, LV_EVENT_CLICKED, s_scr_record);
    lv_obj_add_event_cb(b2, home_btn_event, LV_EVENT_CLICKED, s_scr_pump);
    lv_obj_add_event_cb(b3, home_btn_event, LV_EVENT_CLICKED, s_scr_files);
    lv_obj_add_event_cb(b4, home_btn_event, LV_EVENT_CLICKED, s_scr_settings);
    lv_obj_add_event_cb(b5, home_btn_event, LV_EVENT_CLICKED, s_scr_about);
}

static void ui_create_wifi_screen(void)
{
    s_scr_wifi = lv_obj_create(NULL);
    style_screen(s_scr_wifi);

    /* Compact top section: title + status on first line, 3 buttons on second,
     * then the list takes the remaining height. */
    make_title(s_scr_wifi, "WiFi Networks", 8);

    s_wifi_status = lv_label_create(s_scr_wifi);
    lv_label_set_text(s_wifi_status, "Tap Rescan to search");
    lv_obj_set_style_text_color(s_wifi_status, COLOUR_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wifi_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_wifi_status, LV_ALIGN_TOP_RIGHT, -8, 12);

    lv_obj_t *btn_back = lv_btn_create(s_scr_wifi);
    lv_obj_set_size(btn_back, 80, 30);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 8, 36);
    style_button(btn_back, COLOUR_SURFACE2, COLOUR_SUBTEXT);
    lv_obj_add_event_cb(btn_back, wifi_back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_set_style_text_color(lbl_back, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_center(lbl_back);

    lv_obj_t *btn_main = lv_btn_create(s_scr_wifi);
    lv_obj_set_size(btn_main, 100, 30);
    lv_obj_align(btn_main, LV_ALIGN_TOP_MID, 0, 36);
    style_button(btn_main, COLOUR_SURFACE2, COLOUR_ACCENT);
    lv_obj_add_event_cb(btn_main, wifi_main_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_main = lv_label_create(btn_main);
    lv_label_set_text(lbl_main, "Main Menu");
    lv_obj_set_style_text_color(lbl_main, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_center(lbl_main);

    lv_obj_t *btn_rescan = lv_btn_create(s_scr_wifi);
    lv_obj_set_size(btn_rescan, 80, 30);
    lv_obj_align(btn_rescan, LV_ALIGN_TOP_RIGHT, -8, 36);
    style_button(btn_rescan, COLOUR_SURFACE2, COLOUR_SUCCESS);
    lv_obj_add_event_cb(btn_rescan, wifi_rescan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_rescan = lv_label_create(btn_rescan);
    lv_label_set_text(lbl_rescan, "Rescan");
    lv_obj_set_style_text_color(lbl_rescan, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_center(lbl_rescan);

    /* List container fills the remaining height below the buttons. */
    s_wifi_container = lv_obj_create(s_scr_wifi);
    lv_obj_set_size(s_wifi_container, LCD_H_RES - 16, LCD_V_RES - 72);
    lv_obj_align(s_wifi_container, LV_ALIGN_TOP_MID, 0, 68);
    lv_obj_set_style_bg_color(s_wifi_container, COLOUR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_wifi_container, COLOUR_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_wifi_container, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_wifi_container, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_wifi_container, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_wifi_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_wifi_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_wifi_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_wifi_container, LV_SCROLLBAR_MODE_AUTO);
}

static void ui_create_password_screen(void)
{
    s_scr_pass = lv_obj_create(NULL);
    style_screen(s_scr_pass);

    make_title(s_scr_pass, "WiFi Password", 18);

    lv_obj_t *ssidcap = lv_label_create(s_scr_pass);
    lv_label_set_text(ssidcap, "Network");
    lv_obj_set_style_text_color(ssidcap, COLOUR_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(ssidcap, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(ssidcap, LV_ALIGN_TOP_LEFT, 12, 44);

    s_lbl_ssid = lv_label_create(s_scr_pass);
    lv_label_set_text(s_lbl_ssid, "(none)");
    lv_obj_set_style_text_color(s_lbl_ssid, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_ssid, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(s_lbl_ssid, LV_ALIGN_TOP_LEFT, 12, 62);

    s_lbl_saved_hint = lv_label_create(s_scr_pass);
    lv_label_set_text(s_lbl_saved_hint, "Enter password");
    lv_obj_set_style_text_color(s_lbl_saved_hint, COLOUR_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_saved_hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_lbl_saved_hint, LV_ALIGN_TOP_LEFT, 24, 120);

    s_ta_pass = lv_textarea_create(s_scr_pass);
    lv_obj_set_size(s_ta_pass, 212, 40);
    lv_obj_align(s_ta_pass, LV_ALIGN_TOP_MID, 0, 108);
    lv_textarea_set_password_mode(s_ta_pass, true);
    lv_textarea_set_placeholder_text(s_ta_pass, "Enter password");
    lv_obj_set_style_bg_color(s_ta_pass, COLOUR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ta_pass, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ta_pass, COLOUR_ACCENT, LV_PART_MAIN);
    lv_obj_add_event_cb(s_ta_pass, pass_ta_event_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *btn_back = lv_btn_create(s_scr_pass);
    lv_obj_set_size(btn_back, 100, 40);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 12, 158);
    style_button(btn_back, COLOUR_SURFACE2, COLOUR_SUBTEXT);
    lv_obj_add_event_cb(btn_back, pass_back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_set_style_text_color(lbl_back, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_center(lbl_back);

    lv_obj_t *btn_conn = lv_btn_create(s_scr_pass);
    lv_obj_set_size(btn_conn, 100, 40);
    lv_obj_align(btn_conn, LV_ALIGN_TOP_RIGHT, -12, 158);
    style_button(btn_conn, COLOUR_SURFACE2, COLOUR_SUCCESS);
    lv_obj_add_event_cb(btn_conn, pass_connect_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_conn = lv_label_create(btn_conn);
    lv_label_set_text(lbl_conn, "Connect");
    lv_obj_set_style_text_color(lbl_conn, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_center(lbl_conn);

    s_keyboard = lv_keyboard_create(s_scr_pass);
    lv_obj_set_size(s_keyboard, LCD_H_RES, 180);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, pass_kb_event_cb, LV_EVENT_ALL, NULL);
    /* Reset inactivity timer on every key press */
    lv_obj_add_event_cb(s_keyboard, reset_activity_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
}

static void ui_create_connecting_screen(void)
{
    s_scr_conn = lv_obj_create(NULL);
    style_screen(s_scr_conn);

    make_title(s_scr_conn, "Connecting", 40);

    lv_obj_t *lbl = lv_label_create(s_scr_conn);
    lv_label_set_text(lbl, "Please wait...");
    lv_obj_set_style_text_color(lbl, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *sub = lv_label_create(s_scr_conn);
    lv_label_set_text(sub, "Joining WiFi and syncing time");
    lv_obj_set_style_text_color(sub, COLOUR_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 26);
}

static void ui_create_simple_page(lv_obj_t **scrout, const char *title, const char *body)
{
    *scrout = lv_obj_create(NULL);
    style_screen(*scrout);

    make_title(*scrout, title, 24);

    lv_obj_t *lbl = lv_label_create(*scrout);
    lv_label_set_text(lbl, body);
    lv_obj_set_width(lbl, 210);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -10);

    add_back_button(*scrout);
}

/* ========================================================================== */
/* Touch calibration                                                          */
/* ========================================================================== */

/* NVS namespace / key names for touch calibration. */
#define TOUCH_CAL_NS   "tcal"
#define TOUCH_CAL_XMIN "xmin"
#define TOUCH_CAL_XMAX "xmax"
#define TOUCH_CAL_YMIN "ymin"
#define TOUCH_CAL_YMAX "ymax"

static bool load_touch_cal_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(TOUCH_CAL_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint16_t xmin, xmax, ymin, ymax;
    bool ok = (nvs_get_u16(h, TOUCH_CAL_XMIN, &xmin) == ESP_OK) &&
              (nvs_get_u16(h, TOUCH_CAL_XMAX, &xmax) == ESP_OK) &&
              (nvs_get_u16(h, TOUCH_CAL_YMIN, &ymin) == ESP_OK) &&
              (nvs_get_u16(h, TOUCH_CAL_YMAX, &ymax) == ESP_OK);
    nvs_close(h);
    if (ok) hal_touch_set_calibration(xmin, xmax, ymin, ymax);
    return ok;
}

static void save_touch_cal_nvs(uint16_t xmin, uint16_t xmax,
                                uint16_t ymin, uint16_t ymax)
{
    nvs_handle_t h;
    if (nvs_open(TOUCH_CAL_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, TOUCH_CAL_XMIN, xmin);
    nvs_set_u16(h, TOUCH_CAL_XMAX, xmax);
    nvs_set_u16(h, TOUCH_CAL_YMIN, ymin);
    nvs_set_u16(h, TOUCH_CAL_YMAX, ymax);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Touch cal saved: x %u-%u  y %u-%u", xmin, xmax, ymin, ymax);
}

/* Calibration target screen positions: top-left, top-right,
 * bottom-right, bottom-left (20 px from each corner). */
static const int16_t CAL_TX[4] = { 20, 299, 299,  20 };
static const int16_t CAL_TY[4] = { 20,  20, 219, 219 };
static const char *CAL_NAMES[4] = {
    "Top-left", "Top-right", "Bottom-right", "Bottom-left"
};

static struct {
    int        step;
    uint16_t   raw_x[4];
    uint16_t   raw_y[4];
    int        stable;
    uint16_t   prev_rx, prev_ry;
    bool       pen_was_down;
    lv_obj_t  *scr;
    lv_obj_t  *h_arm;
    lv_obj_t  *v_arm;
    lv_obj_t  *instr;
    lv_obj_t  *step_lbl;
    lv_timer_t *timer;
} s_cal;

static void cal_draw_crosshair(int16_t tx, int16_t ty)
{
    const int ARM = 20, THICK = 2;
    lv_obj_set_size(s_cal.h_arm, ARM * 2, THICK);
    lv_obj_set_pos(s_cal.h_arm,  tx - ARM, ty - THICK / 2);
    lv_obj_set_size(s_cal.v_arm, THICK, ARM * 2);
    lv_obj_set_pos(s_cal.v_arm,  tx - THICK / 2, ty - ARM);
}

static void cal_finish_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    nav_to_home_locked(NULL);
}

static void cal_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_cal.step >= 4) return;

    uint16_t rx, ry;
    bool down = hal_touch_read_raw(&rx, &ry);

    if (!down) {
        /* Require pen-up between corners so a long press doesn't consume
         * multiple steps.  Reset stable count on release. */
        if (s_cal.pen_was_down) s_cal.stable = 0;
        s_cal.pen_was_down = false;
        return;
    }
    s_cal.pen_was_down = true;

    /* Stability: 6 consecutive samples within ±100 raw units. */
    if (s_cal.stable > 0 &&
        (abs((int)rx - s_cal.prev_rx) > 100 ||
         abs((int)ry - s_cal.prev_ry) > 100)) {
        s_cal.stable = 0;
    }
    s_cal.prev_rx = rx;
    s_cal.prev_ry = ry;
    if (++s_cal.stable < 6) return;
    s_cal.stable = 0;
    s_cal.pen_was_down = false;   /* force pen-up before next corner */

    s_cal.raw_x[s_cal.step] = rx;
    s_cal.raw_y[s_cal.step] = ry;
    ESP_LOGI(TAG, "Cal step %d: raw x=%u y=%u", s_cal.step, rx, ry);
    s_cal.step++;

    if (s_cal.step < 4) {
        /* Next corner */
        cal_draw_crosshair(CAL_TX[s_cal.step], CAL_TY[s_cal.step]);
        char buf[48];
        snprintf(buf, sizeof(buf), "Touch Calibration\n%s — tap target",
                 CAL_NAMES[s_cal.step]);
        lv_label_set_text(s_cal.instr, buf);
        char sbuf[16];
        snprintf(sbuf, sizeof(sbuf), "%d / 4", s_cal.step + 1);
        lv_label_set_text(s_cal.step_lbl, sbuf);
        return;
    }

    /* All 4 corners recorded — compute calibration.
     * raw_x: corners 0,3 are LEFT (high), corners 1,2 are RIGHT (low).
     * raw_y: corners 0,1 are TOP (high),  corners 2,3 are BOTTOM (low). */
    uint16_t rx_left  = (uint16_t)(((uint32_t)s_cal.raw_x[0]
                                   + s_cal.raw_x[3]) / 2);
    uint16_t rx_right = (uint16_t)(((uint32_t)s_cal.raw_x[1]
                                   + s_cal.raw_x[2]) / 2);
    uint16_t ry_top   = (uint16_t)(((uint32_t)s_cal.raw_y[0]
                                   + s_cal.raw_y[1]) / 2);
    uint16_t ry_bot   = (uint16_t)(((uint32_t)s_cal.raw_y[2]
                                   + s_cal.raw_y[3]) / 2);

    /* Extrapolate the measured midpoints (at ±20 px from edges) out to
     * the true screen edges (x=0..319, y=0..239).
     * Calibration targets were at x=20,299 (span=279) and y=20,219 (span=199). */
    int32_t dx = (int32_t)rx_left - rx_right;
    int32_t dy = (int32_t)ry_top  - ry_bot;
    uint16_t x_max = (uint16_t)((int32_t)rx_left  + dx * 20 / 279);
    uint16_t x_min = (uint16_t)((int32_t)rx_right - dx * 20 / 279);
    uint16_t y_max = (uint16_t)((int32_t)ry_top   + dy * 20 / 199);
    uint16_t y_min = (uint16_t)((int32_t)ry_bot   - dy * 20 / 199);

    hal_touch_set_calibration(x_min, x_max, y_min, y_max);
    save_touch_cal_nvs(x_min, x_max, y_min, y_max);

    lv_label_set_text(s_cal.instr, "Calibration saved!\nLoading…");
    lv_label_set_text(s_cal.step_lbl, "Done");
    lv_timer_delete(s_cal.timer);
    s_cal.timer = NULL;
    lv_timer_create(cal_finish_cb, 1200, NULL);
}

static void run_touch_calibration(void)
{
    memset(&s_cal, 0, sizeof(s_cal));

    s_cal.scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_cal.scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_cal.scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_cal.scr, LV_SCROLLBAR_MODE_OFF);

    s_cal.step_lbl = lv_label_create(s_cal.scr);
    lv_label_set_text(s_cal.step_lbl, "1 / 4");
    lv_obj_set_style_text_color(s_cal.step_lbl, lv_color_hex(0xA0B0C0), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_cal.step_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_cal.step_lbl, LV_ALIGN_TOP_MID, 0, 8);

    s_cal.instr = lv_label_create(s_cal.scr);
    lv_label_set_text(s_cal.instr, "Touch Calibration\nTop-left — tap target");
    lv_obj_set_style_text_color(s_cal.instr, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_cal.instr, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_cal.instr, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_cal.instr, LV_ALIGN_CENTER, 0, 0);

    /* Horizontal and vertical crosshair arms */
    lv_color_t ch_col = lv_color_hex(0xFF5A30);
    s_cal.h_arm = lv_obj_create(s_cal.scr);
    lv_obj_set_style_bg_color(s_cal.h_arm, ch_col, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_cal.h_arm, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_cal.h_arm, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_cal.h_arm, 0, LV_PART_MAIN);

    s_cal.v_arm = lv_obj_create(s_cal.scr);
    lv_obj_set_style_bg_color(s_cal.v_arm, ch_col, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_cal.v_arm, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_cal.v_arm, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_cal.v_arm, 0, LV_PART_MAIN);

    cal_draw_crosshair(CAL_TX[0], CAL_TY[0]);

#if LVGL_VERSION_MAJOR >= 9
    lv_screen_load(s_cal.scr);
#else
    lv_scr_load(s_cal.scr);
#endif

    s_cal.timer = lv_timer_create(cal_timer_cb, 30, NULL);
}

static void settings_cal_btn_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();
    run_touch_calibration();
}

/* ========================================================================== */

static void ui_create_settings_screen(void)
{
    s_scr_settings = lv_obj_create(NULL);
    style_screen(s_scr_settings);

    make_title(s_scr_settings, "Settings", 18);
    add_back_button(s_scr_settings);

    lv_obj_t *wifirow = settings_row(s_scr_settings, 38, "WiFi", "Scan and connect for time sync");
    lv_obj_t *wifibtn = lv_btn_create(wifirow);
    lv_obj_set_size(wifibtn, 100, 30);
    lv_obj_align(wifibtn, LV_ALIGN_RIGHT_MID, -10, 0);
    style_button(wifibtn, COLOUR_SURFACE2, COLOUR_ACCENT);
    lv_obj_add_event_cb(wifibtn, settings_wifi_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wifilbl = lv_label_create(wifibtn);
    lv_label_set_text(wifilbl, "Connect");
    lv_obj_set_style_text_color(wifilbl, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_center(wifilbl);

    lv_obj_t *brightrow = settings_row(s_scr_settings, 90, "Brightness", "Adjust screen brightness");
    s_slider_bright = lv_slider_create(brightrow);
    lv_obj_set_size(s_slider_bright, 130, 8);
    lv_obj_align(s_slider_bright, LV_ALIGN_RIGHT_MID, -44, -8);
    lv_slider_set_range(s_slider_bright, 10, 100);
    lv_slider_set_value(s_slider_bright, s_brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_slider_bright, brightness_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_lbl_bright_val = lv_label_create(brightrow);
    lv_label_set_text(s_lbl_bright_val, "80%");
    lv_obj_set_style_text_color(s_lbl_bright_val, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_align(s_lbl_bright_val, LV_ALIGN_RIGHT_MID, -12, -8);

    lv_obj_t *timeoutrow = settings_row(s_scr_settings, 142, "Screen timeout", "Auto sleep delay");
    lv_obj_t *dd = lv_dropdown_create(timeoutrow);
    lv_dropdown_set_options(dd, "15 s\n30 s\n60 s\n120 s");
    lv_dropdown_set_selected(dd, 1);
    lv_obj_set_width(dd, 96);
    lv_obj_align(dd, LV_ALIGN_RIGHT_MID, -10, -4);
    lv_obj_add_event_cb(dd, timeout_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *calrow = settings_row(s_scr_settings, 194, "Touch", "Recalibrate touchscreen");
    lv_obj_t *calbtn = lv_btn_create(calrow);
    lv_obj_set_size(calbtn, 110, 30);
    lv_obj_align(calbtn, LV_ALIGN_RIGHT_MID, -10, 0);
    style_button(calbtn, COLOUR_SURFACE2, COLOUR_WARN);
    lv_obj_add_event_cb(calbtn, settings_cal_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *callbl = lv_label_create(calbtn);
    lv_label_set_text(callbl, "Calibrate");
    lv_obj_set_style_text_color(callbl, COLOUR_WARN, LV_PART_MAIN);
    lv_obj_center(callbl);
    (void)calrow;

}

/* -------------------------------------------------------------------------- */
/* B.P. screen                                                                */
/* -------------------------------------------------------------------------- */

/*
 * Helper to start or stop the BP sampler task.
 * Called under the LVGL mutex so s_bp_sampler_task is only touched from the
 * LVGL task; the task itself clears the handle when it exits.
 */
static void bp_start_recording(void)
{
    if (svc_bp_rec_start(s_bp_chosen_dur_s) != ESP_OK) return;

    if (!s_bp_sampler_task) {
        xTaskCreatePinnedToCore(bp_sampler_task, "bp_sampler",
                                4096, NULL, BP_SAMPLER_PRIORITY,
                                &s_bp_sampler_task, BP_CORE_SAMPLER);
    }
}

static void bp_analysis_task(void *arg)
{
    (void)arg;
    static bp_analysis_t result;
    if (bp_analyse_file(svc_bp_rec_get_filename(), &result) == ESP_OK
            && result.valid) {
        s_bp_last_result    = result;
        s_bp_analysis_ready = true;
    }
    vTaskDelete(NULL);
}

static void bp_trigger_analysis(void)
{
    s_bp_analysis_ready = false;
    xTaskCreate(bp_analysis_task, "bp_analysis",
                BP_ANALYSIS_STACK_BYTES, NULL, 3, NULL);
}

static void bp_build_chart(void)
{
    if (s_bp_chart || !s_bp_chart_card) return;

    s_bp_chart = lv_chart_create(s_bp_chart_card);
    lv_obj_set_size(s_bp_chart, LCD_H_RES, 48);
    lv_obj_align(s_bp_chart, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_bp_chart, COLOUR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bp_chart, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bp_chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_bp_chart, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bp_chart, 0, LV_PART_MAIN);
    lv_chart_set_type(s_bp_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_bp_chart, 64);
    lv_chart_set_range(s_bp_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1500000);
    lv_chart_set_div_line_count(s_bp_chart, 0, 0);
    lv_chart_set_update_mode(s_bp_chart, LV_CHART_UPDATE_MODE_CIRCULAR);
    lv_obj_set_style_size(s_bp_chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(s_bp_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_opa(s_bp_chart, LV_OPA_COVER, LV_PART_ITEMS);

    s_bp_rr_series = lv_chart_add_series(s_bp_chart,
                                          COLOUR_ECG, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_series_ext_y_array(s_bp_chart, s_bp_rr_series, s_bp_chart_rr);

    s_bp_pat_series = lv_chart_add_series(s_bp_chart,
                                           COLOUR_PPG, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_series_ext_y_array(s_bp_chart, s_bp_pat_series, s_bp_chart_pat);
}

static void bp_destroy_chart(void)
{
    if (!s_bp_chart) return;
    lv_obj_del(s_bp_chart);
    s_bp_chart      = NULL;
    s_bp_rr_series  = NULL;
    s_bp_pat_series = NULL;
}

static void bp_dur_btn_cb(lv_event_t *e)
{
    reset_activity();
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    static const uint32_t durs[3] = {
        BP_DURATION_30S, BP_DURATION_60S, BP_DURATION_120S
    };
    if (idx < 0 || idx > 2) return;
    s_bp_chosen_dur_s = durs[idx];

    /* Tapping a duration button starts recording immediately.
     * Hide the three duration buttons and show the STOP button. */
    bp_start_recording();
    if (!svc_bp_rec_is_recording()) {
        if (s_lbl_bp_status)
            lv_label_set_text(s_lbl_bp_status, "Error: SD write failed");
        return;
    }
    if (s_btn_bp_start) {
        lv_obj_clear_flag(s_btn_bp_start, LV_OBJ_FLAG_HIDDEN);
        style_button(s_btn_bp_start, COLOUR_ERROR, COLOUR_ERROR);
    }
    for (int i = 0; i < 3; i++)
        if (s_btn_bp_dur[i])
            lv_obj_add_flag(s_btn_bp_dur[i], LV_OBJ_FLAG_HIDDEN);
    if (s_lbl_bp_status)
        lv_label_set_text(s_lbl_bp_status, "Recording...");
    if (s_lbl_bp_hrv)   lv_label_set_text(s_lbl_bp_hrv,      "HRV RMSSD: --");
    if (s_lbl_bp_pat_stat) lv_label_set_text(s_lbl_bp_pat_stat, "PAT: --  var: --");
    bp_destroy_chart();
}

/* bp_startstop_btn_cb: STOP only — the STOP button is only visible while
 * recording.  Recording is started by bp_dur_btn_cb. */
static void bp_startstop_btn_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();
    if (!svc_bp_rec_is_recording()) return;  /* guard */

    svc_bp_rec_stop();
    s_bp_was_recording = false;
    if (s_bp_sampler_task) s_bp_sampler_task = NULL;

    /* Hide STOP button, show duration buttons */
    if (s_btn_bp_start) lv_obj_add_flag(s_btn_bp_start, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 3; i++)
        if (s_btn_bp_dur[i]) lv_obj_clear_flag(s_btn_bp_dur[i], LV_OBJ_FLAG_HIDDEN);

    if (s_lbl_bp_status)
        lv_label_set_text(s_lbl_bp_status, "Analysing...");
    bp_trigger_analysis();
}

static void bp_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
#if LVGL_VERSION_MAJOR >= 9
    if (lv_screen_active() != s_scr_bp) return;
#else
    if (lv_scr_act() != s_scr_bp) return;
#endif

    bool currently_recording = svc_bp_rec_is_recording();

    if (currently_recording) {
        s_bp_was_recording = true;

        uint32_t elapsed_s = (uint32_t)(
            (esp_timer_get_time() / 1000ULL - svc_bp_rec_get_start_ms()) / 1000UL);
        uint32_t remain_s = (elapsed_s < s_bp_chosen_dur_s)
                            ? (s_bp_chosen_dur_s - elapsed_s) : 0;

        if (s_lbl_bp_status) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Recording  %lu:%02lu",
                     (unsigned long)(remain_s / 60),
                     (unsigned long)(remain_s % 60));
            lv_label_set_text(s_lbl_bp_status, buf);
        }

        /* Low battery auto-stop */
        int batt = s_cached_batt_pct;
        if (batt >= 0 && batt <= BP_BATT_STOP_PCT) {
            svc_bp_rec_stop();
            s_bp_sampler_task = NULL;
            currently_recording = false;
        }
        /* Duration auto-stop */
        else if (remain_s == 0) {
            svc_bp_rec_stop();
            s_bp_sampler_task = NULL;
            currently_recording = false;
        }
        else {
            return; /* still recording */
        }
    }

    /* Transition: recording just finished (timer-driven or writer-driven auto-stop) */
    if (s_bp_was_recording && !currently_recording) {
        s_bp_was_recording = false;
        /* Hide STOP button, restore duration selector */
        if (s_btn_bp_start) lv_obj_add_flag(s_btn_bp_start, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 3; i++)
            if (s_btn_bp_dur[i])
                lv_obj_clear_flag(s_btn_bp_dur[i], LV_OBJ_FLAG_HIDDEN);
        if (s_lbl_bp_status)
            lv_label_set_text(s_lbl_bp_status, "Analysing...");
        bp_trigger_analysis();
    }

    /* Poll for analysis results */
    if (s_bp_analysis_ready) {
        s_bp_analysis_ready = false;
        uint32_t n = s_bp_last_result.beat_count;

        char buf[64];
        snprintf(buf, sizeof(buf), "HRV RMSSD: %.1f us  (%lu beats)",
                 (double)s_bp_last_result.hrv_rmssd_us, (unsigned long)n);
        if (s_lbl_bp_hrv) lv_label_set_text(s_lbl_bp_hrv, buf);

        snprintf(buf, sizeof(buf), "PAT: %.0f us  var: %.1f us^2",
                 (double)s_bp_last_result.pat_mean_us,
                 (double)s_bp_last_result.pat_variance_us2);
        if (s_lbl_bp_pat_stat) lv_label_set_text(s_lbl_bp_pat_stat, buf);

        /* Populate chart arrays */
        uint32_t cnt = (n > 64) ? 64 : n;
        for (uint32_t i = 0; i < cnt; i++) {
            s_bp_chart_rr[i]  = (lv_coord_t)s_bp_last_result.rr_series[i];
            s_bp_chart_pat[i] = (lv_coord_t)s_bp_last_result.pat_series[i];
        }

        bp_build_chart();
        if (s_bp_chart) lv_chart_refresh(s_bp_chart);

        const char *fname = svc_bp_rec_get_filename();
        /* show just the basename to save label space */
        const char *base = fname;
        for (const char *p = fname; *p; p++)
            if (*p == '/') base = p + 1;
        snprintf(buf, sizeof(buf), "Saved: %s", base);
        if (s_lbl_bp_status) lv_label_set_text(s_lbl_bp_status, buf);
    }
}

/* ========================================================================== */
/* Files screen                                                               */
/* Layout (320 × 240 px):                                                     */
/*   y= 12   Title "Files"                                                    */
/*   y= 38   Status label (SD / Wi-Fi state, selected file)                  */
/*   y= 54   [Refresh 148×36]  [Connect WiFi 106×36]                        */
/*   y= 96   [Send 106×36]  [Delete 106×36]                                 */
/*   y=136   Scrollable file list (296×74)                                   */
/*   bottom  Transfer progress / filename labels                              */
/* ========================================================================== */

static void files_rebuild_list(void);

static void files_wifi_shutdown_task(void *arg)
{
    (void)arg;
    wifi_shutdown_after_time_sync();
    vTaskDelete(NULL);
}

static void files_row_btn_cb(lv_event_t *e)
{
    reset_activity();
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)s_files_model.count) return;

    /* Deselect previous row */
    if (s_files_selected >= 0 && s_files_selected < (int)s_files_model.count
            && s_files_row_btns[s_files_selected]) {
        style_button(s_files_row_btns[s_files_selected],
                     COLOUR_SURFACE2, COLOUR_SUBTEXT);
    }

    s_files_selected = idx;
    if (s_files_row_btns[idx])
        style_button(s_files_row_btns[idx], COLOUR_SURFACE2, COLOUR_ACCENT);

    char buf[80];
    snprintf(buf, sizeof(buf), "Selected: %s", s_files_model.items[idx].name);
    lv_label_set_text(s_lbl_files_status, buf);
    lv_obj_set_style_text_color(s_lbl_files_status, COLOUR_TEXT, LV_PART_MAIN);
}

static void files_rebuild_list(void)
{
    lv_obj_clean(s_files_list_cont);
    memset(s_files_row_btns, 0, sizeof(s_files_row_btns));

    if (!s_files_model.sd_ok) {
        lv_obj_t *msg = lv_label_create(s_files_list_cont);
        lv_label_set_text(msg, "SD card not mounted");
        lv_obj_set_style_text_color(msg, COLOUR_SUBTEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, LV_PART_MAIN);
        return;
    }
    if (s_files_model.count == 0) {
        lv_obj_t *msg = lv_label_create(s_files_list_cont);
        lv_label_set_text(msg, "No files on card");
        lv_obj_set_style_text_color(msg, COLOUR_SUBTEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, LV_PART_MAIN);
        return;
    }

    for (uint16_t i = 0; i < s_files_model.count; i++) {
        const watch_file_entry_t *fe = &s_files_model.items[i];

        lv_obj_t *row = lv_btn_create(s_files_list_cont);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        style_button(row, COLOUR_SURFACE2, COLOUR_SUBTEXT);
        lv_obj_set_style_pad_all(row, 6, LV_PART_MAIN);
        lv_obj_add_event_cb(row, files_row_btn_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_files_row_btns[i] = row;

        lv_obj_t *lbl_name = lv_label_create(row);
        lv_label_set_text(lbl_name, fe->name);
        lv_obj_set_style_text_color(lbl_name, COLOUR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(lbl_name, lv_pct(100));
        lv_obj_align(lbl_name, LV_ALIGN_TOP_LEFT, 0, 0);

        const char *kind_str = (fe->kind == FILE_KIND_BP)     ? "[BP]"  :
                               (fe->kind == FILE_KIND_RECORD) ? "[REC]" : "[?]";
        char meta[40];
        snprintf(meta, sizeof(meta), "%.1f KB  %s",
                 (double)(fe->size_bytes / 1024.0f), kind_str);

        lv_obj_t *lbl_meta = lv_label_create(row);
        lv_label_set_text(lbl_meta, meta);
        lv_obj_set_style_text_color(lbl_meta, COLOUR_SUBTEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl_meta, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align_to(lbl_meta, lbl_name, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
    }
}

static void files_refresh_btn_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();
    s_files_selected = -1;
    lv_label_set_text(s_lbl_files_status, "Loading...");
    lv_obj_set_style_text_color(s_lbl_files_status, COLOUR_SUBTEXT, LV_PART_MAIN);

    svc_files_refresh_list(&s_files_model);
    files_rebuild_list();

    char buf[48];
    if (s_files_model.sd_ok) {
        snprintf(buf, sizeof(buf), "SD: %u file%s",
                 (unsigned)s_files_model.count,
                 s_files_model.count == 1 ? "" : "s");
        lv_obj_set_style_text_color(s_lbl_files_status, COLOUR_SUCCESS, LV_PART_MAIN);
    } else {
        snprintf(buf, sizeof(buf), "SD mount failed");
        lv_obj_set_style_text_color(s_lbl_files_status, COLOUR_ERROR, LV_PART_MAIN);
    }
    lv_label_set_text(s_lbl_files_status, buf);
}

static void files_wifi_btn_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();
    s_wifi_opened_from_settings = false;
    s_wifi_opened_from_files    = true;
    nav_to_wifi_locked(NULL);
    xTaskCreate(wifi_scan_task, "wifi_scan", 6144, NULL, 5, NULL);
}

static void files_send_btn_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();
    if (s_files_selected < 0 || s_files_selected >= (int)s_files_model.count) return;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        lv_label_set_text(s_lbl_files_xfer, "WiFi not connected");
        lv_obj_set_style_text_color(s_lbl_files_xfer, COLOUR_ERROR, LV_PART_MAIN);
        return;
    }
    if (svc_files_is_busy()) return;

    lv_label_set_text(s_lbl_files_xfer, "Starting upload...");
    lv_obj_set_style_text_color(s_lbl_files_xfer, COLOUR_WARN, LV_PART_MAIN);
    lv_label_set_text(s_lbl_files_detail, "");
    svc_files_start_send_task(&s_files_model.items[s_files_selected]);
}

static void files_delete_btn_cb(lv_event_t *e)
{
    (void)e;
    reset_activity();
    if (s_files_selected < 0 || s_files_selected >= (int)s_files_model.count) return;
    if (svc_files_is_busy()) return;

    esp_err_t err = svc_files_delete_file(&s_files_model.items[s_files_selected]);
    if (err == ESP_OK) {
        lv_label_set_text(s_lbl_files_xfer, "File deleted");
        lv_obj_set_style_text_color(s_lbl_files_xfer, COLOUR_SUCCESS, LV_PART_MAIN);
    } else {
        lv_label_set_text(s_lbl_files_xfer, "Delete failed");
        lv_obj_set_style_text_color(s_lbl_files_xfer, COLOUR_ERROR, LV_PART_MAIN);
    }
    lv_label_set_text(s_lbl_files_detail, "");

    /* Refresh list to reflect the deletion */
    s_files_selected = -1;
    svc_files_refresh_list(&s_files_model);
    files_rebuild_list();

    char buf[48];
    if (s_files_model.sd_ok) {
        snprintf(buf, sizeof(buf), "SD: %u file%s",
                 (unsigned)s_files_model.count,
                 s_files_model.count == 1 ? "" : "s");
        lv_obj_set_style_text_color(s_lbl_files_status, COLOUR_SUBTEXT, LV_PART_MAIN);
        lv_label_set_text(s_lbl_files_status, buf);
    }
}

static void files_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* Detect navigation away from the Files screen and shut down Wi-Fi if
     * we held it alive for file upload.  The timer keeps running after the
     * screen is unloaded, so this fires within one 250 ms tick. */
    lv_obj_t *active_scr =
#if LVGL_VERSION_MAJOR >= 9
        lv_screen_active();
#else
        lv_scr_act();
#endif
    if (active_scr != s_scr_files) {
        /* Only shut down Wi-Fi when leaving Files — not during the incoming
         * 240 ms screen-load animation where lv_screen_active() still returns
         * the previous screen. */
        if (s_files_was_active && s_files_wifi_kept_alive) {
            s_files_wifi_kept_alive = false;
            xTaskCreate(files_wifi_shutdown_task, "files_wifi_stop",
                        4096, NULL, 5, NULL);
        }
        s_files_was_active = false;
        return;
    }
    if (!s_files_was_active) {
        ESP_LOGI(TAG, "Files screen active: wifi_connected=%d kept_alive=%d sel=%d",
                 (int)s_wifi_connected, (int)s_files_wifi_kept_alive,
                 s_files_selected);
    }
    s_files_was_active = true;

    /* Robust Wi-Fi liveness check: trust the actual driver state over the
     * cached s_wifi_connected flag, which can be stale if a transient
     * disconnect event fired between connection and the user arriving on
     * Files (e.g. the popup/delay window in wifi_connect_task). */
    wifi_ap_record_t ap_info;
    bool wifi_live = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
    if (wifi_live && !s_wifi_connected) {
        ESP_LOGI(TAG, "Files: resyncing s_wifi_connected (was false, AP reachable)");
        s_wifi_connected = true;
    }

    /* Reflect Wi-Fi state in the Connect WiFi button label */
    if (s_lbl_files_wifi) {
        lv_label_set_text(s_lbl_files_wifi,
                          wifi_live ? "WiFi: connected" : "Connect WiFi");
    }

    /* Enable / disable action buttons */
    bool busy       = svc_files_is_busy();
    bool sel_valid  = (s_files_selected >= 0
                       && s_files_selected < (int)s_files_model.count);
    bool can_send   = sel_valid && wifi_live && !busy;
    bool can_delete = sel_valid && !busy;

    if (can_send)
        lv_obj_remove_state(s_btn_files_send, LV_STATE_DISABLED);
    else
        lv_obj_add_state(s_btn_files_send, LV_STATE_DISABLED);

    if (can_delete)
        lv_obj_remove_state(s_btn_files_delete, LV_STATE_DISABLED);
    else
        lv_obj_add_state(s_btn_files_delete, LV_STATE_DISABLED);

    /* Poll upload status */
    const file_tx_status_t *tx = svc_files_get_tx_status();
    if (tx->active) {
        char buf[72];
        if (tx->bytes_total > 0) {
            snprintf(buf, sizeof(buf), "Sending: %.1f / %.1f KB",
                     (double)(tx->bytes_sent  / 1024.0f),
                     (double)(tx->bytes_total / 1024.0f));
        } else {
            snprintf(buf, sizeof(buf), "Sending...");
        }
        lv_label_set_text(s_lbl_files_xfer, buf);
        lv_obj_set_style_text_color(s_lbl_files_xfer, COLOUR_WARN, LV_PART_MAIN);
        lv_label_set_text(s_lbl_files_detail, tx->filename);
        lv_obj_set_style_text_color(s_lbl_files_detail, COLOUR_SUBTEXT, LV_PART_MAIN);
    } else if (tx->done) {
        lv_label_set_text(s_lbl_files_xfer, tx->message);
        lv_obj_set_style_text_color(s_lbl_files_xfer,
                                    tx->success ? COLOUR_SUCCESS : COLOUR_ERROR,
                                    LV_PART_MAIN);
    }
}

static void ui_create_files_screen(void)
{
    s_scr_files = lv_obj_create(NULL);
    style_screen(s_scr_files);

    /* ── Title ──────────────────────────────────────────────────── */
    lv_obj_t *title = lv_label_create(s_scr_files);
    lv_label_set_text(title, "Files");
    lv_obj_set_style_text_color(title, COLOUR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    /* ── Status label ───────────────────────────────────────────── */
    s_lbl_files_status = lv_label_create(s_scr_files);
    lv_label_set_text(s_lbl_files_status, "Press Refresh to load files");
    lv_obj_set_style_text_color(s_lbl_files_status, COLOUR_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_files_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(s_lbl_files_status, 8, 38);

    /* ── Button row 1: Refresh | Connect WiFi ───────────────────── */
    lv_obj_t *btn_refresh = lv_btn_create(s_scr_files);
    lv_obj_set_size(btn_refresh, 148, 36);
    lv_obj_set_pos(btn_refresh, 10, 54);
    style_button(btn_refresh, COLOUR_SURFACE2, COLOUR_ACCENT);
    lv_obj_add_event_cb(btn_refresh, files_refresh_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_refresh = lv_label_create(btn_refresh);
    lv_label_set_text(lbl_refresh, "Refresh");
    lv_obj_set_style_text_color(lbl_refresh, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_refresh, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(lbl_refresh);

    lv_obj_t *btn_wifi = lv_btn_create(s_scr_files);
    lv_obj_set_size(btn_wifi, 106, 36);
    lv_obj_set_pos(btn_wifi, 168, 54);
    style_button(btn_wifi, COLOUR_SURFACE2, COLOUR_SUBTEXT);
    lv_obj_add_event_cb(btn_wifi, files_wifi_btn_cb, LV_EVENT_CLICKED, NULL);

    s_lbl_files_wifi = lv_label_create(btn_wifi);
    lv_label_set_text(s_lbl_files_wifi, "Connect WiFi");
    lv_obj_set_style_text_color(s_lbl_files_wifi, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_files_wifi, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(s_lbl_files_wifi);

    /* ── Button row 2: Send | Delete ────────────────────────────── */
    s_btn_files_send = lv_btn_create(s_scr_files);
    lv_obj_set_size(s_btn_files_send, 106, 36);
    lv_obj_set_pos(s_btn_files_send, 10, 98);
    style_button(s_btn_files_send, COLOUR_SURFACE2, COLOUR_SUCCESS);
    lv_obj_add_event_cb(s_btn_files_send, files_send_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(s_btn_files_send, LV_STATE_DISABLED);

    lv_obj_t *lbl_send = lv_label_create(s_btn_files_send);
    lv_label_set_text(lbl_send, "Send");
    lv_obj_set_style_text_color(lbl_send, COLOUR_SUCCESS, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_send, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(lbl_send);

    s_btn_files_delete = lv_btn_create(s_scr_files);
    lv_obj_set_size(s_btn_files_delete, 106, 36);
    lv_obj_set_pos(s_btn_files_delete, 168, 98);
    style_button(s_btn_files_delete, COLOUR_SURFACE2, COLOUR_ERROR);
    lv_obj_add_event_cb(s_btn_files_delete, files_delete_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(s_btn_files_delete, LV_STATE_DISABLED);

    lv_obj_t *lbl_del = lv_label_create(s_btn_files_delete);
    lv_label_set_text(lbl_del, "Delete");
    lv_obj_set_style_text_color(lbl_del, COLOUR_ERROR, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_del, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(lbl_del);

    /* ── File list container (scrollable) ───────────────────────── */
    s_files_list_cont = lv_obj_create(s_scr_files);
    lv_obj_set_size(s_files_list_cont, 296, 74);
    lv_obj_align(s_files_list_cont, LV_ALIGN_TOP_MID, 0, 136);
    lv_obj_set_style_bg_color(s_files_list_cont, COLOUR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_files_list_cont, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_files_list_cont, COLOUR_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_files_list_cont, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_files_list_cont, 14, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_files_list_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_files_list_cont, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_files_list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_files_list_cont,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_files_list_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_files_list_cont, LV_SCROLLBAR_MODE_AUTO);

    /* Placeholder text until Refresh is pressed */
    lv_obj_t *placeholder = lv_label_create(s_files_list_cont);
    lv_label_set_text(placeholder, "Press Refresh to load");
    lv_obj_set_style_text_color(placeholder, COLOUR_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(placeholder, &lv_font_montserrat_14, LV_PART_MAIN);

    /* ── Transfer status ────────────────────────────────────────── */
    s_lbl_files_xfer = lv_label_create(s_scr_files);
    lv_label_set_text(s_lbl_files_xfer, "");
    lv_obj_set_style_text_color(s_lbl_files_xfer, COLOUR_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_files_xfer, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_lbl_files_xfer, LV_ALIGN_BOTTOM_MID, 0, -22);

    s_lbl_files_detail = lv_label_create(s_scr_files);
    lv_label_set_text(s_lbl_files_detail, "");
    lv_obj_set_style_text_color(s_lbl_files_detail, COLOUR_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_files_detail, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_lbl_files_detail, LV_ALIGN_BOTTOM_MID, 0, -6);

    /* ── UI timer (250 ms) ──────────────────────────────────────── */
    if (!s_files_ui_timer)
        s_files_ui_timer = lv_timer_create(files_ui_timer_cb, 250, NULL);
}

/*
 * Layout (320 × 240 px):
 *   y= 12   Title
 *   y= 36   Status label
 *   y= 56   Duration buttons (3 × 86 px) / Countdown (hidden/shown)
 *   y= 88   START/STOP button 290×40
 *   y=130   Results card 290×52
 *   y=184   Chart card 320×52 (lazy — built after analysis)
 *   bottom  hint
 */
static void ui_create_bp_screen(void)
{
    s_scr_bp   = lv_obj_create(NULL);
    s_scr_pump = s_scr_bp;   /* home_btn_event holds s_scr_pump as user_data */
    style_screen(s_scr_bp);

    /* ── Title ──────────────────────────────────────────────────── */
    lv_obj_t *title = lv_label_create(s_scr_bp);
    lv_label_set_text(title, "B.P. Measurement");
    lv_obj_set_style_text_color(title, COLOUR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    /* ── Status label ───────────────────────────────────────────── */
    s_lbl_bp_status = lv_label_create(s_scr_bp);
    lv_label_set_text(s_lbl_bp_status, "Tap duration to start");
    lv_obj_set_style_text_color(s_lbl_bp_status, COLOUR_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_bp_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(s_lbl_bp_status, 8, 36);

    /* ── Duration selector (tap = start recording) ──────────────── */
    static const char *dur_labels[3] = {"30 s", "1 min", "2 min"};
    int dur_x[3] = {10, 116, 222};
    for (int i = 0; i < 3; i++) {
        s_btn_bp_dur[i] = lv_btn_create(s_scr_bp);
        lv_obj_set_size(s_btn_bp_dur[i], 86, 38);
        lv_obj_set_pos(s_btn_bp_dur[i], dur_x[i], 56);
        style_button(s_btn_bp_dur[i], COLOUR_SURFACE2,
                     (i == 1) ? COLOUR_ACCENT : COLOUR_SUBTEXT);

        lv_obj_t *lbl = lv_label_create(s_btn_bp_dur[i]);
        lv_label_set_text(lbl, dur_labels[i]);
        lv_obj_set_style_text_color(lbl, COLOUR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, LV_PART_MAIN);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(s_btn_bp_dur[i], bp_dur_btn_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    /* ── STOP button — occupies same row as duration buttons, shown only
     *    when recording is active; duration buttons hidden while it shows. */
    s_btn_bp_start = lv_btn_create(s_scr_bp);
    lv_obj_set_size(s_btn_bp_start, 290, 38);
    lv_obj_set_pos(s_btn_bp_start, 15, 56);
    style_button(s_btn_bp_start, COLOUR_ERROR, COLOUR_ERROR);
    lv_obj_add_flag(s_btn_bp_start, LV_OBJ_FLAG_HIDDEN);   /* shown when recording */
    lv_obj_add_event_cb(s_btn_bp_start, bp_startstop_btn_cb,
                        LV_EVENT_CLICKED, NULL);

    s_lbl_bp_btn = lv_label_create(s_btn_bp_start);
    lv_label_set_text(s_lbl_bp_btn, LV_SYMBOL_STOP "  STOP");
    lv_obj_set_style_text_color(s_lbl_bp_btn, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_bp_btn, &lv_font_montserrat_18,
                                LV_PART_MAIN);
    lv_obj_center(s_lbl_bp_btn);

    /* Countdown label — no longer displayed; kept as NULL-safe guard for
     * any existing code paths that reference s_lbl_bp_countdown. */
    s_lbl_bp_countdown = NULL;

    /* ── Results card (compact — moved up by the removed START button) */
    lv_obj_t *res_card = lv_obj_create(s_scr_bp);
    lv_obj_set_size(res_card, 290, 68);
    lv_obj_align(res_card, LV_ALIGN_TOP_MID, 0, 98);
    style_card(res_card, 10);
    lv_obj_clear_flag(res_card, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_bp_hrv = lv_label_create(res_card);
    lv_label_set_text(s_lbl_bp_hrv, "HRV RMSSD: --");
    lv_obj_set_style_text_color(s_lbl_bp_hrv, COLOUR_ECG, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_bp_hrv, &lv_font_montserrat_14,
                                LV_PART_MAIN);
    lv_obj_set_pos(s_lbl_bp_hrv, 8, 2);

    s_lbl_bp_pat_stat = lv_label_create(res_card);
    lv_label_set_text(s_lbl_bp_pat_stat, "PAT: --   var: --");
    lv_obj_set_style_text_color(s_lbl_bp_pat_stat, COLOUR_PPG, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_bp_pat_stat, &lv_font_montserrat_14,
                                LV_PART_MAIN);
    lv_obj_set_pos(s_lbl_bp_pat_stat, 8, 18);

    /* ── Chart card (moved up by the removed START button) ──────── */
    s_bp_chart_card = lv_obj_create(s_scr_bp);
    lv_obj_set_size(s_bp_chart_card, LCD_H_RES, 52);
    lv_obj_align(s_bp_chart_card, LV_ALIGN_TOP_MID, 0, 168);
    style_card(s_bp_chart_card, 0);
    lv_obj_clear_flag(s_bp_chart_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_bp_chart_card, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bp_chart_card, 0, LV_PART_MAIN);

    /* ── Bottom hint ────────────────────────────────────────────── */
    lv_obj_t *hint = lv_label_create(s_scr_bp);
    lv_label_set_text(hint, "RR green  |  PAT orange");
    lv_obj_set_style_text_color(hint, COLOUR_SUBTEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    /* ── UI timer (1 Hz) ────────────────────────────────────────── */
    if (!s_bp_ui_timer)
        s_bp_ui_timer = lv_timer_create(bp_ui_timer_cb, 1000, NULL);
}

static void ui_create_app_screens(void)
{
    ui_create_settings_screen();
    ui_create_record_screen();   /* was ui_create_health_screen() */
    ui_create_bp_screen();
    ui_create_files_screen();
}

/* -------------------------------------------------------------------------- */
/* App main                                                                   */
/* -------------------------------------------------------------------------- */

void app_main(void)
{

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    svc_time_restore_from_nvs();

    battery_adc_init();
    ecg_adc_init();
    ppg_sim_init();

    hal_backlight_init();
    hal_display_init();
    hal_touch_init();
    hal_backlight_set_percent(s_brightness);

    /* Load touch calibration from NVS (written by the calibration screen).
     * Falls back to hard-coded defaults in hal_touch.c if not yet calibrated. */
    bool cal_found = load_touch_cal_nvs();

    reset_activity();

    if (hal_display_lock_ms(0)) {
#if ORIENTATION_TEST
        /* ── Orientation test — four coloured quadrants + USB label ─────────
         * Set ORIENTATION_TEST 0 in app_config.h to boot normally.
         * Report: which colour is top-left, which edge shows "v USB v".      */
        lv_obj_t *tscr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(tscr, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tscr, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_scrollbar_mode(tscr, LV_SCROLLBAR_MODE_OFF);

        /* Helper macro: coloured quadrant */
        #define MK_Q(parent, x, y, w, h, col, txt) do { \
            lv_obj_t *_q = lv_obj_create(parent); \
            lv_obj_set_size(_q, w, h); lv_obj_set_pos(_q, x, y); \
            lv_obj_set_style_bg_color(_q, lv_color_hex(col), LV_PART_MAIN); \
            lv_obj_set_style_bg_opa(_q, LV_OPA_COVER, LV_PART_MAIN); \
            lv_obj_set_style_border_width(_q, 0, LV_PART_MAIN); \
            lv_obj_clear_flag(_q, LV_OBJ_FLAG_SCROLLABLE); \
            lv_obj_t *_l = lv_label_create(_q); \
            lv_label_set_text(_l, txt); \
            lv_obj_set_style_text_color(_l, lv_color_hex(0xFFFFFF), LV_PART_MAIN); \
            lv_obj_center(_l); \
        } while(0)

        int hw = LCD_H_RES / 2, hh = LCD_V_RES / 2;
        MK_Q(tscr,  0,  0, hw, hh, 0xCC0000, "TOP-LEFT\nRED");
        MK_Q(tscr, hw,  0, hw, hh, 0x007700, "TOP-RIGHT\nGREEN");
        MK_Q(tscr,  0, hh, hw, hh, 0x0000CC, "BOT-LEFT\nBLUE");
        MK_Q(tscr, hw, hh, hw, hh, 0xAA8800, "BOT-RIGHT\nYELLOW");

        /* USB label — physically the USB connector is on the BOTTOM short edge */
        lv_obj_t *usb_lbl = lv_label_create(tscr);
        lv_label_set_text(usb_lbl, "v  USB  v");
        lv_obj_set_style_text_color(usb_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(usb_lbl, &lv_font_montserrat_18, LV_PART_MAIN);
        lv_obj_align(usb_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);

        lv_screen_load(tscr);
#else
        ui_create_wifi_screen();
        ui_create_password_screen();
        ui_create_connecting_screen();
        ui_create_app_screens();
        ui_create_settings_screen();
        ui_create_home_screen();
        update_home_time_labels();
        health_update_topbar();

        if (!cal_found) {
            /* First boot — no calibration in NVS.  Show calibration screen;
             * it navigates to home when done. */
            run_touch_calibration();
        } else {
            lv_screen_load(s_scr_wifi);
        }
#endif
        hal_display_unlock();
    }

    wifi_driver_init();

    /*
     * Mount the SD card (SPI mode via sdspi_host, SPI3/VSPI, CS=GPIO5).
     * Failure is non-fatal — hal_storage_mount() will be retried lazily the
     * first time the user starts a recording.
     */
    if (hal_storage_mount() != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available at boot — will retry when recording starts");
    }

    svc_rec_init();
    svc_bp_rec_init();

    xTaskCreatePinnedToCore(clock_update_task, "clock_update", 4096, NULL, 4, &s_clock_task, UI_AUX_CORE);
    xTaskCreatePinnedToCore(boot_btn_task, "boot_btn", 3072, NULL, 4, NULL, UI_AUX_CORE);
    xTaskCreatePinnedToCore(ecg_sampler_task, "ecg_sampler", 4096, NULL, 8, &s_ecg_task, ECG_CORE_SAMPLER);
    xTaskCreate(wifi_scan_task, "wifi_scan", 6144, NULL, 5, NULL);

}