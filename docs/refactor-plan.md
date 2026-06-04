# Refactor Plan

## Goal
Split the monolithic ESP-IDF firmware into manageable modules while preserving runtime behavior, hardware behavior, and buildability.

## Overall strategy
Refactor in small phases. Prefer extracting code behind stable boundaries before changing internals. Build and test after each phase. Use the original working firmware behavior as the reference.

## Target module direction
Potential target structure:
- `main/` — startup, orchestration, app init, task creation
- `ui/` — screen creation, navigation, shared widgets, event callbacks
- `services/` — Wi-Fi, time sync, recording, storage-adjacent runtime logic
- `signal/` — biosignal generation, filtering, detection, derived metrics
- `hal/` — display, touch, SD/storage mount helpers, battery, board-specific interfaces
- shared headers — config, state structs, events, interfaces

## Refactor principles
- Preserve behavior first.
- Extract one responsibility at a time.
- Avoid large renames early.
- Keep interfaces thin.
- Use wrappers/adapters if needed to avoid breaking behavior.
- Treat timing-sensitive and hardware-sensitive logic as high risk.
- Keep diffs small and reviewable.

## Proposed phases

### Phase 0 — Analysis and baseline
Objective:
- Inventory the monolithic firmware structure and dependencies.
- Identify logical module boundaries and high-risk areas.
- Confirm build/runtime baseline before deeper changes.

Validation:
- `idf.py build`
- Basic boot test
- Confirm logs appear
- Confirm major screens/features still reachable

### Phase 1 — Shared config/state/event extraction ✓ DONE
Objective:
- Extract constants, enums, shared structs, and interface declarations into headers.
- Reduce duplication and clarify ownership without moving heavy logic yet.

Completed files:
- `main/app_config.h` — all pure numeric/string #defines (display, WiFi, NVS keys, battery thresholds,
  ECG sampling/detection params, recording/SD params, respiration params)
- `main/app_state.h` — rec_row_t, rec_tab_t, health_tab_t
- `main/main.c` — includes added; moved definitions removed; hardware-specific defines
  (BOOT_BTN_GPIO, BATTERY_ADC_*, ECG_ADC_*, ECG_GPIO, SD_MOUNT_POINT) and LVGL color macros
  remain in main.c as they depend on ESP-IDF/LVGL enum types

Validation result:
- idf.py build passes clean, zero errors, zero warnings
- no behavior change (headers only, no logic moved)

### Phase 2 — UI common extraction ✓ DONE
Objective:
- Extract reusable UI helpers, common styles, and shared widget helpers.
- Keep screen-specific logic in place initially.

Completed files:
- `main/ui/ui_common.h` — COLOUR_* palette macros (moved from main.c), declarations for 6 helpers
- `main/ui/ui_common.c` — style_screen, style_button, style_card, make_title, safe_set_label, format_drift_text
- `main/CMakeLists.txt` — added ui/ui_common.c to SRCS, added "ui" to INCLUDE_DIRS
- `main/main.c` — added #include "ui_common.h"; removed COLOUR_* block; removed 6 function bodies

Not extracted in Phase 2 (too entangled — depend on main.c static vars or internal functions):
- nav_to_*_locked: reference static s_scr_* screen objects
- app_back_btn_cb, add_back_button: call reset_activity() and with_display_lock()
- Popup functions: s_popup owned by main.c; public popup API depends on with_display_lock
- reset_activity, screen_sleep, screen_wake: reference main.c static state and BSP

Validation result:
- idf.py build passes clean, zero errors; two pre-existing unused-function/variable warnings unaffected
- no behavior change (same logic, same call sites)

### Phase 3 — HAL extraction (battery) ✓ DONE
Objective:
- Extract battery ADC init and read functions into hal/.

Completed files:
- `main/hal/hal_battery.h` — BATTERY_ADC_* hardware config defines + API declarations
- `main/hal/hal_battery.c` — 3 static ADC handle vars + battery_adc_init, battery_read_voltage, battery_read_percent
- `main/CMakeLists.txt` — added hal/hal_battery.c to SRCS, added "hal" to INCLUDE_DIRS
- `main/main.c` — added #include "hal_battery.h"; removed BATTERY_ADC_* config block, Battery state section, 3 forward declarations, 3 function bodies

Wi-Fi, time sync, and recorder service extraction deferred (too entangled in single phase — share event groups, state flags, LVGL callbacks, FreeRTOS queues with main.c UI logic).

