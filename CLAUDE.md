# CLAUDE.md — tulog-biowatch-tzt

## 1. Project Overview

This repo is a **port of the tulog-biowatch firmware to the TZT ESP32-2432S024C board**. The source firmware was written for a Waveshare ESP32-S3-Touch-AMOLED-2.06; this repo migrates it to a budget TZT 2.4″ ESP32 touchscreen device (ESP32-WROOM-32, ILI9341 TFT, CST820 touch, no PSRAM, 4 MB flash).

**Source project** (Waveshare ESP32-S3 firmware): `/home/benbrandwood/Documents/dev/esp32-s3-watch/`  
**TZT reference materials** (schematics, Arduino demos, datasheets): `/home/benbrandwood/Documents/dev/TZT/2.4inch_ESP32-2432S024-jyc/`  
**Migration plan**: [migration_plan.md](migration_plan.md)

The firmware targets two clinical use cases:

- **Overnight sleep study** — ECG, PPG/SpO₂, nasal thermistor airflow, and ERB chest band for apnoea detection and nocturnal blood pressure variance (BPV) estimation via Pulse Transit Time (PTT).
- **Morning FCG session** — wrist Forcecardiography (FCG) at 1000 Hz for Pre-Ejection Period (PEP) derivation and post-OSA morning BPV tracking.

---

## 2. Migration Status

### Phases complete
- **Phase 1** — scaffold: `CONFIG_IDF_TARGET="esp32"`, BSP removed, managed components resolved, `app_config.h` ported with TZT pin defines, `partitions.csv` redesigned for 4 MB flash.
- **Phase 2** — HAL layer: `hal_backlight.c/h` (LEDC GPIO27), `hal_display.c/h` (ILI9341/SPI2, LVGL 9.5, FreeRTOS mutex, async DMA flush), `hal_touch.c/h` (CST820/I2C0 GPIO33/32). All `bsp_*` symbols eliminated.
- **Phase 3** — peripherals: `hal_storage.c` replaced with `sdspi_host` (SPI3/VSPI); `hal_battery.c` is a permanent stub (IP5306, no sense path); `svc_bp_record.c` DRAM alloc fix (was `MALLOC_CAP_SPIRAM`); `BP_QUEUE_LEN` reduced from 2048 → 512 for DRAM budget.

### Working and tested on hardware
- Boots to Wi-Fi scan screen → home clock screen
- ILI9341 display renders via LVGL 9.5
- CST820 touch input registered in LVGL
- Backlight LEDC PWM on GPIO27
- Wi-Fi scan/connect with up to 3 auto-retries
- NVS credential persistence per SSID
- SNTP time sync; last synced time restored from NVS on reboot
- ECG sampling at 100 Hz — firmware-simulated P-QRS-T waveform
- Pan-Tompkins-inspired QRS detector, heart rate estimate, respiration rate estimate
- Record screen: ECG chart (4 s rolling window, 200 display points), topbar metrics
- SD card recording to CSV at 100 Hz via `sdspi_host`
- B.P. screen — 30/60/120 s recording; 1 kHz sampler; CSV with RR/PAT in µs; post-recording HRV RMSSD + PAT analysis; beat-by-beat chart
- Files screen — SD enumeration, HTTP POST upload to PC receiver, file deletion

### Partially implemented / placeholder
- PPG, RESP, NAS, FCG1, FCG2: recorded as sine-wave simulations; no real sensors
- SpO₂ field in CSV is always 0
- Battery: `battery_read_voltage()` and `battery_read_percent()` permanently return -1 / -1 (IP5306 has no I2C or ADC path to ESP32 on this board)
- About screen is a stub (back button only)

### Not started (Phase 4+)
- UI layout redesign for 240×320 (currently scaled from 410×502 — widgets may overflow or crowd)
- Real ECG via external ADC front-end (GPIO14 = display SPI clock; ADC2 blocked during WiFi — must use SPI/I2C ADC)
- PPG/SpO₂, FCG, nasal, ERB sensor drivers
- MATLAB post-processing pipeline

---

## 3. Hardware Stack

### Target board: TZT ESP32-2432S024C (capacitive touch variant)

