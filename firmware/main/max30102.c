#include "max30102.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MAX30102_I2C_ADDR     0x57

#define REG_INTR_STATUS_1     0x00
#define REG_INTR_ENABLE_1     0x02
#define REG_INTR_ENABLE_2     0x03
#define REG_FIFO_WR_PTR       0x04
#define REG_OVF_COUNTER       0x05
#define REG_FIFO_RD_PTR       0x06
#define REG_FIFO_DATA         0x07
#define REG_FIFO_CONFIG       0x08
#define REG_MODE_CONFIG       0x09
#define REG_SPO2_CONFIG       0x0A
#define REG_LED1_PA           0x0C // RED
#define REG_LED2_PA           0x0D // IR
#define REG_PART_ID           0xFF

#define SAMPLE_RATE_HZ         100
#define WINDOW_SAMPLES         (SAMPLE_RATE_HZ * 1) // ~1 s window for DC/AC amplitude + SpO2
#define RING_SIZE               256

// Beat detector: a slow EMA baseline tracks the IR DC level (cutoff ~0.5 Hz,
// alpha = 1 - exp(-2*pi*fc/fs)), well below the ~1-3 Hz heart-rate band, so
// it follows baseline wander/finger pressure drift but not the pulse itself.
// A beat is a rising zero-crossing of (ir - baseline). Instead of counting
// crossings inside a fixed 1 s window (which only lets HR come out as exact
// multiples of 60/window_seconds - with a 1 s window that means ONLY 0, 60,
// 120 or 180 bpm are representable, so a real 72-85 bpm resting rate would
// just alternate between the two nearest multiples of 60 every second),
// each beat's actual sample-to-sample interval is converted straight to an
// instantaneous bpm and smoothed with its own EMA - this gives normal,
// continuous bpm resolution.
#define IR_BASELINE_ALPHA 0.0309
#define HR_SMOOTH_ALPHA    0.25
#define HR_MIN_BPM         30
#define HR_MAX_BPM         220
// Refractory period between accepted beats, in samples - guards against
// double-counting noise around a single crossing.
#define MIN_BEAT_INTERVAL_SAMPLES (SAMPLE_RATE_HZ * 60 / HR_MAX_BPM)
// If no beat has been detected this recently, treat HR as stale (finger
// lifted, lead fell off the pulse, etc.) rather than reporting an old value.
#define HR_STALE_SAMPLES   (SAMPLE_RATE_HZ * 3)

static const char *TAG = "max30102";
static i2c_master_dev_handle_t s_dev;

// s_lock protects every field below: s_ir_ring/s_red_ring/s_ring_head/
// s_ring_count are written by max30102_poll() (called from ppg_task) and
// read by max30102_estimate_vitals() (called from the sender task) - two
// different FreeRTOS tasks with no other synchronization between them.
static SemaphoreHandle_t s_lock;

static uint32_t s_ir_ring[RING_SIZE];
static uint32_t s_red_ring[RING_SIZE];
static size_t s_ring_head;
static size_t s_ring_count;

static double s_ir_baseline;
static bool s_ir_baseline_init;
static double s_prev_ac;
static bool s_prev_ac_valid;
static int64_t s_sample_counter;
static int64_t s_last_beat_sample = -1;
static double s_hr_ema;
static bool s_hr_ema_init;

static esp_err_t reg_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 1000 / portTICK_PERIOD_MS);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, 1000 / portTICK_PERIOD_MS);
}

esp_err_t max30102_init(i2c_master_bus_handle_t bus)
{
    // Created first, before any possible early return below: poll() bails
    // out gracefully on its own I2C errors without ever touching s_lock,
    // but estimate_vitals() is called unconditionally once a second from
    // the sender task and takes s_lock as its very first step - if init
    // failed before the mutex existed, that would be
    // xSemaphoreTake(NULL, ...), undefined behavior in FreeRTOS.
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed");
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MAX30102_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t part_id = 0;
    reg_read(REG_PART_ID, &part_id);
    ESP_LOGI(TAG, "part id = 0x%02X (expect 0x15)", part_id);

    reg_write(REG_MODE_CONFIG, 0x40); // reset
    vTaskDelay(pdMS_TO_TICKS(100));

    reg_write(REG_FIFO_WR_PTR, 0x00);
    reg_write(REG_OVF_COUNTER, 0x00);
    reg_write(REG_FIFO_RD_PTR, 0x00);

    reg_write(REG_FIFO_CONFIG, 0x5F);  // sample avg=4, rollover enabled
    reg_write(REG_MODE_CONFIG, 0x03);  // SpO2 mode (RED + IR)
    reg_write(REG_SPO2_CONFIG, 0x27);  // ADC range 4096nA, 100 sps, pulse width 411us (18-bit)
    reg_write(REG_LED1_PA, 0x24);      // RED ~7mA
    reg_write(REG_LED2_PA, 0x24);      // IR ~7mA
    reg_write(REG_INTR_ENABLE_1, 0x00);
    reg_write(REG_INTR_ENABLE_2, 0x00);

    s_ring_head = 0;
    s_ring_count = 0;
    s_ir_baseline_init = false;
    s_prev_ac_valid = false;
    s_sample_counter = 0;
    s_last_beat_sample = -1;
    s_hr_ema_init = false;

    ESP_LOGI(TAG, "MAX30102 initialised");
    return ESP_OK;
}

