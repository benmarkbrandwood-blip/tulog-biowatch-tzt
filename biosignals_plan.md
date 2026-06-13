# Biosignal Sensor Integration Implementation Plan

This plan targets the current `tulog-biowatch-tzt` repository as an ESP-IDF C firmware project with a single large `main.c`, supporting service modules under `main/services`, signal-processing modules under `main/signal`, and hardware abstraction modules under `main/hal`.[cite:53][cite:54][cite:55][cite:56] The existing structure already separates display, recording, and signal-processing concerns enough to preserve most of the downstream pipeline while replacing simulated signal generation with real sensor acquisition.[cite:53][cite:55][cite:56]

The recommended rollout order is: **(1)** shared SPI/I2C bus and acquisition framework, **(2)** ADS1293 ECG, **(3)** MAX30102 PPG/SpO2, **(4)** MPU6050 acceleration, and **(5)** ADS1220 respiration bands last.[cite:52][cite:53] ADS1220 should be reserved for last because it needs the most analog configuration work, including excitation current setup for two respiration-band channels and validation of front-end assumptions before it can be trusted in the production path.[cite:52][cite:1]

## Repository baseline

### Firmware stack and language

The repository is a C firmware project built with CMake and ESP-IDF, with the application entry point in `main/main.c` and component metadata in `main/idf_component.yml`.[cite:53] The firmware already has explicit hardware abstraction files such as `hal_display.c`, `hal_touch.c`, `hal_storage.c`, and service-layer modules such as `svc_record.c` and `svc_bp_record.c`, which makes it practical to add sensor drivers as new HAL/service modules rather than expanding `main.c` further.[cite:54][cite:56]

### Existing abstraction point for biosignals

The closest existing abstraction layer is the signal-processing and recording path centered around `main/signal/sig_pipeline.*`, `main/signal/sig_ppg.*`, `main/services/svc_record.*`, and the state/config definitions in `main/app_state.h` and `main/app_config.h`.[cite:53][cite:55][cite:56] The implementation plan should therefore plug new hardware at the boundary where simulated samples are currently produced and then passed into the same graphing, analysis, and SD-recording flow.[cite:53][cite:55][cite:56]

### Design intent

The architecture should be changed from “UI-driven simulated sample generation” to “sensor-driven sample capture feeding a unified sample frame,” while keeping record-screen rendering and downstream analysis interfaces stable.[cite:53][cite:55][cite:56] The initial operating target is 100 Hz on the Record screen, but all new interfaces should be rate-agnostic so the same source abstraction can later support 1 kHz or 2 kHz acquisition for the BP screen without redesign.[cite:1][cite:2][cite:3][cite:4]

## Key architectural change

### Introduce a unified acquisition layer

Add a new hardware-input layer under `main/hal` and/or `main/services` rather than embedding driver logic in `main.c`. The minimum new files should be:

- `main/hal/hal_bus.h` / `hal_bus.c` — shared SPI and I2C bus init, device registration, chip-select helpers, and mutex protection.
- `main/hal/hal_ads1293.h` / `hal_ads1293.c` — ADS1293 register-level driver and sample readout.[cite:2]
- `main/hal/hal_max30102.h` / `hal_max30102.c` — MAX30102 setup, FIFO readout, LED mode config, interrupt or polling support.[cite:3]
- `main/hal/hal_mpu6050.h` / `hal_mpu6050.c` — MPU6050 accel configuration and burst reads.[cite:4]
- `main/hal/hal_ads1220.h` / `hal_ads1220.c` — ADS1220 config, mux switching, IDAC routing, and conversion reads.[cite:1]
- `main/services/svc_biosignal_acq.h` / `svc_biosignal_acq.c` — scheduler, sampling orchestration, per-sensor enable state, timestamping, and unified output frame dispatch.
- Optionally `main/signal/sig_sources.h` / `sig_sources.c` — thin adapter that converts raw hardware reads into the same normalized channel structure expected by the existing pipeline.

Add one shared frame struct in either `app_state.h` or a new header such as `main/signal/sig_frame.h`:

