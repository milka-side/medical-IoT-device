#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// HTTP is used only for "Завантажити дані з SD": live telemetry and
// dashboard commands go through Azure IoT Hub (telemetry_azure.h).

// Sends one piece of an SD-card measurement file to the server
// (web/server.js POST /api/sd-upload). Pieces go in order (part 0, 1, ...);
// `last` marks the final one, after which the server turns the whole file
// into saved history records. boot_id/uptime_s let the server work out when
// the measurement happened if the file carries no wall-clock time.
// Returns ESP_OK only once the server confirmed the piece.
esp_err_t telemetry_http_upload_part(const char *file_name, int part, bool last, uint32_t boot_id,
                                     long long uptime_s, const char *data, size_t len);
