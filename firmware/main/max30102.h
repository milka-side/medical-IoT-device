#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

// One-time I2C/reset/FIFO setup. Leaves the sensor in shutdown (<1 uA) -
// call max30102_start() to actually turn the LEDs/ADC on.
esp_err_t max30102_init(i2c_master_bus_handle_t bus);

// Wakes the sensor into SpO2 mode (LEDs+ADC on) / puts it back into the
// documented MAX3010x full-shutdown mode (MODE_CONFIG SHDN bit, <1 uA).
// start() also clears the ring buffer and beat-detector state, same reason
// as ads1293_start_conversion(): a fresh window shouldn't resume with a
// stale baseline from before the LEDs were off.
esp_err_t max30102_start(void);
esp_err_t max30102_stop(void);

// Drains the sensor's FIFO into an internal ring buffer. Call this often
// (e.g. every 20-50 ms) from the sampling loop.
esp_err_t max30102_poll(void);

// Computes HR from actual inter-beat (RR) intervals, smoothed with an EMA,
// and SpO2 from a ratio-of-ratios estimate over the most recent ~1 s of
// buffered samples. This is a lightweight, uncalibrated approximation (not
// the proprietary Maxim algorithm) - good enough for a student demo, not
// for medical use. Returns false via *out_valid when there isn't enough
// signal or no beat has been detected recently (e.g. no finger on the
// sensor).
esp_err_t max30102_estimate_vitals(int *out_hr_bpm, int *out_spo2_pct, bool *out_valid);
