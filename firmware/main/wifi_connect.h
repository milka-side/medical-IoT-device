#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Starts Wi-Fi in station mode, connects using the SSID/password from
// menuconfig (Medical IoT Device -> Wi-Fi SSID/password), and blocks until
// an IP address is obtained (or retries are exhausted).
esp_err_t wifi_connect_start(void);

bool wifi_connect_is_up(void);
