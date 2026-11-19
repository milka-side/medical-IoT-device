#include "ads1293.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "soc/spi_periph.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pins.h"

static const char *TAG = "ads1293";

// Register addresses (from ProtoCentral's ADS1293 library, see TI SNAS602C).
#define REG_CONFIG          0x00
#define REG_FLEX_CH1_CN     0x01
#define REG_FLEX_CH2_CN     0x02
#define REG_FLEX_CH3_CN     0x03
#define REG_FLEX_PACE_CN    0x04
#define REG_FLEX_VBAT_CN    0x05
#define REG_LOD_CN          0x06
#define REG_LOD_EN          0x07
#define REG_LOD_CURRENT     0x08
#define REG_LOD_AC_CN       0x09
#define REG_CMDET_EN        0x0A
#define REG_CMDET_CN        0x0B
#define REG_RLD_CN          0x0C
#define REG_WILSON_EN1      0x0D
#define REG_WILSON_EN2      0x0E
#define REG_WILSON_EN3      0x0F
#define REG_WILSON_CN       0x10
#define REG_REF_CN          0x11
#define REG_OSC_CN          0x12
#define REG_AFE_RES         0x13
#define REG_AFE_SHDN_CN     0x14
#define REG_AFE_FAULT_CN    0x15
#define REG_AFE_PACE_CN     0x17
#define REG_ERROR_LOD       0x18
#define REG_DIGO_STRENGTH   0x1F
#define REG_R2_RATE         0x21
#define REG_R3_RATE_CH1     0x22
#define REG_R3_RATE_CH2     0x23
#define REG_R3_RATE_CH3     0x24
#define REG_R1_RATE         0x25
#define REG_DIS_EFILTER     0x26
#define REG_DRDYB_SRC       0x27
#define REG_SYNCB_CN        0x28
#define REG_MASK_DRDYB      0x29
#define REG_MASK_ERR        0x2A
#define REG_ALARM_FILTER    0x2E
#define REG_CH_CNFG         0x2F
#define REG_DATA_STATUS     0x30
#define REG_DATA_CH1_ECG    0x37
#define REG_REVID           0x40

#define CONFIG_START_CON    0x01
#define CONFIG_STANDBY      0x02

#define READ_BIT            0x80

static spi_device_handle_t s_spi;
static SemaphoreHandle_t s_drdy_sem;

// The SD module drives MISO even when deselected, so the ADS1293 SDO has
// its own pin. MISO is switched to it for each ADS1293 transaction (GPIO
// matrix) and switched back after. Runs from the SPI ISR.
static int s_miso_gpio = -1;
static int s_bus_miso_gpio = -1;
static uint32_t s_spiq_in_signal;

static void IRAM_ATTR spi_pre_cb(spi_transaction_t *t)
{
    esp_rom_gpio_connect_in_signal(s_miso_gpio, s_spiq_in_signal, false);
}

static void IRAM_ATTR spi_post_cb(spi_transaction_t *t)
{
    esp_rom_gpio_connect_in_signal(s_bus_miso_gpio, s_spiq_in_signal, false);
}

static void IRAM_ATTR drdy_isr_handler(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_drdy_sem, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t reg_write(uint8_t addr, uint8_t value)
{
    uint8_t tx[2] = {(uint8_t)(addr & 0x7F), value};
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
    };
    return spi_device_transmit(s_spi, &t);
}

static esp_err_t reg_read(uint8_t addr, uint8_t *value)
{
    uint8_t tx[2] = {(uint8_t)(addr | READ_BIT), 0x00};
    uint8_t rx[2] = {0};
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_transmit(s_spi, &t);
    if (err == ESP_OK) {
        *value = rx[1];
    }
    return err;
}

