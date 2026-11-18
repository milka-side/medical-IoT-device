#pragma once

// Pin mapping. SCK/MOSI/DRDY read off the KiCad schematic for the
// "ESP32_C6 speed studio" board (Seeed XIAO ESP32-C6 pin names D0..D10).
// MISO/CS corrected to match the actual physical wiring on the real board
// (CSB->D7, SDO->D9 - swapped vs. what the schematic drawing suggested).
//
// One SPI bus (SPI2, the only general-purpose one on the C6) shared by the
// ADS1293 and the microSD module: common SCK/MOSI, separate CS - and
// separate MISO lines, because the SD module's buffer keeps driving its MISO
// high even when deselected, which would corrupt ADS1293 reads on a shared
// wire. The bus is set up with the SD MISO; the ADS1293 driver re-routes the
// bus's MISO input to its own pin for each of its transactions.
#define PIN_SPI_SCK        19  // D8
#define PIN_SPI_MOSI       18  // D10
#define PIN_ADS1293_MISO   20  // D9 - ADS1293 SDO
#define PIN_ADS1293_CS     17  // D7 - ADS1293 CSB
#define PIN_ADS1293_DRDY   16  // D6 - ADS1293 DRDB (data ready, active low)
#define PIN_SD_MISO        2   // D2 - SD card MISO
#define PIN_SD_CS          21  // D3 - SD card CS

// Manual "measure everything" button: reads 0 at rest, 1 while pressed
// (needs the internal pull-down).
#define PIN_BUTTON         1   // D1

// Shared I2C bus: TMP117 + MAX30102
#define PIN_I2C_SDA        22  // D4
#define PIN_I2C_SCL        23  // D5

// XIAO ESP32-C6 on-board RF switch (Seeed wiki): GPIO3 powers the switch
// (active low), GPIO14 selects the antenna (low = built-in ceramic, high =
// external U.FL). The Arduino core sets these in its board variant; plain
// ESP-IDF leaves them floating, so the switch may never route the radio to
// the antenna at all.
#define PIN_RF_SWITCH_EN   3
#define PIN_RF_ANT_SELECT  14

#define SPI_HOST_ID        SPI2_HOST
#define I2C_PORT_NUM        0
#define I2C_CLOCK_HZ        400000
