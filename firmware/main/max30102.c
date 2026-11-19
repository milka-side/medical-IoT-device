#include "max30102.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
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
#define REG_LED1_PA           0x0C
#define REG_LED2_PA           0x0D
#define REG_PART_ID           0xFF

#define LED_PA_DEFAULT        0x24 // ~7 mA

#define SAMPLE_RATE_HZ        100
// Pulse rate and SpO2 come from the last 8 s: long enough for several beats
// even at 40 bpm, short enough to follow real changes.
#define WINDOW_SAMPLES        (SAMPLE_RATE_HZ * 8)
#define RING_SIZE             1024

// Per-sample band-pass that turns the raw PPG into its pulsatile part:
// an EMA baseline (~0.5 Hz) removes DC and slow drift, an EMA low-pass
// (~6 Hz) removes sample noise. alpha = 1 - exp(-2*pi*fc/fs).
#define BASELINE_ALPHA        0.0309
#define LOWPASS_ALPHA         0.3

// Autocorrelation search range, in samples (35..180 bpm).
#define HR_MIN_BPM            35
#define HR_MAX_BPM            180
#define LAG_MIN               (SAMPLE_RATE_HZ * 60 / HR_MAX_BPM)
#define LAG_MAX               (SAMPLE_RATE_HZ * 60 / HR_MIN_BPM)
#define N_LAGS                (LAG_MAX - LAG_MIN + 1)

// Normalised autocorrelation needed to trust the window. Measured on a real
// fingertip recording: a clean pulse gave 0.62-0.81, movement / pressing
// too hard / a lifted finger stayed below ~0.45.
#define MIN_PULSE_QUALITY     0.5

// IR DC level below this = nothing on the sensor (ambient floor).
#define FINGER_DC_MIN         20000.0

static const char *TAG = "max30102";
static i2c_master_dev_handle_t s_dev;

// s_lock protects the rings and filter state: written by max30102_poll()
// (ppg_task), read by max30102_estimate_vitals() (sender task).
static SemaphoreHandle_t s_lock;

static uint32_t s_ir_ring[RING_SIZE];
static uint32_t s_red_ring[RING_SIZE];
static int32_t s_ir_ac_ring[RING_SIZE];  // band-passed, pulsatile part only
static int32_t s_red_ac_ring[RING_SIZE];
static size_t s_ring_head;
static size_t s_ring_count;

typedef struct {
    double base, lp;
    bool init;
} bandpass_t;

static bandpass_t s_ir_bp, s_red_bp;

static esp_err_t reg_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 1000 / portTICK_PERIOD_MS);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, 1000 / portTICK_PERIOD_MS);
}

// Must be called with s_lock held (or before any other task can run).
static void reset_buffers(void)
{
    s_ring_head = 0;
    s_ring_count = 0;
    s_ir_bp.init = false;
    s_red_bp.init = false;
}

esp_err_t max30102_init(i2c_master_bus_handle_t bus)
{
    // Created first, before any possible early return below: estimate_vitals()
    // is called once a second regardless and takes s_lock first - a failed
    // init must not leave it as xSemaphoreTake(NULL, ...).
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

    // SMP_AVE must stay 1: the chip *decimates* by the averaging factor, so
    // avg=4 at 100 sps put only 25 samples/s into the FIFO while everything
    // here assumes SAMPLE_RATE_HZ=100 (HR read ~2x too high). LED pulse rate
    // is set by SPO2_SR, not averaging, so this costs no extra LED current.
    reg_write(REG_FIFO_CONFIG, 0x1F);  // sample avg=1, rollover enabled, A_FULL=15
    reg_write(REG_SPO2_CONFIG, 0x27);  // ADC range 4096nA, 100 sps, pulse width 411us (18-bit)
    reg_write(REG_LED1_PA, LED_PA_DEFAULT);
    reg_write(REG_LED2_PA, LED_PA_DEFAULT);
    reg_write(REG_INTR_ENABLE_1, 0x00);
    reg_write(REG_INTR_ENABLE_2, 0x00);
    // SPO2_CONFIG/LED_PA persist across shutdown - leave the sensor off
    // (<1 uA) until max30102_start() wakes it.
    reg_write(REG_MODE_CONFIG, 0x80);  // SHDN

    reset_buffers();
    ESP_LOGI(TAG, "MAX30102 initialised (shut down)");
    return ESP_OK;
}

