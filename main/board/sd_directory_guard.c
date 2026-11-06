#include "board/sd_directory_guard.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "sdkconfig.h"
#if CONFIG_NUWAX_SD_DIRECTORY_GUARD_ENABLE
#include "esp_heap_caps.h"
#include "ff.h"
#include "sdmmc_cmd.h"

enum { DIRECTORY_SECTOR = 512, DIRECTORY_PATH = 192, DIRECTORY_INTERNAL_RESERVE = 16384 };

static esp_err_t directory_result(FRESULT result)
{
    if (result == FR_OK) return ESP_OK;
    if (result == FR_NOT_ENOUGH_CORE) return ESP_ERR_NO_MEM;
    if (result == FR_TIMEOUT) return ESP_ERR_TIMEOUT;
    if (result == FR_NO_FILE || result == FR_NO_PATH) return ESP_ERR_NOT_FOUND;
    return result == FR_INT_ERR ? ESP_ERR_INVALID_CRC : ESP_FAIL;
}

static uint32_t directory_cluster(const uint8_t *entry, bool fat32)
{
    uint32_t cluster = (uint32_t)entry[26] | (uint32_t)entry[27] << 8U;
    if (fat32) cluster |= (uint32_t)entry[20] << 16U | (uint32_t)entry[21] << 24U;
    return cluster;
}

static bool directory_dot(const uint8_t *entry, bool parent)
{
    static const char names[2][12] = {".          ", "..         "};
    return memcmp(entry, names[parent], 11U) == 0 &&
        (entry[11] & (AM_DIR | 0x08U /* Volume label / LFN */)) == AM_DIR &&
        entry[28] == 0U && entry[29] == 0U && entry[30] == 0U && entry[31] == 0U;
}

esp_err_t nuwax_sd_directory_validate(void *opaque, const char *fat_path, bool allow_missing)
{
    sdmmc_card_t *card = opaque;
    char parent_path[DIRECTORY_PATH];
    if (!card || !fat_path || card->csd.sector_size != DIRECTORY_SECTOR ||
        strlen(fat_path) >= sizeof(parent_path)) return ESP_ERR_INVALID_ARG;
    memcpy(parent_path, fat_path, strlen(fat_path) + 1U);
    char *slash = strrchr(parent_path, '/');
    /* Only absolute, non-root, canonical paths supplied by the storage owner. */
    if (!slash || slash == parent_path || slash[-1] == '/' || !slash[1] ||
        strchr(slash + 1, '\\') || !strcmp(slash + 1, ".") || !strcmp(slash + 1, ".."))
        return ESP_ERR_INVALID_ARG;
    slash[1] = '\0';
    const int saved_errno = errno;
    FILINFO info;
    FRESULT fr = f_stat(fat_path, &info);
    if (fr != FR_OK) { errno = saved_errno; return allow_missing &&
        (fr == FR_NO_FILE || fr == FR_NO_PATH) ? ESP_OK : directory_result(fr); }
    if (!(info.fattrib & AM_DIR)) { errno = saved_errno; return ESP_ERR_INVALID_CRC; }
    FF_DIR child = {0}, parent = {0};
    esp_err_t err = directory_result(f_opendir(&child, fat_path));
    if (err == ESP_OK) err = directory_result(f_opendir(&parent, parent_path));
    uint8_t *sector = NULL;
    bool locked = false;
    FATFS *fs = child.obj.fs;
    if (err == ESP_OK && (!fs || fs != parent.obj.fs || child.obj.id != fs->id ||
        parent.obj.id != fs->id)) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
#if FF_FS_REENTRANT
        locked = ff_mutex_take(fs->ldrv) != 0;
        if (!locked) err = ESP_ERR_TIMEOUT;
#else
        locked = true; /* Host image fixtures serialize the complete operation. */
#endif
    }
    if (err == ESP_OK) {
        const bool fat32 = fs->fs_type == FS_FAT32;
        const uint32_t cluster = child.obj.sclust;
#if FF_MAX_SS != FF_MIN_SS
        const unsigned sector_size = fs->ssize;
#else
        const unsigned sector_size = FF_MIN_SS;
#endif
        if ((fs->fs_type != FS_FAT12 && fs->fs_type != FS_FAT16 && !fat32) ||
            sector_size != DIRECTORY_SECTOR || !fs->csize || cluster < 2U ||
            cluster >= fs->n_fatent) err = ESP_ERR_INVALID_CRC;
        const uint64_t lba = cluster >= 2U ? (uint64_t)fs->database +
            (uint64_t)(cluster - 2U) * fs->csize : UINT64_MAX;
        if (err == ESP_OK && lba >= card->csd.capacity) err = ESP_ERR_INVALID_CRC;
        if (err == ESP_OK) {
            if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >=
                DIRECTORY_INTERNAL_RESERVE + DIRECTORY_SECTOR)
                sector = heap_caps_malloc(DIRECTORY_SECTOR, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            if (!sector || heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < DIRECTORY_INTERNAL_RESERVE)
                err = ESP_ERR_NO_MEM;
        }
        if (err == ESP_OK) err = sdmmc_read_sectors(card, sector, (size_t)lba, 1U);
        if (err == ESP_OK) {
            const uint32_t own = directory_cluster(sector, fat32);
            const uint32_t up = directory_cluster(sector + 32U, fat32);
            const uint32_t expected_parent = parent.obj.sclust;
            const bool root_parent = expected_parent == 0U ||
                (fat32 && expected_parent == fs->dirbase);
            const bool parent_matches = root_parent ?
                (up == 0U || (fat32 && up == fs->dirbase)) : up == expected_parent;
            if (!directory_dot(sector, false) || !directory_dot(sector + 32U, true) ||
                own != cluster || !parent_matches) err = ESP_ERR_INVALID_CRC;
        }
    }
    heap_caps_free(sector);
#if FF_FS_REENTRANT
    if (locked) ff_mutex_give(fs->ldrv);
#else
    (void)locked;
#endif
    /* f_closedir takes its own volume lock. Never call it while holding ours. */
    if (parent.obj.fs) { fr = f_closedir(&parent); if (err == ESP_OK) err = directory_result(fr); }
    if (child.obj.fs) { fr = f_closedir(&child); if (err == ESP_OK) err = directory_result(fr); }
    errno = saved_errno;
    return err;
}
#else
esp_err_t nuwax_sd_directory_validate(void *card, const char *path, bool missing)
{ (void)card; (void)path; (void)missing; return ESP_OK; }
#endif
