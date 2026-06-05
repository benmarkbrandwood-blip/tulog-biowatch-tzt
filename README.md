# tulog-biowatch-tzt

> **This is the TZT ESP32-2432S024C port.** The original Waveshare ESP32-S3 source firmware is at `/home/benbrandwood/Documents/dev/esp32-s3-watch/`. See [CLAUDE.md](CLAUDE.md) and [migration_plan.md](migration_plan.md) for the full migration context.

A wearable biosignal acquisition and display platform built on the **Waveshare ESP32-S3-Touch-AMOLED-2.06** development board. The firmware implements a complete watch UI, Wi-Fi + SNTP time synchronisation, ECG signal processing, and SD card data logging as a validated Stage 1 software foundation, prior to integrating the full external sensor suite. 

---

## Background and Design Intent

### Research goal

The platform targets two clinical measurement modes:

**Overnight sleep study** — captures ECG, PPG/SpO₂, nasal thermistor airflow, and ERB respiratory effort band for obstructive sleep apnoea (OSA) detection and nocturnal Pulse Transit Time (PTT) based blood pressure estimation. OSA produces sympathetic surges at apnoea termination, causing nocturnal BP spikes of 40–80 mmHg SBP. Augmented beat-to-beat BPV is an independent cardiovascular risk predictor beyond mean BP alone.

**Morning FCG session** — captures wrist Forcecardiography (FCG) + ECG + PPG at up to 1000 Hz immediately after waking. FCG-derived Pre-Ejection Period (PEP) correction decouples cardiac contractility from peripheral vascular resistance, producing PEP-corrected PTT → beat-to-beat BP variance. The primary research outcome is within-subject correlation between the previous night's Apnoea-Hypopnoea Index (AHI) and the following morning's BPV.

### Two-stage implementation

The design follows a deliberate staged approach, documented in *watch_design_plan_two_stage.docx*:

**Stage 1 (current)** — prove the firmware, signal pipeline, SD logging, timestamping, Wi-Fi, and display with the minimum viable hardware. The simulated ECG waveform and sine-wave placeholders allow the full software stack to be validated without external sensor hardware, and allow signal processing algorithms to be developed and tested immediately.

**Stage 2 (planned)** — integrate the full sensor suite: ADS127L18 (8-channel 24-bit ΔΣ ADC for FCG, ECG, nasal thermistor, and ERB on SPI2), MAX86140 (optical PPG/SpO₂ on SPI3), and AD8232 (single-lead ECG analogue front-end). The Stage 1 firmware architecture is explicitly designed to make this transition minimally disruptive — replacing `ecg_simulate_raw()` with a real ADC read in the existing sampler task is the primary code change needed for ECG.

---

## Current Status

**Stage 1 firmware — software infrastructure validated; no real external sensors integrated.**

| Feature | Status |
|---|---|
| Display, touch, home clock screen | Working |
| Wi-Fi scan, connect, NVS credential persistence | Working |
| SNTP time sync; last-synced time restored from NVS on reboot | Working |
| Battery percent and voltage via AXP2101 PMU (I²C) | Working |
| ECG sampling at 100 Hz | Simulated (firmware-generated waveform) |
| Pan-Tompkins QRS detection, heart rate estimate | Working (on simulated signal) |
| Respiration rate estimate | Working (on simulated signal) |
| SD card mount and CSV recording at 100 Hz | Working |
| Record screen with 4 s rolling chart | Working |
| PPG channel | Simulated — beat-synchronised two-Gaussian waveform, 0.5–16 Hz bandpass filtered, PAT detection active |
| RESP, NAS, FCG1, FCG2 channels | Simulated sine waves |
| PAT (Pulse Arrival Time) | Working — R-peak to PPG foot; displayed on topbar, logged per-beat in CSV |
| SpO₂ in CSV | Always 0 — MAX86140 not integrated |
| B.P. screen | Working — selectable 30/60/120 s recording; 1 kHz sampler; CSV with RR/PAT in µs; post-recording HRV RMSSD + PAT analysis; beat-by-beat chart |
| Files screen | Working — lists SD card files (name, size, type); select, send via HTTP POST to PC receiver, or delete; Wi-Fi connect and back-navigation integrated |
| ADS127L18 driver | Not started |
| MAX86140 driver | Not started |
| AD8232 integration | Not started |
| PCF85063 RTC time correction | Not integrated |
| QMI8658 IMU data path | Not integrated |