// Must be called with s_lock held.
static void update_beat_detector(uint32_t ir)
{
    double ir_d = (double)ir;
    if (!s_ir_baseline_init) {
        s_ir_baseline = ir_d;
        s_ir_baseline_init = true;
        s_prev_ac_valid = false;
    } else {
        s_ir_baseline += (ir_d - s_ir_baseline) * IR_BASELINE_ALPHA;
    }

    double ac = ir_d - s_ir_baseline;
    bool rising_cross = s_prev_ac_valid && s_prev_ac <= 0.0 && ac > 0.0;
    s_prev_ac = ac;
    s_prev_ac_valid = true;

    int64_t sample_idx = s_sample_counter++;

    if (!rising_cross) {
        return;
    }
    if (s_last_beat_sample >= 0 && (sample_idx - s_last_beat_sample) < MIN_BEAT_INTERVAL_SAMPLES) {
        return; // too soon to be a real new beat - refractory period
    }

    if (s_last_beat_sample >= 0) {
        int64_t interval = sample_idx - s_last_beat_sample;
        double instant_bpm = 60.0 * SAMPLE_RATE_HZ / (double)interval;
        if (instant_bpm >= HR_MIN_BPM && instant_bpm <= HR_MAX_BPM) {
            if (!s_hr_ema_init) {
                s_hr_ema = instant_bpm;
                s_hr_ema_init = true;
            } else {
                s_hr_ema += (instant_bpm - s_hr_ema) * HR_SMOOTH_ALPHA;
            }
        }
    }
    s_last_beat_sample = sample_idx;
}

// Must be called with s_lock held.
static void ring_push(uint32_t ir, uint32_t red)
{
    s_ir_ring[s_ring_head] = ir;
    s_red_ring[s_ring_head] = red;
    s_ring_head = (s_ring_head + 1) % RING_SIZE;
    if (s_ring_count < RING_SIZE) {
        s_ring_count++;
    }
    update_beat_detector(ir);
}

esp_err_t max30102_poll(void)
{
    uint8_t wr_ptr = 0, rd_ptr = 0;
    esp_err_t err = reg_read(REG_FIFO_WR_PTR, &wr_ptr);
    if (err != ESP_OK) {
        return err;
    }
    err = reg_read(REG_FIFO_RD_PTR, &rd_ptr);
    if (err != ESP_OK) {
        return err;
    }

    int available = (int)wr_ptr - (int)rd_ptr;
    if (available < 0) {
        available += 32; // FIFO is 32 samples deep, pointers wrap at 5 bits
    }
    if (available == 0) {
        return ESP_OK;
    }

    for (int i = 0; i < available; i++) {
        uint8_t reg = REG_FIFO_DATA;
        uint8_t raw[6] = {0};
        err = i2c_master_transmit_receive(s_dev, &reg, 1, raw, sizeof(raw), 1000 / portTICK_PERIOD_MS);
        if (err != ESP_OK) {
            return err;
        }
        uint32_t red = (((uint32_t)raw[0] & 0x03) << 16) | ((uint32_t)raw[1] << 8) | raw[2];
        uint32_t ir = (((uint32_t)raw[3] & 0x03) << 16) | ((uint32_t)raw[4] << 8) | raw[5];

        xSemaphoreTake(s_lock, portMAX_DELAY);
        ring_push(ir, red);
        xSemaphoreGive(s_lock);
    }

    return ESP_OK;
}

esp_err_t max30102_estimate_vitals(int *out_hr_bpm, int *out_spo2_pct, bool *out_valid)
{
    *out_valid = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_ring_count < WINDOW_SAMPLES) {
        xSemaphoreGive(s_lock);
        return ESP_OK; // not enough data yet
    }

    // Walk the last WINDOW_SAMPLES entries of the ring buffer.
    size_t start = (s_ring_head + RING_SIZE - WINDOW_SAMPLES) % RING_SIZE;

    uint32_t ir_min = UINT32_MAX, ir_max = 0, red_min = UINT32_MAX, red_max = 0;
    uint64_t ir_sum = 0, red_sum = 0;

    for (int i = 0; i < WINDOW_SAMPLES; i++) {
        size_t idx = (start + i) % RING_SIZE;
        uint32_t ir = s_ir_ring[idx];
        uint32_t red = s_red_ring[idx];

        if (ir < ir_min) ir_min = ir;
        if (ir > ir_max) ir_max = ir;
        if (red < red_min) red_min = red;
        if (red > red_max) red_max = red;
        ir_sum += ir;
        red_sum += red;
    }

    double hr_ema = s_hr_ema;
    bool have_recent_beat = s_hr_ema_init && s_last_beat_sample >= 0 &&
                             (s_sample_counter - s_last_beat_sample) <= HR_STALE_SAMPLES;

    xSemaphoreGive(s_lock);

    double ir_dc = (double)ir_sum / WINDOW_SAMPLES;
    double red_dc = (double)red_sum / WINDOW_SAMPLES;

    // No finger on the sensor: IR DC level stays near the ambient floor.
    if (ir_dc < 20000.0) {
        return ESP_OK;
    }

    double ir_ac = (double)(ir_max - ir_min);
    double red_ac = (double)(red_max - red_min);
    if (ir_ac < 50.0) {
        return ESP_OK; // signal too flat to be a pulse
    }

    if (!have_recent_beat) {
        return ESP_OK; // no beat detected recently - stale/no pulse
    }

    int hr_bpm = (int)lround(hr_ema);
    if (hr_bpm < HR_MIN_BPM || hr_bpm > HR_MAX_BPM) {
        return ESP_OK; // implausible, skip rather than report nonsense
    }

    // --- SpO2: simplified ratio-of-ratios ---
    double r_ratio = (red_ac / red_dc) / (ir_ac / ir_dc);
    double spo2 = 110.0 - 25.0 * r_ratio; // common rough empirical approximation
    if (spo2 > 100.0) spo2 = 100.0;
    if (spo2 < 0.0) spo2 = 0.0;

    *out_hr_bpm = hr_bpm;
    *out_spo2_pct = (int)lround(spo2);
    *out_valid = true;
    return ESP_OK;
}
