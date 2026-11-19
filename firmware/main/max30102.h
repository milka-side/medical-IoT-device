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

// Pulse rate (autocorrelation of the band-passed IR signal) and SpO2
// (ratio-of-ratios of RMS pulsatile amplitudes, linear curve from
// menuconfig) over the last 8 s. Not a medical device. *out_valid is false
// when there is no finger, the finger is moving, or the pulse isn't clear
// enough to trust - nothing is reported rather than a noisy guess.
esp_err_t max30102_estimate_vitals(int *out_hr_bpm, int *out_spo2_pct, bool *out_valid);

// Raw quantities behind the last max30102_estimate_vitals() call, for
// calibration logging. ir_ac/red_ac are RMS pulsatile amplitudes, ratio
// is the SpO2 ratio-of-ratios R (0 if unknown).
typedef struct {
    double ir_dc, ir_ac, red_dc, red_ac;
    double ratio;
    double hr;      // autocorrelation estimate, even if not trusted
    double quality; // normalised autocorrelation peak, 0..1
    bool finger;    // IR DC level high enough to mean a finger is on the sensor
    bool pulse_ok;  // finger + clear periodic pulse: hr/SpO2 are reported
} max30102_debug_t;

void max30102_get_debug(max30102_debug_t *out);
