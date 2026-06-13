#pragma once

#include "driver/gpio.h"

/* -------------------------------------------------------------------------- */
/* Display — ILI9341 SPI TFT, native 240×320, run in landscape 320×240        */
/* Orientation set by writing a full MADCTL byte (0x36) directly after init.  */
/* -------------------------------------------------------------------------- */

/* Landscape 320×240 for 2432S024 (2.4" board). MADCTL=0x40 confirmed working. */
#define LCD_H_RES               320
#define LCD_V_RES               240

/* Physical panel GRAM dimensions (same as above in portrait). */
#define LCD_NATIVE_W            240
#define LCD_NATIVE_H            320

/* MADCTL written to register 0x36 after panel init.  No MV bit — native
 * portrait addressing so LVGL row-major pixel stream matches GRAM layout.
 * BGR bit (0x08) CLEAR — RGB + byte-swap confirmed correct.
 *   0x00 = native portrait (row top→bottom, col left→right)
 *   0x40 = MX — mirrors left/right (try if image appears horizontally flipped) */
#define LCD_MADCTL              0x40

/* Set to 1 to boot into the orientation test screen (four coloured quadrants).
 * Set to 0 to run the normal firmware. */
#define ORIENTATION_TEST        0

/* -------------------------------------------------------------------------- */
/* Pin definitions — TZT ESP32-2432S024C (capacitive touch variant)          */
/*                                                                            */
/* WARNING: GPIO21 is touch INT on the capacitive variant, NOT backlight.    */
/* The resistive variant uses GPIO21 for backlight — do not follow the       */
/* Getting Started PDF which shows TFT_BL=21 (that is for the resistive SKU).*/
/* -------------------------------------------------------------------------- */

/* Display SPI (SPI2 / HSPI) */
#define PIN_LCD_MOSI   GPIO_NUM_13
#define PIN_LCD_MISO   GPIO_NUM_12
#define PIN_LCD_SCLK   GPIO_NUM_14   /* was ECG ADC on source board — now display clock */
#define PIN_LCD_CS     GPIO_NUM_15
#define PIN_LCD_DC     GPIO_NUM_2
/* No hardware reset pin on this board */

/* Backlight — LEDC PWM (capacitive variant) */
#define PIN_LCD_BL     GPIO_NUM_27

/* Touch — XPT2046 resistive, SPI2 (shared with display).
 * The 2432S024 board is the resistive variant; GPIO33 is SPI CS, not I2C SDA. */

/* GPIO35 routes to external expansion connector P3, NOT to a battery sense
 * node.  The IP5306 charger IC has no I2C path to the ESP32 on this board.
 * Battery monitoring is unavailable; battery_read_* returns -1 permanently. */
#define PIN_BAT_ADC    GPIO_NUM_35   /* unused — kept for reference only */

/* SD card SPI (SPI3 / VSPI) — conventional mapping, verify against schematic */
#define PIN_SD_SCLK    GPIO_NUM_18
#define PIN_SD_MISO    GPIO_NUM_19
#define PIN_SD_MOSI    GPIO_NUM_23
#define PIN_SD_CS      GPIO_NUM_5

/* SD card VFS mount point */
#define SD_MOUNT_POINT "/sdcard"

/* XPT2046 resistive touch — SPI2 (shared with display), CS=GPIO33.
 * The 2432S024 board is the resistive variant (XPT2046 SPI), NOT CST820 I2C.
 * GPIO33 is NOT I2C SDA on this board; it is the touch SPI CS.
 * PENIRQ (GPIO36) is NOT connected to the ESP32 on this board variant —
 * touch detection uses Z-pressure measurement only. */
#define PIN_TP_CS      GPIO_NUM_33   /* XPT2046 chip-select (active-low) */

/* -------------------------------------------------------------------------- */
/* WiFi / network                                                             */
/* -------------------------------------------------------------------------- */

