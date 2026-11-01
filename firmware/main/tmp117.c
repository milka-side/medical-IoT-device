#include "tmp117.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Adafruit TMP117 breakout pulls ADDR low on-board when unconnected -> 0x48.
// Change if the ADDR pin is actually bridged to VCC/SDA/SCL on the real board.
#define TMP117_I2C_ADDR   0x48

#define TMP117_REG_TEMP_RESULT  0x00
#define TMP117_REG_CONFIG       0x01
#define TMP117_REG_DEVICE_ID    0x0F

static const char *TAG = "tmp117";
static i2c_master_dev_handle_t s_dev;

static esp_err_t read_reg16(uint8_t reg, uint16_t *out)
{
    uint8_t rx[2] = {0};
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, rx, 2, 1000 / portTICK_PERIOD_MS);
    if (err == ESP_OK) {
        *out = ((uint16_t)rx[0] << 8) | rx[1];
    }
    return err;
}

esp_err_t tmp117_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TMP117_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t id = 0;
    err = read_reg16(TMP117_REG_DEVICE_ID, &id);
    ESP_LOGI(TAG, "device id = 0x%04X (expect 0x0117), err=%s", id, esp_err_to_name(err));

    // Default config (continuous conversion, ~1 Hz, no averaging) works fine
    // as-is; nothing to write here.
    return err;
}

esp_err_t tmp117_read_celsius(float *out_celsius)
{
    uint16_t raw = 0;
    esp_err_t err = read_reg16(TMP117_REG_TEMP_RESULT, &raw);
    if (err != ESP_OK) {
        return err;
    }
    *out_celsius = (int16_t)raw * 0.0078125f; // 7.8125 m°C / LSB
    return ESP_OK;
}
