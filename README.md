# tulog-biowatch-tzt

> **TZT ESP32-2432S024C port.** The original Waveshare ESP32-S3 source firmware is
> at `/home/benbrandwood/Documents/dev/esp32-s3-watch/`. See [CLAUDE.md](CLAUDE.md)
> and [migration_plan.md](migration_plan.md) for the full migration context.

A wearable biosignal acquisition and display platform built on the budget **TZT
ESP32-2432S024C** 2.4″ touchscreen board (ESP32-WROOM-32, ILI9341 TFT, XPT2046
resistive touch). The firmware implements a watch UI, Wi-Fi + SNTP time
synchronisation, ECG/PPG/SpO₂/accelerometer signal acquisition, and SD card data
logging as a validated foundation prior to integrating the full external sensor suite.

---

## Background and Design Intent

### Research goal

The platform targets two clinical measurement modes:

**Overnight sleep study** — captures ECG, PPG/SpO₂, nasal thermistor airflow, and
ERB respiratory effort band for obstructive sleep apnoea (OSA) detection and
nocturnal Pulse Transit Time (PTT) based blood pressure estimation. OSA produces
sympathetic surges at apnoea termination, causing nocturnal BP spikes of 40–80 mmHg
SBP. Augmented beat-to-beat BPV is an independent cardiovascular risk predictor
beyond mean BP alone.

**Morning FCG session** — captures wrist Forcecardiography (FCG) + ECG + PPG
immediately after waking. FCG-derived Pre-Ejection Period (PEP) correction decouples
cardiac contractility from peripheral vascular resistance, producing PEP-corrected
PTT → beat-to-beat BP variance. The primary research outcome is within-subject
correlation between the previous night's Apnoea-Hypopnoea Index (AHI) and the
following morning's BPV.

### Two-stage implementation

**Stage 1 (current)** — prove the firmware, signal pipeline, SD logging, timestamping,
Wi-Fi, and display with the minimum viable hardware. Simulated ECG and sine-wave
placeholders allow the full software stack to be validated without all external sensor
hardware.

**Stage 2 (planned)** — integrate the full sensor suite: ADS1293 (3-lead ECG), ADS1220
(nasal thermistor, ERB, FCG on SPI2), MAX30102 (optical PPG/SpO₂ on I²C), and
MPU-6050/6500 (accelerometer on I²C). The Stage 1 architecture is designed to make
this transition minimally disruptive.

---

## Current Status

| Feature | Status |
|---|---|
| Display (ILI9341 320×240 landscape), backlight PWM | Working |
| XPT2046 resistive touch, LVGL 9.5 | Working |
| Home screen clock, inactivity timeout | Working |
| Wi-Fi scan, connect, NVS credential persistence | Working |
| SNTP time sync; last-synced time restored on reboot | Working |
| ECG Lead I (simulated) — QRS detection, HR, respiration | Working |
| ECG Lead I/II/III (ADS1293 on SPI2) | Working — driver integrated, 853 SPS |
| PPG IR + Red (MAX30102 on I²C) | Working — driver integrated, 100 SPS |
| SpO₂ (MAX30102 Welford accumulator, ~4 s window) | Working — CSV column correct; topbar label not yet wired |
| Accelerometer X/Y/Z (MPU-6050/6500 on I²C) | Working — driver integrated, 100 Hz, CSV in m/s² |
| SD card mount and CSV recording at 100 Hz | Working |
| Record screen — 4 s rolling chart, 6 tabs (ECG/PPG/RESP/NAS/FCG1/Accel) | Working |
| B.P. screen — 30/60/120 s recording; 1 kHz sampler; post-recording analysis | Working |
| Files screen — SD enumeration, HTTP POST upload, file deletion | Working |
| Pan-Tompkins QRS detection, PAT, RR interval | Working |
| Battery percent | Unavailable — IP5306 has no I²C or ADC path to ESP32 |
| PPG foot, PAT (real sensor) | Working |
| RESP / NAS / FCG1 channels | Simulated sine waves |
| SpO₂ topbar display | Pending — field wired to CSV but topbar label not yet updated |

---

## Hardware Overview

