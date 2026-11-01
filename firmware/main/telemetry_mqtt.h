#pragma once

#include <stddef.h>

#include "esp_err.h"

// Starts the MQTT client (only if CONFIG_MIOT_MQTT_ENABLE is set) and
// connects to CONFIG_MIOT_MQTT_URL in the background. No-op (returns
// ESP_OK) when MQTT telemetry is disabled in menuconfig - callers don't
// need to #ifdef around it. Call once from app_main, after Wi-Fi has been
// told to connect (needs the default netif/event loop that sets up).
esp_err_t telemetry_mqtt_init(void);

// Publishes the same JSON body used for the HTTP POST (see
// telemetry_http.h) to wearable/<CONFIG_MIOT_DEVICE_ID>/data, QoS 0, no
// retain - matches the topic web/src/mqtt-ingest.js subscribes to
// (wearable/+/data) and the payload shape web/src/parser.js expects.
// No-op (returns ESP_OK) if MQTT telemetry is disabled or the client
// hasn't connected yet - treat it as best-effort, same as an offline HTTP
// POST.
esp_err_t telemetry_mqtt_publish(const char *json_body, size_t len);