Validation result:
- idf.py build passes clean, zero errors; same two pre-existing warnings
- no behavior change (same logic, same call sites)

### Phase 4 — Signal pipeline extraction ✓ DONE
Objective:
- Extract biosignal simulation and filter functions into signal/.

Completed files:
- `main/signal/sig_pipeline.h` — conditional declaration of ecg_simulate_raw + signal_bandpass_step
- `main/signal/sig_pipeline.c` — both function bodies
- `main/app_config.h` — ECG_USE_SIMULATED_SOURCE define moved here from main.c
- `main/CMakeLists.txt` — added signal/sig_pipeline.c to SRCS, "signal" to INCLUDE_DIRS
- `main/main.c` — added #include "sig_pipeline.h"; removed #define ECG_USE_SIMULATED_SOURCE; removed ecg_simulate_raw body; removed signal_bandpass_step body and forward declaration

Not extracted (remain in main.c): ecg_adc_init/ecg_adc_read_raw (reference s_ecg_adc_handle/s_ecg_last_raw), ecg_sampler_task (deeply entangled with ECG/resp/recording state)

Validation result:
- idf.py build passes clean, zero errors; same two pre-existing warnings
- no behavior change

### Phase 5 — NVS credential helper extraction ✓ DONE
Objective:
- Extract NVS WiFi credential helpers into services/.

Completed files:
- `main/services/svc_nvs.h` — declarations for nvs_load_password_for_ssid, nvs_save_password_for_ssid, nvs_save_last_ssid
- `main/services/svc_nvs.c` — all 4 function bodies (ssid_to_key stays static/private)
- `main/CMakeLists.txt` — added services/svc_nvs.c to SRCS, "services" to INCLUDE_DIRS
- `main/main.c` — added #include "svc_nvs.h"; removed NVS credential helpers section (4 function bodies)

Validation result:
- idf.py build passes clean, zero errors; same two pre-existing warnings

### Phase 6 — Interface design and prerequisite extraction

All remaining candidates share FreeRTOS state, event groups, and/or LVGL callbacks with
main.c. Before extracting any of them the interfaces between modules must be agreed.
The main blocking problem is that service tasks (wifi_connect_task, wifi_scan_task)
call `with_display_lock` directly with LVGL-touching callbacks that reference main.c
screen objects. Those UI callbacks cannot move to the service — they must stay in main.c.
The solution is to decouple each service from the display lock and from LVGL entirely.

#### Prerequisite chain (ordered by risk and dependency)

**6A — `hal/hal_display`: move `with_display_lock` ✓ DONE**
- New files: `main/hal/hal_display.h`, `main/hal/hal_display.c`.
- Function renamed `hal_display_lock`; typedef renamed `hal_display_locked_fn_t`.
- main.c: added `#include "hal_display.h"`; removed typedef + function body; 24 call sites renamed.
- Build: passes clean, zero errors, same two pre-existing warnings.
- Unblocks: WiFi service, recording start/stop handlers.

**6B — `hal/hal_storage`: move SD mount/unmount ✓ DONE**
- New files: `main/hal/hal_storage.h`, `main/hal/hal_storage.c`.
- `sd_init()` renamed `hal_storage_mount()`; `s_sd_mounted` (volatile) moved to hal_storage.c.
- `hal_storage_is_mounted()` getter added.
- main.c: added `#include "hal_storage.h"`; removed sd_init body + s_sd_mounted declaration; 2 call sites updated.
- Build: passes clean, zero errors, same two pre-existing warnings.
- Unblocks: recording service extraction.

**6C — `services/svc_time`: move SNTP sync ✓ DONE**
- New files: `main/services/svc_time.h`, `main/services/svc_time.c`.
- `sntp_sync()` renamed `svc_time_sync()`; `s_time_synced` moved to svc_time.c; getter `svc_time_is_synced()` added.
- `wifi_shutdown_after_time_sync()` call moved OUT of sntp_sync and into wifi_connect_task call site (cleaner layering; same timing).
- main.c: `update_home_time_labels()` uses `svc_time_is_synced()`; `wifi_connect_task` calls `svc_time_sync()` then conditionally calls `wifi_shutdown_after_time_sync()`.
- Build: passes clean, zero errors, same two pre-existing warnings.

#### Higher-risk extraction phases (each needs interface design session before coding)