// Reads `len` registers in one transaction, so all bytes come from the
// same sample.
static esp_err_t reg_read_burst(uint8_t addr, uint8_t *out, size_t len)
{
    uint8_t tx[8] = {0};
    uint8_t rx[8] = {0};
    tx[0] = addr | READ_BIT;
    spi_transaction_t t = {
        .length = (len + 1) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_transmit(s_spi, &t);
    if (err == ESP_OK) {
        memcpy(out, rx + 1, len);
    }
    return err;
}

// Set to 1 to try all SPI modes at boot (wiring debug only).
#define ADS1293_RUN_SPI_DIAG_SWEEP 0

#if ADS1293_RUN_SPI_DIAG_SWEEP
static uint8_t spi_probe_revid_once(spi_host_device_t host, int cs_gpio, uint8_t mode, int clock_hz,
                                     esp_err_t *out_err)
{
    spi_device_interface_config_t cfg = {
        .clock_speed_hz = clock_hz,
        .mode = mode,
        .spics_io_num = cs_gpio,
        .queue_size = 1,
    };
    spi_device_handle_t dev;
    esp_err_t err = spi_bus_add_device(host, &cfg, &dev);
    if (err != ESP_OK) {
        *out_err = err;
        return 0xEE;
    }
    uint8_t tx[2] = {REG_REVID | READ_BIT, 0x00};
    uint8_t rx[2] = {0};
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    err = spi_device_transmit(dev, &t);
    spi_bus_remove_device(dev);
    *out_err = err;
    return rx[1];
}

// Logs REVID for every SPI mode / speed. If all are 0x00/0xFF, it's
// wiring or power, not SPI settings.
static void ads1293_spi_diagnostic_sweep(spi_host_device_t host, int cs_gpio)
{
    const int clocks[] = {100000, 1000000};
    ESP_LOGW(TAG, "--- ADS1293 SPI diagnostic sweep (REVID should be != 0x00 and != 0xFF) ---");
    for (size_t ci = 0; ci < sizeof(clocks) / sizeof(clocks[0]); ci++) {
        for (uint8_t mode = 0; mode < 4; mode++) {
            esp_err_t err;
            uint8_t v = spi_probe_revid_once(host, cs_gpio, mode, clocks[ci], &err);
            ESP_LOGW(TAG, "  mode=%d clock=%dHz -> REVID=0x%02X (%s)", mode, clocks[ci], v,
                     esp_err_to_name(err));
        }
    }
    ESP_LOGW(TAG, "--- end sweep ---");
}
#endif

esp_err_t ads1293_init(spi_host_device_t host, int cs_gpio, int miso_gpio, int bus_miso_gpio)
{
#if ADS1293_RUN_SPI_DIAG_SWEEP
    ads1293_spi_diagnostic_sweep(host, cs_gpio);
#endif

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1 * 1000 * 1000, // ADS1293 max ~2 MHz
        .mode = 0,
        .spics_io_num = cs_gpio,
        .queue_size = 4,
    };
    if (miso_gpio >= 0 && miso_gpio != bus_miso_gpio) {
        gpio_config_t miso_conf = {
            .pin_bit_mask = 1ULL << miso_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE, // SDO is high-Z while CSB is high
        };
        gpio_config(&miso_conf);
        s_miso_gpio = miso_gpio;
        s_bus_miso_gpio = bus_miso_gpio;
        s_spiq_in_signal = spi_periph_signal[host].spiq_in;
        devcfg.pre_cb = spi_pre_cb;
        devcfg.post_cb = spi_post_cb;
    }
    esp_err_t err = spi_bus_add_device(host, &devcfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    s_drdy_sem = xSemaphoreCreateBinary();

    // DRDYB is push-pull, no pull-up needed.
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << PIN_ADS1293_DRDY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_ADS1293_DRDY, drdy_isr_handler, NULL);

    // Config mode first (START_CON=0).
    reg_write(REG_CONFIG, 0x00);

    uint8_t revid = 0;
    reg_read(REG_REVID, &revid);
    ESP_LOGI(TAG, "REVID = 0x%02X (expect non-zero, non-0xFF if SPI wiring is OK)", revid);

    // 3-electrode cable, only channel 1 is used. LL/IN4 aren't connected,
    // so they stay out of the common-mode and RLD.
    // Cable: red/sleeve -> IN1 (RL), yellow/tip -> IN2 (LA), green/ring -> IN3 (RA).
    // POS=IN3, NEG=IN2, otherwise the QRS comes out inverted.
    reg_write(REG_FLEX_CH1_CN, 0x1A); // IN3-IN2 -> channel 1
    reg_write(REG_CMDET_EN, 0x06); // common-mode from IN2+IN3
    reg_write(REG_RLD_CN, 0x01); // RLD -> IN1

    // DC lead-off on RA/LA, ERROR_LOD says which one is off.
    reg_write(REG_LOD_CN, 0x00);
    reg_write(REG_LOD_EN, 0x06); // IN2 (LA) + IN3 (RA)
    reg_write(REG_LOD_CURRENT, 0x04);

    // Clock: OSC_CN = 0, wait 20 ms for the crystal, then STRTCLK.
    reg_write(REG_OSC_CN, 0x00);
    vTaskDelay(pdMS_TO_TICKS(20));
    reg_write(REG_OSC_CN, 0x04);

    // Channels 2 and 3 aren't used, power them off.
    reg_write(REG_AFE_SHDN_CN, 0x36);
    reg_write(REG_AFE_PACE_CN, 0x01); // pace channel off
    reg_write(REG_R2_RATE, 0x01); // R2 = 4
    reg_write(REG_R3_RATE_CH1, 0x04); // R3 = 8 -> 102400/(4*4*8) = 800 sps
    reg_write(REG_DRDYB_SRC, 0x08); // DRDYB on channel 1
    reg_write(REG_CH_CNFG, 0x10); // read back channel 1 only

    // Standby until a measurement starts.
    reg_write(REG_CONFIG, CONFIG_STANDBY);
    ESP_LOGI(TAG, "ADS1293 initialised (standby)");
    return ESP_OK;
}

