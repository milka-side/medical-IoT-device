#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "ads1293.h"
#include "device_commands.h"
#include "max30102.h"
#include "pins.h"
#include "offline_log.h"
#include "sd_logger.h"
#include "sd_upload.h"
#include "telemetry_azure.h"
#include "tmp117.h"
#include "wifi_connect.h"

static const char *TAG = "main";

// Debug: set to 1 to turn Wi-Fi off (checks if the radio adds ECG noise).
#define WIFI_DISABLED_FOR_TEST 0

#define ECG_QUEUE_LEN    2048
#define ECG_DRAIN_MAX    2048

static QueueHandle_t s_ecg_queue;

// Sensor warm-up after power-on, measurements are refused until it ends.
#ifdef CONFIG_MIOT_CALIBRATION_MODE
#define BOOT_LOCKOUT_S   0 // calibration needs the sensors right away
#else
#define BOOT_LOCKOUT_S   CONFIG_MIOT_BOOT_LOCKOUT_S
#endif

// How long one press keeps a sensor on.
#define ECG_MEASURE_S    (10 * 60)
#define VITALS_MEASURE_S (3 * 60)
#define TEMP_MEASURE_S   (3 * 60)

#define US_PER_S 1000000LL

// Set by the sender task only. ecg_task/ppg_task power their sensor
// up/down when the flag changes (bus handles belong to those tasks).
static volatile bool s_ecg_active = false;
static volatile bool s_vitals_active = false;
static volatile bool s_temp_active = false;

// Real end times, so a slow network doesn't stretch the windows.
static int64_t s_ecg_deadline_us, s_vitals_deadline_us, s_temp_deadline_us;

// Set by ppg_task once the MAX30102 is awake with an empty FIFO.
static volatile bool s_vitals_running = false;

// From ecg_task, -1 when ECG is off: lead-off bits and real ADC rate (~800).
static volatile int s_lod_mask = -1;
static volatile int s_adc_sps = -1;
static volatile int s_adc_dups = -1; // duplicate reads per second (debug)

// Pulse/SpO2 and temperature are sent once, as the window average.
static double s_hr_sum, s_spo2_sum;
static int s_hr_count, s_spo2_count;

// Every temperature reading of the window, so the warm-up part can be skipped.
#define TEMP_MAX_SAMPLES (TEMP_MEASURE_S + 60)
static float s_temp_samples[TEMP_MAX_SAMPLES];
static int s_temp_n;
// Settled = changes less than TEMP_SETTLE_DELTA_C over TEMP_SETTLE_SPAN_S.
#define TEMP_SETTLE_SPAN_S  20
#define TEMP_SETTLE_DELTA_C 0.10f

// Room temperature, read while idle. Drops at once but rises slowly,
// so a finger left on the sensor doesn't look like a warm room.
#define ROOM_SAMPLE_S 10
#define ROOM_RISE_C   0.02f
static float s_room_c = NAN;
static float s_window_room_c = NAN;

// Set when a window closes, the next telemetry line carries the result.
static bool s_vitals_result_pending, s_temp_result_pending;

// 800 sps is too dense for the chart. Average every 4 samples (boxcar)
// instead of picking every 4th one, so there's less aliasing.
#define ECG_DECIMATION 4 // 800 sps / 4 = 200 Hz output

// Fixed by the ADS1293 config, so sent as a constant.
#define ECG_RAW_SPS (102400.0 / (4 * 4 * 8))
#define ECG_OUT_FS  (ECG_RAW_SPS / ECG_DECIMATION)

// Lead-off / rate check every ~1 s.
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
            // Drop a partial batch from before the gap.
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
                s_adc_dups = (int)ads1293_take_duplicate_count();
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

#if WIFI_DISABLED_FOR_TEST || defined(CONFIG_MIOT_CALIBRATION_MODE)
            // Plain printf so it's easy to grep from the serial log.
            printf("ECG,%ld\n", (long)avg);
