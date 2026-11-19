#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Buffer for telemetry that didn't get out (no Wi-Fi / Azure). Lines go to
// the "measbuf" flash partition while measuring (the SD card can't be used
// while the ADS1293 runs) and are copied to the card afterwards.
// Survives a power loss. Call from one task (sender loop).

esp_err_t offline_log_init(void);

// Starts a new session: the next line gets a header with device_id, boot,
// uptime and epoch (if the clock is synced).
void offline_log_session_start(uint32_t boot_id);

// Appends one JSON line.
void offline_log_append(const char *line, size_t len);

bool offline_log_has_data(void);

// Copies the buffer to a new SD file and clears it. True if it ends up
// empty. Call only when not measuring.
bool offline_log_copy_to_sd(void);