| Component | Role | Bus / Interface |
|---|---|---|
| ESP32-WROOM-32 | MCU, dual-core LX6 @ 240 MHz, 520 KB SRAM, **no PSRAM**, 4 MB Flash | — |
| ILI9341 TFT 240×320 | Display | SPI2/HSPI (MOSI GPIO13, MISO GPIO12, CLK GPIO14, CS GPIO15, DC GPIO2) |
| CST820 | Capacitive touch | I²C0 (SDA GPIO33, SCL GPIO32, RST GPIO25, INT GPIO21) |
| IP5306 | LiPo boost charger — **no I2C to ESP32, no ADC sense line** | None — battery monitoring unavailable |
| TF/microSD | Raw data logging, FAT filesystem | SPI3/VSPI (CLK GPIO18, MISO GPIO19, MOSI GPIO23, CS GPIO5) |
| Backlight | LEDC PWM | GPIO27 (active-HIGH, N-channel AO3402A) |
| BOOT button | User input | GPIO0 (active-low, pull-up) |
| RGB LED | Not used in firmware | GPIO4/16/17 (common anode, active-low) |

**Critical pin notes:**
- **GPIO14 = display SPI clock** — never use as ADC. On the source (Waveshare) board this was the I²C SCL and ECG ADC path; on this board it is the LCD clock.
- **GPIO21 = touch INT** on the capacitive variant. The Getting Started PDF shows `TFT_BL=21` — that is for the **resistive** variant. Using GPIO21 as backlight output will drive the touch INT line and break touch.
- **ADC2 blocked during WiFi** on classic ESP32 (unlike S3). All sensor ADC must use ADC1 channels (GPIO32–39).
- **GPIO35 = expansion connector P3**, not a battery sense node. Do not implement ADC on GPIO35.

### Source board (for reference only — not the target)
Waveshare ESP32-S3-Touch-AMOLED-2.06 — ESP32-S3R8, 8 MB OPI PSRAM, 32 MB flash, CO5300 AMOLED QSPI, FT3168 touch, AXP2101 PMU. Source firmware at `/home/benbrandwood/Documents/dev/esp32-s3-watch/`.

---

## 4. Firmware / Software Architecture

### Directory layout
```
main/
├── main.c              # App entry, all screen creation, Wi-Fi, ECG sampler task, BP screen + bp_sampler_task
├── app_config.h        # All numeric/string constants, all TZT pin defines
├── app_state.h         # rec_row_t, bp_row_t, bp_analysis_t, rec_tab_t, health_tab_t
├── ui/
│   ├── ui_common.h     # Colour palette macros, helper declarations
│   └── ui_common.c     # style_screen, style_button, style_card, make_title, etc.
├── hal/
│   ├── hal_backlight.c/h # LEDC PWM backlight, GPIO27
│   ├── hal_battery.c/h   # IP5306 stub — returns -1 permanently
│   ├── hal_display.c/h   # ILI9341 SPI2, LVGL 9.5, FreeRTOS mutex, async DMA flush
│   ├── hal_touch.c/h     # CST820 I2C0 driver, LVGL indev registration
│   └── hal_storage.c/h   # SD card via sdspi_host (SPI3/VSPI) with retries
├── services/
│   ├── svc_nvs.c/h     # NVS Wi-Fi credential helpers
│   ├── svc_record.c/h  # SD writer task, CSV file open/write/close (Record screen)
│   ├── svc_bp_record.c/h # DRAM queue (512 × bp_row_t), writer task, CSV, bp_analyse_file()
│   ├── svc_files.c/h   # SD enumeration, HTTP POST upload, file deletion
│   └── svc_time.c/h    # SNTP sync, NVS time persistence/restore
└── signal/
    ├── sig_pipeline.c/h # ecg_simulate_raw(), signal_bandpass_step()
    └── sig_ppg.c/h      # Two-Gaussian PPG simulation, foot detector, PAT
```

