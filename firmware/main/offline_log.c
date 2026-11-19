#include "offline_log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "sd_logger.h"
#include "sdkconfig.h"

static const char *TAG = "offline_log";

// "measbuf" partition (1 MB), fits a 10-minute ECG.
#define PARTITION_LABEL "measbuf"

#define FLASH_SECTOR 4096
#define COPY_CHUNK   2048

static const esp_partition_t *s_part;
// Bytes used. Erased flash is 0xFF, so after a reboot the end is the first 0xFF.
static size_t s_used;
static bool s_header_pending;
static uint32_t s_boot_id;

// Earlier = clock never synced.
#define MIN_VALID_EPOCH 1700000000
static bool s_full_warned;

static size_t find_end(void)
{
    static uint8_t buf[512];
    for (size_t off = 0; off < s_part->size; off += sizeof(buf)) {
        if (esp_partition_read(s_part, off, buf, sizeof(buf)) != ESP_OK) {
            return off;
        }
        for (size_t i = 0; i < sizeof(buf); i++) {
            if (buf[i] == 0xFF) {
                return off + i;
            }
        }
    }
    return s_part->size;
}

esp_err_t offline_log_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PARTITION_LABEL);
    if (s_part == NULL) {
        ESP_LOGE(TAG, "no '%s' partition - offline buffering disabled", PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }
    s_used = find_end();
    if (s_used > 0) {
        ESP_LOGW(TAG, "%u bytes from an earlier measurement still waiting for the SD card", (unsigned)s_used);
    } else {
        ESP_LOGI(TAG, "buffer empty (%u KB)", (unsigned)(s_part->size / 1024));
    }
    return ESP_OK;
}

static bool append_raw(const void *data, size_t len)
{
    if (s_used + len > s_part->size) {
        if (!s_full_warned) {
            s_full_warned = true;
            ESP_LOGE(TAG, "buffer full - further offline data is dropped");
        }
        return false;
    }
    if (esp_partition_write(s_part, s_used, data, len) != ESP_OK) {
        ESP_LOGE(TAG, "flash write failed");
        return false;
    }
    s_used += len;
    return true;
}

void offline_log_session_start(uint32_t boot_id)
{
    s_boot_id = boot_id;
    s_header_pending = true;
}

void offline_log_append(const char *line, size_t len)
{
    if (s_part == NULL) {
        return;
    }
    if (s_header_pending) {
        char hdr[160];
        long long uptime_s = esp_timer_get_time() / 1000000LL;
        time_t now = time(NULL);
        int n;
        if (now >= MIN_VALID_EPOCH) {
            n = snprintf(hdr, sizeof(hdr), "{\"device_id\":\"%s\",\"boot\":%lu,\"uptime_s\":%lld,\"epoch\":%lld}\n",
                         CONFIG_MIOT_DEVICE_ID, (unsigned long)s_boot_id, uptime_s, (long long)now);
        } else {
            n = snprintf(hdr, sizeof(hdr), "{\"device_id\":\"%s\",\"boot\":%lu,\"uptime_s\":%lld}\n",
                         CONFIG_MIOT_DEVICE_ID, (unsigned long)s_boot_id, uptime_s);
        }
        if (!append_raw(hdr, (size_t)n)) {
            return;
        }
        s_header_pending = false;
    }
    if (append_raw(line, len)) {
        append_raw("\n", 1);
    }
}

bool offline_log_has_data(void)
{
    return s_part != NULL && s_used > 0;
}

bool offline_log_copy_to_sd(void)
{
    if (!offline_log_has_data()) {
        return true;
    }
    sd_logger_reinit(); // card needs a re-init after an ECG
    if (!sd_logger_file_begin()) {
        return false; // no card, retry later
    }
    static uint8_t buf[COPY_CHUNK];
    for (size_t off = 0; off < s_used; off += sizeof(buf)) {
        size_t n = s_used - off < sizeof(buf) ? s_used - off : sizeof(buf);
        if (esp_partition_read(s_part, off, buf, n) != ESP_OK || !sd_logger_file_write(buf, n)) {
            ESP_LOGE(TAG, "copy to SD failed at %u/%u bytes - keeping the data", (unsigned)off, (unsigned)s_used);
            sd_logger_file_end();
            return false;
        }
    }
    if (!sd_logger_file_end()) {
        return false;
    }
    ESP_LOGI(TAG, "%u bytes saved to the SD card", (unsigned)s_used);

    size_t erase_len = (s_used + FLASH_SECTOR - 1) / FLASH_SECTOR * FLASH_SECTOR;
    if (esp_partition_erase_range(s_part, 0, erase_len) != ESP_OK) {
        // It's on the card, worst case it gets copied twice.
        ESP_LOGE(TAG, "erasing the buffer failed");
        s_used = find_end();
        return false;
    }
    s_used = 0;
    s_full_warned = false;
    return true;
}
