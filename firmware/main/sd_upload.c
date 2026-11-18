#include "sd_upload.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "sd_logger.h"
#include "telemetry_http.h"

static const char *TAG = "sd_upload";

// Piece size per HTTP request: big enough that a 10-minute ECG (~0.5 MB) is
// ~60 requests, small enough to sit in RAM next to Wi-Fi.
#define PART_SIZE 8192

// Guard against a file that keeps failing to delete being sent forever.
#define MAX_FILES_PER_RUN 200

static bool upload_file(int index, long size, uint32_t boot_id)
{
    static char buf[PART_SIZE];
    char name[32];
    sd_logger_file_name(index, name, sizeof(name));

    FILE *f = sd_logger_open_read(index);
    if (f == NULL) {
        ESP_LOGE(TAG, "can't open %s", name);
        return false;
    }
    long sent = 0;
    int part = 0;
    bool ok = true;
    while (sent < size) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0) {
            ESP_LOGE(TAG, "read error in %s at %ld/%ld bytes", name, sent, size);
            ok = false;
            break;
        }
        sent += (long)n;
        bool last = sent >= size;
        long long uptime_s = esp_timer_get_time() / 1000000LL;
        if (telemetry_http_upload_part(name, part, last, boot_id, uptime_s, buf, n) != ESP_OK) {
            ok = false;
            break;
        }
        part++;
    }
    fclose(f);
    if (ok) {
        ESP_LOGI(TAG, "%s sent (%ld bytes, %d parts)", name, size, part);
    }
    return ok;
}

bool sd_upload_all(uint32_t boot_id)
{
    if (!sd_logger_reinit()) {
        ESP_LOGW(TAG, "no usable SD card");
        return false;
    }
    for (int i = 0; i < MAX_FILES_PER_RUN; i++) {
        int index;
        long size;
        if (!sd_logger_oldest_file(&index, &size)) {
            ESP_LOGI(TAG, "no measurement files left on the SD card");
            return true;
        }
        if (size < 0) {
            return false; // can't even stat it - leave it alone
        }
        if (size > 0 && !upload_file(index, size, boot_id)) {
            ESP_LOGW(TAG, "upload stopped - the remaining files stay on the card");
            return false;
        }
        if (!sd_logger_delete(index)) {
            return false;
        }
    }
    return false;
}