### Tasks and cores
| Task | Core | Priority | Stack | Purpose |
|---|---|---|---|---|
| `ecg_sampler_task` | 0 | 8 | 4096 | 100 Hz sampling, QRS detection, respiration, recording |
| `clock_update_task` | 1 | 4 | 4096 | 1 Hz UI update (time, battery, inactivity timeout) |
| `boot_btn_task` | 1 | 4 | 3072 | BOOT GPIO polling |
| `wifi_scan_task` | any | 5 | 6144 | One-shot Wi-Fi scan; self-deletes |
| `wifi_connect_task` | any | 5 | 8192 | Connect + SNTP sync; self-deletes |
| `sd_writer_task` | 1 | 6 | 4096 | CSV row writer; spawned on REC start, self-deletes on stop |
| `bp_sampler_task` | 1 | 6 | 4096 | 1 kHz BP sampler; reads ECG/PPG ring buffers under spinlock |
| `bp_writer_task` | 1 | 6 | 4096 | BP CSV writer; drains DRAM queue |
| `bp_analysis_task` | any | 3 | 8192 | Post-recording file analysis; one-shot, self-deletes |
| LVGL port task | 1 | 4 | **8192** | LVGL rendering and input (stack increased from 4096 — overflow crash) |

### Key concurrency notes
- `s_ecg_spinlock` (portMUX) protects: `s_ecg_raw[]`, `s_ppg_raw[]`, `s_ecg_write_index`, `s_ecg_total_samples`, `s_ecg_min/max`, `s_ecg_hr_bpm`, `s_ecg_sample_drift_ms`, `s_resp_rate_bpm`, `s_ecg_last_rpeak_expected`.
- `bp_sampler_task` reads ECG/PPG ring buffers under the same `s_ecg_spinlock`.
- All LVGL calls from outside the LVGL port task must be wrapped in `hal_display_lock()`.
- `svc_rec_enqueue()` is called from `ecg_sampler_task` under `portENTER_CRITICAL`.
- `svc_bp_rec_enqueue()` is called from `bp_sampler_task` without a lock (the queue is thread-safe internally).
- `battery_read_*()` is a no-op stub — no I2C or ADC path exists on this board.

### Signal processing pipeline (ECG)
1. `ecg_simulate_raw()` → 12-bit synthetic P-QRS-T sample
2. `signal_bandpass_step()` → first-order HP + LP in series (10–15 Hz)
3. Differentiate → square → moving window integration (15 samples @ 100 Hz)
4. Adaptive threshold from signal/noise level tracking
5. Refractory period (250 ms) guards against double detection
6. BPM computed from RR interval; 3:1 weighted average smoothing

### LVGL 9.5 implementation notes (do not re-derive)
- `lv_color_t` is 3 bytes; draw buffers must allocate `sizeof(uint16_t)` (2 bytes) per pixel for the ILI9341 RGB565 format.
- Flush callback must NOT call `lv_display_flush_ready()` synchronously — wire `on_color_trans_done` ISR callback on the SPI panel IO handle.
- `lv_draw_sw_rgb565_swap(buf, px_count)` must be called in `flush_cb` to correct ILI9341 byte order.
- LVGL 9.5 hangs if a chart with ≥ 400 points exists during the screen-load animation. The record screen chart is built lazily 800 ms after navigation. `REC_CHART_POINTS = 200`.

---

## 5. Design Background

The platform targets obstructive sleep apnoea (OSA) assessment and blood pressure variance (BPV) tracking. OSA causes sympathetic surges at apnoea termination, producing nocturnal BP spikes of 40–80 mmHg SBP. BPV — beat-to-beat fluctuations driven by OSA — is an independent cardiovascular risk predictor.

Key signals:
- **ECG**: R-peak timestamps for PTT and HRV.
- **PPG/SpO₂**: Pulse foot timestamps for PTT; SpO₂ for AHI scoring.
- **Nasal thermistor (NTC)**: Airflow cessation detection.
- **ERB chest band**: Respiratory effort classification (obstructive vs central apnoea).
- **FCG (wrist piezo)**: Pre-Ejection Period (PEP) for contractility-corrected PTT → BP variance.

Planned sensor architecture: ADS1220 or ADS1293 on a dedicated SPI bus (GPIO32/33/25) for ECG/FCG/nasal/ERB; MAX30102 on I2C (GPIO21/22) for PPG/SpO₂. See [IO-pin-plan.md](IO-pin-plan.md) for full GPIO assignment.

---

## 6. Build & Development Workflow

**Prerequisites**: ESP-IDF 6.0.x installed. Source the environment before running `idf.py`.

