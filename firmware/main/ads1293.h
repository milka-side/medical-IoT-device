#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"

// Driver for the TI ADS1293 3-channel ECG analog front end (single-lead use:
// channel 1 only). Shares an SPI bus with the SD card; each device gets its
// own spi_device_handle_t/CS.

esp_err_t ads1293_init(spi_host_device_t host, int cs_gpio);

// Blocks (with timeout) until DRDY goes low, then reads channel 1 ECG and
// converts it to a signed sample. Returns ESP_ERR_TIMEOUT if no new sample
// arrived in time.
esp_err_t ads1293_read_sample(int32_t *out_sample, uint32_t timeout_ms);
