#include "telemetry_http.h"

#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "telemetry_http";

esp_err_t telemetry_http_post(const char *json_body, size_t len)
{
    esp_http_client_config_t config = {
        .url = CONFIG_MIOT_SERVER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (strlen(CONFIG_MIOT_API_KEY) > 0) {
        esp_http_client_set_header(client, "X-API-Key", CONFIG_MIOT_API_KEY);
    }
    esp_http_client_set_post_field(client, json_body, len);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "server returned HTTP %d", status);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "POST failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}