---

## Hardware Overview

### Platform board

**Waveshare ESP32-S3-Touch-AMOLED-2.06** — 50.80 × 42.00 mm watch-form development board.

| Component | Detail |
|---|---|
| MCU | ESP32-S3R8, dual-core LX7 @ 240 MHz, 8 MB OPI PSRAM, 32 MB Flash |
| Display | 2.06″ AMOLED 410 × 502 px, CO5300 driver, QSPI interface |
| Touch | FT3168 capacitive, I²C |
| PMU | AXP2101 — LiPo charging, battery monitoring (I²C 0x34) |
| RTC | PCF85063, ±2 ppm TCXO (I²C shared bus) |
| IMU | QMI8658 6-axis @ 896 Hz (I²C shared bus) — unused in firmware |
| Storage | TF/microSD slot, SDMMC 1-bit peripheral |
| Buttons | BOOT (GPIO0), power via AXP2101 |

**Key GPIO assignments**

| Signal | GPIO |
|---|---|
| QSPI CLK | 38 |
| QSPI D0–D3 | 4, 5, 6, 7 |
| LCD CS | 12 |
| LCD RESET | 39 |
| LCD TE | 13 |
| I²C SDA (touch + PMU + RTC + IMU) | 15 |
| I²C SCL | 14 |
| SD CLK | 2 |
| SD CMD | 1 |
| SD D0 | 3 |
| BOOT button | 0 (active-low) |

> **Important**: GPIO14 is the I²C SCL line for the entire onboard sensor bus. It cannot be used as an ADC input — it is why ECG uses a firmware-generated waveform rather than the ESP32-S3 onboard ADC.

### Planned daughter PCB (Stage 2)

A 35 × 25 mm rigid-flex daughter board stacks inside the rear case and wires to the watch GPIO header.

| Component | Package | Function |
|---|---|---|
| ADS127L18 | VQFN-56 | 8-ch 24-bit ΔΣ ADC: FCG (ch0/1), ECG (ch2), nasal NTC (ch3), ERB band (ch4) |
| AD8232 | LFCSP-12 | Single-lead ECG AFE — analog output to ADS127L18 ch2 |
| MAX86140 | WLP | Optical PPG + SpO₂; LED assembly on watch underside |
| NTC excitation passives | 0402 | 47 kΩ series resistor divider from AVDD for nasal thermistor |
| Solder jumper | — | Selects wrist dry pads (morning) vs chest Ag/AgCl pads (overnight) |
| 6-pin JST-SH | — | Nasal thermistor cable and ERB chest band cable |

---

## Firmware Overview

### Codebase structure

```
main/
├── main.c              # App entry, all UI screens, Wi-Fi, ECG sampler task, BP screen + bp_sampler_task
├── app_config.h        # All compile-time constants (ECG, recording, BP, display, battery)
├── app_state.h         # Shared data structures (rec_row_t, bp_row_t, bp_analysis_t, enums)
├── ui/
│   └── ui_common.c/h  # Shared LVGL style helpers and colour palette
├── hal/
│   ├── hal_battery.c/h # AXP2101 PMU battery reader (I²C)
│   ├── hal_display.c/h # LVGL mutex wrapper (hal_display_lock)
│   └── hal_storage.c/h # SD card mount with retries
├── services/
│   ├── svc_nvs.c/h     # Per-SSID NVS Wi-Fi credential storage
│   ├── svc_record.c/h  # SD writer task and CSV lifecycle (Record screen)
│   ├── svc_bp_record.c/h # PSRAM queue, BP writer task, CSV lifecycle, bp_analyse_file()
│   ├── svc_files.c/h   # SD enumeration, HTTP POST upload (background task), file deletion
│   └── svc_time.c/h    # SNTP sync and NVS time persistence
└── signal/
    ├── sig_pipeline.c/h # ECG simulation and bandpass filter step
    └── sig_ppg.c/h      # Two-Gaussian PPG simulation, foot detector, PAT
```