```c
typedef struct {
    int32_t ecg1_raw;
    int32_t ecg2_raw;
    int32_t ppg_ir_raw;
    int32_t ppg_red_raw;
    int32_t spo2_est;
    int32_t accel_x_raw;
    int32_t accel_y_raw;
    int32_t accel_z_raw;
    int32_t resp_nasal_raw;
    int32_t resp_chest_raw;
    uint32_t sample_index;
    uint64_t timestamp_us;
    uint32_t valid_mask;
} biosignal_frame_t;
```

This frame should become the single handoff object from acquisition into graphing, analysis, and SD logging. That keeps later migration to higher-rate modes mostly a matter of producer timing and buffering, not downstream interface redesign.[cite:53][cite:55][cite:56]

### Remove simulation from the production path

Locate and isolate all simulated biosignal generators currently feeding the record or BP path inside `main.c`, `sig_pipeline.c`, `sig_ppg.c`, or service functions that fabricate ECG/PPG/FCG-like values.[cite:53][cite:55] Refactor them behind a compile-time or runtime source selector during transition, then remove them entirely from the production path once the first sensor stage is validated.[cite:53][cite:55]

Concrete rules for removal:

- Remove any timer callbacks or update loops that synthesize sine waves, noise, or canned biosignal samples for graphs.
- Remove any direct graph writes from simulation code.
- Preserve the graph widget update functions, sample history buffers, derived metric functions, and SD record path unless they are tightly coupled to simulation-specific field names.
- Replace “generate sample” calls with “fetch latest `biosignal_frame_t`” calls from `svc_biosignal_acq`.

## Data-flow target

### New flow

The target flow should become:

1. Shared bus init in startup.
2. Sensor-specific init and self-test.
3. Acquisition scheduler ticks at 100 Hz for the Record screen.
4. Ready sensors populate `biosignal_frame_t`.
5. Existing signal-processing modules consume the same fields they already use, now sourced from hardware instead of simulation.
6. UI graph buffers are updated from the frame.
7. SD recording serializes the frame with new accel and respiration fields.

For higher-rate future modes, keep acquisition decoupled from UI rate. The acquisition service should support a producer rate and a separate render/consumer rate so the BP screen can later sample at 1 kHz or 2 kHz and downsample or batch for display.[cite:1][cite:2][cite:3][cite:4]

## Stage 0: shared bus and framework first

### Goals

Extend the existing SPI2 bus with biosensor device handles and initialise the I2C bus, then establish a reusable acquisition framework so each chip can be added one at a time without changing the UI pipeline repeatedly.[cite:52][cite:53] ADS1293 and ADS1220 are added to SPI2 (shared with display + touch); MAX30102 and MPU6050 use the I2C bus on GPIO21/22. This stage is required before validating any individual sensor.[cite:1][cite:2][cite:3][cite:4]

### Code changes

Add or modify:

- `main/main.c` — move peripheral-bus init out of ad hoc startup sections into dedicated functions; call `hal_bus_init()` and `svc_biosignal_acq_init()` early in boot.[cite:53]
- `main/app_config.h` — define pin mappings, bus host selection, chip-select pins, DRDY/INT pins, I2C addresses, default sample rates, buffer sizes.[cite:53]
- `IO-pin-plan.md` — update with final chosen SPI CS, DRDY, INT, reset, and I2C assignments so firmware and hardware remain consistent.[cite:52]
- `main/hal/hal_bus.*` — create shared SPI/I2C wrapper.
- `main/services/svc_biosignal_acq.*` — create sensor registry, stage enable flags, sample frame, timestamping, and buffer dispatch.
- `main/CMakeLists.txt` — register the new source files.[cite:53]

### Implementation detail

`hal_bus.c` should:

