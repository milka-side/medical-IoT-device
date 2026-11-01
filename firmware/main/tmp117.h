#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t tmp117_init(i2c_master_bus_handle_t bus);

// Reads the latest temperature conversion in degrees Celsius.
esp_err_t tmp117_read_celsius(float *out_celsius);