### Key modules

**ECG sampler task** (`ecg_sampler_task` in main.c, Core 0, priority 8)
Runs at 100 Hz. Calls `ecg_simulate_raw()` for a synthetic P-QRS-T waveform, passes samples through a first-order bandpass filter (10–15 Hz), applies a Pan-Tompkins-style detector (derivative → square → moving window integration → adaptive threshold), and after a 200 ms deferred wait performs a bidirectional search on the bandpassed ring buffer to locate the true R-peak apex. Generates a beat-synchronised two-Gaussian PPG waveform, applies a 0.5–16 Hz PPG bandpass filter, runs a first-derivative foot detector to compute PAT, estimates heart rate from refined RR intervals, estimates respiration rate from signal intersection counting, and enqueues complete `rec_row_t` rows to the SD writer when recording is active.

**Signal pipeline** (`sig_pipeline.c`, `sig_ppg.c`)
- `ecg_simulate_raw()`: deterministic P-QRS-T model at ~75 BPM with slow respiratory baseline wander and LFSR noise. Produces 12-bit samples compatible with the autoscale and QRS detector.
- `signal_bandpass_step()`: single-sample first-order HP + LP in series; externally-held state makes it re-entrant. Reused for both ECG (10–15 Hz) and PPG (0.5–16 Hz) filtering.
- `ppg_sim_on_beat()` / `ppg_sim_get_sample()`: two-Gaussian PPG model (Charlton et al. 2020) triggered by each R-peak; beat width adapts to RR interval; PAT jitter applied via LFSR.
- `ppg_det_update_sample()` / `ppg_det_get_pat_*()`: first-derivative zero-crossing foot detector; outputs instantaneous PAT (for CSV) and EMA-smoothed PAT (for topbar display).

**Recording service** (`svc_record.c`)
On `svc_rec_start()`, opens a timestamped CSV on the SD card, creates a FreeRTOS queue, and spawns `sd_writer_task` (Core 1, priority 6). The sampler task pushes `rec_row_t` items; the writer task drains them with a 0.5 ms inter-row delay and 4 KB stdio buffer. CSV columns: `time_ms, ecg, ppg, resp, nas, fcg1, fcg2, drift_ms, batt_v, spo2, resp_rate, hr_bpm, rr_ms, pat_ms, r_peak_ms`. The last three columns are non-zero only on beat-event rows (~200 ms after the R-peak due to the deferred search); `r_peak_ms` is the recording-relative timestamp of the detected R-peak apex.

**B.P. recording service** (`svc_bp_record.c`)
On `svc_bp_rec_start()`, opens a timestamped `*_bp.csv` on the SD card, allocates a 2048-item queue from OPI PSRAM, and spawns `bp_writer_task` (Core 1, priority 6). A separate `bp_sampler_task` (Core 1, priority 6) reads the ECG/PPG ring buffers under `s_ecg_spinlock` at 1 kHz and enqueues `bp_row_t` items. CSV columns: `time_ms, ecg, ppg, drift_ms, r_peak_ms, rr_us, pat_us`. RR and PAT values are in **microseconds** and are non-zero only on beat-event rows. After recording, `bp_analyse_file()` computes HRV RMSSD, PAT mean, PAT variance, and populates the last-64-beat `rr_series`/`pat_series` arrays for the on-screen chart. Both writer and sampler self-delete on stop.

**Files service** (`svc_files.c`)
`svc_files_refresh_list()` mounts the SD card if needed, enumerates files via `opendir`/`readdir`/`stat`, classifies each file (`_bp.csv` → BP, `.csv` → Record), and sorts the list newest-first (filenames are timestamp-prefixed). `svc_files_start_send_task()` spawns a background FreeRTOS task that opens the selected file, builds an HTTP POST request to the PC receiver at `192.168.137.1:8000/upload`, streams the file in 1 KB chunks using `esp_http_client_write`, and reports progress through a shared `file_tx_status_t` structure polled by the UI timer every 250 ms. The request carries `X-Filename`, `X-File-Size`, and `X-Session-Type` headers for the PC receiver to classify and store the file. `svc_files_delete_file()` removes a file via `remove()`. The PC receiver script specification is in `docs/pc_receiver_stage_plan.md`.

