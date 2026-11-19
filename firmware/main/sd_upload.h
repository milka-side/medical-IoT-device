#pragma once

#include <stdbool.h>
#include <stdint.h>

// "Завантажити дані з SD" on the dashboard: sends every measurement file on
// the SD card to the server (web/server.js /api/sd-upload), oldest first, and
// deletes each file once the server has confirmed it. Empty files (left by a
// measurement that saved nothing) are just deleted. Other files on the card
// are never touched.
//
// This is the only thing the firmware still sends over plain HTTP: a whole
// file at once is far too much for Azure IoT Hub's free F1 tier (8000
// messages/day), so it goes straight to the server at CONFIG_MIOT_SERVER_URL.
//
// Blocks for as long as the transfer takes (seconds for a 10-minute ECG);
// call from the sender task only, and only while nothing is measuring.
// Returns true if the card ended up with no measurement files left.
bool sd_upload_all(uint32_t boot_id);