#define MAX_SCAN_RESULTS        20
#define WIFI_CONNECT_TIMEOUT_S  15
#define SNTP_SERVER             "pool.ntp.org"
#define POSIX_TZ                "AEST-10AEDT,M10.1.0,M4.1.0/3"

/* -------------------------------------------------------------------------- */
/* Boot button                                                                */
/* -------------------------------------------------------------------------- */

#define BOOT_SHORT_PRESS_MAX_MS 500

/* -------------------------------------------------------------------------- */
/* Display / power management                                                 */
/* -------------------------------------------------------------------------- */

#define DEFAULT_BRIGHTNESS          50
#define DEFAULT_TIMEOUT_S           600

#define DISPLAY_LOCK_SLICE_MS       50
#define DISPLAY_LOCK_UI_TIMEOUT_MS  3000
#define DISPLAY_LOCK_SHORT_MS       1000

/* -------------------------------------------------------------------------- */
/* NVS key names                                                              */
/* -------------------------------------------------------------------------- */

#define WIFI_CRED_NAMESPACE     "wifi_creds"
#define WIFI_LAST_NAMESPACE     "wifi_last"
#define WIFI_LAST_SSID_KEY      "last_ssid"

#define NVS_TIME_NAMESPACE      "time_data"
#define NVS_TIME_KEY_LAST_SYNC  "last_sync"

/* -------------------------------------------------------------------------- */
/* Battery voltage thresholds                                                 */
/* -------------------------------------------------------------------------- */

#define BATTERY_VOLTAGE_EMPTY    3.30f
#define BATTERY_VOLTAGE_FULL     4.20f

/* -------------------------------------------------------------------------- */
/* ECG sampling                                                               */
/* -------------------------------------------------------------------------- */

#define ECG_SAMPLE_HZ            100
#define ECG_WINDOW_SECONDS       4
#define ECG_WINDOW_SAMPLES       (ECG_SAMPLE_HZ * ECG_WINDOW_SECONDS)
#define ECG_SAMPLE_PERIOD_MS     (1000 / ECG_SAMPLE_HZ)
#define ECG_UI_REFRESH_MS        50
#define ECG_PLOT_W               306   /* ~95 % of 320 landscape width */
#define ECG_PLOT_H                70   /* chart height in record screen (320×240 landscape) */
#define ECG_PLOT_POINTS          ECG_WINDOW_SAMPLES

/* Chart point count actually rendered. LVGL 9.5 chart hangs at 400 points
 * on this build; 200 is a safe ceiling. rec_update_plot() stride-subsamples
 * the 400-sample raw window down to this count. */
#define REC_CHART_POINTS         200

#define ECG_CORE_SAMPLER         0
#define UI_AUX_CORE              1

/* -------------------------------------------------------------------------- */
/* ECG detection                                                              */
/* -------------------------------------------------------------------------- */

#define ECG_MIN_BPM              35
#define ECG_MAX_BPM              220
#define ECG_REFACTORY_MS         250
#define ECG_THRESHOLD_MARGIN     140

#define ECG_QRS_BP_LOW_HZ        10.0f   /* raised from 5 Hz: attenuates P-wave (~2-8 Hz) */
#define ECG_QRS_BP_HIGH_HZ       15.0f
#define ECG_MWI_WINDOW_MS        150
#define ECG_MWI_SAMPLES          ((ECG_SAMPLE_HZ * ECG_MWI_WINDOW_MS) / 1000)
#define ECG_MIN_THRESHOLD_FLOOR  25.0f

/* Post-bandpass gain applied before the differentiate-square-MWI chain.
 * The ADS1293 24-bit values are shifted >> 12 to the pipeline's 12-bit range,
 * leaving only ±2–5 counts of AC variation on the pipeline signal.
 * Gain × 100 scales MWI by gain² so QRS complexes clear the initial 120.0 threshold.
 * The adaptive tracker self-calibrates after the first beat. */
