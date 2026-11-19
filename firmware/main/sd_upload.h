#pragma once

#include <stdbool.h>
#include <stdint.h>

// "Завантажити дані з SD": sends all measurement files to the server
// (POST /api/sd-upload), oldest first, deleting each one after the server
// confirms it. Goes over plain HTTP, a whole file is too much for Azure F1.
// Blocking, call from the sender task when not measuring.
// True if no files are left.
bool sd_upload_all(uint32_t boot_id);
