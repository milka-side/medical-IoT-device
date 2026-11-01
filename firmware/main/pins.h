#pragma once

// Pin mapping. SCK/MOSI/DRDY read off the KiCad schematic for the
// "ESP32_C6 speed studio" board (Seeed XIAO ESP32-C6 pin names D0..D10).
// MISO/CS corrected to match the actual physical wiring on the real board
// (CSB->D7, SDO->D9 - swapped vs. what the schematic drawing suggested).
//
// SPI bus for the ADS1293 (ECG). The microSD card is wired onto the same
// SCK/MOSI/MISO with its own CS (D3 / GPIO21) but isn't used by this build -
// only sensor data -> server for now.
#define PIN_SPI_SCK        19  // D8
#define PIN_SPI_MOSI       18  // D10
#define PIN_SPI_MISO       20  // D9 - ADS1293 SDO
#define PIN_ADS1293_CS     17  // D7 - ADS1293 CSB
#define PIN_ADS1293_DRDY   16  // D6 - ADS1293 DRDB (data ready, active low)

// Shared I2C bus: TMP117 + MAX30102
#define PIN_I2C_SDA        22  // D4
#define PIN_I2C_SCL        23  // D5

#define SPI_HOST_ID        SPI2_HOST
#define I2C_PORT_NUM        0
#define I2C_CLOCK_HZ        400000
