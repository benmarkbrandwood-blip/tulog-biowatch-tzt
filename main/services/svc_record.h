#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_state.h"

esp_err_t svc_rec_init(void);

/* Returns ESP_ERR_NO_MEM (queue), ESP_FAIL (file), ESP_ERR_INVALID_STATE (task) */
esp_err_t svc_rec_start(const char *label);

void      svc_rec_stop(void);
void      svc_rec_enqueue(const rec_row_t *row);
bool      svc_rec_is_recording(void);
uint32_t  svc_rec_get_start_ms(void);