```sh
. /home/benbrandwood/.espressif/v6.0.1/esp-idf/export.sh

# Build
idf.py build

# Flash (CH340 serial, appears as ttyUSB0 after brltty removal)
idf.py -p /dev/ttyUSB0 flash

# Monitor (UART0 via CH340, 115200 baud)
idf.py -p /dev/ttyUSB0 monitor

# Build + flash + monitor combined
idf.py -p /dev/ttyUSB0 flash monitor

# Clean rebuild
idf.py fullclean && idf.py build
```

**Download mode**: hold BOOT, tap RESET, release BOOT.

**Serial port**: The TZT board uses a CH340 USB-serial converter → `/dev/ttyUSB0`. Unlike the Waveshare board (USB-Serial-JTAG → `/dev/ttyACM0`), this board needs the `ch341` kernel module loaded (`sudo modprobe ch341`) and `brltty` removed (`sudo apt remove brltty`) to get the port to appear.

**Timezone**: set `POSIX_TZ` in [main/app_config.h](main/app_config.h). Current value: `AEST-10AEDT,M10.1.0,M4.1.0/3` (Sydney, Australia).

**ECG source toggle**: `ECG_USE_SIMULATED_SOURCE` in [main/app_config.h](main/app_config.h). Must remain `1` — GPIO14 is the display SPI clock and cannot be used as ADC. Real ECG requires an external SPI/I2C ADC front-end.

---

## 7. Key Files

| File | Purpose |
|---|---|
| [main/main.c](main/main.c) | App entry, all screen UI, Wi-Fi event handling, ECG sampler task, BP screen + `bp_sampler_task` |
| [main/app_config.h](main/app_config.h) | All compile-time constants and TZT pin defines |
| [main/app_state.h](main/app_state.h) | Shared structs: `rec_row_t`, `bp_row_t` (rr_us/pat_us in µs), `bp_analysis_t`, `rec_tab_t`, `health_tab_t` |
| [main/hal/hal_backlight.c](main/hal/hal_backlight.c) | LEDC PWM backlight on GPIO27 |
| [main/hal/hal_display.c](main/hal/hal_display.c) | ILI9341 SPI2, LVGL 9.5 init, async DMA flush, FreeRTOS mutex, `hal_display_lock()` |
| [main/hal/hal_touch.c](main/hal/hal_touch.c) | CST820 I2C0 driver, reset sequence, LVGL indev registration |
| [main/hal/hal_battery.c](main/hal/hal_battery.c) | IP5306 stub — `battery_read_voltage()` = -1.0f, `battery_read_percent()` = -1 permanently |
| [main/hal/hal_storage.c](main/hal/hal_storage.c) | SD card via `sdspi_host` (SPI3/VSPI) with 5-retry mount loop |
| [main/services/svc_record.c](main/services/svc_record.c) | FreeRTOS queue → SD writer task; Record screen CSV lifecycle |
| [main/services/svc_bp_record.c](main/services/svc_bp_record.c) | DRAM queue (512 × `bp_row_t`) → `bp_writer_task`; BP CSV lifecycle; `bp_analyse_file()` |
| [main/services/svc_files.c](main/services/svc_files.c) | SD enumeration, HTTP POST upload to PC receiver, file deletion |
| [main/services/svc_time.c](main/services/svc_time.c) | SNTP sync, NVS time persistence/restore |
| [main/services/svc_nvs.c](main/services/svc_nvs.c) | Per-SSID NVS credential storage |
| [main/signal/sig_pipeline.c](main/signal/sig_pipeline.c) | `ecg_simulate_raw()`, `signal_bandpass_step()` |
| [main/signal/sig_ppg.c](main/signal/sig_ppg.c) | Two-Gaussian PPG simulation, foot detector, PAT |
| [main/ui/ui_common.c](main/ui/ui_common.c) | Shared LVGL style helpers and colour palette |
| [migration_plan.md](migration_plan.md) | Full hardware delta, driver substitution plan, ordered migration steps |
| [IO-pin-plan.md](IO-pin-plan.md) | GPIO assignments for future external biosensor modules |
| [sdkconfig.defaults](sdkconfig.defaults) | KConfig overrides: `esp32` target, 4 MB flash, no PSRAM, UART console, LVGL custom allocator |
| [partitions.csv](partitions.csv) | NVS 24 KB + PHY 4 KB + factory 3.5 MB + FAT storage 448 KB |

---

## 8. Constraints & Engineering Notes