### Target board: TZT ESP32-2432S024C

| Component | Role | Bus / Interface |
|---|---|---|
| ESP32-WROOM-32 | MCU, dual-core LX6 @ 240 MHz, 520 KB SRAM, **no PSRAM**, 4 MB flash | — |
| ILI9341 TFT 240×320 | Display — run in landscape 320×240 | SPI2/HSPI |
| XPT2046 | Resistive touch | SPI2 (shared with display), CS=GPIO33 |
| IP5306 | LiPo boost charger — **no I²C path, no ADC sense line** | None |
| TF/microSD | Raw data logging, FAT filesystem | SPI3/VSPI |
| Backlight | LEDC PWM (active-HIGH, N-channel AO3402A) | GPIO27 |
| BOOT button | User input | GPIO0 (active-low, pull-up) |
| RGB LED | Not used in firmware | GPIO4/16/17 (common anode, active-low) |

### GPIO assignments

| Signal | GPIO | Notes |
|---|---|---|
| Display MOSI | 13 | SPI2 shared with touch and biosensor SPI |
| Display MISO | 12 | SPI2 shared |
| Display SCLK | 14 | **Never use as ADC** — display clock |
| Display CS | 15 | — |
| Display DC | 2 | — |
| Backlight | 27 | LEDC PWM |
| Touch CS (XPT2046) | 33 | SPI2 shared |
| SD SCLK | 18 | SPI3/VSPI |
| SD MISO | 19 | SPI3/VSPI |
| SD MOSI | 23 | SPI3/VSPI |
| SD CS | 5 | SPI3/VSPI |
| I²C SDA | 21 | Shared: MAX30102 + MPU-6050/6500 |
| I²C SCL | 22 | Shared: MAX30102 + MPU-6050/6500 |
| ADS1293 CS | 16 | SPI2 (LED B resistor R20 removed) |
| ADS1293 DRDY | 4 | Active-low; no external pull-up (R17 removed with LED G) |
| MAX30102 INT | 32 | Not used — reads unconditional |
| MPU-6050/6500 INT | 25 | Not used — reads unconditional |
| BOOT button | 0 | Active-low |
| UART0 TX | 1 | Console |
| UART0 RX | 3 | Console |

### Biosensor wiring

| Sensor | Bus | Address / CS | Integrated |
|---|---|---|---|
| ADS1293 (ECG 3-lead) | SPI2, GPIO16 CS | — | Yes |
| MAX30102 (PPG/SpO₂) | I²C0, GPIO21/22 | 0x57 | Yes |
| MPU-6050/6500 (Accel) | I²C0, GPIO21/22 | 0x68 | Yes |

---

## Firmware Overview

### Codebase structure

```
main/
├── main.c                    # App entry, all screen UI, Wi-Fi, ECG sampler task, BP screen
├── app_config.h              # All compile-time constants and TZT pin defines
├── app_state.h               # Shared structs: rec_row_t, bp_row_t, bp_analysis_t, enums
├── ui/
│   └── ui_common.c/h        # Shared LVGL style helpers and colour palette
├── hal/
│   ├── hal_backlight.c/h    # LEDC PWM backlight on GPIO27
│   ├── hal_battery.c/h      # IP5306 stub — returns -1 permanently
│   ├── hal_display.c/h      # ILI9341 SPI2, LVGL 9.5, async DMA flush, display mutex
│   ├── hal_touch.c/h        # XPT2046 SPI2, Z-pressure detection, LVGL indev
│   ├── hal_storage.c/h      # SD card via sdspi_host (SPI3/VSPI) with 5-retry mount
│   ├── hal_bus.c/h          # I²C0 bus init; hal_i2c_reg_read/write; SPI dev add
│   ├── hal_ads1293.c/h      # ADS1293 3-lead ECG front-end, SPI2
│   ├── hal_max30102.c/h     # MAX30102 PPG/SpO₂, I²C0
│   └── hal_mpu6050.c/h      # MPU-6050/6500 accelerometer, I²C0
├── services/
│   ├── svc_biosignal_acq.c/h # 100 Hz sensor step; publishes biosignal_frame_t
│   ├── svc_nvs.c/h          # Per-SSID NVS Wi-Fi credential storage
│   ├── svc_record.c/h       # SD writer task, CSV file open/write/close
│   ├── svc_bp_record.c/h    # DRAM queue (512 × bp_row_t), BP writer, bp_analyse_file()
│   ├── svc_files.c/h        # SD enumeration, HTTP POST upload, file deletion
│   └── svc_time.c/h         # SNTP sync and NVS time persistence/restore
└── signal/
    ├── sig_pipeline.c/h     # ecg_simulate_raw(), signal_bandpass_step()
    └── sig_ppg.c/h          # Two-Gaussian PPG simulation, foot detector, PAT
```

