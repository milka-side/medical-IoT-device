#include "sd_upload.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sd_logger.h"

static const char *TAG = "sd_upload";

// 8 KB per request, a 10-minute ECG is ~60 requests.
#define PART_SIZE 8192

// So a file that can't be deleted isn't sent forever.
#define MAX_FILES_PER_RUN 200

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

// Sends one part to POST /api/sd-upload. Parts go in order, `last` ends the
// file. boot_id/uptime_s let the server date it. ESP_OK once confirmed.
static esp_err_t upload_part(const char *file_name, int part, bool last, uint32_t boot_id, long long uptime_s,
                             const char *data, size_t len)
{
    // Only host:port is used, anything from "/api/" on is dropped.
    const char *base = CONFIG_MIOT_SERVER_URL;
    const char *api = strstr(base, "/api/");
    int base_len = api ? (int)(api - base) : (int)strlen(base);
    if (base_len > 0 && base[base_len - 1] == '/') {
        base_len--;
    }
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

static bool upload_file(int index, long size, uint32_t boot_id)
{
    static char buf[PART_SIZE];
    char name[32];
    sd_logger_file_name(index, name, sizeof(name));

    FILE *f = sd_logger_open_read(index);
    if (f == NULL) {
        ESP_LOGE(TAG, "can't open %s", name);
        return false;
    }
    long sent = 0;
    int part = 0;
    bool ok = true;
    while (sent < size) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0) {
            ESP_LOGE(TAG, "read error in %s at %ld/%ld bytes", name, sent, size);
            ok = false;
            break;
        }
        sent += (long)n;
        bool last = sent >= size;
        long long uptime_s = esp_timer_get_time() / 1000000LL;
        if (upload_part(name, part, last, boot_id, uptime_s, buf, n) != ESP_OK) {
            ok = false;
            break;
        }
        part++;
    }
    fclose(f);
    if (ok) {
        ESP_LOGI(TAG, "%s sent (%ld bytes, %d parts)", name, size, part);
    }
    return ok;
}

bool sd_upload_all(uint32_t boot_id)
{
    if (!sd_logger_reinit()) {
        ESP_LOGW(TAG, "no usable SD card");
        return false;
    }
    for (int i = 0; i < MAX_FILES_PER_RUN; i++) {
        int index;
        long size;
        if (!sd_logger_oldest_file(&index, &size)) {
            ESP_LOGI(TAG, "no measurement files left on the SD card");
            return true;
        }
        if (size < 0) {
            return false; // can't stat it, leave it
        }
        if (size > 0 && !upload_file(index, size, boot_id)) {
            ESP_LOGW(TAG, "upload stopped - the remaining files stay on the card");
            return false;
        }
        if (!sd_logger_delete(index)) {
            return false;
        }
    }
    return false;
}