### Memory — no PSRAM
The classic ESP32 has 520 KB SRAM total. WiFi stack consumes ~130 KB leaving ~390 KB for the app. There is no PSRAM.
- **Never use `MALLOC_CAP_SPIRAM`** — will silently return NULL.
- LVGL draw buffers: 2 × 15 360 bytes (240 × 32 × 2) allocated with `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`.
- BP queue: 512 × `bp_row_t` ≈ 20 KB from DRAM (reduced from 2048 on the source board which had PSRAM).
- `s_rec_chart_points[]` and `raw_copy[]` are `static` inside their functions to avoid blowing task stacks.
- Monitor `esp_get_free_heap_size()` under load; target > 30 KB free headroom.

### Flash — 4 MB
Source board had 32 MB. Partition table redesigned: factory app ≤ 3.5 MB, internal FAT 448 KB. Current app binary ≈ 1.46 MB (58% of partition free).

### Sampling rates
- ECG (simulated): 100 Hz, 4 s window (400 samples), displayed at 50 ms refresh
- Target (Stage 2): ECG/FCG at 1000 Hz, nasal/ERB at 50 Hz, PPG at 100–250 Hz

### Timing / real-time requirements
- `ecg_sampler_task` Core 0, priority 8 — uses `esp_timer_get_time()` with `esp_rom_delay_us()` for sub-ms busy-wait.
- `bp_sampler_task` Core 1, priority 6 — uses `vTaskDelay(1)` for remaining wait each 1 ms tick. **Must not use `esp_rom_delay_us` here** — busy-waits the full sleep and starves IDLE1, causing task watchdog reset within seconds.
- Both `svc_record.c` and `svc_bp_record.c` call `fsync(fileno(file))` after periodic `fflush()` to commit the FATFS directory entry. Without `fsync`, a watchdog reset before `fclose()` leaves a 0-byte file on disk.

### ADC constraints (classic ESP32)
- ADC2 is blocked during WiFi — never use GPIO0, 2, 4, 12–15, 25–27 for sensor ADC.
- ADC1 channels (GPIO32–39) are WiFi-safe.
- GPIO35 is input-only and routes to the expansion header, not a battery sense node.
- GPIO14 = display SPI clock — never use as ADC.

---

## 9. Known Issues / TODOs

| Issue | Location | Notes |
|---|---|---|
| UI layout not redesigned for 240×320 | All UI files in main.c | Source UI was 410×502; widgets are scaled but not properly laid out for portrait 240×320 |
| `spo2` field always 0 | `ecg_sampler_task` in main.c | No optical sensor integrated |
| PPG/NAS/FCG recorded as sine waves | `ecg_sampler_task` in main.c | Placeholder until real sensors added |
| Drain path in `sd_writer_task` missing `hr_bpm` | svc_record.c | `snprintf` in drain loop has 11 format args, CSV header has 12 columns |
| `ui_create_settings_screen()` called twice in `app_main` | main.c | Duplicate call overwrites `s_scr_settings`; harmless but wasteful |
| Wi-Fi service (Phase 6D) not extracted | main.c | Wi-Fi state and UI callbacks remain entangled in main.c |
| ECG sampler task (Phase 6F) not extracted | main.c | All ECG/respiration state remains in main.c |
| BP screen shows "Analysing" immediately (no recording) | `svc_bp_record.c`, `bp_ui_timer_cb` | Intermittent — transient SD mount failure on `fopen()` causes zero-sample recording |

---

## 10. Working Conventions for Claude

- **This is the TZT target repo.** The Waveshare ESP32-S3 source is at `/home/benbrandwood/Documents/dev/esp32-s3-watch/`. TZT reference materials (schematics, Arduino demos) are at `/home/benbrandwood/Documents/dev/TZT/2.4inch_ESP32-2432S024-jyc/`. Read those when verifying hardware behaviour.
- **No PSRAM.** Never use `MALLOC_CAP_SPIRAM`. All dynamic allocation uses `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` or plain `malloc`.
- **GPIO14 = display clock.** `ECG_USE_SIMULATED_SOURCE = 1` must not be changed without wiring an external SPI/I2C ADC front-end.
- **Battery monitoring unavailable.** `battery_read_voltage()` and `battery_read_percent()` return -1 permanently. Do not attempt to implement ADC on GPIO35 or I2C on the IP5306.
- **BP screen uses µs for RR and PAT.** `bp_row_t.rr_us` and `bp_row_t.pat_us` are `int32_t` microseconds. `rec_row_t.rr_ms` / `pat_ms` are `int16_t` milliseconds — do not conflate the two.
- **LVGL must always be called under `hal_display_lock()`** from tasks other than the LVGL port task.
- **The spinlock (`s_ecg_spinlock`) is a portMUX**, not a FreeRTOS mutex — use `portENTER_CRITICAL` / `portEXIT_CRITICAL`.
- **Read code before making architectural claims.** Verify sensor integration by looking at actual driver files, not docs.
- **Keep diffs small.** Prefer extracting one module at a time.
- **Check migration_plan.md** before proposing structural changes.
- **Use Australian English** in all documentation (organise, behaviour, colour, practise, recognise, modularise).

