#include "offline_audio_spool.h"
#include "offline_audio_policy.h"
#include "nuwax_ota_new/nuwax_ota_device.h"
#include "offline_io_schedule.h"
#include "offline_audio_voice.h"
#include "audio_modes/monitoring/monitoring_speech_detector.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdatomic.h>

#include "audio_runtime_memory_policy.h"
#include "board/sd_card_driver.h"
#include "board/nuwax_power_ctrl.h"
#include "conversation/audio_uplink.h"
#include "device_error_report.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "audio_frame.h"
#include "mic_input.h"
#include "nuwax_provision_new/nuwax_device_uid.h"

#ifndef CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE
#define CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES
#define CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES (1024U * 1024U)
#endif
#ifndef CONFIG_NUWAX_OFFLINE_AUDIO_MAX_BYTES
#define CONFIG_NUWAX_OFFLINE_AUDIO_MAX_BYTES (1536U * 1024U * 1024U)
#endif
#ifndef CONFIG_NUWAX_MIC_READ_TIMEOUT_MS
#define CONFIG_NUWAX_MIC_READ_TIMEOUT_MS 100U
#endif
#ifndef CONFIG_NUWAX_MIC_MAX_CONSECUTIVE_TIMEOUTS
#define CONFIG_NUWAX_MIC_MAX_CONSECUTIVE_TIMEOUTS 30U
#endif
#ifndef CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_MEETING_OFFLINE_MAX_HOURS
#define CONFIG_NUWAX_MEETING_OFFLINE_MAX_HOURS 8
#endif
#ifndef CONFIG_NUWAX_MONITORING_OFFLINE_MAX_HOURS
#define CONFIG_NUWAX_MONITORING_OFFLINE_MAX_HOURS 8
#endif
#ifndef CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_URL
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_URL ""
#endif
#ifndef CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_TOKEN
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_TOKEN ""
#endif

#define TAG "offline_audio"
#define ARCHIVE_QUOTA_BYTES nuwax_offline_audio_quota(CONFIG_NUWAX_OFFLINE_AUDIO_MAX_BYTES)
#define ROOT_DIR "/sdcard/offline_audio"
#define QUEUE_DEPTH 128U
#define MAX_FRAME_BYTES 2048U
#define HEADER_BYTES 96U
#define FOOTER_BYTES 32U
#define QUEUE_ADMISSION_WAIT_MS 40U
#define DURABILITY_SYNC_INTERVAL_FRAMES 100U
#define MIC_HANDOFF_TIMEOUT_MS 3000U
#define MIC_HANDOFF_RETRY_MS 20U
#define DONE_BIT BIT0
#define FAILED_BIT BIT1
#define REPLAY_ACK_BIT BIT2
#define REPLAY_CHUNK_ACK_BIT BIT3
#define REPLAY_TEST_MAX_ATTEMPTS 3U
#define REPLAY_TEST_RETRY_DELAY_MS 1000U
#define REPLAY_SEND_RETRY_ATTEMPTS 8U
#define REPLAY_SEND_RETRY_DELAY_MS 5U
#define REPLAY_CHUNK_HEADER_BYTES 64U
#define REPLAY_CHUNK_MAX_PAYLOAD_BYTES 8064U
#define REPLAY_WINDOW_MAX_BYTES (512U * 1024U)
#define REPLAY_WINDOW_INITIAL_BYTES (64U * 1024U)
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE
static atomic_bool s_replay_lane_probe_taken;
#endif
#define REPLAY_FORMAL_CHUNK_PAYLOAD_BYTES 4096U
#define REPLAY_CHUNK_YIELD_MS 1U
#define REPLAY_TEST_CLOSE_MAX_ATTEMPTS 30U
#define OFFLINE_WRITER_TASK_STACK_BYTES 8192U
#define OFFLINE_CAPTURE_TASK_STACK_BYTES 8192U
/* Keep the microphone reader ahead of reconnect work on core 0. The
 * exclusive audio owner makes core 1 available during offline capture. */
#define OFFLINE_CAPTURE_TASK_PRIORITY 12U
#define OFFLINE_CAPTURE_TASK_CORE 1
#define OFFLINE_REPLAY_TEST_TASK_STACK_BYTES 16384U
#define REPLAY_TEST_HELLO_SEND_TIMEOUT_MS 1500U
#define OFFLINE_REPLAY_FORMAL_TASK_STACK_BYTES 16384U
#define OFFLINE_REPLAY_FORMAL_TASK_PRIORITY 2U
#define WRITER_DEQUEUE_WAIT_MS 10U
#define WRITER_RETRY_INITIAL_MS 250U
#define WRITER_RETRY_MAX_MS 30000U
#define MEDIA_RECOVERY_RETRY_INITIAL_MS 1000U
#define MEDIA_RECOVERY_RETRY_MAX_MS 30000U
#define MEDIA_UNAVAILABLE_RETRY_INITIAL_MS 5000U
#define MEDIA_UNAVAILABLE_RETRY_MAX_MS 120000U
#define OFFLINE_STORAGE_SUPERVISOR_STACK_BYTES 6144U
#define OFFLINE_STORAGE_SUPERVISOR_PRIORITY 1U
#define OFFLINE_STORAGE_PROBE_INTERVAL_MS 5000U
#define OFFLINE_STORAGE_BUSY_RETRY_MS 250U
#define OFFLINE_STORAGE_ABSENT_RETRY_INITIAL_MS 1000U
#define OFFLINE_STORAGE_ABSENT_RETRY_MAX_MS 5000U
#define OFFLINE_MIRROR_ATTACH_RETRY_MS 20U
/* 15.36 s of PCM plus the writer queue's 2.56 s. The measured 7.767 s
 * file-open stall must fit even when a previous generation is closing.
 * Explicit PSRAM allocation keeps this reserve out of the scarce DMA heap. */
#define MIRROR_STAGING_FRAMES 768U
#define MIRROR_CLOSE_TIMEOUT_MS 20000U
#define ARCHIVE_STDIO_BUFFER_BYTES 16384U
#define REPLAY_CHUNK_ACK_ATTEMPTS 3U
#define REPLAY_ISOLATED_IO_TIMEOUT_MS 1500U
#define OFFLINE_MIRROR_CLOSE_RETRY_MS 100U

static uint32_t offline_now_ms(void)
{
    return (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
}

static bool offline_deadline_elapsed(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static nuwax_audio_runtime_memory_t runtime_memory_snapshot(void)
{
    return (nuwax_audio_runtime_memory_t) {
        .internal_free = heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .internal_largest = heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .dma_largest = heap_caps_get_largest_free_block(
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
    };
}

static bool runtime_memory_ready(const char *operation)
{
    const nuwax_audio_runtime_memory_t memory = runtime_memory_snapshot();
    const nuwax_audio_runtime_memory_state_t state =
        nuwax_audio_runtime_memory_classify(memory);
    /* The archive foundation is PSRAM-first and its small internal objects
     * have their own allocation checks.  A DEFER classification means the
     * conservative contiguous-heap watermark is below target, not that the
     * operation is unsafe.  Only CRITICAL memory is refused here; otherwise
     * a valid SD card can remain unused forever after boot merely because the
     * heap is fragmented by Wi-Fi/audio startup allocations. */
    if (state != NUWAX_AUDIO_RUNTIME_MEMORY_CRITICAL) {
        return true;
    }
    ESP_LOGW(TAG,
             "offline audio %s deferred by runtime memory admission internal=%u largest=%u dma_largest=%u",
             operation ? operation : "operation",
             (unsigned)memory.internal_free,
             (unsigned)memory.internal_largest,
             (unsigned)memory.dma_largest);
    return false;
}

typedef struct {
    uint8_t type;
    uint8_t complete;
    uint32_t generation;
    size_t bytes;
    uint8_t data[MAX_FRAME_BYTES];
} spool_item_t;

typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t header_bytes;
    uint8_t mode;
    uint8_t channels;
    uint16_t codec;
    uint32_t sample_rate;
    uint16_t frame_duration_ms;
    uint16_t reserved0;
    uint32_t segment_index;
    uint64_t start_uptime_ms;
    char session_id[48];
    uint8_t reserved[12];
} archive_header_t;

typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t footer_bytes;
    uint32_t frame_count;
    uint64_t payload_bytes;
    uint32_t crc32;
    uint8_t complete;
    uint8_t reserved[3];
} archive_footer_t;

_Static_assert(sizeof(archive_header_t) == HEADER_BYTES,
               "offline archive header size changed");
_Static_assert(sizeof(archive_footer_t) == FOOTER_BYTES,
               "offline archive footer size changed");

typedef struct {
    atomic_bool active;
    atomic_bool closing;
    atomic_bool degraded;
    atomic_bool writer_error;
    atomic_bool writer_busy;
    atomic_bool media_recovery_requested;
    bool stop_requested;
    bool stop_enqueued;
    long last_good_offset;
    atomic_uint_fast32_t dropped_frames;
    atomic_uint_fast32_t ready_count;
    uint32_t segment_index;
    uint32_t frame_count;
    uint64_t payload_bytes;
    uint32_t payload_crc32;
    uint64_t used_bytes;
    uint32_t next_index;
    uint32_t writer_retry_deadline_ms;
    uint32_t writer_retry_backoff_ms;
    bool writer_fault_latched;
    uint32_t media_recovery_deadline_ms;
    uint32_t media_recovery_backoff_ms;
    bool media_was_ready;
    esp_err_t media_recovery_last_error;
    FILE *file;
    char *write_buffer;
    char open_path[128];
    char ready_path[128];
    char session_id[48];
    nuwax_offline_audio_mode_t mode;
    bool mirror;
    uint32_t sample_rate;
    uint8_t channels;
    uint16_t frame_duration_ms;
    QueueHandle_t queue;
    QueueHandle_t free_queue;
    spool_item_t *slot_pool;
    spool_item_t stop_item;
    EventGroupHandle_t events;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t admission_lock;
    TaskHandle_t task;
    TaskHandle_t capture_task;
    EventGroupHandle_t capture_events;
    atomic_bool capture_stop;
    atomic_bool capture_complete;
    atomic_bool capture_running;
    atomic_bool replay_running;
    uint32_t capture_generation;
    SemaphoreHandle_t writer_gate;
    struct {
        bool waiting;
        bool ok;
        bool chunk;
        uint32_t epoch;
        uint32_t next_offset;
        char request_id[48];
        char segment_id[48];
        char expected_type[40];
        uint32_t window_bytes;
        char mode[16];
    } replay_ack;
} spool_state_t;

static spool_state_t s;
static StaticSemaphore_t s_writer_gate_storage;
static StaticSemaphore_t s_init_lock_storage;
static SemaphoreHandle_t s_init_lock;
static portMUX_TYPE s_init_lock_mux = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool s_runtime_initialized = ATOMIC_VAR_INIT(false);
/* Published by the storage owner. Frame paths never take the SD driver lock. */
static atomic_bool s_media_ready;
static atomic_bool s_close_complete;
static atomic_bool s_supervisor_started = ATOMIC_VAR_INIT(false);
/* Directory metadata is authoritative after mount/recovery.  Keep the
 * periodic supervisor probe cheap while the medium is healthy, and rescan
 * only after a directory mutation or a remount boundary. */
static atomic_bool s_directory_dirty = ATOMIC_VAR_INIT(true);
/* Recheck legacy .ready.bad files once per completed replay pass after boot.
 * This also admits recovery when every pending archive was misclassified. */
static atomic_bool s_quarantine_recheck_pending = ATOMIC_VAR_INIT(true);
static TaskHandle_t s_supervisor_task;
/* Replay ACK state is touched by the WebSocket RX callback and the replay
 * worker. Keep it independent from the writer/file lock: an SD fsync must
 * never stall protocol RX for hundreds of milliseconds. */
static portMUX_TYPE s_replay_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_uint_fast32_t s_archive_capability_epoch;
static atomic_bool s_offline_limit_reached;
/* Written only by the capture launch owner; copied into each reader config. */
static char s_limit_session[48];
static uint64_t s_limit_used_us;
static atomic_uint s_mirror_closers;

static bool enabled(void);
static esp_err_t begin_archive(nuwax_offline_audio_mode_t mode,
    const char *session_id, uint32_t sample_rate, uint8_t channels,
    uint16_t frame_duration_ms, bool mirror, bool buffered);
static void offline_storage_supervisor_task(void *arg);

static bool runtime_initialized(void)
{
    return atomic_load_explicit(&s_runtime_initialized,
                                memory_order_acquire);
}

static esp_err_t ensure_init_lock(void)
{
    taskENTER_CRITICAL(&s_init_lock_mux);
    if (!s_init_lock) {
        s_init_lock = xSemaphoreCreateMutexStatic(&s_init_lock_storage);
    }
    const bool ready = s_init_lock != NULL;
    taskEXIT_CRITICAL(&s_init_lock_mux);
    return ready ? ESP_OK : ESP_ERR_NO_MEM;
}

void nuwax_offline_audio_supervisor_wake(void)
{
    if (s_supervisor_task) {
        xTaskNotifyGive(s_supervisor_task);
    }
}

esp_err_t nuwax_offline_audio_supervisor_start(void)
{
    if (!enabled()) {
        return ESP_OK;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &s_supervisor_started, &expected, true,
            memory_order_acq_rel, memory_order_acquire)) {
        return ESP_OK;
    }
    if (xTaskCreateWithCaps(offline_storage_supervisor_task,
                            "offline_sd_supervisor",
                            OFFLINE_STORAGE_SUPERVISOR_STACK_BYTES,
                            NULL,
                            OFFLINE_STORAGE_SUPERVISOR_PRIORITY,
                            &s_supervisor_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_supervisor_task = NULL;
        atomic_store_explicit(&s_supervisor_started, false,
                              memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void schedule_media_recovery_locked(esp_err_t err)
{
    const uint32_t previous = s.media_recovery_backoff_ms;
    const uint32_t maximum = s.media_was_ready ? MEDIA_RECOVERY_RETRY_MAX_MS
                                             : MEDIA_UNAVAILABLE_RETRY_MAX_MS;
    if (s.media_recovery_backoff_ms == 0U) {
        s.media_recovery_backoff_ms = s.media_was_ready ? MEDIA_RECOVERY_RETRY_INITIAL_MS
                                                       : MEDIA_UNAVAILABLE_RETRY_INITIAL_MS;
    } else if (s.media_recovery_backoff_ms <
               maximum / 2U) {
        s.media_recovery_backoff_ms *= 2U;
    } else {
        s.media_recovery_backoff_ms = maximum;
    }
    s.media_recovery_deadline_ms =
        offline_now_ms() + s.media_recovery_backoff_ms;
    if (previous != s.media_recovery_backoff_ms || err != s.media_recovery_last_error) {
        ESP_LOGW(TAG,
             "offline SD media recovery deferred err=%s retry_ms=%lu",
             esp_err_to_name(err),
             (unsigned long)s.media_recovery_backoff_ms);
    }
    s.media_recovery_last_error = err;
}

static void reset_media_recovery_locked(void)
{
    s.media_was_ready = true;
    s.media_recovery_last_error = ESP_OK;
    s.media_recovery_deadline_ms = 0U;
    s.media_recovery_backoff_ms = 0U;
}

static void notify_writer_task(void)
{
    if (s.task) {
        xTaskNotifyGive(s.task);
    }
}

void nuwax_offline_audio_observe_formal_hello(const char *payload, int len,
                                              uint32_t epoch)
{
    if (!payload || len <= 0 || epoch == 0U) {
        return;
    }
    cJSON *root = cJSON_ParseWithLength(payload, (size_t)len);
    const bool supported = nuwax_offline_archive_capability(root);
    atomic_store_explicit(&s_archive_capability_epoch, supported ? epoch : 0U,
                          memory_order_release);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "formal SD replay capability epoch=%lu supported=%d; local storage retained",
             (unsigned long)epoch, supported);
}

bool nuwax_offline_audio_limit_reached(void)
{
    return atomic_load_explicit(&s_offline_limit_reached, memory_order_acquire);
}

/* Shared by formal and test replay workers. A storage-yielded worker publishes
 * this edge so the connection owner can rearm replay after the writer idles. */
static atomic_bool s_replay_retry_required = ATOMIC_VAR_INIT(false);

#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE
static atomic_bool s_replay_test_running = ATOMIC_VAR_INIT(false);
static atomic_uint s_test_retry_deadline_ms;
static atomic_uint s_test_retry_backoff_ms;
static atomic_bool s_replay_test_transport_quarantined =
    ATOMIC_VAR_INIT(false);
/* The transport RX task borrows the client object until owner_close() has
 * returned ESP_OK. Keep this object out of the replay task stack so a
 * transient close timeout can never turn into a use-after-return. */
static nuwax_ws_client_t s_replay_test_ws;
/* Accessed only by the worker serialized through s_replay_test_running. */
static bool s_replay_test_ws_initialized;
#endif

#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
typedef struct {
    nuwax_ws_client_t *ws;
    uint32_t epoch;
} formal_replay_context_t;

static atomic_bool s_formal_replay_running = ATOMIC_VAR_INIT(false);
static atomic_bool s_formal_replay_cancel_requested = ATOMIC_VAR_INIT(false);
static atomic_bool s_shutdown_requested = ATOMIC_VAR_INIT(false);
static formal_replay_context_t s_formal_replay_context;
/* Called by the connection owner (including its exclusive-mode poll hook). */
static uint32_t s_replay_launch_epoch;
static uint32_t s_replay_launch_deadline_ms;
#endif

static uint32_t crc32_update(uint32_t crc, const void *data, size_t bytes)
{
#if defined(ESP_PLATFORM) || defined(NUWAX_CRC_ROM_HOST_TEST)
    /* Identical incremental IEEE CRC-32. ROM code/table avoid competing with
     * the recognizer for flash cache; no extra internal RAM allocation. */
    return esp_rom_crc32_le(crc, (const uint8_t *)data, (uint32_t)bytes);
#else
    /* IEEE CRC-32, same incremental seed and archive bytes. One flash-resident
     * byte lookup replaces two dependent nibble lookups; no internal heap. */
    static const uint32_t table[256] = {
        0x00000000U, 0x77073096U, 0xee0e612cU, 0x990951baU, 0x076dc419U, 0x706af48fU, 0xe963a535U, 0x9e6495a3U,
        0x0edb8832U, 0x79dcb8a4U, 0xe0d5e91eU, 0x97d2d988U, 0x09b64c2bU, 0x7eb17cbdU, 0xe7b82d07U, 0x90bf1d91U,
        0x1db71064U, 0x6ab020f2U, 0xf3b97148U, 0x84be41deU, 0x1adad47dU, 0x6ddde4ebU, 0xf4d4b551U, 0x83d385c7U,
        0x136c9856U, 0x646ba8c0U, 0xfd62f97aU, 0x8a65c9ecU, 0x14015c4fU, 0x63066cd9U, 0xfa0f3d63U, 0x8d080df5U,
        0x3b6e20c8U, 0x4c69105eU, 0xd56041e4U, 0xa2677172U, 0x3c03e4d1U, 0x4b04d447U, 0xd20d85fdU, 0xa50ab56bU,
        0x35b5a8faU, 0x42b2986cU, 0xdbbbc9d6U, 0xacbcf940U, 0x32d86ce3U, 0x45df5c75U, 0xdcd60dcfU, 0xabd13d59U,
        0x26d930acU, 0x51de003aU, 0xc8d75180U, 0xbfd06116U, 0x21b4f4b5U, 0x56b3c423U, 0xcfba9599U, 0xb8bda50fU,
        0x2802b89eU, 0x5f058808U, 0xc60cd9b2U, 0xb10be924U, 0x2f6f7c87U, 0x58684c11U, 0xc1611dabU, 0xb6662d3dU,
        0x76dc4190U, 0x01db7106U, 0x98d220bcU, 0xefd5102aU, 0x71b18589U, 0x06b6b51fU, 0x9fbfe4a5U, 0xe8b8d433U,
        0x7807c9a2U, 0x0f00f934U, 0x9609a88eU, 0xe10e9818U, 0x7f6a0dbbU, 0x086d3d2dU, 0x91646c97U, 0xe6635c01U,
        0x6b6b51f4U, 0x1c6c6162U, 0x856530d8U, 0xf262004eU, 0x6c0695edU, 0x1b01a57bU, 0x8208f4c1U, 0xf50fc457U,
        0x65b0d9c6U, 0x12b7e950U, 0x8bbeb8eaU, 0xfcb9887cU, 0x62dd1ddfU, 0x15da2d49U, 0x8cd37cf3U, 0xfbd44c65U,
        0x4db26158U, 0x3ab551ceU, 0xa3bc0074U, 0xd4bb30e2U, 0x4adfa541U, 0x3dd895d7U, 0xa4d1c46dU, 0xd3d6f4fbU,
        0x4369e96aU, 0x346ed9fcU, 0xad678846U, 0xda60b8d0U, 0x44042d73U, 0x33031de5U, 0xaa0a4c5fU, 0xdd0d7cc9U,
        0x5005713cU, 0x270241aaU, 0xbe0b1010U, 0xc90c2086U, 0x5768b525U, 0x206f85b3U, 0xb966d409U, 0xce61e49fU,
        0x5edef90eU, 0x29d9c998U, 0xb0d09822U, 0xc7d7a8b4U, 0x59b33d17U, 0x2eb40d81U, 0xb7bd5c3bU, 0xc0ba6cadU,
        0xedb88320U, 0x9abfb3b6U, 0x03b6e20cU, 0x74b1d29aU, 0xead54739U, 0x9dd277afU, 0x04db2615U, 0x73dc1683U,
        0xe3630b12U, 0x94643b84U, 0x0d6d6a3eU, 0x7a6a5aa8U, 0xe40ecf0bU, 0x9309ff9dU, 0x0a00ae27U, 0x7d079eb1U,
        0xf00f9344U, 0x8708a3d2U, 0x1e01f268U, 0x6906c2feU, 0xf762575dU, 0x806567cbU, 0x196c3671U, 0x6e6b06e7U,
        0xfed41b76U, 0x89d32be0U, 0x10da7a5aU, 0x67dd4accU, 0xf9b9df6fU, 0x8ebeeff9U, 0x17b7be43U, 0x60b08ed5U,
        0xd6d6a3e8U, 0xa1d1937eU, 0x38d8c2c4U, 0x4fdff252U, 0xd1bb67f1U, 0xa6bc5767U, 0x3fb506ddU, 0x48b2364bU,
        0xd80d2bdaU, 0xaf0a1b4cU, 0x36034af6U, 0x41047a60U, 0xdf60efc3U, 0xa867df55U, 0x316e8eefU, 0x4669be79U,
        0xcb61b38cU, 0xbc66831aU, 0x256fd2a0U, 0x5268e236U, 0xcc0c7795U, 0xbb0b4703U, 0x220216b9U, 0x5505262fU,
        0xc5ba3bbeU, 0xb2bd0b28U, 0x2bb45a92U, 0x5cb36a04U, 0xc2d7ffa7U, 0xb5d0cf31U, 0x2cd99e8bU, 0x5bdeae1dU,
        0x9b64c2b0U, 0xec63f226U, 0x756aa39cU, 0x026d930aU, 0x9c0906a9U, 0xeb0e363fU, 0x72076785U, 0x05005713U,
        0x95bf4a82U, 0xe2b87a14U, 0x7bb12baeU, 0x0cb61b38U, 0x92d28e9bU, 0xe5d5be0dU, 0x7cdcefb7U, 0x0bdbdf21U,
        0x86d3d2d4U, 0xf1d4e242U, 0x68ddb3f8U, 0x1fda836eU, 0x81be16cdU, 0xf6b9265bU, 0x6fb077e1U, 0x18b74777U,
        0x88085ae6U, 0xff0f6a70U, 0x66063bcaU, 0x11010b5cU, 0x8f659effU, 0xf862ae69U, 0x616bffd3U, 0x166ccf45U,
        0xa00ae278U, 0xd70dd2eeU, 0x4e048354U, 0x3903b3c2U, 0xa7672661U, 0xd06016f7U, 0x4969474dU, 0x3e6e77dbU,
        0xaed16a4aU, 0xd9d65adcU, 0x40df0b66U, 0x37d83bf0U, 0xa9bcae53U, 0xdebb9ec5U, 0x47b2cf7fU, 0x30b5ffe9U,
        0xbdbdf21cU, 0xcabac28aU, 0x53b39330U, 0x24b4a3a6U, 0xbad03605U, 0xcdd70693U, 0x54de5729U, 0x23d967bfU,
        0xb3667a2eU, 0xc4614ab8U, 0x5d681b02U, 0x2a6f2b94U, 0xb40bbe37U, 0xc30c8ea1U, 0x5a05df1bU, 0x2d02ef8dU,
    };
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0; i < bytes; ++i) {
        crc = (crc >> 8U) ^ table[(crc ^ p[i]) & 0xffU];
    }
    return ~crc;
#endif
}

static esp_err_t validate_archive_frame(const uint8_t *frame,
                                        size_t frame_len,
                                        const archive_header_t *header,
                                        uint32_t *last_sequence,
                                        bool *sequence_seen)
{
    if (!frame || frame_len == 0U || !header || !last_sequence ||
        !sequence_seen) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t codec = 0U;
    uint8_t flags = 0U;
    size_t encoded_len = 0U;
    if (nuwax_audio_frame_peek(frame, frame_len, &codec, &flags,
                               &encoded_len) != ESP_OK ||
        encoded_len != frame_len || codec != NUWAX_AUDIO_CODEC_PCM16 ||
        (flags & (NUWAX_AUDIO_FLAG_START | NUWAX_AUDIO_FLAG_END)) != 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    const int16_t *samples = NULL;
    size_t sample_count = 0U;
    uint32_t sequence = 0U;
    uint16_t sample_rate = 0U;
    uint8_t channels = 0U;
    if (nuwax_audio_frame_parse_pcm16(frame, frame_len, &samples,
                                      &sample_count, &sequence,
                                      &sample_rate, &channels) != ESP_OK ||
        !samples || channels != header->channels ||
        (uint32_t)sample_rate != header->sample_rate ||
        header->codec != NUWAX_AUDIO_CODEC_PCM16 ||
        header->channels != 1U || header->frame_duration_ms == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint64_t expected_samples =
        ((uint64_t)header->sample_rate * header->frame_duration_ms) / 1000U;
    if (expected_samples == 0U || sample_count != expected_samples) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (*sequence_seen && sequence <= *last_sequence) {
        return ESP_ERR_INVALID_SIZE;
    }
    *last_sequence = sequence;
    *sequence_seen = true;
    return ESP_OK;
}

static void release_slot(spool_item_t *slot)
{
    if (!slot || !s.free_queue ||
        xQueueSend(s.free_queue, &slot, 0) != pdTRUE) {
        ESP_LOGE(TAG, "offline SD slot return failed slot=%p", (void *)slot);
    }
}

static bool enabled(void)
{
#if CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE
    return true;
#else
    return false;
#endif
}

static bool path_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

#include "offline_archive_directory.h"

static bool valid_session_id(const char *session_id)
{
    if (!session_id || session_id[0] == '\0' ||
        strlen(session_id) >= sizeof(s.session_id)) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)session_id;
         *p != '\0'; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
            return false;
        }
    }
    return true;
}

static bool valid_audio_geometry(uint32_t sample_rate,
                                 uint8_t channels,
                                 uint16_t frame_duration_ms,
                                 size_t *frame_samples)
{
    if (sample_rate == 0U || channels != 1U || frame_duration_ms == 0U) {
        return false;
    }
    const uint64_t scaled_samples =
        (uint64_t)sample_rate * (uint64_t)frame_duration_ms;
    if ((scaled_samples % 1000U) != 0U) {
        return false;
    }
    const uint64_t samples = scaled_samples / 1000U;
    if (samples == 0U ||
        samples > (MAX_FRAME_BYTES - NUWAX_AUDIO_FRAME_HEADER_BYTES) /
                      sizeof(int16_t)) {
        return false;
    }
    if (frame_samples) {
        *frame_samples = (size_t)samples;
    }
    return true;
}

/* Warm FatFs free-cluster discovery before admitting any recording frames.
 * On a freshly mounted large card this may scan the FAT for several seconds. */
static esp_err_t prepare_media_space(void)
{
    uint64_t total = 0U, free_bytes = 0U;
    const int64_t started = esp_timer_get_time();
    const esp_err_t err = esp_vfs_fat_info("/sdcard", &total, &free_bytes);
    ESP_LOGI(TAG, "SD space prepared elapsed_ms=%lu free=%llu err=%s",
             (unsigned long)((esp_timer_get_time() - started) / 1000),
             (unsigned long long)free_bytes, esp_err_to_name(err));
    return err;
}

static esp_err_t ensure_directory(void)
{
    struct stat st = {0};
    if (stat("/sdcard", &st) != 0 || !S_ISDIR(st.st_mode)) {
        return ESP_ERR_INVALID_STATE;
    }
    return archive_make_directory(ROOT_DIR) == 0 &&
           archive_make_directory(ARCHIVE_NEW_ROOT) == 0 ? ESP_OK : ESP_FAIL;
}

static uint32_t next_segment_index(void)
{
    archive_dir_t walk;
    archive_dir_t *dir = archive_dir_open(&walk);
    if (!dir) {
        return 0U;
    }
    uint32_t next = 0U;
    struct dirent *entry;
    while ((entry = archive_dir_read(dir)) != NULL) {
        const char *name = entry->d_name;
        const char *underscore = strrchr(name, '_');
        if (!underscore || underscore[1] == '\0') {
            continue;
        }
        char *end = NULL;
        const unsigned long index = strtoul(underscore + 1, &end, 10);
        const bool known_suffix = end &&
            (strcmp(end, ".part") == 0 ||
             strcmp(end, ".ready") == 0 ||
             strcmp(end, ".part.bad") == 0 ||
             strcmp(end, ".ready.bad") == 0);
        if (!known_suffix || index >= UINT32_MAX) {
            continue;
        }
        if ((uint32_t)index >= next) {
            next = (uint32_t)index + 1U;
        }
    }
    if (dir->error) atomic_store_explicit(&s.degraded, true, memory_order_release);
    archive_dir_close(dir);
    return next;
}

static esp_err_t recover_open_segments(void)
{
    archive_dir_t walk;
    archive_dir_t *dir = archive_dir_open(&walk);
    if (!dir) {
        return ESP_FAIL;
    }
    struct dirent *entry;
    bool saw_part = false;
    while ((entry = archive_dir_read(dir)) != NULL) {
        const char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".part") != 0) {
            continue;
        }
        saw_part = true;
        char path[160] = {0};
        char ready_path[160] = {0};
        if (!compose_child_path(path, sizeof(path), entry->d_name)) {
            atomic_store_explicit(&s.degraded, true, memory_order_release);
            ESP_LOGW(TAG, "ignoring overlong stale segment name=%s", entry->d_name);
            continue;
        }
        const size_t path_len = strlen(path);
        const size_t base_len = path_len >= 5U ? path_len - 5U : 0U;
        if (base_len + strlen(".ready") + 1U > sizeof(ready_path)) {
            atomic_store_explicit(&s.degraded, true, memory_order_release);
            ESP_LOGW(TAG, "ignoring overlong recovered segment path=%s", path);
            continue;
        }
        snprintf(ready_path, sizeof(ready_path), "%.*s.ready",
                 (int)base_len, path);
        FILE *file = fopen(path, "r+b");
        if (!file) {
            ESP_LOGW(TAG, "cannot open stale segment for recovery path=%s", path);
            atomic_store_explicit(&s.degraded, true, memory_order_release);
            continue;
        }
        archive_header_t header = {0};
        bool valid = fread(&header, 1U, sizeof(header), file) == sizeof(header) &&
                     memcmp(header.magic, "NWXOFF1", 7U) == 0 &&
                     header.version == 1U && header.header_bytes == HEADER_BYTES;
        uint32_t frame_count = 0U;
        uint64_t payload_bytes = 0U;
        uint32_t crc = 0U;
        uint32_t last_sequence = 0U;
        bool sequence_seen = false;
        long valid_end = (long)sizeof(header);
        _Alignas(int16_t) uint8_t payload[MAX_FRAME_BYTES];
        bool footer_found = false;
        while (valid) {
            const long record_start = ftell(file);
            char marker[8] = {0};
            const size_t marker_read = fread(marker, 1U, sizeof(marker), file);
            if (marker_read < sizeof(marker) && feof(file)) {
                /* Power loss can leave a short footer marker (or a partial
                 * length word) after otherwise valid records.  Truncate at
                 * the last complete record and regenerate the footer below. */
                clearerr(file);
                valid_end = record_start;
                break;
            }
            if (marker_read == sizeof(marker) &&
                memcmp(marker, "NWXEND1", 7U) == 0) {
                archive_footer_t existing_footer = {0};
                memcpy(existing_footer.magic, marker, sizeof(marker));
                const size_t footer_tail =
                    fread(((uint8_t *)&existing_footer) + sizeof(marker),
                          1U, sizeof(existing_footer) - sizeof(marker), file);
                if (footer_tail < sizeof(existing_footer) - sizeof(marker) &&
                    feof(file)) {
                    clearerr(file);
                    valid_end = record_start;
                    break;
                }
                if (footer_tail == sizeof(existing_footer) - sizeof(marker) &&
                    existing_footer.version == 1U &&
                    existing_footer.footer_bytes == FOOTER_BYTES &&
                    existing_footer.frame_count == frame_count &&
                    existing_footer.payload_bytes == payload_bytes &&
                    existing_footer.crc32 == crc) {
                    footer_found = true;
                    valid_end = ftell(file);
                    break;
                }
                valid = false;
                break;
            }
            if (fseek(file, record_start, SEEK_SET) != 0) {
                valid = false;
                break;
            }
            uint32_t len = 0U;
            if (fread(&len, 1U, sizeof(len), file) != sizeof(len)) {
                if (feof(file)) {
                    clearerr(file);
                    valid_end = record_start;
                    break;
                }
                valid = false;
                break;
            }
            if (len == 0U || len > MAX_FRAME_BYTES) {
                valid = false;
                break;
            }
            const size_t payload_read = fread(payload, 1U, len, file);
            if (payload_read != len) {
                /* A power loss during the final PCM record should not erase
                 * the complete prefix already flushed to the card.  Treat a
                 * clean EOF as a truncation point; genuine I/O errors remain
                 * unrecoverable and are discarded below. */
                if (feof(file)) {
                    clearerr(file);
                    valid_end = record_start;
                    break;
                }
                valid = false;
                break;
            }
            if (validate_archive_frame(payload, len, &header, &last_sequence,
                                       &sequence_seen) != ESP_OK) {
                valid = false;
                break;
            }
            crc = crc32_update(crc, payload, len);
            payload_bytes += len;
            if (frame_count < UINT32_MAX) {
                frame_count++;
            }
            valid_end = ftell(file);
        }
        if (valid && footer_found && valid_end >= (long)sizeof(header)) {
            /* A complete footer was written but the atomic rename was
             * interrupted.  Keep the bytes exactly as committed. */
            valid = ftruncate(fileno(file), valid_end) == 0;
        } else if (valid && valid_end >= (long)sizeof(header) &&
            ftruncate(fileno(file), valid_end) == 0 &&
            fseek(file, valid_end, SEEK_SET) == 0) {
            archive_footer_t footer = {0};
            memcpy(footer.magic, "NWXEND1", 7U);
            footer.version = 1U;
            footer.footer_bytes = FOOTER_BYTES;
            footer.frame_count = frame_count;
            footer.payload_bytes = payload_bytes;
            footer.crc32 = crc;
            footer.complete = 0U;
            valid = fwrite(&footer, 1U, sizeof(footer), file) == sizeof(footer) &&
                    fflush(file) == 0 && fsync(fileno(file)) == 0;
        } else {
            valid = false;
        }
        const int close_result = fclose(file);
        if (!valid) {
            atomic_store_explicit(&s.degraded, true, memory_order_release);
            /* Preserve the only local copy.  A transient card read error must
             * never turn into silent data loss during boot recovery.  The
             * quarantine suffix keeps it out of replay while allowing an
             * operator or a later recovery pass to inspect it. */
            char bad_path[176] = {0};
            const bool bad_path_ok =
                close_result == 0 &&
                strlen(path) + strlen(".bad") + 1U < sizeof(bad_path);
            if (bad_path_ok) {
                snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
            }
            if (bad_path_ok && rename(path, bad_path) == 0) {
                ESP_LOGW(TAG,
                         "quarantined unrecoverable stale segment path=%s",
                         bad_path);
            } else {
                ESP_LOGW(TAG,
                         "retained unrecoverable stale segment path=%s close=%d errno=%d",
                         path, close_result, errno);
            }
        } else if (close_result != 0) {
            /* fclose() can report a delayed media flush failure. Keep the
             * source file so the next mount can re-validate its complete
             * prefix instead of deleting potentially recoverable audio. */
            atomic_store_explicit(&s.degraded, true, memory_order_release);
            ESP_LOGW(TAG,
                     "retained stale segment after close failure path=%s errno=%d",
                     path, errno);
        } else if (rename(path, ready_path) != 0) {
            /* A rename failure can be transient (media latency, directory
             * update failure, or an interrupted mount). The .part suffix is
             * itself the recovery marker, so retaining it is crash-safe. */
            atomic_store_explicit(&s.degraded, true, memory_order_release);
            ESP_LOGW(TAG,
                     "retained recovered segment after rename failure path=%s target=%s errno=%d",
                     path, ready_path, errno);
        } else {
            atomic_store_explicit(&s_directory_dirty, true,
                                  memory_order_release);
            ESP_LOGI(TAG,
                     "recovered incomplete offline segment path=%s frames=%lu bytes=%llu",
                     ready_path,
                     (unsigned long)frame_count,
                     (unsigned long long)payload_bytes);
        }
    }
    const int scan_error = dir->error;
    archive_dir_close(dir);
    if (scan_error) return ESP_FAIL;
    if (saw_part) {
        /* Keep a follow-up recovery opportunity for a .part that could not
         * be opened or renamed because the medium was temporarily busy. */
        atomic_store_explicit(&s_directory_dirty, true, memory_order_release);
    }
    return ESP_OK;
}