#endif

            // Don't block if the sender is behind.
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
#ifdef CONFIG_MIOT_CALIBRATION_MODE
                // LED test: 10 s LED1, 10 s LED2, then both.
                static const struct {
                    const char *name;
                    uint8_t led1, led2;
                } phases[] = {{"LED1_ONLY", 0x24, 0x00}, {"LED2_ONLY", 0x00, 0x24}};
                for (int p = 0; p < 2; p++) {
                    printf("LEDTEST,%s\n", phases[p].name);
                    max30102_set_led_currents(phases[p].led1, phases[p].led2);
                    for (int i = 0; i < 1000; i++) {
                        max30102_poll();
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                }
                printf("LEDTEST,BOTH\n");
                max30102_set_led_currents(0x24, 0x24);
#endif
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

// snprintf wrapper that can't run past the buffer after a truncation.
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
    float temp_c; // fingertip temperature
    float temp_body_c; // body temperature estimate, NAN if unknown
    float temp_room_c;
    bool vitals_ok;
    int hr, spo2;
    const char *status;
    // -1 = don't send
    int lockout_remaining_s, ecg_remaining_s, vitals_remaining_s, temp_remaining_s;
    int lod_mask, adc_sps;
    int sd_files; // files waiting on the SD card, -1 = no card
} telemetry_t;

// Random id per power-on. With "seq" it lets the server drop repeats
// and date offline data.
static uint32_t s_boot_id;

static uint32_t boot_id(void)
{
    if (s_boot_id == 0) {
        s_boot_id = esp_random() | 1u;
    }
    return s_boot_id;
}

// Returns the JSON length, 0 if it didn't fit.
static size_t build_telemetry_json(char *buf, size_t cap, const telemetry_t *t)
{
    static uint32_t s_seq;
    jbuf_t b = {.buf = buf, .cap = cap};
    jb_append(&b, "{\"device_id\":\"%s\",\"boot\":%lu,\"seq\":%lu", CONFIG_MIOT_DEVICE_ID,
              (unsigned long)boot_id(), (unsigned long)s_seq++);

    if (t->ecg_count > 0) {
        jb_append(&b, ",\"fs\":%.2f,\"ecg\":[", ECG_OUT_FS);
        for (int i = 0; i < t->ecg_count; i++) {
            jb_append(&b, "%s%ld", i ? "," : "", (long)t->ecg[i]);
        }
        jb_append(&b, "]");
    }
    if (t->temp_ok) {
        jb_append(&b, ",\"temp\":%.2f", t->temp_c);
        if (!isnan(t->temp_body_c)) {
            jb_append(&b, ",\"temp_body\":%.2f,\"temp_room\":%.2f", t->temp_body_c, t->temp_room_c);
        }
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
    if (t->sd_files >= 0) {
        jb_append(&b, ",\"sd_files\":%d", t->sd_files);
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
        return INT32_MAX;
    }
    return (int)((left + US_PER_S - 1) / US_PER_S);
}

// Opens (or restarts) the requested windows. ecg_task/ppg_task then power
// the sensors up.
static void apply_command(measure_cmd_t cmd, int64_t now_us)
{
    if (cmd & MEASURE_ECG) {
        s_ecg_deadline_us = now_us + ECG_MEASURE_S * US_PER_S;
        s_ecg_active = true;
    }
    if (cmd & MEASURE_VITALS) {
        if (!s_vitals_active) {
            s_hr_sum = s_spo2_sum = 0;
            s_hr_count = s_spo2_count = 0;
        }
        s_vitals_deadline_us = now_us + VITALS_MEASURE_S * US_PER_S;
        s_vitals_active = true;
    }
    if (cmd & MEASURE_TEMP) {
        if (!s_temp_active) {
            s_temp_n = 0;
            s_window_room_c = s_room_c;
        }
        s_temp_deadline_us = now_us + TEMP_MEASURE_S * US_PER_S;
        s_temp_active = true; // TMP117 is always on, it draws almost nothing
    }
}

// Closes expired windows, the tasks power the sensors down.
static void close_expired_windows(int64_t now_us)
{
    if (s_ecg_active && now_us >= s_ecg_deadline_us) {
        s_ecg_active = false;
    }
    if (s_vitals_active && now_us >= s_vitals_deadline_us) {
        s_vitals_active = false;
        s_vitals_result_pending = true;
    }
    if (s_temp_active && now_us >= s_temp_deadline_us) {
        s_temp_active = false;
        s_temp_result_pending = true;
    }
}

// Mean over the settled part of the window, or the last minute if it never
// settled. False if there was no reading.
static bool temp_window_result(float *out_skin, int *out_settled_after_s)
{
    if (s_temp_n == 0) {
        return false;
    }
    int start = -1;
    for (int i = 0; i + TEMP_SETTLE_SPAN_S < s_temp_n; i++) {
        if (fabsf(s_temp_samples[i + TEMP_SETTLE_SPAN_S] - s_temp_samples[i]) < TEMP_SETTLE_DELTA_C) {
            start = i;
            break;
        }
    }
    if (start < 0) {
        start = s_temp_n > 60 ? s_temp_n - 60 : 0;
    }
    double sum = 0;
    for (int i = start; i < s_temp_n; i++) {
        sum += s_temp_samples[i];
    }
    *out_skin = (float)(sum / (s_temp_n - start));
    *out_settled_after_s = start;
    return true;
}

// PM locks held only while ECG or pulse is measured: ECG needs every DRDY
// on time and MAX30102 is polled every 10 ms. Otherwise the CPU can slow
// down / light-sleep.
static esp_pm_lock_handle_t s_pm_no_sleep, s_pm_cpu_max;
// Held while USB is plugged in: with light sleep the USB port stopped
// responding and flashing failed.
static esp_pm_lock_handle_t s_pm_usb_no_sleep, s_pm_usb_cpu_max;

static void power_init(void)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
#ifdef CONFIG_MIOT_LIGHT_SLEEP
        .light_sleep_enable = true,
#else
        .light_sleep_enable = false,
#endif
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure failed: %s - running without power management", esp_err_to_name(err));
    }
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "measuring", &s_pm_no_sleep);
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "measuring", &s_pm_cpu_max);
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb_host", &s_pm_usb_no_sleep);
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "usb_host", &s_pm_usb_cpu_max);
    // Take them at boot, update_usb_power() drops them if no USB is attached.
    if (s_pm_usb_no_sleep && s_pm_usb_cpu_max) {
        esp_pm_lock_acquire(s_pm_usb_no_sleep);
        esp_pm_lock_acquire(s_pm_usb_cpu_max);
    }
