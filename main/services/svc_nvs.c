#include "svc_nvs.h"
#include "app_config.h"

#include <string.h>
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "NVS";

static void ssid_to_key(const char *ssid, char *out, size_t out_len)
{
    size_t j = 0;
    if (!out || out_len == 0) {
        return;
    }

    for (size_t i = 0; ssid && ssid[i] != '\0' && j < out_len - 1; i++) {
        char c = ssid[i];
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
}

bool nvs_load_password_for_ssid(const char *ssid, char *out_pass, size_t out_len)
{
    if (!ssid || !ssid[0] || !out_pass || out_len == 0) {
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CRED_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    char key[32];
    ssid_to_key(ssid, key, sizeof(key));

    size_t required = out_len;
    err = nvs_get_str(handle, key, out_pass, &required);
    nvs_close(handle);

    if (err == ESP_OK) {

        return true;
    }

    out_pass[0] = '\0';
    return false;
}

void nvs_save_password_for_ssid(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || !pass) {
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CRED_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", WIFI_CRED_NAMESPACE, esp_err_to_name(err));
        return;
    }

    char key[32];
    ssid_to_key(ssid, key, sizeof(key));

    err = nvs_set_str(handle, key, pass);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving password failed for SSID '%s': %s", ssid, esp_err_to_name(err));
    } else {

    }

    nvs_close(handle);
}

void nvs_save_last_ssid(const char *ssid)
{
    if (!ssid || !ssid[0]) {
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_LAST_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", WIFI_LAST_NAMESPACE, esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(handle, WIFI_LAST_SSID_KEY, ssid);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving last SSID failed: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
}
