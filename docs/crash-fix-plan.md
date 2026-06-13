# crash-fix-plan.md — UI freeze / `_invalid_pc_placeholder` crash after ADS1293 + MAX30102 integration

**Audience:** Sonnet (executor).  **Author:** Opus (planner).
**Status:** Diagnosis not yet confirmed on hardware. **Do diagnostics FIRST, then fix.**
**Do not bulk-rewrite.** One change at a time, reflash, capture serial, compare.

---

## 1. Symptom summary (from three hardware logs, 2026-06-13/14)

| Log | Navigation | Outcome | Last healthy timestamp | First failure |
|-----|-----------|---------|------------------------|---------------|
| A | → Record screen, waited | Display freeze (no panic) | ECG raw to 43782 ms, `plot#1` at 37482 ms | `clock_update_task: failed to acquire display lock after 1000 ms` @ 45849 ms |
| B | → BP screen, recorded (OK) → Files | **No crash during BP**; freeze on Files | BP closed @ 42949, `listed 39 file(s)` @ 77208 | display-lock fail @ 78395 ms |
| C | → Settings → WiFi → Files | **Hard crash** then reboot | `Files screen active` @ 39595 | `_invalid_pc_placeholder` (reboot), display-lock fail @ 41080 |

### Invariants
1. Every failure manifests as **`s_lvgl_mutex` held > 1000 ms** → the **LVGL port task is stuck inside `lv_timer_handler()`** (it is the only holder during render; see [hal_display.c:80-94](main/hal/hal_display.c#L80-L94)).
2. Log C additionally shows **`_invalid_pc_placeholder`** = the CPU jumped to an invalid address. That is **memory/stack corruption or a dangling/garbage function pointer**, not a plain deadlock. Treat the hang and the crash as possibly the **same root cause** with two presentations.
3. Failures occur on **heavy-rendering screens** (Record chart; Files list of 39 items + HTTP) while the **ECG sampler task drives the ADS1293 on SPI2 at 100 Hz from the other core**. BP recording alone did **not** trip it (its chart is light: 64 points, 2 series, slow refresh).

---

## 2. What changed this session (the suspect surface)

All added in the PPG/SpO2 + 3-lead ECG work:
- **ADS1293 on SPI2** ([hal_ads1293.c](main/hal/hal_ads1293.c)): `spi_device_transmit()` (blocking) called every 100 Hz tick from `ecg_sampler_task` on **core 0**. **SPI2 is shared with the ILI9341 display (async DMA, core 1) and the XPT2046 touch.**
- **MAX30102 on I2C0** ([hal_max30102.c](main/hal/hal_max30102.c)): I2C read every tick; `sqrtf`-heavy SpO2 calc every 400 samples. (Separate peripheral — cannot stall SPI2, lower suspicion for the *display* freeze.)
- **3-lead chart**: third series + two extra ext arrays; `rec_update_plot()` now `memcpy`s **3 × 1600 B = 4800 B inside `portENTER_CRITICAL(&s_ecg_spinlock)`** ([main.c:2032-2041](main/main.c#L2032-L2041)).
- **DRDY GPIO ISR at ~853 Hz** on core 0 ([hal_ads1293.c:125-144](main/hal/hal_ads1293.c#L125-L144)) — **installed but its flag is never read** (reads are unconditional). Pure overhead + interrupt pressure.

---

## 3. Ranked hypotheses (with evidence)

### H1 — SPI2 bus contention/deadlock between display DMA (core 1) and ADS1293 blocking transmit (core 0). **PRIMARY.**
- **For:** Every freeze is on a screen with heavy display SPI2 traffic; the LVGL task is stuck in `lv_timer_handler` (where `flush_cb` → `esp_lcd_panel_draw_bitmap` runs); ADS1293 is the new concurrent SPI2 master, driven 100×/s from the *other* core. Mixing **async/queued DMA (esp_lcd) with blocking `spi_device_transmit` on the same host from two cores** is a known hazard.
- **Against:** The ESP-IDF bus lock *should* serialize this; a true permanent deadlock would be a driver bug. Needs the breadcrumb diagnostic (§4.3) to confirm the LVGL task is actually stuck in `esp_lcd_panel_draw_bitmap`.

### H2 — Memory exhaustion / heap fragmentation (no PSRAM). **STRONG for Log C crash.**
- **For:** Log C `_invalid_pc_placeholder` + Files screen builds ~39 list rows while WiFi (~130 KB) is up, the record chart/buffers persist, and only ~390 KB DRAM exists. A failed `lv_malloc` returning NULL mid-render can corrupt LVGL state → invalid PC. Boot heap shows fragmented regions (29 KB + 14 KB + 111 KB).
- **Against:** OOM more often yields a clean `LoadProhibited`/`assert`. Confirm with the heap monitor (§4.1).

### H3 — Stack overflow in the LVGL task during deep render. **STRONG for Log C crash.**
- **For:** `_invalid_pc_placeholder` is the canonical stack-overflow signature (corrupted return address). The Files list + concurrently-refreshing record chart deepen the render call tree. LVGL task stack is 8192.
- **Against:** None until measured. Confirm with high-water-mark monitor (§4.2) and by temporarily bumping the stack.

### H4 — Background record-chart timer touching off-screen / stale chart. **CONTRIBUTING.**
- **For:** `rec_ui_timer_cb` only guards `!s_screen_on` ([main.c:2216](main/main.c#L2216)); it never checks the active screen and `s_rec_ui_timer` is **never paused** ([main.c:2491-2492](main/main.c#L2491-L2492)). Once the Record chart is built, the timer keeps running `rec_update_plot()` → `lv_chart_refresh()` + the 4800 B spinlock `memcpy` on **every** other screen, **doubling LVGL/SPI2 load** exactly when the foreground screen (Files) is also heavy. Explains Log A (record chart present) and worsens B/C.
- **Note:** If a screen's chart object is ever deleted while `s_rec_plot` stays non-NULL, this becomes a use-after-free → could *be* the invalid PC. Verify the chart is never deleted out from under the timer.

### H5 — Long `portENTER_CRITICAL(&s_ecg_spinlock)` sections. **CONTRIBUTING, not prime.**
- **For:** `rec_update_plot` holds the spinlock for the 4800 B memcpy (core 1) while `ecg_sampler_task` holds it ~every 10 ms (core 0) across a long section incl. the deferred R-peak loop + resp calc. Cross-core spinlock contention with IRQs disabled adds latency and jitter (`rec_timer: slow 20-28 ms` warnings in Log B confirm stalls).
- **Against:** µs-to-ms scale, not the >1000 ms hang by itself.

### H6 — `ESP_LOG` or other blocking call inside a critical section. **CHECK & RULE OUT.**
- `ppg_det_update_sample()` runs **inside** `s_ecg_spinlock` ([main.c:1554](main/main.c#L1554)). If anything on that path (or the SpO2 path) calls `ESP_LOGx`/`printf`/takes a mutex while IRQs are disabled, it can wedge the console and risk deadlock. The `PPG_DET:` log fires from this region's vicinity — confirm where it is emitted and that it is **not** inside the spinlock.

---

## 4. Diagnostics to add FIRST (no behavioural fixes yet)

Goal: one reflash that makes the next failure self-identify. Add a lightweight `diag` task and breadcrumbs. Keep all of this behind a `#define CRASH_DIAG 1` in [app_config.h](main/app_config.h) so it can be compiled out later.

### 4.1 Heap monitor (1 Hz)
In an existing 1 Hz context (e.g. `clock_update_task`) log:
```
esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
```
A steady decline → H2. A cliff right before failure → H2 confirmed.

### 4.2 Per-task stack high-water marks (1 Hz)
Store the `TaskHandle_t` for `lvgl`, `ecg_sampler`, `clock_update`, and the Files/HTTP task. Log `uxTaskGetStackHighWaterMark(h)` for each. Any value approaching 0 (esp. the LVGL task) → H3. Also: temporarily raise the LVGL task stack 8192 → 12288 ([hal_display.c:27](main/hal/hal_display.c#L27)); if the crash moves or disappears, H3 confirmed.

### 4.3 Breadcrumb in the SPI/flush path (pinpoints H1)
Add a `volatile uint32_t s_flush_phase` (and timestamp) set around the flush:
```
flush_cb: s_flush_phase = 1 (before draw_bitmap); call; s_flush_phase = 2 (returned)
on_trans_done ISR: s_flush_phase = 3
```
And a matching `s_ads_phase` around the ADS1293 `spi_device_transmit` in [hal_ads1293.c:63](main/hal/hal_ads1293.c#L63). The 1 Hz diag task logs both. When the hang occurs: `s_flush_phase` stuck at **1** ⇒ display blocked in `esp_lcd_panel_draw_bitmap` ⇒ **H1 confirmed**. `s_ads_phase` stuck ⇒ ADS1293 transmit never returned.

### 4.4 Enable Task Watchdog with backtrace (single most valuable diagnostic)
Turn on the Task WDT (`CONFIG_ESP_TASK_WDT_*`) and subscribe the LVGL + ECG tasks (`esp_task_wdt_add`). On the next hang the WDT prints the **stuck PC + backtrace of each task** — this alone likely identifies the exact blocking call. Pair with `idf.py monitor` so addresses are symbolized. (The Interrupt WDT will already panic if a spinlock is held too long with IRQs off — its absence in the logs argues against a pure §H5 spinlock-forever.)

### 4.5 Check return codes
`flush_cb` currently ignores the return of `esp_lcd_panel_draw_bitmap`; `hal_ads1293_read_ecg` checks its return but the caller logs nothing on error. Log non-`ESP_OK` from both. A burst of SPI errors right before the hang → H1.

---

## 5. Fixes — apply after diagnostics point to the cause

Order by likelihood × cheapness. **Apply and test ONE at a time.**

### Quick wins (do regardless — low risk, reduce load/noise)
- **F0 — Remove the unused DRDY GPIO ISR.** Reads are unconditional; the 853 Hz ISR is pure overhead on core 0. Delete the `gpio_install_isr_service`/`gpio_isr_handler_add`/`ads1293_drdy_isr_handler` path in [hal_ads1293.c](main/hal/hal_ads1293.c) and configure GPIO4 as a plain input (or leave unconfigured). Keep the init-time DRDY *poll* if desired, or drop it too.
- **F1 — Pause the record UI timer when off the Record screen (fixes H4).** In `rec_ui_timer_cb` return early unless the Record screen is the active screen (`lv_screen_active() == s_scr_record`), **or** `lv_timer_pause(s_rec_ui_timer)` on nav-away and `lv_timer_resume` on nav-to. This stops the background chart refresh from doubling SPI2/LVGL load on Files/Settings/WiFi/BP.
- **F2 — Throttle the `ECG raw` 1 Hz log and the `PPG_DET` log** (or gate behind `CRASH_DIAG`) once diagnostics are done — they are not free.

### Primary fix candidates for H1 (SPI2 contention)
Pick based on §4.3 / §4.4 results:
- **F3 — Serialize all SPI2 access through one lock.** Wrap ADS1293 (and touch) transactions so they cannot run while a display flush is in flight, e.g. take a shared `s_spi2_mutex` in `hal_ads1293_read_ecg` and in the display flush path. Risk: the esp_lcd flush is async (returns before DMA done) — the mutex must be held until `on_trans_done`, which complicates the flush. Prefer F4/F5 if this gets hairy.
- **F4 — Reduce ADS1293 SPI traffic.** The ECG pipeline runs at 100 Hz but the chip is at ~853 SPS; you do **not** need an SPI transaction every 10 ms if you only consume one sample. Confirm one 10-byte read per tick is the minimum; consider reading less often or batching. Fewer SPI2 transactions = less contention window.
- **F5 — Move the ADS1293 read off the hard 100 Hz timing path / lower its task priority** so it yields the bus to the display more readily. (ECG sampler is priority 8; the LVGL task is 4. The high-priority core-0 task repeatedly grabbing SPI2 can starve the core-1 display flush of the bus.)
- **F6 — Confirm the ADS1293 `spi_device_interface_config_t` coexists cleanly** with esp_lcd on SPI2: it must be added **after** `spi_bus_initialize` (it is, in `hal_display_init`), `queue_size` adequate, and not using `SPI_DEVICE_NO_DUMMY`/half-duplex flags that conflict. Verify mode-3 device switching against the mode-0 display isn't the stall point (the breadcrumb will show).

### Fix candidates for H2 / H3 (corruption / OOM — Log C)
- **F7 — Free or hide the record chart + its buffers when leaving the Record screen**, and ensure `s_rec_plot` is nulled atomically with any `lv_obj_del` (it is at [main.c:2338-2339](main/main.c#L2338-L2339) — verify nothing else deletes it). Reclaims memory for the Files screen.
- **F8 — Reduce graph resolution / memory (user's suggestion).** `REC_CHART_POINTS` is 200; `s_rec_chart_points` is sized `ECG_WINDOW_SAMPLES` (400). Dropping points and/or shrinking the oversized array trims DRAM and per-refresh work. The three static `ecg{1,2,3}_copy[400]` + `ppg_copy[400]` copies (~6.4 KB BSS) are fine as BSS but the *refresh cost* scales with points.
- **F9 — Bump LVGL task stack** (8192 → 12288) as a test; if it cures Log C, keep an appropriate value and document why.

### Fix candidate for H5/H6 (critical-section hygiene)
- **F10 — Shrink the `s_ecg_spinlock` critical sections.** Move `ppg_det_update_sample`, the deferred R-peak search loop, and `resp_rate_from_intersections_locked()` **out** of the spinlock where possible (copy needed inputs under lock, compute outside). In `rec_update_plot`, the 4800 B `memcpy` under spinlock is the worst offender on the UI side — consider double-buffering so the copy is lock-free, or splitting per-lead. **Ensure no `ESP_LOGx`/`printf`/blocking call executes inside any `portENTER_CRITICAL`.**

---

## 6. Suggested execution sequence for Sonnet

1. Add §4 diagnostics behind `CRASH_DIAG` (heap, stack HWM, flush/ADS breadcrumbs, Task WDT, return-code logging). Apply **F0** and **F1** at the same time (they are safe and reduce noise/load). Build, flash, reproduce each of the three navigation paths, capture serial.
2. Read the diagnostic output:
   - LVGL stack HWM near 0 or crash cured by bigger stack → **H3** → F9 (+ F8/F7).
   - Heap cliff before failure → **H2** → F7/F8.
   - `s_flush_phase` stuck at 1 / Task-WDT backtrace in `esp_lcd_panel_draw_bitmap` → **H1** → F4/F5 (then F3 if needed).
3. Apply the indicated primary fix, reflash, re-test all three paths. Iterate one change at a time.
4. Once stable, gate or remove the diagnostics, re-enable normal logging cadence, and **update [CLAUDE.md](CLAUDE.md) §9 Known Issues** and add a memory note recording the confirmed root cause + fix.

---

## 7. Notes / unrelated items observed (do NOT fix in this pass unless trivial)

- **SpO2 not shown on screen:** `rec_update_topbar` hard-codes `"SpO2 --%"` ([main.c:768-769](main/main.c#L768-L769)) and never reads `s_spo2_latest`. One-line fix (mirror the HR label), but defer until the crash is resolved to keep the diff clean.
- **ECG raw values rail to ±2^23** in the logs (e.g. Log B `CH3=-8208795`) — electrode/lead signal-quality issue, scaled+clamped in `ecg_adc_read_raw`; not a crash cause.
- **PPG display inversion** has been applied in `rec_fill_ppg_plot` ([main.c](main/main.c)) this session (`y = 1000 - y`). The earlier remark that *ECG* also needs inverting was **not** actioned — confirm with the user before inverting ECG, since the ADS1293 lead polarity may already be correct.
- `gitStatus` showed an untracked file literally named `next sesssion` and `Wearable_Device_Firmware/` — unrelated.
