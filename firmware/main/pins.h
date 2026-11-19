#pragma once

// Pins (XIAO ESP32-C6, D0..D10). CS/MISO match the real board, they're
// swapped on the schematic.
// SPI2 is shared by the ADS1293 and the SD card, with separate MISO lines
// because the SD module drives MISO even when deselected.
#define PIN_SPI_SCK        19  // D8
#define PIN_SPI_MOSI       18  // D10
#define PIN_ADS1293_MISO   20 // D9 - ADS1293 SDO
#define PIN_ADS1293_CS     17 // D7 - ADS1293 CSB
#define PIN_ADS1293_DRDY   16 // D6 - ADS1293 DRDYB
#define PIN_SD_MISO        2   // D2 - SD card MISO
#define PIN_SD_CS          21  // D3 - SD card CS

// Measure button: 0 at rest, 1 when pressed (internal pull-down).
#define PIN_BUTTON         1   // D1

// I2C: TMP117 + MAX30102
#define PIN_I2C_SDA        22  // D4
#define PIN_I2C_SCL        23  // D5

// On-board RF switch: GPIO3 powers it (active low), GPIO14 picks the
// antenna (low = built-in). ESP-IDF leaves them floating.
#define PIN_RF_SWITCH_EN   3
#define PIN_RF_ANT_SELECT  14

#define SPI_HOST_ID        SPI2_HOST
#define I2C_PORT_NUM        0
#define I2C_CLOCK_HZ        400000
