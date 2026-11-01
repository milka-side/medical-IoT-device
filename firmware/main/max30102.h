#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t max30102_init(i2c_master_bus_handle_t bus);

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
