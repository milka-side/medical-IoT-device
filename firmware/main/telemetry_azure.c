#include "telemetry_azure.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "telemetry_azure";

#ifdef CONFIG_MIOT_AZURE_ENABLE

#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mqtt_client.h"

// SAS token lifetime and how long before expiry we reconnect with a new one.
#define SAS_TTL_S        (24 * 3600)
#define SAS_RENEW_EARLY_S (15 * 60)
// Anything before this is "clock not set yet" (SNTP hasn't synced).
#define MIN_VALID_EPOCH  1700000000

static char s_host[128];
static char s_device_id[64];
static char s_key_b64[96];

static char s_uri[160];
static char s_username[256];
static char s_password[512];
static char s_topic[192];

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static int64_t s_last_idle_us = INT64_MIN / 2;

// "HostName=x.azure-devices.net;DeviceId=y;SharedAccessKey=z" -> fields.
static bool parse_connection_string(const char *cs)
{
    s_host[0] = s_device_id[0] = s_key_b64[0] = '\0';
    const char *p = cs;
    while (*p) {
        const char *end = strchr(p, ';');
        size_t seg_len = end ? (size_t)(end - p) : strlen(p);
        const char *eq = memchr(p, '=', seg_len);
        if (eq) {
            size_t klen = (size_t)(eq - p);
            size_t vlen = seg_len - klen - 1;
            char *dst = NULL;
            size_t cap = 0;
            if (klen == 8 && strncmp(p, "HostName", 8) == 0) {
                dst = s_host, cap = sizeof(s_host);
            } else if (klen == 8 && strncmp(p, "DeviceId", 8) == 0) {
                dst = s_device_id, cap = sizeof(s_device_id);
            } else if (klen == 15 && strncmp(p, "SharedAccessKey", 15) == 0) {
                dst = s_key_b64, cap = sizeof(s_key_b64);
            }
            if (dst) {
                if (vlen >= cap) {
                    return false;
                }
                memcpy(dst, eq + 1, vlen);
                dst[vlen] = '\0';
            }
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return s_host[0] && s_device_id[0] && s_key_b64[0];
}

// Percent-encode everything except RFC 3986 unreserved characters
// (same result as JavaScript's encodeURIComponent for ASCII input).
static bool url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *c = (const unsigned char *)in; *c; c++) {
        bool plain = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
                     *c == '-' || *c == '_' || *c == '.' || *c == '~';
        size_t need = plain ? 1 : 3;
        if (o + need >= cap) {
            return false;
        }
        if (plain) {
            out[o++] = (char)*c;
        } else {
            out[o++] = '%';
            out[o++] = hex[*c >> 4];
            out[o++] = hex[*c & 0x0F];
        }
    }
    out[o] = '\0';
    return true;
}

// SharedAccessSignature sr=<enc(uri)>&sig=<enc(b64(hmac_sha256(key, enc(uri)\nexpiry)))>&se=<expiry>
static bool build_sas_token(time_t expiry)
{
    char resource[200];
    char resource_enc[300];
    snprintf(resource, sizeof(resource), "%s/devices/%s", s_host, s_device_id);
    if (!url_encode(resource, resource_enc, sizeof(resource_enc))) {
        return false;
    }

    char to_sign[360];
    int sign_len = snprintf(to_sign, sizeof(to_sign), "%s\n%lld", resource_enc, (long long)expiry);
    if (sign_len <= 0 || sign_len >= (int)sizeof(to_sign)) {
        return false;
    }

    unsigned char key[64];
    size_t key_len = 0;
    if (mbedtls_base64_decode(key, sizeof(key), &key_len, (const unsigned char *)s_key_b64, strlen(s_key_b64)) != 0) {
        ESP_LOGE(TAG, "SharedAccessKey is not valid base64");
        return false;
    }

    unsigned char mac[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_hmac(md, key, key_len, (const unsigned char *)to_sign, (size_t)sign_len, mac) != 0) {
        return false;
    }

    unsigned char sig_b64[64];
    size_t sig_b64_len = 0;
    if (mbedtls_base64_encode(sig_b64, sizeof(sig_b64) - 1, &sig_b64_len, mac, sizeof(mac)) != 0) {
        return false;
    }
    sig_b64[sig_b64_len] = '\0';

    char sig_enc[128];
    if (!url_encode((const char *)sig_b64, sig_enc, sizeof(sig_enc))) {
        return false;
    }

    int n = snprintf(s_password, sizeof(s_password), "SharedAccessSignature sr=%s&sig=%s&se=%lld", resource_enc,
                     sig_enc, (long long)expiry);
    return n > 0 && n < (int)sizeof(s_password);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "connected to Azure IoT Hub %s as %s", s_host, s_device_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected from Azure IoT Hub");
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "transport error: esp_tls=0x%x, tls_stack=0x%x, errno=%d (%s)",
                     event->error_handle->esp_tls_last_esp_err, event->error_handle->esp_tls_stack_err,
                     event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno));
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            // 4 = bad username/password (wrong key or expired/invalid SAS),
            // 5 = not authorized (device disabled / unknown device id).
            ESP_LOGE(TAG, "connection refused by hub, code %d - check the device connection string",
                     event->error_handle->connect_return_code);
        }
        break;
    default:
        break;
    }
}

