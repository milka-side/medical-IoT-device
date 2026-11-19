#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

// One-time setup, leaves the sensor in shutdown.
esp_err_t max30102_init(i2c_master_bus_handle_t bus);

// Wake up in SpO2 mode / back to shutdown (<1 uA). start() also clears
// the buffers.
esp_err_t max30102_start(void);
esp_err_t max30102_stop(void);

// Moves the FIFO into the ring buffer. Call every 20-50 ms.
esp_err_t max30102_poll(void);

// HR (autocorrelation of IR) and SpO2 over the last 8 s. *out_valid is
// false without a finger, with movement or with no clear pulse.
esp_err_t max30102_estimate_vitals(int *out_hr_bpm, int *out_spo2_pct, bool *out_valid);

// LED1/LED2 current (~0.2 mA per LSB), for calibration mode.
esp_err_t max30102_set_led_currents(uint8_t led1_pa, uint8_t led2_pa);

// Raw values behind the last estimate, for calibration logs.
typedef struct {
    double ir_dc, ir_ac, red_dc, red_ac;
    double ratio;
    double hr; // even if not trusted
    double quality; // autocorrelation peak, 0..1
    bool finger; // finger on the sensor
    bool pulse_ok; // finger + clear pulse, values reported
} max30102_debug_t;

void max30102_get_debug(max30102_debug_t *out);
