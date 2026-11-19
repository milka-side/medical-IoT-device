#include "device_commands.h"

#include <string.h>

measure_cmd_t device_commands_parse(const char *body)
{
    // Only look inside "commands":[...].
    const char *start = body ? strstr(body, "\"commands\":[") : NULL;
    if (start == NULL) {
        return MEASURE_NONE;
    }
    start += strlen("\"commands\":[");
    const char *end = strchr(start, ']');
    if (end == NULL) {
        return MEASURE_NONE;
    }

    static const struct {
        const char *name;
        measure_cmd_t bit;
    } types[] = {
        {"\"ecg\"", MEASURE_ECG},
        {"\"vitals\"", MEASURE_VITALS},
        {"\"temp\"", MEASURE_TEMP},
        {"\"all\"", MEASURE_ALL},
        {"\"sd_upload\"", CMD_SD_UPLOAD},
        {"\"all_force\"", MEASURE_ALL | CMD_FORCE},
    };
    measure_cmd_t result = MEASURE_NONE;
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        const char *hit = strstr(start, types[i].name);
        if (hit != NULL && hit < end) {
            result |= types[i].bit;
        }
    }
    return result;
}
