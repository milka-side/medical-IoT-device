#include <math.h>
#include <stdarg.h>
#include <stdint.h>
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
#include "command_poll.h"
#include "max30102.h"
#include "pins.h"
#include "telemetry_http.h"
#include "telemetry_azure.h"
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

// No measurement can start until this many seconds after boot (dashboard
// shows a countdown and disables the buttons for this long too).
#define BOOT_LOCKOUT_S   0 // lockout removed: measurements allowed right after boot

// How long one button press keeps its sensor powered on.
#define ECG_MEASURE_S    (10 * 60)
#define VITALS_MEASURE_S 120
#define TEMP_MEASURE_S   60

#define US_PER_S 1000000LL

// *_active = "a window is open", written only by sender_loop's task.
// ecg_task/ppg_task watch these and do the actual sensor power-up/down
// themselves: each sensor's bus handle may only be used from one task
// (the ESP-IDF SPI/I2C device handles aren't safe to share), and those
// tasks are the ones already talking to the sensor.
static volatile bool s_ecg_active = false;
static volatile bool s_vitals_active = false;
static volatile bool s_temp_active = false;

// Window end times on the esp_timer clock. Real-time deadlines rather than
// counting loop iterations: each sender iteration is 1 s of sleep *plus*
// however long the command poll and telemetry POST took, so counting
// iterations stretched "10 minutes" by however slow the network was.
// Sender-task only, so the non-atomic 64-bit access is fine.
static int64_t s_ecg_deadline_us, s_vitals_deadline_us, s_temp_deadline_us;

// Set by ppg_task once MAX30102 is actually awake with freshly cleared
// buffers, so the sender doesn't average in a stale reading from the
// previous window during the few ms before ppg_task notices a new one.
static volatile bool s_vitals_running = false;

// From ecg_task, -1 while ECG isn't running: ERROR_LOD bitmask (bit0 = RA,
// bit1 = LA electrode off) and the ADC sample rate actually delivered
// (should be ~800; lower means DRDY edges are being missed).
static volatile int s_lod_mask = -1;
static volatile int s_adc_sps = -1;

// Running average accumulators for the current measurement window - the
// reported hr/spo2/temp are the mean over the whole window so far (not a
// single instantaneous sample), reset to empty each time a window opens.
static double s_hr_sum, s_spo2_sum, s_temp_sum;
static int s_hr_count, s_spo2_count, s_temp_count;

// ADS1293's actual output rate (800 sps with the current R2/R3) is far
// denser than a dashboard chart needs, and was making the line look like a
// fuzzy band just from too many points per pixel column. The signal is
// already low-pass filtered to ~40 Hz inside ads1293_read_sample, but that
// filter is a single IIR pole (-20 dB/decade) - at the decimated Nyquist
// (800/4/2 = 100 Hz) it's only attenuated a few dB, so mains/muscle
// content just above 40 Hz would alias back into the 0-40 Hz band if we
// just picked every 4th sample. Instead we average all ECG_DECIMATION raw
// samples per output point (boxcar filter): its response has nulls at
// every multiple of the decimated rate, which gives much better rejection
// right where naive decimation would alias, on top of the existing IIR
// stage - as well as a lighter, cleaner-looking stream.
#define ECG_DECIMATION 4 // 800 sps / 4 = 200 Hz output

// Fixed by the ADS1293 crystal and R1/R2/R3, not by how often we happen to
// send - so it's reported as-is instead of measured per batch (a partial
// first/last batch of a window used to come out as e.g. "30 Hz" and the
// dashboard then stretched that batch across the whole chart).
#define ECG_RAW_SPS (102400.0 / (4 * 4 * 8))
#define ECG_OUT_FS  (ECG_RAW_SPS / ECG_DECIMATION)

// Electrode-contact / delivered-rate check cadence, in raw samples (~1 s).
#define LOD_CHECK_EVERY_SAMPLES 800

