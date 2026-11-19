#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "driver/spi_master.h"
#include "esp_err.h"

// microSD (FAT, /sdcard): one meas_NNNNN.jsonl per measurement. New files
// get the next free number, old files are never written, no auto-format.
// Nothing is written while the ADS1293 runs (the card resets itself), the
// data waits in flash (offline_log.c). Call from one task (sender loop).

// Mounts the card. On failure sd_logger_file_begin() tries again.
esp_err_t sd_logger_init(spi_host_device_t host, int cs_gpio);

bool sd_logger_is_mounted(void);

// Remounts the card. It stops answering after the ADS1293 has used the
// bus, so run this before any card work. True if the card works.
bool sd_logger_reinit(void);

// Creates the next meas_NNNNN.jsonl. False if there's no card.
bool sd_logger_file_begin(void);

// Appends bytes to the open file. False on error (file dropped).
bool sd_logger_file_write(const void *data, size_t len);

// Closes the file. False on error.
bool sd_logger_file_end(void);

// Non-empty files waiting for upload, -1 if no card.
int sd_logger_pending_files(void);

// --- upload (see sd_upload.c) ---

// Oldest file: number and size. False if none.
bool sd_logger_oldest_file(int *index, long *size);
// Opens file `index` for reading, NULL on error.
FILE *sd_logger_open_read(int index);
// Deletes file `index` (after the server confirmed it).
bool sd_logger_delete(int index);
// "meas_00012.jsonl"
void sd_logger_file_name(int index, char *out, size_t cap);