**6D — `services/svc_wifi`: WiFi service**
  Needs: 6A (hal_display_lock), svc_nvs (done), svc_time (6C).

  State to move: `s_wifi_event_group`, `s_ap_records`, `s_ap_count`, `s_retry_count`,
  `s_wifi_*` flags, `s_selected_ssid`, `s_password`.

  Key interface decision — notification pattern:
  - wifi_scan_task and wifi_connect_task currently call `with_display_lock` with
    LVGL callbacks (wifi_status_scanning_locked, wifi_status_done_locked,
    conn_status_locked, update_time_locked, nav_to_*_locked). These callbacks
    reference LVGL objects in main.c and must stay in main.c.
  - Chosen approach: dependency injection. Service accepts a `svc_wifi_event_cb_t`
    at init. Service calls back with a `svc_wifi_event_t` (enum: SCAN_START, SCAN_DONE,
    CONNECT_OK, CONNECT_FAIL, STATUS_MSG). main.c callback acquires display lock and
    updates UI.
  - Event data: for SCAN_DONE, callback carries pointer to ap_records + count. For
    STATUS_MSG, carries a const string.

  Proposed API sketch:
  ```
  typedef enum { SVC_WIFI_SCAN_START, SVC_WIFI_SCAN_DONE, SVC_WIFI_CONNECT_OK,
                 SVC_WIFI_CONNECT_FAIL, SVC_WIFI_STATUS_MSG } svc_wifi_event_t;
  typedef void (*svc_wifi_event_cb_t)(svc_wifi_event_t ev, void *data, void *ctx);

  void svc_wifi_init(svc_wifi_event_cb_t cb, void *ctx);
  void svc_wifi_start_scan(void);
  void svc_wifi_connect(const char *ssid, const char *pass);
  void svc_wifi_shutdown(void);
  bool svc_wifi_is_connected(void);
  bool svc_wifi_is_started(void);
  ```

**6E — `services/svc_record`: Recording service ✓ DONE**
  New files: `main/services/svc_record.h`, `main/services/svc_record.c`.

  State moved: `s_rec_queue`, `s_rec_mutex`, `s_rec_file`, `s_recording`,
  `s_rec_start_ms`, `s_rec_label`, `s_rec_filename`, `s_rec_writer_task` (8 vars).

  Functions privatised in svc_record.c: `rec_open_file`, `sd_writer_task`.
  `rec_stop_recording` became `svc_rec_stop()` (public).

  Public API delivered:
  ```
  esp_err_t svc_rec_init(void);
  esp_err_t svc_rec_start(const char *label);
  void      svc_rec_stop(void);
  void      svc_rec_enqueue(const rec_row_t *row);
  bool      svc_rec_is_recording(void);
  uint32_t  svc_rec_get_start_ms(void);
  ```
  Error codes: ESP_ERR_NO_MEM (queue), ESP_FAIL (file), ESP_ERR_INVALID_STATE (task).
  main.c updated: rec_startstop_btn_cb, ecg_sampler_task, rec_ui_timer_cb, app_main.

  Build: passes clean, zero errors; three pre-existing warnings (one newly visible).

**6F — `signal/sig_ecg`: ECG sampler task + all ECG/resp state**
  Needs: 6E (svc_record for the enqueue path).

  State to move: all `s_ecg_*` vars, `s_ecg_spinlock`, `s_resp_*` vars, `s_sim_phase`,
  `s_ecg_adc_handle`, `s_ecg_last_raw`.

  Key interface decision — UI reads ECG ring buffer under spinlock. Getter functions
  must acquire the spinlock internally so callers never touch the spinlock directly:
  ```
  int      sig_ecg_get_hr_bpm(void);
  int32_t  sig_ecg_get_drift_ms(void);
  void     sig_ecg_read_window(uint16_t *out_buf, size_t buf_len,
                                uint32_t *out_write_index);
  ```
  Recording link: `sig_ecg_start_task(void)` — task calls `svc_rec_enqueue()` directly
  (both are in the same component, or svc_record.h is included).

  Risk: HIGHEST. Touches timing-critical sampler task, beat detection state,
  respiration state, and the ring buffer shared with the LVGL chart path.
  Do only after all other phases are stable.

#### Design decisions agreed for this plan
1. Notification pattern: callback/dependency injection (not a second FreeRTOS queue).
2. Recording error reporting: return codes from svc_rec_start (no error callback).
3. ECG → recording coupling: svc_rec_enqueue called directly from sig_ecg task.
4. `s_time_synced` state: moves to svc_time, exposed via `svc_time_is_synced()` getter.
5. `with_display_lock` signature: unchanged.

