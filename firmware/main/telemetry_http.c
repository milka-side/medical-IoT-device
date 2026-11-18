#include "telemetry_http.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "telemetry_http";

// One client reused for every post, so the TCP connection stays open between
// the once-a-second posts (HTTP keep-alive) instead of a fresh TCP handshake
// - and the radio time that costs - every single second.
static esp_http_client_handle_t s_client;

// Response body: small ({"accepted":..,"commands":[..]}), anything past the
// buffer is dropped - "commands" comes early in the JSON.
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

static esp_http_client_handle_t get_client(void)
{
    if (s_client != NULL) {
        return s_client;
    }
    esp_http_client_config_t config = {
        .url = CONFIG_MIOT_SERVER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .event_handler = on_http_event,
    };
    s_client = esp_http_client_init(&config);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return NULL;
    }
    esp_http_client_set_header(s_client, "Content-Type", "application/json");
    if (strlen(CONFIG_MIOT_API_KEY) > 0) {
        esp_http_client_set_header(s_client, "X-API-Key", CONFIG_MIOT_API_KEY);
    }
    return s_client;
}

static void drop_client(void)
{
    if (s_client != NULL) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}

esp_err_t telemetry_http_post(const char *json_body, size_t len, measure_cmd_t *out_cmds)
{
    if (out_cmds) {
        *out_cmds = MEASURE_NONE;
    }

    esp_err_t err = ESP_FAIL;
    // Second attempt only if the first failed at the transport level: the
    // server closes idle keep-alive connections, and the reused socket then
    // fails once - a fresh connection is the fix, not losing the batch.
    for (int attempt = 0; attempt < 2; attempt++) {
        bool reused = s_client != NULL;
        esp_http_client_handle_t client = get_client();
        if (client == NULL) {
            return ESP_FAIL;
        }
        s_resp_len = 0;
        s_resp[0] = '\0';
        esp_http_client_set_post_field(client, json_body, (int)len);
        err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "POST failed: %s", esp_err_to_name(err));
        drop_client();
        if (!reused) {
            break; // fresh connection failed too - server/network is down, don't double the wait
        }
    }
    if (err != ESP_OK) {
        return err;
    }

    // Commands ride on the response whether or not this batch was accepted -
    // the server has already dequeued them by the time it replies.
    if (out_cmds) {
        *out_cmds = device_commands_parse(s_resp);
    }
    int status = esp_http_client_get_status_code(s_client);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "server returned HTTP %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Separate short-lived client for SD uploads: different URL, larger bodies,
// and it only runs while nothing is being measured.
esp_err_t telemetry_http_upload_part(const char *file_name, int part, bool last, uint32_t boot_id,
                                     long long uptime_s, const char *data, size_t len)
{
    // Same server as the telemetry URL, with the path swapped for the
    // upload endpoint: "http://host:port/api/data" -> ".../api/sd-upload?...".
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