static void azure_task(void *arg)
{
    (void)arg;

    // Wall-clock time is needed for the SAS token's expiry.
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_cfg);
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK || time(NULL) < MIN_VALID_EPOCH) {
        ESP_LOGW(TAG, "waiting for SNTP time sync before connecting to Azure...");
    }
    ESP_LOGI(TAG, "time synced (epoch %lld)", (long long)time(NULL));

    while (1) {
        time_t expiry = time(NULL) + SAS_TTL_S;
        if (!build_sas_token(expiry)) {
            ESP_LOGE(TAG, "failed to build SAS token; Azure telemetry disabled");
            vTaskDelete(NULL);
            return;
        }

        // Full config both times: esp_mqtt_set_config() overwrites some
        // fields (e.g. TLS verification) with whatever is passed in.
        esp_mqtt_client_config_t cfg = {
            .broker.address.uri = s_uri,
            .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
            .credentials.client_id = s_device_id,
            .credentials.username = s_username,
            .credentials.authentication.password = s_password,
            .session.keepalive = 240,
            .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
            .network.reconnect_timeout_ms = 10000,
        };

        if (s_client == NULL) {
            s_client = esp_mqtt_client_init(&cfg);
            if (s_client == NULL) {
                ESP_LOGE(TAG, "esp_mqtt_client_init failed");
                vTaskDelete(NULL);
                return;
            }
            esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
            esp_err_t err = esp_mqtt_client_start(s_client);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(err));
            }
        } else {
            // Token about to expire: swap in the new one and reconnect.
            esp_mqtt_set_config(s_client, &cfg);
            ESP_LOGI(TAG, "renewing SAS token, reconnecting");
            esp_mqtt_client_disconnect(s_client);
            esp_mqtt_client_reconnect(s_client);
        }

        // Seconds * tick rate, not pdMS_TO_TICKS: ms*Hz would overflow 32 bits.
        vTaskDelay((TickType_t)(SAS_TTL_S - SAS_RENEW_EARLY_S) * configTICK_RATE_HZ);
    }
}

esp_err_t telemetry_azure_init(void)
{
    if (!parse_connection_string(CONFIG_MIOT_AZURE_CONNECTION_STRING)) {
        ESP_LOGE(TAG, "Azure device connection string missing or malformed "
                      "(expected HostName=...;DeviceId=...;SharedAccessKey=...)");
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(s_uri, sizeof(s_uri), "mqtts://%s:8883", s_host);
    snprintf(s_username, sizeof(s_username), "%s/%s/?api-version=2021-04-12", s_host, s_device_id);
    // $.ct/$.ce mark the body as UTF-8 JSON for IoT Hub routing/queries.
    snprintf(s_topic, sizeof(s_topic), "devices/%s/messages/events/$.ct=application%%2Fjson&$.ce=utf-8",
             s_device_id);

    if (xTaskCreate(azure_task, "azure_task", 6144, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Azure IoT Hub telemetry enabled: %s, device %s", s_host, s_device_id);
    return ESP_OK;
}

esp_err_t telemetry_azure_publish(const char *json_body, size_t len, bool idle)
{
    if (!s_connected) {
        return ESP_OK;
    }
    if (idle) {
        int64_t now = esp_timer_get_time();
        if (now - s_last_idle_us < (int64_t)CONFIG_MIOT_AZURE_IDLE_PERIOD_S * 1000000) {
            return ESP_OK;
        }
        s_last_idle_us = now;
    }
    int msg_id = esp_mqtt_client_publish(s_client, s_topic, json_body, (int)len, /*qos=*/0, /*retain=*/0);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

#else // !CONFIG_MIOT_AZURE_ENABLE

esp_err_t telemetry_azure_init(void)
{
    ESP_LOGI(TAG, "Azure IoT Hub telemetry disabled (enable via menuconfig -> Medical IoT Device)");
    return ESP_OK;
}

esp_err_t telemetry_azure_publish(const char *json_body, size_t len, bool idle)
{
    (void)json_body;
    (void)len;
    (void)idle;
    return ESP_OK;
}

#endif
