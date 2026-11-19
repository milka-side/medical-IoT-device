#include "telemetry_http.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "telemetry_http";

// Server's reply to an upload piece: small ({"ok":true,...}).
static char s_resp[256];
static size_t s_resp_len;

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        size_t room = sizeof(s_resp) - 1 - s_resp_len;
        size_t n = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
        memcpy(s_resp + s_resp_len, evt->data, n);
        s_resp_len += n;
        s_resp[s_resp_len] = '\0';
    }
    return ESP_OK;
}

// A short-lived client per piece: uploads only run while nothing is being
// measured, a few times a day at most.
esp_err_t telemetry_http_upload_part(const char *file_name, int part, bool last, uint32_t boot_id,
                                     long long uptime_s, const char *data, size_t len)
{
    // CONFIG_MIOT_SERVER_URL is the laptop's server ("http://host:port", any
    // "/api/..." suffix is dropped) -> "http://host:port/api/sd-upload?...".
    const char *base = CONFIG_MIOT_SERVER_URL;
    const char *api = strstr(base, "/api/");
    int base_len = api ? (int)(api - base) : (int)strlen(base);
    char url[256];
    snprintf(url, sizeof(url), "%.*s/api/sd-upload?device_id=%s&file=%s&part=%d&last=%d&boot=%lu&uptime=%lld",
             base_len, base, CONFIG_MIOT_DEVICE_ID, file_name, part, last ? 1 : 0, (unsigned long)boot_id,
             uptime_s);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .event_handler = on_http_event,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "text/plain");
    if (strlen(CONFIG_MIOT_API_KEY) > 0) {
        esp_http_client_set_header(client, "X-API-Key", CONFIG_MIOT_API_KEY);
    }
    s_resp_len = 0;
    s_resp[0] = '\0';
    esp_http_client_set_post_field(client, data, (int)len);
    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD upload %s part %d failed: %s", file_name, part, esp_err_to_name(err));
        return err;
    }
    if (status != 200 || strstr(s_resp, "\"ok\":true") == NULL) {
        ESP_LOGW(TAG, "SD upload %s part %d rejected: HTTP %d %s", file_name, part, status, s_resp);
        return ESP_FAIL;
    }
    return ESP_OK;
}
