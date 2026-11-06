#include "board/sd_card_driver.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board/esp_vocat_v1_0_pins.h"
#include "board/sd_dma_bulk.h"
#include "board/sd_probe_pattern.h"
#include "board/sd_probe_trace.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#if CONFIG_NUWAX_SD_DIRECTORY_GUARD_ENABLE
#include "board/sd_directory_guard.h"
#include "ff.h"
#include "diskio_sdmmc.h"
#endif

static const char *TAG = "nuwax_sd";
static const char *MOUNT_POINT = "/sdcard";
static sdmmc_card_t *s_card;
static bool s_mounted;
static bool s_healthy;
/* Last completed physical mount result, separate from admission/health.
 * Recovery unmounts must not briefly look like a cardless product. */
static atomic_bool s_media_detected;
/* Lock-free admission evidence. Mount/qualification and explicit I/O faults
 * publish it; ordinary unmount/probe-in-progress does not invent a fault. */
static atomic_int s_media_error;
/* Serialized by s_lock. Unique within the boot even before RF entropy is
 * available; a random boot salt also separates old sectors after reboot. */
static uint64_t s_qualification_sequence, s_qualification_salt;
/* Sticky until reboot: recovery must not oscillate between bus speeds. */
static bool s_highspeed_disabled;
/* Preserve the FIRST cause across ordered remounts, so a truncated replay
 * log still explains why this boot is using 20 MHz. Protected by s_lock. */
static const char *s_fallback_stage;
static esp_err_t s_fallback_error;
static int s_fallback_errno;
static uint32_t s_fallback_at_ms;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_storage;
static portMUX_TYPE s_lock_init_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t mount_locked(nuwax_sd_card_info_t *info);
static esp_err_t unmount_locked(void);
static esp_err_t verify_transfer_locked(void);
static esp_err_t ensure_lock(void);
#include "sd_card_deadline.inc"

#if CONFIG_NUWAX_SD_DIRECTORY_GUARD_ENABLE
static esp_err_t validate_directory_locked(const char *path, bool allow_missing)
{
    char fat_path[192];
    const size_t prefix = strlen(MOUNT_POINT);
    if (!path || strncmp(path, MOUNT_POINT, prefix) || path[prefix] != '/' ||
        !path[prefix + 1U] || strstr(path, "/../") || strstr(path, "/./") ||
        strstr(path, "//") || strchr(path, '\\')) return ESP_ERR_INVALID_ARG;
    if (!s_mounted || !s_card || !s_healthy) return ESP_ERR_INVALID_STATE;
    const BYTE drive = ff_diskio_get_pdrv_card(s_card);
    if (drive >= FF_VOLUMES) return ESP_ERR_INVALID_STATE;
#if FF_MULTI_PARTITION
    /* IDF's SDMMC mount registers VFS and f_mount with this same drive index.
     * Multi-partition support is ON in IDF even for the default one-to-one
     * map. Verify that mapping instead of rejecting every normal card. */
    if (VolToPart[drive].pd != drive) return ESP_ERR_INVALID_STATE;
#endif
    const int n = snprintf(fat_path, sizeof(fat_path), "%u:%s", drive, path + prefix);
    if (n < 0 || (size_t)n >= sizeof(fat_path)) return ESP_ERR_INVALID_ARG;
    return nuwax_sd_directory_validate(s_card, fat_path, allow_missing);
}

