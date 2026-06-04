# CLAUDE.md — tulog-biowatch

## 1. Project Overview

ESP-IDF firmware for a **Waveshare ESP32-S3-Touch-AMOLED-2.06** development board repurposed as a wearable biosignal acquisition and display platform. The project targets two clinical use cases:

- **Overnight sleep study** — ECG, PPG/SpO₂, nasal thermistor airflow, and ERB chest band for apnoea detection and nocturnal blood pressure variance (BPV) estimation via Pulse Transit Time (PTT).
- **Morning FCG session** — wrist Forcecardiography (FCG) at 1000 Hz for Pre-Ejection Period (PEP) derivation and post-OSA morning BPV tracking.

The current codebase is **Stage 1 firmware**: the sensing pipeline, logging flow, Wi-Fi, time sync, and UI are validated using simulated or placeholder sensor data. Stage 2 will integrate real external sensors (ADS127L18, MAX86140, AD8232).

---

## 2. Current Project Status

### Working and implemented
- Boots to Wi-Fi scan screen → home clock screen
- Wi-Fi scan/connect with up to 3 auto-retries
- NVS credential persistence per SSID
- SNTP time sync (pool.ntp.org); last synced time restored from NVS on reboot
- Battery monitoring via AXP2101 PMU over I²C (register read at 0x34)
- ECG sampling at 100 Hz — **firmware-simulated P-QRS-T waveform** (see §8)
- Pan-Tompkins-inspired QRS detector: bandpass → derivative → square → moving window integration → adaptive threshold
- Heart rate estimate (adaptive smoothed BPM from RR intervals)
- Respiration rate estimate (delayed-signal intersection counting over 20 s window)
- Record screen: ECG chart (4 s rolling window, 200 display points), topbar metrics
- SD card recording to CSV at 100 Hz; lazy mount with retries; 4 KB stdio buffer
- CSV columns: `time_ms, ecg, ppg, resp, nas, fcg1, fcg2, drift_ms, batt_v, spo2, resp_rate, hr_bpm, rr_ms, pat_ms, r_peak_ms`
- Sample drift tracking (`actual_ms − expected_ms` = cumulative wall-clock vs expected time)
- Display brightness slider and screen-sleep timeout (configurable in Settings)
- BOOT button: short press wakes screen or returns to Home
- **B.P. screen** — fully implemented: 30/60/120 s selectable duration; `bp_sampler_task` reads ECG/PPG ring buffers at 1 kHz; `svc_bp_record` service writes CSV (`time_ms, ecg, ppg, drift_ms, r_peak_ms, rr_us, pat_us`); post-recording HRV RMSSD and PAT mean/variance analysis; beat-by-beat RR/PAT chart (up to 64 beats, µs scale); lazy chart build after analysis completes
- **Files screen** — fully implemented: enumerates SD card files, shows name/size/type (REC/BP) sorted newest-first; file selection with highlight; Refresh, Connect WiFi, Send, Delete buttons; HTTP POST upload to PC receiver (`10.42.0.1:8000/POST /upload`, Linux NM hotspot) via `svc_files` service; background upload task with 250 ms progress polling; delete with immediate list refresh; Wi-Fi Back button returns to Files screen after connecting

### Partially implemented / placeholder
- PPG, RESP, NAS, FCG1, FCG2 channels: recorded as sine-wave simulations; no real sensors
- SpO₂ field in CSV is always 0; MAX86140 not integrated
- About screen is a stub (back button only)

### Not started
- ADS127L18 SPI driver (8-channel 24-bit ΔΣ ADC for FCG/ECG/nasal/ERB)
- MAX86140 SPI driver (optical PPG/SpO₂)
- AD8232 ECG AFE integration
- PCF85063 RTC periodic sync to `esp_timer_get_time()`
- QMI8658 IMU data path (hardware present; no firmware driver used)
- Wi-Fi service extraction (Phase 6D of refactor plan — deferred indefinitely)
- ECG sampler task extraction (Phase 6F — planned, not done)
- MATLAB post-processing pipeline

