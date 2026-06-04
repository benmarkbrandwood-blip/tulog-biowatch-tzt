#pragma once

#include <stddef.h>
#include <stdbool.h>

bool nvs_load_password_for_ssid(const char *ssid, char *out_pass, size_t out_len);
void nvs_save_password_for_ssid(const char *ssid, const char *pass);
void nvs_save_last_ssid(const char *ssid);
