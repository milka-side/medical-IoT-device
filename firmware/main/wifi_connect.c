#include "wifi_connect.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "pins.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRY      10

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;
static volatile bool s_connected = false;
static bool s_started = false;
static esp_timer_handle_t s_reconnect_timer;

// After the quick retries run out, keep trying every 10 s forever rather
// than giving up - an AP reboot or a walk out of range must not leave the
// device offline until someone power-cycles it (which would also restart
// the 10-minute boot lockout and kill any measurement in progress).
#define WIFI_RETRY_BACKOFF_US (10LL * 1000 * 1000)

static void reconnect_timer_cb(void *arg)
{
    esp_wifi_connect();
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry_count < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "retry connecting to AP (%d/%d)", s_retry_count, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "still disconnected, next attempt in 10 s");
            // No-op (ESP_ERR_INVALID_STATE) if an attempt is already scheduled.
            esp_timer_start_once(s_reconnect_timer, WIFI_RETRY_BACKOFF_US);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void rf_switch_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_RF_SWITCH_EN) | (1ULL << PIN_RF_ANT_SELECT),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(PIN_RF_SWITCH_EN, 0);   // power the switch
    vTaskDelay(pdMS_TO_TICKS(100));        // same settle delay as Seeed's own example
    gpio_set_level(PIN_RF_ANT_SELECT, 0);  // built-in ceramic antenna
}

esp_err_t wifi_connect_start(void)
{
    rf_switch_init();
    s_wifi_event_group = xEventGroupCreate();

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, CONFIG_MIOT_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, CONFIG_MIOT_WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
    // WIFI_AUTH_OPEN is just the minimum accepted security, not a
    // requirement that the AP be open - this lets the same build join
    // either an open network (e.g. a guest SSID) or a secured one.
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;

    ESP_LOGI(TAG, "connecting to SSID '%s'...", CONFIG_MIOT_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
                                            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected");
        // Device now spends most of its time idle (sensors off between
        // measurement windows) - modem-sleep lets the radio doze between
        // beacon intervals instead of staying fully powered for no reason.
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "failed to connect to SSID '%s'", CONFIG_MIOT_WIFI_SSID);
    return ESP_FAIL;
}

bool wifi_connect_is_up(void)
{
    return s_connected;
}

void wifi_connect_set_power_save(bool enable)
{
    if (!s_started) {
        return; // Wi-Fi never initialised (e.g. WIFI_DISABLED_FOR_TEST)
    }
    esp_wifi_set_ps(enable ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}
