#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "driver/spi_master.h"
#include "esp_err.h"

// microSD card (FAT, mounted at /sdcard): measurement files
// /sdcard/meas_NNNNN.jsonl, one per saved measurement. Files are only ever
// created with the next free number - existing files are never opened for
// writing, and the card is never formatted automatically.
//
// The card shares the SPI bus with the ADS1293, and writing to it while the
// ADS1293 is converting makes the card reset itself mid-write. So nothing is
// written here during a measurement: offline_log.c buffers the data in
// flash and copies it over once the ADS1293 is back in standby.
//
// Call from one task only (main.c's sender loop), after spi_bus_initialize().

// Mounts the card; on failure logging is just unavailable for now -
// sd_logger_file_begin() retries the mount.
esp_err_t sd_logger_init(spi_host_device_t host, int cs_gpio);

bool sd_logger_is_mounted(void);

// Unmounts and re-initialises the card. The card stops answering after the
// ADS1293 has been running on the shared bus (even untouched, e.g. through
// a 10-minute ECG) and only a fresh init brings it back, so this runs
// before every batch of card work. True if the card is usable afterwards.
bool sd_logger_reinit(void);

// Creates the next meas_NNNNN.jsonl. False if there's no usable card.
bool sd_logger_file_begin(void);

// Appends raw bytes to the open file. False (and the file is dropped) on a
// write error.
bool sd_logger_file_write(const void *data, size_t len);

// Closes the file, making it complete on the card. False on error.
bool sd_logger_file_end(void);

// Number of non-empty measurement files on the card waiting to be sent to
// the server; -1 while no card is mounted.
int sd_logger_pending_files(void);

// --- uploading (web/server.js /api/sd-upload, see sd_upload.c) ---

// Lowest-numbered measurement file: its number and size. False if none.
bool sd_logger_oldest_file(int *index, long *size);
// Opens measurement file `index` for reading (NULL on error).
FILE *sd_logger_open_read(int index);
// Deletes measurement file `index` (after the server confirmed it).
bool sd_logger_delete(int index);
// "meas_00012.jsonl"
void sd_logger_file_name(int index, char *out, size_t cap);