// Monitor-mode ECG band 0.5-40 Hz plus a 50 Hz notch, at 800 sps.

// High-pass 0.5 Hz: subtract an EMA baseline.
#define BASELINE_ALPHA 0.003919

// 50 Hz notch, ~4 Hz wide, fs = 800:
//   w0 = 2*pi*50/800, r = 1 - 3*BW*pi/fs = 0.95288
//   b = [1, -2cos(w0), 1], a = [1, -2r*cos(w0), r^2], gain normalizes to ~1
#define NOTCH_B1 (-1.847759)
#define NOTCH_B2 (1.0)
#define NOTCH_A1 (-1.760685)
#define NOTCH_A2 (0.907973)
#define NOTCH_GAIN (0.967463)

// Low-pass 40 Hz, one pole.
#define LOWPASS_ALPHA 0.2696

static double s_baseline;
static bool s_baseline_init = false;
static double s_notch_x1, s_notch_x2, s_notch_y1, s_notch_y2;
static double s_lowpass;

// Reads equal to the previous code: DRDY fired without a new conversion.
static uint32_t s_last_raw;
static uint32_t s_dup_count;

uint32_t ads1293_take_duplicate_count(void)
{
    uint32_t n = s_dup_count;
    s_dup_count = 0;
    return n;
}

esp_err_t ads1293_start_conversion(void)
{
    // Clear an old DRDY so the first read waits for a fresh sample.
    xSemaphoreTake(s_drdy_sem, 0);
    s_baseline_init = false; // reset the filters on the next sample
    return reg_write(REG_CONFIG, CONFIG_START_CON);
}

esp_err_t ads1293_stop_conversion(void)
{
    // Standby keeps the registers, START_CON wakes it up later.
    return reg_write(REG_CONFIG, CONFIG_STANDBY);
}

esp_err_t ads1293_read_lead_off(uint8_t *out_mask)
{
    uint8_t v = 0;
    esp_err_t err = reg_read(REG_ERROR_LOD, &v);
    if (err == ESP_OK) {
        // Remap to bit0 = RA (IN3), bit1 = LA (IN2).
        *out_mask = (uint8_t)(((v >> 2) & 0x01) | (v & 0x02));
    }
    return err;
}

esp_err_t ads1293_read_sample(int32_t *out_sample, uint32_t timeout_ms)
{
    if (xSemaphoreTake(s_drdy_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t bytes[3] = {0};
    reg_read_burst(REG_DATA_CH1_ECG, bytes, sizeof(bytes));

    uint32_t raw = ((uint32_t)bytes[0] << 16) | ((uint32_t)bytes[1] << 8) | bytes[2];
    if (raw == s_last_raw) {
        // ~30% of DRDY interrupts were spurious and re-read the old sample.
        // Drop those.
        s_dup_count++;
        return ESP_ERR_INVALID_STATE;
    }
    s_last_raw = raw;

    if (!s_baseline_init) {
        s_baseline = (double)raw;
        s_notch_x1 = s_notch_x2 = s_notch_y1 = s_notch_y2 = 0.0;
        s_lowpass = 0.0;
        s_baseline_init = true;
    } else {
        s_baseline += ((double)raw - s_baseline) * BASELINE_ALPHA;
    }

    // high-pass
    double centered = (double)raw - s_baseline;

    // notch
    double notched_raw = centered + NOTCH_B1 * s_notch_x1 + NOTCH_B2 * s_notch_x2 - NOTCH_A1 * s_notch_y1 -
                          NOTCH_A2 * s_notch_y2;
    double notched = notched_raw * NOTCH_GAIN;
    s_notch_x2 = s_notch_x1;
    s_notch_x1 = centered;
    s_notch_y2 = s_notch_y1;
    s_notch_y1 = notched_raw;

    // low-pass
    s_lowpass += (notched - s_lowpass) * LOWPASS_ALPHA;

    *out_sample = (int32_t)s_lowpass;
    return ESP_OK;
}