#define ECG_QRS_GAIN             100.0f

/* R-peak search window around the MWI detection point.
 * After MWI fires, the sampler waits FORWARD_SAMPLES ticks so the true R-peak
 * can accumulate in the ring buffer (handles early detection on P-wave), then
 * searches BACKWARD and FORWARD samples on the bandpassed signal for the peak. */
#define ECG_RPEAK_FORWARD_SAMPLES 20   /* wait + forward search: 200 ms */
#define ECG_RPEAK_BACKWARD_SAMPLES 10  /* backward search: 100 ms */

/* -------------------------------------------------------------------------- */
/* Recording / SD card                                                        */
/* -------------------------------------------------------------------------- */

#define REC_CORE_WRITER          1
#define REC_SAMPLE_HZ            ECG_SAMPLE_HZ
#define REC_CIRC_BUF_SECONDS     2
#define REC_CIRC_BUF_ROWS        (REC_SAMPLE_HZ * REC_CIRC_BUF_SECONDS)
#define REC_WRITE_DELAY_US       500
#define REC_QUEUE_LEN            (REC_CIRC_BUF_ROWS * 2)
#define REC_BATT_STOP_PCT        5
/* Samples between battery percent reads for CSV and recording-stop check (~30 s) */
#define REC_BATT_UPDATE_SAMPLES  (ECG_SAMPLE_HZ * 30)
#define SD_MOUNT_RETRY_MAX       5
#define REC_LABEL_MAX            48
#define REC_FILENAME_MAX         128
#define REC_ROW_BUF              160
#define REC_SIM_AMP              420.0f
#define REC_SIM_CENTRE           500.0f

/* -------------------------------------------------------------------------- */
/* B.P. recording                                                             */
/* -------------------------------------------------------------------------- */

#define BP_SAMPLE_HZ              1000
#define BP_SAMPLE_PERIOD_US       (1000000 / BP_SAMPLE_HZ)
/* Core 1: keeps BP sampler off Core 0, away from ecg_sampler_task */
#define BP_CORE_SAMPLER           1
#define BP_CORE_WRITER            1
#define BP_SAMPLER_PRIORITY       6
#define BP_QUEUE_LEN              512    /* reduced from 2048: no PSRAM on target; allocated from DRAM */
#define BP_WRITE_DELAY_US         200
#define BP_IOBUF_SIZE             8192
#define BP_FLUSH_ROWS             200
#define BP_ROW_BUF                64
#define BP_DURATION_30S           30
#define BP_DURATION_60S           60
#define BP_DURATION_120S          120
#define BP_BATT_STOP_PCT          5
#define BP_ANALYSIS_STACK_BYTES   8192

/* -------------------------------------------------------------------------- */
/* Respiration rate                                                           */
/* -------------------------------------------------------------------------- */

#define RESP_RATE_WINDOW_SECONDS  20
#define RESP_DUP_DELAY_MS         200
#define RESP_DUP_ELEVATION        25
#define RESP_MIN_BPM              4
#define RESP_MAX_BPM              60

/* -------------------------------------------------------------------------- */
/* PPG simulation — two-Gaussian model (Charlton et al., Sci Rep 2020)       */
/* -------------------------------------------------------------------------- */

/* Pulse Arrival Time: ECG R-peak to PPG foot at wrist (ms) */
#define PPG_SIM_PAT_DEFAULT_MS     200.0f
/* ± beat-to-beat jitter applied via LFSR */
#define PPG_SIM_PAT_JITTER_MS       10.0f
/* beats in the PAT running average shown on the topbar (~4 s at 75 BPM) */
#define PPG_PAT_AVG_BEATS            4

/* Systolic Gaussian: amplitude, centre (fraction of RR), width (fraction of RR) */
#define PPG_SIM_A1                   1.0f
#define PPG_SIM_MU1_FRAC            0.18f
#define PPG_SIM_SIGMA1_FRAC         0.08f