esp_err_t max30102_start(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    reset_buffers();
    xSemaphoreGive(s_lock);

    reg_write(REG_FIFO_WR_PTR, 0x00);
    reg_write(REG_OVF_COUNTER, 0x00);
    reg_write(REG_FIFO_RD_PTR, 0x00);
    return reg_write(REG_MODE_CONFIG, 0x03); // wake, SpO2 mode (both LEDs)
}

esp_err_t max30102_stop(void)
{
    return reg_write(REG_MODE_CONFIG, 0x80); // SHDN, <1 uA
}

static int32_t bandpass_step(bandpass_t *bp, uint32_t x)
{
    if (!bp->init) {
        bp->base = bp->lp = (double)x;
        bp->init = true;
    }
    bp->base += ((double)x - bp->base) * BASELINE_ALPHA;
    bp->lp += ((double)x - bp->lp) * LOWPASS_ALPHA;
    return (int32_t)lround(bp->lp - bp->base);
}

// Must be called with s_lock held.
static void ring_push(uint32_t ir, uint32_t red)
{
    s_ir_ring[s_ring_head] = ir;
    s_red_ring[s_ring_head] = red;
    s_ir_ac_ring[s_ring_head] = bandpass_step(&s_ir_bp, ir);
    s_red_ac_ring[s_ring_head] = bandpass_step(&s_red_bp, red);
    s_ring_head = (s_ring_head + 1) % RING_SIZE;
    if (s_ring_count < RING_SIZE) {
        s_ring_count++;
    }
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

    for (int i = 0; i < available; i++) {
        uint8_t reg = REG_FIFO_DATA;
        uint8_t raw[6] = {0};
        err = i2c_master_transmit_receive(s_dev, &reg, 1, raw, sizeof(raw), 1000 / portTICK_PERIOD_MS);
        if (err != ESP_OK) {
            return err;
        }
        uint32_t ch1 = (((uint32_t)raw[0] & 0x03) << 16) | ((uint32_t)raw[1] << 8) | raw[2];
        uint32_t ch2 = (((uint32_t)raw[3] & 0x03) << 16) | ((uint32_t)raw[4] << 8) | raw[5];
#ifdef CONFIG_MIOT_MAX30102_LEDS_SWAPPED
        // Some MAX30102 boards have the red and IR LEDs fitted the other way round.
        uint32_t ir = ch1, red = ch2;
#else
        uint32_t red = ch1, ir = ch2; // datasheet order: LED1 = red, LED2 = IR
#endif

        xSemaphoreTake(s_lock, portMAX_DELAY);
        ring_push(ir, red);
        xSemaphoreGive(s_lock);
#ifdef CONFIG_MIOT_CALIBRATION_MODE
        printf("PPG,%lu,%lu\n", (unsigned long)ir, (unsigned long)red); // raw 100 Hz for offline analysis
#endif
    }

    return ESP_OK;
}

// Written/read only by the caller of estimate_vitals (the sender task).
static max30102_debug_t s_debug;
static int32_t s_win_ir[WINDOW_SAMPLES], s_win_red[WINDOW_SAMPLES];

void max30102_get_debug(max30102_debug_t *out)
{
    *out = s_debug;
}

static double rms_about_mean(const int32_t *x, int n)
{
    double mean = 0;
    for (int i = 0; i < n; i++) {
        mean += x[i];
    }
    mean /= n;
    double ss = 0;
    for (int i = 0; i < n; i++) {
        double d = x[i] - mean;
        ss += d * d;
    }
    return sqrt(ss / n);
}