### Unclear / requires hardware verification
- Whether `bsp_i2c_get_handle()` returns a valid handle before `bsp_display_start()` is called (battery_adc_init() is called before display init in app_main).
- AXP2101 fuel-gauge register 0xA4 reporting 0 before cell learning: fallback linear voltage estimate is implemented.
- SD card 1-bit SDMMC reliability on the specific board revision in use.

---

## 3. Hardware Stack

| Component | Role | Bus / Interface |
|---|---|---|
| ESP32-S3R8 | MCU, dual-core LX7 @ 240 MHz, 8 MB OPI PSRAM, 32 MB Flash | — |
| CO5300 AMOLED 410×502 | Display | QSPI (CLK GPIO38, D0–D3 GPIO4–7, CS GPIO12, RST GPIO39, TE GPIO13) |
| FT3168 | Capacitive touch | I²C shared bus (SDA GPIO15, SCL GPIO14) |
| AXP2101 | PMU: LiPo charging, battery voltage/percent | I²C 0x34 (GPIO14/15 shared) |
| PCF85063 | RTC ±2 ppm TCXO | I²C (GPIO14/15 shared) — not actively used in firmware |
| QMI8658 | 6-axis IMU @ 896 Hz | I²C (GPIO14/15 shared) — not used in firmware |
| TF/microSD | Raw data logging, FAT filesystem | SDMMC 1-bit (CLK GPIO2, CMD GPIO1, D0 GPIO3) |
| BOOT button | User input | GPIO0 (active-low, pull-up) |
| **Planned — daughter PCB** | | |
| ADS127L18 | 8-ch 24-bit ΔΣ ADC: FCG (ch0/1), ECG (ch2), nasal (ch3), ERB (ch4) | SPI2 up to 25 MHz, DRDY ISR |
| AD8232 | Single-lead ECG AFE (analog output to ADS127L18 ch2) | Analog (no SPI) |
| MAX86140 | Optical PPG + SpO₂ (19-bit, FIFO 128 words) | SPI3 up to 10 MHz, INT pin |

**Critical constraint**: GPIO14 is simultaneously BSP_I2C_SCL (touch + AXP2101 + PCF85063 bus) and ADC2_CH3. Reading ADC2 on GPIO14 breaks I²C and races with the Wi-Fi PHY — this is why ECG uses a simulated waveform.

---

## 4. Firmware / Software Architecture