---

## 11. Display Orientation — SOLVED (2026-06-06)

**Status: SOLVED.** Working config documented below and in README.md §Screen Orientation.
Goal was to run the ILI9341 in
**landscape 320×240, USB on the left, readable (non-mirrored) text, full-screen fill,
correct colours.** A four-quadrant test pattern is gated behind `ORIENTATION_TEST` in
[main/app_config.h](main/app_config.h) (set to `1` = test screen, `0` = normal UI). The
test pattern lives in [main/main.c](main/main.c) around the `#if ORIENTATION_TEST` block
(`MK_Q` macro draws RED top-left, GREEN top-right, BLUE bottom-left, YELLOW bottom-right,
plus a "v USB v" label).

### SOLUTION (confirmed working on hardware 2026-06-06)

- `LCD_H_RES=320, LCD_V_RES=240` — panel is physically landscape-native (320 col × 240 row)
- `LCD_MADCTL=0x40` — MX bit (mirror column scan), no MV, BGR bit clear
- `rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB` + `lv_draw_sw_rgb565_swap()` in flush
- Write full MADCTL byte via `esp_lcd_panel_io_tx_param(io, 0x36, {0x40}, 1)` **before** GRAM clear
- No `esp_lcd_panel_swap_xy()` / `mirror()` — they only OR individual bits onto stale init state
- No LVGL software rotation — not needed

### Ground truth from the working TZT reference demos (verified, trust these)
Source: `/home/benbrandwood/Documents/dev/TZT/2.4inch_ESP32-2432S024-jyc/1-Demo/Demo_Arduino/`
- Panel is **ILI9341, native 240×320 portrait**, `offset_x/y = 0`, `invert = false`.
  (`7_1_..._LovyanGFX` Panel_ILI9341 cfg: memory/panel 240×320; `1_2_Factory...` TFT_eSPI
  `User_Setup.h`: `ILI9341_DRIVER`, `TFT_WIDTH 240`, `TFT_HEIGHT 320`.)
- **Colour order = RGB + byte-swap.** `7_3_..._LovyanGFX` uses `rgb_order = false` +
  `tft.setSwapBytes(true)`, and labels `fillScreen(0xF800)` as 红 (red). `0xF800` is pure
  red in RGB565, so to display as red the panel must be in **RGB element order** with a
  byte (endian) swap. → in esp_lcd use `LCD_RGB_ELEMENT_ORDER_RGB` **and** keep
  `lv_draw_sw_rgb565_swap()` in the flush. (The original code used BGR, which made red
  render as blue — that was a real bug, now fixed.)
- **USB-left = TFT_eSPI rotation 3.** `7_3_..._LovyanGFX` setup():
  `tft.setRotation(0); // USB Right` and `//tft.setRotation(3); // USB Left`.
  For ILI9341, TFT_eSPI rotation 3 MADCTL = `0xE8` = MV | MX | MY | BGR
  (rotation 1 = `0x28` = MV | BGR is the other landscape, USB right).

### What has been tried in this repo and the observed result
All edits were to [main/hal/hal_display.c](main/hal/hal_display.c) +
[main/app_config.h](main/app_config.h). LVGL display object is created at
`(LCD_H_RES, LCD_V_RES)`. The GRAM is cleared in **native 240×320 coords** in a loop
**before** swap/mirror are applied (see `s_clear_line[240]` loop) — that part is fine.

