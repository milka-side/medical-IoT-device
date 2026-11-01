#include "telemetry_mqtt.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "telemetry_mqtt";

#ifdef CONFIG_MIOT_MQTT_ENABLE

#include "mqtt_client.h"

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_topic[96];

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "connected to %s, publishing on %s", CONFIG_MIOT_MQTT_URL, s_topic);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected");
        break;
    case MQTT_EVENT_ERROR: {
        esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "transport error: %s", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    }
    default:
        break;
    }
}

esp_err_t telemetry_mqtt_init(void)
{
    snprintf(s_topic, sizeof(s_topic), "wearable/%s/data", CONFIG_MIOT_DEVICE_ID);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_MIOT_MQTT_URL,
    };
    if (strlen(CONFIG_MIOT_MQTT_USERNAME) > 0) {
        cfg.credentials.username = CONFIG_MIOT_MQTT_USERNAME;
    }
    if (strlen(CONFIG_MIOT_MQTT_PASSWORD) > 0) {
        cfg.credentials.authentication.password = CONFIG_MIOT_MQTT_PASSWORD;
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t telemetry_mqtt_publish(const char *json_body, size_t len)
{
    if (!s_connected) {
        // Broker not reachable yet (or Wi-Fi down) - same best-effort
        // policy as the HTTP path, data is just dropped for this batch.
        return ESP_OK;
    }
    int msg_id = esp_mqtt_client_publish(s_client, s_topic, json_body, (int)len, /*qos=*/0, /*retain=*/0);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

#else // !CONFIG_MIOT_MQTT_ENABLE

esp_err_t telemetry_mqtt_init(void)
{
    ESP_LOGI(TAG, "MQTT telemetry disabled (enable via menuconfig -> Medical IoT Device)");
    return ESP_OK;
}

esp_err_t telemetry_mqtt_publish(const char *json_body, size_t len)
{
    (void)json_body;
    (void)len;
    return ESP_OK;
}

#endif