static void ecg_task(void *arg)
{
    bool running = false;
    int decim_counter = 0;
    int64_t decim_sum = 0;
    int lod_counter = 0;
    int64_t window_start_us = 0;
    while (1) {
        bool want = s_ecg_active;
        if (want != running) {
            if (want) {
                ads1293_start_conversion();
                window_start_us = esp_timer_get_time();
            } else {
                ads1293_stop_conversion();
                s_lod_mask = -1;
                s_adc_sps = -1;
            }
            running = want;
            // Drop any partial batch so a fresh window doesn't start by
            // sending an average that mixes pre- and post-gap samples.
            decim_counter = 0;
            decim_sum = 0;
            lod_counter = 0;
        }
        if (!running) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int32_t sample;
        esp_err_t err = ads1293_read_sample(&sample, 1000);
        if (err == ESP_OK) {
            if (++lod_counter >= LOD_CHECK_EVERY_SAMPLES) {
                lod_counter = 0;
                int64_t now_us = esp_timer_get_time();
                if (now_us > window_start_us) {
                    s_adc_sps = (int)lround(LOD_CHECK_EVERY_SAMPLES * (double)US_PER_S / (now_us - window_start_us));
                }
                window_start_us = now_us;
                uint8_t mask;
                if (ads1293_read_lead_off(&mask) == ESP_OK) {
                    s_lod_mask = mask;
                }
            }

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
    bool running = false;
    while (1) {
        bool want = s_vitals_active;
        if (want != running) {
            if (want) {
                max30102_start();
            } else {
                max30102_stop();
            }
            running = want;
            s_vitals_running = want;
        }
        if (!running) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        max30102_poll();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Bounded string builder: a plain `len += snprintf(buf + len, cap - len, ...)`
// chain underflows `cap - len` the moment one call truncates, and every
// following call then writes past the end of the heap buffer.
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool truncated;
} jbuf_t;

static void jb_append(jbuf_t *b, const char *fmt, ...)
{
    if (b->truncated) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) {
        b->truncated = true;
        return;
    }
    b->len += (size_t)n;
}

typedef struct {
    const int32_t *ecg;
    int ecg_count;
    bool temp_ok;
    float temp_c;
    bool vitals_ok;
    int hr, spo2;
    const char *status;
    // -1 = omit the field
    int lockout_remaining_s, ecg_remaining_s, vitals_remaining_s, temp_remaining_s;
    int lod_mask, adc_sps;
} telemetry_t;

// Returns the JSON length, or 0 if it didn't fit (never sends a cut-off,
// malformed payload).
static size_t build_telemetry_json(char *buf, size_t cap, const telemetry_t *t)
{
    jbuf_t b = {.buf = buf, .cap = cap};
    jb_append(&b, "{\"device_id\":\"%s\"", CONFIG_MIOT_DEVICE_ID);

    if (t->ecg_count > 0) {
        jb_append(&b, ",\"fs\":%.2f,\"ecg\":[", ECG_OUT_FS);
        for (int i = 0; i < t->ecg_count; i++) {
            jb_append(&b, "%s%ld", i ? "," : "", (long)t->ecg[i]);
        }
        jb_append(&b, "]");
    }
    if (t->temp_ok) {
        jb_append(&b, ",\"temp\":%.2f", t->temp_c);
    }
    if (t->vitals_ok) {
        jb_append(&b, ",\"hr\":%d,\"spo2\":%d", t->hr, t->spo2);
    }
    jb_append(&b, ",\"status\":\"%s\"", t->status);
    if (t->lockout_remaining_s >= 0) {
        jb_append(&b, ",\"lockout_remaining_s\":%d", t->lockout_remaining_s);
    }
    if (t->ecg_remaining_s >= 0) {
        jb_append(&b, ",\"ecg_remaining_s\":%d", t->ecg_remaining_s);
    }
    if (t->vitals_remaining_s >= 0) {
        jb_append(&b, ",\"vitals_remaining_s\":%d", t->vitals_remaining_s);
    }
    if (t->temp_remaining_s >= 0) {
        jb_append(&b, ",\"temp_remaining_s\":%d", t->temp_remaining_s);
    }
    if (t->lod_mask >= 0) {
        jb_append(&b, ",\"lod\":%d", t->lod_mask);
    }
    if (t->adc_sps >= 0) {
        jb_append(&b, ",\"adc_sps\":%d", t->adc_sps);
    }
    jb_append(&b, "}");
    return b.truncated ? 0 : b.len;
}

static int remaining_s(int64_t deadline_us, int64_t now_us)
{
    int64_t left = deadline_us - now_us;
    if (left <= 0) {
        return 0;
    }
    if (left >= (int64_t)INT32_MAX * US_PER_S) {
        return INT32_MAX; // "forever" window (WIFI_DISABLED_FOR_TEST)
    }
    return (int)((left + US_PER_S - 1) / US_PER_S);
}

// Applies a freshly-polled command: opens the requested window(s),
// (re)setting their end time to a full duration from now. A command for an
// already-active sensor just extends/restarts its window rather than being
// a no-op - simplest consistent behavior for a repeated button press.
// ecg_task/ppg_task see the flag flip and power the sensor up.
static void apply_command(measure_cmd_t cmd, int64_t now_us)
{
    if (cmd == MEASURE_ECG || cmd == MEASURE_ALL) {
        s_ecg_deadline_us = now_us + ECG_MEASURE_S * US_PER_S;
        s_ecg_active = true;
    }
    if (cmd == MEASURE_VITALS || cmd == MEASURE_ALL) {
        if (!s_vitals_active) {
            s_hr_sum = s_spo2_sum = 0;
            s_hr_count = s_spo2_count = 0;
        }
        s_vitals_deadline_us = now_us + VITALS_MEASURE_S * US_PER_S;
        s_vitals_active = true;
    }
    if (cmd == MEASURE_TEMP || cmd == MEASURE_ALL) {
        if (!s_temp_active) {
            s_temp_sum = 0;
            s_temp_count = 0;
        }
        s_temp_deadline_us = now_us + TEMP_MEASURE_S * US_PER_S;
        s_temp_active = true; // TMP117 stays on regardless (negligible draw)
    }
}

// Closes every window whose end time has passed (the owning task powers
// the sensor down when it sees the flag clear).
static void close_expired_windows(int64_t now_us)
{
    if (s_ecg_active && now_us >= s_ecg_deadline_us) {
        s_ecg_active = false;
    }
    if (s_vitals_active && now_us >= s_vitals_deadline_us) {
        s_vitals_active = false;
    }
    if (s_temp_active && now_us >= s_temp_deadline_us) {
        s_temp_active = false;
    }
}

static void sender_loop(void)
{
    int32_t *ecg_batch = malloc(ECG_DRAIN_MAX * sizeof(int32_t));
    bool wifi_ps_on = true;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        bool boot_lockout = esp_timer_get_time() < BOOT_LOCKOUT_S * US_PER_S;
        if (!boot_lockout) {
            measure_cmd_t cmd = command_poll_tick(); // may block up to its HTTP timeout
            apply_command(cmd, esp_timer_get_time());
            close_expired_windows(esp_timer_get_time());
        }

        // Modem-sleep (ESP-IDF's default for STA) wakes the radio every
        // beacon interval, ~102 ms - those periodic current pulses land at
        // ~10 Hz, right inside the ECG band where no filter can remove them.
        // Keep the radio fully awake while ECG is recording, doze otherwise.
        if (s_ecg_active == wifi_ps_on) {
            wifi_ps_on = !s_ecg_active;
            wifi_connect_set_power_save(wifi_ps_on);
        }

        int n = 0;
        int32_t v;
        while (n < ECG_DRAIN_MAX && xQueueReceive(s_ecg_queue, &v, 0) == pdTRUE) {
            ecg_batch[n++] = v;
        }

        // Report the running average over the whole measurement window so
        // far, not a single noisy instantaneous sample - each tick folds
        // one more reading into s_temp_sum/s_hr_sum/s_spo2_sum (reset to
        // empty in apply_command() when the window opened).
        telemetry_t t = {
            .ecg = ecg_batch,
            .ecg_count = n,
            .lockout_remaining_s = -1,
            .ecg_remaining_s = -1,
            .vitals_remaining_s = -1,
            .temp_remaining_s = -1,
            .lod_mask = -1,
            .adc_sps = -1,
        };

        if (s_temp_active) {
            float temp_sample;
            if (tmp117_read_celsius(&temp_sample) == ESP_OK) {
                s_temp_sum += temp_sample;
                s_temp_count++;
            }
            if (s_temp_count > 0) {
                t.temp_c = (float)(s_temp_sum / s_temp_count);
                t.temp_ok = true;
            }
        }

        if (s_vitals_active && s_vitals_running) {
            int hr_sample, spo2_sample;
            bool sample_ok = false;
            max30102_estimate_vitals(&hr_sample, &spo2_sample, &sample_ok);
            if (sample_ok) {
                s_hr_sum += hr_sample;
                s_hr_count++;
                s_spo2_sum += spo2_sample;
                s_spo2_count++;
            }
        }
        if (s_vitals_active && s_hr_count > 0) {
            t.hr = (int)lround(s_hr_sum / s_hr_count);
            t.spo2 = (int)lround(s_spo2_sum / s_spo2_count);
            t.vitals_ok = true;
        }

        int64_t now_us = esp_timer_get_time();
        if (boot_lockout) {
            t.status = "boot_lockout";
            t.lockout_remaining_s = remaining_s(BOOT_LOCKOUT_S * US_PER_S, now_us);
        } else {
            t.status = (s_ecg_active || s_vitals_active || s_temp_active) ? "measuring" : "idle";
        }
        if (s_ecg_active) {
            t.ecg_remaining_s = remaining_s(s_ecg_deadline_us, now_us);
            t.lod_mask = s_lod_mask;
            t.adc_sps = s_adc_sps;
        }
        if (s_vitals_active) {
            t.vitals_remaining_s = remaining_s(s_vitals_deadline_us, now_us);
        }
        if (s_temp_active) {
            t.temp_remaining_s = remaining_s(s_temp_deadline_us, now_us);
        }

        // Up to 11 chars + comma per sample (worst-case int32 after a
        // saturating step, e.g. an electrode coming off) plus fixed fields.
        size_t cap = (size_t)n * 12 + 512;
        char *json = malloc(cap);
        if (json == NULL) {
            ESP_LOGE(TAG, "OOM building JSON payload (n=%d)", n);
            continue;
        }
        size_t len = build_telemetry_json(json, cap, &t);
        if (len == 0) {
            ESP_LOGE(TAG, "telemetry JSON didn't fit in %u bytes, batch dropped", (unsigned)cap);
        } else if (wifi_connect_is_up()) {
            if (telemetry_http_post(json, len) != ESP_OK) {
                ESP_LOGW(TAG, "telemetry POST failed");
            }
            if (telemetry_mqtt_publish(json, len) != ESP_OK) {
                ESP_LOGW(TAG, "telemetry MQTT publish failed");
            }
            bool idle = !boot_lockout && !(s_ecg_active || s_vitals_active || s_temp_active);
            if (telemetry_azure_publish(json, len, idle) != ESP_OK) {
                ESP_LOGW(TAG, "telemetry Azure publish failed");
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

    // Wi-Fi: block here until connected or the quick retries run out -
    // after that wifi_connect keeps retrying every 10 s in the background,
    // and POSTs are just skipped while it's down.
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
    if (telemetry_azure_init() != ESP_OK) {
        ESP_LOGW(TAG, "Azure IoT Hub telemetry init failed; continuing without it");
    }

#if WIFI_DISABLED_FOR_TEST
    // No network -> no command channel to wait on, so force ECG on
    // permanently (bypassing the boot lockout too) for the offline
    // noise-source diagnostic this flag exists for. ecg_task powers the
    // AFE up as soon as it sees the flag.
    s_ecg_deadline_us = INT64_MAX;
    s_ecg_active = true;
#endif

    xTaskCreate(ecg_task, "ecg_task", 4096, NULL, 10, NULL);
    xTaskCreate(ppg_task, "ppg_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "init done, entering send loop (sensors off until a measurement is requested)");
    sender_loop(); // never returns
}
