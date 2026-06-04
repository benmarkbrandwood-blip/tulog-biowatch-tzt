#include "svc_files.h"
#include "app_config.h"
#include "hal_storage.h"

#include "esp_http_client.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp-bsp.h"   /* BSP_SD_MOUNT_POINT */

#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

static const char *TAG = "svc_files";

/* Candidate PC hotspot gateways tried in order.
 * Linux Network Manager hotspot: 10.42.0.1
 * Windows Mobile Hotspot:        192.168.137.1 */
static const char *s_server_hosts[] = { "10.42.0.1", "192.168.137.1" };
#define S_SERVER_HOST_COUNT (sizeof(s_server_hosts) / sizeof(s_server_hosts[0]))

static file_tx_status_t    s_tx_status  = {0};
static watch_file_entry_t  s_send_entry = {0};

/* -------------------------------------------------------------------------- */
/* Internal helpers                                                           */
/* -------------------------------------------------------------------------- */

static file_kind_t classify_file(const char *name)
{
    size_t n = strlen(name);
    if (n >= 7 && strcmp(name + n - 7, "_bp.csv") == 0)
        return FILE_KIND_BP;
    if (n >= 4 && strcmp(name + n - 4, ".csv") == 0)
        return FILE_KIND_RECORD;
    return FILE_KIND_UNKNOWN;
}