**Battery** (`hal_battery.c`)
Reads voltage (14-bit mV register 0x34/0x35) and fuel-gauge percentage (register 0xA4) from the AXP2101 over I²C. Falls back to a linear voltage estimate if the fuel gauge reports 0 (before cell learning).

**Time service** (`svc_time.c`)
`svc_time_sync()` — stops any previous SNTP instance, starts polling mode, waits up to 15 s for a timestamp > 1 Jan 2024, persists the synced epoch to NVS. `svc_time_restore_from_nvs()` — called at boot before Wi-Fi; restores the last known time from NVS so the clock shows a plausible stale time rather than epoch 0.

### Data flow (recording)

```
ecg_simulate_raw()             ← replace with real ADC read for Stage 2
        │
        ▼
signal_bandpass_step()         10–15 Hz bandpass → s_ecg_band[] ring buffer
        │
        ├──→ QRS detector (MWI) → arm deferred R-peak search
        │        └──→ [200 ms later] bidirectional search on s_ecg_band[]
        │                  └──→ refined r_peak_ms, rr_ms → HR estimate, CSV
        ├──→ Respiration history → resp_rate estimate → rec_row_t.resp_rate
        ├──→ s_ecg_raw[] ring buffer → chart display
        │
ppg_sim_get_sample()           two-Gaussian beat-synchronised waveform
        │
signal_bandpass_step()         0.5–16 Hz PPG bandpass
        │
        ├──→ ppg_det_update_sample() → PAT foot detection → pat_ms, topbar
        └──→ s_ppg_raw[] ring buffer → PPG chart display
        │
        ▼
rec_row_t (ECG + PPG + RESP/NAS/FCG sines + drift + batt + rr_ms + pat_ms + r_peak_ms)
        │
        ▼
svc_rec_enqueue() → FreeRTOS queue → sd_writer_task → FAT CSV on SD
```

---

## Build and Flash Instructions

### Requirements