### Directory layout
```
main/
├── main.c              # App entry, all screen creation, Wi-Fi, ECG sampler task, BP screen + bp_sampler_task
├── app_config.h        # All numeric/string constants
├── app_state.h         # rec_row_t, bp_row_t, bp_analysis_t, rec_tab_t, health_tab_t
├── ui/
│   ├── ui_common.h     # Colour palette macros, helper declarations
│   └── ui_common.c     # style_screen, style_button, style_card, make_title, etc.
├── hal/
│   ├── hal_battery.c/h # AXP2101 I²C battery reader
│   ├── hal_display.c/h # hal_display_lock() LVGL mutex wrapper
│   └── hal_storage.c/h # SD card mount via bsp_sdcard_mount() with retries
├── services/
│   ├── svc_nvs.c/h     # NVS Wi-Fi credential helpers
│   ├── svc_record.c/h  # SD writer task, CSV file open/write/close (Record screen)
│   ├── svc_bp_record.c/h # BP recording: PSRAM queue, writer task, CSV, bp_analyse_file()
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
| `bp_sampler_task` | 1 | 6 | 4096 | 1 kHz BP sampler; reads ECG/PPG ring buffers under spinlock; spawned on BP start, self-deletes on stop |
| `bp_writer_task` | 1 | 6 | 4096 | BP CSV writer; drains PSRAM queue; spawned by svc_bp_rec_start, self-deletes on stop |
| `bp_analysis_task` | any | 3 | 8192 | Post-recording file analysis (RMSSD, PAT stats); one-shot, self-deletes |
| LVGL port task | 1 | — | BSP-managed | LVGL rendering and input |

### Key concurrency notes
- `s_ecg_spinlock` (portMUX) protects: `s_ecg_raw[]`, `s_ppg_raw[]`, `s_ecg_write_index`, `s_ecg_total_samples`, `s_ecg_min/max`, `s_ecg_hr_bpm`, `s_ecg_sample_drift_ms`, `s_resp_rate_bpm`, `s_ecg_last_rpeak_expected`.
- `bp_sampler_task` reads `s_ecg_raw[]`, `s_ppg_raw[]`, `s_ecg_write_index`, and `s_ecg_last_rpeak_expected` under the same `s_ecg_spinlock`.
- All LVGL calls must be wrapped in `hal_display_lock()` / `bsp_display_lock()` when called from outside the LVGL port task.
- `svc_rec_enqueue()` is called from `ecg_sampler_task` under `portENTER_CRITICAL`.
- `svc_bp_rec_enqueue()` is called from `bp_sampler_task` without a lock (the PSRAM queue handles concurrency internally).
- `s_bp_was_recording` (volatile bool) is written by the LVGL task (timer callback and button callback) and read by the same LVGL task only — no cross-task race.
- `battery_read_*()` called from `clock_update_task` and `ecg_sampler_task` — AXP2101 I²C is not protected by a mutex (potential issue under concurrent calls; requires hardware verification).

### Signal processing pipeline (ECG)
1. `ecg_simulate_raw()` → 12-bit synthetic P-QRS-T sample
2. `signal_bandpass_step()` → first-order HP + LP in series (5–15 Hz)
3. Differentiate → square → moving window integration (15 samples @ 100 Hz)
4. Adaptive threshold from signal/noise level tracking
5. Refractory period (250 ms) guards against double detection
6. BPM computed from RR interval; 3:1 weighted average smoothing

### LVGL chart workaround
LVGL 9.5 hangs if a chart with ≥ 400 points exists during the screen-load animation. The chart is built lazily 800 ms after the user navigates to the Record screen (`rec_build_chart()` gated in `rec_update_plot()`). `REC_CHART_POINTS = 200`; the 400-sample ECG window is stride-subsampled on display.

---

## 5. Design Background

### From *Project Wrist Watch.docx*
The platform targets obstructive sleep apnoea (OSA) assessment and blood pressure variance (BPV) tracking. OSA causes sympathetic surges at apnoea termination, producing nocturnal BP spikes of 40–80 mmHg SBP. BPV — particularly augmented beat-to-beat fluctuations driven by OSA — is an independent cardiovascular risk predictor.

Key signals required:
- **ECG**: R-peak timestamps for PTT and HRV.
- **PPG/SpO₂**: Pulse foot timestamps for PTT; SpO₂ for AHI scoring (MAX86140).
- **Nasal thermistor (NTC)**: Airflow cessation detection.
- **ERB chest band**: Respiratory effort classification (obstructive vs central apnoea).
- **FCG (wrist piezo)**: Pre-Ejection Period (PEP) for contractility-corrected PTT → BP variance.

Final sensor architecture (Stage 2): single ADS127L18 (24-bit, 8-ch, simultaneous sampling) for ECG/FCG/nasal/ERB on SPI2; MAX86140 for PPG/SpO₂ on SPI3; AD8232 as ECG AFE.

### From *watch_design_plan_two_stage.docx*
Formalises the **two-stage implementation path**:
- **Stage 1** (current): Prove firmware, logging, timestamps, and signal pipeline using the onboard ADC for ECG and MAX86140 for PPG/SpO₂. The codebase currently operates in an even earlier state than described (no real hardware sensors are connected; ECG is fully simulated).
- **Stage 2**: Migrate ECG digitisation from the ESP32-S3 SAR ADC to ADS127L18 ch2; add FCG (ch0/ch1), nasal (ch3), ERB (ch4); keep MAX86140 on its own SPI bus.

Header pins available for Stage 1 external sensor wiring: VBUS, GND, D+/IO20, D-/IO19, IO15, IO14, RXD/GPIO44, TXD/GPIO43, 3VS.

### Current firmware alignment with the plan
The firmware implements the Stage 1 software infrastructure (logging, Wi-Fi, time, recording, display) but without any real sensors connected. The signal pipeline structure (bandpass, QRS detection, respiration rate, per-channel recording) is ready for real data — replacing `ecg_simulate_raw()` with a real ADC read is all that is needed for ECG when hardware is available. PPG, nasal, ERB, and FCG channels require new driver modules.

---

## 6. Build & Development Workflow

**Prerequisites**: ESP-IDF 6.0.x installed and sourced (`$IDF_PATH` set).

```sh
# First build (downloads managed components)
idf.py build

