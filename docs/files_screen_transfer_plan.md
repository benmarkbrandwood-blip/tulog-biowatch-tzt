# Files Screen Wi-Fi Transfer Plan for Claude
**Project:** tulog-biowatch  
**Date:** 2026-05-11  
**Scope:** Watch firmware only — add a Files screen that lists SD-card recordings and transfers selected files over Wi-Fi to a PC running a small Python receiver on a hotspot.

---

## Goal

Add a real **Files screen** to the watch so the user can:

1. Open the Files screen from the home menu.
2. See a list of files present on the SD card.
3. Connect the watch to a PC hotspot using the project’s existing Wi-Fi station flow.
4. Send one file, or later all files, to a Python receiver script running on the PC.
5. Get clear on-watch status feedback: SD mount status, Wi-Fi connected/not connected, transfer progress, success/failure.

This stage is **transport only**. The PC side only needs to receive and save files for now.

---

## Current repo facts to anchor the design

Use the current updated repo as the source of truth:

- `main/main.c` already has working Wi-Fi STA infrastructure, including scan/connect/event handling and `s_wifi_connected` state.
- `main/hal/hal_storage.c` already mounts the SD card lazily using `bsp_sdcard_mount()` and exposes `hal_storage_mount()` / `hal_storage_is_mounted()`.
- The home screen already routes the Files tile to `s_scr_files`, but there is currently no implemented Files screen creation path in the visible code.
- The project already records to SD card using timestamped CSV files, including Record and BP workflows.
- `main/CMakeLists.txt` already depends on `esp_wifi`, `esp_event`, `esp_netif`, `lwip`, `fatfs`, `sdmmc`, and related components, so the networking foundation is already present.
- The current codebase already added `svc_bp_record.c`, so do not assume the earlier repo state.

---

## Recommended architecture

Use a **watch-as-client, PC-as-server** design.

### Why this direction

The watch already operates in **Wi-Fi station mode** and connects to an access point. A PC hotspot is naturally the AP. Once connected, the watch should initiate HTTP requests to the PC receiver. This avoids needing the ESP32 to host a web server for this feature and keeps the receiver logic simpler.

### Protocol choice

Use **HTTP POST** from the watch to the PC receiver.

Why:
- Simpler than raw TCP for a staged feature.
- Easy to debug with logs and curl.
- Matches ESP-IDF’s `esp_http_client` model well.
- Easy to implement in Python with Flask or a minimal `BaseHTTPRequestHandler` server.

### Transfer model

Use **one file per POST request**.

Recommended endpoint on PC:
- `POST /upload`

Recommended headers from watch:
- `X-Filename: <basename>`
- `X-File-Size: <bytes>`
- `X-Session-Type: record|bp|unknown`
- `Content-Type: application/octet-stream`

HTTP body:
- raw file bytes streamed from SD card in chunks

Do **not** use multipart/form-data in Stage 1. Keep it simple: raw body + headers.

---

## Watch-side implementation plan

### 1. Add a Files service component

Create a new service pair:

```text
main/services/svc_files.h
main/services/svc_files.c
```

Purpose:
- Enumerate files on SD card.
- Expose a small in-memory file list model for the UI.
- Send a selected file to the PC over HTTP.

### 2. Add data structures

Append to `main/app_state.h`:

```c
#pragma once

typedef enum {
    FILE_KIND_UNKNOWN = 0,
    FILE_KIND_RECORD,
    FILE_KIND_BP
} file_kind_t;

typedef struct {
    char        name[64];
    char        path[128];
    uint32_t    size_bytes;
    file_kind_t kind;
} watch_file_entry_t;

typedef struct {
    watch_file_entry_t items[64];
    uint16_t           count;
    bool               sd_ok;
} watch_file_list_t;

typedef struct {
    bool        active;
    bool        done;
    bool        success;
    uint32_t    bytes_sent;
    uint32_t    bytes_total;
    char        filename[64];
    char        message[96];
} file_tx_status_t;
```

### 3. Add config constants

Append to `main/app_config.h`:

```c
/* -------------------------------------------------------------------------- */
/* Files / Wi-Fi transfer                                                     */
/* -------------------------------------------------------------------------- */

#define FILES_MAX_LIST_COUNT        64
#define FILES_LIST_NAME_MAX         64
#define FILES_PATH_MAX              128
#define FILES_TX_CHUNK_BYTES        1024
#define FILES_HTTP_TIMEOUT_MS       15000
#define FILES_SERVER_PORT           8000
#define FILES_SERVER_PATH           "/upload"
#define FILES_REFRESH_STACK_BYTES   6144
#define FILES_UPLOAD_STACK_BYTES    8192
```