static uint64_t scan_used_bytes(void)
{
    archive_dir_t walk;
    archive_dir_t *dir = archive_dir_open(&walk);
    if (!dir) {
        return 0U;
    }
    uint64_t total = 0U;
    struct dirent *entry;
    while ((entry = archive_dir_read(dir)) != NULL) {
        const char *dot = strrchr(entry->d_name, '.');
        if (!dot || (strcmp(dot, ".part") != 0 &&
                     strcmp(dot, ".ready") != 0 &&
                     strcmp(dot, ".bad") != 0)) {
            continue;
        }
        char path[160] = {0};
        if (!compose_child_path(path, sizeof(path), entry->d_name)) {
            atomic_store_explicit(&s.degraded, true, memory_order_release);
            ESP_LOGW(TAG, "ignoring overlong archive name=%s", entry->d_name);
            continue;
        }
        struct stat st = {0};
        if (stat(path, &st) == 0 && st.st_size > 0) {
            total += (uint64_t)st.st_size;
        }
    }
    if (dir->error) atomic_store_explicit(&s.degraded, true, memory_order_release);
    archive_dir_close(dir);
    return total;
}

static uint32_t scan_ready_count(void)
{
    archive_dir_t walk;
    archive_dir_t *dir = archive_dir_open(&walk);
    if (!dir) {
        return 0U;
    }
    uint32_t count = 0U;
    struct dirent *entry;
    while ((entry = archive_dir_read(dir)) != NULL) {
        const char *dot = strrchr(entry->d_name, '.');
        if (dot && strcmp(dot, ".ready") == 0 && count < UINT32_MAX) {
            ++count;
        }
    }
    if (dir->error) atomic_store_explicit(&s.degraded, true, memory_order_release);
    archive_dir_close(dir);
    return count;
}

static void log_sd_step(const char *step, int64_t started_us)
{
    const int64_t elapsed = esp_timer_get_time() - started_us;
    if (elapsed >= 200000LL) ESP_LOGW(TAG, "SD latency step=%s elapsed_ms=%lu",
                                    step, (unsigned long)(elapsed / 1000));
}

static bool sync_archive_file(FILE *file, const char *step)
{
    const int64_t started = esp_timer_get_time();
    const bool ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
    log_sd_step(step, started);
    return ok;
}

