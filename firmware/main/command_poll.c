#include "command_poll.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "command_poll";

measure_cmd_t command_poll_tick(void)
{
    char url[192];
    snprintf(url, sizeof(url), "%s?device_id=%s", CONFIG_MIOT_COMMAND_URL, CONFIG_MIOT_DEVICE_ID);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGW(TAG, "esp_http_client_init failed");
        return MEASURE_NONE;
    }
    if (strlen(CONFIG_MIOT_API_KEY) > 0) {
        esp_http_client_set_header(client, "X-API-Key", CONFIG_MIOT_API_KEY);
    }

    measure_cmd_t result = MEASURE_NONE;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        // Don't gate the read on the returned content length: it's 0 for a
        // chunked response (Node's default when no Content-Length is set),
        // and gating on it meant the body was never read - the server had
        // already dequeued the command, so the button press was silently lost.
        esp_http_client_fetch_headers(client);
        char buf[128] = {0};
        int read_len = esp_http_client_read_response(client, buf, sizeof(buf) - 1);
        if (esp_http_client_get_status_code(client) == 200 && read_len > 0) {
            // Response is always the flat shape {"type":"ecg"|"vitals"|"temp"|"all"|null} -
            // a hand-rolled substring check is plenty, no need for a JSON library.
            if (strstr(buf, "\"ecg\"")) {
                result = MEASURE_ECG;
            } else if (strstr(buf, "\"vitals\"")) {
                result = MEASURE_VITALS;
            } else if (strstr(buf, "\"temp\"")) {
                result = MEASURE_TEMP;
            } else if (strstr(buf, "\"all\"")) {
                result = MEASURE_ALL;
            }
        }
    } else {
        ESP_LOGW(TAG, "command poll failed: %s", esp_err_to_name(err));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}
