#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef void (*hal_display_locked_fn_t)(void *ctx);

bool hal_display_lock(uint32_t total_timeout_ms,
                      const char *who,
                      hal_display_locked_fn_t fn,
                      void *ctx);
