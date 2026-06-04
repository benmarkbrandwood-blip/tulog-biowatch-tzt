#pragma once

#include "esp_err.h"
#include "app_state.h"

/* -------------------------------------------------------------------------- */
/* Files service — SD enumeration, HTTP POST upload, file deletion            */
/* -------------------------------------------------------------------------- */

/* Fill *out with the list of files on the SD card.
 * Sorts newest-first (filenames are timestamp-prefixed).
 * Calls hal_storage_mount() if the card is not already mounted.
 * Returns ESP_OK on success; out->sd_ok reflects mount status. */
esp_err_t svc_files_refresh_list(watch_file_list_t *out);

/* Delete the file at entry->path from the SD card. */
esp_err_t svc_files_delete_file(const watch_file_entry_t *entry);

/* Start a background FreeRTOS task that uploads entry to the PC receiver.
 * No-op if a transfer is already active (svc_files_is_busy() returns true).
 * Poll svc_files_get_tx_status() to track progress. */
void svc_files_start_send_task(const watch_file_entry_t *entry);

/* Pointer to the current transfer status (valid for the lifetime of the app). */
const file_tx_status_t *svc_files_get_tx_status(void);

/* True while a background upload is running. */
bool svc_files_is_busy(void);