- **Config A (original baseline):** `LCD_H_RES=240, LCD_V_RES=320`,
  `swap_xy=false`, `mirror(true,false)`, `rgb_ele_order=BGR`, draw-buf 32 lines.
  Result (held landscape, USB-left): coherent content but **portrait** — fills only the
  left ~3/4, **grey snowy strip in the right ~1/4** (leftover Arduino-demo GRAM, never
  written because LVGL is only 240 px on the 320-px-wide axis); **colours R/B-swapped**
  (red showed blue); text readable.
- **Config B (current on disk + flashed):** `LCD_H_RES=320, LCD_V_RES=240`,
  `swap_xy=true`, `mirror(false,false)`, `rgb_ele_order=RGB`, draw-buf reduced to 24
  lines (keeps per-buffer bytes = old 15 360, so DRAM budget unchanged).
  Result: **far worse — fine vertical stripes** (orange/yellow over left 1/3, blue over
  middle 1/3) and the **grey snowy strip still on the right 1/3**. Text unreadable.

### Diagnosis / leading hypothesis
The vertical-stripe shear is the signature of a **pixel-buffer / address-window width
mismatch** introduced by hardware `esp_lcd_panel_swap_xy(true)`. `esp_lcd_panel_draw_bitmap`
swaps only the CASET/RASET *window* coords; it does **not** transpose the pixel buffer.
On this IDF 6.0.1 `esp_lcd_ili9341` component the MV-bit transpose does not line up with
LVGL's row-major partial buffer, so a 320-wide LVGL area is streamed into a 240-wide
physical GRAM row and shears. The colour order (RGB) and the chosen mirror flags are
**not** the cause of the stripes — mirrors only flip, they cannot shear.

### Recommended next step (for opus) — two viable paths
1. **Preferred: LVGL software rotation, keep the panel in its coherent native portrait
   addressing.** Set `swap_xy=false` (Config-A addressing, which rendered coherent
   pixels), keep `rgb_ele_order=RGB` + byte-swap, create the LVGL display at the *native*
   240×320, then call `lv_display_set_rotation(s_disp, LV_DISP_ROTATION_90)` (try 90 and
   270 — one gives USB-left). LVGL transposes pixels in software so the buffer matches the
   native window → no shear, and because the logical surface is now 320×240 it fills the
   whole panel (kills the grey strip). Caveat: LVGL 9 software rotation in
   `RENDER_MODE_PARTIAL` may need a full-width draw buffer or `RENDER_MODE_FULL`; watch
   DRAM (no PSRAM — a full 320×240×2 = 150 KB framebuffer will NOT fit, so partial +
   rotation is required, verify it renders without shear).
2. **Alternative: bypass esp_lcd's coord-swap and push MADCTL `0xE8` directly** via
   `esp_lcd_panel_io_tx_param(io, 0x36, (uint8_t[]){0xE8}, 1)`, create LVGL at 320×240,
   and do **not** call `esp_lcd_panel_swap_xy`/`mirror`. This replicates TFT_eSPI
   rotation-3 exactly. Risk: it still relies on the MV bit, so if the shear is truly a
   buffer-transpose issue it may persist — test the test-pattern before trusting it.

Verify on the `ORIENTATION_TEST=1` four-quadrant screen first (RED must be physically
top-left, full-screen fill, no grey strip), then set `ORIENTATION_TEST=0`. **Note:** the
real UI screens in main.c were laid out for 240×320 portrait and will need a separate
240×320→320×240 layout pass once orientation/colour plumbing is correct.

### Build / flash for this investigation
`. /home/benbrandwood/.espressif/v6.0.1/esp-idf/export.sh && idf.py build && idf.py -p /dev/ttyUSB0 flash`
(board is a CH340 → `/dev/ttyUSB0`). Current binary ≈ 1.27 MB, 65% partition free.

---

### UPDATE 2026-06-05 (later) — software rotation IMPLEMENTED, partially working

**Root cause confirmed by reading the component source:**
`managed_components/espressif__esp_lcd_ili9341/esp_lcd_ili9341.c` `panel_ili9341_draw_bitmap`
(lines 288-317) sets `CASET=x_start..x_end` / `RASET=y_start..y_end` **verbatim — it never
transposes coordinates for `swap_xy`.** So with MV set, LVGL streams row-major while the
controller fills column-first → shear + grey strip. Hardware `swap_xy` is therefore unusable
with this component. Also confirmed: **LVGL 9.5 has no automatic software pixel-rotation for
PARTIAL render mode** (core rotation is gated behind `matrix_rotation`, which needs
`LV_DRAW_TRANSFORM_USE_MATRIX` + a FULL/DIRECT framebuffer = 320×240×2 = 150 KB, impossible
without PSRAM). So rotation must be done by hand in `flush_cb`.