static int file_cmp_desc(const void *a, const void *b)
{
    /* Timestamp-prefixed filenames sort newest-first in descending order. */
    return strcmp(((const watch_file_entry_t *)b)->name,
                  ((const watch_file_entry_t *)a)->name);
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t svc_files_refresh_list(watch_file_list_t *out)
{
    memset(out, 0, sizeof(*out));

    if (hal_storage_mount() != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed — cannot list files");
        return ESP_FAIL;
    }
    out->sd_ok = true;

    DIR *dir = opendir(BSP_SD_MOUNT_POINT);
    if (!dir) {
        ESP_LOGW(TAG, "opendir failed on %s", BSP_SD_MOUNT_POINT);
        return ESP_FAIL;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL && out->count < FILES_MAX_LIST_COUNT) {
        if (de->d_type == DT_DIR) continue;
        if (de->d_name[0] == '.') continue;

        watch_file_entry_t *fe = &out->items[out->count];
        strlcpy(fe->name, de->d_name, sizeof(fe->name));
        /* Suppress format-truncation: d_name on FAT is ≤ 255 chars but the
         * compiler treats it as unbounded.  In practice our filenames are
         * ~25 chars and the 128-byte path buffer is always sufficient. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(fe->path, sizeof(fe->path), "%s/%s", BSP_SD_MOUNT_POINT, de->d_name);
#pragma GCC diagnostic pop
        fe->kind = classify_file(de->d_name);

        struct stat st;
        if (stat(fe->path, &st) == 0)
            fe->size_bytes = (uint32_t)st.st_size;

        out->count++;
    }
    closedir(dir);

    qsort(out->items, out->count, sizeof(watch_file_entry_t), file_cmp_desc);
    ESP_LOGI(TAG, "listed %u file(s)", (unsigned)out->count);
    return ESP_OK;
}

esp_err_t svc_files_delete_file(const watch_file_entry_t *entry)
{
    if (!entry) return ESP_ERR_INVALID_ARG;
    if (remove(entry->path) != 0) {
        ESP_LOGW(TAG, "delete failed: %s", entry->path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "deleted: %s", entry->name);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Upload                                                                     */
/* -------------------------------------------------------------------------- */

static esp_err_t send_file(const watch_file_entry_t *entry, file_tx_status_t *status)
{
    FILE *f = fopen(entry->path, "rb");
    if (!f) {
        snprintf(status->message, sizeof(status->message), "File open failed");
        return ESP_FAIL;
    }

    const char *kind_str =
        (entry->kind == FILE_KIND_BP)     ? "bp"     :
        (entry->kind == FILE_KIND_RECORD) ? "record" : "unknown";

    char size_str[24];
    snprintf(size_str, sizeof(size_str), "%lu", (unsigned long)entry->size_bytes);

    /* Try each candidate host in order; move on only if the TCP connect
     * fails (before any file data has been read). */
    esp_http_client_handle_t client = NULL;
    for (size_t hi = 0; hi < S_SERVER_HOST_COUNT; hi++) {
        char url[160];
        snprintf(url, sizeof(url), "http://%s:%d%s",
                 s_server_hosts[hi], FILES_SERVER_PORT, FILES_SERVER_PATH);

        esp_http_client_config_t cfg = {
            .url        = url,
            .method     = HTTP_METHOD_POST,
            .timeout_ms = FILES_HTTP_TIMEOUT_MS,
        };

        client = esp_http_client_init(&cfg);
        if (!client) {
            fclose(f);
            snprintf(status->message, sizeof(status->message), "HTTP init failed");
            return ESP_FAIL;
        }

        esp_http_client_set_header(client, "Content-Type",    "application/octet-stream");
        esp_http_client_set_header(client, "X-Filename",      entry->name);
        esp_http_client_set_header(client, "X-File-Size",     size_str);
        esp_http_client_set_header(client, "X-Session-Type",  kind_str);

        esp_err_t err = esp_http_client_open(client, (int)entry->size_bytes);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "connected to %s", s_server_hosts[hi]);
            break;
        }

        ESP_LOGW(TAG, "connect to %s failed (%s), trying next",
                 s_server_hosts[hi], esp_err_to_name(err));
        esp_http_client_cleanup(client);
        client = NULL;

        if (hi == S_SERVER_HOST_COUNT - 1) {
            fclose(f);
            snprintf(status->message, sizeof(status->message), "Connect failed");
            return ESP_FAIL;
        }
    }

    static uint8_t s_chunk[FILES_TX_CHUNK_BYTES];
    size_t n;
    bool write_ok = true;
    while ((n = fread(s_chunk, 1, sizeof(s_chunk), f)) > 0) {
        int written = esp_http_client_write(client, (const char *)s_chunk, (int)n);
        if (written < 0) {
            write_ok = false;
            break;
        }
        status->bytes_sent += (uint32_t)written;
    }
    fclose(f);

    if (!write_ok) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        snprintf(status->message, sizeof(status->message), "Write failed");
        return ESP_FAIL;
    }

    esp_http_client_fetch_headers(client);
    int code = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (code != 200) {
        snprintf(status->message, sizeof(status->message), "Server error %d", code);
        return ESP_FAIL;
    }

    snprintf(status->message, sizeof(status->message), "Upload complete");
    return ESP_OK;
}

static void files_upload_task(void *arg)
{
    (void)arg;
    esp_err_t err = send_file(&s_send_entry, &s_tx_status);
    s_tx_status.success = (err == ESP_OK);
    s_tx_status.active  = false;
    s_tx_status.done    = true;
    ESP_LOGI(TAG, "upload %s: %s",
             s_tx_status.success ? "OK" : "FAIL", s_tx_status.message);
    vTaskDelete(NULL);
}

void svc_files_start_send_task(const watch_file_entry_t *entry)
{
    if (s_tx_status.active) return;

    memcpy(&s_send_entry, entry, sizeof(s_send_entry));

    memset(&s_tx_status, 0, sizeof(s_tx_status));
    s_tx_status.active      = true;
    s_tx_status.bytes_total = entry->size_bytes;
    strlcpy(s_tx_status.filename, entry->name, sizeof(s_tx_status.filename));
    snprintf(s_tx_status.message, sizeof(s_tx_status.message), "Sending...");

    xTaskCreate(files_upload_task, "files_upload",
                FILES_UPLOAD_STACK_BYTES, NULL, 4, NULL);
}

const file_tx_status_t *svc_files_get_tx_status(void)
{
    return &s_tx_status;
}

bool svc_files_is_busy(void)
{
    return s_tx_status.active;
}
