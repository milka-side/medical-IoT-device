#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"

// Driver for the TI ADS1293 3-channel ECG analog front end (single-lead use:
// channel 1 only). Shares an SPI bus with the SD card; each device gets its
// own spi_device_handle_t/CS.

// One-time hardware setup only (SPI device, DRDY GPIO/ISR, register
// programming). Leaves the chip in standby - call
// ads1293_start_conversion() to actually start sampling.
// miso_gpio is the ADS1293 SDO pin; if it differs from bus_miso_gpio (the
// MISO the bus was initialised with, i.e. the SD card's), the bus's MISO
// input is switched to miso_gpio for each ADS1293 transaction only.
esp_err_t ads1293_init(spi_host_device_t host, int cs_gpio, int miso_gpio, int bus_miso_gpio);

// Starts conversion (START_CON) / returns to standby without re-touching
// SPI/GPIO setup, so a measurement window can power the AFE up and back down
// without redoing the one-time init. Resets the filter chain's internal
// state on start, so each fresh window begins from a clean baseline
// instead of resuming with a stale DC estimate from before the AFE was
// powered down.
esp_err_t ads1293_start_conversion(void);
esp_err_t ads1293_stop_conversion(void);

// Lead-off bitmask, bit0 = RA (IN3), bit1 = LA (IN2): a set bit means that
// electrode has no skin contact. Only RA/LA have lead-off detection enabled.
esp_err_t ads1293_read_lead_off(uint8_t *out_mask);

// Number of reads since the last call that returned exactly the previous
// sample (a sign of DRDY firing without a new conversion). Resets on read.
uint32_t ads1293_take_duplicate_count(void);

// All functions in this driver share one SPI device handle, which the
// ESP-IDF SPI master driver does not allow two tasks to use at once - call
// them all from a single task (main.c's ecg_task).

// Blocks (with timeout) until DRDY goes low, then reads channel 1 ECG and
// converts it to a signed sample. Returns ESP_ERR_TIMEOUT if no new sample
// arrived in time (including while conversions are stopped), and
// ESP_ERR_INVALID_STATE if DRDY fired but the data was the previous sample
// again (spurious edge) - the caller should just try again.
esp_err_t ads1293_read_sample(int32_t *out_sample, uint32_t timeout_ms);
