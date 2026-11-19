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

// Publishes a telemetry JSON body to devices/<deviceId>/messages/events/.
// It reaches the dashboard via the hub's built-in Event Hub-compatible
// endpoint (web/src/azure-ingest.js).
//
// `reliable` = measurement data: sent with QoS 1, the hub acknowledges it,
// and *out_msg_id gets the id to ask telemetry_azure_msg_state() about.
// Otherwise (status messages) QoS 0 and *out_msg_id = 0. *out_msg_id is
// also 0 when the message was skipped by the throttle below.
//
// `idle` = nothing in this message the dashboard doesn't already have (no
// ECG samples, no result, no state change). Such messages go out at most
// once per CONFIG_MIOT_AZURE_IDLE_PERIOD_S, because the free F1 tier allows
// only 8000 messages/day and the main loop ticks once a second.
// Returns ESP_ERR_INVALID_STATE while not connected to the hub (and
// ESP_ERR_NOT_SUPPORTED when Azure is disabled), so the caller can keep
// measurement data for the SD card instead.
esp_err_t telemetry_azure_publish(const char *json_body, size_t len, bool idle, bool reliable, int *out_msg_id);

// Fate of a QoS 1 message: still waiting for the hub's acknowledgement,
// acknowledged, or given up by the MQTT client (not acknowledged within
// CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS, retries included) - only then is it
// safe to keep a copy for the SD card without risking a duplicate.
typedef enum {
    AZURE_MSG_PENDING,
    AZURE_MSG_DELIVERED,
    AZURE_MSG_LOST,
} azure_msg_state_t;
azure_msg_state_t telemetry_azure_msg_state(int msg_id);

// Dashboard commands over Azure IoT Hub direct methods: the server calls
// method "command" with payload {"commands":["all"]} (same format as the
// /api/data response). The handler runs on the MQTT client's task, must be
// quick, and decides right away: it returns the status sent back to the
// server (200 = accepted, 4xx = refused) and may write a short reason.
typedef int (*azure_command_handler_t)(measure_cmd_t cmds, char *reason, size_t reason_cap);
void telemetry_azure_set_command_handler(azure_command_handler_t handler);
