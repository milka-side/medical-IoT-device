#pragma once

// Dashboard buttons, sent as Azure direct method "command" ({"commands":[...]}).

// Bitmask, several presses in one tick are OR-ed.
typedef enum {
    MEASURE_NONE = 0,
    MEASURE_ECG = 1 << 0,
    MEASURE_VITALS = 1 << 1,
    MEASURE_TEMP = 1 << 2,
    MEASURE_ALL = MEASURE_ECG | MEASURE_VITALS | MEASURE_TEMP,
    // Upload the SD card files.
    CMD_SD_UPLOAD = 1 << 3,
    // Measure even during warm-up ("Виміряти все одно", double press).
    CMD_FORCE = 1 << 4,
} measure_cmd_t;

// Parses "commands":[...]. Anything broken = no command.
measure_cmd_t device_commands_parse(const char *body);