Validation per sub-phase:
- After each sub-phase: `idf.py build` passes clean.
- After 6A: nav, popup, settings screens still work.
- After 6B: SD mounts and recording still writes files.
- After 6C: SNTP still syncs after WiFi connect; time hint label still correct.
- After 6D: WiFi scan populates list; connect saves credentials; SNTP syncs.
- After 6E: recording starts/stops; files written; status labels update.
- After 6F: graph updates; HR displayed; drift shown; recording captures signal.

### Phase 4 — Signal pipeline extraction
Objective:
- Extract biosignal generation, filtering, detection, HR/RR derivation, and drift metrics into `signal/`.
- Keep data flow equivalent to original working firmware.

Likely files:
- `signal/sig_pipeline.c`
- `signal/sig_pipeline.h`

Risks:
- subtle behavior regressions in displayed/recorded values
- graph feed mismatch
- queueing mismatch to recorder/UI

Validation:
- build
- graph updates correctly
- recorded data still sane
- HR/derived metrics still plausible

### Phase 5 — HAL/boundary cleanup
Objective:
- Separate board-specific setup and helpers from services/UI logic where practical.
- Keep hardware initialization order intact.

Likely files:
- `hal/hal_display.*`
- `hal/hal_touch.*`
- `hal/hal_storage.*`
- `hal/hal_battery.*`

Risks:
- hardware init regressions
- boot failures or partial feature breakage

Validation:
- boot test
- display/touch test
- SD test
- battery reading test if applicable

### Phase 6 — Integration and compile-fix pass
Objective:
- Reconcile headers, interfaces, app wiring, queues, task creation, and ownership boundaries.
- Remove temporary glue once build/runtime stability is proven.

Risks:
- broad compile fallout
- hidden dependency breakage

Validation:
- `idf.py build`
- flash and smoke test
- serial log review
- UI navigation
- Wi-Fi/time/SD/graph/record verification

### Phase 7 — Controlled cleanup
Objective:
- Do only low-risk cleanup after functionality is stable.
- Improve naming, comments, and local structure where safe.

Risks:
- accidental regressions from unnecessary cleanup

Validation:
- full build and smoke test again

## High-risk areas to preserve carefully
- Task priorities, core affinity, and timing loops
- USB/serial interaction history and any GPIO conflicts
- Graph rendering path and data feed path
- Recorder buffering and flush behavior
- Wi-Fi and SNTP init/teardown sequence
- LVGL screen object lifecycle

## Acceptance criteria
The refactor is successful when:
- the project builds cleanly,
- the device boots reliably,
- logs appear normally,
- UI/navigation works,
- biosignal graph is visible and updating,
- recording to SD works,
- Wi-Fi and time sync work,
- no major feature regression is introduced,
- module boundaries are clearer than in the monolithic version.

## Stage 2 hardware integration notes

### Battery E-Gauge accuracy
The AXP2101 E-Gauge (REG 0xA4) reports battery percentage and is already read by
`battery_read_percent()` in `hal/hal_battery.c`. The firmware falls back to a linear
voltage estimate when the gauge reports 0 (before cell learning completes on a fresh chip).

Once the final battery cell is chosen for the device, write its chemistry parameters to
**REG 0xA1** (Battery parameter register) inside `battery_adc_init()`. The AXP2101
datasheet (§6.11) explicitly recommends this: with battery parameters programmed, the
E-Gauge can initialise accurately on each boot rather than requiring several charge/discharge
cycles to learn the cell, eliminating the cold-start fallback condition.

Steps when the battery is finalised:
1. Obtain the battery cell's nominal capacity (mAh), chemistry, and cutoff voltage from
   the datasheet.
2. Encode them into the REG 0xA1 format per AXP2101 §6.13.2 (Battery parameter register).
3. Write the register in `battery_adc_init()` immediately after the probe succeeds.
4. Verify `battery_read_percent()` returns a non-zero value on cold boot without needing
   the voltage-fallback path.

## Recommended recurring prompt
Read `CLAUDE.md`, `MEMORY.md`, and `docs/refactor-plan.md` first. Summarize current state and execute the next phase only. Keep edits minimal, preserve behavior, and stop after listing changed files, risks, and validation steps.
