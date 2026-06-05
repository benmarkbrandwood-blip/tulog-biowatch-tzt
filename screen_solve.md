# screen_solve.md — Plan to get the TZT display rendering correctly

> Methodical, evidence-led plan. **No code yet.** Companion to CLAUDE.md §11.
> Owner: continue from the 2026-06-06 session. Board: TZT ESP32-2432S024C
> (2.4″, capacitive CST820). Display driven via ESP-IDF `esp_lcd` + LVGL 9.5.

---

## 1. Objective (the only definition of "done")

On the `ORIENTATION_TEST=1` four-quadrant screen, held **landscape with USB on the
left**:

1. **Full fill** — no grey/snow strip anywhere.
2. **Correct geometry** — RED top-left, GREEN top-right, BLUE bottom-left,
   YELLOW bottom-right (each a true quarter of 320×240).
3. **Text readable and NOT mirror-reversed.**
4. **Colours correct** (already achieved — see §3).

Only when all four hold do we set `ORIENTATION_TEST 0` and move to UI relayout.

---

## 2. Where we are now (facts, not theories)

| Item | State |
|---|---|
| Colours | ✅ **SOLVED.** `LCD_RGB_ELEMENT_ORDER_RGB` + `lv_draw_sw_rgb565_swap()` in flush. Keep both. |
| Boot / memory | ✅ No crash. `Display: Init OK — ILI9341 320×240, 2×10240-byte draw buffers`. DRAM 75 KB + 111 KB regions free at init. |
| Geometry | ❌ Text mirror-reversed; quadrants scrambled/duplicated (green both sides, red centre column, blue centre column); grey strip persists; image still effectively portrait, 90° out. |

Current implementation (on disk + flashed): **LVGL software rotation** —
[main/hal/hal_display.c](main/hal/hal_display.c) `lvgl_flush_cb` rotates each chunk
with `lv_display_rotate_area()` + `lv_draw_sw_rotate()`; panel left in native
portrait MADCTL; `LCD_LV_ROTATION=LV_DISPLAY_ROTATION_270` in
[main/app_config.h](main/app_config.h).

---

## 3. New evidence — the forum's working CYD code

A user got a Cheap-Yellow-Display working in landscape with **TFT_eSPI**:

- `#define ST7789_DRIVER` — **not** ILI9341.
- `TFT_RGB_ORDER TFT_BGR`, `TFT_INVERSION_OFF`, native `TFT_WIDTH 240 / HEIGHT 320`.
- `tft.setRotation(1)` → landscape (320×240).
- Touch: `tx = map(p.y, …)`, `ty = map(p.x, …)` → **x/y swapped** in landscape,
  plus `touchscreen.setRotation(1)`.
- **Their board is the 2.8″ `2432S028R` (resistive, XPT2046).** Ours is the 2.4″
  `2432S024C` (capacitive, CST820) — so pins/touch differ and the **panel
  controller may differ too** (see Phase 0).

### Why this matters most
`tft.setRotation(1)` **proves hardware-MV landscape works on this hardware
family**: with MV set, the controller accepts `CASET 0..319` and displays
row-major data correctly. TFT_eSPI achieves this by writing a **complete MADCTL
byte**, then pushing pixels into a 320-wide address window.

---

## 4. The central contradiction (must be resolved, drives the plan)

> TFT_eSPI proves hardware-MV landscape works, **yet our earlier `esp_lcd`
> `swap_xy=true` attempt ("config B") sheared.**

