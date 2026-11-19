#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "device_commands.h"
#include "esp_err.h"

// Azure IoT Hub over MQTT (port 8883), the only telemetry and command
// channel. Off unless CONFIG_MIOT_AZURE_ENABLE (then publish returns
// ESP_ERR_NOT_SUPPORTED).
// Auth: SAS token made from the device connection string, needs SNTP time,
// renewed before it expires. TLS checked with the ESP-IDF cert bundle.
// Call once after wifi_connect_start().
esp_err_t telemetry_azure_init(void);

// Sends telemetry (QoS 0). The server reads it from the hub's Event Hub
// endpoint. While idle only one message per MIOT_AZURE_IDLE_PERIOD_S goes
// out (F1 = 8000 messages/day).
// ESP_OK if sent or skipped, ESP_ERR_INVALID_STATE if not connected.
esp_err_t telemetry_azure_publish(const char *json_body, size_t len, bool idle);

// Commands as direct method "command" ({"commands":["all"]}). The handler
// runs on the MQTT task, must be quick, and returns the status for the
// server (200 ok, 4xx refused) plus an optional reason.
typedef int (*azure_command_handler_t)(measure_cmd_t cmds, char *reason, size_t reason_cap);
void telemetry_azure_set_command_handler(azure_command_handler_t handler);