static esp_err_t validate_owned_directories_locked(void)
{
    static const char *const paths[] = {"/sdcard/offline_audio", "/sdcard/offline2",
        "/sdcard/audio_sessions"};
    for (unsigned i = 0U; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        if (sd_io_deadline_expired()) return ESP_ERR_TIMEOUT;
        const esp_err_t err = validate_directory_locked(paths[i], true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SD directory admission rejected path=%s err=%s; no qualification write or format",
                     paths[i], esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
#endif

esp_err_t nuwax_sd_card_validate_directory(const char *path, bool allow_missing)
{
#if CONFIG_NUWAX_SD_DIRECTORY_GUARD_ENABLE
    if (ensure_lock() != ESP_OK || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_NO_MEM;
    const esp_err_t err = validate_directory_locked(path, allow_missing);
    if (err != ESP_OK && err != ESP_ERR_NO_MEM && err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_ARG) {
        s_healthy = false;
        atomic_store_explicit(&s_media_error, err, memory_order_release);
        ESP_LOGE(TAG, "SD directory integrity rejected path=%s err=%s; media retained for recovery",
                 path, esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
    return err;
#else
    (void)path; (void)allow_missing; return ESP_OK;
#endif
}

esp_err_t nuwax_sd_card_validate_evidence_directory(const char *path, bool allow_missing)
{
#if CONFIG_NUWAX_SD_DIRECTORY_GUARD_ENABLE
    if (!path || strcmp(path, "/sdcard/sd_integrity")) return ESP_ERR_INVALID_ARG;
    if (ensure_lock() != ESP_OK || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_NO_MEM;
    const esp_err_t err = validate_directory_locked(path, allow_missing);
    xSemaphoreGive(s_lock);
    return err;
#else
    (void)path; (void)allow_missing; return ESP_OK;
#endif
}

static void latch_highspeed_fallback(const char *stage, esp_err_t err, int saved_errno)
{
    if (!s_highspeed_disabled) {
        s_fallback_stage = stage;
        s_fallback_error = err;
        s_fallback_errno = saved_errno;
        s_fallback_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
    s_highspeed_disabled = true;
}

static esp_err_t ensure_lock(void)
{
    taskENTER_CRITICAL(&s_lock_init_mux);
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    }
    const bool ready = s_lock != NULL;
    taskEXIT_CRITICAL(&s_lock_init_mux);
    return ready ? ESP_OK : ESP_ERR_NO_MEM;
}

static void fill_info_locked(nuwax_sd_card_info_t *info)
{
    if (!info) {
        return;
    }
    memset(info, 0, sizeof(*info));
    if (!s_mounted || !s_card) {
        return;
    }
    info->mounted = true;
    info->capacity_bytes =
        (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    info->sector_size = s_card->csd.sector_size;
    info->max_frequency_khz = s_card->max_freq_khz;
}

bool nuwax_sd_card_is_mounted(void)
{
    if (ensure_lock() != ESP_OK || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool mounted = s_mounted && s_card != NULL && s_healthy;
    xSemaphoreGive(s_lock);
    return mounted;
}

bool nuwax_sd_card_media_detected(void)
{
    return atomic_load_explicit(&s_media_detected, memory_order_acquire);
}

esp_err_t nuwax_sd_card_media_error(void)
{
    return atomic_load_explicit(&s_media_error, memory_order_acquire);
}

bool nuwax_sd_card_get_identity(nuwax_sd_card_identity_t *identity)
{
    if (!identity || ensure_lock() != ESP_OK ||
        xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return false;
    memset(identity, 0, sizeof(*identity));
    const bool valid = s_mounted && s_card && s_healthy;
    if (valid) {
        const uint32_t fields[5] = {(uint32_t)s_card->cid.mfg_id, (uint32_t)s_card->cid.oem_id,
            (uint32_t)s_card->cid.revision, (uint32_t)s_card->cid.serial, (uint32_t)s_card->cid.date};
        const uint64_t capacity = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
        const uint32_t sector = s_card->csd.sector_size;
        memcpy(identity->bytes, fields, sizeof(fields));
        memcpy(identity->bytes + 20U, s_card->cid.name, 8U);
        memcpy(identity->bytes + 28U, &capacity, sizeof(capacity));
        memcpy(identity->bytes + 36U, &sector, sizeof(sector));
    }
    xSemaphoreGive(s_lock);
    return valid;
}

void nuwax_sd_card_log_clock(void)
{
    if (ensure_lock() != ESP_OK || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
    if (s_mounted && s_card) {
        int khz = 0;
        const esp_err_t err = sdmmc_host_get_real_freq(s_card->host.slot, &khz);
        ESP_LOGI(TAG, "SD replay clock negotiated_khz=%u actual_khz=%d fallback_latched=%u healthy=%u clock_err=%s",
                 (unsigned)s_card->max_freq_khz, err == ESP_OK ? khz : 0,
                 (unsigned)s_highspeed_disabled, (unsigned)s_healthy, esp_err_to_name(err));
        if (s_highspeed_disabled) {
            ESP_LOGW(TAG, "SD fallback origin stage=%s err=%s errno=%d at_ms=%lu; 20MHz retained for this boot",
                     s_fallback_stage ? s_fallback_stage : "unknown",
                     esp_err_to_name(s_fallback_error), s_fallback_errno,
                     (unsigned long)s_fallback_at_ms);
        }
    }
    xSemaphoreGive(s_lock);
}

esp_err_t nuwax_sd_card_probe(void)
{
    if (ensure_lock() != ESP_OK || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_mounted || !s_card || !s_healthy) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    /* stat() alone can be satisfied by a stale VFS directory entry after a
     * physical hot-unplug. Force a FAT metadata transaction first; it reaches
     * the SD controller and therefore distinguishes an alive card from a
     * still-registered but unusable mount. Keep the directory check as a
     * second invariant because recovery must recreate it when necessary. */
    uint64_t total_bytes = 0U;
    uint64_t free_bytes = 0U;
    errno = 0;
    const esp_err_t info_err =
        esp_vfs_fat_info("/sdcard", &total_bytes, &free_bytes);
    const int info_errno = errno;
    struct stat root_info = {0};
    errno = 0;
    const int stat_result = stat("/sdcard/offline_audio", &root_info);
    esp_err_t err =
        info_err == ESP_OK && stat_result == 0 && S_ISDIR(root_info.st_mode)
            ? ESP_OK
            : (info_err != ESP_OK ? info_err : ESP_FAIL);
#if CONFIG_NUWAX_SD_DIRECTORY_GUARD_ENABLE
    if (err == ESP_OK) err = validate_owned_directories_locked();
#endif
    if (err != ESP_OK) {
        s_healthy = false;
        atomic_store_explicit(&s_media_error, err, memory_order_release);
        if (s_card->max_freq_khz > SDMMC_FREQ_DEFAULT)
            latch_highspeed_fallback(info_err != ESP_OK ? "health_fat_info" : "health_directory",
                                     err, info_err != ESP_OK ? info_errno : errno);
        ESP_LOGW(TAG,
                 "runtime SD health probe failed err=%s errno=%d total=%llu free=%llu",
                 esp_err_to_name(err),
                 errno,
                 (unsigned long long)total_bytes,
                 (unsigned long long)free_bytes);
    }
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t mount_locked(nuwax_sd_card_info_t *info)
{
    if (s_mounted && s_card) {
        if (!s_healthy) {
            if (info) memset(info, 0, sizeof(*info));
            return ESP_ERR_INVALID_STATE;
        }
        fill_info_locked(info);
        return ESP_OK;
    }
    if (s_mounted || s_card) {
        ESP_LOGE(TAG, "inconsistent mount state mounted=%d card=%p",
                 (int)s_mounted, (void *)s_card);
        if (info) memset(info, 0, sizeof(*info));
        return ESP_ERR_INVALID_STATE;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.do_transaction = sd_deadline_transaction;
#if CONFIG_NUWAX_SD_CARD_HIGHSPEED_ENABLE
    if (!s_highspeed_disabled) host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
#endif
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = NUWAX_VOCAT_SD_CLK_GPIO;
    slot.cmd = NUWAX_VOCAT_SD_CMD_GPIO;
    slot.d0 = NUWAX_VOCAT_SD_D0_GPIO;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err;
    bool detected = false;
    for (;;) {
        if (sd_io_deadline_expired()) { err = ESP_ERR_TIMEOUT; break; }
        s_card = NULL;
        errno = 0;
        err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount_config, &s_card);
        if (err == ESP_OK) {
            detected = true;
            s_mounted = true;
            s_healthy = true;
            /* A mounted FAT volume is not proof of reliable storage. The
             * 20 MHz fallback must pass the same readback before admission. */
#if CONFIG_NUWAX_SD_DIRECTORY_GUARD_ENABLE
            err = validate_owned_directories_locked();
            if (err == ESP_OK)
#endif
            err = verify_transfer_locked();
            if (err != ESP_OK) {
                s_healthy = false;
                atomic_store_explicit(&s_media_error, err, memory_order_release);
                if (unmount_locked() != ESP_OK) {
                    atomic_store_explicit(&s_media_detected, true, memory_order_release);
                    if (info) memset(info, 0, sizeof(*info));
                    return ESP_FAIL;
                }
            }
        }
        if (err == ESP_OK || sd_io_deadline_expired() || host.max_freq_khz <= SDMMC_FREQ_DEFAULT) break;
        latch_highspeed_fallback("mount", err, errno);
        ESP_LOGW(TAG, "high-speed qualification failed err=%s; retrying 20MHz for this boot",
                 esp_err_to_name(err));
        /* Failed mount cleans up its host/card in IDF. A successful mount
         * followed by failed verification must be explicitly unmounted. */
        if (s_mounted && unmount_locked() != ESP_OK) return ESP_FAIL;
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "mount failed clk=%d cmd=%d d0=%d: %s",
                 NUWAX_VOCAT_SD_CLK_GPIO,
                 NUWAX_VOCAT_SD_CMD_GPIO,
                 NUWAX_VOCAT_SD_D0_GPIO,
                 esp_err_to_name(err));
        atomic_store_explicit(&s_media_error, err, memory_order_release);
        s_card = NULL;
        s_mounted = false;
        s_healthy = false;
        /* Present but unverifiable storage must not silently become the
         * cardless streaming product. A genuinely absent card stays false. */
        atomic_store_explicit(&s_media_detected, detected, memory_order_release);
        if (info) memset(info, 0, sizeof(*info));
        return err;
    }
    s_mounted = true;
    s_healthy = true;
    atomic_store_explicit(&s_media_error, ESP_OK, memory_order_release);
    atomic_store_explicit(&s_media_detected, true, memory_order_release);
    fill_info_locked(info);
    int real_khz = 0;
    const esp_err_t clock_err = sdmmc_host_get_real_freq(s_card->host.slot, &real_khz);
    ESP_LOGI(TAG, "SD clock requested_khz=%d negotiated_khz=%u actual_khz=%d clock_err=%s fallback_latched=%u",
             host.max_freq_khz, (unsigned)s_card->max_freq_khz,
             clock_err == ESP_OK ? real_khz : 0, esp_err_to_name(clock_err),
             (unsigned)s_highspeed_disabled);
    ESP_LOGI(TAG,
             "mounted name=%s capacity=%llu sector=%u max_freq=%ukHz",
             s_card->cid.name,
             (unsigned long long)((uint64_t)s_card->csd.capacity * s_card->csd.sector_size),
             (unsigned)s_card->csd.sector_size,
             (unsigned)s_card->max_freq_khz);
    return ESP_OK;
}

/* Exclusive mount qualification, before storage admission. No archive is
 * opened or overwritten. Reopen after fsync/close to exercise the read path.
 * PSRAM keeps this bounded test off scarce internal heap and task stacks. */
static esp_err_t verify_transfer_locked(void)
{
    enum { BLOCK = 4096, TOTAL = 65536 };
    uint8_t *buffer = heap_caps_malloc(BLOCK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        latch_highspeed_fallback("qualification_alloc", ESP_ERR_NO_MEM, 0);
        return ESP_ERR_NO_MEM;
    }
    if (!s_qualification_sequence)
        s_qualification_salt = ((uint64_t)esp_random() << 32U) | esp_random();
    const uint64_t challenge = s_qualification_salt + ++s_qualification_sequence;
    char path[96];
    snprintf(path, sizeof(path), "/sdcard/.nuwax_speed_%016llx_%llu.tmp",
             (unsigned long long)challenge, (unsigned long long)esp_timer_get_time());
    errno = 0;
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) {
        latch_highspeed_fallback("qualification_create", ESP_FAIL, errno);
        heap_caps_free(buffer); return ESP_FAIL;
    }
    nuwax_sd_io_probe_t witness = {0};
    const bool observed = nuwax_sd_io_probe_begin(&witness);
#ifdef ESP_PLATFORM
    const bool trace_observed = observed && nuwax_sd_probe_trace_begin();
#endif
    esp_err_t err = ESP_OK;
    for (uint32_t offset = 0; offset < TOTAL && err == ESP_OK; offset += BLOCK) {
        for (uint32_t i = 0; i < BLOCK; ++i)
            buffer[i] = nuwax_sd_probe_byte(offset + i, challenge);
        errno = 0;
        if (write(fd, buffer, BLOCK) != BLOCK) {
            err = ESP_FAIL;
            latch_highspeed_fallback("qualification_write", err, errno);
        }
    }
    errno = 0;
    if (err == ESP_OK && fsync(fd) != 0) {
        err = ESP_FAIL;
        latch_highspeed_fallback("qualification_sync", err, errno);
    }
    errno = 0;
    if (close(fd) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
        latch_highspeed_fallback("qualification_write_close", err, errno);
    }
    fd = -1;
    if (err == ESP_OK) {
        errno = 0;
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            err = ESP_FAIL;
            latch_highspeed_fallback("qualification_reopen", err, errno);
        }
    }
    for (uint32_t offset = 0; offset < TOTAL && err == ESP_OK; offset += BLOCK) {
        errno = 0;
        if (read(fd, buffer, BLOCK) != BLOCK) {
            err = ESP_FAIL;
            latch_highspeed_fallback("qualification_read", err, errno);
            break;
        }
        for (uint32_t i = 0; i < BLOCK; ++i) {
            if (buffer[i] != nuwax_sd_probe_byte(offset + i, challenge)) {
                ESP_LOGE(TAG, "SD qualification mismatch freq_khz=%u offset=%lu expected=%02x actual=%02x",
                    (unsigned)s_card->max_freq_khz, (unsigned long)(offset + i),
                    (unsigned)nuwax_sd_probe_byte(offset + i, challenge), (unsigned)buffer[i]);
                err = ESP_ERR_INVALID_CRC;
                latch_highspeed_fallback("qualification_compare", err, 0);
                break;
            }
        }
    }
    errno = 0;
    if (fd >= 0 && close(fd) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
        latch_highspeed_fallback("qualification_read_close", err, errno);
    }
    if (observed) nuwax_sd_io_probe_end();
#ifdef ESP_PLATFORM
    if (trace_observed) nuwax_sd_probe_trace_finish("bulk", err,
        witness.first_bad_sector, witness.first_expected_crc, witness.first_read_crc);
#endif
    errno = 0;
    if (unlink(path) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
        latch_highspeed_fallback("qualification_cleanup", err, errno);
    }
    heap_caps_free(buffer);
    ESP_LOGI(TAG, "SD speed qualification bytes=%u err=%s challenge=%016llx", (unsigned)TOTAL, esp_err_to_name(err),
        (unsigned long long)challenge);
    ESP_LOGI(TAG, "SD IO witness revision=205 scope=bulk observed=%u reads=%lu writes=%lu sdk_errors=%lu witnessed=%lu matches=%lu mismatches=%lu first_sector=%llu expected_crc=%08lx read_crc=%08lx",
        observed, (unsigned long)witness.reads, (unsigned long)witness.writes, (unsigned long)witness.sdk_errors,
        (unsigned long)witness.write_witnesses, (unsigned long)witness.read_matches, (unsigned long)witness.read_mismatches,
        (unsigned long long)witness.first_bad_sector, (unsigned long)witness.first_expected_crc, (unsigned long)witness.first_read_crc);
    return err;
}

esp_err_t nuwax_sd_card_mount(nuwax_sd_card_info_t *info)
{
    if (ensure_lock() != ESP_OK || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        if (info) memset(info, 0, sizeof(*info));
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = mount_locked(info);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t nuwax_sd_card_read_write_selftest(void)
{
    if (ensure_lock() != ESP_OK || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_mounted || !s_card || !s_healthy) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    static const char payload[] = "NUWAX_MIAOPAN_V2_SD_SELFTEST_V1\n";
    char path[96] = {0};
    snprintf(path,
             sizeof(path),
             "/sdcard/.nuwax_hw_selftest_%llu.tmp",
             (unsigned long long)esp_timer_get_time());
    struct stat existing = {0};
    if (stat(path, &existing) == 0) {
        ESP_LOGE(TAG, "refusing to overwrite existing selftest file %s", path);
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "cannot create %s", path);
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    size_t written = fwrite(payload, 1, sizeof(payload), file);
    int close_result = fclose(file);
    if (written != sizeof(payload) || close_result != 0) {
        (void)remove(path);
        ESP_LOGE(TAG, "write verify failed bytes=%u close=%d", (unsigned)written, close_result);
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }

    char readback[sizeof(payload)] = {0};
    file = fopen(path, "rb");
    if (!file) {
        (void)remove(path);
        ESP_LOGE(TAG, "cannot reopen %s", path);
        xSemaphoreGive(s_lock);
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
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "read/write/delete selftest PASS bytes=%u", (unsigned)sizeof(payload));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static esp_err_t unmount_locked(void)
{
    if (!s_mounted || !s_card) {
        if (s_mounted || s_card) {
            ESP_LOGE(TAG, "inconsistent mount state during unmount mounted=%d card=%p",
                     (int)s_mounted, (void *)s_card);
            s_card = NULL;
            s_mounted = false;
            s_healthy = false;
            return ESP_ERR_INVALID_STATE;
        }
        s_healthy = false;
        return ESP_OK;
    }
    esp_err_t err = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    /* IDF releases the card/host context before returning from this API. Do
     * not retain a pointer when path unregistration reports an error: keeping
     * it would turn a later recovery attempt into a use-after-free. */
    s_card = NULL;
    s_mounted = false;
    s_healthy = false;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "unmounted");
    } else {
        ESP_LOGW(TAG, "unmount completed with err=%s; software state invalidated",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t nuwax_sd_card_unmount(void)
{
    if (ensure_lock() != ESP_OK || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = unmount_locked();
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t recover_with_lock(uint32_t timeout_ms)
{
    if (ensure_lock() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    const TickType_t wait_ticks = timeout_ms == UINT32_MAX
                                      ? portMAX_DELAY
                                      : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_lock, wait_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_mounted && s_card && s_healthy) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    /* This function is intentionally only called by the ordered storage
     * owner. At this point no active FILE may remain; unmounting underneath a
     * writer would invalidate FatFS state and can corrupt the archive. */
    const bool had_mount_state = s_mounted || s_card;
    const esp_err_t unmount_err = had_mount_state ? unmount_locked() : ESP_OK;
    if (unmount_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "runtime SD recovery unmount reported err=%s; retrying clean mount",
                 esp_err_to_name(unmount_err));
    }
    nuwax_sd_card_info_t info = {0};
    const esp_err_t mount_err = mount_locked(&info);
    if (mount_err == ESP_OK) {
        ESP_LOGI(TAG,
                 "runtime SD recovery mounted capacity=%llu sector=%u max_freq=%ukHz",
                 (unsigned long long)info.capacity_bytes,
                 (unsigned)info.sector_size,
                 (unsigned)info.max_frequency_khz);
    } else {
        ESP_LOGW(TAG,
                 "runtime SD recovery mount failed err=%s",
                 esp_err_to_name(mount_err));
    }
    xSemaphoreGive(s_lock);
    return mount_err;
}

esp_err_t nuwax_sd_card_recover(uint32_t timeout_ms)
{
    bool owns_deadline = false;
    if (!sd_io_deadline_current() && timeout_ms != UINT32_MAX) {
        const esp_err_t scope = nuwax_sd_card_io_deadline_begin(
            esp_timer_get_time() + (int64_t)timeout_ms * 1000);
        if (scope != ESP_OK) return scope;
        owns_deadline = true;
    }
    const int64_t until = sd_io_deadline_current();
    const int64_t now = esp_timer_get_time();
    esp_err_t err;
    if (until && until <= now) err = ESP_ERR_TIMEOUT;
    else {
        if (until) {
            const uint64_t left = (uint64_t)(until - now + 999) / 1000U;
            if (left < timeout_ms) timeout_ms = (uint32_t)left;
        }
        err = recover_with_lock(timeout_ms);
        if (sd_io_deadline_expired()) err = ESP_ERR_TIMEOUT;
    }
    if (owns_deadline) nuwax_sd_card_io_deadline_end();
    return err;
}

void nuwax_sd_card_mark_io_fault(esp_err_t result)
{
    if (result == ESP_OK || ensure_lock() != ESP_OK ||
        xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    const bool first = s_healthy;
    atomic_store_explicit(&s_media_error, result, memory_order_release);
    if (s_card && s_card->max_freq_khz > SDMMC_FREQ_DEFAULT)
        latch_highspeed_fallback("runtime_io", result, 0);
    s_healthy = false;
    xSemaphoreGive(s_lock);
    if (first) {
        ESP_LOGE(TAG,
                 "runtime SD media marked unhealthy err=%s; new users are fenced until ordered recovery",
                 esp_err_to_name(result));
    }
}

bool nuwax_sd_card_fallback_after_read_error(esp_err_t result)
{
    if ((result != ESP_ERR_INVALID_CRC && result != ESP_ERR_INVALID_SIZE) ||
        ensure_lock() != ESP_OK || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return false;
    const bool highspeed = s_mounted && s_card &&
                          s_card->max_freq_khz > SDMMC_FREQ_DEFAULT;
    if (highspeed) {
        latch_highspeed_fallback("archive_validation", result, 0);
        s_healthy = false;
        atomic_store_explicit(&s_media_error, result, memory_order_release);
        ESP_LOGW(TAG, "SD validation failed at high speed err=%s; archive retained pending ordered 20MHz recovery",
                 esp_err_to_name(result));
    }
    xSemaphoreGive(s_lock);
    return highspeed;
}
