#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"

// TI ADS1293 ECG front end, channel 1 only. Shares the SPI bus with the SD card.

// One-time setup, leaves the chip in standby. If miso_gpio differs from
// bus_miso_gpio, MISO is switched to it for each ADS1293 transaction.
esp_err_t ads1293_init(spi_host_device_t host, int cs_gpio, int miso_gpio, int bus_miso_gpio);

// Start / stop conversions. Start also resets the filters.
esp_err_t ads1293_start_conversion(void);
esp_err_t ads1293_stop_conversion(void);

// Lead-off bits: bit0 = RA, bit1 = LA, set = no contact.
esp_err_t ads1293_read_lead_off(uint8_t *out_mask);

// Repeated reads since the last call (spurious DRDY). Resets on read.
uint32_t ads1293_take_duplicate_count(void);

// Call all of these from one task (ecg_task), the SPI handle isn't shared.

// Waits for DRDY and reads one filtered sample. ESP_ERR_TIMEOUT if nothing
// came, ESP_ERR_INVALID_STATE if it was the old sample again (just retry).
esp_err_t ads1293_read_sample(int32_t *out_sample, uint32_t timeout_ms);
