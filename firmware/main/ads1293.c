#include "ads1293.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pins.h"

static const char *TAG = "ads1293";

// Register addresses, from ProtoCentral's open-source ADS1293 library
// (github.com/Protocentral/protocentral-ads1293-arduino, src/Protocentral_ADS1293.h),
// cross-reference with TI datasheet SNAS602C sec. 8.6 if a reading looks wrong.
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

// Reads `len` consecutive registers starting at `addr` in a single SPI
// transaction (one CS-low..CS-high window). A single spi_device_transmit
// call is atomic w.r.t. FreeRTOS task scheduling - the CS line can't be
// released mid-transfer for another task to run - unlike doing N separate
// reg_read() calls, where a scheduler preemption between them could let
// the chip advance to a new conversion between bytes, splicing together a
// 24-bit value from two different samples.
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

// Wiring/SPI-mode bring-up diagnostic - leave at 0 for normal operation.
// The register sequence below already assumes mode 0 @ 1 MHz works (that
// was confirmed with this sweep during bring-up). Flip to 1 only when
// chasing a dead/garbled REVID read; running it on every boot adds ~8
// extra SPI transactions and several hundred ms of avoidable startup
// delay/log spam for no benefit once the wiring is known good.
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

// Tries every SPI mode (CPOL/CPHA) at two clock speeds and logs the REVID
// read back from each combination. A correctly wired, powered ADS1293
// should answer with a non-0x00/0xFF value on at least one of these - if
// every combination comes back 0x00/0xFF/0xEE, the problem is physical
// (wiring, power, CS, or a dead/absent chip), not SPI configuration.
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

esp_err_t ads1293_init(spi_host_device_t host, int cs_gpio)
{
#if ADS1293_RUN_SPI_DIAG_SWEEP
    ads1293_spi_diagnostic_sweep(host, cs_gpio);
#endif

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1 * 1000 * 1000, // ADS1293 SPI max ~2 MHz, stay conservative
        .mode = 0,
        .spics_io_num = cs_gpio,
        .queue_size = 4,
    };
    esp_err_t err = spi_bus_add_device(host, &devcfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    s_drdy_sem = xSemaphoreCreateBinary();

    // DRDYB is a push-pull output, no pull needed - and ProtoCentral's
    // README explicitly says to remove any pull-up/down on DRDY.
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << PIN_ADS1293_DRDY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_ADS1293_DRDY, drdy_isr_handler, NULL);

    // Make sure we are in config mode (START_CON=0) before touching registers.
    reg_write(REG_CONFIG, 0x00);

    uint8_t revid = 0;
    reg_read(REG_REVID, &revid);
    ESP_LOGI(TAG, "REVID = 0x%02X (expect non-zero, non-0xFF if SPI wiring is OK)", revid);

    // Based on ProtoCentral's 3-lead sequence, but trimmed to what this
    // device actually wires up: a single 3-electrode cable (RA, LA, RL) and
    // only channel 1 (Lead I) is ever read. ProtoCentral's version also
    // routes IN3 (LL) into channel 2 and into the common-mode detector -
    // with no LL electrode that input floats, the common-mode estimate
    // picks up whatever it floats to, and the RLD amp then drives that
    // garbage back into the body as noise on Lead I.
    reg_write(REG_FLEX_CH1_CN, 0x11);   // IN2(LA)-IN1(RA) -> channel 1 (Lead I)
    reg_write(REG_CMDET_EN, 0x03);      // common-mode from IN1+IN2 only (RA, LA)
    reg_write(REG_RLD_CN, 0x04);        // RLD amp output -> IN4 (RL electrode)

    // DC lead-off detection on RA/LA (same LOD_CN/LOD_CURRENT ProtoCentral's
    // v2 library enables by default) - ERROR_LOD then reports which of the
    // two sensing electrodes has no skin contact.
    reg_write(REG_LOD_CN, 0x00);
    reg_write(REG_LOD_EN, 0x03);
    reg_write(REG_LOD_CURRENT, 0x04);

    // Verified against ProtoCentral's own ads1293-arduino v2.0 startClock():
    // force OSC_CN to 0 first, give the crystal 20ms to settle, only then
    // raise STRTCLK (0x04) - this board had been working without the
    // settle delay, but skipping it risks an unreliable clock lock on a
    // cold boot or a marginal crystal.
    reg_write(REG_OSC_CN, 0x00);
    vTaskDelay(pdMS_TO_TICKS(20));
    reg_write(REG_OSC_CN, 0x04);

    // Bits c / c+3 shut down channel (c+1)'s INA / sigma-delta - channels 2
    // and 3 are both unused, so both are powered off (ProtoCentral's
    // 3-lead default only shut down channel 3: 0x24).
    reg_write(REG_AFE_SHDN_CN, 0x36);
    reg_write(REG_AFE_PACE_CN, 0x01);   // PACE_SHDN - pace channel unused here, shut its AFE down
    reg_write(REG_R2_RATE, 0x01);       // R2 = 4 (one-hot encoding, see datasheet)
    reg_write(REG_R3_RATE_CH1, 0x04);   // R3 = 8 -> 102400/(4*4*8) = 800 sps (decimated x4 to 200 Hz)
    reg_write(REG_DRDYB_SRC, 0x08);     // DRDYB pulses on channel 1 ECG ready
    reg_write(REG_CH_CNFG, 0x10);       // ECG read-back on channel 1 only

    // Park in standby until a measurement window opens.
    reg_write(REG_CONFIG, CONFIG_STANDBY);
    ESP_LOGI(TAG, "ADS1293 initialised (standby)");
    return ESP_OK;
}

