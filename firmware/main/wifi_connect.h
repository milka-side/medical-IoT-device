#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Connects to the Wi-Fi from menuconfig and waits for an IP (or until
// the retries run out).
esp_err_t wifi_connect_start(void);

bool wifi_connect_is_up(void);

// true = modem sleep, false = radio always on.
void wifi_connect_set_power_save(bool enable);
