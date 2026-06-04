#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t hal_storage_mount(void);
bool      hal_storage_is_mounted(void);
