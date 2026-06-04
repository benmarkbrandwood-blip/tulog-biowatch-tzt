#pragma once

#include <stdbool.h>

void svc_time_sync(void);
bool svc_time_is_synced(void);
void svc_time_restore_from_nvs(void);
