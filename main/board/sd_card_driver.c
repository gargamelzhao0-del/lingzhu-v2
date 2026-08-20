#include "board/sd_card_driver.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "board/esp_vocat_v1_0_pins.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "nuwax_sd";
static const char *MOUNT_POINT = "/sdcard";
static sdmmc_card_t *s_card;
static bool s_mounted;

bool nuwax_sd_card_is_mounted(void)
{
    return s_mounted;
}

esp_err_t nuwax_sd_card_mount(nuwax_sd_card_info_t *info)
{
    if (info) memset(info, 0, sizeof(*info));
    if (s_mounted) {
        if (info) {
            info->mounted = true;
            info->capacity_bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
            info->sector_size = s_card->csd.sector_size;
            info->max_frequency_khz = s_card->max_freq_khz;
        }
        return ESP_OK;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = NUWAX_VOCAT_SD_CLK_GPIO;
    slot.cmd = NUWAX_VOCAT_SD_CMD_GPIO;
    slot.d0 = NUWAX_VOCAT_SD_D0_GPIO;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "mount failed clk=%d cmd=%d d0=%d: %s",
                 NUWAX_VOCAT_SD_CLK_GPIO,
                 NUWAX_VOCAT_SD_CMD_GPIO,
                 NUWAX_VOCAT_SD_D0_GPIO,
                 esp_err_to_name(err));
        s_card = NULL;
        return err;
    }
    s_mounted = true;
    if (info) {
        info->mounted = true;
        info->capacity_bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
        info->sector_size = s_card->csd.sector_size;
        info->max_frequency_khz = s_card->max_freq_khz;
    }
    ESP_LOGI(TAG,
             "mounted name=%s capacity=%llu sector=%u max_freq=%ukHz",
             s_card->cid.name,
             (unsigned long long)((uint64_t)s_card->csd.capacity * s_card->csd.sector_size),
             (unsigned)s_card->csd.sector_size,
             (unsigned)s_card->max_freq_khz);
    return ESP_OK;
}

esp_err_t nuwax_sd_card_read_write_selftest(void)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    static const char payload[] = "NUWAX_MIAOPAN_V2_SD_SELFTEST_V1\n";
    char path[96] = {0};
    snprintf(path,
             sizeof(path),
             "/sdcard/.nuwax_hw_selftest_%llu.tmp",
             (unsigned long long)esp_timer_get_time());
    struct stat existing = {0};
    if (stat(path, &existing) == 0) {
        ESP_LOGE(TAG, "refusing to overwrite existing selftest file %s", path);
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "cannot create %s", path);
        return ESP_FAIL;
    }
    size_t written = fwrite(payload, 1, sizeof(payload), file);
    int close_result = fclose(file);
    if (written != sizeof(payload) || close_result != 0) {
        (void)remove(path);
        ESP_LOGE(TAG, "write verify failed bytes=%u close=%d", (unsigned)written, close_result);
        return ESP_FAIL;
    }

    char readback[sizeof(payload)] = {0};
    file = fopen(path, "rb");
    if (!file) {
        (void)remove(path);
        ESP_LOGE(TAG, "cannot reopen %s", path);
        return ESP_FAIL;
    }
    size_t read = fread(readback, 1, sizeof(readback), file);
    close_result = fclose(file);
    int remove_result = remove(path);
    if (read != sizeof(payload) || close_result != 0 || remove_result != 0 ||
        memcmp(readback, payload, sizeof(payload)) != 0) {
        ESP_LOGE(TAG,
                 "readback failed read=%u close=%d remove=%d",
                 (unsigned)read, close_result, remove_result);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "read/write/delete selftest PASS bytes=%u", (unsigned)sizeof(payload));
    return ESP_OK;
}

esp_err_t nuwax_sd_card_unmount(void)
{
    if (!s_mounted) return ESP_OK;
    esp_err_t err = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    if (err == ESP_OK) {
        s_card = NULL;
        s_mounted = false;
        ESP_LOGI(TAG, "unmounted");
    }
    return err;
}