#endif
}

static void power_set_measuring(bool measuring)
{
    if (s_pm_no_sleep == NULL || s_pm_cpu_max == NULL) {
        return; // PM not enabled in this build
    }
    if (measuring) {
        esp_pm_lock_acquire(s_pm_no_sleep);
        esp_pm_lock_acquire(s_pm_cpu_max);
    } else {
        esp_pm_lock_release(s_pm_cpu_max);
        esp_pm_lock_release(s_pm_no_sleep);
    }
}

// Keeps the chip awake while USB is attached.
static void update_usb_power(void)
{
    static bool held = true;
    if (s_pm_usb_no_sleep == NULL || s_pm_usb_cpu_max == NULL) {
        return;
    }
    bool attached = usb_serial_jtag_is_connected();
    if (attached != held) {
        held = attached;
        if (attached) {
            esp_pm_lock_acquire(s_pm_usb_no_sleep);
            esp_pm_lock_acquire(s_pm_usb_cpu_max);
        } else {
            esp_pm_lock_release(s_pm_usb_cpu_max);
            esp_pm_lock_release(s_pm_usb_no_sleep);
        }
    }
}

// Sets radio / CPU power depending on what is measured.
static void update_power_state(bool *wifi_ps_on, bool *power_measuring)
{
    // Modem sleep wakes the radio every ~102 ms, which shows up as ~10 Hz
    // noise in the ECG. Keep the radio awake while recording.
    if (s_ecg_active == *wifi_ps_on) {
        *wifi_ps_on = !s_ecg_active;
        wifi_connect_set_power_save(*wifi_ps_on);
    }
    bool measuring = s_ecg_active || s_vitals_active;
    if (measuring != *power_measuring) {
        *power_measuring = measuring;
        power_set_measuring(measuring);
    }
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "PANIC (crash)";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (supply voltage dropped)";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB (flashing/monitor)";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "unknown";
    }
}

