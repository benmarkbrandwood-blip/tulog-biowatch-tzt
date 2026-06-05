# Firmware Migration Plan: Waveshare ESP32-S3 AMOLED → TZT ESP32-2432S024C

**Source project:** `tulog-biowatch` (ESP32-S3, Waveshare 2.06" AMOLED)  
**Target board:** TZT ESP32-2432S024C (ESP32-WROOM-32, 2.4" ILI9341 TFT, capacitive touch variant)  
**Toolchain:** ESP-IDF + VS Code (no Arduino IDE)  
**Date:** 2026-06-05

---

## Table of Contents

1. [Hardware Delta Summary](#1-hardware-delta-summary)
2. [Source Repo Hardware Abstraction Analysis](#2-source-repo-hardware-abstraction-analysis)
3. [Driver Substitution Plan](#3-driver-substitution-plan)
4. [Pin Remapping Table](#4-pin-remapping-table)
5. [ESP-IDF Project Configuration Changes](#5-esp-idf-project-configuration-changes)
6. [Ordered Migration Sequence](#6-ordered-migration-sequence)
7. [Arduino-to-ESP-IDF Translation Notes](#7-arduino-to-esp-idf-translation-notes)
8. [Risks and Gotchas](#8-risks-and-gotchas)
9. [Recommended Project Structure](#9-recommended-project-structure)

---

## 1. Hardware Delta Summary

### SoC Comparison

| Parameter | Source: ESP32-S3 | Target: ESP32-WROOM-32 | Impact |
|---|---|---|---|
| CPU core | Xtensa LX7 | Xtensa LX6 | ABI change; recompile only |
| Clock | 240 MHz | 240 MHz | None |
| SRAM | 512 KB internal | 520 KB internal | Minor |
| PSRAM | 8 MB OPI (external) | **None** | **Critical** — LVGL pool must move to SRAM |
| Flash | 32 MB | **4 MB** | **Critical** — partition table must be redesigned |
| USB | USB-JTAG/OTG built-in | **CH340 UART only** | Console config, no USB CDC |
| ADC1 | GPIO1–10 (S3) | GPIO32–39 | All battery ADC pins change |
| ADC2 | Available (no WiFi conflict on S3) | **Blocked during WiFi** | Never use ADC2 channels for sensors |
| I2C | Any GPIO (BSP-managed) | Any GPIO (manual) | Pin assignment required |
| SPI | Any GPIO (BSP-managed) | Any GPIO (manual) | Pin assignment required |

### Peripheral Comparison

| Subsystem | Source | Target | Action |
|---|---|---|---|
| Display | CO5300 QSPI AMOLED, 410×502 | **ILI9341 SPI TFT, 240×320** | Replace panel driver entirely |
| Display bus | QSPI (4-bit) via BSP | **SPI (1-bit data)** | New `spi_bus_initialize` config |
| Touch controller | FT3168 capacitive (via BSP I2C) | **CST820 capacitive, I2C 0x15** | New I2C driver (note: seller docs say CST816S; actual chip is CST820 — same I2C address, mostly compatible register map) |
| Backlight | BSP-managed PWM | **GPIO27, LEDC PWM** | New `ledc` implementation |
| PMIC | AXP2101 (I2C 0x34) — fuel gauge, voltage registers | **TP4054 linear charger — no I2C, no firmware control** | Replace with ADC voltage-curve battery estimation |
| SD card | SDMMC 1-bit native (GPIO2/1/3) | **SPI mode via `sdspi_host`** | New host config and mount call |
| RGB LED | None used | GPIO4 (common anode, active-low) | Optional: add or leave unused |
| Audio/Temp | Not used in source | DHT11/speaker pins present | Out of scope; leave unused |
| Boot button | GPIO0 | **GPIO0** | No change |

### Memory Budget (Target)

| Region | Capacity | Allocated (estimate) |
|---|---|---|
| Internal SRAM (520 KB total) | ~300–350 KB usable | WiFi stack: ~130 KB; FreeRTOS + app: ~120 KB; LVGL buffers: ~30 KB |
| PSRAM | **0** | Nothing can assume `MALLOC_CAP_SPIRAM` |
| Flash | 4 MB | App: ≤3.5 MB; NVS: 24 KB; FAT storage: ≤448 KB |

---

## 2. Source Repo Hardware Abstraction Analysis

The source project is well-layered. All board-specific hardware is isolated in two places:

### Files That Must Be Completely Replaced

| File | Why |
|---|---|
| `main/hal/hal_battery.c/.h` | Reads AXP2101 via BSP I2C handle — PMIC does not exist on target |
| `main/hal/hal_display.c` | Calls `bsp_display_lock/unlock` — entire BSP display stack is gone |
| `main/hal/hal_storage.c` | Uses `bsp_sdcard_mount()` — BSP wrapper for SDMMC host |

### Files That Need Targeted Edits

| File | Required Changes |
|---|---|
| `main/app_config.h` | `LCD_H_RES 410→240`, `LCD_V_RES 502→320`, all pin defines, ECG plot dimensions |
| `main/main.c` | Remove BSP `#include`s and BSP init calls; wire up new HAL init functions |
| `main/CMakeLists.txt` | Remove BSP component dep; add `esp_lcd_ili9341`, `esp_adc`, `spi_flash`, `fatfs` |
| `idf_component.yml` (root) | Remove `waveshare__esp32_s3_touch_amoled_2_06`; add `espressif/esp_lcd_ili9341` |
| `sdkconfig.defaults` | Target, flash size, PSRAM removal, console UART, LVGL pool |
| `partitions.csv` | Complete redesign for 4 MB flash |

### Files That Are Portable (No Changes Needed)

| File | Reason |
|---|---|
| `main/hal/hal_display.h` | Public API only — `display_init()`, `display_flush_cb()` |
| All LVGL UI source files | LVGL is hardware-agnostic once flush/input callbacks are set |
| `main/tasks/` (WiFi, clock, BP, ECG sim) | FreeRTOS primitives fully portable |
| `main/hal/hal_battery.h` | Public API — `battery_read_voltage()`, `battery_read_percent()` |
| `main/hal/hal_storage.h` | Public API — `storage_mount()`, `storage_unmount()` |

### New Files Required

| File | Purpose |
|---|---|
| `main/hal/hal_touch.c/.h` | CST820 driver (I2C, reset sequence, coordinate read) |
| `main/hal/hal_backlight.c/.h` | LEDC PWM backlight control on GPIO27 |

### BSP Dependency Removal

The BSP (`waveshare__esp32_s3_touch_amoled_2_06`) handles display init, I2C bus creation, touch init, SD mount, and exposes `bsp_i2c_get_handle()`. Every call to a `bsp_*` function in the source must be replaced by a direct ESP-IDF driver call in the new HAL layer.

Search the source for these BSP symbols to find all replacement sites:

```
grep -rn "bsp_" main/
grep -rn "esp32_s3_touch_amoled" main/
```

---

## 3. Driver Substitution Plan

### 3.1 Display — CO5300 QSPI → ILI9341 SPI

**Remove:** `esp_lcd_co5300` panel driver, QSPI panel IO  
**Add:** `espressif/esp_lcd_ili9341` managed component

```c
// SPI bus init (call once, shared with nothing else on this board)
spi_bus_config_t buscfg = {
    .mosi_io_num   = GPIO_NUM_13,
    .miso_io_num   = GPIO_NUM_12,
    .sclk_io_num   = GPIO_NUM_14,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t),
};
spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);

// Panel IO
esp_lcd_panel_io_handle_t io_handle;
esp_lcd_panel_io_spi_config_t io_config = {
    .cs_gpio_num       = GPIO_NUM_15,
    .dc_gpio_num       = GPIO_NUM_2,
    .spi_clock_hz      = 40 * 1000 * 1000,   // start at 40 MHz; try 80 MHz after validation
    .lcd_cmd_bits      = 8,
    .lcd_param_bits    = 8,
    .trans_queue_depth = 10,
    .on_color_trans_done = NULL,              // set to LVGL flush-done callback
};
esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &io_handle);

// ILI9341 panel
esp_lcd_panel_handle_t panel_handle;
esp_lcd_panel_dev_config_t panel_config = {
    .reset_gpio_num = -1,                     // no hardware reset pin on this board
    .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
    .bits_per_pixel = 16,
};
esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle);
esp_lcd_panel_reset(panel_handle);
esp_lcd_panel_init(panel_handle);
esp_lcd_panel_mirror(panel_handle, false, false);
esp_lcd_panel_disp_on_off(panel_handle, true);
```

**LVGL flush callback** — replace the PSRAM DMA double-buffer with internal SRAM partial buffers:

```c
// In hal_display.c init, replace 128 KB PSRAM pool with:
#define LVGL_DRAW_BUF_LINES  32
static lv_color_t *buf1;
static lv_color_t *buf2;

buf1 = heap_caps_malloc(LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color_t),
                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
buf2 = heap_caps_malloc(LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color_t),
                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
// Each buffer = 240 × 32 × 2 = 15,360 bytes (~15 KB)

lv_disp_draw_buf_init(&draw_buf, buf1, buf2,
                       LCD_H_RES * LVGL_DRAW_BUF_LINES);
```

**Flush callback:**

```c
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                           lv_color_t *color_map)
{
    esp_lcd_panel_draw_bitmap(panel_handle,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              color_map);
    lv_disp_flush_ready(drv);
}
```

**Resolution update in `app_config.h`:**

```c
// Confirmed working (Phase 4) — panel is physically landscape-native:
#define LCD_H_RES   320
#define LCD_V_RES   240
```

> **Note:** Earlier drafts of this plan specified 240×320. The actual TZT panel is landscape-native
> (320 col × 240 row). See Phase 4 in Section 10 and `README.md §Screen Orientation` for the full
> explanation and working MADCTL configuration.

Adjust `ECG_PLOT_W` and `ECG_PLOT_H` for 320×240 landscape (Phase 5 work).

### 3.2 Touch — FT3168 BSP → CST820 I2C

**Chip identity note:** The user-facing brief and reference link mention CST816S. The seller's zip (`CST820.h`) and factory sketch confirm the chip is **CST820**. Both share I2C address `0x15` and a nearly identical register map. Use CST820 as the authoritative reference; the register definitions below are from the seller's `CST820.cpp`.

**I2C bus init** (new, shared with nothing — AXP2101 is gone):

```c
i2c_master_bus_config_t bus_config = {
    .i2c_port    = I2C_NUM_0,
    .sda_io_num  = GPIO_NUM_33,
    .scl_io_num  = GPIO_NUM_32,
    .clk_source  = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    .flags.enable_internal_pullup = true,
};
i2c_master_bus_handle_t i2c_bus;
i2c_master_bus_create(&bus_config, &i2c_bus);

i2c_device_config_t dev_cfg = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address  = 0x15,   // CST820 fixed address
    .scl_speed_hz    = 400000,
};
i2c_master_dev_handle_t cst820_dev;
i2c_master_bus_add_device(i2c_bus, &dev_cfg, &cst820_dev);
```

**CST820 reset sequence** (must be performed before I2C transactions):

```c
// Assumes TP_INT = GPIO21, TP_RST = GPIO25
gpio_set_direction(GPIO_NUM_21, GPIO_MODE_OUTPUT);
gpio_set_direction(GPIO_NUM_25, GPIO_MODE_OUTPUT);

gpio_set_level(GPIO_NUM_21, 1);   // INT high
vTaskDelay(pdMS_TO_TICKS(1));
gpio_set_level(GPIO_NUM_21, 0);   // INT low
vTaskDelay(pdMS_TO_TICKS(1));
gpio_set_level(GPIO_NUM_25, 0);   // RST low
vTaskDelay(pdMS_TO_TICKS(10));
gpio_set_level(GPIO_NUM_25, 1);   // RST high
vTaskDelay(pdMS_TO_TICKS(300));   // wait for chip ready

// Disable auto-sleep (register 0xFE = 0xFF)
uint8_t cmd[] = {0xFE, 0xFF};
i2c_master_transmit(cst820_dev, cmd, 2, pdMS_TO_TICKS(100));
```

**CST820 register map:**

| Register | Description |
|---|---|
| 0x01 | Gesture ID (0x00=none, 0x01=slide↓, 0x02=slide↑, 0x03=slide←, 0x04=slide→, 0x05=tap, 0x0B=double-tap, 0x0C=long-press) |
| 0x02 | Finger count (0 or 1 for single-touch) |
| 0x03 | X coordinate high byte (bits [3:0]) |
| 0x04 | X coordinate low byte |
| 0x05 | Y coordinate high byte (bits [3:0]) |
| 0x06 | Y coordinate low byte |
| 0xFE | Auto-sleep control (write 0xFF to disable) |

**Coordinate read:**

```c
uint8_t reg = 0x01;
uint8_t data[6];
i2c_master_transmit_receive(cst820_dev, &reg, 1, data, 6,
                             pdMS_TO_TICKS(100));
// data[0] = gesture, data[1] = finger count
// data[2..3] = x_high, x_low → x = ((data[2] & 0x0F) << 8) | data[3]
// data[4..5] = y_high, y_low → y = ((data[4] & 0x0F) << 8) | data[5]
```

**LVGL input driver registration:**

```c
lv_indev_drv_t indev_drv;
lv_indev_drv_init(&indev_drv);
indev_drv.type    = LV_INDEV_TYPE_POINTER;
indev_drv.read_cb = cst820_lvgl_read_cb;  // reads above registers, sets data->point + data->state
lv_indev_drv_register(&indev_drv);
```

### 3.3 Backlight — BSP PWM → LEDC on GPIO27

```c
// In hal_backlight.c
ledc_timer_config_t timer = {
    .speed_mode      = LEDC_LOW_SPEED_MODE,
    .timer_num       = LEDC_TIMER_0,
    .duty_resolution = LEDC_TIMER_8_BIT,
    .freq_hz         = 5000,
    .clk_cfg         = LEDC_AUTO_CLK,
};
ledc_timer_config(&timer);

ledc_channel_config_t channel = {
    .gpio_num   = GPIO_NUM_27,
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .channel    = LEDC_CHANNEL_0,
    .timer_sel  = LEDC_TIMER_0,
    .duty       = 0,
    .hpoint     = 0,
};
ledc_channel_config(&channel);

void backlight_set_percent(uint8_t pct) {
    uint32_t duty = (pct * 255) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
```

Map `DEFAULT_BRIGHTNESS 80` (from `app_config.h`) directly to `backlight_set_percent(80)`.

### 3.4 Battery — AXP2101 I2C → ADC Oneshot

**Remove:** All AXP2101 I2C register reads (`PMU_STATUS1/2`, `VBAT_H/L`, `BATT_PERCENT` at 0x00/0x01/0x34/0x35/0xA4).

**Add:** `adc_oneshot` on GPIO35 (ADC1_CH7). GPIO35 is a confirmed ADC1 channel on the classic ESP32 and is safe for use during WiFi (ADC1 is not gated by the RF block).

> **Pin status:** GPIO35 is not assigned to any other function in the seller's sample code. Verify on the schematic (`b5e957d9840f6abd6caa7919de8d750f6fe6b52a.png`) that the "BATTERY ADC" or "BAT_ADC" net routes to GPIO35 before finalising. If it routes elsewhere, update `HAL_BATTERY_ADC_GPIO` accordingly.

```c
// hal_battery.c replacement
#define HAL_BATTERY_ADC_CHANNEL  ADC_CHANNEL_7   // GPIO35
#define BATTERY_VOLTAGE_EMPTY    3300  // mV (from app_config.h)
#define BATTERY_VOLTAGE_FULL     4200  // mV (from app_config.h)

static adc_oneshot_unit_handle_t adc_handle;
static adc_cali_handle_t         cali_handle;

void battery_adc_init(void) {
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_new_unit(&init_cfg, &adc_handle);

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_config_channel(adc_handle, HAL_BATTERY_ADC_CHANNEL, &chan_cfg);

    // Calibration (curve fitting preferred over line fitting on classic ESP32)
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .chan     = HAL_BATTERY_ADC_CHANNEL,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle);
}

int battery_read_voltage(void) {  // returns mV
    int raw, mv;
    adc_oneshot_read(adc_handle, HAL_BATTERY_ADC_CHANNEL, &raw);
    adc_cali_raw_to_voltage(cali_handle, raw, &mv);
    // Account for voltage divider if one is present on the PCB (check schematic)
    return mv;
}

int battery_read_percent(void) {
    int mv = battery_read_voltage();
    if (mv >= BATTERY_VOLTAGE_FULL) return 100;
    if (mv <= BATTERY_VOLTAGE_EMPTY) return 0;
    return (mv - BATTERY_VOLTAGE_EMPTY) * 100 /
           (BATTERY_VOLTAGE_FULL - BATTERY_VOLTAGE_EMPTY);
}
```

Keep the existing `battery_read_percent()` linear interpolation fallback from `hal_battery.c` — the voltage curve approach is equivalent. The fuel-gauge branch (AXP2101 `BATT_PERCENT` register) simply becomes dead code to delete.

### 3.5 SD Card — SDMMC 1-bit → SPI via `sdspi_host`

**Remove:** `bsp_sdcard_mount()`, `SDMMC_HOST_DEFAULT()`, GPIO2/1/3 slot config.

**Add:** `spi_bus_initialize` on the SD SPI bus + `sdspi_host` mount.

> **Pin status:** SD SPI pins are not stated in the seller's capacitive-variant sketch. The conventional mapping for this 2432S024 PCB family is CLK=18, MISO=19, MOSI=23, CS=5. Confirm against the schematic (`TF-CARD` section, bottom-right of image) before use.

```c
// hal_storage.c replacement
#define SD_SPI_HOST  SPI3_HOST   // VSPI, separate bus from display
#define SD_PIN_CLK   GPIO_NUM_18
#define SD_PIN_MISO  GPIO_NUM_19
#define SD_PIN_MOSI  GPIO_NUM_23
#define SD_PIN_CS    GPIO_NUM_5

esp_err_t storage_mount(void) {
    spi_bus_config_t bus = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    spi_bus_initialize(SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs   = SD_PIN_CS;
    slot.host_id   = SD_SPI_HOST;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card;
    return esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot,
                                   &mount_cfg, &card);
}
```

Keep the retry logic from `hal_storage.c` (`SD_MOUNT_RETRY_MAX`, 200 ms delays) — it is portable.

### 3.6 I2C Bus

Source uses `bsp_i2c_get_handle()` to share one I2C bus between AXP2101 and FT3168. On the target, AXP2101 is gone and CST820 is the only I2C device. Create one I2C bus (I2C_NUM_0, GPIO33/SDA, GPIO32/SCL) in `hal_touch.c` and expose the bus handle if any future sensor needs it.

### 3.7 Sensor Subsystems (ECG / BP)

Source uses simulated data (`ECG_USE_SIMULATED_SOURCE=1`) and the ECG ADC path is already disabled (`#if 0` in `main.c`). The disabled path referenced GPIO14/ADC2_CH3 on the S3; on the target, **GPIO14 is the display SPI clock** — that path must remain disabled or be rerouted to an ADC1 channel (GPIO32–39) if biosensors are added later.

The BP sampler task is hardware-independent (simulated). No changes required.

---

## 4. Pin Remapping Table

| Function | Source Pin (ESP32-S3) | Target Pin (ESP32) | Notes |
|---|---|---|---|
| Display MOSI | BSP-managed | **GPIO13** | `TFT_MOSI` from seller User_Setup.h |
| Display MISO | BSP-managed | **GPIO12** | `TFT_MISO` |
| Display SCLK | BSP-managed | **GPIO14** | `TFT_SCLK` — **was ECG ADC on source, now display clock** |
| Display CS | BSP-managed | **GPIO15** | `TFT_CS` |
| Display DC | BSP-managed | **GPIO2** | `TFT_DC` |
| Display RST | BSP-managed | **-1 (none)** | No hardware reset pin |
| Backlight | BSP PWM | **GPIO27** | LEDC PWM, capacitive-variant confirmed; **GPIO21 is touch INT here, NOT backlight** |
| Touch SDA | GPIO15 (source board) | **GPIO33** | `I2C_SDA` from factory sketch |
| Touch SCL | GPIO14 (source board) | **GPIO32** | `I2C_SCL` |
| Touch RST | BSP-managed | **GPIO25** | `TP_RST` |
| Touch INT | BSP-managed | **GPIO21** | `TP_INT` — **NOT backlight** (resistive variant uses GPIO21 for backlight; capacitive variant uses it for touch interrupt) |
| Boot button | GPIO0 | **GPIO0** | No change |
| Battery ADC | AXP2101 I2C | **GPIO35** (ADC1_CH7) | Verify against schematic; ADC1 safe during WiFi |
| SD SCLK | GPIO2 (SDMMC) | **GPIO18** (SPI) | Conventional; verify schematic |
| SD MISO | GPIO3 (SDMMC D0) | **GPIO19** (SPI) | Conventional; verify schematic |
| SD MOSI | GPIO1 (SDMMC CMD) | **GPIO23** (SPI) | Conventional; verify schematic |
| SD CS | BSP-managed | **GPIO5** (SPI) | Conventional; verify schematic |
| RGB LED R | Not used | GPIO4 (active-low) | Optional |
| USB/Console | USB-JTAG (internal) | **UART0 via CH340** (GPIO1 TX, GPIO3 RX) | Console config change required |

> **Backlight warning:** The Getting Started PDF screenshot shows `TFT_BL=21`. This is from the **resistive** variant (SKU ending in R). The **capacitive** variant (SKU ending in C, which this board is) uses GPIO27 for backlight and GPIO21 for touch interrupt. Mixing these up will drive the touch INT line as a PWM output and prevent touch from working.

---

## 5. ESP-IDF Project Configuration Changes

### 5.1 `sdkconfig.defaults` — Complete Replacement

```
# --- Target ---
CONFIG_IDF_TARGET="esp32"

# --- Flash ---
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHMODE_DIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_40M=y

# --- Remove PSRAM (entire OPI/Octal section gone) ---
# CONFIG_SPIRAM is not set
# CONFIG_SPIRAM_MODE_OCT is not set

# --- Console: UART0 via CH340 (was USB-Serial-JTAG) ---
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
# CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG is not set

# --- FreeRTOS ---
CONFIG_FREERTOS_HZ=1000

# --- LVGL: custom allocator, no PSRAM pool ---
CONFIG_LV_MEM_CUSTOM=y
# Remove CONFIG_LV_MEM_SIZE_KILOBYTES=128

# --- ADC ---
CONFIG_ADC_ONESHOT_CTRL_FUNC_IN_IRAM=y

# --- WiFi ---
CONFIG_ESP_WIFI_ENABLED=y
# ADC2 is unavailable during WiFi on classic ESP32 — no code should use it

# --- SPI ---
CONFIG_SPI_MASTER_IN_IRAM=y        # reduces latency for display flush
```

### 5.2 `partitions.csv` — 4 MB Redesign

The source partition table requires ~32 MB (3 MB app + 28 MB FAT). The target has 4 MB.

**Option A — Maximum app space (no FAT storage):**

```csv
# Name,   Type, SubType,  Offset,   Size
nvs,      data, nvs,      0x9000,   0x6000
phy_init, data, phy,      0xF000,   0x1000
factory,  app,  factory,  0x10000,  0x3F0000
```

App space: 3.9375 MB. Use NVS for all key-value config. No SD-card FAT partition in internal flash.

**Option B — Minimal FAT for config/log storage:**

```csv
# Name,   Type, SubType,  Offset,   Size
nvs,      data, nvs,      0x9000,   0x6000
phy_init, data, phy,      0xF000,   0x1000
factory,  app,  factory,  0x10000,  0x380000
storage,  data, fat,      0x390000, 0x70000
```

App space: 3.5 MB. FAT storage: 448 KB. Total: exactly 4 MB.

Recommendation: **Option B** preserves the `/sdcard/` virtual filesystem path used in source code, replacing the external SD card with a small internal FAT partition for log files and config. If SD card data logging is not required at all, use Option A for maximum headroom.

### 5.3 `idf_component.yml` Changes

```yaml
# Remove:
dependencies:
  waveshare/esp32_s3_touch_amoled_2_06: ">=1.0.0"

# Add:
dependencies:
  espressif/esp_lcd_ili9341: ">=1.0.0"
  idf: ">=5.1.0"
```

### 5.4 `main/CMakeLists.txt` Changes

```cmake
# Remove from REQUIRES:
idf::waveshare__esp32_s3_touch_amoled_2_06
idf::sdmmc           # replaced by spi + sdspi_host

# Add to REQUIRES:
idf::esp_lcd
idf::esp_adc
idf::driver          # for LEDC, SPI master
idf::fatfs
idf::wear_levelling  # if using internal FAT
```

Keep: `idf::nvs_flash`, `idf::esp_wifi`, `idf::esp_http_client`, `idf::esp_sntp`, `idf::lvgl`.

---

## 6. Ordered Migration Sequence

### Step 1 — Create New ESP-IDF Project Skeleton

1. Copy source repo to a new directory (preserve original).
2. Edit `sdkconfig.defaults`: set `CONFIG_IDF_TARGET="esp32"`, remove S3/PSRAM/USB-JTAG entries.
3. Replace `partitions.csv` with the 4 MB version.
4. Run `idf.py set-target esp32` to regenerate `sdkconfig`.
5. Confirm `idf.py build` fails only on missing BSP symbols, not toolchain errors.

### Step 2 — Remove BSP Dependency

1. Edit `idf_component.yml`: remove `waveshare/esp32_s3_touch_amoled_2_06`.
2. Edit `main/CMakeLists.txt`: remove BSP component from `REQUIRES`.
3. Remove BSP `#include` lines from `main.c`, `hal_display.c`, `hal_battery.c`, `hal_storage.c`.
4. Run `idf.py reconfigure`. Expect linker errors for `bsp_*` symbols — these will be resolved in subsequent steps.

### Step 3 — Port `app_config.h`

Update all hardware-specific defines:

```c
#define LCD_H_RES          240
#define LCD_V_RES          320
#define ECG_PLOT_W         216   // scale from 370/410 * 240
#define ECG_PLOT_H         157   // scale from 250/502 * 320
#define DEFAULT_BRIGHTNESS  80   // unchanged

// New pin defines (move from scattered .c files into one place)
#define PIN_LCD_MOSI  GPIO_NUM_13
#define PIN_LCD_MISO  GPIO_NUM_12
#define PIN_LCD_SCLK  GPIO_NUM_14
#define PIN_LCD_CS    GPIO_NUM_15
#define PIN_LCD_DC    GPIO_NUM_2
#define PIN_LCD_BL    GPIO_NUM_27
#define PIN_TP_SDA    GPIO_NUM_33
#define PIN_TP_SCL    GPIO_NUM_32
#define PIN_TP_RST    GPIO_NUM_25
#define PIN_TP_INT    GPIO_NUM_21
#define PIN_BAT_ADC   GPIO_NUM_35
```

### Step 4 — Implement `hal_backlight.c/.h`

Implement LEDC PWM driver as shown in Section 3.3. This is the simplest HAL and validates the build pipeline with a concrete hardware interaction.

Test: call `backlight_set_percent(50)` from `app_main()`, flash, confirm the display backlight is on at half brightness.

### Step 5 — Implement `hal_display.c`

Replace BSP display with `esp_lcd` + ILI9341 as shown in Section 3.1. Keep the `display_init()` / `display_flush_cb()` public API surface from `hal_display.h` unchanged so LVGL integration in `main.c` requires no edits.

Key change: LVGL draw buffers must be allocated with `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` (no `MALLOC_CAP_SPIRAM`).

Test: render a solid-color screen fill via `lv_obj_set_style_bg_color()` on the root object.

### Step 6 — Implement `hal_touch.c/.h`

Implement CST820 reset sequence and I2C coordinate read as shown in Section 3.2. Register the LVGL input driver.

Test: `ESP_LOGI` the gesture ID and (x, y) coordinates on each interrupt, confirm they print when the screen is tapped.

### Step 7 — Port `hal_battery.c`

Replace AXP2101 I2C reads with `adc_oneshot` as shown in Section 3.4.

Test: log `battery_read_voltage()` every 5 seconds, confirm the value is in the 3.3–4.2 V range when a battery is connected. If the reading is wildly wrong, check whether a voltage divider is present on the PCB (schematic section: `lithium battery charging circuit`) and apply the appropriate scaling factor.

### Step 8 — Port `hal_storage.c`

Replace SDMMC 1-bit with `sdspi_host` as shown in Section 3.5.

Test: insert an SD card, call `storage_mount()`, confirm `/sdcard/` is accessible via `fopen("/sdcard/test.txt", "w")`.

### Step 9 — Port `main.c`

1. Remove all `bsp_*` init calls.
2. Replace with calls to the new HAL init sequence:
   - `backlight_init()` → `display_init()` → `touch_init()` → `battery_adc_init()` → optionally `storage_mount()`
3. Keep all task creation calls unchanged (they are portable).
4. The `bsp_display_lock` / `bsp_display_unlock` pattern in `hal_display.c` must be replaced with a direct `portMUX_TYPE` spinlock or a FreeRTOS mutex if LVGL task safety is required.

### Step 10 — Validate and Tune

1. Run all original tasks (clock, WiFi scan, WiFi connect, BP sampler).
2. Monitor heap with `esp_get_free_heap_size()` under load — target: stay above 50 KB free.
3. If heap is tight, reduce LVGL `LVGL_DRAW_BUF_LINES` from 32 to 16 (halves buffer size, increases flush calls).
4. Validate WiFi + battery ADC coexistence: ADC1 (GPIO35) must not glitch during WiFi activity.
5. Tune SPI clock: if ILI9341 display shows corruption at 80 MHz, reduce to 40 MHz in `io_config.spi_clock_hz`.

---

## 7. Arduino-to-ESP-IDF Translation Notes

The seller's sample code is Arduino. These are the ESP-IDF equivalents for patterns used in the factory sketch.

### Wire (I2C)

| Arduino | ESP-IDF |
|---|---|
| `Wire.begin(SDA, SCL)` | `i2c_master_bus_create(&bus_config, &bus)` |
| `Wire.beginTransmission(addr)` + `Wire.write(reg)` + `Wire.endTransmission()` | `i2c_master_transmit(dev, buf, len, timeout)` |
| `Wire.requestFrom(addr, n)` + `Wire.read()` | `i2c_master_receive(dev, buf, n, timeout)` |
| Combined write-then-read | `i2c_master_transmit_receive(dev, tx, tx_len, rx, rx_len, timeout)` |

Use the new `i2c_master` API (ESP-IDF ≥5.1). Do not use the legacy `i2c_driver_install` / `i2c_cmd_link_create` pattern.

### SPI (display)

| Arduino (TFT_eSPI) | ESP-IDF |
|---|---|
| `tft.init()` | `esp_lcd_new_panel_ili9341(io, &cfg, &panel)` + `esp_lcd_panel_init(panel)` |
| `tft.pushImageDMA(x, y, w, h, buf)` | `esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, buf)` |
| `tft.setRotation(n)` | `esp_lcd_panel_swap_xy(panel, ...)` + `esp_lcd_panel_mirror(panel, ...)` |
| `tft.setWindow(x1, y1, x2, y2)` | Part of `draw_bitmap` internally |

### GPIO

| Arduino | ESP-IDF |
|---|---|
| `pinMode(pin, OUTPUT)` | `gpio_set_direction(pin, GPIO_MODE_OUTPUT)` |
| `digitalWrite(pin, HIGH)` | `gpio_set_level(pin, 1)` |
| `digitalRead(pin)` | `gpio_get_level(pin)` |

### Backlight (PWM)

| Arduino | ESP-IDF |
|---|---|
| `ledcSetup(ch, freq, bits)` | `ledc_timer_config(&timer)` |
| `ledcAttachPin(pin, ch)` | `ledc_channel_config(&channel)` |
| `ledcWrite(ch, duty)` | `ledc_set_duty(mode, ch, duty)` + `ledc_update_duty(mode, ch)` |

### Heap Allocation

| Arduino / IDF (both) | Notes |
|---|---|
| `heap_caps_malloc(size, MALLOC_CAP_DMA \| MALLOC_CAP_INTERNAL)` | Keep this pattern — it is already correct for DMA-backed LVGL buffers on classic ESP32 |
| `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` | **Do not use** — no PSRAM on target |

### Delay

| Arduino | ESP-IDF |
|---|---|
| `delay(ms)` | `vTaskDelay(pdMS_TO_TICKS(ms))` |

---

## 8. Risks and Gotchas

### 8.1 GPIO14 is Now the Display SPI Clock

On the source board, GPIO14 was the disabled ECG ADC path (ADC2_CH3 on S3). On the target, **GPIO14 = display SPI clock**. Any future attempt to re-enable a GPIO14 ADC path will corrupt the display. Reroute biosensor ADC to ADC1 channels (GPIO32–39) on the target.

### 8.2 ADC2 Unavailable During WiFi

Classic ESP32 (unlike S3) gates the entire ADC2 subsystem when the RF block is active. ADC2 channels include GPIO0, 2, 4, 12–15, 25, 26, 27. The battery ADC must use ADC1 (GPIO32–39). GPIO35 (ADC1_CH7) is confirmed safe.

### 8.3 Backlight Pin Conflict — GPIO21 vs GPIO27

The Getting Started PDF screenshot shows `TFT_BL=21`. This is the **resistive** variant. The **capacitive** variant (this board) uses:
- **GPIO27** = `TFT_BL` (backlight PWM)
- **GPIO21** = `TP_INT` (touch interrupt)

Setting GPIO21 as backlight output will drive the touch INT line and prevent touch from initialising. Always derive pin assignments from the capacitive-variant `User_Setup.h` and factory `.ino` sketch, not the Getting Started PDF.

### 8.4 CST820 vs CST816S Chip Identity

The user brief and reference link mention CST816S. The seller's actual files (`CST820.h`) identify the chip as CST820. Both:
- Share I2C address 0x15
- Use the same reset sequence (INT toggle before RST)
- Have the same gesture and coordinate registers at 0x01–0x06
- Use register 0xFE for auto-sleep control

In practice the driver is compatible. The chip ID register (`0xA7` on CST816S) may return a different value; do not gate init on an exact chip ID check.

### 8.5 No PSRAM — Stack and Heap Discipline

The source was written assuming 8 MB PSRAM. On the target, all dynamic memory comes from 520 KB SRAM, shared with WiFi (~130 KB), Bluetooth (disabled = no cost), FreeRTOS, and the app stack. Scrutinise every `heap_caps_malloc` and remove any `MALLOC_CAP_SPIRAM` flag. Consider reducing FreeRTOS task stack sizes:

| Task | Source stack | Recommended target |
|---|---|---|
| `wifi_scan` | 6144 | 4096 |
| `wifi_connect` | 8192 | 6144 |
| `clock_update` | default | 2048 |

Monitor with `esp_get_free_heap_size()` and `uxTaskGetStackHighWaterMark()` during integration testing.

### 8.6 ILI9341 SPI Clock Stability

The seller's `User_Setup.h` specifies `SPI_FREQUENCY=80000000` (80 MHz). This is the maximum rated clock for ILI9341 and may be unstable on some PCBs due to trace capacitance. Start integration at 40 MHz and increase to 80 MHz only if the display is stable at 40 MHz and faster frame rates are needed.

### 8.7 Partition Table — Complete Incompatibility

The source `partitions.csv` (3 MB app + 28 MB FAT) cannot fit in 4 MB flash. Flashing the source partition table to the target will brick the device until erased with `idf.py erase_flash`. Always update `partitions.csv` before flashing.

### 8.8 USB Console Change

Source uses `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`. Target has no USB-JTAG. Serial monitor output goes via CH340 on UART0. Set `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` in `sdkconfig.defaults` and connect at 115200 baud via the USB-C connector (which routes to CH340, not a native USB CDC endpoint).

### 8.9 SD Card Pin Uncertainty

The SD SPI pins (GPIO18/19/23/5) are conventional for this ESP32 board family but are not explicitly stated in the capacitive-variant factory sketch. Verify the `TF-CARD` section of the schematic (`b5e957d9840f6abd6caa7919de8d750f6fe6b52a.png`, bottom-right) before mounting. Using the wrong CS pin will cause the SD init to hang or return `ESP_ERR_TIMEOUT`.

### 8.10 Display Orientation

**Resolved in Phase 4.** The TZT panel is physically landscape-native (320×240). Do not use
`esp_lcd_panel_swap_xy` or `esp_lcd_panel_mirror` — they OR individual MADCTL bits and cause shear.
Write the full MADCTL byte via `esp_lcd_panel_io_tx_param(io, 0x36, {0x40}, 1)` before the GRAM clear.
Working config: `LCD_H_RES=320, LCD_V_RES=240, LCD_MADCTL=0x40, rgb_ele_order=RGB`.
UI layout redesign for 320×240 is Phase 5 work (see Section 10).

---

## 9. Recommended Project Structure

Minimal structural changes are needed. The HAL layer is already well-separated. The only mandatory structural change is adding two new HAL files and removing the BSP dependency.

```
project-root/
├── CMakeLists.txt
├── idf_component.yml          # updated: remove BSP, add esp_lcd_ili9341
├── partitions.csv             # replaced: 4 MB layout
├── sdkconfig.defaults         # replaced: esp32 target, no PSRAM
├── main/
│   ├── CMakeLists.txt         # updated: component deps
│   ├── main.c                 # updated: remove bsp_* calls
│   ├── app_config.h           # updated: resolution, pin defines
│   ├── hal/
│   │   ├── hal_display.c/.h   # replaced: esp_lcd + ILI9341
│   │   ├── hal_touch.c/.h     # NEW: CST820 driver
│   │   ├── hal_backlight.c/.h # NEW: LEDC PWM on GPIO27
│   │   ├── hal_battery.c/.h   # replaced: adc_oneshot on GPIO35
│   │   └── hal_storage.c/.h   # replaced: sdspi_host
│   ├── tasks/
│   │   ├── task_wifi.c        # unchanged
│   │   ├── task_clock.c       # unchanged
│   │   ├── task_ecg.c         # unchanged (simulated source)
│   │   └── task_bp.c          # unchanged
│   └── ui/
│       └── *.c                # widget layout values need updating for 240×320
└── components/
    └── (no custom components needed — managed components via idf_component.yml)
```

### Dependency Graph

```
main.c
  ├── hal_backlight.c   [ledc driver]
  ├── hal_display.c     [spi_master → esp_lcd → ILI9341 → LVGL flush]
  ├── hal_touch.c       [i2c_master → CST820 → LVGL indev]
  ├── hal_battery.c     [adc_oneshot → voltage curve]
  ├── hal_storage.c     [spi_master → sdspi_host → vfs_fat]
  └── tasks/            [FreeRTOS tasks, all portable]
```

All hardware-specific code is confined to the `hal/` layer. The `tasks/` and `ui/` directories remain untouched except for numerical layout constants.

---

---

## 10. Implementation Status & Phase 3 Plan

### Phase 1 — COMPLETE (2026-06-05)
Scaffold: target set to esp32, BSP removed, managed components resolved, app_config.h ported,
partitions.csv redesigned for 4 MB. Build fails only on BSP symbol errors (expected).

### Phase 2 — COMPLETE (2026-06-05) — commit 7433067
Steps 4–6 implemented and building clean:
- `hal_backlight.c/h` — LEDC PWM, GPIO27, AO3402A N-channel (active-HIGH)
- `hal_display.c/h` — ILI9341/SPI2, LVGL 9.5, FreeRTOS mutex, LVGL task Core 1, async DMA flush
- `hal_touch.c/h` — CST820/I2C0, GPIO33/32, reset via GPIO25/21, LVGL indev
- All `bsp_*` symbols eliminated; `hal_battery.c` and `hal_storage.c` are compile-clean stubs

**LVGL 9.5 implementation notes (do not re-derive):**
- `lv_color_t` = 3 bytes; draw buffers must use `sizeof(uint16_t)` = 2 bytes/pixel
- Flush callback must NOT call `lv_display_flush_ready()` synchronously — wire `on_color_trans_done` ISR
- `lv_draw_sw_rgb565_swap(buf, px_count)` in flush_cb for ILI9341 byte order
- IDF 6 I2C: `i2c_new_master_bus()` (not `i2c_master_bus_create()`)

### Phase 3 — COMPLETE (2026-06-05) — commit 1dbe823

Steps 7–11 implemented and hardware-validated:

- **`hal_battery.c/h`** — IP5306 permanent stub confirmed from schematic: no I2C or ADC path to ESP32.
  `battery_read_voltage()` = -1.0f, `battery_read_percent()` = -1 permanently. GPIO35 routes to
  expansion connector P3, NOT battery sense — do not implement ADC on GPIO35.
- **`hal_storage.c`** — `sdspi_host` (SPI3/VSPI) with 5-retry mount loop. Pins confirmed from
  schematic: CS=GPIO5, CLK=GPIO18, MISO=GPIO19, MOSI=GPIO23.
- **`svc_bp_record.c`** — `MALLOC_CAP_SPIRAM` → `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`;
  `BP_QUEUE_LEN` 2048→512 for DRAM budget.
- **`main.c`** — stale BSP comment removed; SD mount comment updated.

**Hardware validated on-device:**
- Boots to Wi-Fi scan → home clock screen
- ILI9341 display renders via LVGL 9.5
- CST820 touch input registered in LVGL
- Backlight LEDC PWM on GPIO27
- Wi-Fi scan/connect with auto-retry, NVS credential persistence
- SNTP sync, NVS time persistence
- ECG 100 Hz simulation, QRS detection, HR/RR estimates
- SD card recording (sdspi) to CSV at 100 Hz
- BP screen: 30/60/120 s, 1 kHz sampler, CSV, HRV RMSSD + PAT analysis
- Files screen: SD enumeration, HTTP POST upload, deletion

---

### Phase 4 — COMPLETE (2026-06-06) — branch display-orientation-fix

**Display orientation — solved after extended investigation.**

> Full investigation log in `CLAUDE.md §11` and `README.md §Screen Orientation`.

**Root cause:** The ILI9341 on the TZT board is physically mounted **landscape-native** (320 columns × 240 rows). Prior attempts used portrait dims (240×320) or hardware `swap_xy` which caused vertical stripe shear — `esp_lcd_panel_swap_xy` sets the MV bit in MADCTL but `draw_bitmap` still sends CASET/RASET in the original axis order, so the pixel stream and GRAM scan direction disagree.

**What does NOT work (do not retry):**
- `esp_lcd_panel_swap_xy(true)` / `mirror()` — shear on this component
- `MALLOC_CAP_SPIRAM` LVGL rotation buffers — no PSRAM
- `lv_display_set_rotation()` in PARTIAL mode — requires full-frame buffer (150 KB)
- `LCD_RGB_ELEMENT_ORDER_BGR` — renders red as blue

**Working configuration (locked in `app_config.h` + `hal_display.c`):**

| Setting | Value |
|---|---|
| `LCD_H_RES` | `320` |
| `LCD_V_RES` | `240` |
| `LCD_NATIVE_W/H` | `240 / 320` (for GRAM clear loop) |
| `LCD_MADCTL` | `0x40` (MX bit — mirror column scan; no MV; BGR=0) |
| `rgb_ele_order` | `LCD_RGB_ELEMENT_ORDER_RGB` |
| Flush callback | `lv_draw_sw_rgb565_swap()` + `esp_lcd_panel_draw_bitmap()` |
| Software rotation | None |
| MADCTL write | Via `esp_lcd_panel_io_tx_param(io, 0x36, {0x40}, 1)` — full byte, before GRAM clear |

---

### Phase 5 — NOT STARTED

**UI layout redesign: 240×320 portrait → 320×240 landscape**

All UI screens in `main.c` were designed for the Waveshare source board (410×502 AMOLED), then scaled to 240×320. They now need reworking for 320×240 landscape.

**Scope:**
- All `lv_obj_set_size`, `lv_obj_set_pos`, `lv_obj_align`, chart dimensions in `main.c`
- `ECG_PLOT_W` and `ECG_PLOT_H` in `app_config.h` — currently ~240×320 proportioned
- Topbar layout (battery, time, HR, SpO₂ labels) — needs horizontal reflow
- Wi-Fi scan list, connecting screen, password screen — vertical scroll → horizontal
- Record screen chart (`REC_CHART_POINTS=200`, 4 s window) — chart width increases from ~216 to ~290
- BP screen — chart, labels, buttons need landscape reflow
- Files screen — list and send/delete buttons need landscape layout
- Settings screen — stub, low priority
- Home clock screen — recentre and resize for 320 wide

**Approach:** Work screen-by-screen, starting with the home clock (simplest) and record screen (most used). `ORIENTATION_TEST` flag in `app_config.h` can be kept as a regression check.

**Touch calibration:** CST820 reports native portrait coords (x: 0–240, y: 0–320). With no LVGL software rotation, the touch driver now needs to map portrait-native coords to the 320×240 logical surface. In `hal_touch.c` `cst820_read_cb`: swap x/y and mirror as needed to match `LCD_MADCTL=0x40` (MX = col mirrored, no row/col swap). Verify by tapping known on-screen targets after each screen is laid out.

*End of migration plan.*