- **ESP-IDF 6.0.x** — install from [https://docs.espressif.com/projects/esp-idf](https://docs.espressif.com/projects/esp-idf) and source the environment.
- On first build, the IDF component manager downloads managed components automatically (requires internet access).

### Commands

```sh
# Build
idf.py build

# Flash
idf.py -p /dev/ttyACM0 flash

# Serial monitor (USB-Serial-JTAG hardware peripheral)
idf.py -p /dev/ttyACM0 monitor

# Build + flash + monitor combined
idf.py -p /dev/ttyACM0 flash monitor

# Clean rebuild
idf.py fullclean && idf.py build
```

### Entering download mode

Hold **BOOT**, tap **RESET**, release **BOOT**.

### Timezone configuration

Edit the `POSIX_TZ` constant in [main/app_config.h](main/app_config.h):

```c
#define POSIX_TZ  "AEST-10AEDT,M10.1.0,M4.1.0/3"   // Sydney, Australia (default)
```

Common alternatives:

| Location | POSIX string |
|---|---|
| London | `GMT0BST,M3.5.0/1,M10.5.0` |
| New York | `EST5EDT,M3.2.0,M11.1.0` |
| Tokyo | `JST-9` |
| Berlin | `CET-1CEST,M3.5.0,M10.5.0/3` |

### Console note

The project uses the **USB-Serial-JTAG hardware peripheral** (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`) rather than TinyUSB CDC. This avoids a known firmware freeze caused by the software CDC TX FIFO stalling during Wi-Fi association bursts. Both flashing and monitoring work over the same USB-C port.

---

## Repository Structure

```
tulog-biowatch/
├── main/                       # Application source
│   ├── main.c                  # App entry, UI, Wi-Fi, ECG sampler
│   ├── app_config.h            # All compile-time constants
│   ├── app_state.h             # Shared data structures
│   ├── ui/                     # LVGL shared style helpers
│   ├── hal/                    # Hardware abstraction (battery, display, storage)
│   ├── services/               # Runtime services (NVS, recording, time)
│   ├── signal/                 # Signal simulation and filtering
│   ├── CMakeLists.txt          # Component build config
│   └── idf_component.yml       # Managed component dependencies
├── components/
│   ├── waveshare__esp32_s3_touch_amoled_2_06/   # Waveshare BSP v1.0.6
│   └── espressif__esp_codec_dev/                # Audio codec (dependency of BSP)
├── docs/
│   ├── refactor-plan.md        # Phase-by-phase modularisation log
│   ├── files_screen_transfer_plan.md  # Files screen and Wi-Fi transfer design
│   └── pc_receiver_stage_plan.md      # Python PC receiver design (Stage 1 + Stage 2)
├── CMakeLists.txt              # Top-level project cmake
├── partitions.csv              # Custom partition table (32 MB flash)
├── sdkconfig.defaults          # KConfig defaults (PSRAM, LVGL, USB-JTAG, FAT)
├── dependencies.lock           # Resolved component versions
├── CLAUDE.md                   # Claude Code project instructions
├── SIGNAL_PROCESSING.md        # Detailed documentation of all ECG/PPG/PAT algorithms
├── plot_signals.py             # Python script to plot ECG/PPG with R-peak markers
├── Project Wrist Watch.docx    # Hardware design, physiology rationale, sensor specs
├── watch_design_plan_two_stage.docx  # Two-stage firmware implementation plan
├── AXP2101.pdf                 # AXP2101 PMU datasheet
└── schematic.pdf               # Board schematic
```

---

## Roadmap / Next Steps

### Immediate engineering priorities

1. **PC companion receiver** — implement the Python Stage 1 receiver described in `docs/pc_receiver_stage_plan.md`. Run on a PC acting as a Wi-Fi hotspot; accept `POST /upload` on port 8000; save files to `inbox/record/` or `inbox/bp/`; log uploads. The watch Files screen is already wired to send to `192.168.137.1:8000` (Windows Mobile Hotspot default gateway — change the constant in `svc_files.c` if different).

2. **Verify AXP2101 battery integration** — confirm `bsp_i2c_get_handle()` returns a valid handle in the boot sequence and that `battery_read_percent()` returns plausible values on hardware.

3. **Connect MAX86140 (Stage 1 completion)** — wire MAX86140 to the watch header (SPI on IO14/IO15/TXD/D−IO19); write a minimal driver that reads PPG FIFO over SPI and feeds `rec_row_t.ppg` and `rec_row_t.spo2`. Replace `ppg_sim_get_sample()` call in `ecg_sampler_task`; the bandpass filter, foot detector, PAT pipeline, and chart autoscale require no changes.

4. **AD8232 Stage 1 ECG** — wire AD8232 output to GPIO44 (RXD header pin), switch `ECG_USE_SIMULATED_SOURCE` to 0, read via ADC1 on GPIO44. Validate R-peak detection and PAT on real ECG + PPG.

### Stage 2 milestones

5. **ADS127L18 daughter PCB** — fabricate 35 × 25 mm daughter board; integrate DRDY ISR + GDMA SPI2 read path; validate simultaneous 5-channel sampling.

6. **Migrate ECG digitisation** — move AD8232 output from GPIO44 to ADS127L18 ch2; validate phase-coherent ECG + FCG sampling.

7. **FCG validation** — connect PVDF or PZT wrist sensor to ADS127L18 ch0/ch1; validate LF-FCG (0.5–30 Hz) and HF-FCG for PEP extraction.

8. **Overnight mode** — implement mode manager: power-down ADS127L18 ch0/ch1, reduce ECG/nasal/ERB to 50–100 SPS, enable MCU light-sleep between DRDY events.

9. **PCF85063 RTC sync** — periodic `esp_timer_get_time()` correction from RTC SQW pulse; target < 1 ms timestamp drift over 8 hours.

10. **MATLAB pipeline** — offline apnoea scoring (nasal cessation + ERB effort + SpO₂ dip → AHI) and morning BPV analysis (PTT − PEP → beat-to-beat BP variance).

### Firmware refactor backlog

- **Phase 6D** — Wi-Fi service extraction (`svc_wifi`). Currently deferred — Wi-Fi tasks are entangled with LVGL callbacks in main.c. Proposed interface: `svc_wifi_event_cb_t` callback pattern (see docs/refactor-plan.md).
- **Phase 6F** — ECG sampler task extraction (`sig_ecg`). Highest-risk extraction; all ECG/respiration state and the ring buffer must move while preserving timing behaviour.

---

---

## Screen Orientation — ILI9341 on the TZT ESP32-2432S024C

This section documents the confirmed-working display orientation configuration for
the TZT board, which required significant investigation to resolve. Record it here
so it is never lost.

### The answer

The ILI9341 panel on this board is **physically mounted in landscape orientation**
(the long physical axis is the column axis = 320 columns wide × 240 rows tall).
Despite the ILI9341 datasheet quoting 240×320, the TZT board glass is landscape-native.

**Working configuration** (in [main/app_config.h](main/app_config.h) and
[main/hal/hal_display.c](main/hal/hal_display.c)):

| Setting | Value | Reason |
|---|---|---|
| `LCD_H_RES` | `320` | Logical width — matches the 320-column physical panel |
| `LCD_V_RES` | `240` | Logical height — matches the 240-row physical panel |
| `LCD_MADCTL` | `0x40` | MX bit (mirror column scan); no MV (no axis swap); BGR bit clear |
| `rgb_ele_order` | `LCD_RGB_ELEMENT_ORDER_RGB` | Panel is RGB order |
| `lv_draw_sw_rgb565_swap()` | in flush callback | Corrects SPI big-endian byte order |
| LVGL software rotation | none | Not needed; hardware MADCTL is sufficient |

### Critical sequencing rule

The MADCTL register **must be written before the GRAM clear**. If the GRAM is
cleared while the panel is still in a stale orientation from a previous flash,
the clear addresses the wrong columns/rows and leaves a grey strip. The correct
order in `hal_display_init()` is:

1. Panel reset + `esp_lcd_panel_init()`
2. **Write MADCTL** via `esp_lcd_panel_io_tx_param(io, 0x36, {LCD_MADCTL}, 1)`
3. **Then clear GRAM** (240 × 320 loop in native portrait coords)
4. Turn display on
5. LVGL init at 320×240

### Why not `esp_lcd_panel_swap_xy()` / `mirror()`?

The `esp_lcd_ili9341` component's `swap_xy` and `mirror` functions only OR/clear
**individual MADCTL bits** onto whatever the init sequence left. If the init
leaves any scan-order bits set, the combination produces shear (vertical stripe
corruption). Writing the **full MADCTL byte** in one shot via `io_tx_param`
overwrites all bits cleanly — this is what TFT_eSPI's `setRotation()` does and
is why that library works reliably on this hardware.

### Tuning orientation

To change orientation, edit only `LCD_MADCTL` in `app_config.h` and reflash.
BGR bit (`0x08`) must stay **clear** — RGB element order is confirmed correct.

| `LCD_MADCTL` | Effect |
|---|---|
| `0x40` | **Working — landscape, correct orientation** |
| `0x00` | Native (landscape columns addressed only 0..239, grey strip) |
| `0xC0` | 180° rotation |
| `0x80` | Mirror rows only |

### What did not work (and why)

- **`swap_xy=true` via `esp_lcd_panel_swap_xy()`** — produces vertical stripe shear because
  the component sets the MV bit in MADCTL but `draw_bitmap` still sends CASET/RASET
  in the original axis order. The pixel stream and GRAM scan direction disagree.
- **LVGL software rotation** (`lv_display_set_rotation`) in PARTIAL render mode — LVGL 9.5
  does not automatically transpose pixels for partial buffers without
  `LV_DRAW_TRANSFORM_USE_MATRIX` + a full-screen framebuffer (150 KB, impossible without PSRAM).
- **`LCD_RGB_ELEMENT_ORDER_BGR`** — renders red as blue. RGB is correct for this panel.

---

## Licence

MIT — free to use, modify, and distribute.