- Add ADS1293 and ADS1220 as additional devices on the **existing SPI2 host**
  already initialised by `hal_display`. Do **not** call `spi_bus_initialize` again —
  call `spi_bus_add_device(SPI2_HOST, ...)` for each sensor. The ESP32 has only two
  user SPI hosts (SPI2/SPI3), both already claimed; sharing SPI2 via the ESP-IDF
  `spi_master` mutex is the correct approach. Each device carries its own clock
  divider (ADS1293 ≤8 MHz, ADS1220 ≤2 MHz, independent of the display's 40 MHz).
  Pin assignment: MOSI=GPIO13, MISO=GPIO12, SCLK=GPIO14 (shared), ADS1293 CS=GPIO16,
  ADS1220 CS=GPIO17. DRDY lines: ADS1220=GPIO35, ADS1293=GPIO34.
- Configure one I2C bus for MAX30102 and MPU6050 on GPIO21 (SDA) / GPIO22 (SCL).
  INT lines: MAX30102 INT=GPIO32, MPU6050 INT=GPIO25.
- Provide thin wrappers such as:

```c
esp_err_t hal_spi_dev_add(spi_host_device_t host, const spi_device_interface_config_t *cfg, spi_device_handle_t *out);
esp_err_t hal_spi_txrx(spi_device_handle_t dev, const uint8_t *tx, uint8_t *rx, size_t len);
esp_err_t hal_i2c_reg_read(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len);
esp_err_t hal_i2c_reg_write(uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len);
```

`svc_biosignal_acq.c` should:

- Own the current `biosignal_frame_t latest_frame`.
- Expose `svc_biosignal_acq_get_latest()` and `svc_biosignal_acq_step_100hz()`.
- Maintain per-sensor readiness flags.
- Support later multiple acquisition rates by separating “sensor poll/read” from “UI consume.”

### What remains unchanged

- Existing display driver code in `hal_display.*` can remain unchanged.[cite:54]
- Existing storage mount code in `hal_storage.*` and file service plumbing in `svc_files.*` can remain largely unchanged.[cite:54][cite:56]
- Existing graph rendering helpers and generic UI utilities can remain unless they hardcode old channel names.[cite:57]

### Validation

- Confirm boot-time bus init succeeds and no existing display/touch/storage functions regress.
- Add a diagnostics page or serial log block showing bus init status, detected I2C devices, and ADS1293/ADS1220 register readback.
- Validate mutex-safe access if UI, logging, and acquisition tasks can overlap.

### Risks / decisions

- GPIO allocation is resolved — see IO-pin-plan.md. SPI2 sharing is chosen;
  all CS, DRDY, and INT pin assignments are finalised.
- Need decision on whether acquisition runs in a dedicated FreeRTOS task, an ESP timer callback, or a hybrid ISR-plus-task model. A mixed design is recommended: GPIO interrupt for DRDY-capable devices, worker task for register reads and frame assembly.

## Stage 1: ADS1293 first sensor

### Why first

ADS1293 should be the first real biosignal source because ECG replaces the most central simulated waveform path and its SPI plus DRDY integration validates the shared bus architecture early.[cite:2][cite:53] The chip supports up to three ECG-capable channels, flexible routing, DRDY output, and register-programmable channel setup, which fits the staged verification requirement well.[cite:2]

### Target behavior

Use both ECG leads from ADS1293. One lead should feed the normal ECG path and the other should feed the Record-window FCG graph in the initial integration, matching the requirement that one ECG lead occupy the existing FCG graph position.[cite:2] Treat the two leads as independent source channels in the pipeline, even if the UI initially overlays or repurposes existing graph slots, because this keeps the architecture compatible with future UI changes.[cite:2][cite:53]

### Code changes

Add:

- `main/hal/hal_ads1293.*`
- optional `main/hal/hal_ads1293_regs.h` for register addresses and bitfields

Modify:

- `main/services/svc_biosignal_acq.*` — add ADS1293 init, start, read, and frame-population logic.
- `main/app_config.h` — add ADS1293 CS, DRDYB, RSTB, clock-source choice, and lead mapping constants.[cite:53]
- `main/app_state.h` — add raw ECG channel state, lead-off/alarm flags, and availability bits.[cite:53]
- `main/signal/sig_pipeline.*` — accept real ECG samples from the frame instead of simulated samples.[cite:55]
- `main/services/svc_record.c` and/or `svc_bp_record.c` — switch logging source from simulation fields to `ecg1_raw` and `ecg2_raw`.[cite:56]
- UI update code in `main.c` or `ui_common.c` — remap graph buffers so ECG lead 2 drives the current FCG graph.[cite:53][cite:57]

### ADS1293 implementation detail

Driver functions should include:

```c
esp_err_t hal_ads1293_init(const hal_ads1293_cfg_t *cfg);
esp_err_t hal_ads1293_reset(void);
esp_err_t hal_ads1293_configure_two_lead_ecg(void);
esp_err_t hal_ads1293_start(void);
bool hal_ads1293_data_ready(void);
esp_err_t hal_ads1293_read_channels(int32_t *ecg1, int32_t *ecg2);
esp_err_t hal_ads1293_read_status(uint8_t *error_status, uint8_t *error_lod);
```

Use DRDYB as the preferred acquisition trigger because the device explicitly provides a data-ready output for interrupt-driven diagnostics and streaming.[cite:2] Configure channel routing based on the datasheet’s example procedures for two- and multi-lead ECG paths, then read channel 1 and channel 2 in loop read-back mode before adding any advanced lead-off logic.[cite:2]

Recommended first-pass configuration decisions:

- Use only two ECG channels initially.
- Skip Wilson/reference complexity unless the board wiring requires it immediately.
- Bring up RLD and common-mode detection only if needed for stable waveforms after a dry bench test, because they add analog dependence that can slow first verification.[cite:2]
- Log `ERRORSTATUS` and `ERRORLOD` continuously for early debugging.[cite:2]

### What can remain unchanged

- Any existing heart-rate or waveform post-processing that only expects a scalar ECG-like input can remain if fed from `ecg1_raw`.
- Existing graph widgets can remain if their data source functions are changed rather than their rendering implementation.

### What should be removed

- Simulated ECG/FCG waveform generation.
- Any hardcoded assumptions that ECG and FCG originate from internal math models.

### Validation

- Confirm ADS1293 register read/write at startup.
- Confirm DRDYB toggles at the configured output data rate.[cite:2]
- Plot both channel streams on the Record screen; route channel 2 to the existing FCG graph.
- Record to SD and inspect files for real nonrepeating ECG values.
- Add a temporary debug screen or UART log for lead-off and error bits.

### Risks / blockers

- Electrode topology is not fully specified in the repository snapshot, so lead mapping and whether RLD/WCT are required may still need board-level confirmation.[cite:2]
- The chosen output data rate should align with 100 Hz UI operation, but the device itself can run much faster; keep acquisition and display decoupled.[cite:2]

## Stage 2: MAX30102 second sensor

### Why second

MAX30102 should be integrated after ECG because it adds optical PPG and SpO2 without disturbing the ECG flow, and it uses I2C rather than SPI, which broadens the validated bus framework.[cite:3] This stage also lets the existing `sig_ppg.*` module be preserved more directly than the other sensors.[cite:55]

### Target behavior

The MAX30102 should supply raw PPG data for the existing PPG graph path and provide the inputs necessary for SpO2 calculation using red and IR readings.[cite:3] In the initial stage, it is acceptable to display raw or lightly filtered PPG first and gate SpO2 display on signal-quality validation.[cite:3][cite:55]

### Code changes

Add:

- `main/hal/hal_max30102.*`

Modify:

- `main/services/svc_biosignal_acq.*` — add I2C init, FIFO service, frame population for `ppg_ir_raw` and `ppg_red_raw`.
- `main/signal/sig_ppg.*` — refactor so raw hardware samples enter where simulated PPG samples previously entered, preserving filtering and pulse feature extraction where possible.[cite:55]
- `main/app_state.h` — add signal quality, finger-detect, and SpO2 estimate fields.[cite:53]
- Record-screen update code in `main.c` — source PPG graph from MAX30102 values instead of simulation.[cite:53]
- `svc_record.c` — add raw IR, raw red, and optionally computed SpO2 columns if not already present.[cite:56]

### MAX30102 implementation detail

Driver functions should include:

```c
esp_err_t hal_max30102_init(const hal_max30102_cfg_t *cfg);
esp_err_t hal_max30102_configure_100hz(void);
esp_err_t hal_max30102_read_fifo(max30102_sample_t *samples, size_t *count);
esp_err_t hal_max30102_get_status(uint8_t *int_status);
```

Implementation choices:

- Configure sample rate initially around the Record screen’s 100 Hz target if supported cleanly by the desired LED pulse-width/resolution combination.[cite:3]
- Prefer FIFO burst reads in the acquisition task; use the interrupt pin if wired, otherwise polling is acceptable at this stage.[cite:3]
- Put SpO2 estimation behind a quality gate so the UI does not present unstable values during motion or poor finger placement.

### What can remain unchanged

- Much of `sig_ppg.c` should remain if it already contains filtering, peak tracking, or derived metrics that accept raw sample inputs.[cite:55]
- Existing PPG graph rendering can remain with only source redirection.

### What should be removed

- Simulated PPG sample generator and any synthetic SpO2 estimator using fake signals.

### Validation

- Confirm device ID / register access over I2C.
- Verify FIFO levels change when the sensor is covered or a finger is placed.
- Display IR-based waveform on the PPG graph.
- Confirm raw red and IR values are written to SD and vary physiologically.
- Only enable displayed SpO2 after stable dual-channel behavior is observed.

### Risks / blockers

- The quality of SpO2 depends heavily on board optics, LED current, ambient cancellation, and finger contact, so raw data validation must happen before algorithm tuning.[cite:3]
- If the current signal pipeline assumes one scalar PPG stream, add a small adapter that derives the display waveform from IR while preserving both raw channels for storage and later SpO2 calculation.

## Stage 3: MPU6050 third sensor

### Why third

MPU6050 is functionally simpler than ADS1220 and does not replace an existing critical bioelectric path, making it a good third step once ECG and PPG are stable.[cite:4] It also satisfies the Record-screen requirement to replace the current FCG2 graph with a 3-axis acceleration display.[cite:4]

### Target behavior

The Record screen should replace the current FCG2 graph with one graph panel containing three traces: `accx`, `accy`, and `accz`.[cite:4] These three channels must also be added to the data written to the SD card during Record mode.[cite:4][cite:56]

### Code changes

Add:

- `main/hal/hal_mpu6050.*`

Modify:

- `main/services/svc_biosignal_acq.*` — add accelerometer read scheduling and frame fill for `accel_x_raw`, `accel_y_raw`, `accel_z_raw`.
- `main/app_state.h` — add accel display buffer state and sensor-ready bits.[cite:53]
- Record-screen UI code in `main.c` — replace FCG2 data source with a 3-trace acceleration graph data source.[cite:53]
- `ui_common.c` if graph helper functions currently assume single-trace graphs only.[cite:57]
- `svc_record.c` — append `accx`, `accy`, `accz` fields to CSV or log rows.[cite:56]

### MPU6050 implementation detail

Driver functions should include:

```c
esp_err_t hal_mpu6050_init(const hal_mpu6050_cfg_t *cfg);
esp_err_t hal_mpu6050_configure_accel_only(uint16_t sample_rate_hz);
esp_err_t hal_mpu6050_read_accel(int16_t *x, int16_t *y, int16_t *z);
```

Recommended decisions:

- Use one combined graph panel with three traces, not three separate graphs. This matches the request, preserves screen real estate, and minimizes UI restructuring.
- Start with accel-only mode; gyro can remain disabled unless future work requires it.[cite:4]
- Read at 100 Hz initially and maintain unit conversion in one place, such as raw LSB in storage and optional g-converted values only for display labels.

### What can remain unchanged

- Existing graph container and record-screen layout can remain if one panel can host three traces.
- File handling and SD rotation logic remain unchanged except for added columns.[cite:56]

### What should be removed

- The existing simulated FCG2 source and any code that assumes the second graph panel is scalar-only.

### Validation

- Confirm register read/write and WHO_AM_I response.
- Shake/tilt test the device and verify correlated X/Y/Z changes on screen.
- Confirm all three axes are written to SD and aligned with the same timestamps as ECG/PPG.
- Validate graph color assignment and scaling so all axes remain readable in one panel.

### Risks / blockers

- If the UI graph widget only supports one trace, a small reusable multi-trace graph extension will be required in `ui_common.c` or the record screen code.[cite:57]
- Motion bursts can exceed current autoscaling assumptions; add fixed or clipped scaling for first bring-up.

## Stage 4: ADS1220 last

### Why last

ADS1220 should be last because the respiration-band input path is the least defined and requires the most front-end decisions around excitation current, channel topology, muxing, settling, and whether the bands are truly resistive sensors compatible with the device’s IDAC-based approach.[cite:1] The ADS1220 also introduces extra analog design risk compared with the other devices, even though its digital interface is straightforward SPI.[cite:1]

### Target behavior

Use ADS1220 to sample two respiration-band signals: one nasal sensor and one chest band.[cite:1] The implementation should support exciting the respiration sensors with the ADS1220 programmable current sources if the sensor topology and compliance voltage permit, but the final plan must explicitly leave room for front-end validation before locking the analog configuration.[cite:1]

### Code changes

Add:

- `main/hal/hal_ads1220.*`
- optional `main/hal/hal_ads1220_regs.h`

Modify:

- `main/services/svc_biosignal_acq.*` — add ADS1220 read scheduling and frame fill for `resp_nasal_raw` and `resp_chest_raw`.
- `main/app_config.h` — add ADS1220 channel map, gain, reference, current-source, and data-rate constants.[cite:53]
- `main/app_state.h` — add respiration readiness/error fields.[cite:53]
- Record-screen UI code in `main.c` — route respiration channels into the existing respiration graph path or other existing graph slots once available.[cite:53]
- `svc_record.c` — append nasal and chest respiration columns.[cite:56]

### ADS1220 implementation detail

**SPI2 bus priority — mandatory for all ADS1220 SPI transfers:**
ADS1220 shares SPI2 with the ILI9341 display DMA.  The display flush (core 1) and any
biosensor blocking transmit (core 0) deadlock if they run simultaneously — this was
confirmed and fixed for ADS1293 (F3 fix, 2026-06-14).  ADS1220 must use the same gate:

```c
SemaphoreHandle_t gate = hal_display_get_spi2_gate();
if (gate) xSemaphoreTake(gate, portMAX_DELAY);   /* biosensor has absolute priority */
esp_err_t err = hal_spi_txrx(s_spi, tx, rx, len);
if (gate) xSemaphoreGive(gate);
```

Apply this pattern in `hal_ads1220_read_raw()` and any other `hal_spi_txrx()` call in
the ADS1220 driver.  The display flush skips a chunk with a non-blocking try-take
(`timeout=0`) if the gate is held — the dirty area redraws in the next LVGL cycle
(~50 ms), which is imperceptible.

Driver functions should include:

```c
esp_err_t hal_ads1220_init(const hal_ads1220_cfg_t *cfg);
esp_err_t hal_ads1220_configure_respiration(void);
esp_err_t hal_ads1220_select_channel(ads1220_channel_t ch);
esp_err_t hal_ads1220_start_conversion(void);
esp_err_t hal_ads1220_wait_drdy(TickType_t timeout);
esp_err_t hal_ads1220_read_raw(int32_t *value);
```

Recommended first-pass design:

- Use continuous or repeated single-shot acquisition with explicit channel alternation between nasal and chest if one ADS1220 is sampling both signals.[cite:1]
- Keep the output interface abstract so later migration to 1 kHz or 2 kHz on the BP screen can swap in a faster acquisition policy or dedicated ADC path without changing downstream consumers.[cite:1]
- Evaluate whether the two respiration bands should be measured as two differential inputs, or as one differential plus one pseudo-differential path, based on the actual analog front end.
- Start with modest gain and conservative data rate; validate noise and settling before tuning.

Important ADS1220-specific considerations from the datasheet:

- The device supports two differential or four single-ended inputs and programmable data rates up to 2 kSPS.[cite:1]
- It provides dual matched IDAC excitation currents from 10 µA to 1.5 mA, which may suit resistive respiration bands if compliance voltage and sensor impedance are acceptable.[cite:1]
- IDACs need startup time and must be routed after the current value is programmed.[cite:1]
- Channel switching requires attention to settling; use one-sample discard or explicit settle logic if needed after MUX changes.

### What can remain unchanged

- Existing respiration analysis and display code can remain if it already consumes a scalar respiratory waveform and is decoupled from the simulator.
- Shared SPI infrastructure from Stage 0 remains unchanged.[cite:1]

### What should be removed

- Simulated respiration and any placeholder dual-band values.

### Validation

- Confirm SPI register readback and DRDY behavior.
- Validate one channel at a time first before alternating both channels.
- Inject known test changes into each band and confirm that nasal and chest traces move independently.
- Verify logged values are stable and timestamps remain aligned with other sensors.
- Confirm excitation-current routing and analog compliance on the actual hardware before relying on measured amplitude.[cite:1]

### Risks / blockers

- The largest unresolved blocker is the exact electrical model of the nasal and chest respiration bands. The firmware plan can assume resistive sensing with ADS1220 IDAC excitation, but hardware validation must confirm that assumption before final driver constants are fixed.[cite:1]
- If both channels cannot be sampled with sufficient rate and settling margin on one device, the architecture should still permit an upgraded acquisition strategy later because the downstream interface is already frame-based.

## Required refactors across the codebase

### `main/main.c`

`main.c` is very large and should not absorb more driver logic.[cite:53] Refactor it so that it only:

- initializes services and HAL layers,
- manages app state transitions,
- triggers per-screen update functions,
- consumes `biosignal_frame_t` snapshots for display.

Remove from `main.c`:

- direct simulated waveform generation,
- direct per-sensor register transactions,
- sensor-specific timing loops.

### `main/app_state.h`

Extend state definitions to include:

- active sensor bitmask,
- per-sensor ready/error flags,
- current acquisition rate,
- latest-frame cache,
- graph source mapping metadata.

### `main/services/svc_record.c`

Refactor record serialization so it takes a `const biosignal_frame_t *frame` rather than many independent scalar globals.[cite:56] This will make adding channels trivial and reduce bugs when later moving to high-rate capture.

Suggested new record columns:

| Column | Source |
|---|---|
| `sample_index` | acquisition service |
| `timestamp_us` | acquisition service |
| `ecg1_raw` | ADS1293 CH1 |
| `ecg2_raw` | ADS1293 CH2 |
| `ppg_ir_raw` | MAX30102 |
| `ppg_red_raw` | MAX30102 |
| `spo2_est` | derived from PPG pipeline |
| `accx_raw` | MPU6050 |
| `accy_raw` | MPU6050 |
| `accz_raw` | MPU6050 |
| `resp_nasal_raw` | ADS1220 |
| `resp_chest_raw` | ADS1220 |

### `main/signal/sig_pipeline.c`

Refactor this module so it consumes externally acquired samples and no longer owns sample synthesis.[cite:55] Keep filtering, normalization, derived metrics, and graph-buffer updates if those are already encapsulated there.[cite:55]

### `main/signal/sig_ppg.c`

Preserve this module if it already implements useful filtering and feature extraction, but change the input entry point to raw MAX30102 reads instead of synthetic values.[cite:55] Separate “raw acquisition” from “derived PPG/SpO2 processing” so the same code can be reused at different future sample rates.[cite:55]

## Scheduling model

A mixed interrupt-driven and polling model is the best fit.

- ADS1293: interrupt-driven on `DRDYB`, worker task performs SPI read.[cite:2]
- MAX30102: interrupt-driven if INT pin exists on the board, otherwise FIFO polling at 100 Hz is acceptable initially.[cite:3]
- MPU6050: polling at 100 Hz is sufficient for first Record-screen integration.[cite:4]
- ADS1220: polling or DRDY wait in the acquisition task is acceptable initially; its timing is deterministic enough for staged integration.[cite:1]

Implement one acquisition task that blocks on an event group or queue fed by interrupts and periodic timers. This avoids doing SPI/I2C transfers in ISRs while still supporting deterministic capture.

## Buffering and rate strategy

To support future 1 kHz or 2 kHz BP-screen work without redesign:

- Use a ring buffer of `biosignal_frame_t` entries between acquisition and consumers.
- Keep sensor drivers independent of UI frame rate.
- Store raw sensor samples with timestamps before any display downsampling.
- Make graph code consume decimated or selected frames, not the acquisition ISR/task directly.
- Parameterize sample rate in `svc_biosignal_acq` and `app_config.h` rather than baking 100 Hz into sensor drivers.

This change matters most for ADS1293 and ADS1220 because both can run well above the initial Record-screen rate.[cite:1][cite:2]

## Answers to the clarifying questions

These are the best repository-supported answers, plus planning assumptions where the repo does not make them explicit.

| Question | Answer |
|---|---|
| What language and firmware stack is the project using? | C with ESP-IDF/CMake structure, centered on `main/main.c` and component-style submodules.[cite:53] |
| Is there an existing abstraction layer for biosignal sources? | Not a dedicated hardware-source layer, but `signal` and `services` modules provide a natural insertion point for one.[cite:55][cite:56] |
| Should the plan assume interrupt-driven acquisition, polling, or a mix? | A mix: interrupt-driven where the chip provides useful ready interrupts, otherwise polling in an acquisition task.[cite:1][cite:2][cite:3][cite:4] |
| Are there constraints on memory, sampling buffers, or display update rates? | The repo snapshot does not clearly state hard limits, so the plan assumes moderate embedded constraints and recommends bounded ring buffers plus decoupled UI updates.[cite:53] |
| For ADS1293, are both ECG leads fully independent in UI/pipeline? | Treat them as independent pipeline channels; map one to the current ECG graph and one to the current FCG graph in the first UI stage.[cite:2] |
| For MPU6050, should the three axes share one graph panel or be separate? | One shared graph panel with three traces, replacing FCG2 as requested.[cite:4] |
| Any preferred naming convention? | The repo consistently uses `hal_*`, `svc_*`, `sig_*`, and snake_case C identifiers, so new modules should follow that convention.[cite:54][cite:55][cite:56] |

## Staged verification checklist

### Stage 0

- Bus init succeeds.
- Sensor devices respond on SPI/I2C.
- No regressions in screen, touch, SD, or boot.

### Stage 1: ADS1293

- Register write/readback passes.
- DRDY interrupt fires.
- ECG1 and ECG2 visible on screen.
- ECG2 successfully occupies FCG graph.
- SD file contains live ECG values.

### Stage 2: MAX30102

- Device responds and FIFO fills.
- PPG waveform visible.
- Raw red and IR logged.
- SpO2 shown only after quality threshold.

### Stage 3: MPU6050

- WHO_AM_I passes.
- Three-axis graph replaces FCG2.
- Accel values written to SD.
- Axis motion matches physical movement.

### Stage 4: ADS1220

- Register readback passes.
- One respiration channel validated first.
- Dual-channel nasal/chest acquisition validated second.
- Excitation current behavior confirmed on hardware.
- Respiration data logged and graphed.

## Instructions for Claude inside the repo

Claude should be told to look for the sensor datasheets in the repository’s `datasheets/` directory and use them while tracing the actual firmware call graph and data path. The repository plan should specifically identify where simulated signals are currently created, where those values enter the Record-screen graph buffers, where they pass through `sig_pipeline` / `sig_ppg`, and where `svc_record` serializes them so each stage can replace that path with real hardware incrementally.[cite:53][cite:55][cite:56]

## Recommended implementation order summary

1. Add shared SPI/I2C bus layer and acquisition service.
2. Integrate ADS1293 and replace simulated ECG/FCG inputs.
3. Integrate MAX30102 and replace simulated PPG/SpO2 inputs.
4. Integrate MPU6050 and replace FCG2 with a 3-trace accel panel; add accel logging.
5. Integrate ADS1220 last for nasal and chest respiration bands with validated excitation-current configuration.

This order satisfies the requirement to verify one chip at a time, sets up additional SPI communication first, preserves as much of the existing processing/display pipeline as possible, and keeps the architecture extensible to higher-rate future work.[cite:1][cite:2][cite:3][cite:4][cite:53]