Use a fixed server IP string in Stage 1, configurable near the top of `svc_files.c`, for example:

```c
static const char *s_files_server_host = "192.168.137.1";
```

Do **not** over-engineer dynamic discovery yet. Stage 1 can use a fixed IP for the PC hotspot gateway.

---

## 4. `svc_files.c` responsibilities

### 4.1 `svc_files_refresh_list()`

Prototype:

```c
esp_err_t svc_files_refresh_list(watch_file_list_t *out);
```

Responsibilities:
- Ensure SD is mounted via `hal_storage_mount()` if needed.
- Enumerate files in `BSP_SD_MOUNT_POINT` using `opendir()` / `readdir()`.
- Ignore directories and hidden/system entries.
- Save basename, full path, file size.
- Infer `kind` using filename suffix rules:
  - ends with `_bp.csv` => `FILE_KIND_BP`
  - ends with `.csv` => `FILE_KIND_RECORD`
  - otherwise `FILE_KIND_UNKNOWN`
- Sort newest-first by filename if possible, since the filenames are timestamp-prefixed.

Implementation note:
- If you do not want to stat every file, use `stat()` only for files actually shown.
- Keep Stage 1 capped at 64 entries.

### 4.2 `svc_files_send_file()`

Prototype:

```c
esp_err_t svc_files_send_file(const watch_file_entry_t *entry,
                              file_tx_status_t *status);
```

Responsibilities:
- Open the file from SD card.
- Build URL from host + port + `/upload`.
- Use `esp_http_client` in HTTP POST mode.
- Send file bytes in 1024-byte chunks.
- Update `status->bytes_sent` while transferring.
- Mark success/failure and a short human-readable message.

Recommended pattern:
- Use `esp_http_client_init()`
- `esp_http_client_set_method(client, HTTP_METHOD_POST)`
- Add headers:
  - `X-Filename`
  - `X-File-Size`
  - `X-Session-Type`
- Open request with known content length.
- Loop over `fread()` and `esp_http_client_write()`.
- Read response status code.
- Require HTTP 200 for success.

Do not attempt TLS in Stage 1.
Do not attempt resumable uploads in Stage 1.

### 4.3 Background task wrapper

The actual send should run in a worker task, not directly in the LVGL callback.

Add:

```c
void svc_files_start_send_task(const watch_file_entry_t *entry);
const file_tx_status_t *svc_files_get_tx_status(void);
bool svc_files_is_busy(void);
```

Internally:
- Copy the selected entry into static service state.
- Spawn `files_upload_task`.
- Update a static `file_tx_status_t` as the transfer progresses.

This mirrors the project’s existing use of background tasks for Wi-Fi connect and file writing.

---

## 5. Files screen UI in `main.c`

Create a real `ui_create_files_screen()` and call it from `ui_create_app_screens()`.

### 5.1 New globals

Add near the other screen globals:

```c
static lv_obj_t   *s_files_list_container   = NULL;
static lv_obj_t   *s_lbl_files_status       = NULL;
static lv_obj_t   *s_lbl_files_transfer     = NULL;
static lv_obj_t   *s_btn_files_refresh      = NULL;
static lv_obj_t   *s_btn_files_send         = NULL;
static lv_obj_t   *s_btn_files_connect      = NULL;
static lv_timer_t *s_files_ui_timer         = NULL;
static watch_file_list_t s_files_model      = {0};
static int        s_files_selected_index    = -1;
```

### 5.2 Screen layout

Suggested layout for 410 × 502 px:

```text
y=14   Title: "Files"
y=48   Status label (SD/Wi-Fi state)
y=78   Button row: Refresh | Connect WiFi | Send
y=128  Scrollable file list card
y=442  Transfer status / progress
y=480  Bottom hint
```

### 5.3 Required controls

- **Refresh** button
  - Reload file list from SD.
- **Connect WiFi** button
  - If not connected, navigate to or invoke the existing Wi-Fi connect flow.
  - If already connected, show current connected state.
- **Send** button
  - Sends the selected file.
  - Disabled if no file selected, Wi-Fi disconnected, or transfer already active.

### 5.4 File list presentation

Each row should show:
- filename basename
- size in KB
- kind badge: `REC` or `BP`

Clicking a row:
- sets `s_files_selected_index`
- visually highlights the row
- updates status label: `Selected: <name>`

### 5.5 UI timer

Add `files_ui_timer_cb()` running every 250–500 ms.