// ---- Measure button (D1: 0 at rest, 1 when pressed) ----
// The ISR masks the pin, the sender task unmasks it once the button has
// been released for BUTTON_RELEASE_MS (debounce).
#define BUTTON_RELEASE_MS 50
#define DOUBLE_PRESS_MS   700

static TaskHandle_t s_sender_task;
static volatile bool s_button_pressed;
static volatile uint32_t s_button_irqs; // ISR hits since boot (debug)

static void button_isr(void *arg)
{
    gpio_intr_disable(PIN_BUTTON);
    s_button_irqs++;
    s_button_pressed = true;
    BaseType_t woken = pdFALSE;
    if (s_sender_task != NULL) {
        vTaskNotifyGiveFromISR(s_sender_task, &woken);
    }
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static void button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_HIGH_LEVEL,
    };
    gpio_config(&io);
    // ISR service is already installed by ads1293_init().
    gpio_isr_handler_add(PIN_BUTTON, button_isr, NULL);
}

// Re-arms the button after release. True while still waiting.
static bool button_rearm_pending(void)
{
    static int64_t released_since_us = -1;
    if (!s_button_pressed) {
        return false;
    }
    if (gpio_get_level(PIN_BUTTON) != 0) {
        released_since_us = -1;
        return true;
    }
    int64_t now = esp_timer_get_time();
    if (released_since_us < 0) {
        released_since_us = now;
    }
    if (now - released_since_us < BUTTON_RELEASE_MS * 1000LL) {
        return true;
    }
    released_since_us = -1;
    s_button_pressed = false;
    gpio_intr_enable(PIN_BUTTON);
    return false;
}

// Commands from Azure direct methods, picked up on the next tick.
static portMUX_TYPE s_remote_lock = portMUX_INITIALIZER_UNLOCKED;
static measure_cmd_t s_remote_cmds;

// Runs on the MQTT task: answer right away, then queue the command.
static int on_azure_command(measure_cmd_t cmds, char *reason, size_t cap)
{
    bool lockout = esp_timer_get_time() < BOOT_LOCKOUT_S * US_PER_S;
    bool measuring = s_ecg_active || s_vitals_active || s_temp_active;
    if ((cmds & MEASURE_ALL) && lockout && !(cmds & CMD_FORCE)) {
        snprintf(reason, cap, "warmup"); // web/src/azure-commands.js REASONS
        return 409;
    }
    if ((cmds & CMD_SD_UPLOAD) && !(cmds & MEASURE_ALL) && measuring) {
        snprintf(reason, cap, "measuring");
        return 409;
    }
    portENTER_CRITICAL(&s_remote_lock);
    s_remote_cmds |= cmds;
    portEXIT_CRITICAL(&s_remote_lock);
    snprintf(reason, cap, "ok");
    return 200;
}

static measure_cmd_t take_remote_commands(void)
{
    portENTER_CRITICAL(&s_remote_lock);
    measure_cmd_t c = s_remote_cmds;
    s_remote_cmds = MEASURE_NONE;
    portEXIT_CRITICAL(&s_remote_lock);
    return c;
}