// Clinical "monitor mode" ECG bandwidth is 0.5-40 Hz (vs. 0.05-150 Hz for
// diagnostic 12-lead) per AAMI/AHA practice and real monitor datasheets
// (Philips, Edan), plus a separate notch at mains frequency. Tuned to that
// spec at the actual ODR (800 sps with the current R2/R3).
//
// The first version of the mains notch here was a double boxcar average,
// which turned out to have a much lower effective cutoff than intended
// (~15 Hz, not 40 Hz) - it was blurring real QRS slopes into rounded
// humps, not just removing 50 Hz. Replaced with a narrow 2nd-order IIR
// notch: zeros on the unit circle at exactly 50 Hz null that frequency,
// poles at the same angle but radius r<1 keep the notch only a few Hz
// wide, so 10-40 Hz content (where QRS slopes live) passes through
// essentially unaffected.

// High-pass: EMA baseline tracker subtracted from the raw sample, cutoff
// 0.5 Hz (alpha = 1 - exp(-2*pi*fc/fs)). Also sidesteps needing to know
// the exact offset-binary zero point for this R2/R3 combo.
#define BASELINE_ALPHA 0.003919

// Narrow notch at 50 Hz, ~4 Hz wide, at fs = 800 sps:
//   w0 = 2*pi*50/800 = 0.39270 rad, cos(w0) = 0.92388
//   r  = 1 - 3*BW*pi/fs = 1 - 3*4*pi/800 = 0.95288
//   b1 = -2*cos(w0), b2 = 1 (zeros at e^{+-j w0})
//   a1 = -2*r*cos(w0), a2 = r^2 (poles at same angle, radius r)
//   gain = (1+a1+a2)/(1+b1+b2), normalizes passband gain to ~1
#define NOTCH_B1 (-1.847759)
#define NOTCH_B2 (1.0)
#define NOTCH_A1 (-1.760685)
#define NOTCH_A2 (0.907973)
#define NOTCH_GAIN (0.967463)

// Low-pass: single one-pole stage, cutoff 40 Hz (alpha = 1 - exp(-2*pi*fc/fs)).
#define LOWPASS_ALPHA 0.2696

static double s_baseline;
static bool s_baseline_init = false;
static double s_notch_x1, s_notch_x2, s_notch_y1, s_notch_y2;
static double s_lowpass;

esp_err_t ads1293_start_conversion(void)
{
    // Drain any stale DRDY signal left over from before conversions were
    // stopped, so the first xSemaphoreTake in ads1293_read_sample() blocks
    // for a genuinely new sample instead of returning immediately.
    xSemaphoreTake(s_drdy_sem, 0);
    s_baseline_init = false; // force a clean filter-state reset on the next sample
    return reg_write(REG_CONFIG, CONFIG_START_CON);
}

esp_err_t ads1293_stop_conversion(void)
{
    // Standby rather than just clearing START_CON: powers the analog blocks
    // down between windows but keeps every register, and writing START_CON
    // later leaves standby directly (same as ProtoCentral's standby()/start()).
    return reg_write(REG_CONFIG, CONFIG_STANDBY);
}

esp_err_t ads1293_read_lead_off(uint8_t *out_mask)
{
    uint8_t v = 0;
    esp_err_t err = reg_read(REG_ERROR_LOD, &v);
    if (err == ESP_OK) {
        *out_mask = v & 0x3F;
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

    if (!s_baseline_init) {
        s_baseline = (double)raw;
        s_notch_x1 = s_notch_x2 = s_notch_y1 = s_notch_y2 = 0.0;
        s_lowpass = 0.0;
        s_baseline_init = true;
    } else {
        s_baseline += ((double)raw - s_baseline) * BASELINE_ALPHA;
    }

    // 1) high-pass (baseline removal)
    double centered = (double)raw - s_baseline;

    // 2) narrow notch (mains hum only, ~48-52 Hz)
    double notched_raw = centered + NOTCH_B1 * s_notch_x1 + NOTCH_B2 * s_notch_x2 - NOTCH_A1 * s_notch_y1 -
                          NOTCH_A2 * s_notch_y2;
    double notched = notched_raw * NOTCH_GAIN;
    s_notch_x2 = s_notch_x1;
    s_notch_x1 = centered;
    s_notch_y2 = s_notch_y1;
    s_notch_y1 = notched_raw;

    // 3) low-pass (general high-frequency/muscle noise, well above QRS content)
    s_lowpass += (notched - s_lowpass) * LOWPASS_ALPHA;

    *out_sample = (int32_t)s_lowpass;
    return ESP_OK;
}