### Tasks

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
| LVGL port task | 1 | 4 | 8192 | LVGL rendering and input |

### Signal pipeline

```
ADS1293 (SPI2)              ← ECG Lead I/II/III, ~853 SPS
        │
        ▼
ecg_adc_read_raw()          right-shift 12 bits → ±2048 pipeline scale
        │
        ▼
signal_bandpass_step()      10–15 Hz → s_ecg_band[] ring buffer
        │
        ├──→ Pan-Tompkins QRS detector (MWI) → arm deferred R-peak search
        │        └──→ [200 ms later] bidirectional argmax on s_ecg_band[]
        │                  └──→ refined r_peak_ms → RR interval → HR estimate
        │
MAX30102 (I²C)              ← PPG IR/Red, 100 SPS, 18-bit
        │
        ▼
signal_bandpass_step()      0.5–16 Hz PPG bandpass
        │
        ├──→ ppg_det_update_sample() → PAT foot detection → pat_ms, topbar
        └──→ s_ppg_raw[] ring buffer → PPG chart display
        │
MPU-6050/6500 (I²C)        ← 3-axis accel, 100 Hz, ±2g, 16384 LSB/g
        │
        ▼
raw int16_t × 9.81 / 16384  → m/s² (float) → CSV + Accel chart
        │
        ▼
rec_row_t (ECG + PPG + Accel + RESP/NAS/FCG sines + drift + rr_ms + pat_ms)
        │
        ▼
svc_rec_enqueue() → FreeRTOS queue → sd_writer_task → FAT CSV on SD
```

See [docs/SIGNAL_PROCESSING.md](docs/SIGNAL_PROCESSING.md) for the full pipeline
description including QRS detection constants, PAT accuracy analysis, and SpO₂
algorithm.

---

## Build and Flash Instructions

### Requirements

- **ESP-IDF 6.0.x** — source the environment before running `idf.py`.
- On first build, the IDF component manager downloads managed components
  automatically (internet required).
- The TZT board uses a **CH340 USB-serial converter** → `/dev/ttyUSB0`.
  If the port does not appear: `sudo modprobe ch341` and remove `brltty`
  (`sudo apt remove brltty`).

### Commands

```sh
# Source ESP-IDF
. /home/benbrandwood/.espressif/v6.0.1/esp-idf/export.sh

# Build
idf.py build

# Flash (hold BOOT, tap RESET, release BOOT to enter download mode)
idf.py -p /dev/ttyUSB0 flash

# Serial monitor (115200 baud via CH340)
idf.py -p /dev/ttyUSB0 monitor

# Build + flash + monitor combined
idf.py -p /dev/ttyUSB0 flash monitor

# Clean rebuild
idf.py fullclean && idf.py build
```

### Timezone configuration

Edit `POSIX_TZ` in [main/app_config.h](main/app_config.h):

```c
#define POSIX_TZ  "AEST-10AEDT,M10.1.0,M4.1.0/3"   // Sydney, Australia (default)
```

---

## Screen Orientation — ILI9341 on the TZT ESP32-2432S024C

The ILI9341 panel on this board is physically mounted in landscape orientation
(320 columns wide × 240 rows tall). Working configuration in
[main/app_config.h](main/app_config.h) and [main/hal/hal_display.c](main/hal/hal_display.c):