static void sender_loop(void)
{
    int32_t *ecg_batch = malloc(ECG_DRAIN_MAX * sizeof(int32_t));
    bool wifi_ps_on = true;
    bool power_measuring = false;
    bool in_session = false; // measuring (or just finished this tick)
    int idle_ticks = 0; // ticks with nothing measured
    int64_t next_copy_try_us = 0; // retry delay for the SD copy
    TickType_t next_tick = xTaskGetTickCount() + pdMS_TO_TICKS(1000);

    int presses = 0; // presses in the current double-press window
    int64_t first_press_us = 0;

    while (1) {
        // One press = measure all, two quick presses = measure even during warm-up.
        if (presses > 0 && esp_timer_get_time() - first_press_us >= DOUBLE_PRESS_MS * 1000LL) {
            bool force = presses >= 2;
            presses = 0;
            if (esp_timer_get_time() < BOOT_LOCKOUT_S * US_PER_S && !force) {
                ESP_LOGW(TAG, "button pressed during the warm-up - press twice to measure anyway");
            } else {
                ESP_LOGI(TAG, "button %s - measuring everything", force ? "pressed twice" : "pressed");
                apply_command(MEASURE_ALL, esp_timer_get_time());
                update_power_state(&wifi_ps_on, &power_measuring);
            }
        }

        // Wait for the next 1 s tick, or wake early on a button press.
        TickType_t now_ticks = xTaskGetTickCount();
        if ((int32_t)(next_tick - now_ticks) > 0) {
            TickType_t wait = next_tick - now_ticks;
            if ((s_button_pressed || presses > 0) && wait > pdMS_TO_TICKS(20)) {
                wait = pdMS_TO_TICKS(20);
            }
            if (ulTaskNotifyTake(pdTRUE, wait) > 0) {
                if (presses == 0) {
                    first_press_us = esp_timer_get_time();
                }
                presses++;
            }
            button_rearm_pending();
            continue;
        }
        next_tick += pdMS_TO_TICKS(1000);
        if ((int32_t)(xTaskGetTickCount() - next_tick) > 0) {
            next_tick = xTaskGetTickCount() + pdMS_TO_TICKS(1000); // fell behind
        }
        button_rearm_pending();

        static int s_rst_log_tick;
        if (++s_rst_log_tick >= 10) {
            s_rst_log_tick = 0;
            ESP_LOGW(TAG, "uptime %lld s, last reset: %s, free heap %lu, button pin=%d irqs=%lu",
                     esp_timer_get_time() / US_PER_S, reset_reason_str(esp_reset_reason()),
                     (unsigned long)esp_get_free_heap_size(), gpio_get_level(PIN_BUTTON),
                     (unsigned long)s_button_irqs);
        }

        bool boot_lockout = esp_timer_get_time() < BOOT_LOCKOUT_S * US_PER_S;
        close_expired_windows(esp_timer_get_time());
        update_power_state(&wifi_ps_on, &power_measuring);
        update_usb_power();

        int n = 0;
        int32_t v;
        while (n < ECG_DRAIN_MAX && xQueueReceive(s_ecg_queue, &v, 0) == pdTRUE) {
            ecg_batch[n++] = v;
        }

        telemetry_t t = {
            .ecg = ecg_batch,
            .ecg_count = n,
            .lockout_remaining_s = -1,
            .ecg_remaining_s = -1,
            .vitals_remaining_s = -1,
            .temp_remaining_s = -1,
            .lod_mask = -1,
            .adc_sps = -1,
            .sd_files = sd_logger_pending_files(),
        };

        static int s_room_tick;
        if (!s_temp_active && ++s_room_tick >= ROOM_SAMPLE_S) {
            s_room_tick = 0;
            float room;
            if (tmp117_read_celsius(&room) == ESP_OK) {
                s_room_c = (isnan(s_room_c) || room < s_room_c) ? room : fminf(room, s_room_c + ROOM_RISE_C);
            }
        }

        float temp_sample = NAN;
        if (s_temp_active) {
            if (tmp117_read_celsius(&temp_sample) == ESP_OK) {
                if (s_temp_n == TEMP_MAX_SAMPLES) { // buffer full, drop the oldest
                    memmove(s_temp_samples, s_temp_samples + 1, (TEMP_MAX_SAMPLES - 1) * sizeof(float));
                    s_temp_n--;
                }
                s_temp_samples[s_temp_n++] = temp_sample;
            } else {
                temp_sample = NAN;
            }
        }
        if (s_temp_result_pending) {
            s_temp_result_pending = false;
            float skin;
            int settled_after_s;
            if (temp_window_result(&skin, &settled_after_s)) {
                t.temp_c = skin;
                t.temp_ok = true;
                t.temp_body_c = NAN;
                // Body temperature estimate: skin + k * (skin - room), see Kconfig.
                if (!isnan(s_window_room_c)) {
                    float k = CONFIG_MIOT_TEMP_TISSUE_K_X100 / 100.0f;
                    float rise = k * (skin - s_window_room_c);
                    rise = rise < 0 ? 0 : rise > 5.0f ? 5.0f : rise;
                    t.temp_body_c = skin + rise;
                    t.temp_room_c = s_window_room_c;
                }
                ESP_LOGI(TAG, "temperature: skin %.2f C (settled after %d of %d s), room %.2f C -> body ~%.2f C",
                         skin, settled_after_s, s_temp_n, s_window_room_c, t.temp_body_c);
            }
        }

        int hr_sample = 0, spo2_sample = 0;
        bool sample_ok = false;
        if (s_vitals_active && s_vitals_running) {
            max30102_estimate_vitals(&hr_sample, &spo2_sample, &sample_ok);
            if (sample_ok) {
                s_hr_sum += hr_sample;
                s_hr_count++;
                s_spo2_sum += spo2_sample;
                s_spo2_count++;
            }
        }
        if (s_vitals_result_pending) {
            s_vitals_result_pending = false;
            if (s_hr_count > 0) {
                t.hr = (int)lround(s_hr_sum / s_hr_count);
                t.spo2 = (int)lround(s_spo2_sum / s_spo2_count);
                t.vitals_ok = true;
                ESP_LOGI(TAG, "pulse %d bpm, SpO2 %d %% (mean of %d valid seconds)", t.hr, t.spo2, s_hr_count);
            } else {
                ESP_LOGW(TAG, "no valid pulse/SpO2 reading in the whole window (finger on the sensor?)");
            }
        }

        int64_t now_us = esp_timer_get_time();
        bool any_window = s_ecg_active || s_vitals_active || s_temp_active;
        t.status = any_window ? "measuring" : boot_lockout ? "boot_lockout" : "idle";
        if (boot_lockout) {
            t.lockout_remaining_s = remaining_s(BOOT_LOCKOUT_S * US_PER_S, now_us);
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

#ifdef CONFIG_MIOT_CALIBRATION_MODE
        {
            max30102_debug_t d;
            max30102_get_debug(&d);
            printf("CAL,%lld,%.3f,%.0f,%.1f,%.0f,%.1f,%.4f,%.1f,%.2f,%d,%d,%d,%d,%d,%d\n", (long long)(now_us / US_PER_S),
                   temp_sample, d.ir_dc, d.ir_ac, d.red_dc, d.red_ac, d.ratio, d.hr, d.quality,
                   sample_ok ? spo2_sample : 0, d.finger, d.pulse_ok, s_lod_mask, s_adc_sps, s_adc_dups);
        }
#endif

        // Up to 11 chars + comma per sample, plus the other fields.
        size_t cap = (size_t)n * 12 + 512;
        char *json = malloc(cap);
        if (json == NULL) {
            ESP_LOGE(TAG, "OOM building JSON payload (n=%d)", n);
            continue;
        }
        size_t len = build_telemetry_json(json, cap, &t);
        bool measuring_now = s_ecg_active || s_vitals_active || s_temp_active;
        if (measuring_now && !in_session) {
            offline_log_session_start(boot_id());
        }
        // A session lasts until the line that reports the last window closed.
        bool log_this_line = measuring_now || in_session;
        in_session = measuring_now;

        bool delivered = false;
        if (len == 0) {
            ESP_LOGE(TAG, "telemetry JSON didn't fit in %u bytes, batch dropped", (unsigned)cap);
        } else if (wifi_connect_is_up()) {
            // warm-up counts as idle too: once a minute is enough for the countdown
            bool idle = !(s_ecg_active || s_vitals_active || s_temp_active);
            esp_err_t err = telemetry_azure_publish(json, len, idle);
            if (err == ESP_OK) {
                delivered = true;
            } else if (!idle) { // no warning every second while idle
                ESP_LOGW(TAG, "telemetry Azure publish failed: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGW(TAG, "Wi-Fi down, skipping telemetry (ecg_samples=%d)", n);
        }
        // If it didn't get out, keep it for the SD card.
        if (log_this_line && !delivered && len > 0) {
            offline_log_append(json, len);
        }
        free(json);

        // Copy to the SD card once idle (can't use the card while the ADS1293
        // runs). Wait two ticks so the ADS1293 is surely in standby.
        idle_ticks = measuring_now ? 0 : idle_ticks + 1;
        if (idle_ticks >= 2 && offline_log_has_data() && esp_timer_get_time() >= next_copy_try_us) {
            if (!offline_log_copy_to_sd()) {
                next_copy_try_us = esp_timer_get_time() + 30 * US_PER_S;
            }
        }

        measure_cmd_t cmds = take_remote_commands();

        // SD upload: only when idle and the flash buffer is already on the card.
        if (cmds & CMD_SD_UPLOAD) {
            cmds &= ~CMD_SD_UPLOAD;
            if (measuring_now || offline_log_has_data()) {
                ESP_LOGW(TAG, "SD upload requested while measuring / still copying - ignored, press again later");
            } else {
                ESP_LOGI(TAG, "uploading the SD card to the server");
                sd_upload_all(boot_id());
            }
        }
        if (cmds & MEASURE_ALL) {
            if (boot_lockout && !(cmds & CMD_FORCE)) {
                ESP_LOGW(TAG, "measurement requested during the warm-up lockout - ignored");
            } else {
                apply_command(cmds, esp_timer_get_time());
                update_power_state(&wifi_ps_on, &power_measuring);
            }
        }
    }
}

void app_main(void)
{
    // Also printed every 10 s, for when the monitor is attached late.
    ESP_LOGW(TAG, "reset reason: %s", reset_reason_str(esp_reset_reason()));

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    power_init();
    s_ecg_queue = xQueueCreate(ECG_QUEUE_LEN, sizeof(int32_t));

    // Waits for Wi-Fi for a bit, then keeps retrying in the background.
#if !WIFI_DISABLED_FOR_TEST && !defined(CONFIG_MIOT_CALIBRATION_MODE)
    if (wifi_connect_start() != ESP_OK) {
        ESP_LOGW(TAG, "starting without Wi-Fi; will keep retrying in background");
    }
#ifndef CONFIG_MIOT_AZURE_ENABLE
    // No RTC, so sync time over SNTP for timestamps on the SD card
    // (the Azure module does its own SNTP).
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_cfg);
#endif
#else
    ESP_LOGW(TAG, "Wi-Fi intentionally disabled for noise-source test - no network, watch serial log");
#endif

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

    // SPI shared by the SD card and the ADS1293 (separate MISO, see pins.h).
    // GPIO matrix pins, needed for the MISO switch in the ADS1293 driver.
    spi_bus_config_t spi_bus_cfg = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_ID, &spi_bus_cfg, SPI_DMA_CH_AUTO));
    gpio_pullup_en(PIN_SD_MISO); // SD needs pull-ups

    // ADS1293 must be added before the SD card is mounted, the other way
    // round the card stopped answering. SD CS goes high first.
    gpio_set_level(PIN_SD_CS, 1);
    gpio_set_direction(PIN_SD_CS, GPIO_MODE_OUTPUT);
    ESP_ERROR_CHECK(ads1293_init(SPI_HOST_ID, PIN_ADS1293_CS, PIN_ADS1293_MISO, PIN_SD_MISO));
    sd_logger_init(SPI_HOST_ID, PIN_SD_CS);
    offline_log_init();
    tmp117_init(i2c_bus);
    max30102_init(i2c_bus);

    telemetry_azure_set_command_handler(on_azure_command);
    if (telemetry_azure_init() != ESP_OK) {
        ESP_LOGW(TAG, "Azure IoT Hub telemetry init failed; continuing without it");
    }

#if WIFI_DISABLED_FOR_TEST
    // No network: keep ECG on all the time for the noise test.
    s_ecg_deadline_us = INT64_MAX;
    s_ecg_active = true;
#endif
#ifdef CONFIG_MIOT_CALIBRATION_MODE
    ESP_LOGW(TAG, "CALIBRATION MODE: all sensors always on, raw values on serial");
    s_ecg_deadline_us = s_vitals_deadline_us = s_temp_deadline_us = INT64_MAX;
    s_ecg_active = s_vitals_active = s_temp_active = true;
#endif

    xTaskCreate(ecg_task, "ecg_task", 4096, NULL, 10, NULL);
    xTaskCreate(ppg_task, "ppg_task", 4096, NULL, 5, NULL);

    s_sender_task = xTaskGetCurrentTaskHandle();
    button_init();

    ESP_LOGI(TAG, "init done, entering send loop (sensors off until a measurement is requested)");
    sender_loop(); // never returns
}
