#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "ads1293.h"
#include "max30102.h"
#include "pins.h"
#include "telemetry_http.h"
#include "telemetry_mqtt.h"
#include "tmp117.h"
#include "wifi_connect.h"

static const char *TAG = "main";

// TEMP DIAGNOSTIC: Wi-Fi disabled to test whether its radio is a noise
// source for the ADS1293 analog front end (both radiated pickup and
// supply-rail current spikes during TX are documented on ESP32 boards).
// Observe raw/filtered ECG over USB serial instead, since there's no
// network to POST over. Flip back to 0 to restore normal operation.
#define WIFI_DISABLED_FOR_TEST 0

#define ECG_QUEUE_LEN    2048
#define ECG_DRAIN_MAX    2048

static QueueHandle_t s_ecg_queue;

// ADS1293's actual output rate (~850 sps with the current R2/R3) is far
// denser than a dashboard chart needs, and was making the line look like a
// fuzzy band just from too many points per pixel column. The signal is
// already low-pass filtered to ~40 Hz inside ads1293_read_sample, but that
// filter is a single IIR pole (-20 dB/decade) - at the decimated Nyquist
// (~853.5/8/2 ~= 53 Hz) it's only attenuated a few dB, so mains/muscle
// content just above 40 Hz would alias back into the 0-40 Hz band if we
// just picked every 8th sample. Instead we average all ECG_DECIMATION raw
// samples per output point (boxcar filter): its response has nulls at
// every multiple of the decimated rate, which gives much better rejection
// right where naive decimation would alias, on top of the existing IIR
// stage - as well as a lighter, cleaner-looking stream.
#define ECG_DECIMATION 8

static void ecg_task(void *arg)
{
    int decim_counter = 0;
    int64_t decim_sum = 0;
    while (1) {
        int32_t sample;
        esp_err_t err = ads1293_read_sample(&sample, 1000);
        if (err == ESP_OK) {
            decim_sum += sample;
            if (++decim_counter < ECG_DECIMATION) {
                continue;
            }
            int32_t avg = (int32_t)(decim_sum / ECG_DECIMATION);
            decim_counter = 0;
            decim_sum = 0;

#if WIFI_DISABLED_FOR_TEST
            // Plain printf (not ESP_LOGI) so it's easy to grep/parse out of
            // the serial log for a quick stats check, same as the ones run
            // against the WebSocket feed when Wi-Fi was on.
            printf("ECG,%ld\n", (long)avg);
#endif

            // Non-blocking send: if the sender hasn't caught up yet, drop
            // the oldest-pending sample rather than stalling acquisition.
            if (xQueueSend(s_ecg_queue, &avg, 0) != pdTRUE) {
                int32_t discard;
                xQueueReceive(s_ecg_queue, &discard, 0);
                xQueueSend(s_ecg_queue, &avg, 0);
            }
        } else if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "ADS1293: no DRDY in 1s - check wiring/config");
        }
    }
}

static void ppg_task(void *arg)
{
    while (1) {
        max30102_poll();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static size_t build_telemetry_json(char *buf, size_t cap, const int32_t *ecg, int ecg_count, int fs,
                                    bool temp_ok, float temp_c, bool vitals_ok, int hr, int spo2)
{
    size_t len = 0;
    len += snprintf(buf + len, cap - len, "{\"device_id\":\"%s\"", CONFIG_MIOT_DEVICE_ID);

    if (ecg_count > 0) {
        len += snprintf(buf + len, cap - len, ",\"fs\":%d,\"ecg\":[", fs);
        for (int i = 0; i < ecg_count; i++) {
            len += snprintf(buf + len, cap - len, "%s%ld", i ? "," : "", (long)ecg[i]);
        }
        len += snprintf(buf + len, cap - len, "]");
    }
    if (temp_ok) {
        len += snprintf(buf + len, cap - len, ",\"temp\":%.2f", temp_c);
    }
    if (vitals_ok) {
        len += snprintf(buf + len, cap - len, ",\"hr\":%d,\"spo2\":%d", hr, spo2);
    }
    len += snprintf(buf + len, cap - len, "}");
    return len;
}

static void sender_loop(void)
{
    int32_t *ecg_batch = malloc(ECG_DRAIN_MAX * sizeof(int32_t));
    int64_t last_send_us = esp_timer_get_time();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        int n = 0;
        int32_t v;
        while (n < ECG_DRAIN_MAX && xQueueReceive(s_ecg_queue, &v, 0) == pdTRUE) {
            ecg_batch[n++] = v;
        }

        int64_t now_us = esp_timer_get_time();
        double elapsed_s = (now_us - last_send_us) / 1e6;
        last_send_us = now_us;
        int fs = (elapsed_s > 0 && n > 0) ? (int)lround(n / elapsed_s) : 0;

        float temp_c = 0.0f;
        bool temp_ok = (tmp117_read_celsius(&temp_c) == ESP_OK);

        int hr = 0, spo2 = 0;
        bool vitals_ok = false;
        max30102_estimate_vitals(&hr, &spo2, &vitals_ok);

        size_t cap = (size_t)n * 9 + 256;
        char *json = malloc(cap);
        if (json == NULL) {
            ESP_LOGE(TAG, "OOM building JSON payload (n=%d)", n);
            continue;
        }
        size_t len = build_telemetry_json(json, cap, ecg_batch, n, fs, temp_ok, temp_c, vitals_ok, hr, spo2);

        if (wifi_connect_is_up()) {
            esp_err_t err = telemetry_http_post(json, len);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "telemetry POST failed");
            }
            if (telemetry_mqtt_publish(json, len) != ESP_OK) {
                ESP_LOGW(TAG, "telemetry MQTT publish failed");
            }
        } else {
            ESP_LOGW(TAG, "Wi-Fi down, skipping POST (ecg_samples=%d)", n);
        }

        free(json);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_ecg_queue = xQueueCreate(ECG_QUEUE_LEN, sizeof(int32_t));

    // Wi-Fi: block here until connected (or retries exhausted - firmware
    // keeps running either way, POSTs are just skipped while down).
#if !WIFI_DISABLED_FOR_TEST
    if (wifi_connect_start() != ESP_OK) {
        ESP_LOGW(TAG, "starting without Wi-Fi; will keep retrying in background");
    }
#else
    ESP_LOGW(TAG, "Wi-Fi intentionally disabled for noise-source test - no network, watch serial log");
#endif

    // Shared I2C bus: TMP117 + MAX30102.
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_PORT_NUM,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus));

    // SPI bus for the ADS1293 (SD card logging left out for now - this build
    // only cares about getting sensor data to the server).
    spi_bus_config_t spi_bus_cfg = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_ID, &spi_bus_cfg, SPI_DMA_CH_AUTO));

    ESP_ERROR_CHECK(ads1293_init(SPI_HOST_ID, PIN_ADS1293_CS));
    tmp117_init(i2c_bus);
    max30102_init(i2c_bus);

    if (telemetry_mqtt_init() != ESP_OK) {
        ESP_LOGW(TAG, "MQTT telemetry init failed; continuing with HTTP only");
    }

    xTaskCreate(ecg_task, "ecg_task", 4096, NULL, 10, NULL);
    xTaskCreate(ppg_task, "ppg_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "init done, entering send loop");
    sender_loop(); // never returns
}