static esp_err_t open_segment_locked(void)
{
    if (!atomic_load_explicit(&s.active, memory_order_acquire) || s.file) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!nuwax_sd_card_is_mounted()) {
        /* The driver latches an ordered media fault instead of allowing the
         * writer to hammer a removed/read-only card on every queued frame. */
        if (!s.writer_fault_latched) {
            (void)nuwax_device_error_report_enqueue(
                "error", "sd", "media_unavailable",
                ESP_ERR_INVALID_STATE,
                s.mode == NUWAX_OFFLINE_AUDIO_MODE_MONITORING
                    ? "monitoring"
                    : "meeting",
                s.capture_generation, s.session_id);
        }
        nuwax_sd_card_mark_io_fault(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    uint64_t total_bytes = 0U;
    uint64_t free_bytes = 0U;
    const int64_t space_started = esp_timer_get_time();
    const esp_err_t info_err =
        esp_vfs_fat_info("/sdcard", &total_bytes, &free_bytes);
    log_sd_step("free_space", space_started);
    if (info_err != ESP_OK) {
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        ESP_LOGE(TAG,
                 "offline archive free-space query failed err=%s total=%llu free=%llu",
                 esp_err_to_name(info_err),
                 (unsigned long long)total_bytes,
                 (unsigned long long)free_bytes);
        return info_err;
    }
    const uint64_t minimum_free_bytes =
        NUWAX_OFFLINE_AUDIO_FREE_RESERVE_BYTES +
        CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES;
    if (free_bytes < minimum_free_bytes ||
        s.used_bytes > ARCHIVE_QUOTA_BYTES ||
        ARCHIVE_QUOTA_BYTES - s.used_bytes <
            (uint64_t)HEADER_BYTES + FOOTER_BYTES) {
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        ESP_LOGW(TAG,
                 "offline archive storage admission rejected total=%llu free=%llu used=%llu quota=%u minimum_free=%llu",
                 (unsigned long long)total_bytes,
                 (unsigned long long)free_bytes,
                 (unsigned long long)s.used_bytes,
                 (unsigned)ARCHIVE_QUOTA_BYTES,
                 (unsigned long long)minimum_free_bytes);
        return ESP_ERR_NO_MEM;
    }
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    s.segment_index = s.next_index++;
    const int64_t bucket_started = esp_timer_get_time();
    if (!archive_new_paths(s.segment_index, s.session_id, s.open_path, sizeof(s.open_path),
                           s.ready_path, sizeof(s.ready_path))) return ESP_FAIL;
    log_sd_step("bucket_prepare", bucket_started);
    const int64_t lookup_started = esp_timer_get_time();
    const bool exists = path_exists(s.open_path) || path_exists(s.ready_path);
    log_sd_step("path_lookup", lookup_started);
    if (exists) {
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t open_started = esp_timer_get_time();
    s.file = fopen(s.open_path, "wb");
    log_sd_step("file_open", open_started);
    if (!s.file) {
        ESP_LOGE(TAG,
                 "offline archive file open failed path=%s errno=%d",
                 s.open_path, errno);
        return ESP_FAIL;
    }
    if (setvbuf(s.file, s.write_buffer, _IOFBF, ARCHIVE_STDIO_BUFFER_BYTES) != 0) {
        fclose(s.file);
        s.file = NULL;
        return ESP_ERR_NO_MEM;
    }
    archive_header_t header = {0};
    memcpy(header.magic, "NWXOFF1", 7U);
    header.version = 1U;
    header.header_bytes = HEADER_BYTES;
    header.mode = (uint8_t)s.mode;
    header.channels = s.channels;
    header.codec = NUWAX_AUDIO_CODEC_PCM16;
    header.sample_rate = s.sample_rate;
    header.frame_duration_ms = s.frame_duration_ms;
    header.reserved0 = s.mirror ? 1U : 0U;
    header.segment_index = s.segment_index;
    header.start_uptime_ms = now_ms;
    snprintf(header.session_id, sizeof(header.session_id), "%s", s.session_id);
    if (fwrite(&header, 1U, sizeof(header), s.file) != sizeof(header) ||
        !sync_archive_file(s.file, "header_sync")) {
        ESP_LOGE(TAG,
                 "offline archive header write failed path=%s errno=%d",
                 s.open_path, errno);
        fclose(s.file);
        s.file = NULL;
        remove(s.open_path);
        return ESP_FAIL;
    }
    s.used_bytes += sizeof(header);
    s.frame_count = 0U;
    s.payload_bytes = 0U;
    s.payload_crc32 = 0U;
    s.last_good_offset = (long)sizeof(header);
    return ESP_OK;
}

static esp_err_t commit_segment_locked(bool complete)
{
    if (!s.file) {
        return ESP_OK;
    }
    if (s.frame_count == 0U) {
        /* A stop can race capture-task startup (for example a local privacy
         * close immediately after reconnect).  Do not create a replayable
         * zero-frame archive for that race. */
        const int close_result = fclose(s.file);
        s.file = NULL;
        const int remove_result = remove(s.open_path);
        if (s.used_bytes >= sizeof(archive_header_t)) {
            s.used_bytes -= sizeof(archive_header_t);
        }
        s.open_path[0] = '\0';
        s.ready_path[0] = '\0';
        return close_result == 0 && remove_result == 0 ? ESP_OK : ESP_FAIL;
    }
    /* A failed record write may leave a valid length word or a partial PCM
     * payload in the .part file.  Always truncate back to the last complete
     * record before appending the footer so recovery/replay can never expose
     * a torn record. */
    if (s.last_good_offset >= (long)sizeof(archive_header_t) &&
        (atomic_load_explicit(&s.degraded, memory_order_acquire) ||
         ftell(s.file) != s.last_good_offset) &&
        (ftruncate(fileno(s.file), s.last_good_offset) != 0 ||
         fseek(s.file, s.last_good_offset, SEEK_SET) != 0)) {
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        (void)fclose(s.file);
        s.file = NULL;
        return ESP_FAIL;
    }
    archive_footer_t footer = {0};
    memcpy(footer.magic, "NWXEND1", 7U);
    footer.version = 1U;
    footer.footer_bytes = FOOTER_BYTES;
    footer.frame_count = s.frame_count;
    footer.payload_bytes = s.payload_bytes;
    footer.crc32 = s.payload_crc32;
    footer.complete = complete ? 1U : 0U;
    const bool ok = fwrite(&footer, 1U, sizeof(footer), s.file) == sizeof(footer) &&
                    sync_archive_file(s.file, "footer_sync");
    const int close_result = fclose(s.file);
    s.file = NULL;
    if (!ok || close_result != 0) {
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        return ESP_FAIL;
    }
    const int64_t rename_started = esp_timer_get_time();
    const int rename_result = rename(s.open_path, s.ready_path);
    log_sd_step("rename", rename_started);
    if (rename_result != 0) {
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        return ESP_FAIL;
    }
    atomic_store_explicit(&s_directory_dirty, true, memory_order_release);
    s.used_bytes += sizeof(footer);
    (void)atomic_fetch_add_explicit(&s.ready_count, 1U, memory_order_release);
    s.open_path[0] = '\0';
    s.ready_path[0] = '\0';
    return ESP_OK;
}

static esp_err_t append_locked(const spool_item_t *item)
{
    if (!atomic_load_explicit(&s.active, memory_order_acquire) ||
        !item || item->bytes == 0U ||
        item->generation != s.capture_generation) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s.file) {
        const esp_err_t open_err = open_segment_locked();
        if (open_err != ESP_OK) {
            return open_err;
        }
    }
    const uint64_t record_bytes = sizeof(uint32_t) + item->bytes;
    if (s.used_bytes + record_bytes + FOOTER_BYTES > ARCHIVE_QUOTA_BYTES) {
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    if (sizeof(archive_header_t) + s.payload_bytes +
            (uint64_t)s.frame_count * sizeof(uint32_t) + record_bytes + FOOTER_BYTES >
        CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES) {
        if (s.frame_count == 0U) {
            return ESP_ERR_NO_MEM;
        }
        if (commit_segment_locked(!atomic_load_explicit(&s.degraded, memory_order_acquire)) != ESP_OK ||
            open_segment_locked() != ESP_OK) {
            return ESP_FAIL;
        }
    }
    const long record_start = ftell(s.file);
    const uint32_t len = (uint32_t)item->bytes;
    const bool length_ok = record_start >= (long)sizeof(archive_header_t) &&
                           fwrite(&len, 1U, sizeof(len), s.file) == sizeof(len);
    const bool payload_ok = length_ok &&
                            fwrite(item->data, 1U, item->bytes, s.file) == item->bytes;
    if (!payload_ok) {
        clearerr(s.file);
        (void)fflush(s.file);
        if (record_start >= (long)sizeof(archive_header_t)) {
            (void)ftruncate(fileno(s.file), record_start);
            (void)fseek(s.file, record_start, SEEK_SET);
        }
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        return ESP_FAIL;
    }
    s.last_good_offset = ftell(s.file);
    s.frame_count++;
    s.payload_bytes += item->bytes;
    s.payload_crc32 = crc32_update(s.payload_crc32, item->data, item->bytes);
    s.used_bytes += record_bytes;
    if ((s.frame_count % DURABILITY_SYNC_INTERVAL_FRAMES) == 0U &&
        !sync_archive_file(s.file, "periodic_sync")) {
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void writer_schedule_retry_locked(esp_err_t err)
{
    atomic_store_explicit(&s.writer_error, true, memory_order_release);
    atomic_store_explicit(&s.degraded, true, memory_order_release);
    /* A failed write/flush/rename may leave a recoverable .part or a
     * directory entry whose metadata is no longer reflected by the cached
     * counters. Force the next idle supervisor pass to rescan. */
    atomic_store_explicit(&s_directory_dirty, true, memory_order_release);
    if (s.writer_retry_backoff_ms == 0U) {
        s.writer_retry_backoff_ms = WRITER_RETRY_INITIAL_MS;
    } else if (s.writer_retry_backoff_ms < WRITER_RETRY_MAX_MS / 2U) {
        s.writer_retry_backoff_ms *= 2U;
    } else {
        s.writer_retry_backoff_ms = WRITER_RETRY_MAX_MS;
    }
    s.writer_retry_deadline_ms = offline_now_ms() + s.writer_retry_backoff_ms;
    if (err != ESP_ERR_NO_MEM && err != ESP_ERR_NOT_FINISHED &&
        err != ESP_ERR_INVALID_STATE) {
        atomic_store_explicit(&s_media_ready, false, memory_order_release);
        nuwax_sd_card_mark_io_fault(err);
    }
    if (!s.writer_fault_latched) {
        s.writer_fault_latched = true;
        (void)nuwax_device_error_report_enqueue_detail(
            "error", "sd", "writer_backoff", err,
            esp_err_to_name(err),
            s.mode == NUWAX_OFFLINE_AUDIO_MODE_MONITORING
                ? "monitoring"
                : "meeting",
            s.capture_generation, s.session_id);
        ESP_LOGW(TAG,
                 "offline SD writer entering backoff err=%s retry_ms=%lu",
                 esp_err_to_name(err),
                 (unsigned long)s.writer_retry_backoff_ms);
    }
}

static bool writer_retry_due_locked(void)
{
    return atomic_load_explicit(&s.writer_error, memory_order_acquire) &&
           offline_deadline_elapsed(offline_now_ms(),
                                    s.writer_retry_deadline_ms);
}

static bool writer_try_recover_locked(void)
{
    if (!atomic_load_explicit(&s.writer_error, memory_order_acquire)) {
        return true;
    }
    if (!writer_retry_due_locked()) {
        return false;
    }
    /* A failed footer/rename may leave a recoverable .part.  Re-scan once per
     * backoff interval before opening a new segment so the only local copy is
     * not stranded until the next reboot. */
    (void)recover_open_segments();
    s.used_bytes = scan_used_bytes();
    atomic_store_explicit(&s.ready_count, scan_ready_count(),
                          memory_order_release);
    const esp_err_t recover_err =
        s.file ? ESP_OK : open_segment_locked();
    if (recover_err == ESP_OK) {
        atomic_store_explicit(&s.writer_error, false, memory_order_release);
        s.writer_retry_deadline_ms = 0U;
        s.writer_retry_backoff_ms = 0U;
        s.writer_fault_latched = false;
        ESP_LOGI(TAG, "offline SD writer recovered after backoff");
        return true;
    }
    writer_schedule_retry_locked(recover_err);
    return false;
}

static void writer_task(void *arg)
{
    (void)arg;
    uint32_t processed_frames = 0U;
    offline_io_budget_t budget = {.since_us = esp_timer_get_time()};
    ESP_LOGI(TAG,
             "offline SD writer started stack_min_free=%u stack_bytes=%u priority=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)OFFLINE_WRITER_TASK_STACK_BYTES, (unsigned)OFFLINE_IO_TASK_PRIORITY);
    for (;;) {
        /* A recovery owner may be waiting to replace the mounted medium. The
         * queue itself remains allocated, so pausing before dequeue keeps
         * recovery from acquiring a new in-flight FILE operation. */
        while (atomic_load_explicit(&s.media_recovery_requested,
                                    memory_order_acquire)) {
            TickType_t recovery_tick = pdMS_TO_TICKS(1);
            if (recovery_tick == 0) {
                recovery_tick = 1;
            }
            vTaskDelay(recovery_tick);
        }
        if (!s.writer_gate ||
            xSemaphoreTake(s.writer_gate,
                           pdMS_TO_TICKS(WRITER_DEQUEUE_WAIT_MS)) != pdTRUE) {
            TickType_t retry_tick = pdMS_TO_TICKS(1);
            if (retry_tick == 0) {
                retry_tick = 1;
            }
            vTaskDelay(retry_tick);
            continue;
        }
        spool_item_t *item = NULL;
        const BaseType_t received = xQueueReceive(s.queue, &item, 0);
        spool_item_t terminal = {0};
        if (received != pdTRUE &&
            atomic_load_explicit(&s.closing, memory_order_acquire) &&
            xSemaphoreTake(s.admission_lock, 0) == pdTRUE) {
            /* Admission is fenced before closing is published. Recheck under
             * the admission lock to include a producer already copying a frame.
             * An out-of-band terminal cannot be starved by a full data queue. */
            if (uxQueueMessagesWaiting(s.queue) == 0U) {
                terminal.type = 2U;
                terminal.generation = s.capture_generation;
                terminal.complete = atomic_load_explicit(
                    &s_close_complete, memory_order_acquire) ? 1U : 0U;
                item = &terminal;
            }
            xSemaphoreGive(s.admission_lock);
        }
        if (received != pdTRUE && !item) {
            xSemaphoreGive(s.writer_gate);
            ulTaskNotifyTake(pdTRUE,
                atomic_load_explicit(&s.closing, memory_order_acquire)
                    ? pdMS_TO_TICKS(10U) : portMAX_DELAY);
            continue;
        }
        atomic_store_explicit(&s.writer_busy, true, memory_order_release);
        xSemaphoreGive(s.writer_gate);
        if (!item) {
            atomic_store_explicit(&s.writer_error, true, memory_order_release);
            atomic_store_explicit(&s.degraded, true, memory_order_release);
            ESP_LOGE(TAG, "offline SD writer received null queue slot");
            atomic_store_explicit(&s.writer_busy, false, memory_order_release);
            continue;
        }
        if (item->type == 2U) {
            if (xSemaphoreTake(s.lock, portMAX_DELAY) != pdTRUE) {
                (void)xEventGroupSetBits(s.events, FAILED_BIT);
                atomic_store_explicit(&s.writer_busy, false, memory_order_release);
                continue;
            }
            const bool stale = item->generation != s.capture_generation;
            const esp_err_t close_err = stale
                                            ? ESP_ERR_INVALID_STATE
                                            : commit_segment_locked(
                                                  item->complete != 0U &&
                                                  atomic_load_explicit(&s_close_complete,
                                                                       memory_order_acquire) &&
                                                  !atomic_load_explicit(&s.degraded,
                                                                        memory_order_acquire) &&
                                                  !atomic_load_explicit(
                                                      &s.writer_error,
                                                      memory_order_acquire));
            if (!stale) {
                ESP_LOGI(TAG,
                         "offline SD session closed session=%s dropped_frames=%lu degraded=%d complete=%d err=%s",
                         s.session_id,
                         (unsigned long)atomic_load_explicit(&s.dropped_frames, memory_order_relaxed),
                         atomic_load_explicit(&s.degraded, memory_order_acquire) ? 1 : 0,
                         close_err == ESP_OK && item->complete != 0U &&
                         atomic_load_explicit(&s_close_complete, memory_order_acquire) &&
                         !atomic_load_explicit(&s.degraded, memory_order_acquire) &&
                         !atomic_load_explicit(&s.writer_error, memory_order_acquire),
                         esp_err_to_name(close_err));
                /* The close transaction owns the generation fence.  Clearing
                 * the lifecycle flags here also recovers from a waiter
                 * timeout: the writer cannot leave a completed footer
                 * permanently blocking the next offline session. */
                s.stop_enqueued = false;
                atomic_store_explicit(&s.closing, false, memory_order_release);
                atomic_store_explicit(&s.active, false, memory_order_release);
            }
            /* Publish completion while holding the generation lock. A new
             * begin must not clear its events before this old writer signals. */
            (void)xEventGroupSetBits(s.events,
                                     close_err == ESP_OK ? DONE_BIT : FAILED_BIT);
            xSemaphoreGive(s.lock);
            atomic_store_explicit(&s.writer_busy, false, memory_order_release);
            continue;
        }
        if (item->type != 1U) {
            release_slot(item);
            atomic_store_explicit(&s.writer_busy, false, memory_order_release);
            continue;
        }
        if (xSemaphoreTake(s.lock, portMAX_DELAY) != pdTRUE) {
            atomic_store_explicit(&s.degraded, true, memory_order_release);
            release_slot(item);
            atomic_store_explicit(&s.writer_busy, false, memory_order_release);
            continue;
        }
        const int64_t write_started_us = esp_timer_get_time();
        esp_err_t err = ESP_OK;
        if (!writer_try_recover_locked()) {
            err = ESP_ERR_NOT_FINISHED;
        } else {
            err = append_locked(item);
        }
        if (err != ESP_OK) {
            (void)atomic_fetch_add_explicit(&s.dropped_frames, 1U,
                                            memory_order_relaxed);
            atomic_store_explicit(&s.capture_complete, false,
                                  memory_order_release);
            if (!atomic_load_explicit(&s.writer_error, memory_order_acquire)) {
                /* Quarantine the failed segment and schedule one bounded
                 * recovery attempt.  The current frame is intentionally
                 * dropped; reopening must never replay a torn record. */
                if (s.file) {
                    (void)commit_segment_locked(false);
                }
                writer_schedule_retry_locked(err);
            }
        }
        const int64_t write_us = esp_timer_get_time() - write_started_us;
        if (write_us >= 200000LL) {
            ESP_LOGW(TAG, "SD writer slow operation elapsed_ms=%lu queue=%u err=%s",
                     (unsigned long)(write_us / 1000),
                     (unsigned)uxQueueMessagesWaiting(s.queue), esp_err_to_name(err));
        }
        const size_t work_bytes = item->bytes;
        xSemaphoreGive(s.lock);
        release_slot(item);
        atomic_store_explicit(&s.writer_busy, false, memory_order_release);
        ++processed_frames;
        if (offline_io_budget_due(&budget, work_bytes, esp_timer_get_time())) {
            vTaskDelay(1U);
            offline_io_budget_reset(&budget, esp_timer_get_time());
        }
        if ((processed_frames % DURABILITY_SYNC_INTERVAL_FRAMES) == 0U) {
            ESP_LOGI(TAG,
                     "offline SD writer progress frames=%lu queue=%u free_slots=%u stack_min_free=%u err=%s dropped_frames=%lu",
                     (unsigned long)processed_frames,
                     (unsigned)uxQueueMessagesWaiting(s.queue),
                     (unsigned)uxQueueMessagesWaiting(s.free_queue),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL),
                     esp_err_to_name(err),
                     (unsigned long)atomic_load_explicit(&s.dropped_frames, memory_order_relaxed));
        }
    }
}

static void destroy_unpublished_runtime(void)
{
    if (s.queue) vQueueDeleteWithCaps(s.queue);
    if (s.free_queue) vQueueDeleteWithCaps(s.free_queue);
    if (s.slot_pool) heap_caps_free(s.slot_pool);
    if (s.write_buffer) heap_caps_free(s.write_buffer);
    if (s.events) vEventGroupDelete(s.events);
    if (s.capture_events) vEventGroupDelete(s.capture_events);
    if (s.lock) vSemaphoreDelete(s.lock);
    if (s.admission_lock) vSemaphoreDelete(s.admission_lock);
    if (s.writer_gate) vSemaphoreDelete(s.writer_gate);
    memset(&s, 0, sizeof(s));
}

esp_err_t nuwax_offline_audio_init(void)
{
    if (!enabled()) {
        return ESP_OK;
    }
    if (ensure_init_lock() != ESP_OK ||
        xSemaphoreTake(s_init_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    if (runtime_initialized()) {
        const bool mounted = nuwax_sd_card_is_mounted();
        xSemaphoreGive(s_init_lock);
        /* A runtime media fault latches the SD driver unhealthy without
         * tearing down active FILE users. Once those users are gone, the
         * ordered recovery gate can remount a card that was reinserted. */
        return mounted
                   ? ESP_OK
                   : nuwax_offline_audio_recover_if_idle(
                         NUWAX_OFFLINE_AUDIO_RECOVERY_TIMEOUT_MS);
    }
    if (!runtime_memory_ready("init")) {
        xSemaphoreGive(s_init_lock);
        return ESP_ERR_NO_MEM;
    }

    /* Allocate the bounded queue and writer independently from media
     * presence. This prevents a card inserted during a live meeting from
     * requiring a large PSRAM allocation on the audio frame path. */
    memset(&s, 0, sizeof(s));
    atomic_init(&s.active, false);
    atomic_init(&s.closing, false);
    atomic_init(&s.degraded, false);
    atomic_init(&s.writer_error, false);
    atomic_init(&s.writer_busy, false);
    atomic_init(&s.media_recovery_requested, false);
    atomic_init(&s.dropped_frames, 0U);
    atomic_init(&s.ready_count, 0U);
    atomic_init(&s.capture_stop, false);
    atomic_init(&s.capture_complete, true);
    atomic_init(&s.capture_running, false);
    atomic_init(&s.replay_running, false);
    s.slot_pool = heap_caps_calloc(QUEUE_DEPTH, sizeof(spool_item_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.write_buffer = heap_caps_malloc(ARCHIVE_STDIO_BUFFER_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.queue = xQueueCreateWithCaps(QUEUE_DEPTH, sizeof(spool_item_t *),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.free_queue = xQueueCreateWithCaps(QUEUE_DEPTH, sizeof(spool_item_t *),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.events = xEventGroupCreate();
    s.capture_events = xEventGroupCreate();
    s.lock = xSemaphoreCreateMutex();
    s.admission_lock = xSemaphoreCreateMutex();
    s.writer_gate = xSemaphoreCreateMutexStatic(&s_writer_gate_storage);
    if (!s.write_buffer || !s.slot_pool || !s.queue || !s.free_queue || !s.events ||
        !s.capture_events || !s.lock || !s.admission_lock ||
        !s.writer_gate) {
        destroy_unpublished_runtime();
        xSemaphoreGive(s_init_lock);
        return ESP_ERR_NO_MEM;
    }
    for (uint32_t i = 0U; i < QUEUE_DEPTH; ++i) {
        spool_item_t *slot = &s.slot_pool[i];
        if (xQueueSend(s.free_queue, &slot, 0) != pdTRUE) {
            destroy_unpublished_runtime();
            xSemaphoreGive(s_init_lock);
            return ESP_ERR_NO_MEM;
        }
    }

    nuwax_sd_card_info_t info = {0};
    esp_err_t media_result = nuwax_sd_card_mount(&info);
    if (media_result == ESP_OK) {
        media_result = ensure_directory();
        if (media_result == ESP_OK) media_result = prepare_media_space();
    }
    if (media_result == ESP_OK) {
        media_result = recover_open_segments();
    }
    if (media_result == ESP_OK) {
        s.next_index = next_segment_index();
        s.used_bytes = scan_used_bytes();
        atomic_store_explicit(&s.ready_count, scan_ready_count(),
                              memory_order_release);
    } else {
        if (nuwax_sd_card_is_mounted()) {
            (void)nuwax_sd_card_unmount();
        }
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        ESP_LOGW(TAG,
                 "offline SD media unavailable during runtime initialization err=%s; supervisor will retry",
                 esp_err_to_name(media_result));
        /* Initialization already probed the medium. Avoid a duplicate probe
         * during Wi-Fi/model startup; no-card boot is an optional degradation. */
        schedule_media_recovery_locked(media_result);
    }
    if (media_result == ESP_OK) reset_media_recovery_locked();

    if (xTaskCreatePinnedToCoreWithCaps(writer_task, "offline_sd_writer",
                                        OFFLINE_WRITER_TASK_STACK_BYTES, NULL, OFFLINE_IO_TASK_PRIORITY,
                                        &s.task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        if (nuwax_sd_card_is_mounted()) {
            (void)nuwax_sd_card_unmount();
        }
        destroy_unpublished_runtime();
        xSemaphoreGive(s_init_lock);
        return ESP_ERR_NO_MEM;
    }
    atomic_store_explicit(&s_runtime_initialized, true,
                          memory_order_release);
    atomic_store_explicit(&s_directory_dirty,
                          atomic_load_explicit(&s.degraded,
                                               memory_order_acquire),
                          memory_order_release);
    atomic_store_explicit(&s_media_ready, media_result == ESP_OK,
                          memory_order_release);
    xSemaphoreGive(s_init_lock);
    ESP_LOGI(TAG,
             "offline SD archive runtime ready media=%d capacity=%llu segment_limit=%u quota=%u queue_frames=%u durability_frames=%u",
             media_result == ESP_OK ? 1 : 0,
             (unsigned long long)info.capacity_bytes,
             (unsigned)CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES,
             (unsigned)ARCHIVE_QUOTA_BYTES,
             (unsigned)QUEUE_DEPTH,
             (unsigned)DURABILITY_SYNC_INTERVAL_FRAMES);
    nuwax_offline_audio_supervisor_wake();
    return media_result;
}

static uint32_t recovery_remaining_ms(bool unlimited, int64_t deadline_us)
{
    if (unlimited) {
        return UINT32_MAX;
    }
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0;
    }
    uint64_t remaining_ms =
        ((uint64_t)remaining_us + 999ULL) / 1000ULL;
    if (remaining_ms > UINT32_MAX) {
        remaining_ms = UINT32_MAX;
    }
    return (uint32_t)remaining_ms;
}

static void offline_storage_supervisor_task(void *arg)
{
    (void)arg;
    uint32_t delay_ms = OFFLINE_STORAGE_ABSENT_RETRY_INITIAL_MS;
    for (;;) {
        esp_err_t err = runtime_initialized()
            ? nuwax_offline_audio_recover_if_idle(250U)
            : nuwax_offline_audio_init();
        if (err == ESP_OK) {
            delay_ms = OFFLINE_STORAGE_PROBE_INTERVAL_MS;
        } else if (err == ESP_ERR_NOT_FINISHED || err == ESP_ERR_TIMEOUT) {
            delay_ms = OFFLINE_STORAGE_BUSY_RETRY_MS;
        } else {
            delay_ms = delay_ms < OFFLINE_STORAGE_ABSENT_RETRY_MAX_MS / 2U
                ? delay_ms * 2U : OFFLINE_STORAGE_ABSENT_RETRY_MAX_MS;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
    }
}

/* The real-time producer retains PCM before the FILE generation can attach.
 * Queue rejection is backpressure, not loss, until this bounded staging fills. */
typedef struct {
    uint32_t sequence;
    uint32_t timestamp_ms;
    uint8_t flags;
    int16_t samples[320];
} mirror_staged_frame_t;

static esp_err_t append_frame(const nuwax_offline_audio_frame_t *frame,
                              TickType_t wait, bool count_loss);

static void mirror_note_loss(nuwax_offline_audio_mirror_t *m, uint32_t frames)
{
    m->rejected_frames += frames;
    m->complete = false;
    if (m->rejected_frames == frames ||
        offline_now_ms() - m->last_drop_log_ms >= 1000U) {
        m->last_drop_log_ms = offline_now_ms();
        ESP_LOGW(TAG, "SD staging overflow/unavailable session=%s lost=%lu retained=%u",
                 m->session_id, (unsigned long)m->rejected_frames,
                 (unsigned)m->staging_count);
    }
}

static void mirror_drain(nuwax_offline_audio_mirror_t *m, bool closing)
{
    if (!m->started || !m->staging_count) return;
    if (!m->attached) {
        if ((!closing && atomic_load_explicit(&s_mirror_closers, memory_order_acquire)) ||
            (m->next_service_ms && !offline_deadline_elapsed(offline_now_ms(), m->next_service_ms))) return;
        m->next_service_ms = offline_now_ms() + OFFLINE_MIRROR_ATTACH_RETRY_MS;
        m->last_error = begin_archive(m->mode, m->session_id, 16000U, 1U, 20U,
                                     !m->offline, true);
        if (m->last_error != ESP_OK) return;
        m->attached = true;
    }
    mirror_staged_frame_t *ring = m->staging;
    /* Cap frame-path work. A close worker repeats this outside MIC ownership. */
    for (unsigned n = 0; n < 8U && m->staging_count; ++n) {
        const mirror_staged_frame_t *f = &ring[m->staging_head];
        const nuwax_offline_audio_frame_t frame = {
            .sequence=f->sequence, .timestamp_ms=f->timestamp_ms,
            .flags=f->flags, .samples=f->samples, .sample_count=320U,
        };
        m->last_error = append_frame(&frame, 0, false);
        if (m->last_error != ESP_OK) {
            if (m->last_error != ESP_ERR_NOT_FINISHED) {
                /* Release a failed FILE generation so the idle storage owner
                 * can remount/recover. Retain this unsent PCM for reattachment. */
                (void)nuwax_offline_audio_request_end(false);
                m->attached = false;
                m->complete = false;
                nuwax_offline_audio_supervisor_wake();
            }
            return;
        }
        m->staging_head = (m->staging_head + 1U) % MIRROR_STAGING_FRAMES;
        --m->staging_count;
        ++m->accepted_frames;
    }
}

static void mirror_finish(nuwax_offline_audio_mirror_t *m)
{
    if (m->staging_count) {
        mirror_note_loss(m, m->staging_count);
        m->staging_count = 0U;
    }
    if (m->attached) {
        (void)nuwax_offline_audio_request_end(m->complete && !m->rejected_frames);
    }
    ESP_LOGI(TAG, "SD mirror admission session=%s accepted=%lu rejected=%lu complete=%d; persistence follows writer close",
             m->session_id, (unsigned long)m->accepted_frames,
             (unsigned long)m->rejected_frames, m->complete && !m->rejected_frames);
    heap_caps_free(m->staging);
    m->staging = NULL;
    m->started = m->attached = false;
}

static void mirror_close_task(void *arg)
{
    nuwax_offline_audio_mirror_t *m = arg;
    const uint32_t deadline = offline_now_ms() + MIRROR_CLOSE_TIMEOUT_MS;
    while (m->staging_count && !offline_deadline_elapsed(offline_now_ms(), deadline)) {
        mirror_drain(m, true);
        if (m->staging_count) vTaskDelay(pdMS_TO_TICKS(20U));
    }
    mirror_finish(m);
    heap_caps_free(m);
    atomic_fetch_sub_explicit(&s_mirror_closers, 1U, memory_order_release);
    nuwax_offline_audio_supervisor_wake();
    vTaskDeleteWithCaps(NULL);
}

void nuwax_offline_audio_mirror_reset(nuwax_offline_audio_mirror_t *mirror)
{
    if (mirror) memset(mirror, 0, sizeof(*mirror));
}

bool nuwax_offline_audio_mirror_started(const nuwax_offline_audio_mirror_t *mirror)
{
    return mirror && mirror->started;
}

esp_err_t nuwax_offline_audio_mirror_request_close(
    nuwax_offline_audio_mirror_t *m, bool complete, uint32_t now_ms)
{
    (void)now_ms;
    if (!m) return ESP_ERR_INVALID_ARG;
    if (!m->started) {
        if (m->media_deferred) {
            ESP_LOGI(TAG, "SD mirror closed without staging session=%s skipped=%lu media_unavailable=1",
                     m->session_id, (unsigned long)m->rejected_frames);
            const bool offline = m->offline;
            memset(m, 0, sizeof(*m));
            m->offline = offline;
        }
        return ESP_OK;
    }
    m->complete = m->complete && complete;
    mirror_drain(m, false);
    if (m->staging_count && atomic_load_explicit(&s_mirror_closers, memory_order_acquire) < 2U) {
        nuwax_offline_audio_mirror_t *tail = heap_caps_malloc(sizeof(*tail), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (tail) {
            *tail = *m;
            atomic_fetch_add_explicit(&s_mirror_closers, 1U, memory_order_release);
            if (xTaskCreatePinnedToCoreWithCaps(mirror_close_task, "sd_mirror_close",
                    4096U, tail, 3U, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
                const bool offline = m->offline;
                memset(m, 0, sizeof(*m));
                m->offline = offline;
                return ESP_OK;
            }
            atomic_fetch_sub_explicit(&s_mirror_closers, 1U, memory_order_release);
            heap_caps_free(tail);
        }
    }
    mirror_finish(m);
    nuwax_offline_audio_supervisor_wake();
    return ESP_OK;
}

esp_err_t nuwax_offline_audio_mirror_service(
    nuwax_offline_audio_mirror_t *m, nuwax_offline_audio_mode_t mode,
    const char *session_id, uint32_t sample_rate, uint8_t channels,
    uint16_t frame_duration_ms, uint32_t now_ms)
{
    (void)now_ms;
    if (!m || !valid_session_id(session_id) || sample_rate != 16000U ||
        channels != 1U || frame_duration_ms != 20U) return ESP_ERR_INVALID_ARG;
    if (!m->started) {
        /* Only new staging admission is gated. An existing buffer must survive
         * transient media recovery and retain its original bounded close grace. */
        if (!atomic_load_explicit(&s_media_ready, memory_order_acquire)) {
            if (!m->media_deferred) {
                snprintf(m->session_id, sizeof(m->session_id), "%s", session_id);
                ESP_LOGW(TAG, "SD mirror staging deferred: media not ready session=%s revision=media-admission-v1",
                         session_id);
            }
            m->media_deferred = true;
            m->complete = false;
            m->last_error = ESP_ERR_NOT_FINISHED;
            return ESP_ERR_NOT_FINISHED;
        }
        if (!m->staging) m->staging = heap_caps_calloc(MIRROR_STAGING_FRAMES,
            sizeof(mirror_staged_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!m->staging) {
            if (!m->fault_latched) ESP_LOGW(TAG, "SD staging allocation unavailable session=%s", session_id);
            m->fault_latched = true;
            return ESP_ERR_NO_MEM;
        }
        m->mode = mode;
        snprintf(m->session_id, sizeof(m->session_id), "%s", session_id);
        m->staging_head = m->staging_count = 0U;
        m->next_service_ms = 0U;
        m->complete = m->rejected_frames == 0U && !m->media_deferred;
        m->media_deferred = false;
        m->attached = false;
        m->started = true;
    }
    mirror_drain(m, false);
    return ESP_OK;
}

esp_err_t nuwax_offline_audio_mirror_append(
    nuwax_offline_audio_mirror_t *m,
    const nuwax_offline_audio_frame_t *frame, uint32_t now_ms)
{
    (void)now_ms;
    if (!m || !frame || !frame->samples || frame->sample_count != 320U) return ESP_ERR_INVALID_ARG;
    if (!m->started) {
        if (m->media_deferred) {
            /* Known unavailability is reported once on admission, not as a
             * per-second overflow of a buffer that was never allocated. */
            ++m->rejected_frames;
            m->complete = false;
            return ESP_ERR_NOT_FINISHED;
        }
        mirror_note_loss(m, 1U);
        return ESP_ERR_NOT_FINISHED;
    }
    mirror_drain(m, false);
    if (m->staging_count == MIRROR_STAGING_FRAMES) {
        mirror_note_loss(m, 1U);
        return ESP_ERR_NOT_FINISHED;
    }
    mirror_staged_frame_t *ring = m->staging;
    mirror_staged_frame_t *f = &ring[(m->staging_head + m->staging_count) % MIRROR_STAGING_FRAMES];
    f->sequence = frame->sequence;
    f->timestamp_ms = frame->timestamp_ms;
    f->flags = frame->flags;
    memcpy(f->samples, frame->samples, sizeof(f->samples));
    ++m->staging_count;
    mirror_drain(m, false);
    return ESP_OK;
}

static TickType_t recovery_remaining_ticks(bool unlimited,
                                           int64_t deadline_us)
{
    const uint32_t remaining_ms =
        recovery_remaining_ms(unlimited, deadline_us);
    if (remaining_ms == UINT32_MAX) {
        return portMAX_DELAY;
    }
    if (remaining_ms == 0U) {
        return 0;
    }
    TickType_t ticks = pdMS_TO_TICKS(remaining_ms);
    if (ticks == 0) {
        ticks = 1;
    }
    return ticks;
}

esp_err_t nuwax_offline_audio_recover_if_idle(uint32_t timeout_ms)
{
    if (nuwax_power_ctrl_shutdown_preparing()) return ESP_ERR_INVALID_STATE;
    if (!enabled()) {
        return ESP_OK;
    }
    if (!runtime_initialized()) {
        return nuwax_offline_audio_init();
    }
    if (atomic_load_explicit(&s.active, memory_order_acquire) ||
        nuwax_offline_audio_replay_running()) {
        return ESP_ERR_NOT_FINISHED;
    }
    if (!s.admission_lock || !s.lock || !s.writer_gate || !s.queue) {
        return ESP_ERR_INVALID_STATE;
    }

    const bool unlimited = timeout_ms == UINT32_MAX;
    const int64_t deadline_us = unlimited
                                    ? INT64_MAX
                                    : esp_timer_get_time() +
                                          (int64_t)timeout_ms * 1000LL;
    if (xSemaphoreTake(s.admission_lock,
                       recovery_remaining_ticks(unlimited, deadline_us)) !=
        pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Keep the admission gate for the entire quiesce wait. This prevents a
     * realtime mirror append from creating a new queue item while the writer
     * is being drained and the media instance is replaced. */
    bool recovery_gate_set = false;
    bool storage_locks_held = false;
    esp_err_t result = ESP_ERR_NOT_FINISHED;
    for (;;) {
        if (xSemaphoreTake(
                s.writer_gate,
                recovery_remaining_ticks(unlimited, deadline_us)) != pdTRUE) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        if (xSemaphoreTake(
                s.lock,
                recovery_remaining_ticks(unlimited, deadline_us)) != pdTRUE) {
            xSemaphoreGive(s.writer_gate);
            result = ESP_ERR_TIMEOUT;
            break;
        }

        if (nuwax_power_ctrl_shutdown_preparing()) {
            xSemaphoreGive(s.lock);
            xSemaphoreGive(s.writer_gate);
            break;
        }
        const bool lifecycle_active =
            atomic_load_explicit(&s.active, memory_order_acquire) ||
            atomic_load_explicit(&s.closing, memory_order_acquire) ||
            nuwax_offline_audio_replay_running() || s.file != NULL;
        const bool queue_busy = uxQueueMessagesWaiting(s.queue) != 0U;
        if (!recovery_gate_set) {
            if (lifecycle_active) {
                xSemaphoreGive(s.lock);
                xSemaphoreGive(s.writer_gate);
                result = ESP_ERR_NOT_FINISHED;
                break;
            }
            if (queue_busy) {
                /* admission_lock already prevents new producers. Leave the
                 * recovery flag clear until the persistent writer consumes
                 * the residual queue; setting it here would make the writer
                 * wait for recovery while recovery waits for the queue. */
                notify_writer_task();
                xSemaphoreGive(s.lock);
                xSemaphoreGive(s.writer_gate);
                if (!unlimited && esp_timer_get_time() >= deadline_us) {
                    result = ESP_ERR_TIMEOUT;
                    break;
                }
                TickType_t drain_tick = pdMS_TO_TICKS(2);
                if (drain_tick == 0) {
                    drain_tick = 1;
                }
                vTaskDelay(drain_tick);
                continue;
            }
            /* Set the gate before releasing s.lock so a writer that is still
             * blocked in xQueueReceive cannot start a new dequeue after this
             * point. begin_archive() also rejects while the gate is set. */
            atomic_store_explicit(&s.media_recovery_requested, true,
                                  memory_order_release);
            recovery_gate_set = true;
        }

        if (!lifecycle_active && !queue_busy &&
            !atomic_load_explicit(&s.writer_busy, memory_order_acquire) &&
            !nuwax_offline_audio_replay_running()) {
            /* Keep both locks held through the probe/remount below. */
            storage_locks_held = true;
            break;
        }
        if (queue_busy) {
            /* A notification can have been consumed just before a recovery
             * request arrived. Nudge the persistent writer so queued items
             * drain instead of waiting for a later producer event. */
            notify_writer_task();
        }
        xSemaphoreGive(s.lock);
        xSemaphoreGive(s.writer_gate);
        if (!unlimited && esp_timer_get_time() >= deadline_us) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        TickType_t quiesce_tick = pdMS_TO_TICKS(2);
        if (quiesce_tick == 0) {
            quiesce_tick = 1;
        }
        vTaskDelay(quiesce_tick);
    }

    if (result == ESP_ERR_TIMEOUT) {
        if (recovery_gate_set) {
            atomic_store_explicit(&s.media_recovery_requested, false,
                                  memory_order_release);
        }
        xSemaphoreGive(s.admission_lock);
        return result;
    }
    if (!storage_locks_held || !recovery_gate_set) {
        atomic_store_explicit(&s.media_recovery_requested, false,
                              memory_order_release);
        xSemaphoreGive(s.admission_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    const bool final_users_active =
        atomic_load_explicit(&s.active, memory_order_acquire) ||
        atomic_load_explicit(&s.closing, memory_order_acquire) ||
        nuwax_offline_audio_replay_running() ||
        atomic_load_explicit(&s.writer_busy, memory_order_acquire) ||
        s.file != NULL ||
        uxQueueMessagesWaiting(s.queue) != 0U;
    if (final_users_active) {
        xSemaphoreGive(s.lock);
        xSemaphoreGive(s.writer_gate);
        atomic_store_explicit(&s.media_recovery_requested, false,
                              memory_order_release);
        xSemaphoreGive(s.admission_lock);
        return ESP_ERR_NOT_FINISHED;
    }

    atomic_store_explicit(&s_media_ready, false, memory_order_release);

    /* A card can be removed and reinserted entirely while the product is
     * idle, so the driver's health bit may still be true. Probe only while
     * the recovery gate is held; a failed probe falls through to remount. */
    if (nuwax_sd_card_is_mounted() && nuwax_sd_card_probe() == ESP_OK) {
        if (!atomic_load_explicit(&s_directory_dirty, memory_order_acquire)) {
            /* No writer/replay mutation occurred since the last successful
             * scan. The probe already checked the medium, so avoid repeating
             * four full directory walks on every supervisor tick. */
            atomic_store_explicit(&s.media_recovery_requested, false,
                                  memory_order_release);
            reset_media_recovery_locked();
            atomic_store_explicit(&s_media_ready, true, memory_order_release);
            xSemaphoreGive(s.lock);
            xSemaphoreGive(s.writer_gate);
            xSemaphoreGive(s.admission_lock);
            return ESP_OK;
        }
        result = ensure_directory();
        if (result == ESP_OK) result = prepare_media_space();
        if (result == ESP_OK) {
            result = recover_open_segments();
        }
        if (result == ESP_OK) {
            s.next_index = next_segment_index();
            s.used_bytes = scan_used_bytes();
            atomic_store_explicit(&s.ready_count, scan_ready_count(),
                                  memory_order_release);
            atomic_store_explicit(&s_directory_dirty,
                                  atomic_load_explicit(&s.degraded,
                                                       memory_order_acquire),
                                  memory_order_release);
            atomic_store_explicit(&s.media_recovery_requested, false,
                                  memory_order_release);
            reset_media_recovery_locked();
            atomic_store_explicit(&s_media_ready, true, memory_order_release);
            xSemaphoreGive(s.lock);
            xSemaphoreGive(s.writer_gate);
            xSemaphoreGive(s.admission_lock);
            return ESP_OK;
        }
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        nuwax_sd_card_mark_io_fault(result);
    }
    if (s.media_recovery_deadline_ms != 0U &&
        !offline_deadline_elapsed(offline_now_ms(),
                                  s.media_recovery_deadline_ms)) {
        atomic_store_explicit(&s.media_recovery_requested, false,
                              memory_order_release);
        xSemaphoreGive(s.lock);
        xSemaphoreGive(s.writer_gate);
        xSemaphoreGive(s.admission_lock);
        return ESP_ERR_NOT_FINISHED;
    }

    /* Both spool locks and the recovery gate remain held while the storage
     * driver is replaced. No writer dequeue, append, or new archive can cross
     * this boundary. */
    const esp_err_t recover_err = nuwax_sd_card_recover(
        recovery_remaining_ms(unlimited, deadline_us));
    if (recover_err != ESP_OK) {
        schedule_media_recovery_locked(recover_err);
        atomic_store_explicit(&s.media_recovery_requested, false,
                              memory_order_release);
        xSemaphoreGive(s.lock);
        xSemaphoreGive(s.writer_gate);
        xSemaphoreGive(s.admission_lock);
        return recover_err;
    }

    /* A successful remount starts a fresh media health epoch. Any malformed
     * prefix found by the recovery scan below will set degraded again. */
    atomic_store_explicit(&s.degraded, false, memory_order_release);
    result = ensure_directory();
    if (result == ESP_OK) result = prepare_media_space();
    if (result == ESP_OK) {
        /* A card can be removed after a segment footer was written but before
         * its rename. Re-run the crash-safe recovery pass before admitting a
         * new archive so no valid prefix is stranded as .part. */
        result = recover_open_segments();
    }
    if (result != ESP_OK) {
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        nuwax_sd_card_mark_io_fault(result);
        schedule_media_recovery_locked(result);
        atomic_store_explicit(&s.media_recovery_requested, false,
                              memory_order_release);
        xSemaphoreGive(s.lock);
        xSemaphoreGive(s.writer_gate);
        xSemaphoreGive(s.admission_lock);
        return result;
    }

    s.next_index = next_segment_index();
    s.used_bytes = scan_used_bytes();
    atomic_store_explicit(&s.ready_count, scan_ready_count(),
                          memory_order_release);
    atomic_store_explicit(&s_directory_dirty,
                          atomic_load_explicit(&s.degraded,
                                               memory_order_acquire),
                          memory_order_release);
    atomic_store_explicit(&s.writer_error, false, memory_order_release);
    s.writer_retry_deadline_ms = 0U;
    s.writer_retry_backoff_ms = 0U;
    s.writer_fault_latched = false;
    atomic_store_explicit(&s.capture_complete, true, memory_order_release);
    reset_media_recovery_locked();
    atomic_store_explicit(&s_media_ready, true, memory_order_release);
    atomic_store_explicit(&s.media_recovery_requested, false,
                          memory_order_release);
    ESP_LOGI(TAG,
             "offline SD media recovery complete ready=%u used_bytes=%llu degraded=%d",
             (unsigned)atomic_load_explicit(&s.ready_count,
                                            memory_order_acquire),
             (unsigned long long)s.used_bytes,
             atomic_load_explicit(&s.degraded, memory_order_acquire) ? 1 : 0);
    xSemaphoreGive(s.lock);
    xSemaphoreGive(s.writer_gate);
    xSemaphoreGive(s.admission_lock);
    return ESP_OK;
}

static esp_err_t begin_archive(nuwax_offline_audio_mode_t mode,
                                    const char *session_id,
                                    uint32_t sample_rate,
                                    uint8_t channels,
                                    uint16_t frame_duration_ms, bool mirror, bool buffered)
{
    if (!enabled()) {
        return ESP_OK;
    }
    if (!runtime_initialized() || !valid_session_id(session_id) ||
        !valid_audio_geometry(sample_rate, channels, frame_duration_ms, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The archive foundation and its PSRAM queue are allocated once during
     * boot.  Do not apply the boot-time internal-heap watermark again here:
     * the actual microphone/I2S owner below is the component that knows which
     * internal/DMA allocations are required and reports their real failure.
     * Rechecking the conservative watermark at this point used to reject a
     * valid SD handoff when the online owner had just released the MIC. */
    if (xSemaphoreTake(s.lock, buffered ? 0 : portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (mode != NUWAX_OFFLINE_AUDIO_MODE_MEETING &&
        mode != NUWAX_OFFLINE_AUDIO_MODE_MONITORING) {
        xSemaphoreGive(s.lock);
        return ESP_ERR_INVALID_ARG;
    }
    const bool already_active =
        atomic_load_explicit(&s.active, memory_order_acquire) ||
        atomic_load_explicit(&s.closing, memory_order_acquire);
    if (already_active || atomic_load_explicit(&s_shutdown_requested, memory_order_acquire) ||
        nuwax_power_ctrl_shutdown_preparing() ||
        atomic_load_explicit(&s.media_recovery_requested,
                             memory_order_acquire)) {
        xSemaphoreGive(s.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!atomic_load_explicit(&s_media_ready, memory_order_acquire)) {
        xSemaphoreGive(s.lock);
        return ESP_ERR_NOT_FINISHED;
    }
    if (s.used_bytes + CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES > ARCHIVE_QUOTA_BYTES) {
        xSemaphoreGive(s.lock);
        return ESP_ERR_NO_MEM;
    }
    atomic_store_explicit(&s_close_complete, true, memory_order_release);
    xEventGroupClearBits(s.events, DONE_BIT | FAILED_BIT);
    s.capture_generation++;
    s.stop_enqueued = false;
    s.mode = mode;
    s.mirror = mirror;
    s.sample_rate = sample_rate;
    s.channels = channels;
    s.frame_duration_ms = frame_duration_ms;
    snprintf(s.session_id, sizeof(s.session_id), "%s", session_id);
    atomic_store_explicit(&s.closing, false, memory_order_release);
    atomic_store_explicit(&s.degraded, false, memory_order_release);
    atomic_store_explicit(&s.writer_error, false, memory_order_release);
    atomic_store_explicit(&s.dropped_frames, 0U, memory_order_release);
    s.writer_retry_deadline_ms = 0U;
    s.writer_retry_backoff_ms = 0U;
    s.writer_fault_latched = false;
    atomic_store_explicit(&s.active, true, memory_order_release);
    const esp_err_t err = buffered ? ESP_OK : open_segment_locked();
    if (err != ESP_OK) {
        atomic_store_explicit(&s.active, false, memory_order_release);
    }
    xSemaphoreGive(s.lock);
    return err;
}

esp_err_t nuwax_offline_audio_begin(nuwax_offline_audio_mode_t mode,
                                    const char *session_id, uint32_t sample_rate,
                                    uint8_t channels, uint16_t frame_duration_ms)
{
    return begin_archive(mode, session_id, sample_rate, channels, frame_duration_ms, false, false);
}

esp_err_t nuwax_offline_audio_begin_mirror(nuwax_offline_audio_mode_t mode,
                                           const char *session_id, uint32_t sample_rate,
                                           uint8_t channels, uint16_t frame_duration_ms)
{
    return begin_archive(mode, session_id, sample_rate, channels, frame_duration_ms, true, true);
}

static esp_err_t append_frame(const nuwax_offline_audio_frame_t *frame, TickType_t wait, bool count_loss)
{
    if (!enabled()) {
        return ESP_OK;
    }
    if (!frame || !frame->samples || !s.admission_lock ||
        xSemaphoreTake(s.admission_lock,
                       wait) != pdTRUE) {
        if (count_loss) {
            (void)atomic_fetch_add_explicit(&s.dropped_frames, 1U,
                                            memory_order_relaxed);
            atomic_store_explicit(&s.degraded, true, memory_order_release);
        }
        return ESP_ERR_NOT_FINISHED;
    }
    size_t expected_samples = 0U;
    if (!runtime_initialized() ||
        !atomic_load_explicit(&s.active, memory_order_acquire) ||
        atomic_load_explicit(&s.closing, memory_order_acquire) ||
        atomic_load_explicit(&s.writer_error, memory_order_acquire) ||
        !s.queue || !s.free_queue ||
        !valid_audio_geometry(s.sample_rate, s.channels, s.frame_duration_ms,
                              &expected_samples) ||
        frame->sample_count != expected_samples ||
        s.sample_rate > UINT16_MAX) {
        if (count_loss) {
            (void)atomic_fetch_add_explicit(&s.dropped_frames, 1U,
                                            memory_order_relaxed);
            atomic_store_explicit(&s.degraded, true, memory_order_release);
        }
        xSemaphoreGive(s.admission_lock);
        return ESP_ERR_INVALID_STATE;
    }
    spool_item_t *item = NULL;
    if (xQueueReceive(s.free_queue, &item,
                      wait) != pdTRUE ||
        !item) {
        if (count_loss) {
            (void)atomic_fetch_add_explicit(&s.dropped_frames, 1U,
                                            memory_order_relaxed);
            atomic_store_explicit(&s.degraded, true, memory_order_release);
        }
        xSemaphoreGive(s.admission_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    item->type = 1U;
    item->complete = 0U;
    item->generation = s.capture_generation;
    size_t frame_len = 0U;
    const esp_err_t err = nuwax_audio_frame_build_pcm16(
        item->data, sizeof(item->data), frame->samples, frame->sample_count,
        frame->sequence, frame->timestamp_ms, frame->flags,
        (uint16_t)s.sample_rate, s.channels, s.frame_duration_ms, &frame_len);
    if (err != ESP_OK || frame_len > sizeof(item->data)) {
        release_slot(item);
        xSemaphoreGive(s.admission_lock);
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    item->bytes = frame_len;
    if (xQueueSend(s.queue, &item,
                   wait) != pdTRUE) {
        release_slot(item);
        if (count_loss) {
            (void)atomic_fetch_add_explicit(&s.dropped_frames, 1U, memory_order_relaxed);
            atomic_store_explicit(&s.degraded, true, memory_order_release);
        }
        xSemaphoreGive(s.admission_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    notify_writer_task();
    xSemaphoreGive(s.admission_lock);
    return ESP_OK;
}

esp_err_t nuwax_offline_audio_append(const nuwax_offline_audio_frame_t *frame)
{
    return append_frame(frame, pdMS_TO_TICKS(QUEUE_ADMISSION_WAIT_MS), true);
}

esp_err_t nuwax_offline_audio_try_append(const nuwax_offline_audio_frame_t *frame)
{
    return append_frame(frame, 0, true);
}

esp_err_t nuwax_offline_audio_flush(uint32_t timeout_ms)
{
    if (!enabled() || !runtime_initialized() ||
        !atomic_load_explicit(&s.active, memory_order_acquire)) {
        return ESP_OK;
    }
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    while (uxQueueMessagesWaiting(s.queue) != 0U) {
        if (esp_timer_get_time() >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const int result = s.file && fflush(s.file) == 0 &&
                               fsync(fileno(s.file)) == 0
                           ? ESP_OK
                           : ESP_FAIL;
    xSemaphoreGive(s.lock);
    return result;
}

esp_err_t nuwax_offline_audio_request_end(bool complete)
{
    if (!enabled() || !runtime_initialized() ||
        !atomic_load_explicit(&s.active, memory_order_acquire)) {
        return ESP_OK;
    }
    /* One capture owner publishes this fence; only the writer clears it.
     * No FILE lock, queue capacity or slow fsync can delay privacy stop. */
    if (!complete) {
        atomic_store_explicit(&s_close_complete, false, memory_order_release);
    }
    atomic_store_explicit(&s.closing, true, memory_order_release);
    notify_writer_task();
    return ESP_OK;
}

esp_err_t nuwax_offline_audio_end(bool complete)
{
    if (!enabled() || !runtime_initialized() ||
        !atomic_load_explicit(&s.active, memory_order_acquire)) {
        return ESP_OK;
    }
    (void)nuwax_offline_audio_request_end(complete);
    const EventBits_t bits = xEventGroupWaitBits(s.events, DONE_BIT | FAILED_BIT,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(5000));
    return (bits & FAILED_BIT) ? ESP_FAIL
         : (bits & DONE_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static atomic_uint_fast32_t s_next_capture_sequence = ATOMIC_VAR_INIT(0U);
/* Only the connection owner requests/takes a MIC; the reader publishes it at
 * a complete PCM frame boundary. Cancellation wins unless READY was published. */
/* Odd tickets request transfer, the following even ticket publishes READY.
 * Unique tickets fence an old reader after a timed-out request is retried. */
static atomic_uint s_mic_transfer = ATOMIC_VAR_INIT(0U);
static unsigned s_mic_transfer_ticket = 1U; /* connection owner only */
static nuwax_mic_input_t s_transfer_mic;

esp_err_t nuwax_offline_audio_take_capture_input(nuwax_mic_input_t *mic, uint32_t timeout_ms)
{
    if (!mic || !nuwax_offline_audio_capture_running()) return ESP_ERR_INVALID_STATE;
    unsigned expected = 0U;
    const unsigned ticket = s_mic_transfer_ticket;
    s_mic_transfer_ticket = (ticket + 2U) & 0x7fffffffU;
    if (!atomic_compare_exchange_strong_explicit(&s_mic_transfer, &expected,
            ticket, memory_order_acq_rel, memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (nuwax_offline_audio_capture_running() && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(2U) ? pdMS_TO_TICKS(2U) : 1U);
    }
    const bool stopped = !nuwax_offline_audio_capture_running();
    const unsigned state = atomic_exchange_explicit(&s_mic_transfer, 0U, memory_order_acq_rel);
    if (state == ticket + 1U) {
        if (stopped) {
            *mic = s_transfer_mic;
            memset(&s_transfer_mic, 0, sizeof(s_transfer_mic));
            return ESP_OK;
        }
        (void)nuwax_mic_input_deinit(&s_transfer_mic);
    }
    return stopped ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
}


static bool publish_capture_input(const nuwax_mic_input_t *mic, unsigned ticket)
{
    s_transfer_mic = *mic;
    return atomic_compare_exchange_strong_explicit(&s_mic_transfer, &ticket,
        ticket + 1U, memory_order_release, memory_order_relaxed);
}

uint32_t nuwax_offline_audio_next_capture_sequence(void)
{
    return (uint32_t)atomic_load_explicit(&s_next_capture_sequence, memory_order_acquire);
}

typedef struct {
    nuwax_offline_audio_mode_t mode;
    uint64_t limit_used_us;
    uint32_t first_sequence;
    uint32_t sample_rate;
    uint8_t channels;
    uint16_t frame_duration_ms;
    char session_id[48];
} offline_capture_config_t;

static void offline_capture_task(void *arg)
{
    offline_capture_config_t cfg = {0};
    if (arg) {
        memcpy(&cfg, arg, sizeof(cfg));
        heap_caps_free(arg);
    }
    size_t frame_samples = 0U;
    (void)valid_audio_geometry(cfg.sample_rate, cfg.channels,
                               cfg.frame_duration_ms, &frame_samples);
    int16_t *samples = frame_samples == 0U
                           ? NULL
                           : heap_caps_malloc(frame_samples * sizeof(int16_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    nuwax_mic_input_t mic = {0};
    bool mic_ready = false;
    unsigned transfer_ticket = 0U;
    uint64_t offline_started_us = 0U;
    nuwax_offline_audio_mirror_t archive = {.offline = true};
    nuwax_offline_voice_t voice_archive = {0};
    monitoring_speech_detector_t detector = {0};
    const bool monitoring = cfg.mode == NUWAX_OFFLINE_AUDIO_MODE_MONITORING;
    if (monitoring) {
        monitoring_speech_detector_init(&detector);
        if (nuwax_offline_voice_init(&voice_archive, cfg.session_id,
                                     cfg.sample_rate, cfg.frame_duration_ms) != ESP_OK) {
            atomic_store_explicit(&s.capture_complete, false, memory_order_release);
            goto done;
        }
        voice_archive.mirror.offline = true;
    }
    const nuwax_mic_owner_t owner =
        cfg.mode == NUWAX_OFFLINE_AUDIO_MODE_MEETING
            ? NUWAX_MIC_OWNER_MEETING_CAPTURE
            : NUWAX_MIC_OWNER_MONITORING_CAPTURE;
    if (!samples || frame_samples == 0U) {
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        atomic_store_explicit(&s.capture_complete, false, memory_order_release);
        ESP_LOGE(TAG,
                 "offline capture sample buffer allocation failed frame_samples=%u bytes=%u",
                 (unsigned)frame_samples,
                 (unsigned)(frame_samples * sizeof(int16_t)));
        goto done;
    }
    esp_err_t mic_init_err = ESP_FAIL;
    const int64_t mic_handoff_deadline_us =
        esp_timer_get_time() + (int64_t)MIC_HANDOFF_TIMEOUT_MS * 1000LL;
    for (uint32_t attempt = 0U;
         !atomic_load_explicit(&s.capture_stop, memory_order_acquire);
         ++attempt) {
        mic_init_err = nuwax_mic_input_init_with_owner(&mic, owner);
        if (mic_init_err == ESP_OK) {
            break;
        }
        if (esp_timer_get_time() >= mic_handoff_deadline_us) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(MIC_HANDOFF_RETRY_MS));
    }
    if (mic_init_err != ESP_OK) {
        if (!atomic_load_explicit(&s.capture_stop, memory_order_acquire)) {
            atomic_store_explicit(&s.degraded, true, memory_order_release);
            atomic_store_explicit(&s.capture_complete, false,
                                  memory_order_release);
            ESP_LOGE(TAG,
                     "offline microphone handoff failed timeout_ms=%u err=%s",
                     (unsigned)MIC_HANDOFF_TIMEOUT_MS,
                     esp_err_to_name(mic_init_err));
        }
        goto done;
    }
    mic_ready = true;
    offline_started_us = (uint64_t)esp_timer_get_time();
    ESP_LOGI(TAG,
             "offline capture active mode=%u session=%s archive=deferred stack_min_free=%u stack_bytes=%u queue=%u free_slots=%u",
             (unsigned)cfg.mode,
             cfg.session_id,
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)OFFLINE_CAPTURE_TASK_STACK_BYTES,
             (unsigned)uxQueueMessagesWaiting(s.queue),
             (unsigned)uxQueueMessagesWaiting(s.free_queue));
    uint32_t sequence = cfg.first_sequence;
    uint32_t frame_timestamp_ms = 0U;
    uint32_t consecutive_empty_reads = 0U;
    uint32_t discontinuities = 0U;
    uint64_t discarded_partial_samples = 0U;
    size_t current_samples = 0U;
    while (!atomic_load_explicit(&s.capture_stop, memory_order_acquire)) {
        const unsigned ticket = atomic_load_explicit(&s_mic_transfer, memory_order_acquire);
        if (current_samples == 0U && (ticket & 1U) != 0U) {
            transfer_ticket = ticket;
            atomic_store_explicit(&s.capture_complete, false, memory_order_release);
            break;
        }
        const uint32_t offline_limit_hours =
            cfg.mode == NUWAX_OFFLINE_AUDIO_MODE_MEETING
                ? CONFIG_NUWAX_MEETING_OFFLINE_MAX_HOURS
                : CONFIG_NUWAX_MONITORING_OFFLINE_MAX_HOURS;
        if (nuwax_offline_limit_elapsed(nuwax_offline_budget_used(
                                            cfg.limit_used_us, offline_started_us,
                                            (uint64_t)esp_timer_get_time()), 0U,
                                        offline_limit_hours)) {
            ESP_LOGI(TAG, "offline %s duration limit reached hours=%u; finalizing",
                     cfg.mode == NUWAX_OFFLINE_AUDIO_MODE_MEETING ? "meeting" : "monitoring",
                     (unsigned)offline_limit_hours);
            atomic_store_explicit(&s_offline_limit_reached, true, memory_order_release);
            break;
        }
        const size_t samples_remaining = frame_samples - current_samples;
        size_t samples_read = 0U;
        const esp_err_t read_err = nuwax_mic_input_read_pcm16(
            &mic, &samples[current_samples], samples_remaining,
            CONFIG_NUWAX_MIC_READ_TIMEOUT_MS, &samples_read);
        /* A stop can arrive while I2S is waiting. Do not admit that last
         * read into a new voice segment after the user has stopped capture. */
        if (atomic_load_explicit(&s.capture_stop, memory_order_acquire)) {
            break;
        }
        if (samples_read > samples_remaining) {
            atomic_store_explicit(&s.degraded, true, memory_order_release);
            atomic_store_explicit(&s.capture_complete, false,
                                  memory_order_release);
            break;
        }
        if (read_err == ESP_ERR_INVALID_RESPONSE) {
            ++discontinuities;
            discarded_partial_samples += current_samples;
            current_samples = 0U;
            frame_timestamp_ms = 0U;
            if (monitoring) {
                monitoring_speech_detector_reset_decision(&detector);
                nuwax_offline_voice_reset(&voice_archive);
            }
            consecutive_empty_reads = 0U;
            atomic_store_explicit(&s.capture_complete, false,
                                  memory_order_release);
            if (discontinuities == 1U || (discontinuities % 20U) == 0U) {
                ESP_LOGW(TAG,
                         "offline microphone discontinuity reset partial frame events=%lu discarded_samples=%llu",
                         (unsigned long)discontinuities,
                         (unsigned long long)discarded_partial_samples);
            }
            continue;
        }
        if (read_err != ESP_OK && read_err != ESP_ERR_TIMEOUT) {
            atomic_store_explicit(&s.degraded, true, memory_order_release);
            atomic_store_explicit(&s.capture_complete, false, memory_order_release);
            break;
        }
        if (samples_read == 0U) {
            if (++consecutive_empty_reads >=
                CONFIG_NUWAX_MIC_MAX_CONSECUTIVE_TIMEOUTS) {
                atomic_store_explicit(&s.degraded, true, memory_order_release);
                atomic_store_explicit(&s.capture_complete, false,
                                      memory_order_release);
                ESP_LOGE(TAG,
                         "offline microphone stalled empty_reads=%lu partial_samples=%u",
                         (unsigned long)consecutive_empty_reads,
                         (unsigned)current_samples);
                break;
            }
            continue;
        }
        consecutive_empty_reads = 0U;
        if (current_samples == 0U) {
            frame_timestamp_ms =
                (uint32_t)(esp_timer_get_time() / 1000ULL);
        }
        current_samples += samples_read;
        if (current_samples < frame_samples) {
            continue;
        }
        nuwax_offline_audio_frame_t frame = {
            .sequence = sequence++,
            .timestamp_ms = frame_timestamp_ms,
            .flags = 0U,
            .samples = samples,
            .sample_count = frame_samples,
        };
        atomic_store_explicit(&s_next_capture_sequence, sequence, memory_order_release);
        if (monitoring) {
            const bool speech = monitoring_detect_speech(&detector, samples,
                                                         frame_samples, NULL, NULL);
            nuwax_offline_voice_process(&voice_archive, samples, frame_samples,
                                        speech, frame.sequence, frame.timestamp_ms);
        } else {
            (void)nuwax_offline_audio_mirror_service(
                &archive, cfg.mode, cfg.session_id, cfg.sample_rate, cfg.channels,
                cfg.frame_duration_ms, offline_now_ms());
            if (nuwax_offline_audio_mirror_append(&archive, &frame, offline_now_ms()) != ESP_OK) {
                atomic_store_explicit(&s.capture_complete, false, memory_order_release);
            }
        }
        current_samples = 0U;
        frame_timestamp_ms = 0U;
    }
    if (current_samples > 0U) {
        discarded_partial_samples += current_samples;
        atomic_store_explicit(&s.capture_complete, false, memory_order_release);
    }
    ESP_LOGI(TAG,
             "offline capture reader stopped frames=%lu discontinuities=%lu discarded_partial_samples=%llu dropped_frames=%lu complete=%d",
             (unsigned long)(sequence - cfg.first_sequence),
             (unsigned long)discontinuities,
             (unsigned long long)discarded_partial_samples,
             (unsigned long)nuwax_offline_audio_dropped_frames(),
             atomic_load_explicit(&s.capture_complete, memory_order_acquire)
                 ? 1
                 : 0);
done:
    if (mic_ready) {
        s_limit_used_us = nuwax_offline_budget_used(cfg.limit_used_us,
            offline_started_us, (uint64_t)esp_timer_get_time());
        bool transferred = false;
        if (transfer_ticket) transferred = publish_capture_input(&mic, transfer_ticket);
        if (!transferred) (void)nuwax_mic_input_deinit(&mic);
    }
    const bool complete = atomic_load_explicit(&s.capture_complete, memory_order_acquire);
    if (monitoring) {
        nuwax_offline_voice_deinit(&voice_archive, complete);
        monitoring_speech_detector_deinit(&detector);
    } else {
        (void)nuwax_offline_audio_mirror_request_close(&archive, complete, offline_now_ms());
    }
    if (samples) {
        heap_caps_free(samples);
    }
    /* Publish task termination in an order that cannot let a new capture
     * overwrite its handle and then have this task clear it.  A caller may
     * only pass the start gate after capture_running becomes false; by then
     * this task has already relinquished the handle and emitted DONE. */
    s.capture_task = NULL;
    ESP_LOGI(TAG,
             "offline capture task exiting stack_min_free=%u stack_bytes=%u queue=%u free_slots=%u degraded=%d",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)OFFLINE_CAPTURE_TASK_STACK_BYTES,
             s.queue ? (unsigned)uxQueueMessagesWaiting(s.queue) : 0U,
             s.free_queue ? (unsigned)uxQueueMessagesWaiting(s.free_queue) : 0U,
             atomic_load_explicit(&s.degraded, memory_order_acquire) ? 1 : 0);
    (void)xEventGroupSetBits(s.capture_events, DONE_BIT);
    atomic_store_explicit(&s.capture_running, false, memory_order_release);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t nuwax_offline_audio_start_capture(nuwax_offline_audio_mode_t mode,
                                            const char *session_id,
                                            uint32_t sample_rate,
                                            uint8_t channels,
                                            uint16_t frame_duration_ms)
{
    return nuwax_offline_audio_start_capture_continuation(mode, session_id, sample_rate,
                                                          channels, frame_duration_ms, 0U);
}

esp_err_t nuwax_offline_audio_start_capture_continuation(nuwax_offline_audio_mode_t mode,
    const char *session_id, uint32_t sample_rate, uint8_t channels,
    uint16_t frame_duration_ms, uint32_t first_sequence)
{
    if (!enabled()) {
        return ESP_OK;
    }
    if (!runtime_initialized() || !valid_session_id(session_id) ||
        (mode != NUWAX_OFFLINE_AUDIO_MODE_MEETING &&
         mode != NUWAX_OFFLINE_AUDIO_MODE_MONITORING) ||
        !valid_audio_geometry(sample_rate, channels, frame_duration_ms, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s.admission_lock || !s.lock) {
        ESP_LOGW(TAG, "offline capture start deferred reason=runtime_not_ready");
        return ESP_ERR_NOT_FINISHED;
    }
    if (xSemaphoreTake(s.admission_lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "offline capture start deferred reason=admission_busy");
        return ESP_ERR_NOT_FINISHED;
    }
    if (atomic_load_explicit(&s.media_recovery_requested,
                             memory_order_acquire)) {
        ESP_LOGW(TAG, "offline capture start deferred reason=media_recovery");
        xSemaphoreGive(s.admission_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    bool expected_running = false;
    if (!atomic_compare_exchange_strong_explicit(
            &s.capture_running, &expected_running, true,
            memory_order_acq_rel, memory_order_acquire)) {
        ESP_LOGW(TAG, "offline capture start deferred reason=capture_already_running");
        xSemaphoreGive(s.admission_lock);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s.admission_lock);
    if (strcmp(s_limit_session, session_id) == 0 &&
        nuwax_offline_audio_limit_reached()) {
        atomic_store_explicit(&s.capture_running, false, memory_order_release);
        return ESP_ERR_INVALID_STATE;
    }
    offline_capture_config_t *cfg = heap_caps_calloc(
        1U, sizeof(*cfg), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cfg) {
        /* Keep the capture task launch independent from the small internal
         * heap.  This fallback is only for PSRAM-unavailable boards and is
         * bounded by the normal task-creation result below. */
        cfg = heap_caps_calloc(1U, sizeof(*cfg),
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!cfg) {
        atomic_store_explicit(&s.capture_running, false, memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    cfg->mode = mode;
    if (strcmp(s_limit_session, session_id) != 0) {
        snprintf(s_limit_session, sizeof(s_limit_session), "%s", session_id);
        s_limit_used_us = 0U;
        atomic_store_explicit(&s_offline_limit_reached, false, memory_order_release);
    }
    cfg->limit_used_us = s_limit_used_us;
    cfg->first_sequence = first_sequence;
    atomic_store_explicit(&s_next_capture_sequence, first_sequence, memory_order_release);
    cfg->sample_rate = sample_rate;
    cfg->channels = channels;
    cfg->frame_duration_ms = frame_duration_ms;
    snprintf(cfg->session_id, sizeof(cfg->session_id), "%s", session_id);
    atomic_store_explicit(&s.capture_stop, false, memory_order_release);
    atomic_store_explicit(&s.capture_complete, true, memory_order_release);
    xEventGroupClearBits(s.capture_events, DONE_BIT);
    BaseType_t task_result = xTaskCreatePinnedToCoreWithCaps(
        offline_capture_task,
        "offline_mic_capture",
        OFFLINE_CAPTURE_TASK_STACK_BYTES,
        cfg,
        OFFLINE_CAPTURE_TASK_PRIORITY,
        &s.capture_task,
        OFFLINE_CAPTURE_TASK_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_result != pdPASS) {
        /* PSRAM is the normal placement.  A bounded internal-stack fallback
         * keeps boards without usable PSRAM functional, while the normal
         * path never consumes the small internal heap for this task. */
        task_result = xTaskCreatePinnedToCoreWithCaps(
            offline_capture_task,
            "offline_mic_capture",
            OFFLINE_CAPTURE_TASK_STACK_BYTES,
            cfg,
            OFFLINE_CAPTURE_TASK_PRIORITY,
            &s.capture_task,
            OFFLINE_CAPTURE_TASK_CORE,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (task_result != pdPASS) {
        atomic_store_explicit(&s.capture_running, false, memory_order_release);
        heap_caps_free(cfg);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "offline capture task scheduled mode=%u session=%s",
             (unsigned)mode, session_id);
    return ESP_OK;
}

esp_err_t nuwax_offline_audio_stop_capture(bool complete)
{
    if (!enabled() || !runtime_initialized()) {
        return ESP_OK;
    }
    if (atomic_load_explicit(&s.capture_running, memory_order_acquire)) {
        (void)nuwax_offline_audio_request_stop(complete);
        const EventBits_t bits = xEventGroupWaitBits(
            s.capture_events, DONE_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(6000));
        if ((bits & DONE_BIT) == 0U) {
            return ESP_ERR_TIMEOUT;
        }
    }
    /* The capture task can finish after its end() waiter timed out.  Continue
     * the close transaction here instead of reporting success while the SD
     * FILE and generation fence are still active. */
    if (nuwax_offline_audio_is_active()) {
        return nuwax_offline_audio_end(complete);
    }
    return nuwax_offline_audio_is_degraded() ? ESP_FAIL : ESP_OK;
}

esp_err_t nuwax_offline_audio_request_stop(bool complete)
{
    if (!enabled() || !runtime_initialized()) {
        return ESP_OK;
    }
    /* Completeness is monotonic within one capture generation. A later clean
     * stop must never erase a queue drop, discontinuity, or reader failure
     * already observed by the capture task. start_capture() resets it for the
     * next generation. */
    if (!complete) {
        atomic_store_explicit(&s.capture_complete, false,
                              memory_order_release);
    }
    atomic_store_explicit(&s.capture_stop, true, memory_order_release);
    /* The reader's mirror owns its FILE/tail generation. Once the MIC has
     * stopped, a late mode cancellation must not close a draining tail or a
     * subsequently attached online archive. Each mirror closes its own sink. */
    return ESP_OK;
}

bool nuwax_offline_audio_capture_running(void)
{
    return enabled() &&
           atomic_load_explicit(&s.capture_running, memory_order_acquire);
}

bool nuwax_offline_audio_has_ready(void)
{
    if (!enabled() || !runtime_initialized()) {
        return false;
    }
    return atomic_load_explicit(&s.ready_count, memory_order_acquire) != 0U ||
           atomic_load_explicit(&s_quarantine_recheck_pending, memory_order_acquire);
}

bool nuwax_offline_audio_is_active(void)
{
    if (!enabled() || !s.lock || xSemaphoreTake(s.lock, 0) != pdTRUE) {
        return enabled() && atomic_load_explicit(&s.active, memory_order_acquire);
    }
    const bool active = atomic_load_explicit(&s.active, memory_order_acquire);
    xSemaphoreGive(s.lock);
    return active;
}

bool nuwax_offline_audio_is_degraded(void)
{
    if (!enabled() || !s.lock || xSemaphoreTake(s.lock, 0) != pdTRUE) {
        return enabled() && atomic_load_explicit(&s.degraded, memory_order_acquire);
    }
    const bool degraded = atomic_load_explicit(&s.degraded, memory_order_acquire);
    xSemaphoreGive(s.lock);
    return degraded;
}

uint32_t nuwax_offline_audio_dropped_frames(void)
{
    if (!enabled() || !s.lock || xSemaphoreTake(s.lock, 0) != pdTRUE) {
        return (uint32_t)atomic_load_explicit(&s.dropped_frames, memory_order_relaxed);
    }
    const uint32_t dropped =
        (uint32_t)atomic_load_explicit(&s.dropped_frames, memory_order_relaxed);
    xSemaphoreGive(s.lock);
    return dropped;
}

static esp_err_t replay_send_frame_buffered(nuwax_ws_client_t *ws,
                                    uint32_t epoch,
                                    const uint8_t *payload,
                                    size_t len,
                                    bool chunk, bool independent_transport,
                                    uint8_t *scratch, size_t scratch_capacity,
                                    nuwax_ws_send_metrics_t *metrics)
{
    if (!ws || !payload || len == 0U || epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_ERR_TIMEOUT;
    for (uint32_t attempt = 1U; attempt <= REPLAY_SEND_RETRY_ATTEMPTS;
         ++attempt) {
        err = chunk
            ? (independent_transport
                ? nuwax_ws_send_archive_buffered_if_epoch(
                      ws, payload, len, REPLAY_ISOLATED_IO_TIMEOUT_MS, epoch,
                      scratch, scratch_capacity, metrics)
                : nuwax_ws_send_archive_chunk_if_epoch(ws, payload, len, epoch))
            : nuwax_ws_send_archive_text_if_epoch(
                  ws, (const char *)payload, epoch);
        if (err != ESP_ERR_TIMEOUT || !nuwax_ws_is_connected(ws) ||
            nuwax_ws_connection_epoch(ws) != epoch) {
            return err;
        }
        ESP_LOGD(TAG,
                 "offline replay frame send deferred kind=%s attempt=%lu/%u epoch=%lu",
                 chunk ? "chunk" : "text",
                 (unsigned long)attempt,
                 (unsigned)REPLAY_SEND_RETRY_ATTEMPTS,
                 (unsigned long)epoch);
        const TickType_t ticks = pdMS_TO_TICKS(REPLAY_SEND_RETRY_DELAY_MS);
        vTaskDelay(ticks ? ticks : 1U);
    }
    ESP_LOGW(TAG,
             "offline replay frame send exhausted kind=%s epoch=%lu err=%s",
             chunk ? "chunk" : "text",
             (unsigned long)epoch,
             esp_err_to_name(err));
    return err;
}

static esp_err_t replay_send_parts(nuwax_ws_client_t *ws, uint32_t epoch,
    const uint8_t *header, const uint8_t *data, size_t bytes,
    uint8_t *wire, size_t wire_capacity, nuwax_ws_send_metrics_t *metrics)
{
    esp_err_t err = ESP_ERR_TIMEOUT;
    for (unsigned attempt = 0; attempt < REPLAY_SEND_RETRY_ATTEMPTS; ++attempt) {
        if (nuwax_offline_audio_storage_busy()) return ESP_ERR_NOT_FINISHED;
        err = nuwax_ws_send_archive_parts_if_epoch(ws, header, data, bytes,
            REPLAY_ISOLATED_IO_TIMEOUT_MS, epoch, wire, wire_capacity, metrics);
        if (err != ESP_ERR_TIMEOUT || !nuwax_ws_is_connected(ws) ||
            nuwax_ws_connection_epoch(ws) != epoch) return err;
        const TickType_t ticks = pdMS_TO_TICKS(REPLAY_SEND_RETRY_DELAY_MS);
        vTaskDelay(ticks ? ticks : 1U);
    }
    return err;
}

static esp_err_t replay_send_batch(nuwax_ws_client_t *ws, uint32_t epoch,
    const nuwax_ws_archive_frame_t *frames, size_t count,
    uint8_t *wire, size_t wire_capacity, nuwax_ws_send_metrics_t *metrics)
{
    if (count == 1U) return replay_send_parts(ws, epoch, frames[0].header,
        frames[0].data, frames[0].bytes, wire, wire_capacity, metrics);
    esp_err_t err = ESP_ERR_TIMEOUT;
    for (unsigned attempt = 0; attempt < REPLAY_SEND_RETRY_ATTEMPTS; ++attempt) {
        if (nuwax_offline_audio_storage_busy()) return ESP_ERR_NOT_FINISHED;
        err = nuwax_ws_send_archive_batch_if_epoch(ws, frames, count,
            REPLAY_ISOLATED_IO_TIMEOUT_MS, epoch, wire, wire_capacity, metrics);
        /* A partially written batch always closes its transport. Only an
         * admission timeout with no wire progress can retry on this epoch. */
        if (err != ESP_ERR_TIMEOUT || !nuwax_ws_is_connected(ws) ||
            nuwax_ws_connection_epoch(ws) != epoch) return err;
        const TickType_t ticks = pdMS_TO_TICKS(REPLAY_SEND_RETRY_DELAY_MS);
        vTaskDelay(ticks ? ticks : 1U);
    }
    return err;
}

static esp_err_t replay_send_frame(nuwax_ws_client_t *ws, uint32_t epoch,
    const uint8_t *payload, size_t len, bool chunk, bool independent_transport)
{
    return replay_send_frame_buffered(ws, epoch, payload, len, chunk,
                                      independent_transport, NULL, 0U, NULL);
}

bool nuwax_offline_audio_storage_busy(void)
{
    /* Queue handles are immutable after runtime publication. No storage or
     * lifecycle lock is taken here, including from the CRC validation loop.
     * A recording starting after this snapshot can overlap at most one
     * bounded read; the 128-slot writer queue absorbs that in-flight read. */
    return enabled() && runtime_initialized() &&
        (atomic_load_explicit(&s.media_recovery_requested, memory_order_acquire) ||
         atomic_load_explicit(&s_mirror_closers, memory_order_acquire) != 0U ||
         atomic_load_explicit(&s.active, memory_order_acquire) ||
         atomic_load_explicit(&s.closing, memory_order_acquire) ||
         atomic_load_explicit(&s.capture_running, memory_order_acquire) ||
         atomic_load_explicit(&s.writer_busy, memory_order_acquire) ||
         (s.queue && uxQueueMessagesWaiting(s.queue) != 0U));
}

esp_err_t nuwax_offline_audio_prepare_shutdown(void)
{
    if (!enabled() || !runtime_initialized()) return ESP_OK;
    /* Serialize with replay admission/context publication and archive begin.
     * Never wait for a network send while holding this storage lock. */
    if (xSemaphoreTake(s.lock, 0) != pdTRUE) return ESP_ERR_NOT_FINISHED;
    atomic_store_explicit(&s_shutdown_requested, true, memory_order_release);
    atomic_store_explicit(&s_formal_replay_cancel_requested, true, memory_order_release);
    xSemaphoreGive(s.lock);
    if (s.events) xEventGroupSetBits(s.events, REPLAY_ACK_BIT | REPLAY_CHUNK_ACK_BIT);
    /* Include dequeue-to-writer_busy handoff. A queue-only snapshot can be
     * empty while the writer has just taken its last item. */
    if (xSemaphoreTake(s.admission_lock, 0) != pdTRUE) return ESP_ERR_NOT_FINISHED;
    if (xSemaphoreTake(s.writer_gate, 0) != pdTRUE) {
        xSemaphoreGive(s.admission_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    if (xSemaphoreTake(s.lock, 0) != pdTRUE) {
        xSemaphoreGive(s.writer_gate);
        xSemaphoreGive(s.admission_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    const bool busy = s.file != NULL || nuwax_offline_audio_storage_busy() ||
                      nuwax_offline_audio_replay_running();
    xSemaphoreGive(s.lock);
    xSemaphoreGive(s.writer_gate);
    xSemaphoreGive(s.admission_lock);
    return busy ? ESP_ERR_NOT_FINISHED : ESP_OK;
}

static esp_err_t replay_send_json(nuwax_ws_client_t *ws,
                                  uint32_t epoch,
                                  cJSON *root,
                                  bool independent_transport)
{
    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = (independent_transport ||
                           nuwax_audio_uplink_background_ready())
        ? replay_send_frame(ws, epoch, (const uint8_t *)json, strlen(json), false,
                            independent_transport)
        : ESP_ERR_NOT_FINISHED;
    free(json);
    return err;
}

static esp_err_t replay_arm_ack(uint32_t epoch,
                                const char *request_id,
                                const char *expected_type,
                                const char *segment_id)
{
    if (!request_id || !request_id[0] || !expected_type || !expected_type[0] ||
        !segment_id || !segment_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool is_chunk =
        strcmp(expected_type, "offline_audio_chunk_ack") == 0;
    const EventBits_t clear_bit =
        is_chunk ? REPLAY_CHUNK_ACK_BIT : REPLAY_ACK_BIT;
    /* Clear the old signal before publishing waiting=true.  Otherwise a
     * valid ACK arriving in the tiny arm/clear window could be erased. */
    xEventGroupClearBits(s.events, clear_bit);
    portENTER_CRITICAL(&s_replay_lock);
    memset(&s.replay_ack, 0, sizeof(s.replay_ack));
    s.replay_ack.waiting = true;
    s.replay_ack.chunk = is_chunk;
    s.replay_ack.epoch = epoch;
    snprintf(s.replay_ack.request_id, sizeof(s.replay_ack.request_id), "%s",
             request_id);
    snprintf(s.replay_ack.segment_id, sizeof(s.replay_ack.segment_id), "%s",
             segment_id);
    snprintf(s.replay_ack.expected_type,
             sizeof(s.replay_ack.expected_type), "%s", expected_type);
    portEXIT_CRITICAL(&s_replay_lock);
    return ESP_OK;
}

static void replay_disarm_ack(void)
{
    portENTER_CRITICAL(&s_replay_lock);
    s.replay_ack.waiting = false;
    portEXIT_CRITICAL(&s_replay_lock);
    xEventGroupClearBits(s.events, REPLAY_ACK_BIT | REPLAY_CHUNK_ACK_BIT);
}

static esp_err_t replay_wait_ack(nuwax_ws_client_t *ws,
                                 uint32_t epoch,
                                 const char *request_id,
                                 bool chunk,
                                 uint32_t *next_offset)
{
    (void)ws;
    if (!request_id || !request_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
    if (atomic_load_explicit(&s_formal_replay_cancel_requested,
                             memory_order_acquire)) {
        replay_disarm_ack();
        return ESP_ERR_INVALID_STATE;
    }
#endif
    const EventBits_t wait_bit = chunk ? REPLAY_CHUNK_ACK_BIT : REPLAY_ACK_BIT;
    EventBits_t bits = 0U;
    const uint32_t ack_deadline = offline_now_ms() + 2500U;
    do {
        if (nuwax_offline_audio_storage_busy()) {
            replay_disarm_ack();
            return ESP_ERR_NOT_FINISHED;
        }
        if (!nuwax_ws_is_connected(ws) || nuwax_ws_connection_epoch(ws) != epoch) {
            replay_disarm_ack();
            return ESP_ERR_INVALID_STATE;
        }
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
        if (atomic_load_explicit(&s_formal_replay_cancel_requested, memory_order_acquire)) {
            replay_disarm_ack();
            return ESP_ERR_INVALID_STATE;
        }
#endif
        bits = xEventGroupWaitBits(s.events, wait_bit, pdTRUE, pdFALSE,
                                   pdMS_TO_TICKS(100U));
    } while ((bits & wait_bit) == 0U &&
             !offline_deadline_elapsed(offline_now_ms(), ack_deadline));
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
    if (atomic_load_explicit(&s_formal_replay_cancel_requested,
                             memory_order_acquire)) {
        replay_disarm_ack();
        return ESP_ERR_INVALID_STATE;
    }
#endif
    if ((bits & wait_bit) == 0U) {
        portENTER_CRITICAL(&s_replay_lock);
        s.replay_ack.waiting = false;
        portEXIT_CRITICAL(&s_replay_lock);
        ESP_LOGW(TAG,
                 "offline replay ACK timeout kind=%s request=%s epoch=%lu",
                 chunk ? "chunk" : "control",
                 request_id,
                 (unsigned long)epoch);
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&s_replay_lock);
    const bool ok = s.replay_ack.ok && s.replay_ack.epoch == epoch &&
                    s.replay_ack.chunk == chunk &&
                    strcmp(s.replay_ack.request_id, request_id) == 0;
    if (next_offset) {
        *next_offset = s.replay_ack.next_offset;
    }
    s.replay_ack.waiting = false;
    portEXIT_CRITICAL(&s_replay_lock);
    if (!ok) {
        ESP_LOGW(TAG,
                 "offline replay ACK mismatch kind=%s request=%s epoch=%lu",
                 chunk ? "chunk" : "control",
                 request_id,
                 (unsigned long)epoch);
    }
    return ok ? ESP_OK : ESP_FAIL;
}

/* Reuse the exact request and bytes after a lost ACK. The peer's durable
 * offset/idempotency check makes this safe; a partial transport failure has
 * already cleared connected and must instead resume via a new begin. */
static esp_err_t replay_exchange_chunk(nuwax_ws_client_t *ws, uint32_t epoch,
                                        const uint8_t *chunk, size_t bytes,
                                        const char *request_id,
                                        const char *segment_id,
                                        bool independent_transport,
                                        uint32_t *acknowledged)
{
    esp_err_t err = ESP_ERR_TIMEOUT;
    for (uint32_t attempt = 1U; attempt <= REPLAY_CHUNK_ACK_ATTEMPTS; ++attempt) {
        if (nuwax_offline_audio_storage_busy()) return ESP_ERR_NOT_FINISHED;
        err = replay_arm_ack(epoch, request_id, "offline_audio_chunk_ack", segment_id);
        if (err != ESP_OK) return err;
        err = replay_send_frame(ws, epoch, chunk, bytes, true, independent_transport);
        if (err == ESP_OK) {
            err = replay_wait_ack(ws, epoch, request_id, true, acknowledged);
        } else {
            replay_disarm_ack();
        }
        if (err != ESP_ERR_TIMEOUT || !nuwax_ws_is_connected(ws) ||
            nuwax_ws_connection_epoch(ws) != epoch) return err;
        ESP_LOGW(TAG, "offline replay chunk ACK/send retry segment=%s attempt=%lu/%u epoch=%lu",
                 segment_id, (unsigned long)attempt,
                 (unsigned)REPLAY_CHUNK_ACK_ATTEMPTS, (unsigned long)epoch);
    }
    return err;
}

static esp_err_t validate_archive_records(FILE *file,
                                          const archive_header_t *header,
                                          const archive_footer_t *footer,
                                          long footer_offset)
{
    if (!file || !header || !footer ||
        footer_offset < (long)sizeof(archive_header_t) ||
        fseek(file, (long)sizeof(archive_header_t), SEEK_SET) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    _Alignas(int16_t) uint8_t payload[MAX_FRAME_BYTES];
    uint32_t frame_count = 0U;
    uint64_t payload_bytes = 0U;
    uint32_t crc = 0U;
    uint32_t last_sequence = 0U;
    bool sequence_seen = false;
    while (ftell(file) < footer_offset) {
        if (nuwax_offline_audio_storage_busy()) {
            return ESP_ERR_NOT_FINISHED;
        }
        const long record_start = ftell(file);
        if (record_start < (long)sizeof(archive_header_t) ||
            footer_offset - record_start < (long)sizeof(uint32_t)) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint32_t length = 0U;
        if (fread(&length, 1U, sizeof(length), file) != sizeof(length) ||
            length == 0U || length > MAX_FRAME_BYTES ||
            footer_offset - ftell(file) < (long)length) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (fread(payload, 1U, length, file) != length) {
            return ESP_FAIL;
        }
        if (validate_archive_frame(payload, length, header, &last_sequence,
                                   &sequence_seen) != ESP_OK) {
            return ESP_ERR_INVALID_SIZE;
        }
        crc = crc32_update(crc, payload, length);
        payload_bytes += length;
        if (frame_count == UINT32_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        ++frame_count;
    }
    if (ftell(file) != footer_offset || frame_count != footer->frame_count ||
        payload_bytes != footer->payload_bytes || crc != footer->crc32) {
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

#include "offline_replay_stream.inc"
#include "offline_replay_identity.h"
#include "board/sd_dma_bulk.h"
#include "board/sd_event_profile.h"

/* Independent, aligned FILE validation is required before either restoring
 * an old quarantine or classifying a streaming/protocol failure as damage. */
static esp_err_t recheck_archive_path(const char *path)
{
    if (nuwax_offline_audio_storage_busy()) return ESP_ERR_NOT_FINISHED;
    char *buffer = heap_caps_malloc(ARCHIVE_STDIO_BUFFER_BYTES,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) return ESP_ERR_NO_MEM;
    FILE *file = fopen(path, "rb");
    if (!file) { heap_caps_free(buffer); return ESP_FAIL; }
    esp_err_t err = ESP_ERR_INVALID_SIZE;
    archive_header_t header;
    archive_footer_t footer;
    struct stat st;
    if (setvbuf(file, buffer, _IOFBF, ARCHIVE_STDIO_BUFFER_BYTES) != 0 ||
        fstat(fileno(file), &st) != 0) {
        err = ESP_FAIL;
    } else if (st.st_size >= (off_t)(HEADER_BYTES + FOOTER_BYTES) &&
               st.st_size <= (off_t)INT32_MAX &&
               fread(&header, 1U, sizeof(header), file) == sizeof(header) &&
               memcmp(header.magic, "NWXOFF1", 7U) == 0 && header.version == 1U &&
               header.header_bytes == HEADER_BYTES &&
               header.mode <= NUWAX_OFFLINE_AUDIO_MODE_MONITORING &&
               header.codec == NUWAX_AUDIO_CODEC_PCM16 && header.channels == 1U &&
               header.sample_rate && header.sample_rate <= UINT16_MAX && header.frame_duration_ms) {
        if (fseek(file, -(long)FOOTER_BYTES, SEEK_END) != 0) {
            err = ESP_FAIL;
        } else if (fread(&footer, 1U, sizeof(footer), file) == sizeof(footer) &&
                   memcmp(footer.magic, "NWXEND1", 7U) == 0 && footer.version == 1U &&
                   footer.footer_bytes == FOOTER_BYTES) {
            err = validate_archive_records(file, &header, &footer,
                                           (long)st.st_size - FOOTER_BYTES);
        }
    }
    if (ferror(file)) err = ESP_FAIL;
    if (fclose(file) != 0) err = ESP_FAIL;
    heap_caps_free(buffer);
    return err;
}

static esp_err_t restore_quarantined_archive(char *path)
{
    const size_t length = strlen(path);
    if (length < 10U || strcmp(path + length - 10U, ".ready.bad") != 0)
        return ESP_ERR_INVALID_ARG;
    const esp_err_t err = recheck_archive_path(path);
    if (err != ESP_OK) return err;
    if (nuwax_offline_audio_storage_busy()) return ESP_ERR_NOT_FINISHED;
    char ready[160];
    if (length - 4U >= sizeof(ready)) return ESP_ERR_INVALID_ARG;
    memcpy(ready, path, length - 4U); ready[length - 4U] = '\0';
    struct stat existing;
    if (stat(ready, &existing) == 0 || errno != ENOENT) return ESP_ERR_INVALID_STATE;
    if (rename(path, ready) != 0) return ESP_FAIL;
    strcpy(path, ready);
    atomic_fetch_add_explicit(&s.ready_count, 1U, memory_order_release);
    atomic_store_explicit(&s_directory_dirty, true, memory_order_release);
    ESP_LOGI(TAG, "offline quarantine revalidated and restored path=%s", path);
    return ESP_OK;
}

#include "wake_word_detector.h"
#include "offline_replay_pressure.inc"
#include "offline_replay_read.inc"
#include "offline_replay_prefetch.inc"

static esp_err_t replay_send_windowed(nuwax_ws_client_t *ws, uint32_t epoch,
                                      FILE *file, const char *segment_id,
                                      uint32_t total, uint32_t resume,
                                      uint32_t window_limit,
                                      const archive_header_t *header,
                                      const archive_footer_t *footer)
{
    /* Bound PSRAM use and keep memory available for a new recording. */
    if (!file || !ws || !header || !footer || resume > total ||
        window_limit < REPLAY_CHUNK_MAX_PAYLOAD_BYTES) return ESP_ERR_INVALID_ARG;
    size_t capacity = window_limit < 128U * 1024U ? window_limit : 128U * 1024U;
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t tx_batch_frames = 2U * capacity + 65536U + 1024U * 1024U <= free_psram
        ? NUWAX_WS_ARCHIVE_BATCH_MAX_FRAMES : 1U;
    const size_t overhead = tx_batch_frames > 1U ? 65536U : 32768U;
    while (capacity > REPLAY_CHUNK_MAX_PAYLOAD_BYTES &&
           2U * capacity + overhead + 1024U * 1024U > free_psram) capacity /= 2U;
    if (2U * capacity + overhead + 1024U * 1024U > free_psram) return ESP_ERR_NO_MEM;
    uint8_t *buffer = NULL;
    while (capacity >= REPLAY_CHUNK_MAX_PAYLOAD_BYTES && !buffer) {
        buffer = heap_caps_malloc(2U * capacity + tx_batch_frames * (REPLAY_CHUNK_MAX_PAYLOAD_BYTES + 142U),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buffer) {
            /* Preserve the ACK window before sacrificing it to fragmentation. */
            if (tx_batch_frames > 1U) tx_batch_frames = 1U;
            else capacity /= 2U;
        }
    }
    if (!buffer) return ESP_ERR_NO_MEM;
    uint8_t *packet = buffer + 2U * capacity;
    uint8_t *wire = packet + tx_batch_frames * 64U;
    const size_t wire_capacity = tx_batch_frames * (REPLAY_CHUNK_MAX_PAYLOAD_BYTES + 78U);
    replay_stream_t validator = {0};
    esp_err_t err = replay_read_rewind(file);
    uint32_t position = 0U;
    size_t window = capacity < REPLAY_WINDOW_INITIAL_BYTES ? capacity : REPLAY_WINDOW_INITIAL_BYTES;
    uint64_t read_us = 0U, validate_us = 0U, send_us = 0U, ack_us = 0U, pressure_us = 0U;
    uint64_t data_wait_us = 0U;
    int64_t first_payload_us = -1;
    uint32_t batches = 0U;
    uint32_t tx_groups[NUWAX_WS_ARCHIVE_BATCH_MAX_FRAMES] = {0};
    uint32_t ack_yields = 0U, ack_yields_saved = 0U;
    const int64_t started = esp_timer_get_time();
    const bool lane_probe =
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_LANE_PROBE
        err == ESP_OK && resume == 0U &&
        capacity == 128U * 1024U && total >= capacity &&
        !atomic_exchange_explicit(&s_replay_lane_probe_taken, true, memory_order_relaxed);
#else
        false;
#endif
    replay_prefetch_t *reader = err == ESP_OK ? replay_prefetch_start(
        file, ws, epoch, total, resume, capacity, buffer, header, footer, lane_probe) : NULL;
    if (reader) position = resume;
    unsigned read_slot = 0U;
    nuwax_ws_send_metrics_t metrics = {0};
    offline_replay_budget_t budget = {0};
    replay_read_stats_t read_stats = {0};
    if (!reader && err == ESP_OK) replay_read_prepare(&read_stats);
    ESP_LOGI(TAG, "offline replay reader=%s buffers=2 capacity=%u publish_bytes=8192 handoff=event tx_layout=split reader_priority=%u sender_priority=%u lane_probe=%u tx_batch_frames=%u tx_batch_policy=ready_prefix", reader ? "streaming_prefetch" : "serial", (unsigned)capacity,
             (unsigned)(reader ? OFFLINE_REPLAY_READER_PRIORITY : uxTaskPriorityGet(NULL)),
             (unsigned)uxTaskPriorityGet(NULL), lane_probe ? 1U : 0U, (unsigned)tx_batch_frames);
    while (err == ESP_OK && position < total) {
        if (nuwax_offline_audio_storage_busy()) { err = ESP_ERR_NOT_FINISHED; break; }
        if (!nuwax_ws_is_connected(ws) || nuwax_ws_connection_epoch(ws) != epoch) {
            err = ESP_ERR_INVALID_STATE; break;
        }
        size_t count = total - position;
        if (count > window) count = window;
        /* Read the confirmed prefix once for full end-to-end CRC on resume. */
        if (position < resume && count > resume - position) count = resume - position;
        uint8_t *data = buffer;
        int64_t phase;
        if (reader) {
            replay_read_slot_t *slot = &reader->slots[read_slot];
            int64_t ready_wait_started = 0;
            while (!atomic_load_explicit(&slot->ready, memory_order_acquire)) {
                err = atomic_load_explicit(&reader->error, memory_order_acquire);
                if (err == ESP_OK) err = replay_prefetch_check(reader);
                if (err != ESP_OK) break;
                if (!ready_wait_started) ready_wait_started = esp_timer_get_time();
                if (replay_prefetch_wait_signal(reader->data_signal)) offline_replay_budget_relax(&budget);
            }
            if (ready_wait_started) data_wait_us += (uint64_t)(esp_timer_get_time() - ready_wait_started);
            if (err != ESP_OK) break;
            if (slot->position != position) { err = ESP_ERR_INVALID_STATE; break; }
            count = slot->count;
            data = slot->data;
            /* One real 128 KiB window per test run: validate it completely,
             * then pause the reader until its durable ACK. No dummy data. */
            if (reader->lane_probe && position == resume) {
                err = replay_prefetch_wait_probe(reader);
                if (err != ESP_OK) break;
                offline_replay_budget_relax(&budget);
            }
        } else {
            /* Bound each FILE operation so a recording can preempt between reads. */
            for (size_t loaded = 0U; loaded < count;) {
                if (nuwax_offline_audio_storage_busy()) { err = ESP_ERR_NOT_FINISHED; break; }
                size_t n = replay_read_slice_size(position + (uint32_t)loaded, count - loaded);
                const uint64_t pressure_before = pressure_us;
                err = replay_wait_capture_headroom(ws, epoch, NULL, &pressure_us);
                if (pressure_us != pressure_before) offline_replay_budget_relax(&budget);
                if (err != ESP_OK) break;
                uint64_t work_us = 0U;
                const uint64_t read_before = read_us;
                err = replay_read_validate(file, buffer + loaded, &n, &validator,
                    total, header, footer, &read_us, &validate_us, &work_us, &read_stats);
                if (err != ESP_OK) break;
                if (offline_replay_wait_relaxes(read_us - read_before,
                                               portTICK_PERIOD_MS * 1000U)) {
                    offline_replay_budget_relax(&budget);
                }
                loaded += n;
                if (offline_replay_budget_due(&budget, n, work_us)) {
                    vTaskDelay(1U);
                }
            }
        }
        if (err != ESP_OK) break;
        if (position < resume) { position += count; continue; }
        char request[48];
        snprintf(request, sizeof(request), "offline_window_%08lx", (unsigned long)esp_random());
        const int64_t batch_started = esp_timer_get_time();
        const uint64_t batch_pressure_before = pressure_us;
        uint64_t last_ack_wait_us = 0U;
        for (unsigned attempt = 0; attempt < REPLAY_CHUNK_ACK_ATTEMPTS; ++attempt) {
            err = replay_arm_ack(epoch, request, "offline_audio_chunk_ack", segment_id);
            if (err != ESP_OK) break;
            for (size_t sent = 0U; sent < count;) {
                if (nuwax_offline_audio_storage_busy()) { err = ESP_ERR_NOT_FINISHED; break; }
                size_t n = count - sent;
                if (n > tx_batch_frames * REPLAY_CHUNK_MAX_PAYLOAD_BYTES)
                    n = tx_batch_frames * REPLAY_CHUNK_MAX_PAYLOAD_BYTES;
                if (reader) {
                    bool data_blocked = false;
                    /* Start when one logical frame is ready. Waiting for four
                     * here unnecessarily serializes SD reads and network TX. */
                    const size_t first = n < REPLAY_CHUNK_MAX_PAYLOAD_BYTES
                        ? n : REPLAY_CHUNK_MAX_PAYLOAD_BYTES;
                    err = replay_prefetch_wait_bytes(reader, &reader->slots[read_slot], sent + first, &data_wait_us, &data_blocked);
                    if (data_blocked) offline_replay_budget_relax(&budget);
                    if (err != ESP_OK) break;
                }
                const uint64_t pressure_before = pressure_us;
                err = replay_wait_capture_headroom(ws, epoch, NULL, &pressure_us);
                if (pressure_us != pressure_before) offline_replay_budget_relax(&budget);
                if (err != ESP_OK) break;
                if (reader) {
                    /* Re-sample after capture pressure drains: the reader may
                     * have published more. Never shorten a logical frame to
                     * a partial read; offsets/flags and ACK retry bytes stay
                     * identical regardless of how TCP writes are grouped. */
                    const size_t published = atomic_load_explicit(
                        &reader->slots[read_slot].published, memory_order_acquire);
                    if (published < count) {
                        const size_t complete = (published - sent) /
                            REPLAY_CHUNK_MAX_PAYLOAD_BYTES * REPLAY_CHUNK_MAX_PAYLOAD_BYTES;
                        if (n > complete) n = complete;
                    }
                }
                const int64_t packet_started = esp_timer_get_time();
                const uint64_t encode_before = metrics.encode_us;
                nuwax_ws_archive_frame_t frames[NUWAX_WS_ARCHIVE_BATCH_MAX_FRAMES];
                size_t frame_count = 0U;
                for (size_t offset = 0U; offset < n; ++frame_count) {
                    size_t frame_bytes = n - offset;
                    if (frame_bytes > REPLAY_CHUNK_MAX_PAYLOAD_BYTES)
                        frame_bytes = REPLAY_CHUNK_MAX_PAYLOAD_BYTES;
                    uint8_t *prefix = packet + frame_count * 64U;
                    memset(prefix, 0, 64U);
                    memcpy(prefix, "NWXCH1", 6U);
                    uint32_t value = 2U;
                    memcpy(prefix + 8U, &value, 4U);
                    value = position + sent + offset; memcpy(prefix + 12U, &value, 4U);
                    value = frame_bytes; memcpy(prefix + 16U, &value, 4U);
                    memcpy(prefix + 20U, request, strlen(request));
                    value = sent + offset + frame_bytes == count ? 1U : 0U;
                    memcpy(prefix + 60U, &value, 4U);
                    frames[frame_count] = (nuwax_ws_archive_frame_t){
                        prefix, data + sent + offset, frame_bytes};
                    offset += frame_bytes;
                }
                /* Each frame keeps its own mask/header; only TCP writes are
                 * coalesced. The archive slot stays immutable until its ACK. */
                phase = esp_timer_get_time();
                const uint64_t packet_us = phase - packet_started;
                err = replay_send_batch(ws, epoch, frames, frame_count,
                                        wire, wire_capacity, &metrics);
                send_us += esp_timer_get_time() - phase;
                if (err != ESP_OK) break;
                tx_groups[frame_count - 1U]++;
                if (first_payload_us < 0) first_payload_us = esp_timer_get_time() - started;
                sent += n;
                if (offline_replay_budget_due(&budget, n, packet_us + metrics.encode_us - encode_before)) {
                    vTaskDelay(1U);
                }
            }
            if (err != ESP_OK) { replay_disarm_ack(); break; }
            uint32_t confirmed = 0U;
            phase = esp_timer_get_time();
            err = replay_wait_ack(ws, epoch, request, true, &confirmed);
            last_ack_wait_us = esp_timer_get_time() - phase;
            ack_us += last_ack_wait_us;
            if (err == ESP_OK && confirmed != position + count) err = ESP_ERR_INVALID_RESPONSE;
            if (err != ESP_ERR_TIMEOUT || !nuwax_ws_is_connected(ws) ||
                nuwax_ws_connection_epoch(ws) != epoch) break;
            ESP_LOGW(TAG, "offline window ACK retry segment=%s offset=%lu attempt=%u",
                     segment_id, (unsigned long)position, attempt + 1U);
        }
        if (err != ESP_OK) break;
        if (reader && reader->lane_probe && position == resume) {
            const uint64_t tx_wall = esp_timer_get_time() - batch_started;
            ESP_LOGI(TAG, "offline replay lane probe bytes=%u read_validate_wall_ms=%llu read_ms=%llu validate_ms=%llu read_pressure_ms=%llu tx_ack_wall_ms=%llu tx_pressure_ms=%llu read_KiBps=%llu tx_ack_KiBps=%llu",
                (unsigned)count,
                (unsigned long long)(reader->probe_read_wall_us / 1000U),
                (unsigned long long)(reader->read_us / 1000U),
                (unsigned long long)(reader->validate_us / 1000U),
                (unsigned long long)(reader->pressure_us / 1000U),
                (unsigned long long)(tx_wall / 1000U),
                (unsigned long long)((pressure_us - batch_pressure_before) / 1000U),
                (unsigned long long)(reader->probe_read_wall_us ? (uint64_t)count * 1000000U / 1024U / reader->probe_read_wall_us : 0U),
                (unsigned long long)(tx_wall ? (uint64_t)count * 1000000U / 1024U / tx_wall : 0U));
        }
        position += count; batches++;
        if (reader) {
            atomic_store_explicit(&reader->slots[read_slot].ready, false, memory_order_release);
            xSemaphoreGive(reader->space_signal);
            read_slot ^= 1U;
        }
        const int64_t batch_us = esp_timer_get_time() - batch_started;
        /* Serial fallback retains adaptive batches. Prefetch uses bounded
         * full slots, with cancellation still checked on every read/packet. */
        if (!reader) {
            if (batch_us < 750000 && window < capacity) {
                window *= 2U; if (window > capacity) window = capacity;
            } else if (batch_us > 1500000 && window > REPLAY_WINDOW_INITIAL_BYTES) window /= 2U;
        }
        /* A blocking ACK already gave the reader/idle tasks scheduling time.
         * Immediate ACKs still yield; byte/CPU budgets above remain in force. */
        if (!offline_replay_wait_relaxes(last_ack_wait_us, (uint64_t)portTICK_PERIOD_MS * 1000U)) {
            vTaskDelay(1U);
            ack_yields++;
        } else {
            ack_yields_saved++;
        }
        offline_replay_budget_relax(&budget);
    }
    if (reader) {
        const esp_err_t reader_err = replay_prefetch_join(reader, err != ESP_OK, &read_us, &validate_us);
        if (err == ESP_OK) err = reader_err;
    }
    const uint64_t elapsed = esp_timer_get_time() - started;
    ESP_LOGI(TAG, "offline window transfer segment=%s bytes=%lu batches=%lu capacity=%u read_validate_ms=%llu sd_read_ms=%llu validate_ms=%llu send_ms=%llu ack_ms=%llu elapsed_ms=%llu bytes_per_sec=%llu err=%s",
             segment_id, (unsigned long)(position > resume ? position - resume : 0U),
             (unsigned long)batches, (unsigned)capacity,
             (unsigned long long)((read_us + validate_us) / 1000U),
             (unsigned long long)(read_us / 1000U), (unsigned long long)(validate_us / 1000U),
             (unsigned long long)(send_us / 1000U),
             (unsigned long long)(ack_us / 1000U), (unsigned long long)(elapsed / 1000U),
             (unsigned long long)(elapsed && position > resume ? (uint64_t)(position-resume)*1000000U/elapsed : 0U),
             esp_err_to_name(err));
    ESP_LOGI(TAG, "offline send detail segment=%s calls=%lu short=%lu retries=%lu admission_ms=%llu io_lock_ms=%llu write_ms=%llu writable_wait_ms=%llu encode_ms=%llu write_bytes=%lu batch_calls=%lu batch_frames=%lu batch_max=%lu",
             segment_id, (unsigned long)metrics.write_calls, (unsigned long)metrics.short_writes,
             (unsigned long)metrics.retries, (unsigned long long)(metrics.admission_us / 1000U),
             (unsigned long long)(metrics.io_lock_us / 1000U), (unsigned long long)(metrics.write_us / 1000U),
             (unsigned long long)(metrics.writable_wait_us / 1000U), (unsigned long long)(metrics.encode_us / 1000U),
             (unsigned long)metrics.write_bytes, (unsigned long)metrics.batch_calls,
             (unsigned long)metrics.batch_frames, (unsigned long)metrics.batch_max_frames);
    ESP_LOGI(TAG, "offline replay capture pressure sender_wait_ms=%llu",
             (unsigned long long)(pressure_us / 1000U));
    ESP_LOGI(TAG, "offline replay sender work yields=%lu serial_direct_frames=%lu serial_copied_bytes=%lu",
             (unsigned long)budget.yields, (unsigned long)validator.direct_frames,
             (unsigned long)validator.copied_bytes);
    ESP_LOGI(TAG, "offline replay pipeline first_payload_ms=%lld data_wait_ms=%llu",
             (long long)(first_payload_us < 0 ? -1 : first_payload_us / 1000U),
             (unsigned long long)(data_wait_us / 1000U));
    ESP_LOGI(TAG, "offline replay batching groups_1=%lu groups_2=%lu groups_3=%lu groups_4=%lu ack_yields=%lu ack_yields_saved=%lu",
             (unsigned long)tx_groups[0], (unsigned long)tx_groups[1],
             (unsigned long)tx_groups[2], (unsigned long)tx_groups[3],
             (unsigned long)ack_yields, (unsigned long)ack_yields_saved);
    replay_read_release(&read_stats);
    if (!reader) replay_read_log_fallback(&read_stats);
    if (!reader) ESP_LOGI(TAG, "offline replay read path=descriptor validator=header_stream state_bytes=%u internal_bytes=%lu fallback_bytes=%lu copy_ms=%llu allocation_ms=%llu",
        (unsigned)sizeof(replay_stream_t),
        (unsigned long)read_stats.internal_bytes, (unsigned long)read_stats.fallback_bytes,
        (unsigned long long)(read_stats.copy_us / 1000U), (unsigned long long)(read_stats.allocation_us / 1000U));
    heap_caps_free(buffer);
    return err;
}

#include "offline_replay_metadata.h"

static esp_err_t replay_one_ready_file(nuwax_ws_client_t *ws,
                                  uint32_t epoch,
                                  const char *path,
                                  bool independent_transport, FILE *file, bool *file_closed)
{
    archive_header_t header = {0};
    archive_footer_t footer = {0};
    const int64_t metadata_started = esp_timer_get_time();
    struct stat st = {0};
    if (fstat(fileno(file), &st) != 0) {
        nuwax_sd_card_mark_io_fault(ESP_FAIL);
        return ESP_FAIL;
    }
    if (st.st_size < (off_t)(HEADER_BYTES + FOOTER_BYTES) ||
        (uint64_t)st.st_size > UINT32_MAX) return ESP_ERR_INVALID_SIZE;
    esp_err_t validation_err = replay_metadata_read_at(file, &header, sizeof(header), 0);
    if (validation_err == ESP_OK) {
        validation_err = replay_metadata_read_at(file, &footer, sizeof(footer),
                                                 st.st_size - (off_t)sizeof(footer));
    }
    if (validation_err != ESP_OK) {
        if (validation_err == ESP_FAIL) nuwax_sd_card_mark_io_fault(validation_err);
        return validation_err;
    }
    bool valid =
                 memcmp(header.magic, "NWXOFF1", 7U) == 0 &&
                 header.version == 1U && header.header_bytes == HEADER_BYTES &&
                 header.mode <= (uint8_t)NUWAX_OFFLINE_AUDIO_MODE_MONITORING &&
                 header.codec == NUWAX_AUDIO_CODEC_PCM16 &&
                 header.channels == 1U && header.sample_rate <= UINT16_MAX &&
                 header.sample_rate != 0U && header.frame_duration_ms != 0U;
    if (valid && (memcmp(footer.magic, "NWXEND1", 7U) != 0 ||
                   footer.version != 1U || footer.footer_bytes != FOOTER_BYTES)) {
        valid = false;
    }
    if (valid && !independent_transport) {
        const long footer_offset = (long)st.st_size - (long)sizeof(footer);
        const int64_t validate_started = esp_timer_get_time();
        validation_err = validate_archive_records(file, &header, &footer,
                                                  footer_offset);
        log_sd_step("replay_validate", validate_started);
        valid = validation_err == ESP_OK;
    }
    if (!valid) {
        const bool media_error = ferror(file) != 0;
        if (media_error) {
            nuwax_sd_card_mark_io_fault(ESP_FAIL);
        }
        return validation_err != ESP_OK ? validation_err : ESP_ERR_INVALID_SIZE;
    }
    const int64_t begin_started = esp_timer_get_time();
    const uint32_t total_bytes = (uint32_t)st.st_size;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char segment_id[48] = {0};
    /* The on-disk basename can exceed the protocol's compact identifier
     * buffer (session IDs are followed by mode/index/extension).  Truncating
     * it would make adjacent segments share an idempotency key.  Derive a
     * stable short id from the complete basename and retain the parsed
     * segment index; retries and reboot recovery therefore address exactly
     * the same archive without collisions between segments. */
    const size_t base_len = replay_basename_length(base);
    uint32_t basename_hash = crc32_update(0U, base, base_len);
    uint32_t segment_index = 0U;
    const char *last_underscore = NULL;
    for (const char *cursor = base; cursor < base + base_len; ++cursor) {
        if (*cursor == '_') {
            last_underscore = cursor;
        }
    }
    if (last_underscore && last_underscore + 1U < base + base_len) {
        char *end = NULL;
        const unsigned long parsed =
            strtoul(last_underscore + 1U, &end, 10);
        if (end == base + base_len && parsed <= UINT32_MAX) {
            segment_index = (uint32_t)parsed;
        }
    }
    snprintf(segment_id, sizeof(segment_id), "offline_%08lx_%08lx",
             (unsigned long)basename_hash,
             (unsigned long)segment_index);
    uint32_t nonce = (uint32_t)esp_random();
    char request_id[48] = {0};
    snprintf(request_id, sizeof(request_id), "offline_begin_%08lx",
             (unsigned long)nonce);
    cJSON *begin = cJSON_CreateObject();
    if (!begin) {
        return ESP_ERR_NO_MEM;
    }
    /* Keep the control JSON small.  The archive header is authoritative for
     * mode/session/audio parameters; this message only opens an identifiable,
     * resumable and integrity-checked byte transaction. */
    cJSON_AddStringToObject(begin, "type", "offline_audio_begin");
    /* Archive replay is storage-only.  The cloud must never route these bytes
     * through ASR, meeting transcription, or monitoring event detection. */
    cJSON_AddStringToObject(begin, "mode", "archive_only");
    cJSON_AddStringToObject(begin, "request_id", request_id);
    cJSON_AddStringToObject(begin, "segment_id", segment_id);
    cJSON_AddNumberToObject(begin, "total_bytes", total_bytes);
    cJSON_AddNumberToObject(begin, "crc32", footer.crc32);
    if (independent_transport) {
        cJSON_AddNumberToObject(begin, "replay_version", 2U);
        cJSON_AddNumberToObject(begin, "replay_revision", 2U);
        cJSON_AddNumberToObject(begin, "window_bytes", REPLAY_WINDOW_MAX_BYTES);
    }
    esp_err_t err = replay_arm_ack(epoch, request_id,
                                   "offline_audio_begin_ack", segment_id);
    if (err == ESP_OK) {
        err = replay_send_json(ws, epoch, begin, independent_transport);
    }
    cJSON_Delete(begin);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "offline replay begin send failed segment=%s err=%s",
                 segment_id, esp_err_to_name(err));
        replay_disarm_ack();
        return err;
    }
    uint32_t offset = 0U;
    err = replay_wait_ack(ws, epoch, request_id, false, &offset);
    if (err != ESP_OK || offset > total_bytes) {
        ESP_LOGW(TAG,
                 "offline replay begin ACK failed segment=%s err=%s next_offset=%lu",
                 segment_id, esp_err_to_name(err), (unsigned long)offset);
        /* A bad peer offset must not quarantine a validated local archive. */
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }
    portENTER_CRITICAL(&s_replay_lock);
    const uint32_t window_limit = independent_transport ? s.replay_ack.window_bytes : 0U;
    portEXIT_CRITICAL(&s_replay_lock);
    ESP_LOGI(TAG, "offline replay negotiated version=%u window_bytes=%lu segment=%s resume=%lu",
             window_limit ? 2U : 1U, (unsigned long)window_limit, segment_id,
             (unsigned long)offset);
    const int64_t payload_started = esp_timer_get_time();
    if (window_limit) {
        err = replay_send_windowed(ws, epoch, file, segment_id, total_bytes, offset,
                                  window_limit, &header, &footer);
        if (err != ESP_OK) return err;
        offset = total_bytes;
    } else if (independent_transport) {
        /* Old gateways ignore negotiation. Keep their original checked v1 path. */
        err = validate_archive_records(file, &header, &footer, (long)total_bytes - FOOTER_BYTES);
        if (err != ESP_OK) return err;
    }
    uint8_t *chunk = offset < total_bytes ? heap_caps_malloc(
        REPLAY_CHUNK_HEADER_BYTES + REPLAY_CHUNK_MAX_PAYLOAD_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (offset < total_bytes && !chunk) {
        return ESP_ERR_NO_MEM;
    }
    uint32_t read_position = UINT32_MAX;
    uint64_t pressure_wait_us = 0U;
    while (offset < total_bytes && nuwax_ws_is_connected(ws) &&
           nuwax_ws_connection_epoch(ws) == epoch) {
        /* The replay worker shares the authenticated WebSocket with live
         * audio. Never hold the transport send lock ahead of a real-time
         * uplink sender; yielding here lets conversation/meeting/monitoring
         * frames keep their deadline while the archive resumes afterward. */
        while (!independent_transport &&
               !nuwax_offline_audio_storage_busy() &&
               !nuwax_audio_uplink_background_ready() &&
               nuwax_ws_is_connected(ws) &&
               nuwax_ws_connection_epoch(ws) == epoch) {
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
            if (atomic_load_explicit(&s_formal_replay_cancel_requested,
                                     memory_order_acquire)) {
                err = ESP_ERR_INVALID_STATE;
                break;
            }
#endif
            vTaskDelay(pdMS_TO_TICKS(20U));
        }
        if (err != ESP_OK || !nuwax_ws_is_connected(ws) ||
            nuwax_ws_connection_epoch(ws) != epoch) {
            break;
        }
        if (nuwax_offline_audio_storage_busy()) {
            err = ESP_ERR_NOT_FINISHED;
            break;
        }
        const atomic_bool *pressure_cancel = NULL;
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
        if (!independent_transport) pressure_cancel = &s_formal_replay_cancel_requested;
#endif
        err = replay_wait_capture_headroom(ws, epoch, pressure_cancel, &pressure_wait_us);
        if (err != ESP_OK) break;
        /* Preserve stdio read-ahead on sequential ACKs; seek only for initial
         * resume or a partial ACK. Seeking every chunk discards cached SD data. */
        if (read_position != offset && fseek(file, (long)offset, SEEK_SET) != 0) {
            err = ESP_FAIL;
            nuwax_sd_card_mark_io_fault(ESP_FAIL);
            break;
        }
        const size_t max_payload = independent_transport
                                        ? REPLAY_CHUNK_MAX_PAYLOAD_BYTES
                                        : REPLAY_FORMAL_CHUNK_PAYLOAD_BYTES;
        const size_t want = (total_bytes - offset) > max_payload
                                ? max_payload
                                : (size_t)(total_bytes - offset);
        const size_t got = fread(chunk + 64U, 1U, want, file);
        if (got != want) {
            err = ESP_FAIL;
            if (ferror(file)) {
                nuwax_sd_card_mark_io_fault(ESP_FAIL);
            }
            break;
        }
        read_position = offset + (uint32_t)got;
        memset(chunk, 0, 64U);
        memcpy(chunk, "NWXCH1", 6U);
        uint32_t version = 1U;
        uint32_t payload_offset = offset;
        uint32_t payload_len = (uint32_t)got;
        memcpy(chunk + 8U, &version, sizeof(version));
        memcpy(chunk + 12U, &payload_offset, sizeof(payload_offset));
        memcpy(chunk + 16U, &payload_len, sizeof(payload_len));
        snprintf(request_id, sizeof(request_id), "offline_chunk_%08lx",
                 (unsigned long)esp_random());
        /* The binary header carries the chunk request id. Bytes 60..63 are
         * reserved; the complete segment id is bound by the preceding begin
         * transaction and must never be represented by a truncated prefix. */
        memset(chunk + 20U, 0, 40U);
        memcpy(chunk + 20U, request_id,
               strlen(request_id) < 40U ? strlen(request_id) : 40U);
        memset(chunk + 60U, 0, 4U);
        uint32_t acknowledged = 0U;
        err = replay_exchange_chunk(ws, epoch, chunk, got + 64U,
                                     request_id, segment_id, independent_transport,
                                     &acknowledged);
        const uint32_t sent_end = offset + (uint32_t)got;
        if (err != ESP_OK || acknowledged <= offset ||
            acknowledged > sent_end) {
            ESP_LOGW(TAG,
                     "offline replay chunk ACK failed segment=%s offset=%lu err=%s next_offset=%lu",
                     segment_id, (unsigned long)offset, esp_err_to_name(err),
                     (unsigned long)acknowledged);
            err = err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
            break;
        }
        offset = acknowledged;
        /* One short background frame per scheduling slice; never burst a
         * complete SD segment ahead of a newly admitted realtime sender. */
        /* ACK already yields to the receiver. A one-tick cooperative yield
         * keeps the independent test link fast and lets realtime frames win
         * the next I/O-lock admission on the formal shared connection. */
        vTaskDelay(pdMS_TO_TICKS(REPLAY_CHUNK_YIELD_MS));
    }
    heap_caps_free(chunk);
    if (err != ESP_OK || offset != total_bytes) {
        return err == ESP_OK ? ESP_ERR_TIMEOUT : err;
    }
    if (nuwax_offline_audio_storage_busy()) {
        return ESP_ERR_NOT_FINISHED;
    }
    const int64_t commit_started = esp_timer_get_time();
    snprintf(request_id, sizeof(request_id), "offline_commit_%08lx",
             (unsigned long)esp_random());
    cJSON *commit = cJSON_CreateObject();
    if (!commit) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(commit, "type", "offline_audio_commit");
    cJSON_AddStringToObject(commit, "mode", "archive_only");
    cJSON_AddStringToObject(commit, "request_id", request_id);
    cJSON_AddStringToObject(commit, "segment_id", segment_id);
    cJSON_AddNumberToObject(commit, "total_bytes", total_bytes);
    cJSON_AddNumberToObject(commit, "crc32", footer.crc32);
    err = replay_arm_ack(epoch, request_id,
                         "offline_audio_commit_ack", segment_id);
    if (err == ESP_OK) {
        err = replay_send_json(ws, epoch, commit, independent_transport);
    }
    cJSON_Delete(commit);
    if (err == ESP_OK) {
        err = replay_wait_ack(ws, epoch, request_id, false, NULL);
    } else {
        ESP_LOGW(TAG, "offline replay commit send failed segment=%s err=%s",
                 segment_id, esp_err_to_name(err));
        replay_disarm_ack();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "offline replay commit ACK failed segment=%s err=%s",
                 segment_id, esp_err_to_name(err));
    }
    const int64_t cleanup_started = esp_timer_get_time();
    fclose(file);
    *file_closed = true;
    /* A new recording may start while commit waits for its ACK. Keep the
     * local file until idle; begin/commit is idempotent on the next pass. */
    if (err == ESP_OK && nuwax_offline_audio_storage_busy()) {
        return ESP_ERR_NOT_FINISHED;
    }
    if (err == ESP_OK) {
        if (remove(path) != 0) {
            ESP_LOGW(TAG,
                     "offline segment commit acknowledged but delete failed segment=%s errno=%d",
                     segment_id, errno);
            return ESP_FAIL;
        }
        if (!replay_path_quarantined(path) &&
            atomic_load_explicit(&s.ready_count, memory_order_acquire) > 0U) {
            (void)atomic_fetch_sub_explicit(&s.ready_count, 1U,
                                            memory_order_acq_rel);
        }
        atomic_store_explicit(&s_directory_dirty, true, memory_order_release);
        if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            s.used_bytes = s.used_bytes >= total_bytes
                               ? s.used_bytes - total_bytes
                               : 0U;
            xSemaphoreGive(s.lock);
        } else {
            /* A stale high quota estimate is safer than under-accounting.
             * Keep the successful cloud transaction committed and rescan on
             * the next boot if the writer lock is momentarily unavailable. */
            atomic_store_explicit(&s.degraded, true, memory_order_release);
            ESP_LOGW(TAG,
                     "offline quota update deferred after delete segment=%s",
                     segment_id);
        }
        ESP_LOGI(TAG, "offline replay phases metadata_ms=%lu begin_ms=%lu payload_ms=%lu commit_ms=%lu cleanup_ms=%lu",
                 (unsigned long)((begin_started - metadata_started) / 1000U),
                 (unsigned long)((payload_started - begin_started) / 1000U),
                 (unsigned long)((commit_started - payload_started) / 1000U),
                 (unsigned long)((cleanup_started - commit_started) / 1000U),
                 (unsigned long)((esp_timer_get_time() - cleanup_started) / 1000U));
        ESP_LOGI(TAG, "offline segment acknowledged and deleted segment=%s bytes=%lu",
                 segment_id, (unsigned long)total_bytes);
    }
    return err;
}

static esp_err_t replay_one_ready(nuwax_ws_client_t *ws, uint32_t epoch,
                                   const char *path, bool independent_transport)
{
    if (nuwax_offline_audio_storage_busy()) return ESP_ERR_NOT_FINISHED;
    const int64_t segment_started = esp_timer_get_time();
    const nuwax_sd_bulk_stats_t bulk_before = nuwax_sd_bulk_stats();
    const nuwax_sd_event_profile_t event_before = nuwax_sd_event_profile();
    nuwax_sd_card_log_clock();
    char *buffer = heap_caps_malloc(ARCHIVE_STDIO_BUFFER_BYTES,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) return ESP_ERR_NO_MEM;
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (errno != ENOENT) nuwax_sd_card_mark_io_fault(ESP_FAIL);
        heap_caps_free(buffer);
        return ESP_FAIL;
    }
    if (setvbuf(file, buffer, _IOFBF, ARCHIVE_STDIO_BUFFER_BYTES) != 0) {
        fclose(file);
        heap_caps_free(buffer);
        return ESP_ERR_NO_MEM;
    }
    bool file_closed = false;
    esp_err_t err = replay_one_ready_file(ws, epoch, path,
                                               independent_transport, file, &file_closed);
    if (!file_closed) fclose(file);
    heap_caps_free(buffer);
    /* Recheck a high-speed CRC/size failure only after ordered slow remount,
     * never quarantine a possibly transient bus error at the same speed. */
    if (nuwax_sd_card_fallback_after_read_error(err)) err = ESP_FAIL;
    if (err == ESP_OK) archive_remove_empty_bucket(path);
    const nuwax_sd_bulk_stats_t bulk_after = nuwax_sd_bulk_stats();
    const nuwax_sd_event_profile_t event_after = nuwax_sd_event_profile();
    const uint32_t empty_events = event_after.empty_entry_events - event_before.empty_entry_events;
    ESP_LOGI(TAG, "offline SD event profile events=%lu queued_on_entry=%lu empty_entry=%lu empty_lag_avg_us=%lu empty_lag_total_us=%lu empty_lag_gt1ms=%lu empty_lag_gt5ms=%lu receive_wait_ms=%lu",
             (unsigned long)(event_after.events - event_before.events),
             (unsigned long)(event_after.queued_on_entry - event_before.queued_on_entry),
             (unsigned long)empty_events,
             (unsigned long)(empty_events ? (uint32_t)(event_after.empty_entry_lag_us - event_before.empty_entry_lag_us) / empty_events : 0U),
             (unsigned long)(event_after.empty_entry_lag_us - event_before.empty_entry_lag_us),
             (unsigned long)(event_after.empty_entry_over1ms - event_before.empty_entry_over1ms),
             (unsigned long)(event_after.empty_entry_over5ms - event_before.empty_entry_over5ms),
             (unsigned long)((uint32_t)(event_after.receive_wait_us - event_before.receive_wait_us) / 1000U));
    ESP_LOGI(TAG, "offline SD read profile calls=%lu sectors=%lu wall_ms=%lu errors=%lu buckets_le2_le5_le10_le20_gt20_ms=%lu,%lu,%lu,%lu,%lu",
             (unsigned long)(bulk_after.read_calls - bulk_before.read_calls),
             (unsigned long)(bulk_after.read_sectors - bulk_before.read_sectors),
             (unsigned long)((uint32_t)(bulk_after.read_us - bulk_before.read_us) / 1000U),
             (unsigned long)(bulk_after.read_errors - bulk_before.read_errors),
             (unsigned long)(bulk_after.read_le2ms - bulk_before.read_le2ms),
             (unsigned long)(bulk_after.read_le5ms - bulk_before.read_le5ms),
             (unsigned long)(bulk_after.read_le10ms - bulk_before.read_le10ms),
             (unsigned long)(bulk_after.read_le20ms - bulk_before.read_le20ms),
             (unsigned long)(bulk_after.read_gt20ms - bulk_before.read_gt20ms));
    ESP_LOGI(TAG, "offline SD bulk commands=%lu sectors=%lu fallback_sectors=%lu native_calls=%lu native_sectors=%lu",
             (unsigned long)(bulk_after.commands - bulk_before.commands),
             (unsigned long)(bulk_after.sectors - bulk_before.sectors),
             (unsigned long)(bulk_after.fallback_sectors - bulk_before.fallback_sectors),
             (unsigned long)(bulk_after.native_calls - bulk_before.native_calls),
             (unsigned long)(bulk_after.native_sectors - bulk_before.native_sectors));
    ESP_LOGI(TAG, "offline SD timing command_ms=%lu copy_ms=%lu allocation_ms=%lu replay_priority=%u",
             (unsigned long)((uint32_t)(bulk_after.command_us - bulk_before.command_us) / 1000U),
             (unsigned long)((uint32_t)(bulk_after.copy_us - bulk_before.copy_us) / 1000U),
             (unsigned long)((uint32_t)(bulk_after.allocation_us - bulk_before.allocation_us) / 1000U),
             (unsigned)uxTaskPriorityGet(NULL));
    ESP_LOGI(TAG, "offline segment total elapsed_ms=%llu err=%s path=%s",
             (unsigned long long)((esp_timer_get_time() - segment_started) / 1000U),
             esp_err_to_name(err), path);
    return err;
}

static esp_err_t find_next_ready_name(const char *after,
                                      char *name,
                                      size_t name_size,
                                      bool *found)
{
    if (!after || !name || name_size == 0U || !found) {
        return ESP_ERR_INVALID_ARG;
    }
    *found = false;
    name[0] = '\0';
    /* Replay is serialized by replay_running. Cache a sorted page of names
     * so a large FAT directory is not rescanned for every 1 MiB segment.
     * A fresh pass always invalidates the page; files are still opened and
     * validated individually before commit/delete. */
    enum { READY_PAGE_NAMES = 64, READY_NAME_BYTES = 128 };
    static char (*page)[READY_NAME_BYTES];
    static size_t page_count;
    static char page_after[READY_NAME_BYTES];
    if (!page) page = heap_caps_malloc(READY_PAGE_NAMES * READY_NAME_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!page) return ESP_ERR_NO_MEM;
    if (after[0] && strcmp(after, page_after) == 0 && page_count) {
        if (strlen(page[0]) + 1U > name_size) return ESP_ERR_INVALID_SIZE;
        strcpy(name, page[0]); strcpy(page_after, name);
        memmove(page, page + 1U, (--page_count) * READY_NAME_BYTES);
        *found = true;
        return ESP_OK;
    }
    page_count = 0U;
    archive_dir_t walk;
    archive_dir_t *dir = archive_dir_open(&walk);
    if (!dir) {
        /* ROOT_DIR is created before the spool is admitted. A media-level
         * errno means that the mounted filesystem is no longer trustworthy;
         * descriptor/memory pressure, however, is transient and must not
         * force a remount while a live writer is still using the card. */
        const int open_errno = errno;
        if (open_errno == ENOENT || open_errno == EIO ||
            open_errno == ENODEV || open_errno == ENXIO) {
            nuwax_sd_card_mark_io_fault(ESP_FAIL);
        }
        ESP_LOGW(TAG, "offline ready directory open failed errno=%d", open_errno);
        return ESP_ERR_NOT_FOUND;
    }
    struct dirent *entry;
    while ((entry = archive_dir_read(dir)) != NULL) {
        if (nuwax_offline_audio_storage_busy()) {
            archive_dir_close(dir);
            return ESP_ERR_NOT_FINISHED;
        }
        const char *dot = strrchr(entry->d_name, '.');
        const size_t candidate_len = strlen(entry->d_name);
        const bool recheck = atomic_load_explicit(&s_quarantine_recheck_pending, memory_order_acquire) &&
            candidate_len >= 10U && strcmp(entry->d_name + candidate_len - 10U, ".ready.bad") == 0;
        if (!dot || (strcmp(dot, ".ready") != 0 && !recheck) ||
            strcmp(entry->d_name, after) <= 0) {
            continue;
        }
        const size_t entry_len = strlen(entry->d_name);
        if (entry_len + 1U > name_size) {
            atomic_store_explicit(&s.degraded, true, memory_order_release);
            ESP_LOGW(TAG, "ignoring overlong ready archive name=%s",
                     entry->d_name);
            continue;
        }
        if (entry_len >= READY_NAME_BYTES) continue;
        size_t at = 0U;
        while (at < page_count && strcmp(page[at], entry->d_name) < 0) ++at;
        if (at == READY_PAGE_NAMES) continue;
        if (page_count < READY_PAGE_NAMES) ++page_count;
        memmove(page + at + 1U, page + at,
                (page_count - at - 1U) * READY_NAME_BYTES);
        memcpy(page[at], entry->d_name, entry_len + 1U);
    }
    const int scan_error = dir->error;
    archive_dir_close(dir);
    if (scan_error) {
        page_count = 0U;
        ESP_LOGW(TAG, "offline ready directory read failed errno=%d", scan_error);
        return ESP_FAIL;
    }
    if (page_count) {
        strcpy(name, page[0]); strcpy(page_after, name);
        memmove(page, page + 1U, (--page_count) * READY_NAME_BYTES);
        *found = true;
    }
    return ESP_OK;
}

static bool replay_ota_active(void) {
#if CONFIG_NUWAX_OTA_ENABLE
    return nuwax_ota_device_is_active();
#else
    return false;
#endif
}

static esp_err_t replay_ready_impl(nuwax_ws_client_t *ws,
                                   uint32_t expected_epoch,
                                   bool independent_transport)
{
    if (!enabled()) {
        return ESP_OK;
    }
    if (nuwax_offline_audio_storage_busy()) return ESP_ERR_NOT_FINISHED;
    if (!runtime_initialized() || !ws || expected_epoch == 0U ||
        !nuwax_ws_is_connected(ws) ||
        nuwax_ws_connection_epoch(ws) != expected_epoch) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!nuwax_sd_card_is_mounted()) return ESP_ERR_NOT_FINISHED;
    if (atomic_load_explicit(&s.media_recovery_requested,
                             memory_order_acquire)) {
        return ESP_ERR_NOT_FINISHED;
    }
    bool expected_running = false;
    if (!atomic_compare_exchange_strong_explicit(
            &s.replay_running, &expected_running, true,
            memory_order_acq_rel, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (nuwax_power_ctrl_shutdown_preparing() ||
        atomic_load_explicit(&s_shutdown_requested, memory_order_acquire) ||
        (independent_transport && replay_ota_active()) ||
        atomic_load_explicit(&s.media_recovery_requested, memory_order_acquire)) {
        atomic_store_explicit(&s.replay_running, false,
                              memory_order_release);
        return ESP_ERR_NOT_FINISHED;
    }
    esp_err_t result = ESP_OK;
    char after_name[128] = {0};
    for (;;) {
 #if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
        if (atomic_load_explicit(&s_formal_replay_cancel_requested,
                                 memory_order_acquire)) {
            result = result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
            break;
        }
 #endif
        char next_name[128] = {0};
        bool found = false;
        if (nuwax_offline_audio_storage_busy()) {
            result = ESP_ERR_NOT_FINISHED;
            break;
        }
        const esp_err_t find_err = find_next_ready_name(
            after_name, next_name, sizeof(next_name), &found);
        if (find_err != ESP_OK) {
            result = result == ESP_OK ? find_err : result;
            break;
        }
        if (!found) {
            atomic_store_explicit(&s_quarantine_recheck_pending, false, memory_order_release);
            break;
        }
        char path[160] = {0};
        if (!compose_child_path(path, sizeof(path), next_name)) {
            ESP_LOGW(TAG, "ignoring overlong ready archive name=%s", next_name);
            /* Advance past an invalid entry; otherwise the lexical lookup
             * would select the same overlong name forever in this replay
             * pass. The file remains untouched for operator inspection. */
            strlcpy(after_name, next_name, sizeof(after_name));
            continue;
        }
        const bool was_quarantined = replay_path_quarantined(path);
        if (was_quarantined && !independent_transport) {
            const esp_err_t restore_err = restore_quarantined_archive(path);
            if (restore_err != ESP_OK) {
                ESP_LOGW(TAG, "offline quarantine retained after recheck path=%s err=%s", path, esp_err_to_name(restore_err));
                if (restore_err == ESP_ERR_INVALID_SIZE || restore_err == ESP_ERR_INVALID_CRC ||
                    restore_err == ESP_ERR_INVALID_STATE) {
                    strlcpy(after_name, next_name, sizeof(after_name));
                    continue;
                }
                result = restore_err;
                break;
            }
        }
        if (was_quarantined && independent_transport) {
            ESP_LOGI(TAG, "offline quarantine replay validating in single pass path=%s", path);
        }
        const esp_err_t archive_err = replay_one_ready(
            ws, expected_epoch, path, independent_transport);
        if (archive_err != ESP_OK) {
            /* One damaged or temporarily rejected archive must not prevent
             * later segments from being delivered.  Retain the first error
             * for diagnostics; the file itself remains for a later retry. */
            if (result == ESP_OK) {
                result = archive_err;
            }
            ESP_LOGW(TAG,
                     "offline archive replay deferred segment=%s err=%s",
                     path, esp_err_to_name(archive_err));
            if (!nuwax_sd_card_is_mounted()) {
                ESP_LOGW(TAG, "offline replay stopped for ordered SD recovery; archives retained");
                break;
            }
            /* A timeout on the authenticated connection usually means the
             * cloud has not deployed archive replay yet (or the transport is
             * unavailable). Do not spend another timeout on every .ready
             * file: retain the whole batch and let realtime mode resume. */
            if (archive_err == ESP_ERR_NOT_FINISHED) {
                ESP_LOGI(TAG, "offline replay yielded to recording; archive retained");
                break;
            }
            if (archive_err == ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG,
                         "offline archive replay paused after first timeout; %s remains for a later epoch",
                         path);
                break;
            }
            bool quarantined = false;
            if (was_quarantined && (archive_err == ESP_ERR_INVALID_SIZE ||
                                    archive_err == ESP_ERR_INVALID_CRC)) {
                /* Leave the original quarantine untouched. No commit is sent
                 * before full validation, and unrelated ready files can proceed. */
                strlcpy(after_name, next_name, sizeof(after_name));
                continue;
            }
            if ((archive_err == ESP_ERR_INVALID_SIZE ||
                 archive_err == ESP_ERR_INVALID_CRC) &&
                nuwax_ws_is_connected(ws) &&
                nuwax_ws_connection_epoch(ws) == expected_epoch) {
                const esp_err_t verified_error = recheck_archive_path(path);
                if (verified_error != ESP_ERR_INVALID_SIZE && verified_error != ESP_ERR_INVALID_CRC) {
                    ESP_LOGE(TAG, "offline archive retained; corruption not confirmed upload_err=%s recheck=%s path=%s",
                             esp_err_to_name(archive_err), esp_err_to_name(verified_error), path);
                    result = verified_error == ESP_OK ? ESP_ERR_INVALID_STATE : verified_error;
                    break;
                }
                char bad_path[160] = {0};
                if (strlen(path) + strlen(".bad") + 1U < sizeof(bad_path)) {
                    snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
                    struct stat occupied;
                    if (stat(bad_path, &occupied) != 0 && errno == ENOENT &&
                        !nuwax_offline_audio_storage_busy() && rename(path, bad_path) == 0) {
                        quarantined = true;
                        if (atomic_load_explicit(&s.ready_count,
                                                 memory_order_acquire) > 0U) {
                            (void)atomic_fetch_sub_explicit(
                                &s.ready_count, 1U, memory_order_acq_rel);
                        }
                        ESP_LOGE(TAG,
                                 "offline archive quarantined as corrupt path=%s",
                                 bad_path);
                    }
                }
            }
            if (quarantined) {
                strlcpy(after_name, next_name, sizeof(after_name));
                continue;
            }
            if (!nuwax_ws_is_connected(ws) ||
                nuwax_ws_connection_epoch(ws) != expected_epoch) {
                /* A transport/epoch failure invalidates the remaining batch;
                 * leave every uncommitted file on SD for the next epoch. */
                break;
            }
            /* Preserve temporary/media errors for a later scheduler pass. Do
             * not immediately select the same filename again in this pass. */
            break;
        }
        /* Continue from the last successfully handled lexical name. This
         * avoids rescanning the already-consumed prefix of a large backlog;
         * a later scheduler pass starts from the beginning so files created
         * concurrently with replay are still observed. */
        strlcpy(after_name, next_name, sizeof(after_name));
    }
    atomic_store_explicit(&s.replay_running, false, memory_order_release);
    return result;
}

#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
static void formal_replay_task(void *arg)
{
    (void)arg;
    const formal_replay_context_t context = s_formal_replay_context;
    const esp_err_t result = replay_ready_impl(context.ws, context.epoch, false);
    ESP_LOGI(TAG,
             "offline formal replay finished err=%s epoch=%lu stack_min_free=%u",
             esp_err_to_name(result),
             (unsigned long)context.epoch,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    /* Clear the cancellation latch before publishing worker completion so a
     * new epoch cannot inherit a prior disconnect's stop request. */
    atomic_store_explicit(&s_formal_replay_cancel_requested, false,
                          memory_order_release);
    if (result == ESP_ERR_NOT_FINISHED) {
        /* The launch gate is only an admission throttle. A worker that yielded
         * to recording must not leave the same epoch suppressed for the
         * normal five-second launch window. */
        s_replay_launch_epoch = 0U;
        s_replay_launch_deadline_ms = 0U;
        atomic_store_explicit(&s_replay_retry_required, true,
                              memory_order_release);
    }
    atomic_store_explicit(&s_formal_replay_running, false,
                          memory_order_release);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t replay_formal_start_locked(nuwax_ws_client_t *ws,
                                                  uint32_t expected_epoch)
{
    if (atomic_load_explicit(&s_shutdown_requested, memory_order_acquire) ||
        nuwax_power_ctrl_shutdown_preparing()) return ESP_ERR_INVALID_STATE;
    if (nuwax_offline_audio_storage_busy()) {
        atomic_store_explicit(&s_replay_retry_required, true,
                              memory_order_release);
        return ESP_ERR_NOT_FINISHED;
    }
    if (!enabled() || !runtime_initialized() || !ws || expected_epoch == 0U ||
        !nuwax_ws_is_connected(ws) ||
        nuwax_ws_connection_epoch(ws) != expected_epoch) {
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_load_explicit(&s.media_recovery_requested,
                             memory_order_acquire)) {
        atomic_store_explicit(&s_replay_retry_required, true,
                              memory_order_release);
        return ESP_ERR_NOT_FINISHED;
    }
    if (atomic_load_explicit(&s_archive_capability_epoch, memory_order_acquire) !=
        expected_epoch) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!nuwax_offline_audio_has_ready() ||
        atomic_load_explicit(&s_formal_replay_running, memory_order_acquire)) {
        return ESP_OK;
    }
    const uint32_t launch_now = offline_now_ms();
    if (!atomic_load_explicit(&s_media_ready, memory_order_acquire) ||
        !nuwax_audio_uplink_background_ready() ||
        (s_replay_launch_epoch == expected_epoch &&
         !offline_deadline_elapsed(launch_now, s_replay_launch_deadline_ms))) {
        return ESP_ERR_NOT_FINISHED;
    }
    s_replay_launch_epoch = expected_epoch;
    s_replay_launch_deadline_ms = launch_now + 5000U;
    bool expected_running = false;
    if (!atomic_compare_exchange_strong_explicit(
            &s_formal_replay_running, &expected_running, true,
            memory_order_acq_rel, memory_order_acquire)) {
        return ESP_OK;
    }
    if (atomic_load_explicit(&s.media_recovery_requested,
                             memory_order_acquire)) {
        atomic_store_explicit(&s_formal_replay_running, false,
                              memory_order_release);
        return ESP_ERR_NOT_FINISHED;
    }
    atomic_store_explicit(&s_formal_replay_cancel_requested, false,
                          memory_order_release);
    s_formal_replay_context.ws = ws;
    s_formal_replay_context.epoch = expected_epoch;
    if (xTaskCreateWithCaps(formal_replay_task,
                            "offline_replay_formal",
                            OFFLINE_REPLAY_FORMAL_TASK_STACK_BYTES,
                            NULL,
                            OFFLINE_REPLAY_FORMAL_TASK_PRIORITY,
                            NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        atomic_store_explicit(&s_formal_replay_running, false,
                              memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "offline formal replay scheduled epoch=%lu stack_bytes=%u",
             (unsigned long)expected_epoch,
             (unsigned)OFFLINE_REPLAY_FORMAL_TASK_STACK_BYTES);
    return ESP_OK;
}

esp_err_t nuwax_offline_audio_replay_formal_start(nuwax_ws_client_t *ws,
                                                uint32_t expected_epoch)
{
    if (!runtime_initialized() || !s.lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s.lock, 0) != pdTRUE) return ESP_ERR_NOT_FINISHED;
    const esp_err_t result = replay_formal_start_locked(ws, expected_epoch);
    xSemaphoreGive(s.lock);
    return result;
}

bool nuwax_offline_audio_replay_formal_running(void)
{
    return atomic_load_explicit(&s_formal_replay_running,
                                memory_order_acquire);
}

esp_err_t nuwax_offline_audio_replay_formal_cancel(uint32_t expected_epoch,
                                                   uint32_t timeout_ms)
{
    if (expected_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load_explicit(&s_formal_replay_running,
                             memory_order_acquire)) {
        return ESP_OK;
    }
    const formal_replay_context_t context = s_formal_replay_context;
    if (context.epoch != expected_epoch) {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_formal_replay_cancel_requested, true,
                          memory_order_release);
    if (s.events) {
        /* Wake a worker blocked in replay_wait_ack immediately. A send already
         * in progress remains bounded by its transport timeout. */
        (void)xEventGroupSetBits(s.events,
                                 REPLAY_ACK_BIT | REPLAY_CHUNK_ACK_BIT);
    }
    const uint32_t start_ms = offline_now_ms();
    const uint32_t deadline_ms = start_ms + timeout_ms;
    while (atomic_load_explicit(&s_formal_replay_running,
                                memory_order_acquire)) {
        if (timeout_ms == 0U ||
            offline_deadline_elapsed(offline_now_ms(), deadline_ms)) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    return ESP_OK;
}
#else
esp_err_t nuwax_offline_audio_replay_formal_start(nuwax_ws_client_t *ws,
                                                  uint32_t expected_epoch)
{
    (void)ws;
    (void)expected_epoch;
    return ESP_ERR_NOT_SUPPORTED;
}

bool nuwax_offline_audio_replay_formal_running(void)
{
    return false;
}

esp_err_t nuwax_offline_audio_replay_formal_cancel(uint32_t expected_epoch,
                                                   uint32_t timeout_ms)
{
    (void)expected_epoch;
    (void)timeout_ms;
    return ESP_OK;
}
#endif

esp_err_t nuwax_offline_audio_replay_ready(nuwax_ws_client_t *ws,
                                           uint32_t expected_epoch)
{
    return replay_ready_impl(ws, expected_epoch, false);
}

#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE
/* The replay gateway is intentionally a separate protocol boundary.  It may
 * receive a hello/heartbeat at the transport level, but no message from this
 * connection is ever dispatched to the normal conversation state machine. */
static esp_err_t replay_test_send_hello(nuwax_ws_client_t *ws)
{
    if (!ws || !nuwax_ws_is_connected(ws)) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t epoch = nuwax_ws_connection_epoch(ws);
    const char *device_id = nuwax_device_uid_string();
    if (epoch == 0U || !device_id || device_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    /* This task deliberately uses a PSRAM stack. The normal cloud hello reads
     * OTA state and may freeze the external-memory cache while mapping otadata,
     * which is illegal from a PSRAM stack. The LAN replay receiver only needs
     * a stable device id and capability marker, so keep this handshake free of
     * OTA, NVS, partition and other flash-sensitive calls. */
    char hello[256];
    const int written = snprintf(
        hello, sizeof(hello),
        "{\"type\":\"hello\",\"version\":1,\"device_id\":\"%s\","
        "\"transport\":\"websocket\",\"role\":\"offline_audio_replay\","
        "\"features\":{\"offline_audio_replay_v1\":true}}",
        device_id);
    if (written <= 0 || (size_t)written >= sizeof(hello)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return nuwax_ws_send_text_with_timeout_if_epoch(
        ws, hello, REPLAY_TEST_HELLO_SEND_TIMEOUT_MS, epoch);
}

static void replay_test_on_text(const char *payload, int len, void *user_ctx)
{
    nuwax_ws_client_t *ws = (nuwax_ws_client_t *)user_ctx;
    const uint32_t epoch = ws ? nuwax_ws_connection_epoch(ws) : 0U;
    (void)nuwax_offline_audio_handle_protocol_text(payload, len, epoch);
}

static bool replay_test_connect_cancelled(void *ctx)
{
    (void)ctx;
    return nuwax_power_ctrl_shutdown_preparing() ||
           nuwax_offline_audio_storage_busy() || replay_ota_active();
}

static void replay_test_task(void *arg)
{
    (void)arg;
    atomic_store_explicit(&s_replay_lane_probe_taken, false, memory_order_relaxed);
    nuwax_ws_client_t *replay_ws = &s_replay_test_ws;
    ESP_LOGI(TAG,
             "offline replay test task started stack_min_free=%u stack_bytes=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)OFFLINE_REPLAY_TEST_TASK_STACK_BYTES);
    if (!s_replay_test_ws_initialized) {
        nuwax_ws_init(replay_ws);
        s_replay_test_ws_initialized = true;
    }
    esp_err_t err = ESP_FAIL;

    if (!nuwax_offline_audio_has_ready()) {
        ESP_LOGI(TAG, "offline replay test skipped; no ready archives");
        goto done;
    }
    if (CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_URL[0] == '\0' ||
        CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_TOKEN[0] == '\0') {
        ESP_LOGW(TAG, "offline replay test disabled at runtime; URL/token missing");
        goto done;
    }

    const nuwax_ws_config_t config = {
        .url = CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_URL,
        .token = CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_TOKEN,
        .on_text = replay_test_on_text,
        .on_binary = NULL,
        .on_rx_activity = NULL,
        .on_handshake_verified = NULL,
        .connect_cancelled = replay_test_connect_cancelled,
        .tcp_connect_timeout_ms = 3000U,
        .local_connect_diagnostics_only = true,
        .user_ctx = replay_ws,
    };
    for (uint32_t attempt = 1U; attempt <= REPLAY_TEST_MAX_ATTEMPTS;
         ++attempt) {
        if (!nuwax_offline_audio_has_ready()) {
            err = ESP_OK;
            break;
        }
        if (attempt > 1U) {
            for (uint32_t waited = 0; waited < REPLAY_TEST_RETRY_DELAY_MS &&
                 !replay_test_connect_cancelled(NULL); waited += 50U)
                vTaskDelay(pdMS_TO_TICKS(50U));
        }
        if (replay_test_connect_cancelled(NULL)) {
            err = ESP_ERR_NOT_FINISHED;
            break;
        }
        err = nuwax_ws_connect(replay_ws, &config);
        const bool connect_failed = err != ESP_OK;
        if (replay_test_connect_cancelled(NULL)) err = ESP_ERR_NOT_FINISHED;
        if (err == ESP_OK) {
            /* The receiver's hello ACK is deliberately ignored by this
             * isolated callback; begin/chunk/commit each carry their own ACK. */
            err = replay_test_send_hello(replay_ws);
            if (err == ESP_OK && replay_test_connect_cancelled(NULL)) err = ESP_ERR_NOT_FINISHED;
            if (err == ESP_OK) {
                const uint32_t epoch =
                    nuwax_ws_connection_epoch(replay_ws);
                err = replay_ready_impl(replay_ws, epoch, true);
                ESP_LOGI(TAG,
                         "offline replay test attempt=%lu finished err=%s epoch=%lu stack_min_free=%u",
                         (unsigned long)attempt,
                         esp_err_to_name(err),
                         (unsigned long)epoch,
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
            } else {
                ESP_LOGW(TAG,
                         "offline replay test hello failed attempt=%lu err=%s",
                         (unsigned long)attempt, esp_err_to_name(err));
            }
        } else {
            ESP_LOGW(TAG,
                     "offline replay test gateway connect failed attempt=%lu err=%s",
                     (unsigned long)attempt, esp_err_to_name(err));
        }

        /* owner_close requires an admitted close even when the peer is
         * healthy. This client is owned solely by this task, so force-close
         * cannot affect the primary cloud WebSocket. */
        if (nuwax_ws_is_connected(replay_ws)) {
            const uint32_t epoch = nuwax_ws_connection_epoch(replay_ws);
            (void)nuwax_ws_force_close_if_epoch(replay_ws, epoch);
        }
        esp_err_t close_err;
        uint32_t close_attempt = 0U;
        do {
            close_err = nuwax_ws_owner_close(replay_ws);
            if (close_err != ESP_OK) {
                ++close_attempt;
                ESP_LOGW(TAG,
                         "offline replay test transport close deferred attempt=%lu err=%s",
                         (unsigned long)close_attempt,
                         esp_err_to_name(close_err));
                vTaskDelay(pdMS_TO_TICKS(100U));
            }
        } while (close_err != ESP_OK &&
                 close_attempt < REPLAY_TEST_CLOSE_MAX_ATTEMPTS);
        if (close_err != ESP_OK) {
            atomic_store_explicit(&s_replay_test_transport_quarantined, true,
                                  memory_order_release);
            ESP_LOGE(TAG,
                     "offline replay test transport close still pending after %lu attempts; client quarantined until reboot",
                     (unsigned long)close_attempt);
            /* The RX task may still reference replay_ws. Never reconnect or
             * reinitialize this client after owner_close() failed to prove
             * quiescence; doing so would race the old transport generation. */
            break;
        }
        if (err == ESP_OK || err == ESP_ERR_NOT_FINISHED || connect_failed) {
            break;
        }
    }
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED && nuwax_offline_audio_has_ready()) {
        ESP_LOGW(TAG,
                 "offline replay test deferred; ready archives retained err=%s",
                 esp_err_to_name(err));
    }

    /* A recording can preempt this worker after it has connected but before
     * the first begin/chunk. Preserve that edge for the connection owner;
     * otherwise an already armed backoff can suppress the retry after the
     * writer becomes idle. */
    if (err == ESP_ERR_NOT_FINISHED) {
        atomic_store_explicit(&s_replay_retry_required, true,
                              memory_order_release);
    }

done:
    if (err == ESP_OK) {
        atomic_store_explicit(&s_test_retry_backoff_ms, 0U, memory_order_release);
        atomic_store_explicit(&s_test_retry_deadline_ms, 0U, memory_order_release);
    } else if (err != ESP_ERR_NOT_FINISHED) {
        uint32_t delay_ms = atomic_load_explicit(&s_test_retry_backoff_ms, memory_order_acquire);
        delay_ms = delay_ms == 0U ? 5000U : (delay_ms < 15000U ? delay_ms * 2U : 30000U);
        atomic_store_explicit(&s_test_retry_backoff_ms, delay_ms, memory_order_release);
        atomic_store_explicit(&s_test_retry_deadline_ms, offline_now_ms() + delay_ms, memory_order_release);
        ESP_LOGI(TAG, "offline replay gateway retry backoff_ms=%lu", (unsigned long)delay_ms);
    }
    ESP_LOGI(TAG,
             "offline replay test task exiting stack_min_free=%u stack_bytes=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)OFFLINE_REPLAY_TEST_TASK_STACK_BYTES);
    atomic_store_explicit(&s_replay_test_running, false, memory_order_release);
    vTaskDeleteWithCaps(NULL);
}
#endif

esp_err_t nuwax_offline_audio_replay_test_start(void)
{
#if !CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE
    return ESP_OK;
#else
    if (nuwax_power_ctrl_shutdown_preparing() || replay_ota_active()) return ESP_ERR_NOT_FINISHED;
    const uint32_t deadline = atomic_load_explicit(&s_test_retry_deadline_ms, memory_order_acquire);
    if (deadline && !offline_deadline_elapsed(offline_now_ms(), deadline)) return ESP_ERR_NOT_FINISHED;
    if (nuwax_offline_audio_storage_busy()) {
        atomic_store_explicit(&s_replay_retry_required, true,
                              memory_order_release);
        return ESP_ERR_NOT_FINISHED;
    }
    if (!enabled() || !runtime_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_load_explicit(&s.media_recovery_requested,
                         memory_order_acquire)) {
        atomic_store_explicit(&s_replay_retry_required, true,
                              memory_order_release);
        return ESP_ERR_NOT_FINISHED;
    }
    if (atomic_load_explicit(&s_replay_test_transport_quarantined,
                             memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!nuwax_offline_audio_has_ready()) {
        return ESP_OK;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &s_replay_test_running, &expected, true,
            memory_order_acq_rel, memory_order_acquire)) {
        return ESP_OK;
    }
    if (nuwax_power_ctrl_shutdown_preparing() || replay_ota_active() || atomic_load_explicit(&s.media_recovery_requested,
                             memory_order_acquire)) {
        atomic_store_explicit(&s_replay_test_running, false,
                              memory_order_release);
        return ESP_ERR_NOT_FINISHED;
    }
    if (xTaskCreatePinnedToCoreWithCaps(replay_test_task,
                            "offline_replay_test",
                            OFFLINE_REPLAY_TEST_TASK_STACK_BYTES,
                            NULL,
                            OFFLINE_IO_TASK_PRIORITY,
                            NULL,
                            0,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        atomic_store_explicit(&s_replay_test_running, false,
                              memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "offline replay test scheduled url=%s",
             CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_URL);
    return ESP_OK;
#endif
}

bool nuwax_offline_audio_replay_test_running(void)
{
#if !CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE
    return false;
#else
    return atomic_load_explicit(&s_replay_test_running, memory_order_acquire) ||
           atomic_load_explicit(&s_replay_test_transport_quarantined,
                                memory_order_acquire);
#endif
}

bool nuwax_offline_audio_replay_running(void)
{
    if (!enabled() || !runtime_initialized()) {
        return false;
    }
    return atomic_load_explicit(&s.replay_running, memory_order_acquire) ||
           nuwax_offline_audio_replay_formal_running() ||
           nuwax_offline_audio_replay_test_running();
}

/* A LAN connection attempt is cancellable; actual SD replay and a quarantined
 * transport still block OTA. Do not weaken replay_running used by other owners. */
bool nuwax_offline_audio_replay_blocks_ota(void) {
    if (!enabled() || !runtime_initialized()) return false;
    bool busy = atomic_load_explicit(&s.replay_running, memory_order_acquire) ||
                nuwax_offline_audio_replay_formal_running();
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE
    busy = busy || atomic_load_explicit(&s_replay_test_transport_quarantined, memory_order_acquire);
#endif
    return busy;
}

esp_err_t nuwax_offline_audio_replay_prepare_ota(uint32_t timeout_ms) {
    if (!replay_ota_active()) return ESP_ERR_INVALID_STATE;
    if (nuwax_offline_audio_replay_blocks_ota()) return ESP_ERR_INVALID_STATE;
    const uint32_t started = offline_now_ms();
    while (nuwax_offline_audio_replay_test_running()) {
        if (nuwax_offline_audio_replay_blocks_ota()) return ESP_ERR_INVALID_STATE;
        if ((uint32_t)(offline_now_ms() - started) >= timeout_ms) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(20U));
    }
    return ESP_OK;
}

bool nuwax_offline_audio_replay_retry_required(void)
{
    return atomic_exchange_explicit(&s_replay_retry_required, false,
                                    memory_order_acq_rel);
}

bool nuwax_offline_audio_replay_retry_pending(void)
{
    return atomic_load_explicit(&s_replay_retry_required,
                                memory_order_acquire);
}

bool nuwax_offline_audio_handle_protocol_text(const char *payload, int len,
                                              uint32_t connection_epoch)
{
    if (!enabled() || !payload || len <= 0 || !runtime_initialized() ||
        connection_epoch == 0U) {
        return false;
    }
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) {
        return false;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *request = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    const bool is_ack = cJSON_IsString(type) && cJSON_IsString(request) &&
                        (strcmp(type->valuestring, "offline_audio_begin_ack") == 0 ||
                         strcmp(type->valuestring, "offline_audio_chunk_ack") == 0 ||
                         strcmp(type->valuestring, "offline_audio_commit_ack") == 0);
    if (!is_ack) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *next = cJSON_GetObjectItemCaseSensitive(root, "next_offset");
    const cJSON *segment = cJSON_GetObjectItemCaseSensitive(root, "segment_id");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "replay_version");
    const cJSON *window = cJSON_GetObjectItemCaseSensitive(root, "window_bytes");
    const bool requires_offset =
        strcmp(type->valuestring, "offline_audio_commit_ack") != 0;
    const double offset_value = cJSON_IsNumber(next) ? next->valuedouble : -1.0;
    const bool valid_offset =
        !requires_offset ||
        (offset_value >= 0.0 && offset_value <= (double)UINT32_MAX &&
         (double)(uint32_t)offset_value == offset_value);
    bool matched_request = false;
    bool accepted = false;
    EventBits_t signal_bit = 0U;
    portENTER_CRITICAL(&s_replay_lock);
    if (s.replay_ack.waiting &&
        strcmp(s.replay_ack.request_id, request->valuestring) == 0) {
        matched_request = true;
        const bool epoch_matches = s.replay_ack.epoch == connection_epoch;
        const bool type_matches =
            strcmp(s.replay_ack.expected_type, type->valuestring) == 0;
        const bool segment_matches =
            !cJSON_IsString(segment) ||
            strcmp(s.replay_ack.segment_id, segment->valuestring) == 0;
        if (epoch_matches && type_matches && segment_matches && valid_offset) {
            s.replay_ack.ok = cJSON_IsTrue(ok);
            if (strcmp(type->valuestring, "offline_audio_begin_ack") == 0 &&
                cJSON_IsNumber(version) && version->valuedouble == 2.0 &&
                cJSON_IsNumber(window) && window->valuedouble >= REPLAY_CHUNK_MAX_PAYLOAD_BYTES &&
                window->valuedouble <= REPLAY_WINDOW_MAX_BYTES &&
                window->valuedouble == (double)(uint32_t)window->valuedouble) {
                s.replay_ack.window_bytes = (uint32_t)window->valuedouble;
            }
            s.replay_ack.next_offset = requires_offset
                                           ? (uint32_t)offset_value
                                           : 0U;
            signal_bit = s.replay_ack.chunk ? REPLAY_CHUNK_ACK_BIT
                                            : REPLAY_ACK_BIT;
            accepted = true;
        }
    }
    portEXIT_CRITICAL(&s_replay_lock);
    if (signal_bit != 0U) {
        (void)xEventGroupSetBits(s.events, signal_bit);
    }
    if (matched_request && !accepted) {
        ESP_LOGW(TAG,
                 "offline replay ACK rejected request=%s type=%s epoch=%lu offset_valid=%d segment=%s",
                 request->valuestring,
                 type->valuestring,
                 (unsigned long)connection_epoch,
                 valid_offset ? 1 : 0,
                 cJSON_IsString(segment) ? segment->valuestring : "-");
    }
    cJSON_Delete(root);
    return true;
}