**What is now implemented (current code on disk + flashed):**
- `app_config.h`: `LCD_H_RES=320, LCD_V_RES=240` (logical), `LCD_NATIVE_W=240, LCD_NATIVE_H=320`,
  and tunable macros `LCD_LV_ROTATION` (=`LV_DISPLAY_ROTATION_270`), `LCD_MIRROR_X` (=false),
  `LCD_MIRROR_Y` (=false).
- `hal_display.c`: panel kept in NATIVE portrait (`swap_xy=false`, `mirror(MIRROR_X,MIRROR_Y)`),
  `rgb_ele_order=RGB`. Display created at native 240×320 then `lv_display_set_rotation(...270)`.
  `flush_cb` now rotates every chunk in software: `lv_display_rotate_area()` to map the logical
  area → native panel rect, `lv_draw_sw_rotate()` into a 3rd buffer `s_rot_buf`, then
  `lv_draw_sw_rgb565_swap()`, then `esp_lcd_panel_draw_bitmap()`. Buffers: 2 draw + 1 rotation,
  each `320×16×2 = 10 240 B` (≈30 KB total, memory-neutral).
- Boot log healthy: `Display: Init OK — ILI9341 320×240, 2×10240-byte draw buffers`, no crash,
  heap fine (DRAM 75 KB + 111 KB regions free at init). (SD mount fails = no card inserted,
  unrelated.)

**Observed result on the ORIENTATION_TEST screen (photo 2026-06-05):**
- ✅ **Colours now CORRECT** — RGB element order + byte-swap is the right combo (keep it).
- ❌ **Text is MIRROR-REVERSED (backward).**
- ❌ **Quadrants scrambled / DUPLICATED** — green appears on BOTH left and right, yellow-green on
  both bottom sides, a red column up the centre-top and blue column centre-bottom.
- ❌ **Grey strip still on the right**; image still looks essentially portrait, 90° out on the
  short landscape axis. USB label points at the USB.

**Interpretation / where to start tomorrow:**
1. **Backward text = a mirror problem.** First cheap thing to try: flip the mirror macros —
   set `LCD_MIRROR_X true` (and/or `LCD_MIRROR_Y true`) in `app_config.h`, and try
   `LCD_LV_ROTATION = LV_DISPLAY_ROTATION_90`. These are one-line edits + reflash. Sweep the
   4 combos (rot 90/270 × mirror_x/_y) — but note the duplication below may need fixing first.
2. **The duplication/scramble is the real blocker** and is NOT just a mirror/rotation choice —
   it means the per-chunk rotate *placement* is wrong for PARTIAL mode. Prime suspects:
   - `src_stride`: the px_map LVGL hands to `flush_cb` may have a stride wider than `w` (buffer
     stride, not area stride). We used `lv_draw_buf_width_to_stride(w,cf)`; verify against the
     actual buffer. If LVGL renders into a fixed-width (320) buffer and passes a sub-area,
     the row stride is the BUFFER width, not the area width → would shear/duplicate exactly
     like this. **This is the most likely bug.** Add a temporary `ESP_LOGI` in `flush_cb`
     dumping `area->x1/y1/x2/y2` and the computed `phys` rect + strides for the first ~6 flushes
     and confirm they tile the native panel without overlap.
   - Confirm `lv_draw_sw_rotate` dest layout matches what `esp_lcd_panel_draw_bitmap` expects
     (tight, dest_stride = phys_rect_width × 2).
3. If partial-rotation keeps fighting, the debuggable fallback is a hand-written transpose loop
   in `flush_cb` (slower, but you control every index) — or render full-width strips only and
   place them as vertical native strips, verifying x-offset increments correctly.

**Reference ground truth still stands:** panel native 240×320, RGB+byteswap = correct colour,
TFT_eSPI rotation 3 = USB-left. The colour half of the problem is SOLVED; only the geometry
(rotation placement + mirror) remains.
