#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "device_commands.h"
#include "esp_err.h"

// Azure IoT Hub telemetry over MQTT (mqtts://<hub>.azure-devices.net:8883).
//
// Only active when CONFIG_MIOT_AZURE_ENABLE is set; otherwise both calls
// are no-ops returning ESP_OK, so callers don't need to #ifdef around them.
//
// Auth is a SAS token generated on the device from the device connection
// string (CONFIG_MIOT_AZURE_CONNECTION_STRING, the one `az iot hub
// device-identity connection-string show` / the portal's "Primary
// connection string" gives). The token embeds an expiry timestamp, so the
// module first syncs wall-clock time over SNTP, and reconnects with a fresh
// token shortly before the old one expires. TLS is verified against the
// ESP-IDF certificate bundle (contains DigiCert Global Root G2, which Azure
// IoT Hub uses).
//
// Call once from app_main after wifi_connect_start().
esp_err_t telemetry_azure_init(void);

// Publishes the same JSON body used for the HTTP POST to
// devices/<deviceId>/messages/events/ (QoS 0). It reaches the dashboard via
// the hub's built-in Event Hub-compatible endpoint (web/src/azure-ingest.js).
//
// `idle` = no measurement running. Idle status messages are throttled to
// one per CONFIG_MIOT_AZURE_IDLE_PERIOD_S, because the free F1 tier allows
// only 8000 messages/day and the main loop ticks once a second.
// Best-effort: silently dropped while not connected.
esp_err_t telemetry_azure_publish(const char *json_body, size_t len, bool idle);

// Dashboard commands over Azure IoT Hub direct methods: the server calls
// method "command" with payload {"commands":["all"]} (same format as the
// /api/data response). The handler runs on the MQTT client's task, must be
// quick, and decides right away: it returns the status sent back to the
// server (200 = accepted, 4xx = refused) and may write a short reason.
typedef int (*azure_command_handler_t)(measure_cmd_t cmds, char *reason, size_t reason_cap);
void telemetry_azure_set_command_handler(azure_command_handler_t handler);