Responsibilities:
- Poll `svc_files_get_tx_status()`.
- Update transfer progress label:
  - `Sending <file>: 12.3 KB / 83.4 KB`
  - `Upload complete`
  - `Upload failed: timeout`
- Reflect Wi-Fi state using `s_wifi_connected`.
- Disable/enable buttons based on connection and busy state.

---

## 6. Reuse of existing Wi-Fi flow

Do **not** create a new Wi-Fi stack.

Use the project’s existing station-mode system in `main.c`:
- existing AP scan list
- password screen
- connect task
- `s_wifi_connected`
- event-group-based connection lifecycle

Recommended Stage 1 behaviour:
- User enters the Files screen.
- If not connected, they press **Connect WiFi** and use the existing hotspot connection flow.
- After the watch connects to the PC hotspot, return to Files screen and allow Send.

Optional refinement:
- If Wi-Fi credentials for the PC hotspot are already stored in NVS, the Files screen may later add a “Reconnect last network” shortcut, but do not make that part of the first implementation unless very easy.

---

## 7. PC hotspot assumptions for Stage 1

The PC acts as:
1. Wi-Fi hotspot / AP
2. HTTP receiver

Assumptions for Stage 1:
- PC hotspot provides DHCP.
- Watch receives an IP via existing station flow.
- PC gateway IP is stable and manually configured in the watch firmware (for example `192.168.137.1` on Windows mobile hotspot, but do not hardcode this assumption in comments as universally true — expose it as a constant).

Plan the watch code so the server host can later become:
- user-configurable,
- NVS-stored,
- or auto-discovered.

But for now, keep it simple: single compile-time constant.

---

## 8. Recommended file naming assumptions on PC

The receiver should preserve the watch filename exactly.

Examples:
- `20260511_063015_123.csv` → Record session
- `20260511_071030_456_bp.csv` → BP session

Do not rename files on the watch.
Do not package into ZIP in Stage 1.
Do not transform CSV content in transit.

---

## 9. Error handling requirements

Claude should implement at least these cases:

- SD not mounted / mount failed
- No files found
- No Wi-Fi connection
- File open failed
- HTTP connect failed
- Partial write / short write
- Server returned non-200
- Timeout
- Transfer already active

On-watch messages should be short and LVGL-friendly, for example:
- `SD mount failed`
- `No files on card`
- `WiFi not connected`
- `Sending...`
- `Upload complete`
- `Upload failed: server`

---

## 10. Minimal implementation order

1. Create `svc_files.h/.c`
2. Implement SD enumeration only
3. Implement `ui_create_files_screen()` with list + selection
4. Wire Files screen into `ui_create_app_screens()`
5. Add HTTP upload function in service
6. Add background upload task
7. Add progress polling to Files screen timer
8. Test with one small CSV file
9. Test with larger Record/BP CSV files

---

## 11. Constraints for Claude

- Do not rewrite the existing Wi-Fi architecture.
- Do not replace the current Record or BP services.
- Do not add TLS.
- Do not add file deletion yet.
- Do not add multi-select yet.
- Do not add server auto-discovery yet.
- Keep the implementation compatible with the existing coding style: static globals in `main.c`, service modules in `main/services`, LVGL object pointers as file-scope statics.

---

## 12. Prompt-ready implementation request for Claude

Use this as the implementation brief:

```text
Implement a Files screen in tulog-biowatch.

Read this plan first and then inspect the current repo state before coding.

Goal:
- create a real Files screen for the watch
- enumerate files from the SD card
- allow selecting a file
- reuse the existing Wi-Fi station connection flow
- send the selected file from the watch to a Python receiver running on a PC hotspot using HTTP POST
- show on-watch transfer status and progress

Create:
- main/services/svc_files.h
- main/services/svc_files.c

Modify:
- main/app_state.h
- main/app_config.h
- main/CMakeLists.txt
- main/main.c

Implementation requirements:
- use hal_storage_mount()/hal_storage_is_mounted()
- enumerate files with opendir()/readdir()
- cap displayed files at 64 entries
- classify `_bp.csv` as BP and `.csv` as Record
- use esp_http_client for upload
- send raw file bytes in the POST body to /upload
- add headers X-Filename, X-File-Size, X-Session-Type
- run upload in a background task, not in an LVGL event callback
- add a Files screen with Refresh, Connect WiFi, Send buttons
- add a scrollable file list and selection highlight
- add a timer to poll upload status and update labels
- do not change existing Wi-Fi connection architecture
- do not add TLS, deletion, multi-select, or auto-discovery yet
```

