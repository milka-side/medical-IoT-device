#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Sends a raw JSON body with POST to CONFIG_MIOT_SERVER_URL (web/server.js
// /api/data). Adds the X-API-Key header when CONFIG_MIOT_API_KEY is set.
// Returns ESP_OK on a 2xx response.
esp_err_t telemetry_http_post(const char *json_body, size_t len);