| Setting | Value | Reason |
|---|---|---|
| `LCD_H_RES` | `320` | Logical width — matches 320-column physical panel |
| `LCD_V_RES` | `240` | Logical height — matches 240-row physical panel |
| `LCD_MADCTL` | `0x40` | MX bit (mirror column scan); no MV; BGR bit clear |
| `rgb_ele_order` | `LCD_RGB_ELEMENT_ORDER_RGB` | Panel is RGB order |
| `lv_draw_sw_rgb565_swap()` | in flush callback | Corrects SPI big-endian byte order |

**Critical sequencing rule**: write MADCTL *before* the GRAM clear. Write the full
byte directly via `esp_lcd_panel_io_tx_param(io, 0x36, {0x40}, 1)` — not via
`esp_lcd_panel_swap_xy()` or `mirror()`, which only OR individual bits onto stale
init state and cause shear. See [CLAUDE.md §11](CLAUDE.md) for the full
investigation record.

---

## Touchscreen — XPT2046 Resistive

The TZT 2432S024 ships in resistive (XPT2046) and capacitive (CST820) variants.
This board is the **resistive variant**; GPIO33 is the touch SPI CS, not I²C SDA.
GPIO36 (PENIRQ) is not routed to the ESP32 — touch detection uses Z-pressure only:

```
Z = 4095 + Z1 − Z2 > 350
```

Calibration constants (measured on hardware 2026-06-06):

| Constant | Value |
|---|---|
| `XPT_X_MIN` | 650 |
| `XPT_X_MAX` | 3200 |
| `XPT_Y_MIN` | 650 |
| `XPT_Y_MAX` | 3100 |
| `XPT_Z_THRESHOLD` | 350 |

Both axes are inverted; no axis swap is needed.

---

## Documentation

| File | Contents |
|---|---|
| [CLAUDE.md](CLAUDE.md) | Project instructions, migration status, hardware stack, constraints |
| [migration_plan.md](migration_plan.md) | Phase-by-phase Waveshare → TZT migration log |
| [IO-pin-plan.md](IO-pin-plan.md) | GPIO assignments for all biosensor modules |
| [biosignals_plan.md](biosignals_plan.md) | Sensor integration roadmap |
| [docs/SIGNAL_PROCESSING.md](docs/SIGNAL_PROCESSING.md) | ECG/PPG/PAT/SpO₂/Accel algorithms, CSV format |
| [docs/ads1293.md](docs/ads1293.md) | ADS1293 ECG front-end driver reference |
| [docs/MAX30102.md](docs/MAX30102.md) | MAX30102 PPG/SpO₂ driver reference |
| [docs/mpu-6050-6500.md](docs/mpu-6050-6500.md) | MPU-6050/6500 accelerometer driver reference |

---

## Roadmap

### Active / next priorities

1. **ADS1293 electrode validation** — attach Ag/AgCl electrodes and confirm CH1 raw
   values change with ECG contact. Tune `ECG_QRS_GAIN` in `app_config.h` until HR
   reads correctly.

2. **SpO₂ topbar** — wire `s_lbl_rec_spo2` label to `hal_max30102_get_spo2()`
   result in `clock_update_task`.

3. **PC companion receiver** — implement the Python Stage 1 receiver described in
   `docs/pc_receiver_stage_plan.md`. Accept `POST /upload` on port 8000; save files
   to `inbox/record/` or `inbox/bp/`; log uploads.

4. **FCG (wrist piezo)** — connect PVDF or PZT sensor to ADS1220 on SPI2; validate
   low-frequency cardiomechanical signal (0.5–30 Hz) and PEP extraction.

5. **Nasal thermistor + ERB** — connect to ADS1220 ch2/ch3; validate cessation
   detection and respiratory effort classification.

### Firmware refactor backlog

- **Wi-Fi service extraction (`svc_wifi`)** — Wi-Fi tasks and LVGL callbacks are
  currently entangled in main.c.
- **ECG sampler task extraction (`sig_ecg`)** — all ECG/respiration state and the
  ring buffer must move to a dedicated module.
- **BP sampler upgrade** — currently reads 100 Hz ECG/PPG ring buffers; should call
  `hal_ads1293_read_ecg()` directly at 1 kHz for true 1 kHz PTT resolution.

---

## Licence

MIT — free to use, modify, and distribute.