// Pulse period by autocorrelation over the whole window. Unlike detecting
// individual beats, this isn't thrown off by the secondary (dicrotic) wave
// or by noise near the crossing point - on a real low-perfusion recording
// the per-beat detector paired up intervals like 45+91 samples (= two real
// beats) and read ~15% high. Returns bpm, quality in *q (0..1).
static double autocorr_hr(int32_t *x, int n, double *q)
{
    int64_t sum = 0;
    for (int i = 0; i < n; i++) {
        sum += x[i];
    }
    int32_t mean = (int32_t)(sum / n);
    int64_t energy = 0;
    for (int i = 0; i < n; i++) {
        x[i] -= mean;
        energy += (int64_t)x[i] * x[i];
    }
    *q = 0;
    if (energy == 0) {
        return 0;
    }

    static double ac[N_LAGS];
    int best = 0;
    for (int k = 0; k < N_LAGS; k++) {
        int lag = LAG_MIN + k;
        int64_t c = 0;
        for (int i = 0; i + lag < n; i++) {
            c += (int64_t)x[i] * x[i + lag];
        }
        ac[k] = (double)c / (double)energy;
        if (ac[k] > ac[best]) {
            best = k;
        }
    }
    // Prefer the first strong local maximum: the true period, not a 2x
    // multiple of it that happens to correlate slightly better.
    for (int k = 1; k < N_LAGS - 1; k++) {
        if (ac[k] >= ac[k - 1] && ac[k] >= ac[k + 1] && ac[k] >= 0.8 * ac[best]) {
            best = k;
            break;
        }
    }
    if (best == 0 || best == N_LAGS - 1) {
        return 0; // peak at the search edge = no real period in range
    }
    *q = ac[best];
    double a = ac[best - 1], b = ac[best], c = ac[best + 1];
    double denom = a - 2 * b + c;
    double lag = LAG_MIN + best + (denom != 0 ? (a - c) / (2 * denom) : 0); // sub-sample peak
    return 60.0 * SAMPLE_RATE_HZ / lag;
}

esp_err_t max30102_estimate_vitals(int *out_hr_bpm, int *out_spo2_pct, bool *out_valid)
{
    *out_valid = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ring_count < WINDOW_SAMPLES) {
        xSemaphoreGive(s_lock);
        return ESP_OK; // not enough data yet
    }
    size_t start = (s_ring_head + RING_SIZE - WINDOW_SAMPLES) % RING_SIZE;
    uint64_t ir_sum = 0, red_sum = 0;
    for (int i = 0; i < WINDOW_SAMPLES; i++) {
        size_t idx = (start + i) % RING_SIZE;
        ir_sum += s_ir_ring[idx];
        red_sum += s_red_ring[idx];
        s_win_ir[i] = s_ir_ac_ring[idx];
        s_win_red[i] = s_red_ac_ring[idx];
    }
    xSemaphoreGive(s_lock);

    double ir_dc = (double)ir_sum / WINDOW_SAMPLES;
    double red_dc = (double)red_sum / WINDOW_SAMPLES;
    // RMS of the pulsatile part rather than max-min: a single motion spike
    // no longer dominates the amplitude, and with it the SpO2 ratio.
    double ir_ac = rms_about_mean(s_win_ir, WINDOW_SAMPLES);
    double red_ac = rms_about_mean(s_win_red, WINDOW_SAMPLES);
    double quality;
    double hr = autocorr_hr(s_win_ir, WINDOW_SAMPLES, &quality);

    bool finger = ir_dc >= FINGER_DC_MIN;
    bool pulse_ok = finger && quality >= MIN_PULSE_QUALITY && hr >= HR_MIN_BPM && hr <= HR_MAX_BPM;
    s_debug = (max30102_debug_t){
        .ir_dc = ir_dc,
        .ir_ac = ir_ac,
        .red_dc = red_dc,
        .red_ac = red_ac,
        .ratio = (ir_ac > 0 && red_dc > 0) ? (red_ac / red_dc) / (ir_ac / ir_dc) : 0,
        .hr = hr,
        .quality = quality,
        .finger = finger,
        .pulse_ok = pulse_ok,
    };
    if (!pulse_ok) {
        return ESP_OK; // no finger, movement, or no clear pulse - report nothing rather than noise
    }

    // --- SpO2: ratio-of-ratios, linear curve SpO2 = A - B*R ---
    // No R-curve ships with the MAX30102; A/B are set in menuconfig from
    // calibration (default: the common rough 110 - 25*R).
    double spo2 = CONFIG_MIOT_SPO2_COEF_A_X100 / 100.0 - (CONFIG_MIOT_SPO2_COEF_B_X100 / 100.0) * s_debug.ratio;
    if (spo2 > 100.0) spo2 = 100.0;
    if (spo2 < 0.0) spo2 = 0.0;

    *out_hr_bpm = (int)lround(hr);
    *out_spo2_pct = (int)lround(spo2);
    *out_valid = true;
    return ESP_OK;
}
