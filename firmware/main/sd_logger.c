#include "sd_logger.h"

#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd_logger";

#define MOUNT_POINT "/sdcard"
#define FILE_PREFIX "meas_"
#define FILE_SUFFIX ".jsonl"

// stdio buffer in front of FATFS: fewer, larger card writes.
#define WRITE_BUF_SIZE 4096

static spi_host_device_t s_host;
static int s_cs_gpio = -1;
static sdmmc_card_t *s_card;
static FILE *s_file;
static char s_buf[WRITE_BUF_SIZE];
static int s_next_index = 1;
static int s_pending = 0; // non-empty measurement files on the card

static bool parse_index(const char *name, int *idx)
{
    // FATFS may hand names back in upper case.
    return sscanf(name, FILE_PREFIX "%d", idx) == 1 || sscanf(name, "MEAS_%d", idx) == 1;
}

void sd_logger_file_name(int index, char *out, size_t cap)
{
    snprintf(out, cap, FILE_PREFIX "%05d" FILE_SUFFIX, index);
}

static void file_path(int index, char *out, size_t cap)
{
    char name[32];
    sd_logger_file_name(index, name, sizeof(name));
    snprintf(out, cap, MOUNT_POINT "/%s", name);
}

static long file_size(int index)
{
    char path[48];
    file_path(index, path, sizeof(path));
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

// Walks the measurement files: highest number, lowest number, and how many
// are non-empty.
static void scan(int *highest, int *lowest, int *non_empty)
{
    *highest = 0;
    *lowest = 0;
    *non_empty = 0;
    DIR *dir = opendir(MOUNT_POINT);
    if (dir == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        int idx;
        if (!parse_index(e->d_name, &idx)) {
            continue;
        }
        if (idx > *highest) {
            *highest = idx;
        }
        if (*lowest == 0 || idx < *lowest) {
            *lowest = idx;
        }
        if (file_size(idx) > 0) {
            (*non_empty)++;
        }
    }
    closedir(dir);
}

// ESP-IDF's card init always probes for SDIO first (CMD52, CMD5). This
// SDHC card answers those with "illegal command" as it should, but then
// carries the illegal-command flag over into its reply to the next command
// (CMD59, CRC on), so the init aborts. A raw probe showed the card fine with
// the plain memory-card sequence (CMD0/8/59/55/41/58). So the SDIO commands
// never go out: they're answered locally with ESP_ERR_NOT_SUPPORTED, which
// sdmmc treats as "not an IO card" in SPI mode.
static esp_err_t do_transaction_no_sdio(int slot, sdmmc_command_t *cmd)
{
    if (cmd->opcode == 52 || cmd->opcode == 5) { // IO_RW_DIRECT, IO_SEND_OP_COND
        cmd->error = ESP_ERR_NOT_SUPPORTED;
        return ESP_ERR_NOT_SUPPORTED;
    }
    return sdspi_host_do_transaction(slot, cmd);
}

static esp_err_t mount(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = s_host;
    host.do_transaction = do_transaction_no_sdio;
    // 10 MHz: well inside what jumper-wired modules tolerate.
    host.max_freq_khz = 10000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = s_host;
    slot.gpio_cs = s_cs_gpio;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false, // never wipe somebody's card
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        // Keep the (absent or unusable) card deselected so it never reacts
        // to ADS1293 traffic on the shared bus.
        gpio_set_direction(s_cs_gpio, GPIO_MODE_OUTPUT);
        gpio_set_level(s_cs_gpio, 1);
        if (err == ESP_FAIL) {
            ESP_LOGW(TAG, "SD card answers but no FAT filesystem was read - if this persists, format it (FAT32)");
        } else {
            ESP_LOGW(TAG, "no SD card (%s)", esp_err_to_name(err));
        }
        return err;
    }

    uint64_t mb = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024);
    int highest, lowest;
    scan(&highest, &lowest, &s_pending);
    s_next_index = highest + 1;
    ESP_LOGI(TAG, "SD card mounted: %s, %llu MB, %d measurement file(s) waiting, next " FILE_PREFIX "%05d" FILE_SUFFIX,
             s_card->cid.name, (unsigned long long)mb, s_pending, s_next_index);
    return ESP_OK;
}

static void unmount(void)
{
    if (s_card != NULL) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
        s_card = NULL;
    }
}

esp_err_t sd_logger_init(spi_host_device_t host, int cs_gpio)
{
    s_host = host;
    s_cs_gpio = cs_gpio;
    // A card that was left mid-command (ESP reset while the card stayed
    // powered) can need more than one CMD0 round to come back.
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3 && err != ESP_OK; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        err = mount();
    }
    return err;
}

bool sd_logger_is_mounted(void)
{
    return s_card != NULL;
}

bool sd_logger_reinit(void)
{
    if (s_file != NULL) {
        return true; // a file is open - leave the card alone
    }
    unmount();
    return mount() == ESP_OK;
}

// Write error = card pulled or broken: drop it; the next file_begin()
// tries to mount again.
static void drop_card(void)
{
    if (s_file != NULL) {
        fclose(s_file);
        s_file = NULL;
    }
    unmount();
}

bool sd_logger_file_begin(void)
{
    if (s_file != NULL) {
        sd_logger_file_end();
    }
    if (s_card == NULL && mount() != ESP_OK) {
        return false;
    }
    char path[48];
    file_path(s_next_index, path, sizeof(path));
    s_file = fopen(path, "w");
    if (s_file == NULL) {
        ESP_LOGE(TAG, "can't create %s", path);
        drop_card();
        return false;
    }
    s_next_index++;
    setvbuf(s_file, s_buf, _IOFBF, sizeof(s_buf));
    ESP_LOGI(TAG, "writing %s", path);
    return true;
}

bool sd_logger_file_write(const void *data, size_t len)
{
    if (s_file == NULL) {
        return false;
    }
    if (fwrite(data, 1, len, s_file) != len) {
        ESP_LOGE(TAG, "SD write failed");
        drop_card();
        return false;
    }
    return true;
}

bool sd_logger_file_end(void)
{
    if (s_file == NULL) {
        return false;
    }
    // fclose flushes and closes the FAT file, which updates its directory
    // entry (size) - after this the file is complete even if power drops.
    int err = fclose(s_file);
    s_file = NULL;
    if (err != 0) {
        ESP_LOGE(TAG, "SD write failed while closing the file");
        drop_card();
        return false;
    }
    s_pending++;
    return true;
}

int sd_logger_pending_files(void)
{
    return s_card != NULL ? s_pending : -1;
}

bool sd_logger_oldest_file(int *index, long *size)
{
    if (s_card == NULL) {
        return false;
    }
    int highest, lowest, non_empty;
    scan(&highest, &lowest, &non_empty);
    s_pending = non_empty;
    if (lowest == 0) {
        return false;
    }
    *index = lowest;
    *size = file_size(lowest);
    return true;
}

FILE *sd_logger_open_read(int index)
{
    if (s_card == NULL) {
        return NULL;
    }
    char path[48];
    file_path(index, path, sizeof(path));
    return fopen(path, "r");
}

bool sd_logger_delete(int index)
{
    if (s_card == NULL) {
        return false;
    }
    char path[48];
    file_path(index, path, sizeof(path));
    long size = file_size(index);
    if (unlink(path) != 0) {
        ESP_LOGE(TAG, "can't delete %s", path);
        return false;
    }
    if (size > 0 && s_pending > 0) {
        s_pending--;
    }
    return true;
}