/* Diastolic/reflected Gaussian */
#define PPG_SIM_A2                   0.35f
#define PPG_SIM_MU2_FRAC            0.45f
#define PPG_SIM_SIGMA2_FRAC         0.12f

/* -------------------------------------------------------------------------- */
/* PPG bandpass filter                                                        */
/* -------------------------------------------------------------------------- */

/* Applied to the raw PPG sample before foot detection and chart display.
 * Removes DC baseline wander (<0.5 Hz) and high-frequency noise (>16 Hz). */
#define PPG_BP_LOW_HZ            0.5f
#define PPG_BP_HIGH_HZ           16.0f

/* -------------------------------------------------------------------------- */
/* Files / Wi-Fi transfer                                                     */
/* -------------------------------------------------------------------------- */

#define FILES_MAX_LIST_COUNT        64
#define FILES_LIST_NAME_MAX         64
#define FILES_PATH_MAX              128
#define FILES_TX_CHUNK_BYTES        1024
#define FILES_HTTP_TIMEOUT_MS       15000
#define FILES_SERVER_PORT           8000
#define FILES_SERVER_PATH           "/upload"
#define FILES_UPLOAD_STACK_BYTES    8192

/* -------------------------------------------------------------------------- */
/* ECG source selection                                                       */
/* -------------------------------------------------------------------------- */

/* Set to 1 to use the firmware-simulated ECG waveform instead of real ADC.
 * On the TZT board, GPIO14 = display SPI clock and must never be used as ADC.
 * ADC2 is also blocked during WiFi on classic ESP32. Keep this 1 until an
 * external ADC front-end (SPI/I2C) is wired in. */
#define ECG_USE_SIMULATED_SOURCE  1

/* -------------------------------------------------------------------------- */
/* Biosensor bus — SPI2 shared with display; I2C0 for MAX30102 + MPU-6050    */
/* See IO-pin-plan.md for full pin assignments and board-mod notes.           */
/* -------------------------------------------------------------------------- */

/* ADS1293 (ECG / FCG) — SPI2 shared bus, CS active-low, DRDY active-low.
 * Remove R20 from the LED B footprint to free GPIO16 as CS.
 * GPIO4 also drives LED G via R17 (1 kΩ); leave R17 in place — it does not
 * interfere with digital DRDY input. */
#define PIN_ADS1293_CS      GPIO_NUM_16
#define PIN_ADS1293_DRDY    GPIO_NUM_4

/* ADS1220 (nasal thermistor / ERB) — SPI2 shared bus.
 * Remove R16 from the LED R footprint to free GPIO17 as CS.
 * GPIO35 is input-only (expansion connector P3); no internal pull-up. */
#define PIN_ADS1220_CS      GPIO_NUM_17
#define PIN_ADS1220_DRDY    GPIO_NUM_35   /* input-only; no pull-up capability */

/* MAX30102 (PPG / SpO₂) — I2C0. GPIO32 is digital input only; ADC2 blocked
 * during WiFi on classic ESP32 so never use GPIO32 as ADC. */
#define PIN_MAX30102_INT    GPIO_NUM_32

/* MPU-6050 (accelerometer / gyroscope) — I2C0. GPIO25 is digital input only. */
#define PIN_MPU6050_INT     GPIO_NUM_25

/* I2C0 master bus — SDA=GPIO21, SCL=GPIO22 (board I2C header). */
#define PIN_I2C_SDA         GPIO_NUM_21
#define PIN_I2C_SCL         GPIO_NUM_22
#define I2C_BUS_FREQ_HZ     400000

/* I2C device addresses */
#define I2C_ADDR_MAX30102   0x57
#define I2C_ADDR_MPU6050    0x68   /* AD0 low; use 0x69 when AD0 tied high */

/* -------------------------------------------------------------------------- */
