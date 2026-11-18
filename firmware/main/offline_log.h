#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Offline buffer for telemetry that couldn't be delivered to the server
// (no Wi-Fi, or the server didn't answer). Lines are appended to the
// "measbuf" flash partition while a measurement runs - not to the SD card,
// which can't be written while the ADS1293 converts - and are copied to
// the SD card (one meas_NNNNN.jsonl per copy) once the measurement is over.
//
// Flash keeps the data across a power loss: whatever is still in the
// buffer at boot is copied on the first chance. Call everything from one
// task (main.c's sender loop).

esp_err_t offline_log_init(void);

// Marks the start of a measurement: the next appended line is preceded by a
// header line ({"device_id":..,"boot":..,"uptime_s":..[,"epoch":..]}) so
// sessions stay apart and the server can date them ("epoch" only once the
// clock has been synced over the internet).
void offline_log_session_start(uint32_t boot_id);

// Appends one telemetry JSON line (newline added).
void offline_log_append(const char *line, size_t len);

bool offline_log_has_data(void);

// Copies the buffer to a new file on the SD card and empties it. Returns
// true when the buffer is empty afterwards (copied, or nothing to copy).
// Takes a few seconds for a full 10-minute ECG; call only while no
// measurement is running.
bool offline_log_copy_to_sd(void);