# Flash
idf.py -p /dev/ttyACM0 flash

# Monitor (USB-Serial-JTAG, hardware peripheral — does not stall under Wi-Fi load)
idf.py -p /dev/ttyACM0 monitor

# Clean build
idf.py fullclean && idf.py build

# Build + flash + monitor in one step
idf.py -p /dev/ttyACM0 flash monitor
```

**Download mode**: hold BOOT, tap RESET, release BOOT.

**Timezone**: set `POSIX_TZ` in [main/app_config.h](main/app_config.h). Current value: `AEST-10AEDT,M10.1.0,M4.1.0/3` (Sydney, Australia).

**ECG source toggle**: `ECG_USE_SIMULATED_SOURCE` in [main/app_config.h](main/app_config.h). `1` = simulated (safe, default). `0` = ADC2/GPIO14 path (do not enable — breaks I²C and fights Wi-Fi PHY; see the large comment block in [main/main.c](main/main.c):280).

---

## 7. Key Files

| File | Purpose |
|---|---|
| [main/main.c](main/main.c) | App entry, all screen UI creation, Wi-Fi event handling, ECG sampler task, recording callbacks, BP screen + `bp_sampler_task` |
| [main/app_config.h](main/app_config.h) | All compile-time constants: display size, ECG params, recording params, BP params (BP_SAMPLE_HZ=1000, BP_QUEUE_LEN=2048, etc.), NVS keys, battery thresholds |
| [main/app_state.h](main/app_state.h) | Shared structs: `rec_row_t`, `bp_row_t` (rr_us/pat_us in µs), `bp_analysis_t` (hrv_rmssd_us, pat_mean_us, pat_variance_us2, int32_t series[64]), `rec_tab_t`, `health_tab_t` |
| [main/hal/hal_battery.c](main/hal/hal_battery.c) | AXP2101 I²C driver: voltage (14-bit mV register), percent (fuel gauge with voltage fallback) |
| [main/hal/hal_storage.c](main/hal/hal_storage.c) | SD mount via `bsp_sdcard_mount()` with 5-retry loop |
| [main/hal/hal_display.c](main/hal/hal_display.c) | `hal_display_lock()`: polling LVGL mutex with slice-and-retry |
| [main/services/svc_record.c](main/services/svc_record.c) | FreeRTOS queue → SD writer task; Record screen CSV file lifecycle |
| [main/services/svc_bp_record.c](main/services/svc_bp_record.c) | PSRAM queue (2048 × `bp_row_t`) → `bp_writer_task`; BP CSV lifecycle; `bp_analyse_file()` (RMSSD, PAT mean/variance, last-64-beat series) |
| [main/services/svc_files.c](main/services/svc_files.c) | SD file enumeration (opendir/readdir/stat, qsort newest-first); HTTP POST upload to PC receiver (`esp_http_client`, chunked, background task); file deletion via `remove()` |
| [main/services/svc_time.c](main/services/svc_time.c) | SNTP sync with timeout and NVS persistence of last-synced epoch |
| [main/services/svc_nvs.c](main/services/svc_nvs.c) | Per-SSID NVS credential storage |
| [main/signal/sig_pipeline.c](main/signal/sig_pipeline.c) | `ecg_simulate_raw()` (P-QRS-T at ~75 BPM), `signal_bandpass_step()` (first-order HP+LP) |
| [main/signal/sig_ppg.c](main/signal/sig_ppg.c) | Two-Gaussian PPG simulation, PPG bandpass filter, `ppg_det_*` foot detector, PAT (instantaneous + EMA-smoothed) |
| [main/ui/ui_common.c](main/ui/ui_common.c) | Shared LVGL style helpers and colour palette |
| [docs/refactor-plan.md](docs/refactor-plan.md) | Phase-by-phase modularisation history and planned future phases |
| [sdkconfig.defaults](sdkconfig.defaults) | KConfig overrides: PSRAM, partition table, console (USB-Serial-JTAG), LVGL pool (128 KB), LVGL widgets, FAT |
| [partitions.csv](partitions.csv) | NVS 24K + PHY 4K + factory 3M + FAT storage 28M |
| [Project Wrist Watch.docx](Project%20Wrist%20Watch.docx) | Consolidated hardware design, physiological rationale, sensor specs, power budget |
| [watch_design_plan_two_stage.docx](watch_design_plan_two_stage.docx) | Two-stage implementation plan; Stage 1 header pin mapping |

---

## 8. Constraints & Engineering Notes

### Sampling rates
- ECG (simulated): 100 Hz (`ECG_SAMPLE_HZ`), 4 s window (400 samples), displayed at 50 ms refresh
- PPG/RESP/NAS/FCG (simulated): sine waves generated at chart refresh time
- Target (Stage 2): ECG/FCG at 1000 Hz, nasal/ERB at 50 Hz, PPG at 100–250 Hz

### Timing / real-time requirements
- `ecg_sampler_task` runs on Core 0, priority 8. Uses `esp_timer_get_time()` for precise period tracking; busy-waits < 1 ms with `esp_rom_delay_us()`.
- `bp_sampler_task` runs on Core 1, priority 6. Uses `esp_timer_get_time()` with `vTaskDelay(1)` for any remaining wait each 1 ms tick — **must not use `esp_rom_delay_us` here**, as that busy-waits the full sub-ms sleep and starves the IDLE1 task, causing a task watchdog reset within seconds.
- Sample drift = `actual_ms − expected_ms` grows as a cumulative offset from the ideal timeline. This is correct and expected — it reflects wall-clock jitter relative to an ideal grid.
- The SD writer task uses `esp_rom_delay_us(500)` between rows to throttle SD bus. The BP writer uses `esp_rom_delay_us(200)`.
- Both `svc_record.c` and `svc_bp_record.c` call `fsync(fileno(file))` after each periodic `fflush()` to commit the FATFS directory entry to SD card. Without `fsync`, a watchdog reset before `fclose()` leaves a 0-byte file on disk.

### Memory constraints
- 400-sample ECG ring buffer + 2000-sample respiration history live in static RAM.
- `s_rec_chart_points[ECG_WINDOW_SAMPLES]` (400 × 2 bytes) is a `static lv_coord_t` array to keep it off the LVGL stack.
- Raw window copy for chart rendering is `static uint16_t raw_copy[400]` inside `rec_update_plot()` for the same reason.
- LVGL memory pool is **128 KB** (`CONFIG_LV_MEM_SIZE_KILOBYTES=128` in sdkconfig.defaults). Increased from 64 KB after the BP screen's additional objects (chart, series, result card, three duration buttons, timer) caused `LV_ASSERT_MALLOC` failures at `lv_draw_label.c` during post-SNTP screen redraws.
- BP PSRAM queue: `BP_QUEUE_LEN * sizeof(bp_row_t) = 2048 × ~40 bytes ≈ 80 KB`, allocated from OPI PSRAM via `heap_caps_malloc(MALLOC_CAP_SPIRAM)`.
- Current firmware flash footprint: ~1.49 MB of the 3 MB factory partition (50% free). IRAM: ~95 KB used of ~300 KB available. `esp_http_client` added ~110 KB for the Files screen. Ample headroom for Stage 2 SPI sensor drivers (~40–50 KB total).

### GPIO / pin constraints
- GPIO14 (I²C SCL) cannot be used as ADC input — it is the shared I²C bus SCL line.
- GPIO1–10: all taken by SD (1,2,3) or LCD QSPI (4,5,6,7,12,38,39) — no free ADC1 channels.
- GPIO0: BOOT button (active-low, pull-up).
- GPIO14/15: shared I²C bus (FT3168, AXP2101, PCF85063, QMI8658).

### Power
- AXP2101 manages all power rails; battery read via I²C (not ADC).
- Battery thresholds: EMPTY = 3.30 V, FULL = 4.20 V (`BATTERY_VOLTAGE_EMPTY/FULL` in app_config.h).
- Recording stops automatically at ≤ 5% battery (`REC_BATT_STOP_PCT`).
- Target overnight power: ~13–17 mA average (Stage 2 with MCU light-sleep).

### Signal processing assumptions
- ECG simulation runs at ~75 BPM (80 samples/beat at 100 Hz); resets autoscale every 1600 samples.
- QRS detector uses 15-sample MWI window (`ECG_MWI_WINDOW_MS = 150 ms`), 250 ms refractory period, adaptive signal/noise level tracking.
- Respiration rate: intersection counting over 20 s; valid range 4–60 BPM; multiplied by 3 (one zero-crossing per breath in a 20 s window → bpm = crossings × 3).

---

## 9. Known Issues / TODOs

| Issue | Location | Notes |
|---|---|---|
| `spo2` field always 0 | `ecg_sampler_task` in main.c | MAX86140 not integrated |
| PPG/NAS/FCG recorded as sine waves | `ecg_sampler_task` in main.c | Placeholder until real sensors added |
| Drain path in `sd_writer_task` missing `hr_bpm` | svc_record.c | `snprintf` in drain loop has 11 format args, CSV header has 12 columns |
| `battery_read_*()` called from two tasks without mutex | hal_battery.c | I²C concurrent access; not observed to fail but not protected |
| `ui_create_settings_screen()` called twice in `app_main` | main.c | Duplicate call; second call overwrites `s_scr_settings`. Likely harmless but wasteful. |
| PCF85063 RTC not polled | — | `esp_timer_get_time()` drifts; no periodic RTC sync implemented |
| QMI8658 IMU not used | — | Hardware present; no firmware integration |
| Wi-Fi service (Phase 6D) not extracted | main.c | Wi-Fi state and UI callbacks remain entangled in main.c |
| ECG sampler task (Phase 6F) not extracted | main.c | All ECG/resp state remains in main.c |
| BP screen shows "Analysing" immediately (no recording) | `svc_bp_record.c` → `bp_open_file()`, `bp_ui_timer_cb` in main.c | Intermittent. Likely transient SD mount failure: `fopen()` fails → `svc_bp_rec_start()` returns `ESP_FAIL` → `svc_bp_rec_is_recording()` false on next 1 s timer tick → recording-to-idle transition fires with 0 recorded samples. No fix yet. |

---

## 10. Working Conventions for Claude

- **BP screen uses µs for RR and PAT.** `bp_row_t.rr_us` and `bp_row_t.pat_us` are `int32_t` in microseconds. `bp_analysis_t` fields are `hrv_rmssd_us`, `pat_mean_us`, `pat_variance_us2`, and `rr_series`/`pat_series` are `int32_t[64]` in µs. The Record screen (`rec_row_t`) keeps `int16_t rr_ms`/`pat_ms` in **milliseconds** — do not conflate the two.
- **Read code before making architectural claims.** Do not assume a sensor is integrated — verify by looking at the actual driver files.
- **Do not invent hardware features.** The ADS127L18, MAX86140, and AD8232 are not integrated into firmware. The QMI8658 and PCF85063 are present on the board but unused in the codebase.
- **ECG_USE_SIMULATED_SOURCE = 1 is intentional and must not be changed** without understanding the GPIO14/ADC2/I²C conflict documented in main.c:280–325 and app_config.h:126.
- **Preserve FreeRTOS task pinning, priorities, and stack sizes** unless a change is required and explicitly justified.
- **LVGL must always be called under `hal_display_lock()` or `bsp_display_lock()`** from tasks other than the LVGL port task.
- **The spinlock (`s_ecg_spinlock`) is a portMUX**, not a FreeRTOS mutex — use `portENTER_CRITICAL` / `portEXIT_CRITICAL`, never from ISR context.
- **Keep diffs small.** This is a refactor-first project. Prefer extracting one module at a time; avoid sweeping changes.
- **Update CLAUDE.md** when understanding of hardware or architecture changes materially.
- **Check docs/refactor-plan.md** before proposing any structural change — planned phases are already documented there.
- **Use Australian English** spelling in all documentation (organise, behaviour, colour, practise, recognise, modularise).
