#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "device_commands.h"
#include "esp_err.h"

// POSTs a raw JSON body to CONFIG_MIOT_SERVER_URL (web/server.js /api/data)
// over a reused keep-alive connection. Adds the X-API-Key header when
// CONFIG_MIOT_API_KEY is set. Any dashboard button presses queued for this
// device come back in the same response and are returned via *out_cmds
// (may be NULL). Returns ESP_OK on a 2xx response.
esp_err_t telemetry_http_post(const char *json_body, size_t len, measure_cmd_t *out_cmds);

// Sends one piece of an SD-card measurement file to the server
// (web/server.js POST /api/sd-upload). Pieces go in order (part 0, 1, ...);
// `last` marks the final one, after which the server turns the whole file
// into saved history records. boot_id/uptime_s let the server work out when
// the measurement happened if the file carries no wall-clock time.
// Returns ESP_OK only once the server confirmed the piece.
esp_err_t telemetry_http_upload_part(const char *file_name, int part, bool last, uint32_t boot_id,
                                     long long uptime_s, const char *data, size_t len);
