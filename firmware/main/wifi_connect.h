#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Starts Wi-Fi in station mode, connects using the SSID/password from
// menuconfig (Medical IoT Device -> Wi-Fi SSID/password), and blocks until
// an IP address is obtained (or retries are exhausted).
esp_err_t wifi_connect_start(void);

bool wifi_connect_is_up(void);

// true = modem-sleep (radio dozes between beacons), false = radio always
// awake. No-op if Wi-Fi was never started.
void wifi_connect_set_power_save(bool enable);
