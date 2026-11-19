#pragma once

// Button presses from the dashboard ("measure X", "upload the SD card"). The
// server sends them as an Azure IoT Hub direct method "command" with payload
// {"commands":[...]} (web/src/azure-commands.js, telemetry_azure.c).

// Bitmask: several different buttons pressed between two sender-loop ticks
// all arrive together, so they're OR-ed rather than one overwriting another.
typedef enum {
    MEASURE_NONE = 0,
    MEASURE_ECG = 1 << 0,
    MEASURE_VITALS = 1 << 1,
    MEASURE_TEMP = 1 << 2,
    MEASURE_ALL = MEASURE_ECG | MEASURE_VITALS | MEASURE_TEMP,
    // Not a measurement: send the saved files on the SD card to the server.
    CMD_SD_UPLOAD = 1 << 3,
    // With a measurement: start it even during the power-on warm-up lockout
    // ("Виміряти все одно" on the dashboard, double press on the button).
    CMD_FORCE = 1 << 4,
} measure_cmd_t;

// Parses the "commands":[...] array out of a direct-method payload.
// Anything missing or malformed counts as "no command".
measure_cmd_t device_commands_parse(const char *body);