Leading hypothesis (testable): `esp_lcd_panel_swap_xy()` / `esp_lcd_panel_mirror()`
in [esp_lcd_ili9341.c](managed_components/espressif__esp_lcd_ili9341/esp_lcd_ili9341.c)
only **OR/clear the single MV / MX / MY bits** onto whatever MADCTL the component's
init left. If that init leaves a scan-order bit set (**MH bit2 / ML bit4**, or an
MX/MY the test didn't clear), the MV combination produces a wrong fill order →
shear. TFT_eSPI avoids this by writing the *entire* MADCTL (e.g. `0x28`, `0xE8`),
overwriting every bit.

**Implication:** prefer setting the **complete MADCTL register explicitly** over
relying on `swap_xy`+`mirror`. This is the spine of Phase 1.

Secondary hypothesis: the panel is actually an **ST7789** (forum evidence). An
ILI9341 init on an ST7789 panel can give coherent portrait but wrong landscape
scan and/or a missing offset (the grey strip). Resolved in Phase 0 / Phase 3.

---

## 5. Plan — ordered by (confidence × low effort)

### Phase 0 — Confirm the panel controller ✅ COMPLETE (2026-06-06)

**Result: ILI9341 confirmed. Keep the current `esp_lcd_ili9341` driver.**

Evidence:
- `0xD3` read returned `00 00 00 00` — ILI9341 `0xD3` SPI read is unreliable on
  4-wire mode without exact dummy-cycle handling; inconclusive but not disqualifying.
- `RDDID 0x04` returned `00 78 1E 00` — non-zero response; byte 3 is `00` not `0x41`
  but the dummy-cycle offset likely accounts for this.
- **Decisive:** the ILI9341-specific vendor init commands (`0xCF`, `0xED`, `0xE8`,
  `0xCB`, `0xF7`…) run without error and the display shows coherent content.
  An ST7789 would ignore or mishandle these commands. The 2.4″ capacitive factory
  `User_Setup.h` explicitly uses `#define ILI9341_DRIVER` — the manufacturer's own
  reference confirms it.

`s_io_handle` is now stored as module state in `hal_display.c`, ready for
Phase 1's explicit MADCTL write.

### Phase 1 — PRIMARY fix: hardware landscape via an explicit MADCTL byte
*Rationale: proven on this hardware by the forum; simpler than software rotation;
no per-frame CPU cost; no 3rd buffer; sidesteps the SW-rotation placement bug.*

- Revert the flush to the **plain baseline** (byte-swap + `draw_bitmap`, no
  `lv_draw_sw_rotate`, no `lv_display_set_rotation`). Create the LVGL display at
  **320×240 logical** directly.
- After `esp_lcd_panel_init()`, **write the full MADCTL** with
  `esp_lcd_panel_io_tx_param(io, 0x36, {VALUE}, 1)` — do **not** use
  `swap_xy`/`mirror` (they only touch single bits).
- **Colour bit:** we verified esp_lcd **RGB** is correct, so the MADCTL **BGR bit
  `0x08` must be CLEAR**. Therefore test the BGR-clear landscape values:
  - `0x20` (MV only)
  - `0xA0` (MV+MY), `0x60` (MV+MX), `0xE0` (MV+MX+MY)
  (These are TFT_eSPI's `0x28/0xE8/…` with the colour bit removed.)
- **Test matrix** — for each value, on the quadrant screen record:
  (a) sheared? (b) which corner is RED? (c) text mirrored? (d) full fill?
  Choose the value that gives full-fill + RED top-left + readable text with
  USB-left. `0xE0` is the prime candidate (reference: TFT_eSPI rotation 3 = USB-left).
- If **every** value still shears → hardware-MV path is genuinely unusable here;
  fall through to Phase 2.

### Phase 2 — FALLBACK: repair the software rotation
*Only if Phase 1 cannot be made to work.* The duplication is a **placement/stride
bug** in `lvgl_flush_cb`, not a rotation-direction choice.
- Add temporary `ESP_LOGI` for the first ~8 flushes dumping: logical
  `area{x1,y1,x2,y2}`, `w`, `h`, the computed physical rect, `src_stride`,
  `dst_stride`. Confirm the rotated strips tile native 240×320 with **no overlap
  and no gap** (per the math: ROTATION_270 maps logical top-strip → physical
  rightmost 16 columns, marching left).
- Verify the **src stride assumption**: in LVGL 9.5 partial mode, is `px_map`
  tightly packed (`stride = w×2`) or the full draw-buffer stride? If LVGL packs to
  the buffer width, our `lv_draw_buf_width_to_stride(w,…)` is wrong → exactly this
  duplication. Check `LV_DRAW_BUF_STRIDE_ALIGN`.
- Confirm `lv_draw_sw_rotate` output layout (tight, `dst_stride = phys_width×2`)
  matches what `esp_lcd_panel_draw_bitmap` expects.
- Confirm `LV_DRAW_SW_SUPPORT_RGB565` is enabled in sdkconfig.

### Phase 3 — Grey strip / panel offset
- If a strip remains **after** geometry is otherwise correct, it is an
  **unaddressed GRAM region = a panel offset**, not stale content.
- Apply `esp_lcd_panel_set_gap(panel, x_gap, y_gap)` (ST7789 commonly needs a
  rotation-dependent offset). Derive the offset from the strip width.
- Re-confirm the boot-time GRAM clear loop covers the full addressable area in the
  chosen orientation.

### Phase 4 — Touch alignment *(after the display is correct)*
- LVGL auto-rotates indev points (`lv_indev.c:743 lv_display_rotate_point`) **iff**
  the driver reports **native-frame** coords. Forum proves landscape needs x/y
  swapped + axis-mapped.
- Decide: report raw CST820 native coords and let LVGL rotate, **or** swap/mirror
  in [main/hal/hal_touch.c](main/hal/hal_touch.c). If Phase 1 (hardware MADCTL, no
  LVGL rotation) wins, LVGL will **not** auto-rotate touch — so the swap/mirror
  must be done in the touch driver to match the chosen MADCTL.
- Verify by touching known on-screen targets (e.g. add temporary corner dots).

### Phase 5 — Lock config + UI relayout
- Bake the winning MADCTL/orientation into [main/hal/hal_display.c](main/hal/hal_display.c),
  remove diagnostics, set `ORIENTATION_TEST 0`.
- Separate task: relayout the real UI screens from 240×320 portrait to 320×240
  landscape (tracked in CLAUDE.md §9).

---

## 6. Verification protocol (run every iteration)

1. `ORIENTATION_TEST=1`.
2. `. …/export.sh && idf.py build && idf.py -p /dev/ttyUSB0 flash`.
3. Capture boot log → confirm `Display: Init OK`, free heap healthy, no panic.
4. Hold device **USB-left**; photograph; record against the four §1 criteria.

---

## 7. Decision table (symptom → cause → action)

| Symptom | Most likely cause | Action |
|---|---|---|
| Vertical-stripe shear | MADCTL scan bits wrong (MH/ML or partial bit OR'ing) | Phase 1: write full MADCTL byte |
| Text mirror-reversed | Wrong MX (or MY) bit for this orientation | Phase 1: flip MX/MY in the MADCTL value |
| Quadrants duplicated/centre columns | SW-rotation stride/placement bug | Phase 2: log strides; fix `src_stride` |
| Image 90° wrong but coherent | Wrong rotation/MV axis | Phase 1: toggle MV / use the other landscape value |
| Grey strip with otherwise-correct image | Panel offset (gap), or clear miss | Phase 3: `set_gap`; re-check clear extent |
| Colours swapped | (already solved) BGR vs RGB bit | Keep RGB (BGR bit `0x08` clear) + byte-swap |
| Touch offset/rotated | Touch frame not matched to display orientation | Phase 4: swap/mirror touch coords |

---

## 8. Guiding principles for the next session

- **Empirical over theoretical.** Pure MADCTL reasoning has misfired twice; lean on
  the test matrix and the forum's known-good values.
- **One variable per flash.** Change a single MADCTL bit (or one toggle), reflash,
  photograph, record. No bundled changes.
- **Write the whole MADCTL, don't OR bits** — this is the core lesson from the
  TFT_eSPI vs esp_lcd contradiction.
- **Colour is done** — never reintroduce BGR; keep the byte-swap.
- Prefer the **hardware-MADCTL path (Phase 1)**; it's proven on this hardware and
  removes the software-rotation complexity entirely.
