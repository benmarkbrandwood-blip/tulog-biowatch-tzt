# Button Fixes — Plan

## Root cause: `hal_display_lock()` inside LVGL callbacks = deadlock

LVGL event callbacks run from `lv_timer_handler()` inside `lvgl_task`, which holds
`s_lvgl_mutex`. `hal_display_lock()` tries to acquire the same mutex via
`xSemaphoreTake()`. Result: timeout every time. The button animates (visual feedback is
separate) but the navigation action is never executed.

### Affected callbacks (all call `hal_display_lock` then a nav function):
- `wifi_back_btn_cb` — Back button on WiFi scan screen
- `wifi_main_btn_cb` — Main Menu button on WiFi scan screen
- `settings_wifi_btn_cb` — Connect button on Settings screen
- `files_wifi_btn_cb` — Connect WiFi button on Files screen
- `files_send_btn_cb` — Send button on Files screen (check)
- `files_delete_btn_cb` — Delete button on Files screen (check)

### Fix
Remove all `hal_display_lock()` wrappers from LVGL event callbacks. Call the navigation
functions (e.g. `lv_screen_load_anim`, task creation) directly — they are already running
under the mutex. Pattern to replace:

```c
// BEFORE (deadlocks):
hal_display_lock(DISPLAY_LOCK_SHORT_MS, "cb_name", nav_fn, NULL);

// AFTER (direct call, already under mutex):
nav_fn(NULL);
// OR inline the action:
lv_screen_load_anim(s_scr_target, LV_SCR_LOAD_ANIM_FADE_IN, 240, 0, false);
```

The helper functions `nav_to_home_locked`, `nav_to_wifi_locked`, etc. were designed for
use from NON-LVGL tasks (e.g. the boot button task). LVGL callbacks must call LVGL
functions directly, never through `hal_display_lock`.

---

## Other pending fixes (from hardware test session 2026-06-06)

### 1. Default brightness — too bright
- File: `main/app_config.h`
- `DEFAULT_BRIGHTNESS 80` → `50`

### 2. Touch Y offset — registers 40 px above finger
- File: `main/hal/hal_touch.c`
- Add `#define XPT_Y_OFFSET 40`
- Apply in `xpt2046_read_cb`:
  ```c
  int16_t ly_raw = (int16_t)(LCD_V_RES - 1)
                   - xpt_map(raw_y, XPT_Y_MIN, XPT_Y_MAX, LCD_V_RES - 1);
  int16_t ly = ly_raw + XPT_Y_OFFSET;
  if (ly >= (int16_t)LCD_V_RES) ly = (int16_t)(LCD_V_RES - 1);
  ```
- Side effect: top ~40 px of touch range becomes inaccessible (topbar area, which has
  no interactive buttons — tab buttons are at y=49..71, still reachable).

### 3. BP screen — hint label too tall
- File: `main/main.c`, `ui_create_bp_screen`
- Hint: `"RR (cyan)  PAT (orange)  BOOT: Home"` with `font_montserrat_14`
- Fix: use `lv_font_montserrat_12` and shorten text to `"RR cyan  PAT orange"`
  (remove "BOOT: Home" — self-evident from other screens)

### 4. Record screen — topbar too low, tab buttons hard to hit
- "Lift top text one character height": move topbar from y=4 to y=1.
- Tab buttons at y=49..71 are 22 px tall — small for resistive touch.
  Consider replacing the 6 small tab buttons with two cycle buttons:
  ```
  [< Prev]  [ECG ▾]  [Next >]
  ```
  - "< Prev" cycles backward through signals
  - "Next >" cycles forward through signals
  - Centre label shows current signal name
  This gives two 110 px wide touch targets instead of six 38 px wide ones.

### 5. Settings screen — crowded, WiFi not navigating
- WiFi navigate fix is covered in §Root cause above (deadlock).
- Remove the bottom note text (health screen scaffolding note) — user-facing noise.
  Lines ~2695-2703: delete the `lv_obj_t *note = ...` block.
- Consider reducing `settings_row` panel height from 56 → 46 to give more space.

### 6. Wifi screen — rescan / list working but buttons dead
- Fix is the `hal_display_lock` deadlock (§Root cause).
- After fix, verify `wifi_rescan_btn_cb` (no lock needed, it just triggers a scan task).

### 7. Files screen — wifi connect and send/delete dead
- Fix is the `hal_display_lock` deadlock (§Root cause).
- After fix, test send and delete paths with a real file selected.

---

## Merge to main
Before starting these fixes, merge `touchscreen-solve` to `main`:
```sh
git checkout main && git merge --no-ff touchscreen-solve -m "merge: touchscreen-solve → main"
git push
```
