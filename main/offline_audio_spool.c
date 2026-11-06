#include "offline_audio_spool.h"
#include "offline_audio_policy.h"
#include "sd_audio_session.h"
#include "sd_audio_file_fault.h"
#include "sd_capture_admission.h"
#include "sd_audio_protocol.h"
#include "sd_audio_scene_protocol.h"
#include "sd_audio_file_time.h"
#include "sd_audio_retention_policy.h"
#include "offline_replay_policy.h"
#include "sd_socket_wait.h"
#include "sd_tcp_recovery.h"
#include "sd_gateway_discovery.h"
#if defined(ESP_PLATFORM) && CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE
#include "network_tx_probe.h"
#endif
#if CONFIG_NUWAX_SD_SESSION_ENABLE
#include "sd_audio_catalog.h"
#include "nuwax_provision_new/nuwax_cloud_config.h"
#include "psa/crypto.h"
#endif
#include "nuwax_ota_new/nuwax_ota_device.h"
#include "offline_io_schedule.h"
#include "offline_audio_voice.h"
#include "audio_modes/monitoring/monitoring_speech_detector.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdatomic.h>

#include "audio_runtime_memory_policy.h"
#include "board/sd_card_driver.h"
#include "board/sd_dma_bulk.h"
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
#include "json_resource_budget.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "audio_frame.h"
#include "mic_input.h"
#include "nuwax_provision_new/nuwax_device_uid.h"
#if CONFIG_NUWAX_SD_WRITE_VERIFY_ENABLE
#include "board/sd_write_verify.h"
#endif

#ifndef CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
#define CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS 0
#endif
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
#include "sd_audio_integrity.h"
#endif

#ifndef CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE
#define CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_SD_SESSION_ENABLE
#define CONFIG_NUWAX_SD_SESSION_ENABLE 0
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
#ifndef CONFIG_NUWAX_SD_MEETING_MAX_HOURS
#define CONFIG_NUWAX_SD_MEETING_MAX_HOURS 8
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
#define REPLAY_STORAGE_PROGRESS_BIT BIT4
#define REPLAY_TEST_MAX_ATTEMPTS 1U
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
/* 3896 PCM/archive bytes + 64 protocol + 8 masked WS header fit the existing
 * 3968-byte transport scratch. Do not split every low-memory frame into two
 * TLS writes merely because the protocol header was omitted from the budget. */
#define REPLAY_FORMAL_CHUNK_PAYLOAD_BYTES 3896U
#define REPLAY_TEST_CLOSE_MAX_ATTEMPTS 30U
#define OFFLINE_WRITER_TASK_STACK_BYTES 8192U
#define OFFLINE_CAPTURE_TASK_STACK_BYTES 8192U
/* Keep the microphone reader ahead of reconnect work on core 0. The
 * exclusive audio owner makes core 1 available during offline capture. */
#define OFFLINE_CAPTURE_TASK_PRIORITY 12U
#define OFFLINE_CAPTURE_TASK_CORE 1
#define OFFLINE_REPLAY_TEST_TASK_STACK_BYTES 16384U
#define OFFLINE_REPLAY_FORMAL_TASK_STACK_BYTES 16384U
#define OFFLINE_REPLAY_FORMAL_TASK_PRIORITY OFFLINE_IO_TASK_PRIORITY
#define WRITER_DEQUEUE_WAIT_MS 10U
#define WRITER_RETRY_INITIAL_MS 250U
#define WRITER_RETRY_MAX_MS 30000U
#define MEDIA_RECOVERY_RETRY_INITIAL_MS 1000U
#define MEDIA_RECOVERY_RETRY_MAX_MS 30000U
#define MEDIA_UNAVAILABLE_RETRY_INITIAL_MS 5000U
#define MEDIA_UNAVAILABLE_RETRY_MAX_MS 120000U
/* Recovery walks two directory levels and reconciles owner-bound metadata.
 * Leave stack headroom for nested FatFs/VFS calls; this stack uses PSRAM. */
#define OFFLINE_STORAGE_SUPERVISOR_STACK_BYTES 8192U
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
    bool realtime;
    uint32_t generation;
    size_t bytes;
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
    uint32_t queued_crc;
    bool queued_crc_valid;
#endif
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
    uint64_t file_started_at_ms;
    uint32_t file_duration_ms;
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
_Static_assert(__builtin_offsetof(archive_header_t, file_started_at_ms) == 84U &&
               __builtin_offsetof(archive_header_t, file_duration_ms) == 92U,
               "offline archive time offsets changed");
_Static_assert(sizeof(archive_footer_t) == FOOTER_BYTES,
               "offline archive footer size changed");

typedef struct {
    atomic_bool active;
    atomic_bool closing;
    atomic_bool degraded;
    atomic_bool writer_error;
    atomic_bool writer_busy;
    atomic_bool archive_ready;
    atomic_bool media_recovery_requested;
    bool stop_requested;
    bool stop_enqueued;
    long last_good_offset;
    atomic_uint_fast32_t dropped_frames;
    /* Legacy archive count. Card-session readiness belongs to its catalog;
     * retained damaged/foreign-owner audio must not trigger empty reconnects. */
    atomic_uint_fast32_t ready_count;
    uint32_t segment_index;
    uint32_t frame_count;
    uint64_t payload_bytes;
    uint32_t payload_crc32;
    uint64_t file_started_at_ms;
    uint64_t used_bytes;
    uint32_t next_index;
    uint32_t writer_retry_deadline_ms;
    uint32_t writer_retry_backoff_ms;
    bool writer_fault_latched;
    bool writer_frame_written;
    bool writer_frame_uncertain;
    long verify_offset;
    uint32_t verify_bytes, verify_crc32;
    uint32_t verify_calls;
    uint64_t verify_us;
    uint32_t media_recovery_deadline_ms;
    uint32_t media_recovery_backoff_ms;
    bool media_was_ready;
    esp_err_t media_recovery_last_error;
    FILE *file;
    char *write_buffer;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    sd_audio_seal_head_t *seal_head;
#endif
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
    sd_integrity_t *integrity;
#endif
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
        uint32_t received_ms;
        char request_id[48];
        char segment_id[48];
        char expected_type[40];
        uint32_t window_bytes;
        char mode[16];
        sd_scene_ack_t scene;
    } replay_ack;
} spool_state_t;

static spool_state_t s;
/* Process/runtime lifetime signal; never retain a retiring reader handle. */
static atomic_bool s_storage_wait_armed;
static void replay_storage_signal(void)
{
#ifdef ESP_PLATFORM
    if (atomic_load_explicit(&s_storage_wait_armed, memory_order_acquire) && s.events)
        (void)xEventGroupSetBits(s.events, REPLAY_STORAGE_PROGRESS_BIT);
#endif
}
static void writer_storage_idle(void)
{
    atomic_store_explicit(&s.writer_busy, false, memory_order_release);
    replay_storage_signal();
}
static StaticSemaphore_t s_writer_gate_storage;
static StaticSemaphore_t s_init_lock_storage;
static SemaphoreHandle_t s_init_lock;
static portMUX_TYPE s_init_lock_mux = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool s_runtime_initialized = ATOMIC_VAR_INIT(false);
/* Published by the storage owner. Frame paths never take the SD driver lock. */
static atomic_bool s_media_ready;
/* A replay reader only publishes the fault. The writer/supervisor owns FILE
 * closure and remount; never unmount from a reader or WebSocket callback. */
static atomic_bool s_replay_media_fault;
/* Advances only at completed media reset boundaries, never on a healthy probe. */
static atomic_uint s_media_generation;
static atomic_bool s_space_blocked;
static atomic_int s_capture_health;
static atomic_uint s_capture_recovery_until;
static atomic_uint s_capture_cancel_epoch;
static atomic_bool s_close_complete;
static atomic_bool s_supervisor_started = ATOMIC_VAR_INIT(false);
/* Directory metadata is authoritative after mount/recovery.  Keep the
 * periodic supervisor probe cheap while the medium is healthy, and rescan
 * only after a directory mutation or a remount boundary. */
static atomic_bool s_directory_dirty = ATOMIC_VAR_INIT(true);
/* Recheck legacy .ready.bad files once per completed replay pass after boot.
 * This also admits recovery when every pending archive was misclassified. */
static atomic_bool s_quarantine_recheck_pending = ATOMIC_VAR_INIT(true);
/* A boot-time directory check is not upload work. Only a successful local
 * inventory may publish a quarantined file for replay. */
static atomic_bool s_quarantine_files_present = ATOMIC_VAR_INIT(false);
static TaskHandle_t s_supervisor_task;
/* Replay ACK state is touched by the WebSocket RX callback and the replay
 * worker. Keep it independent from the writer/file lock: an SD fsync must
 * never stall protocol RX for hundreds of milliseconds. */
static portMUX_TYPE s_replay_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool s_offline_limit_reached;
/* Written only by the capture launch owner; copied into each reader config. */
static char s_limit_session[48];
static uint64_t s_limit_used_us;
static atomic_uint s_mirror_closers;
static unsigned s_replay_route;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
static atomic_bool s_card_active, s_card_created, s_card_finishing, s_card_failed;
static atomic_bool s_card_partial;
static sd_audio_damage_window_t s_card_damage;
static atomic_bool s_offline_verify_running, s_offline_verify_cancel;
static uint64_t s_offline_verify_nonce;
static void card_offline_verify_service(void);
static sd_audio_file_fault_t s_card_file_fault[2];
static struct {
    atomic_bool pending;
    uint32_t started_ms, deadline_ms, next_ms, attempts, capture_generation;
    bool identity_valid;
    nuwax_sd_card_identity_t identity;
    bool retry_probe, skip_retry;
    uint32_t retry_index, retry_sequence, retry_crc, retry_generation, retry_bytes;
    bool prefix_probe;
    uint32_t prefix_index, prefix_frames, prefix_crc;
    uint64_t prefix_payload_bytes;
} s_card_io_recovery;
static atomic_bool s_card_online, s_card_pending_upload;
static atomic_bool s_monitor_space_blocked;
/* ESP32-S3 does not provide lock-free 64-bit atomics. This mux protects the
 * online-time and capture-owner snapshots; no I/O or waits while held. */
static portMUX_TYPE s_card_network_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_card_online_since;
static char s_card_mirror_session[48];
static atomic_int s_card_mode;
static uint32_t s_card_boot_id;
static sd_audio_session_t s_card_session, s_card_replay_session;
static sd_audio_segment_t s_card_segment, s_card_replay_segment;
static uint64_t s_card_frame_start;
static bool s_card_frame_realtime;
static bool s_card_preempted;
static bool card_live_pending(nuwax_ws_client_t *ws, uint32_t epoch);
static void card_finalize(void);
static esp_err_t card_monitor_make_room_locked(bool ahead, uint32_t budget_ms);
static esp_err_t card_monitor_prepare_space(void);
#endif

static bool recording_space_blocked(void)
{
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (nuwax_sd_session_monitoring()) return atomic_load(&s_monitor_space_blocked);
#endif
    return atomic_load(&s_space_blocked);
}

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

static void replay_checkpoint_invalidate(void);

static void schedule_media_recovery_locked(esp_err_t err)
{
    replay_checkpoint_invalidate();
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

static void reset_media_recovery_locked(bool contents_may_have_changed)
{
    /* Routine healthy probes run between network retries. They do not change
     * sealed files; invalidating here forced every resume to re-read its
     * entire prefix. Recovery scans/remounts must still invalidate. */
    if (contents_may_have_changed) {
        replay_checkpoint_invalidate();
        atomic_fetch_add_explicit(&s_media_generation, 1U, memory_order_acq_rel);
    }
    atomic_store_explicit(&s_replay_media_fault, false, memory_order_release);
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

static atomic_bool s_formal_replay_cancel_requested = ATOMIC_VAR_INIT(false);
static atomic_bool s_shutdown_requested = ATOMIC_VAR_INIT(false);
#if CONFIG_NUWAX_SD_SESSION_ENABLE
static sd_capture_admission_t s_capture_admission;
#endif

#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
typedef struct {
    nuwax_ws_client_t *ws;
    uint32_t epoch;
    unsigned core;
} formal_replay_context_t;

static atomic_bool s_formal_replay_running = ATOMIC_VAR_INIT(false);
static formal_replay_context_t s_formal_replay_context;
static atomic_uintptr_t s_formal_worker_identity;
static unsigned replay_formal_target_core(void)
{
    return offline_replay_formal_core(atomic_load_explicit(&s.capture_running, memory_order_acquire));
}
#ifdef ESP_PLATFORM
static bool replay_formal_core_handoff(void)
{
    return atomic_load_explicit(&s_formal_worker_identity, memory_order_acquire) ==
        (uintptr_t)xTaskGetCurrentTaskHandle() && replay_formal_target_core() != (unsigned)xPortGetCoreID();
}
#endif
/* Called by the connection owner (including its exclusive-mode poll hook). */
static uint32_t s_replay_launch_epoch;
static uint32_t s_replay_launch_revision;
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
#if CONFIG_NUWAX_SD_COMMAND_HISTORY_ENABLE
#include "board/sd_command_history.h"
#endif

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

/* Called only under the storage owner's FILE lock (or unpublished init).
 * FatFs free-cluster discovery was already warmed before recording starts. */
static esp_err_t refresh_capture_capacity_locked(void)
{
    uint64_t total = 0U, free_bytes = 0U;
    const esp_err_t err = esp_vfs_fat_info("/sdcard", &total, &free_bytes);
    const bool blocked = err != ESP_OK ||
        free_bytes < NUWAX_OFFLINE_AUDIO_FREE_RESERVE_BYTES +
                         CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES ||
        nuwax_offline_quota_blocked(CONFIG_NUWAX_SD_SESSION_ENABLE, s.used_bytes,
            ARCHIVE_QUOTA_BYTES, CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES);
    const bool was_blocked = atomic_exchange_explicit(
        &s_space_blocked, blocked, memory_order_acq_rel);
    if (blocked != was_blocked) {
        ESP_LOGW(TAG, "offline recording capacity %s free=%llu used=%llu quota=%llu",
                 blocked ? "unavailable" : "restored",
                 (unsigned long long)free_bytes, (unsigned long long)s.used_bytes,
                 (unsigned long long)ARCHIVE_QUOTA_BYTES);
    }
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

/* A failed/partial directory walk must never publish a smaller filename
 * counter. Logical catalog indices remain independent of this high-water mark. */
static esp_err_t refresh_next_segment_index(void)
{
    archive_dir_t walk;
    archive_dir_t *dir = archive_dir_open(&walk);
    if (!dir) {
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        return ESP_FAIL;
    }
    uint32_t next = s.next_index;
    esp_err_t err = ESP_OK;
    struct dirent *entry;
    while ((entry = archive_dir_read(dir)) != NULL) {
        const char *name = entry->d_name;
        const char *underscore = strrchr(name, '_');
        if (!underscore || underscore[1] < '0' || underscore[1] > '9') {
            continue;
        }
        char *end = NULL;
        errno = 0;
        const unsigned long index = strtoul(underscore + 1, &end, 10);
        const bool overflow = errno == ERANGE || index >= UINT32_MAX;
        const bool known_suffix = end &&
            (strcmp(end, ".part") == 0 ||
             strcmp(end, ".ready") == 0 ||
             strcmp(end, ".part.bad") == 0 ||
             strcmp(end, ".ready.bad") == 0);
        if (!known_suffix) {
            continue;
        }
        if (overflow) { err = ESP_ERR_INVALID_SIZE; break; }
        if ((uint32_t)index >= next) {
            next = (uint32_t)index + 1U;
        }
    }
    if (dir->error) err = ESP_FAIL;
    archive_dir_close(dir);
    if (err == ESP_OK) s.next_index = next;
    else atomic_store_explicit(&s.degraded, true, memory_order_release);
    return err;
}

#if CONFIG_NUWAX_SD_BATCH_REPAIR_ENABLE
static esp_err_t writer_repair_quarantine_known(const char *path, bool *handled);
#endif

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
#if CONFIG_NUWAX_SD_BATCH_REPAIR_ENABLE
        /* A confirmed CRC failure has stronger evidence than an ordinary
         * interrupted write. Never synthesize a new checksum for its bytes. */
        bool handled = false;
        const esp_err_t quarantine = writer_repair_quarantine_known(path, &handled);
        if (quarantine != ESP_OK) { archive_dir_close(dir); return quarantine; }
        if (handled) continue;
#endif
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
        sd_audio_file_span_t file_span = {0};
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
            if (header.reserved0 == 2U) {
                uint32_t timestamp_ms;
                memcpy(&timestamp_ms, payload + 14U, sizeof(timestamp_ms));
                sd_audio_file_span_add(&file_span, timestamp_ms,
                    header.start_uptime_ms, header.frame_duration_ms);
            }
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
            if (header.reserved0 == 2U) {
                uint64_t started_at_ms = nuwax_cloud_time_valid(header.file_started_at_ms)
                    ? header.file_started_at_ms : 0;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
                /* Prefer the original boot's durable scene clock. Never
                 * infer an old file's UTC from the current boot's clock. */
                sd_audio_session_t session;
                sd_audio_session_time_t time = {0};
                char id[sizeof(header.session_id)];
                memcpy(id, header.session_id, sizeof(id)); id[sizeof(id) - 1] = '\0';
                if (sd_audio_catalog_load(id, &session) == ESP_OK &&
                    sd_audio_catalog_session_time(&session, 0, NULL, false, 0, &time) == ESP_OK) {
                    const uint64_t recovered = sd_audio_file_started_at(&time,
                        session.start_ms, header.start_uptime_ms);
                    if (recovered) started_at_ms = recovered;
                }
#endif
                valid = sd_audio_file_time_write(file, started_at_ms,
                    file_span.seen && !file_span.invalid ? (uint32_t)file_span.duration_ms : 0);
            }
            valid = valid && fwrite(&footer, 1U, sizeof(footer), file) == sizeof(footer) &&
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
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        nuwax_sd_card_mark_io_fault(ESP_FAIL);
        ESP_LOGW(TAG, "offline inventory open failed; previous pending state retained");
        return atomic_load_explicit(&s.ready_count, memory_order_acquire);
    }
    uint32_t count = 0U;
    bool quarantined = false;
    struct dirent *entry;
    while ((entry = archive_dir_read(dir)) != NULL) {
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        if (strstr(entry->d_name, "/card_") || !strncmp(entry->d_name, "card_", 5)) continue;
#endif
        const char *dot = strrchr(entry->d_name, '.');
        if (dot && strcmp(dot, ".ready") == 0 && count < UINT32_MAX) {
            ++count;
        }
        const size_t length = strlen(entry->d_name);
        if (length >= 10U && strcmp(entry->d_name + length - 10U, ".ready.bad") == 0) {
            quarantined = true;
        }
    }
    if (dir->error) {
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        nuwax_sd_card_mark_io_fault(ESP_FAIL);
        ESP_LOGW(TAG, "offline inventory read failed; previous pending state retained");
        count = atomic_load_explicit(&s.ready_count, memory_order_acquire);
    } else {
        atomic_store_explicit(&s_quarantine_files_present, quarantined, memory_order_release);
    }
    archive_dir_close(dir);
    return count;
}

static void log_sd_step(const char *step, int64_t started_us)
{
    const int64_t elapsed = esp_timer_get_time() - started_us;
    if (elapsed >= 200000LL) ESP_LOGW(TAG, "SD latency step=%s elapsed_ms=%lu",
                                    step, (unsigned long)(elapsed / 1000));
}

/* Storage-lock owner only. Save errno before any rollback/close changes it. */
static const char *s_writer_fault_stage;
static int s_writer_fault_errno;
static void writer_note_fault(const char *stage, int saved_errno)
{
    if (!s_writer_fault_stage) { s_writer_fault_stage = stage; s_writer_fault_errno = saved_errno; }
}

static bool sync_archive_file(FILE *file, const char *step)
{
    const int64_t started = esp_timer_get_time();
    const bool flushed = fflush(file) == 0;
    const bool ok = flushed && fsync(fileno(file)) == 0;
    if (!ok) writer_note_fault(step, errno);
    log_sd_step(step, started);
    return ok;
}

#include "sd_audio_session_storage.inc"
#include "sd_audio_integrity_writer.inc"
#include "sd_audio_batch_repair.inc"
#include "sd_monitor_storage.inc"

static FILE *archive_create_exclusive(const char *path)
{
    int flags = O_CREAT | O_EXCL | O_RDWR;
#ifdef O_BINARY
    flags |= O_BINARY;
#endif
    const int fd = open(path, flags, 0600);
    if (fd < 0) return NULL;
    FILE *file = fdopen(fd, "w+b");
    if (!file) {
        const int saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
    }
    return file;
}

static esp_err_t open_segment_locked(void)
{
    if (!atomic_load_explicit(&s.active, memory_order_acquire) || s.file) {
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (card_recording_locked() && !s.seal_head) {
        s.seal_head = heap_caps_calloc(1U, sizeof(*s.seal_head), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s.seal_head) return ESP_ERR_NO_MEM;
    }
#endif
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
    bool circular = false;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    circular = card_recording_locked() && s.mode == NUWAX_OFFLINE_AUDIO_MODE_MONITORING;
    if (circular) {
        const esp_err_t room = card_monitor_make_room_locked(false, 100U);
        if (room != ESP_OK) return room;
    }
#endif
    uint64_t total_bytes = 0U;
    uint64_t free_bytes = 0U;
    const int64_t space_started = esp_timer_get_time();
    const esp_err_t info_err =
        esp_vfs_fat_info("/sdcard", &total_bytes, &free_bytes);
    log_sd_step("free_space", space_started);
    if (info_err != ESP_OK) {
        writer_note_fault("free_space", errno);
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        ESP_LOGE(TAG,
                 "offline archive free-space query failed err=%s total=%llu free=%llu",
                 esp_err_to_name(info_err),
                 (unsigned long long)total_bytes,
                 (unsigned long long)free_bytes);
        return info_err;
    }
    const uint64_t minimum_free_bytes = circular
        ? sd_monitor_required_bytes(total_bytes, CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES, false)
        : NUWAX_OFFLINE_AUDIO_FREE_RESERVE_BYTES + CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES;
    if (free_bytes < minimum_free_bytes ||
        (!circular && nuwax_offline_quota_blocked(CONFIG_NUWAX_SD_SESSION_ENABLE,
            s.used_bytes, ARCHIVE_QUOTA_BYTES, (uint64_t)HEADER_BYTES + FOOTER_BYTES))) {
        atomic_store_explicit(&s_space_blocked, true, memory_order_release);
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
    if (s.next_index == UINT32_MAX) {
        writer_note_fault("filename_exhausted", 0); return ESP_ERR_INVALID_SIZE;
    }
    s.segment_index = s.next_index++;
    const int64_t bucket_started = esp_timer_get_time();
    if (!archive_new_paths(s.segment_index, s.session_id, s.open_path, sizeof(s.open_path),
                           s.ready_path, sizeof(s.ready_path))) {
        writer_note_fault("bucket_prepare", errno); return ESP_FAIL;
    }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (card_recording_locked()) {
        char *open_base = strrchr(s.open_path, '/');
        char *ready_base = strrchr(s.ready_path, '/');
        if (!open_base || !ready_base) return ESP_ERR_INVALID_STATE;
        memmove(open_base + 6, open_base + 9, strlen(open_base + 9) + 1);
        memcpy(open_base + 1, "card_", 5);
        memmove(ready_base + 6, ready_base + 9, strlen(ready_base + 9) + 1);
        memcpy(ready_base + 1, "card_", 5);
        errno = 0;
        const esp_err_t reserved = card_reserve_segment(s_card_frame_start, s_card_frame_realtime);
        if (reserved != ESP_OK) { writer_note_fault("index_reserve", reserved == ESP_FAIL ? errno : 0); return reserved; }
    }
#endif
    log_sd_step("bucket_prepare", bucket_started);
    const int64_t lookup_started = esp_timer_get_time();
    struct stat path_stat;
    const bool exists_open = stat(s.open_path, &path_stat) == 0;
    if (!exists_open && errno != ENOENT) { writer_note_fault("part_lookup", errno); return ESP_FAIL; }
    const bool exists_ready = stat(s.ready_path, &path_stat) == 0;
    if (!exists_ready && errno != ENOENT) { writer_note_fault("ready_lookup", errno); return ESP_FAIL; }
    const bool exists = exists_open || exists_ready;
    log_sd_step("path_lookup", lookup_started);
    if (exists) {
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t open_started = esp_timer_get_time();
    s.file = archive_create_exclusive(s.open_path);
    log_sd_step("file_open", open_started);
    if (!s.file) {
        writer_note_fault("file_open", errno);
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (card_recording_locked()) {
        header.reserved0 = 2U;
        header.segment_index = s_card_segment.index;
        header.start_uptime_ms = s_card_segment.start_ms;
        header.file_started_at_ms = card_file_started_at(s_card_segment.start_ms);
    }
#endif
    snprintf(header.session_id, sizeof(header.session_id), "%s", s.session_id);
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (card_recording_locked() && !sd_audio_seal_head_begin(s.seal_head, &header, sizeof(header),
            s.capture_generation, s.segment_index, crc32_update)) {
        (void)fclose(s.file); s.file = NULL; return ESP_ERR_INVALID_STATE;
    }
#endif
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
    if (card_recording_locked() && sd_integrity_begin(s.integrity, s.session_id, s.open_path,
            s.capture_generation, s.segment_index, &header, sizeof(header)))
        nuwax_sd_integrity_io_arm();
#endif
    const bool header_written = fwrite(&header, 1U, sizeof(header), s.file) == sizeof(header);
    if (!header_written) writer_note_fault("header_write", errno);
    if (!header_written ||
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
    s.file_started_at_ms = header.file_started_at_ms;
    s.frame_count = 0U;
    s.payload_bytes = 0U;
    s.payload_crc32 = 0U;
    s.last_good_offset = (long)sizeof(header);
    s.verify_offset = (long)sizeof(header);
    s.verify_bytes = s.verify_crc32 = 0U;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (card_recording_locked()) {
        /* The first durable frame verifies its scene/format header too. */
        s.verify_offset = 0;
        s.verify_bytes = sizeof(header);
        s.verify_crc32 = crc32_update(0U, &header, sizeof(header));
    }
#endif
    return ESP_OK;
}

/* The writer has already been synced. Its clean FatFs sector cache can still
 * hide a card that acknowledged a write without persisting it. Read through a
 * fresh, read-only handle; never change the writer's cursor or share its buffer.
 * The read handle closes before releasing the storage lock; the writer
 * remains owned by the writer task and must close before any remount. */
static esp_err_t writer_verify_records_locked(void)
{
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (!card_recording_locked() || !s.verify_bytes) return ESP_OK;
    const int64_t started = esp_timer_get_time();
    const long end = s.last_good_offset;
    if (!s.file || end < s.verify_offset ||
        (uint64_t)(end - s.verify_offset) != s.verify_bytes || ftell(s.file) != end) {
        writer_note_fault("readback_seek", errno); return ESP_FAIL;
    }
    uint8_t block[512];
    uint32_t crc = 0U, remaining = s.verify_bytes;
    esp_err_t err = ESP_OK;
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        const bool independent = attempt != 0U;
        if (independent && !nuwax_sd_bulk_read_recovery_begin(1500U)) break;
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
        if (writer_integrity_live()) {
            sd_integrity_read_attempt(s.integrity);
            nuwax_sd_integrity_io_read_phase(true);
        }
#endif
        FILE *readback = fopen(s.open_path, "rb");
        if (!readback) {
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
            nuwax_sd_integrity_io_read_phase(false);
#endif
            if (independent) nuwax_sd_bulk_read_recovery_end(false);
            writer_note_fault("readback_open", errno); return ESP_FAIL;
        }
        crc = 0U; remaining = s.verify_bytes; err = ESP_OK;
        if (setvbuf(readback, s.write_buffer + ARCHIVE_STDIO_BUFFER_BYTES,
                _IOFBF, ARCHIVE_STDIO_BUFFER_BYTES) != 0 ||
            fseek(readback, s.verify_offset, SEEK_SET) != 0) err = ESP_FAIL;
        while (remaining && err == ESP_OK) {
            const size_t n = remaining < sizeof(block) ? remaining : sizeof(block);
            if (fread(block, 1U, n, readback) != n) { err = ESP_FAIL; break; }
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
            if (writer_integrity_live()) sd_integrity_compare(s.integrity, block, n, s.verify_bytes - remaining);
#endif
            crc = crc32_update(crc, block, n); remaining -= n;
        }
        if (err == ESP_OK && crc != s.verify_crc32) err = ESP_ERR_INVALID_CRC;
        if (ferror(readback)) err = ESP_FAIL;
        if (fclose(readback) != 0) err = ESP_FAIL;
        if (independent) {
            nuwax_sd_bulk_read_recovery_end(err == ESP_OK);
            if (err == ESP_OK) ESP_LOGW(TAG,
                "SD independent read recovered revision=217 stage=record index=%lu bytes=%lu; original CRC matched, single-sector reads retained",
                (unsigned long)s.segment_index, (unsigned long)s.verify_bytes);
        }
        ++s.verify_calls;
        if (err != ESP_ERR_INVALID_CRC) break; /* One independent retry on a mismatch. */
    }
    s.verify_us += (uint64_t)(esp_timer_get_time() - started);
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
    nuwax_sd_integrity_io_read_phase(false);
#if CONFIG_NUWAX_SD_BATCH_REPAIR_ENABLE
    if (err == ESP_ERR_INVALID_CRC && writer_repair_batch(crc) == ESP_OK) err = ESP_OK;
#endif
    if (err == ESP_ERR_INVALID_CRC) writer_integrity_fault(SD_INTEGRITY_RECORD, s.verify_crc32, crc);
#endif
    if (err != ESP_OK) {
        writer_note_fault(err == ESP_ERR_INVALID_CRC ? "record_readback_crc" : "record_readback", errno);
        ESP_LOGE(TAG, "SD durable record readback failed session=%s index=%lu offset=%ld bytes=%lu expected_crc=%08lx actual_crc=%08lx remaining=%lu err=%s",
            s.session_id, (unsigned long)s.segment_index, s.verify_offset,
            (unsigned long)s.verify_bytes, (unsigned long)s.verify_crc32,
            (unsigned long)crc, (unsigned long)remaining, esp_err_to_name(err));
        return err;
    }
    s.verify_offset = end; s.verify_bytes = s.verify_crc32 = 0U;
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
    if (writer_integrity_live()) {
        sd_integrity_verified(s.integrity, (uint64_t)end);
        /* Keep the latest body-write witnesses through footer/header updates.
         * New files arm a new generation; bounded tables evict older sectors. */
    }
#endif
#endif
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
        (!sync_archive_file(s.file, "tail_repair_sync") ||
         ftruncate(fileno(s.file), s.last_good_offset) != 0 ||
         fseek(s.file, s.last_good_offset, SEEK_SET) != 0)) {
        writer_note_fault("tail_repair", errno);
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        (void)fclose(s.file);
        s.file = NULL;
        return ESP_FAIL;
    }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (card_recording_locked() && s.verify_bytes &&
        (!sync_archive_file(s.file, "tail_sync") || writer_verify_records_locked() != ESP_OK)) {
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        (void)fclose(s.file); s.file = NULL;
        return ESP_FAIL; /* Keep .part and its original bytes for recovery. */
    }
#endif
    archive_footer_t footer = {0};
    memcpy(footer.magic, "NWXEND1", 7U);
    footer.version = 1U;
    footer.footer_bytes = FOOTER_BYTES;
    footer.frame_count = s.frame_count;
    footer.payload_bytes = s.payload_bytes;
    footer.crc32 = s.payload_crc32;
    footer.complete = complete ? 1U : 0U;
    bool time_ok = true;
    bool footer_written = false;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (card_recording_locked()) {
        if (!s.file_started_at_ms) s.file_started_at_ms = card_file_started_at(s_card_segment.start_ms);
        const uint32_t duration = sd_audio_file_duration(s_card_segment.start_ms, s_card_segment.end_ms);
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
        if (writer_integrity_live()) sd_integrity_prepare_seal(s.integrity, s.file_started_at_ms,
            duration, &footer, sizeof(footer));
#endif
        /* This scratch belongs to the closed readback operation, not to the
         * writer's stdio cache. A CRC-protected original prefix avoids a
         * 12-byte read/modify/write of a potentially stale first-sector cache. */
        uint8_t *prefix = (uint8_t *)s.write_buffer + ARCHIVE_STDIO_BUFFER_BYTES;
        const size_t prefix_bytes = sd_audio_seal_head_prepare(s.seal_head,
            s.capture_generation, s.segment_index, s.file_started_at_ms, duration,
            prefix, 512U, crc32_update);
        if (!prefix_bytes) { s_writer_fault_stage = "seal_prefix_ram"; s_writer_fault_errno = 0; }
        time_ok = prefix_bytes && sd_audio_file_head_write(s.file, prefix, prefix_bytes);
        uint64_t tail_offset = 0U;
        const size_t tail_bytes = time_ok ? sd_audio_seal_tail_prepare(s.seal_head,
            s.capture_generation, s.segment_index, (uint64_t)s.last_good_offset,
            s.file_started_at_ms, duration, &footer, sizeof(footer), prefix,
            ARCHIVE_STDIO_BUFFER_BYTES, &tail_offset, crc32_update) : 0U;
        if (time_ok && !tail_bytes) { s_writer_fault_stage = "seal_tail_ram"; s_writer_fault_errno = 0; }
        footer_written = time_ok && tail_bytes && sd_audio_file_tail_write(s.file,
            (uint64_t)s.last_good_offset, tail_offset, prefix, tail_bytes);
        if (time_ok && !footer_written) writer_note_fault("footer_write", errno);
    }
#endif
    if (!time_ok) writer_note_fault("file_time", errno);
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (!card_recording_locked())
#endif
        footer_written = time_ok && fwrite(&footer, 1U, sizeof(footer), s.file) == sizeof(footer);
    if (time_ok && !footer_written) writer_note_fault("footer_write", errno);
    const bool ok = footer_written &&
                    sync_archive_file(s.file, "footer_sync");
    const int close_result = fclose(s.file);
    if (close_result != 0) writer_note_fault("file_close", errno);
    s.file = NULL;
    if (!ok || close_result != 0) {
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        return ESP_FAIL;
    }
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
#if CONFIG_NUWAX_SD_BATCH_REPAIR_ENABLE
    const esp_err_t prefix_integrity = writer_repair_check_seal();
    if (prefix_integrity != ESP_OK) {
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        return prefix_integrity; /* Keep .part, including late footer-write damage. */
    }
#endif
    const esp_err_t seal_integrity = writer_integrity_check_seal();
    if (seal_integrity != ESP_OK) {
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        return seal_integrity; /* Retain .part, never publish unchecked footer/time. */
    }
#endif
    const int64_t rename_started = esp_timer_get_time();
    const int rename_result = rename(s.open_path, s.ready_path);
    log_sd_step("rename", rename_started);
    if (rename_result != 0) {
        writer_note_fault("rename", errno);
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        return ESP_FAIL;
    }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    const esp_err_t catalog_err = card_seal_segment();
    if (catalog_err != ESP_OK) {
        writer_note_fault("index_seal", errno);
        atomic_store(&s.degraded, true);
        return catalog_err;
    }
#endif
    atomic_store_explicit(&s_directory_dirty, true, memory_order_release);
    s.used_bytes += sizeof(footer);
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    const char *ready_base = strrchr(s.ready_path, '/');
    if (strncmp(ready_base ? ready_base + 1 : s.ready_path, "card_", 5) != 0)
#endif
    (void)atomic_fetch_add_explicit(&s.ready_count, 1U, memory_order_release);
    s.open_path[0] = '\0';
    s.ready_path[0] = '\0';
    return ESP_OK;
}

static esp_err_t append_locked(const spool_item_t *item)
{
    s.writer_frame_written = false;
    s.writer_frame_uncertain = false;
    if (!atomic_load_explicit(&s.active, memory_order_acquire) ||
        !item || item->bytes == 0U ||
        item->generation != s.capture_generation) {
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (card_recording_locked()) {
        uint32_t timestamp = 0;
        memcpy(&timestamp, item->data + 14U, sizeof(timestamp));
        const uint64_t now = (uint64_t)esp_timer_get_time() / 1000U;
        s_card_frame_start = (uint64_t)((int64_t)now + (int32_t)(timestamp - (uint32_t)now));
        s_card_frame_realtime = item->realtime;
        if (s.file && s.frame_count && s.mode == NUWAX_OFFLINE_AUDIO_MODE_MONITORING &&
            s_card_segment.realtime != item->realtime) {
            const esp_err_t boundary = commit_segment_locked(!atomic_load(&s.degraded));
            if (boundary != ESP_OK) return boundary;
        }
    }
#endif
    if (!s.file) {
        const esp_err_t open_err = open_segment_locked();
        if (open_err != ESP_OK) {
            return open_err;
        }
    }
    const uint64_t record_bytes = sizeof(uint32_t) + item->bytes;
    if (nuwax_offline_quota_blocked(CONFIG_NUWAX_SD_SESSION_ENABLE, s.used_bytes,
                                  ARCHIVE_QUOTA_BYTES, record_bytes + FOOTER_BYTES)) {
        atomic_store_explicit(&s_space_blocked, true, memory_order_release);
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    if (sizeof(archive_header_t) + s.payload_bytes +
            (uint64_t)s.frame_count * sizeof(uint32_t) + record_bytes + FOOTER_BYTES >
        CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES) {
        if (s.frame_count == 0U) {
            return ESP_ERR_NO_MEM;
        }
        const esp_err_t sealed = commit_segment_locked(!atomic_load_explicit(&s.degraded, memory_order_acquire));
        if (sealed != ESP_OK) return sealed;
        const esp_err_t opened = open_segment_locked();
        if (opened != ESP_OK) return opened;
    }
    /* last_good_offset is the logical end accepted by fwrite. verify_offset
     * advances only after sync and independent readback. Picolibc ftell flushes
     * its write buffer, so calling it for each 20 ms record defeats batching. */
    const long record_start = s.last_good_offset;
    if (record_start < (long)sizeof(archive_header_t) ||
        record_bytes > (uint64_t)LONG_MAX - (uint64_t)record_start) {
        writer_note_fault("record_offset", 0); return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t len = (uint32_t)item->bytes;
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
    uint32_t before_crc = 0U;
    if (card_recording_locked() && !writer_integrity_before(item, &before_crc)) return ESP_ERR_INVALID_CRC;
#endif
    const bool length_ok = record_start >= (long)sizeof(archive_header_t) &&
                           fwrite(&len, 1U, sizeof(len), s.file) == sizeof(len);
    const bool payload_ok = length_ok &&
                            fwrite(item->data, 1U, item->bytes, s.file) == item->bytes;
    if (!payload_ok) {
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
        if (writer_integrity_live()) {
            s.integrity->active = false; nuwax_sd_integrity_io_freeze(NULL);
        }
#endif
        writer_note_fault(length_ok ? "record_payload" : "record_length", errno);
        clearerr(s.file);
        const bool flushed = fflush(s.file) == 0;
        bool rolled_back = false;
        if (record_start >= (long)sizeof(archive_header_t)) {
            const bool truncated = ftruncate(fileno(s.file), record_start) == 0;
            const bool positioned = fseek(s.file, record_start, SEEK_SET) == 0;
            rolled_back = flushed && truncated && positioned;
        }
        s.writer_frame_uncertain = !rolled_back;
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        return ESP_FAIL;
    }
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
    if (card_recording_locked() && !writer_integrity_after(item, before_crc)) return ESP_ERR_INVALID_CRC;
#endif
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (card_recording_locked() && !sd_audio_seal_head_record(s.seal_head, item->data,
            (uint32_t)item->bytes, s.capture_generation, s.segment_index, crc32_update)) {
        s.writer_frame_uncertain = true;
        s_writer_fault_stage = "seal_prefix_ram"; s_writer_fault_errno = 0; return ESP_ERR_INVALID_CRC;
    }
#endif
    s.last_good_offset = record_start + (long)record_bytes;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (card_recording_locked()) s_card_segment.end_ms = s_card_frame_start + s.frame_duration_ms;
#endif
    s.frame_count++;
    s.writer_frame_written = true;
    s.payload_bytes += item->bytes;
    s.payload_crc32 = crc32_update(s.payload_crc32, item->data, item->bytes);
    s.verify_crc32 = crc32_update(s.verify_crc32, &len, sizeof(len));
    s.verify_crc32 = crc32_update(s.verify_crc32, item->data, item->bytes);
    s.verify_bytes += (uint32_t)record_bytes;
    s.used_bytes += record_bytes;
    bool first_card_frame = false;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    first_card_frame = card_recording_locked() && s.frame_count == 1U;
#endif
    if ((first_card_frame || (s.frame_count % DURABILITY_SYNC_INTERVAL_FRAMES) == 0U) &&
        (!sync_archive_file(s.file, first_card_frame ? "first_frame_sync" : "periodic_sync") ||
         writer_verify_records_locked() != ESP_OK)) {
        atomic_store_explicit(&s.degraded, true, memory_order_release);
        return ESP_FAIL;
    }
    if (first_card_frame) atomic_store_explicit(&s.archive_ready, true, memory_order_release);
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    /* Card monitoring uses fixed-size PCM records. Seal as soon as another
     * complete record would exceed the file limit, without waiting for the
     * next MIC frame. Capture keeps writing the next file while this sealed
     * file uploads; stop/pause and online/offline boundaries still seal tails. */
    if (card_recording_locked() && s.mode == NUWAX_OFFLINE_AUDIO_MODE_MONITORING &&
        sizeof(archive_header_t) + s.payload_bytes +
            (uint64_t)s.frame_count * sizeof(uint32_t) + FOOTER_BYTES + record_bytes >
            CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES)
        return commit_segment_locked(!atomic_load(&s.degraded));
#endif
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
    const bool source_fault = s_writer_fault_stage &&
        (!strcmp(s_writer_fault_stage, "frame_queue_crc") ||
         !strcmp(s_writer_fault_stage, "frame_write_crc") ||
         !strcmp(s_writer_fault_stage, "seal_prefix_ram") ||
         !strcmp(s_writer_fault_stage, "seal_tail_ram"));
    if (!source_fault && err != ESP_ERR_NO_MEM && err != ESP_ERR_NOT_FINISHED &&
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
                 "offline SD writer entering backoff err=%s retry_ms=%lu stage=%s errno=%d index=%lu",
                 esp_err_to_name(err),
                 (unsigned long)s.writer_retry_backoff_ms,
                 s_writer_fault_stage ? s_writer_fault_stage : "unspecified", s_writer_fault_errno,
                 (unsigned long)s.segment_index);
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

#include "sd_audio_active_recovery.inc"

static void writer_task(void *arg)
{
    (void)arg;
    uint32_t processed_frames = 0U;
    spool_item_t *retry_item = NULL;
    uint32_t retry_started_ms = 0;
    offline_io_budget_t budget = {.since_us = esp_timer_get_time()};
    ESP_LOGI(TAG,
             "offline SD writer started stack_min_free=%u stack_bytes=%u priority=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)OFFLINE_WRITER_TASK_STACK_BYTES, (unsigned)OFFLINE_WRITER_TASK_PRIORITY);
    for (;;) {
        /* A recovery owner may be waiting to replace the mounted medium. The
         * queue itself remains allocated, so pausing before dequeue keeps
         * recovery from acquiring a new in-flight FILE operation. */
        while (atomic_load_explicit(&s.media_recovery_requested, memory_order_acquire)
#if CONFIG_NUWAX_SD_SESSION_ENABLE
               || atomic_load_explicit(&s_card_io_recovery.pending, memory_order_acquire)
#endif
        ) {
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
        spool_item_t *item = retry_item;
        const BaseType_t received = item ? pdTRUE : xQueueReceive(s.queue, &item, 0);
        retry_item = NULL;
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
            writer_storage_idle();
            continue;
        }
        if (item->type == 2U) {
            if (xSemaphoreTake(s.lock, portMAX_DELAY) != pdTRUE) {
                (void)xEventGroupSetBits(s.events, FAILED_BIT);
                writer_storage_idle();
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
            writer_storage_idle();
            continue;
        }
        if (item->type != 1U) {
            release_slot(item);
            writer_storage_idle();
            continue;
        }
        if (xSemaphoreTake(s.lock, portMAX_DELAY) != pdTRUE) {
            atomic_store_explicit(&s.degraded, true, memory_order_release);
            release_slot(item);
            writer_storage_idle();
            continue;
        }
        const int64_t write_started_us = esp_timer_get_time();
        esp_err_t err = ESP_OK;
        s.writer_frame_written = false;
        s.writer_frame_uncertain = false;
        if (atomic_exchange_explicit(&s_replay_media_fault, false, memory_order_acq_rel)) {
            writer_note_fault("replay_read", 0);
            err = ESP_FAIL;
        } else if (card_io_recovery_consume_retry_locked(item)) {
            release_slot(item);
            xSemaphoreGive(s.lock);
            writer_storage_idle();
            continue;
        } else if (!writer_try_recover_locked()) {
            err = ESP_ERR_NOT_FINISHED;
        } else {
            err = append_locked(item);
        }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        if (err != ESP_OK && card_io_recovery_request_locked(err, item)) {
            /* A frame not yet appended stays at the queue head. An appended
             * frame is never duplicated; recovered tails retain honest CRC
             * and completeness evidence. No FILE is used during remount. */
            if (!s.writer_frame_written) retry_item = item;
            else release_slot(item);
            retry_started_ms = 0U;
            xSemaphoreGive(s.lock);
            writer_storage_idle();
            nuwax_offline_audio_supervisor_wake();
            vTaskDelay(pdMS_TO_TICKS(20U));
            continue;
        }
#endif
        if (err != ESP_OK && !s.writer_frame_written &&
#if CONFIG_NUWAX_SD_SESSION_ENABLE
            !atomic_load(&s_card_failed) &&
#endif
            (!retry_started_ms || (uint32_t)(offline_now_ms() - retry_started_ms) < 1500U)) {
            if (!retry_started_ms) retry_started_ms = offline_now_ms();
            if (!atomic_load(&s.writer_error)) {
                if (s.file) (void)commit_segment_locked(false);
                writer_schedule_retry_locked(err);
            }
            retry_item = item; /* Keep ownership; never discard frames during backoff. */
            xSemaphoreGive(s.lock);
            writer_storage_idle();
            vTaskDelay(pdMS_TO_TICKS(20U));
            continue;
        }
        retry_started_ms = 0;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        if (err != ESP_OK && nuwax_sd_session_active()) {
            atomic_store(&s_card_failed, true);
            (void)nuwax_offline_audio_request_stop(false);
        }
#endif
        if (err != ESP_OK) {
            if (!s.writer_frame_written) (void)atomic_fetch_add_explicit(&s.dropped_frames, 1U,
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
        const uint32_t readback_calls = s.verify_calls;
        const uint64_t readback_us = s.verify_us;
        xSemaphoreGive(s.lock);
        release_slot(item);
        writer_storage_idle();
        ++processed_frames;
        if (offline_io_budget_due(&budget, work_bytes, esp_timer_get_time())) {
            vTaskDelay(1U);
            offline_io_budget_reset(&budget, esp_timer_get_time());
        }
        if ((processed_frames % DURABILITY_SYNC_INTERVAL_FRAMES) == 0U) {
            ESP_LOGI(TAG,
                     "offline SD writer progress frames=%lu queue=%u free_slots=%u stack_min_free=%u err=%s dropped_frames=%lu readback_calls=%lu readback_ms=%llu",
                     (unsigned long)processed_frames,
                     (unsigned)uxQueueMessagesWaiting(s.queue),
                     (unsigned)uxQueueMessagesWaiting(s.free_queue),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL),
                     esp_err_to_name(err),
                     (unsigned long)atomic_load_explicit(&s.dropped_frames, memory_order_relaxed),
                     (unsigned long)readback_calls, (unsigned long long)(readback_us / 1000U));
#if CONFIG_NUWAX_SD_WRITE_VERIFY_ENABLE
            if ((processed_frames % (5U * DURABILITY_SYNC_INTERVAL_FRAMES)) == 0U) {
                const nuwax_sd_write_verify_stats_t verified = nuwax_sd_write_verify_stats();
                ESP_LOGI(TAG, "SD verified writes revision=218 writes=%lu sectors=%lu rewrites=%lu recovered=%lu failures=%lu memory_denied=%lu max_us=%lu reserve_uses=%lu single_read_recoveries=%lu",
                    (unsigned long)verified.writes, (unsigned long)verified.verified_sectors,
                    (unsigned long)verified.rewrites, (unsigned long)verified.recovered,
                    (unsigned long)verified.failures, (unsigned long)verified.memory_denied,
                    (unsigned long)verified.max_us, (unsigned long)verified.reserve_uses,
                    (unsigned long)verified.single_read_recoveries);
            }
#endif
        }
    }
}

static void destroy_unpublished_runtime(void)
{
    if (s.queue) vQueueDeleteWithCaps(s.queue);
    if (s.free_queue) vQueueDeleteWithCaps(s.free_queue);
    if (s.slot_pool) heap_caps_free(s.slot_pool);
    if (s.write_buffer) heap_caps_free(s.write_buffer);
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (s.seal_head) heap_caps_free(s.seal_head);
#endif
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
    sd_integrity_destroy(s.integrity);
#endif
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
    atomic_init(&s.archive_ready, false);
    atomic_init(&s.closing, false);
    atomic_init(&s.degraded, false);
    atomic_init(&s.writer_error, false);
    atomic_init(&s.writer_busy, false);
    atomic_store_explicit(&s_storage_wait_armed, false, memory_order_release);
    atomic_init(&s.media_recovery_requested, false);
    atomic_init(&s.dropped_frames, 0U);
    atomic_init(&s.ready_count, 0U);
    atomic_init(&s.capture_stop, false);
    atomic_init(&s.capture_complete, true);
    atomic_init(&s.capture_running, false);
    atomic_init(&s.replay_running, false);
    s.slot_pool = heap_caps_calloc(QUEUE_DEPTH, sizeof(spool_item_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* Separate stdio caches keep independent readback on bulk SD reads,
     * without repeated allocations or sharing the writer's clean cache. */
    s.write_buffer = heap_caps_malloc(ARCHIVE_STDIO_BUFFER_BYTES *
                                      (CONFIG_NUWAX_SD_SESSION_ENABLE ? 2U : 1U),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "SD writer batching revision=216 stdio_bytes=%u logical_offset=1 prefix_guard_bytes=%u durability_frames=%u",
        ARCHIVE_STDIO_BUFFER_BYTES, (unsigned)sizeof(sd_audio_seal_head_t), DURABILITY_SYNC_INTERVAL_FRAMES);
    ESP_LOGI(TAG, "SD stability revision=218 guarded_tail_bytes=1024 independent_read_retry=1 same_original_crc=1 boundary_sectors=64 native_reserve_bytes=1024 replay_checkpoint=1");
    s.queue = xQueueCreateWithCaps(QUEUE_DEPTH, sizeof(spool_item_t *),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.free_queue = xQueueCreateWithCaps(QUEUE_DEPTH, sizeof(spool_item_t *),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.events = xEventGroupCreate();
    s.capture_events = xEventGroupCreate();
    s.lock = xSemaphoreCreateMutex();
    s.admission_lock = xSemaphoreCreateMutex();
    s.writer_gate = xSemaphoreCreateMutexStatic(&s_writer_gate_storage);
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
    if (nuwax_sd_integrity_io_init()) s.integrity = sd_integrity_create(crc32_update);
    ESP_LOGI(TAG, "SD integrity diagnostics revision=213 enabled=%u psram_bytes=%u queue_slots=%u; existing CRC guard always active",
        s.integrity != NULL, (unsigned)(s.integrity ? sizeof(*s.integrity) : 0U), QUEUE_DEPTH);
#endif
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

#if CONFIG_NUWAX_SD_SESSION_ENABLE
    /* Initialization can retry after a writer-task allocation failure too.
     * An old successful probe must not survive that physical mount boundary. */
    const esp_err_t prepare_mount = sd_audio_catalog_prepare_remount();
    if (prepare_mount != ESP_OK) {
        destroy_unpublished_runtime();
        xSemaphoreGive(s_init_lock);
        return prepare_mount;
    }
#endif
    nuwax_sd_card_info_t info = {0};
    esp_err_t media_result = nuwax_sd_card_mount(&info);
    if (media_result == ESP_OK) {
        media_result = ensure_directory();
        if (media_result == ESP_OK) media_result = prepare_media_space();
    }
    if (media_result == ESP_OK) {
        media_result = recover_open_segments();
    }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    s_card_boot_id = esp_random();
    if (!s_card_boot_id) s_card_boot_id = 1U;
    if (media_result == ESP_OK) {
        media_result = sd_audio_catalog_recover(s_card_boot_id);
        bool pending = true;
        if (media_result == ESP_OK) media_result = sd_audio_catalog_has_pending(&pending);
        atomic_store(&s_card_pending_upload, pending);
    }
#endif
    if (media_result == ESP_OK) media_result = refresh_next_segment_index();
    if (media_result == ESP_OK) {
        s.used_bytes = scan_used_bytes();
        atomic_store_explicit(&s.ready_count, scan_ready_count(),
                              memory_order_release);
        media_result = refresh_capture_capacity_locked();
    } else {
        if (nuwax_sd_card_is_mounted()) {
            /* Metadata qualification can fail after bulk qualification.
             * Latch that real I/O fault before unmount, so recovery also
             * tries the validated 20 MHz profile instead of repeating 40. */
            if (media_result == ESP_FAIL || media_result == ESP_ERR_INVALID_CRC ||
                media_result == ESP_ERR_INVALID_SIZE) nuwax_sd_card_mark_io_fault(media_result);
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
    if (media_result == ESP_OK) reset_media_recovery_locked(true);

    if (xTaskCreatePinnedToCoreWithCaps(writer_task, "offline_sd_writer",
                                        OFFLINE_WRITER_TASK_STACK_BYTES, NULL, OFFLINE_WRITER_TASK_PRIORITY,
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        if (runtime_initialized()) {
            card_io_recovery_observe_reader_fault();
            (void)card_io_recovery_service();
            card_identity_service(); card_time_service(); card_monitor_retention_service(); card_finalize();
            card_offline_verify_service();
        }
#endif
        esp_err_t err = runtime_initialized()
            ? nuwax_offline_audio_recover_if_idle(250U)
            : nuwax_offline_audio_init();
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
        if (err == ESP_OK) writer_integrity_service();
#endif
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

/* Counts ownership of actual allocated rings, including asynchronous tails.
 * Publication follows allocation; removal precedes free, so races under-credit. */
static atomic_uint s_mirror_staging_rings;

_Static_assert(REPLAY_PSRAM_RESERVE_BYTES >=
    MIRROR_STAGING_FRAMES * sizeof(mirror_staged_frame_t) + 256U * 1024U,
    "replay reserve must cover capture staging and control headroom");

static esp_err_t append_frame(const nuwax_offline_audio_frame_t *frame,
                              TickType_t wait, bool count_loss);

static void mirror_note_loss(nuwax_offline_audio_mirror_t *m, uint32_t frames)
{
    m->rejected_frames += frames;
    m->complete = false;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (frames) card_note_mirror_result(m->session_id, true);
#endif
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    /* The asynchronous tail can fail after the MIC task has exited. Keep
     * that loss visible to the eventual whole-session completion record. */
    if (!m->complete || m->rejected_frames)
        card_note_mirror_result(m->session_id, m->rejected_frames != 0U);
#endif
    ESP_LOGI(TAG, "SD mirror admission session=%s accepted=%lu rejected=%lu complete=%d; persistence follows writer close",
             m->session_id, (unsigned long)m->accepted_frames,
             (unsigned long)m->rejected_frames, m->complete && !m->rejected_frames);
    if (m->staging) atomic_fetch_sub_explicit(&s_mirror_staging_rings, 1U, memory_order_acq_rel);
    heap_caps_free(m->staging);
    m->staging = NULL;
    m->started = m->attached = false;
}

#include "offline_mirror_close_queue.inc"

static void mirror_close_task(void *arg)
{
    mirror_close_node_t *node = arg;
    nuwax_offline_audio_mirror_t *m = &node->mirror;
    while (!mirror_close_is_head(node)) vTaskDelay(pdMS_TO_TICKS(20U));
    /* Queue waiting consumes the same budget; never grant a fresh interval
     * after a slow predecessor. Only the head may close storage resources. */
    const uint32_t deadline = node->deadline_ms;
    while (m->staging_count && !offline_deadline_elapsed(offline_now_ms(), deadline)) {
        mirror_drain(m, true);
        if (m->staging_count) vTaskDelay(pdMS_TO_TICKS(20U));
    }
    mirror_finish(m);
    mirror_close_remove(node);
    heap_caps_free(node);
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
    /* Legacy callers pass PCM timestamps or zero. The storage owner owns
     * the deadline clock; freeze it before draining or queueing a tail. */
    (void)now_ms;
    const uint32_t deadline_ms = offline_now_ms() + MIRROR_CLOSE_TIMEOUT_MS;
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
        mirror_close_node_t *tail = heap_caps_malloc(sizeof(*tail), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (tail) {
            tail->mirror = *m;
            tail->deadline_ms = deadline_ms;
            mirror_close_enqueue(tail);
            if (xTaskCreatePinnedToCoreWithCaps(mirror_close_task, "sd_mirror_close",
                    4096U, tail, 3U, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
                const bool offline = m->offline;
                memset(m, 0, sizeof(*m));
                m->offline = offline;
                return ESP_OK;
            }
            mirror_close_remove(tail);
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
        if (!atomic_load_explicit(&s_media_ready, memory_order_acquire) ||
            recording_space_blocked()) {
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
        if (!m->staging) {
            m->staging = heap_caps_calloc(MIRROR_STAGING_FRAMES,
                sizeof(mirror_staged_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (m->staging) atomic_fetch_add_explicit(&s_mirror_staging_rings, 1U, memory_order_release);
        }
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

static bool healthy_capture_defers_media_probe(void)
{
    /* Silent monitoring has no FILE, but still owns a healthy recording
     * path. Do not turn media_ready off for a routine probe in that window.
     * A real fault/full card must remain recoverable while PCM is buffered. */
    return atomic_load_explicit(&s.capture_running, memory_order_acquire) &&
           atomic_load_explicit(&s_media_ready, memory_order_acquire) &&
           !recording_space_blocked();
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
    if (nuwax_sd_session_active() || healthy_capture_defers_media_probe() ||
        atomic_load_explicit(&s.active, memory_order_acquire) ||
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
            /* A scene may reserve the card while this caller waits for the
             * locks, before its first FILE or capture worker is published. */
            nuwax_sd_session_active() || healthy_capture_defers_media_probe() ||
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
        nuwax_sd_session_active() ||
        healthy_capture_defers_media_probe() ||
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
        const bool directory_dirty = atomic_load_explicit(&s_directory_dirty,
                                                          memory_order_acquire);
        const bool recovery_required = nuwax_offline_recovery_scan_required(
            s.media_was_ready, atomic_load_explicit(&s.degraded, memory_order_acquire),
            atomic_load_explicit(&s.writer_error, memory_order_acquire),
            s.media_recovery_last_error != ESP_OK);
        if (!recovery_required) {
            /* Successful seals/deletes change the directory, not the validity
             * of its contents. Re-reading every sealed PCM file here blocked
             * replay for tens of seconds after a long meeting or reconnect.
             * Boot, remount and fault recovery still validate every prefix. */
            esp_err_t capacity_err = ESP_OK;
            if (directory_dirty) {
#if CONFIG_NUWAX_SD_SESSION_ENABLE
                bool pending = true;
                capacity_err = sd_audio_catalog_has_pending(&pending);
                if (capacity_err == ESP_OK) atomic_store(&s_card_pending_upload, pending);
#endif
                if (capacity_err == ESP_OK) capacity_err = refresh_next_segment_index();
                s.used_bytes = scan_used_bytes();
                atomic_store_explicit(&s.ready_count, scan_ready_count(), memory_order_release);
            }
            if (capacity_err == ESP_OK) capacity_err = refresh_capture_capacity_locked();
            if (capacity_err == ESP_OK) {
                atomic_store_explicit(&s_directory_dirty,
                    atomic_load_explicit(&s.degraded, memory_order_acquire), memory_order_release);
                reset_media_recovery_locked(false);
            } else {
                atomic_store_explicit(&s.degraded, true, memory_order_release);
            }
            atomic_store_explicit(&s.media_recovery_requested, false,
                                  memory_order_release);
            atomic_store_explicit(&s_media_ready, capacity_err == ESP_OK, memory_order_release);
            xSemaphoreGive(s.lock);
            xSemaphoreGive(s.writer_gate);
            xSemaphoreGive(s.admission_lock);
            return capacity_err;
        }
        result = ensure_directory();
        if (result == ESP_OK) result = prepare_media_space();
        if (result == ESP_OK) {
            result = recover_open_segments();
#if CONFIG_NUWAX_SD_SESSION_ENABLE
            if (result == ESP_OK) result = sd_audio_catalog_recover(s_card_boot_id);
            bool pending = true;
            if (result == ESP_OK) result = sd_audio_catalog_has_pending(&pending);
            if (result == ESP_OK) atomic_store(&s_card_pending_upload, pending);
#endif
        }
        if (result == ESP_OK) result = refresh_next_segment_index();
        if (result == ESP_OK) {
            s.used_bytes = scan_used_bytes();
            atomic_store_explicit(&s.ready_count, scan_ready_count(),
                                  memory_order_release);
            result = refresh_capture_capacity_locked();
            atomic_store_explicit(&s_directory_dirty,
                                  atomic_load_explicit(&s.degraded,
                                                       memory_order_acquire),
                                  memory_order_release);
            atomic_store_explicit(&s.media_recovery_requested, false,
                                  memory_order_release);
            reset_media_recovery_locked(true);
            atomic_store_explicit(&s_media_ready, result == ESP_OK, memory_order_release);
            xSemaphoreGive(s.lock);
            xSemaphoreGive(s.writer_gate);
            xSemaphoreGive(s.admission_lock);
            return result;
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
    esp_err_t recover_err = ESP_OK;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    recover_err = sd_audio_catalog_prepare_remount();
#endif
    if (recover_err == ESP_OK) recover_err = nuwax_sd_card_recover(
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        if (result == ESP_OK) result = sd_audio_catalog_recover(s_card_boot_id);
        bool pending = true;
        if (result == ESP_OK) result = sd_audio_catalog_has_pending(&pending);
        if (result == ESP_OK) atomic_store(&s_card_pending_upload, pending);
#endif
    }
    if (result == ESP_OK) result = refresh_next_segment_index();
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

    s.used_bytes = scan_used_bytes();
    /* A remounted card may contain quarantined files from another media epoch. */
    atomic_store_explicit(&s_quarantine_recheck_pending, true, memory_order_release);
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
    s_writer_fault_stage = NULL;
    s_writer_fault_errno = 0;
    /* A remount may run while the reader still retains PCM. It must not
     * erase that reader's earlier discontinuity or failed write. */
    result = refresh_capture_capacity_locked();
    reset_media_recovery_locked(true);
    atomic_store_explicit(&s_media_ready, result == ESP_OK, memory_order_release);
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
    return result;
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
    if (nuwax_offline_quota_blocked(CONFIG_NUWAX_SD_SESSION_ENABLE, s.used_bytes,
                                  ARCHIVE_QUOTA_BYTES, CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES)) {
        atomic_store_explicit(&s_space_blocked, true, memory_order_release);
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
    atomic_store_explicit(&s.archive_ready, false, memory_order_release);
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (nuwax_sd_session_active() &&
        (!atomic_load_explicit(&s_media_ready, memory_order_acquire) ||
         atomic_load_explicit(&s_replay_media_fault, memory_order_acquire) ||
         atomic_load_explicit(&s_card_io_recovery.pending, memory_order_acquire))) {
        /* Backpressure retains PCM in the existing 768-frame mirror ring.
         * INVALID_STATE would detach it and request a false terminal close. */
        xSemaphoreGive(s.admission_lock);
        return ESP_ERR_NOT_FINISHED;
    }
#endif
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
    item->realtime = false;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    item->realtime = nuwax_sd_session_active() && card_frame_realtime(frame->timestamp_ms);
#endif
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
#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
    item->queued_crc_valid = nuwax_sd_session_active();
    item->queued_crc = item->queued_crc_valid ? crc32_update(0U, item->data, frame_len) : 0U;
#endif
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
    int64_t start_deadline_us;
    uint32_t cancel_epoch;
    uint32_t first_sequence;
    uint32_t sample_rate;
    uint8_t channels;
    uint16_t frame_duration_ms;
    char session_id[48];
} offline_capture_config_t;

static bool offline_capture_cancelled(uint32_t epoch)
{
    return atomic_load_explicit(&s.capture_stop, memory_order_acquire) ||
           atomic_load_explicit(&s_capture_cancel_epoch, memory_order_acquire) != epoch;
}

nuwax_offline_capture_health_t nuwax_offline_audio_capture_health(void)
{
    return (nuwax_offline_capture_health_t)atomic_load_explicit(
        &s_capture_health, memory_order_acquire);
}

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
    nuwax_offline_capture_watch_t storage_watch = {0};
    nuwax_offline_audio_mirror_t archive = {.offline = true};
    nuwax_offline_voice_t voice_archive = {0};
    monitoring_speech_detector_t detector = {0};
    const bool monitoring = cfg.mode == NUWAX_OFFLINE_AUDIO_MODE_MONITORING;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    uint64_t card_meeting_started_us = 0U;
    uint64_t card_next_frame_ms = 0U;
    if (cfg.start_deadline_us && esp_timer_get_time() >= cfg.start_deadline_us) goto done;
    if (!card_prepare_capture()) goto done;
    /* The reserved session start survives network changes and MIC handoffs. */
    if (nuwax_sd_session_active()) card_meeting_started_us = s_card_session.start_ms * 1000ULL;
    if (cfg.start_deadline_us && esp_timer_get_time() >= cfg.start_deadline_us) goto done;
#endif
    if (monitoring) {
        monitoring_speech_detector_init(&detector);
        esp_err_t voice_init_err;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        if (nuwax_sd_session_active()) {
            const nuwax_offline_voice_policy_t policy = {
                .silence_ms = CONFIG_NUWAX_MONITORING_SILENCE_STOP_MS,
                .resume_confirm_frames = CONFIG_NUWAX_MONITORING_RESUME_CONFIRM_FRAMES,
                .activity_confirm_frames = CONFIG_NUWAX_MONITORING_SPEECH_REFRESH_FRAMES,
                .start_recording = true,
            };
            voice_init_err = nuwax_offline_voice_init_with_policy(&voice_archive,
                cfg.session_id, cfg.sample_rate, cfg.frame_duration_ms, &policy);
            ESP_LOGI(TAG, "SD monitoring capture policy=continuous120 silence_ms=%lu resume_frames=%lu activity_frames=%lu preroll_ms=500 local_resume=1 seal=file_limit segment_limit=%u",
                (unsigned long)policy.silence_ms, (unsigned long)policy.resume_confirm_frames,
                (unsigned long)policy.activity_confirm_frames,
                (unsigned)CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES);
        } else
#endif
        {
            voice_init_err = nuwax_offline_voice_init(&voice_archive, cfg.session_id,
                cfg.sample_rate, cfg.frame_duration_ms);
        }
        if (voice_init_err != ESP_OK) {
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
         !offline_capture_cancelled(cfg.cancel_epoch);
         ++attempt) {
        if (cfg.start_deadline_us && esp_timer_get_time() >= cfg.start_deadline_us) break;
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
        if (!offline_capture_cancelled(cfg.cancel_epoch)) {
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
    if (cfg.start_deadline_us && esp_timer_get_time() >= cfg.start_deadline_us) goto done;
    offline_started_us = (uint64_t)esp_timer_get_time();
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    card_next_frame_ms = offline_started_us / 1000U;
#endif
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
    while (!offline_capture_cancelled(cfg.cancel_epoch)) {
        if (cfg.start_deadline_us && esp_timer_get_time() >= cfg.start_deadline_us) break;
        const unsigned ticket = atomic_load_explicit(&s_mic_transfer, memory_order_acquire);
        if (current_samples == 0U && (ticket & 1U) != 0U) {
            transfer_ticket = ticket;
            atomic_store_explicit(&s.capture_complete, false, memory_order_release);
            break;
        }
        uint32_t offline_limit_hours =
            cfg.mode == NUWAX_OFFLINE_AUDIO_MODE_MEETING
                ? CONFIG_NUWAX_MEETING_OFFLINE_MAX_HOURS
                : CONFIG_NUWAX_MONITORING_OFFLINE_MAX_HOURS;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        const bool card_active = nuwax_sd_session_active();
        if (card_active && !monitoring) offline_limit_hours = CONFIG_NUWAX_SD_MEETING_MAX_HOURS;
        const uint64_t budget_used = card_active
            ? nuwax_offline_budget_used(0U, card_meeting_started_us, (uint64_t)esp_timer_get_time())
            : nuwax_offline_budget_used(cfg.limit_used_us, offline_started_us, (uint64_t)esp_timer_get_time());
        if (!sd_monitor_unlimited(nuwax_sd_session_active(), monitoring) &&
            nuwax_offline_limit_elapsed(budget_used, 0U, offline_limit_hours)) {
#else
        if (nuwax_offline_limit_elapsed(nuwax_offline_budget_used(
                                            cfg.limit_used_us, offline_started_us,
                                            (uint64_t)esp_timer_get_time()), 0U,
                                        offline_limit_hours)) {
#endif
            ESP_LOGI(TAG, "%s duration limit reached hours=%u; finalizing",
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
        if (offline_capture_cancelled(cfg.cancel_epoch)) {
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
            const uint64_t after_gap = (uint64_t)esp_timer_get_time() / 1000U;
            if (after_gap > card_next_frame_ms) card_next_frame_ms = after_gap;
#endif
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
            if (nuwax_sd_session_active()) frame_timestamp_ms = (uint32_t)card_next_frame_ms;
#endif
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        if (nuwax_sd_session_active()) card_next_frame_ms += cfg.frame_duration_ms;
#endif
        atomic_store_explicit(&s_next_capture_sequence, sequence, memory_order_release);
        const uint32_t rejected_before = monitoring
            ? voice_archive.mirror.rejected_frames : archive.rejected_frames;
        if (monitoring) {
            const bool speech = monitoring_detect_speech(&detector, samples,
                                                         frame_samples, NULL, NULL);
            const bool was_recording = voice_archive.recording;
            nuwax_offline_voice_process(&voice_archive, samples, frame_samples,
                                        speech, frame.sequence, frame.timestamp_ms);
#if CONFIG_NUWAX_SD_SESSION_ENABLE
            if (nuwax_sd_session_active() && was_recording != voice_archive.recording) {
                ESP_LOGI(TAG, "SD monitoring capture %s session=%s sequence=%lu quiet_ms=%llu; session retained microphone listening",
                    voice_archive.recording ? "resumed" : "paused", cfg.session_id,
                    (unsigned long)frame.sequence,
                    (unsigned long long)voice_archive.quiet_frames * cfg.frame_duration_ms);
            }
#else
            (void)was_recording;
#endif
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
        const nuwax_offline_audio_mirror_t *sink =
            monitoring ? &voice_archive.mirror : &archive;
        const bool media_ready =
            atomic_load_explicit(&s_media_ready, memory_order_acquire) &&
            !recording_space_blocked();
        /* Paused monitoring intentionally accepts no PCM for storage. Keep
         * listening during a media retry; actual speech must still acquire
         * staging or take the normal honest unavailable/loss stop path. */
        const bool quiet_listening = monitoring && !voice_archive.recording;
        const bool sink_ready = quiet_listening || (media_ready &&
#if CONFIG_NUWAX_SD_SESSION_ENABLE
            /* Starting/attached queues are not proof of a saved recording.
             * The writer publishes this only after the first full NA frame
             * and both metadata records have reached durable storage. */
            (!nuwax_sd_session_active() || !sink->started ||
             atomic_load_explicit(&s.archive_ready, memory_order_acquire)) &&
#endif
            (!atomic_load_explicit(&s.active, memory_order_acquire) ||
             !atomic_load_explicit(&s.writer_error, memory_order_acquire)) &&
            /* Quiet monitoring has no open mirror by design. */
            (!sink->started || (sink->attached && sink->staging_count < 2U)));
        const bool lost = sink->rejected_frames > rejected_before ||
            (monitoring && voice_archive.recording && !sink->started);
        nuwax_offline_capture_health_t health = nuwax_offline_capture_observe(
            &storage_watch, offline_now_ms(), sink_ready,
            sink->started && sink->staging_count < MIRROR_STAGING_FRAMES, lost);
        const uint32_t budget_now = offline_now_ms();
        const uint32_t recovery_budget = quiet_listening ? 10000U : nuwax_offline_recovery_budget_ms(&storage_watch,
            budget_now, sink->started ? MIRROR_STAGING_FRAMES - sink->staging_count : 0U,
            20U, 2000U, 10000U);
        atomic_store_explicit(&s_capture_recovery_until, budget_now + recovery_budget, memory_order_release);
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        health = nuwax_offline_capture_admission_health(health, nuwax_sd_session_active(),
            quiet_listening || atomic_load_explicit(&s.archive_ready, memory_order_acquire));
#endif
        atomic_store_explicit(&s_capture_health, health, memory_order_release);
        if (nuwax_offline_capture_health_is_recording(health)) cfg.start_deadline_us = 0;
        if (health == NUWAX_OFFLINE_CAPTURE_UNAVAILABLE) {
            atomic_store_explicit(&s.capture_complete, false, memory_order_release);
            ESP_LOGW(TAG, "offline recording paused: storage unavailable session=%s next_sequence=%lu retained=%u rejected=%lu; saved archives kept",
                     cfg.session_id, (unsigned long)sequence,
                     (unsigned)sink->staging_count, (unsigned long)sink->rejected_frames);
            char detail[128];
            snprintf(detail, sizeof(detail), "next_seq=%lu;retained=%u;rejected=%lu;coverage=incomplete",
                     (unsigned long)sequence, (unsigned)sink->staging_count,
                     (unsigned long)sink->rejected_frames);
            (void)nuwax_device_warning_report_enqueue_detail(
                "audio", "offline_storage_paused", ESP_ERR_NOT_FINISHED, detail,
                monitoring ? "monitoring" : "meeting", 0U, cfg.session_id);
            nuwax_offline_audio_supervisor_wake();
            break;
        }
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
    /* Only this reader publishes its health. Do not erase failures when a
     * writer or supervisor happens to recover another archive generation. */
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    card_note_capture_stopped();
#endif
    if (atomic_load_explicit(&s_capture_health, memory_order_acquire) !=
            NUWAX_OFFLINE_CAPTURE_UNAVAILABLE) {
        atomic_store_explicit(&s_capture_health,
            offline_capture_cancelled(cfg.cancel_epoch) || transfer_ticket ||
                    nuwax_offline_audio_limit_reached()
                ? NUWAX_OFFLINE_CAPTURE_IDLE : NUWAX_OFFLINE_CAPTURE_UNAVAILABLE,
            memory_order_release);
    }
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (nuwax_sd_session_active()) {
        atomic_store(&s_card_finishing, true);
        if (!complete) atomic_store(&s_card_failed, true);
    }
#endif
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
    nuwax_offline_audio_supervisor_wake();
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
    const uint32_t cancel_epoch = atomic_load_explicit(
        &s_capture_cancel_epoch, memory_order_acquire);
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
    if (atomic_load_explicit(&s.media_recovery_requested, memory_order_acquire) ||
        !atomic_load_explicit(&s_media_ready, memory_order_acquire) ||
        recording_space_blocked() ||
        atomic_load_explicit(&s_mirror_closers, memory_order_acquire) != 0U ||
        atomic_load_explicit(&s_shutdown_requested, memory_order_acquire) ||
        nuwax_power_ctrl_shutdown_preparing()) {
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    cfg->start_deadline_us = nuwax_sd_session_active() ? s_card_start_deadline_us : 0;
#endif
    cfg->cancel_epoch = cancel_epoch;
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
    atomic_store_explicit(&s_capture_health, NUWAX_OFFLINE_CAPTURE_STARTING,
                          memory_order_release);
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
        atomic_store_explicit(&s_capture_health, NUWAX_OFFLINE_CAPTURE_UNAVAILABLE,
                              memory_order_release);
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
    atomic_fetch_add_explicit(&s_capture_cancel_epoch, 1U, memory_order_acq_rel);
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (atomic_load(&s_card_pending_upload)) return true;
#endif
    return atomic_load_explicit(&s.ready_count, memory_order_acquire) != 0U ||
           (atomic_load_explicit(&s_quarantine_recheck_pending, memory_order_acquire) &&
            atomic_load_explicit(&s_quarantine_files_present, memory_order_acquire));
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

#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE && CONFIG_NUWAX_SD_SESSION_ENABLE
static bool replay_shared_cancelled(void);
static size_t replay_shared_memory_reserve(void);
#endif

static esp_err_t replay_send_frame_buffered(nuwax_ws_client_t *ws,
                                    uint32_t epoch,
                                    const uint8_t *payload,
                                    size_t len,
                                    bool chunk, bool independent_transport,
                                    uint8_t *scratch, size_t scratch_capacity,
                                    nuwax_ws_send_metrics_t *metrics)
{
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (!atomic_load(&s_card_online)) return ESP_ERR_NOT_FINISHED;
#endif
    if (!ws || !payload || len == 0U || epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE && CONFIG_NUWAX_SD_SESSION_ENABLE
    /* Low-memory replay has no prefetch/wire allocation. It must still use
     * the same bounded TLS progress policy as the streaming formal sender. */
    nuwax_ws_send_metrics_t shared_metrics = {
        .shared_tls_archive = true, .cancelled = replay_shared_cancelled,
        .tcp_queue_ceiling_bytes = CONFIG_LWIP_TCP_SND_BUF_DEFAULT,
        .tcp_psram_reserve_bytes = replay_shared_memory_reserve() - REPLAY_TCP_HEADROOM_BYTES};
    if (chunk && !independent_transport && !metrics) metrics = &shared_metrics;
#endif
    esp_err_t err = ESP_ERR_TIMEOUT;
    const int64_t safe_retry_until = esp_timer_get_time() +
        (int64_t)REPLAY_SHARED_ADMISSION_RETRY_MS * 1000;
    for (uint32_t attempt = 1U; attempt <= REPLAY_SEND_RETRY_ATTEMPTS ||
             (metrics && metrics->shared_tls_archive && esp_timer_get_time() < safe_retry_until);
         ++attempt) {
        if (nuwax_offline_audio_storage_busy()) return ESP_ERR_NOT_FINISHED;
        if (metrics && metrics->cancelled && metrics->cancelled())
            return ESP_ERR_NOT_FINISHED;
        err = chunk
            ? (independent_transport
                ? nuwax_ws_send_archive_buffered_if_epoch(
                      ws, payload, len, REPLAY_ISOLATED_IO_TIMEOUT_MS, epoch,
                      scratch, scratch_capacity, metrics)
                : nuwax_ws_send_archive_buffered_if_epoch(
                      ws, payload, len, 250U, epoch, scratch, scratch_capacity, metrics))
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (!atomic_load(&s_card_online)) return ESP_ERR_NOT_FINISHED;
#endif
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (!atomic_load(&s_card_online)) return ESP_ERR_NOT_FINISHED;
#endif
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (sd_capture_admission_held(&s_capture_admission, offline_now_ms())) return true;
#endif
#if CONFIG_NUWAX_OTA_ENABLE
    /* A queued/accepted OTA stops replay at read/frame/ACK boundaries. The
     * OTA controller joins all replay workers before writing firmware. */
    if (nuwax_ota_device_is_active()) return true;
#endif
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (nuwax_sd_session_active()) {
        /* Both card modes upload only closed files. The writer/read DMA gate
         * and queue hysteresis protect the concurrently open recording tail. */
        return atomic_load(&s.media_recovery_requested) || atomic_load(&s.writer_error) ||
            (atomic_load(&s_card_finishing) && atomic_load(&s_card_failed)) ||
            atomic_load(&s_shutdown_requested) || !atomic_load(&s_media_ready);
    }
#endif
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

#if CONFIG_NUWAX_SD_SESSION_ENABLE
esp_err_t nuwax_sd_session_prepare_start(nuwax_offline_audio_mode_t mode, int64_t deadline_us)
{
    if ((unsigned)mode > 1U) return ESP_ERR_INVALID_ARG;
    if (!runtime_initialized()) return ESP_ERR_NOT_FINISHED;
    atomic_store_explicit(&s_offline_verify_cancel, true, memory_order_release);
    const int64_t now = esp_timer_get_time();
    if (deadline_us && deadline_us <= now) return ESP_ERR_TIMEOUT;
    /* The controller owns the execution deadline. A forgotten local caller
     * cannot suppress uploads indefinitely, nor renew a cloud command. */
    const int64_t until = deadline_us ? deadline_us : now + 5000000LL;
    /* Publish intent before trying the storage gate. A background evidence
     * exporter holding that gate can then observe the request and yield. */
    sd_capture_admission_hold(&s_capture_admission, (unsigned)mode, (uint32_t)(until / 1000LL));
    if (xSemaphoreTake(s.admission_lock, 0) != pdTRUE) return ESP_ERR_NOT_FINISHED;
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
    if (atomic_load_explicit(&s_formal_replay_running, memory_order_acquire)) {
        atomic_store_explicit(&s_formal_replay_cancel_requested, true, memory_order_release);
        if (s.events) (void)xEventGroupSetBits(s.events, REPLAY_ACK_BIT | REPLAY_CHUNK_ACK_BIT);
    }
#endif
    const bool running = nuwax_offline_audio_replay_running() || atomic_load(&s_offline_verify_running);
    xSemaphoreGive(s.admission_lock);
    return running ? ESP_ERR_NOT_FINISHED : ESP_OK;
}

void nuwax_sd_session_release_start(nuwax_offline_audio_mode_t mode)
{
    sd_capture_admission_release(&s_capture_admission, (unsigned)mode);
}
#endif

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
    const bool busy = s.file != NULL || nuwax_sd_session_active() || nuwax_offline_audio_capture_running() ||
                      nuwax_offline_audio_storage_busy() ||
                      nuwax_offline_audio_replay_running()
#if CONFIG_NUWAX_SD_SESSION_ENABLE
                      || atomic_load_explicit(&s_offline_verify_running, memory_order_acquire)
#endif
                      ;
    xSemaphoreGive(s.lock);
    xSemaphoreGive(s.writer_gate);
    xSemaphoreGive(s.admission_lock);
    return busy ? ESP_ERR_NOT_FINISHED : ESP_OK;
}

esp_err_t nuwax_offline_audio_cancel_shutdown(void)
{
    if (!enabled() || !runtime_initialized()) return ESP_OK;
    if (xSemaphoreTake(s.lock, 0) != pdTRUE) return ESP_ERR_NOT_FINISHED;
    /* Shutdown has already drained all owners; the barrier still prevents
     * new admission. Keep replay's cancel flag until its next start resets
     * it, so cancellation cannot resurrect an older transport generation. */
    atomic_store_explicit(&s_shutdown_requested, false, memory_order_release);
    xSemaphoreGive(s.lock);
    return ESP_OK;
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
        sd_audio_type_is(expected_type, SD_CHUNK_ACK);
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

static esp_err_t replay_ack_cancel_status(nuwax_ws_client_t *ws, uint32_t epoch,
                                          bool bulk_wait)
{
    if (!nuwax_ws_is_connected(ws) || nuwax_ws_connection_epoch(ws) != epoch)
        return ESP_ERR_INVALID_STATE;
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
    if (atomic_load_explicit(&s_formal_replay_cancel_requested, memory_order_acquire))
        return ESP_ERR_NOT_FINISHED;
#endif
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (!atomic_load(&s_card_online) || (bulk_wait && nuwax_sd_session_active()))
        return ESP_ERR_NOT_FINISHED;
#else
    (void)bulk_wait;
#endif
    if (nuwax_offline_audio_storage_busy()) return ESP_ERR_NOT_FINISHED;
    return ESP_OK;
}

static esp_err_t replay_wait_ack(nuwax_ws_client_t *ws,
                                 uint32_t epoch,
                                 const char *request_id,
                                 bool chunk,
                                  bool bulk_wait,
                                 uint32_t *next_offset)
{
    (void)ws;
    if (!request_id || !request_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    const EventBits_t wait_bit = chunk ? REPLAY_CHUNK_ACK_BIT : REPLAY_ACK_BIT;
    EventBits_t bits = 0U;
    portENTER_CRITICAL(&s_replay_lock);
    const uint32_t timeout_ms = replay_ack_timeout_ms(s.replay_ack.expected_type, bulk_wait);
    portEXIT_CRITICAL(&s_replay_lock);
    const uint32_t wait_started_ms = offline_now_ms();
    const uint32_t ack_deadline = wait_started_ms + timeout_ms;
    do {
        const esp_err_t cancelled = replay_ack_cancel_status(ws, epoch, bulk_wait);
        if (cancelled != ESP_OK) {
            replay_disarm_ack();
            return cancelled;
        }
#if defined(ESP_PLATFORM) && CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE
        nuwax_network_tx_retry_poll();
#endif
        bits = xEventGroupWaitBits(s.events, wait_bit, pdTRUE, pdFALSE,
                                   pdMS_TO_TICKS(100U));
    } while ((bits & wait_bit) == 0U &&
             !offline_deadline_elapsed(offline_now_ms(), ack_deadline));
    /* Cancellation/epoch wins even if its transition and the ACK wake this
     * task together. Never advance/delete an archive using a retired ACK. */
    const esp_err_t cancelled = replay_ack_cancel_status(ws, epoch, bulk_wait);
    if (cancelled != ESP_OK) {
        replay_disarm_ack();
        return cancelled;
    }
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
    const bool identity_matches = s.replay_ack.epoch == epoch &&
                    s.replay_ack.chunk == chunk &&
                    strcmp(s.replay_ack.request_id, request_id) == 0;
    const bool ok = identity_matches && s.replay_ack.ok;
    const uint32_t received_ms = s.replay_ack.received_ms;
    if (next_offset) {
        *next_offset = s.replay_ack.next_offset;
    }
    s.replay_ack.waiting = false;
    portEXIT_CRITICAL(&s_replay_lock);
    if (!ok) {
        ESP_LOGW(TAG,
                 "offline replay ACK %s kind=%s request=%s epoch=%lu",
                 identity_matches ? "peer refused" : "mismatch",
                 chunk ? "chunk" : "control",
                 request_id,
                 (unsigned long)epoch);
    }
    const uint32_t finished_ms = offline_now_ms();
    if (ok && finished_ms - wait_started_ms >= 100U) {
        ESP_LOGI(TAG, "offline ACK latency type=%s request=%s wait_ms=%lu rx_to_owner_ms=%lu",
                 s.replay_ack.expected_type, request_id, (unsigned long)(finished_ms - wait_started_ms),
                 (unsigned long)(finished_ms - received_ms));
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
        err = replay_arm_ack(epoch, request_id, sd_audio_type(s_replay_route, SD_CHUNK_ACK), segment_id);
        if (err != ESP_OK) return err;
        err = replay_send_frame(ws, epoch, chunk, bytes, true, independent_transport);
        if (err == ESP_OK) {
            err = replay_wait_ack(ws, epoch, request_id, true, false, acknowledged);
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
                                          long footer_offset,
                                          nuwax_ws_client_t *ws, uint32_t epoch,
                                          const atomic_bool *cancel)
{
    if (!file || !header || !footer ||
        footer_offset < (long)sizeof(archive_header_t) ||
        fseek(file, (long)sizeof(archive_header_t), SEEK_SET) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (footer->complete > 1U) return ESP_ERR_INVALID_SIZE;
    _Alignas(int16_t) uint8_t payload[MAX_FRAME_BYTES];
    uint32_t frame_count = 0U;
    uint64_t payload_bytes = 0U;
    uint32_t crc = 0U;
    uint32_t last_sequence = 0U;
    bool sequence_seen = false;
    size_t work_bytes = 0U;
    while (ftell(file) < footer_offset) {
        if ((cancel && atomic_load_explicit(cancel, memory_order_acquire)) ||
            (ws && (!nuwax_ws_is_connected(ws) || nuwax_ws_connection_epoch(ws) != epoch)))
            return ESP_ERR_INVALID_STATE;
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
        work_bytes += sizeof(length) + length;
        if (work_bytes >= 65536U) { work_bytes = 0U; vTaskDelay(1U); }
    }
    if ((cancel && atomic_load_explicit(cancel, memory_order_acquire)) ||
        (ws && (!nuwax_ws_is_connected(ws) || nuwax_ws_connection_epoch(ws) != epoch)))
        return ESP_ERR_INVALID_STATE;
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
static esp_err_t recheck_archive_path_once(const char *path, const sd_audio_segment_t *expected)
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
               (!expected || (uint64_t)st.st_size == expected->total_bytes) &&
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
            if (expected && (header.segment_index != expected->index || footer.crc32 != expected->payload_crc)) {
                (void)fclose(file); heap_caps_free(buffer); return ESP_ERR_INVALID_CRC;
            }
            const atomic_bool *cancel = NULL;
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
            /* Rechecks are replay work too; never hide a full-file scan from
             * the formal owner's cancellation/join deadline. */
            cancel = &s_formal_replay_cancel_requested;
#endif
            err = validate_archive_records(file, &header, &footer,
                (long)st.st_size - FOOTER_BYTES, NULL, 0U, cancel);
        }
    }
    if (ferror(file)) err = ESP_FAIL;
    if (fclose(file) != 0) err = ESP_FAIL;
    heap_caps_free(buffer);
    return err;
}

static esp_err_t recheck_archive_checked(const char *path, const sd_audio_segment_t *expected)
{
    esp_err_t err = recheck_archive_path_once(path, expected);
    if (err != ESP_ERR_INVALID_CRC && err != ESP_ERR_INVALID_SIZE) return err;
    /* This is background replay, with the existing per-record cancellation
     * checks. A whole 4 MiB single-sector scan needs more than the 1.5 s
     * per-slice budget; it never holds the capture writer's FILE or lock. */
    if (!nuwax_sd_bulk_read_recovery_begin(30000U)) return ESP_ERR_NOT_FINISHED;
    err = recheck_archive_path_once(path, expected);
    nuwax_sd_bulk_read_recovery_end(err == ESP_OK && expected != NULL);
    if (err == ESP_OK) ESP_LOGW(TAG,
        "SD archive independent validation recovered revision=218 path=%s original_index_crc=%u", path, expected != NULL);
    return err;
}

static esp_err_t recheck_archive_path(const char *path)
{
    return recheck_archive_checked(path, NULL);
}

static esp_err_t recheck_archive_expected(const sd_audio_segment_t *segment)
{
    return recheck_archive_checked(segment->path, segment);
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
/* Queue pressure yields SD reads, but must never cancel a valid ACK or TCP
 * connection. The writer can drain while RX and keepalive continue. */
static uint32_t replay_capture_queue_depth(void)
{
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (nuwax_sd_session_active() && s.queue) return uxQueueMessagesWaiting(s.queue);
#endif
    return 0U;
}
#include "sd_audio_scene_replay.inc"
#include "offline_replay_pressure.inc"
#include "offline_replay_read.inc"
#include "offline_replay_prefetch.inc"

static bool replay_bulk_cancelled(void)
{
    if (nuwax_offline_audio_storage_busy()) return true;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (!atomic_load(&s_card_online) || nuwax_sd_session_active()) return true;
#endif
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
    if (atomic_load_explicit(&s_formal_replay_cancel_requested, memory_order_acquire)) return true;
#endif
    return false;
}

static esp_err_t replay_send_windowed(nuwax_ws_client_t *ws, uint32_t epoch,
                                      FILE *file, const char *segment_id,
                                      uint32_t total, uint32_t resume,
                                      uint32_t window_limit,
                                      const archive_header_t *header,
                                      const archive_footer_t *footer, bool *allocation_deferred,
                                      const char *file_request)
{
    /* Typed SD files carry scene/index identity and are confirmed by commit_ack.
     * Legacy archives retain their original request-bound window ACKs. */
    const bool file_ack = file_request != NULL;
#ifdef ESP_PLATFORM
    nuwax_replay_schedule_init();
    uint32_t schedule_idle, schedule_ui;
    ESP_LOGI(TAG, "SD scheduler revision=147 help_priority=5 reader_handoff=0 sender_handoff=event idle_observer=%d byte_bound=65536 fairness_wait=idle_event cooperate_order=shared_wait help_lead_us=25000 help_pending=original_deadline",
             nuwax_replay_schedule_snapshot(&schedule_idle, &schedule_ui));
#endif
    if (allocation_deferred) *allocation_deferred = false;
    ESP_LOGI(TAG, "SD bulk recovery revision=138 ack_ms=%lu cancel_slice_ms=100",
             (unsigned long)replay_ack_timeout_ms(NULL, true));
    /* Bound PSRAM use and keep memory available for a new recording. */
    if (!file || !ws || !header || !footer || resume > total ||
        window_limit < REPLAY_CHUNK_MAX_PAYLOAD_BYTES) return ESP_ERR_INVALID_ARG;
    size_t capacity = window_limit < 128U * 1024U ? window_limit : 128U * 1024U;
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t tx_batch_frames = NUWAX_WS_ARCHIVE_BATCH_MAX_FRAMES;
    uint8_t *buffer = NULL;
    unsigned allocation_attempts = 0U, budget_rejects = 0U;
    unsigned allocation_failures = 0U, post_reserve_rejects = 0U;
    size_t free_last = free_psram, free_after = free_psram;
    size_t largest_rejected = 0U;
    while (capacity >= REPLAY_CHUNK_MAX_PAYLOAD_BYTES && !buffer) {
        allocation_attempts++;
        free_last = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const size_t bytes = 2U * capacity + tx_batch_frames * (REPLAY_CHUNK_MAX_PAYLOAD_BYTES + 142U);
        /* Re-evaluate each layout against live heap usage. A concurrent owner
         * may allocate after the snapshot, so a failed post-allocation reserve
         * check must also try a smaller v2 layout before returning to v1.
         * Prefer 64 KiB slots with four-frame TX over 128 KiB slots with
         * single-frame TX. The sealed SD file retains the ACK retry source;
         * large read slots must not consume the wire batching budget.
         * At smaller sizes both dimensions still decrease monotonically. */
        if (bytes + REPLAY_WORKER_HEADROOM_BYTES + REPLAY_PSRAM_RESERVE_BYTES <= free_last) {
            buffer = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (buffer) {
                free_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                if (free_after < REPLAY_PSRAM_RESERVE_BYTES + REPLAY_WORKER_HEADROOM_BYTES) {
                    post_reserve_rejects++;
                    heap_caps_free(buffer);
                    buffer = NULL;
                }
            } else {
                allocation_failures++;
            }
        } else {
            budget_rejects++;
        }
        if (!buffer) {
            /* Largest-block walks are diagnostic only, never in the hot path. */
            if (!largest_rejected && allocation_failures)
                largest_rejected = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
            if (capacity > 64U * 1024U) capacity /= 2U;
            else if (tx_batch_frames > 1U) --tx_batch_frames;
            else { capacity /= 2U; tx_batch_frames = NUWAX_WS_ARCHIVE_BATCH_MAX_FRAMES; }
        }
    }
    if (allocation_attempts > 1U || !buffer) {
        ESP_LOGI(TAG, "SD bulk allocation attempts=%u budget_rejects=%u allocation_failures=%u post_reserve_rejects=%u free_start=%u free_last=%u free_after=%u largest_rejected=%u capacity=%u tx_batch_frames=%u fallback=%u",
                 allocation_attempts, budget_rejects, allocation_failures, post_reserve_rejects,
                 (unsigned)free_psram, (unsigned)free_last, (unsigned)free_after,
                 (unsigned)largest_rejected, (unsigned)(buffer ? capacity : 0U),
                 (unsigned)(buffer ? tx_batch_frames : 0U), buffer ? 0U : 1U);
    }
    if (!buffer) {
        if (allocation_deferred) *allocation_deferred = true;
        return ESP_ERR_NO_MEM;
    }
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
    uint32_t send_wait_credits = 0U;
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
        file, ws, epoch, total, resume, capacity, buffer, header, footer, lane_probe, segment_id) : NULL;
    if (reader) position = resume;
    else if (err == ESP_OK) err = replay_read_rewind(file);
    /* The sealed SD file, not an ACK-retained RAM slot, is the retry source.
     * Recycle a slot only after its synchronous TCP send has consumed it.
     * On an interrupted/lost-ACK window retire this independent connection;
     * the next begin obtains the peer's durable offset before rereading.
     * Keep legacy serial/probe retries unchanged. No extra PSRAM is needed. */
    /* ACK bytes are bounded by the peer, independent of RAM slot count.
     * Smaller prefetch slots must not halve the negotiated durable window. */
    const size_t ack_slots = window_limit / capacity;
    const bool streaming_window = reader && !lane_probe && ack_slots > 1U;
    const size_t ack_capacity = streaming_window ? capacity * ack_slots : capacity;
    uint32_t acknowledged = resume, ack_end = resume;
    bool window_open = false;
    char request[48] = {0};
    if (file_ack) snprintf(request, sizeof(request), "%s", file_request);
    unsigned read_slot = 0U;
    nuwax_ws_send_metrics_t metrics = {.bounded_progress = true,
        .cancelled = replay_bulk_cancelled};
    offline_replay_budget_t budget = {0};
    uint32_t served_help = 0U, ui_handoffs = 0U;
    uint64_t ui_handoff_us = 0U;
    uint32_t fairness_idle_wakes = 0U, fairness_timer_wakes = 0U;
    uint32_t fairness_shared_ui = 0U, fairness_ui_pending = 0U;
    uint32_t fairness_ui_ready = 0U, fairness_ui_expired = 0U, fairness_ui_cancelled = 0U;
    uint64_t fairness_wait_us = 0U;
    (void)served_help; /* Host serial checks do not run the RTOS handoff. */
    replay_read_stats_t read_stats = {0};
    if (!reader && err == ESP_OK) replay_read_prepare(&read_stats);
    ESP_LOGI(TAG, "offline replay reader=%s buffers=2 capacity=%u publish_bytes=8192 handoff=event tx_layout=split reader_priority=%u sender_priority=%u lane_probe=%u tx_batch_frames=%u tx_batch_policy=ready_prefix", reader ? "streaming_prefetch" : "serial", (unsigned)capacity,
             (unsigned)(reader ? OFFLINE_REPLAY_READER_PRIORITY : uxTaskPriorityGet(NULL)),
             (unsigned)uxTaskPriorityGet(NULL), lane_probe ? 1U : 0U, (unsigned)tx_batch_frames);
    ESP_LOGI(TAG, "SD bulk pipeline revision=sd-backed-window-v4 layout=batch_first route=%u psram_reserve=%u free_before=%u ack_capacity=%u retry_source=%s target_KiBps=500",
             s_replay_route, (unsigned)REPLAY_PSRAM_RESERVE_BYTES, (unsigned)free_psram,
             (unsigned)ack_capacity, streaming_window ? "sealed_sd" : "retained_slot");
    ESP_LOGI(TAG, "SD bulk memory network_headroom=%u reader_headroom=%u",
             (unsigned)REPLAY_NETWORK_HEADROOM_BYTES, (unsigned)REPLAY_READER_HEADROOM_BYTES);
    while (err == ESP_OK && position < total) {
        if (replay_bulk_cancelled()) { err = ESP_ERR_NOT_FINISHED; break; }
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
                if (replay_bulk_cancelled()) { err = ESP_ERR_NOT_FINISHED; break; }
                size_t n = replay_read_slice_size(position + (uint32_t)loaded, count - loaded);
                const uint64_t pressure_before = pressure_us;
                err = replay_read_storage_enter(ws, epoch, NULL, &pressure_us);
                if (pressure_us != pressure_before) offline_replay_budget_relax(&budget);
                if (err != ESP_OK) break;
                uint64_t work_us = 0U;
                const uint64_t read_before = read_us;
                err = replay_read_validate(file, buffer + loaded, &n, &validator,
                    total, header, footer, &read_us, &validate_us, &work_us, &read_stats);
                replay_read_storage_leave();
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
        if (!file_ack && (!streaming_window || position == acknowledged)) {
            snprintf(request, sizeof(request), "offline_window_%08lx", (unsigned long)esp_random());
            const uint32_t remaining = total - position;
            ack_end = position + (streaming_window && remaining > ack_capacity
                ? (uint32_t)ack_capacity : streaming_window ? remaining : (uint32_t)count);
        }
        const bool window_final = position + count == (file_ack ? total : ack_end);
        const int64_t batch_started = esp_timer_get_time();
        const uint64_t batch_pressure_before = pressure_us;
        uint64_t last_ack_wait_us = 0U;
        for (unsigned attempt = 0; attempt < ((file_ack || streaming_window) ? 1U : REPLAY_CHUNK_ACK_ATTEMPTS); ++attempt) {
            if (!file_ack && (!streaming_window || position == acknowledged)) {
                err = replay_arm_ack(epoch, request, sd_audio_type(s_replay_route, SD_CHUNK_ACK), segment_id);
                window_open = err == ESP_OK;
            }
            if (err != ESP_OK) break;
            for (size_t sent = 0U; sent < count;) {
                if (replay_bulk_cancelled()) { err = ESP_ERR_NOT_FINISHED; break; }
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
                    value = window_final && sent + offset + frame_bytes == count ? 1U : 0U;
                    memcpy(prefix + 60U, &value, 4U);
                    if (file_ack) sd_scene_packet(prefix, header->session_id, header->segment_index,
                        position + sent + offset, (uint32_t)frame_bytes, value != 0U);
                    frames[frame_count] = (nuwax_ws_archive_frame_t){
                        prefix, data + sent + offset, frame_bytes};
                    offset += frame_bytes;
                }
                /* Each frame keeps its own mask/header; only TCP writes are
                 * coalesced. The archive slot is immutable through this send. */
                phase = esp_timer_get_time();
                const uint64_t packet_us = phase - packet_started;
                metrics.operation_wait_max_us = 0U;
                err = replay_send_batch(ws, epoch, frames, frame_count,
                                        wire, wire_capacity, &metrics);
                send_us += esp_timer_get_time() - phase;
                if (replay_bulk_cancelled()) err = ESP_ERR_NOT_FINISHED;
                if (err != ESP_OK) break;
                /* Decide fairness before waiting for a frame. The same blocked
                 * interval lets the decoder run AND preserves the byte/IDLE
                 * guard; two consecutive waits only spend its lease twice.
                 * No transport lock is held at this point. */
                bool fairness_waited = false;
                /* A real socket wait already yielded the CPU. Retain the
                 * byte/work guard for uninterrupted fast sends. */
                if (offline_replay_wait_relaxes(metrics.operation_wait_max_us,
                                               (uint64_t)portTICK_PERIOD_MS * 1000U)) {
                    offline_replay_budget_relax(&budget);
                    send_wait_credits++;
                } else if (offline_replay_budget_due(&budget, n, packet_us + metrics.encode_us - encode_before)) {
                    fairness_waited = true;
                    const int64_t fairness_started = esp_timer_get_time();
#ifdef ESP_PLATFORM
                    uint32_t before_generation, after_generation;
                    const nuwax_replay_ui_help_state_t before_help =
                        nuwax_replay_ui_help_snapshot(&before_generation, NULL);
                    if (before_help == NUWAX_REPLAY_UI_HELP_REQUESTED) {
                        fairness_shared_ui++;
                    }
                    if (nuwax_replay_schedule_yield()) fairness_idle_wakes++;
                    else fairness_timer_wakes++;
                    const nuwax_replay_ui_help_state_t after_help =
                        nuwax_replay_ui_help_snapshot(&after_generation, NULL);
                    /* Diagnostic outcomes only: none can waive fairness or
                     * revive an expired grant. Ignore replacement grants. */
                    if (before_help == NUWAX_REPLAY_UI_HELP_REQUESTED &&
                        before_generation == after_generation) {
                        fairness_ui_ready += after_help == NUWAX_REPLAY_UI_HELP_READY;
                        fairness_ui_expired += after_help == NUWAX_REPLAY_UI_HELP_EXPIRED;
                        fairness_ui_cancelled += after_help == NUWAX_REPLAY_UI_HELP_CANCELLED;
                        fairness_ui_pending += after_help == NUWAX_REPLAY_UI_HELP_REQUESTED;
                        if (after_help == NUWAX_REPLAY_UI_HELP_READY ||
                            after_help == NUWAX_REPLAY_UI_HELP_EXPIRED ||
                            after_help == NUWAX_REPLAY_UI_HELP_CANCELLED)
                            served_help = before_generation;
                    }
#else
                    vTaskDelay(1U);
                    fairness_timer_wakes++;
#endif
                    fairness_wait_us += esp_timer_get_time() - fairness_started;
                }
#ifdef ESP_PLATFORM
                /* When fairness is not due, an owner outcome wakes this one
                 * bounded help wait early. After a fairness wait, do not add
                 * another help wait in the same batch. A newly arriving grant
                 * remains unserved for the next batch; an unfinished grant
                 * can continue within its original deadline, never renew it. */
                if (!fairness_waited) {
                    const int64_t handoff_started = esp_timer_get_time();
                    if (nuwax_replay_ui_handoff(&served_help)) {
                        ui_handoffs++;
                        ui_handoff_us += esp_timer_get_time() - handoff_started;
                    }
                }
#endif
                (void)fairness_waited;
                tx_groups[frame_count - 1U]++;
                if (first_payload_us < 0) first_payload_us = esp_timer_get_time() - started;
                sent += n;
            }
            if (err != ESP_OK) { replay_disarm_ack(); break; }
            if (file_ack || !window_final) break;
            uint32_t confirmed = 0U;
            phase = esp_timer_get_time();
            err = replay_wait_ack(ws, epoch, request, true, true, &confirmed);
            last_ack_wait_us = esp_timer_get_time() - phase;
            ack_us += last_ack_wait_us;
            if (err == ESP_OK && confirmed != position + count) err = ESP_ERR_INVALID_RESPONSE;
            if (err == ESP_OK) window_open = false;
            if (streaming_window) break; /* RAM prefixes may already be reused. */
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
        position += count;
        if (!file_ack && window_final) acknowledged = position;
        if (file_ack || window_final) batches++;
        if (reader) {
            atomic_store_explicit(&reader->slots[read_slot].ready, false, memory_order_release);
            xSemaphoreGive(reader->space_signal);
            read_slot ^= 1U;
        }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        /* Hand a newly started monitor the sender after this RAM slot. A
         * partial protocol window retires the connection below; its sealed
         * SD source is resumed from the next begin ACK's durable offset. */
        if (position < total && nuwax_sd_session_active()) {
            s_card_preempted = true;
            err = ESP_ERR_NOT_FINISHED;
            break;
        }
#endif
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
        if (!file_ack && window_final && !offline_replay_wait_relaxes(last_ack_wait_us, (uint64_t)portTICK_PERIOD_MS * 1000U)) {
            vTaskDelay(1U);
            ack_yields++;
        } else if (!file_ack && window_final) {
            ack_yields_saved++;
        }
        if (!file_ack && window_final) offline_replay_budget_relax(&budget);
    }
    if (reader) {
        const esp_err_t reader_err = replay_prefetch_join(reader, err != ESP_OK, &read_us, &validate_us);
        if (err == ESP_OK) err = reader_err;
    }
    if ((file_ack && err != ESP_OK) || (streaming_window && window_open)) {
        replay_disarm_ack();
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        s_card_preempted = false; /* The scheduler must exit this retired connection. */
#endif
        /* Never start another transaction on a peer with an uncertain window.
         * Joining above guarantees no reader can access a retired transport. */
        (void)nuwax_ws_close_if_epoch(ws, epoch, NUWAX_WS_CLOSE_CAUSE_OWNERSHIP_TRANSFER);
        ESP_LOGW(TAG, "offline streaming window retained on SD confirmed=%lu sent_prefix=%lu err=%s",
                 (unsigned long)acknowledged, (unsigned long)position, esp_err_to_name(err));
    }
    const uint64_t elapsed = esp_timer_get_time() - started;
    const uint32_t transferred = file_ack ? (position > resume ? position - resume : 0U) : acknowledged - resume;
#if defined(ESP_PLATFORM) && CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE
    nuwax_network_tx_probe_log();
#endif
    ESP_LOGI(TAG, "offline window transfer segment=%s bytes=%lu batches=%lu capacity=%u read_validate_ms=%llu sd_read_ms=%llu validate_ms=%llu send_ms=%llu ack_ms=%llu elapsed_ms=%llu bytes_per_sec=%llu err=%s ack_scope=%s",
             segment_id, (unsigned long)transferred,
             (unsigned long)batches, (unsigned)capacity,
             (unsigned long long)((read_us + validate_us) / 1000U),
             (unsigned long long)(read_us / 1000U), (unsigned long long)(validate_us / 1000U),
             (unsigned long long)(send_us / 1000U),
             (unsigned long long)(ack_us / 1000U), (unsigned long long)(elapsed / 1000U),
             (unsigned long long)(elapsed ? (uint64_t)transferred*1000000U/elapsed : 0U),
             esp_err_to_name(err), file_ack ? "file_commit" : "window");
    ESP_LOGI(TAG, "offline send detail segment=%s calls=%lu short=%lu retries=%lu admission_ms=%llu io_lock_ms=%llu write_ms=%llu writable_wait_ms=%llu encode_ms=%llu write_bytes=%lu batch_calls=%lu batch_frames=%lu batch_max=%lu",
             segment_id, (unsigned long)metrics.write_calls, (unsigned long)metrics.short_writes,
             (unsigned long)metrics.retries, (unsigned long long)(metrics.admission_us / 1000U),
             (unsigned long long)(metrics.io_lock_us / 1000U), (unsigned long long)(metrics.write_us / 1000U),
             (unsigned long long)(metrics.writable_wait_us / 1000U), (unsigned long long)(metrics.encode_us / 1000U),
             (unsigned long)metrics.write_bytes, (unsigned long)metrics.batch_calls,
             (unsigned long)metrics.batch_frames, (unsigned long)metrics.batch_max_frames);
    ESP_LOGI(TAG, "offline send backpressure segment=%s writable_waits=%lu writable_timeouts=%lu writable_max_us=%llu timeout_overrun_ms=%llu timeout_overrun_max_us=%llu",
             segment_id, (unsigned long)metrics.writable_wait_calls,
             (unsigned long)metrics.writable_timeouts,
             (unsigned long long)metrics.writable_wait_max_us,
             (unsigned long long)(metrics.writable_timeout_overrun_us / 1000U),
             (unsigned long long)metrics.writable_timeout_overrun_max_us);
    ESP_LOGI(TAG, "offline mask detail word_bytes=%lu scalar_bytes=%lu copy_ms=%llu random_ms=%llu timing=wall_including_preemption",
             (unsigned long)metrics.mask_word_bytes, (unsigned long)metrics.mask_scalar_bytes,
             (unsigned long long)(metrics.mask_us / 1000U),
             (unsigned long long)(metrics.mask_random_us / 1000U));
    ESP_LOGI(TAG, "offline replay capture pressure sender_wait_ms=%llu",
             (unsigned long long)(pressure_us / 1000U));
    ESP_LOGI(TAG, "offline writable trace revision=148 after_timeout_progress=%lu blocked=%lu error=%lu accepted_bytes=%llu resume_max_us=%llu",
             (unsigned long)metrics.timeout_next_progress,
             (unsigned long)metrics.timeout_next_blocked,
             (unsigned long)metrics.timeout_next_error,
             (unsigned long long)metrics.timeout_next_bytes,
             (unsigned long long)metrics.timeout_resume_max_us);
    ESP_LOGI(TAG, "offline replay transport progress recoveries=%lu no_progress_max_ms=%llu",
        (unsigned long)metrics.progress_recoveries,
        (unsigned long long)(metrics.no_progress_max_us / 1000U));
    ESP_LOGI(TAG, "offline replay sender work yields=%lu serial_direct_frames=%lu serial_copied_bytes=%lu send_wait_credits=%lu idle_credits=%lu ui_credits=%lu",
             (unsigned long)budget.yields, (unsigned long)validator.direct_frames,
             (unsigned long)validator.copied_bytes, (unsigned long)send_wait_credits,
             (unsigned long)budget.idle_credits, (unsigned long)budget.ui_credits);
    ESP_LOGI(TAG, "offline replay sender UI handoffs=%lu wall_ms=%llu reader_handoff=0",
             (unsigned long)ui_handoffs, (unsigned long long)(ui_handoff_us / 1000U));
    ESP_LOGI(TAG, "offline replay fairness idle_wakes=%lu timer_wakes=%lu wait_ms=%llu byte_bound=65536 shared_ui=%lu ui_ready=%lu ui_expired=%lu ui_cancelled=%lu ui_pending=%lu",
             (unsigned long)fairness_idle_wakes, (unsigned long)fairness_timer_wakes,
             (unsigned long long)(fairness_wait_us / 1000U), (unsigned long)fairness_shared_ui,
             (unsigned long)fairness_ui_ready, (unsigned long)fairness_ui_expired,
             (unsigned long)fairness_ui_cancelled, (unsigned long)fairness_ui_pending);
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
#include "tls_archive_crypto.h"
#include "offline_replay_shared.inc"
#include "sd_audio_pending_commit.inc"

static esp_err_t replay_one_ready_file(nuwax_ws_client_t *ws,
                                  uint32_t epoch,
                                  const char *path,
                                  bool independent_transport, FILE *file, bool *file_closed)
{
    archive_header_t header = {0};
    archive_footer_t footer = {0};
    const int64_t metadata_started = esp_timer_get_time();
    bool catalog_reused = false;
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
                   footer.version != 1U || footer.footer_bytes != FOOTER_BYTES || footer.complete > 1U)) {
        valid = false;
    }
    if (!valid) {
        const bool media_error = ferror(file) != 0;
        if (media_error) {
            nuwax_sd_card_mark_io_fault(ESP_FAIL);
        }
        return validation_err != ESP_OK ? validation_err : ESP_ERR_INVALID_SIZE;
    }
    s_replay_route = SD_UPLOAD_LEGACY;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if ((header.reserved0 & 2U) != 0U) {
        if (!memchr(header.session_id, 0, sizeof(header.session_id))) return ESP_ERR_INVALID_SIZE;
        uint8_t owner[32];
        if (!card_owner(owner)) return ESP_ERR_INVALID_STATE;
        if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_NOT_FINISHED;
        esp_err_t bind_err = sd_audio_catalog_bind(header.session_id, header.segment_index,
            &s_card_replay_session, &s_card_replay_segment, &catalog_reused);
        xSemaphoreGive(s.lock);
        if (bind_err != ESP_OK) {
            ESP_LOGW(TAG, "SD file prepare failed stage=catalog_bind err=%s header_session=%.*s header_index=%lu path=%s",
                esp_err_to_name(bind_err), (int)sizeof(header.session_id), header.session_id,
                (unsigned long)header.segment_index, path);
            return bind_err;
        }
        if (memcmp(owner, s_card_replay_session.owner, 32) ||
            s_card_replay_session.done) return ESP_ERR_NOT_FINISHED;
        if (header.mode != s_card_replay_session.mode || !s_card_replay_segment.sealed ||
            s_card_replay_segment.missing) return ESP_ERR_INVALID_STATE;
        if (strcmp(path, s_card_replay_segment.path)) return ESP_ERR_INVALID_STATE;
        if (s_card_replay_segment.payload_crc != footer.crc32 ||
            s_card_replay_segment.total_bytes != (uint32_t)st.st_size) {
            /* A CRC-valid index can still describe stale/wrong audio. Repair
             * only after an independent full record/CRC scan, never from the
             * footer alone. Do not turn an index mismatch into audio damage. */
            nuwax_sd_card_identity_t before, after;
            if (!nuwax_sd_card_get_identity(&before)) return ESP_ERR_NOT_FINISHED;
            esp_err_t repaired = recheck_archive_path(path);
            if (repaired != ESP_OK) return repaired;
            if (nuwax_offline_audio_storage_busy() || !card_owner(owner) ||
                memcmp(owner, s_card_replay_session.owner, 32U) ||
                !nuwax_sd_card_get_identity(&after) || memcmp(&before, &after, sizeof(before)))
                return ESP_ERR_NOT_FINISHED;
            if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_NOT_FINISHED;
            bool changed = false;
            repaired = sd_audio_catalog_reconcile_file(&s_card_replay_session, &s_card_replay_segment, &changed);
            if (repaired == ESP_OK)
                repaired = sd_audio_catalog_bind(header.session_id, header.segment_index,
                    &s_card_replay_session, &s_card_replay_segment, &catalog_reused);
            xSemaphoreGive(s.lock);
            ESP_LOGW(TAG, "SD index recheck session=%s index=%lu repaired=%u err=%s",
                header.session_id, (unsigned long)header.segment_index, changed ? 1U : 0U, esp_err_to_name(repaired));
            if (repaired != ESP_OK) return repaired;
            if (s_card_replay_segment.acknowledged) return ESP_ERR_NOT_FINISHED;
            if (s_card_replay_segment.payload_crc != footer.crc32 ||
                s_card_replay_segment.total_bytes != (uint32_t)st.st_size ||
                strcmp(path, s_card_replay_segment.path)) return ESP_ERR_INVALID_STATE;
        }
        s_replay_route = !header.mode ? SD_UPLOAD_MEETING :
            (sd_audio_segment_realtime(&s_card_replay_session, &s_card_replay_segment, s_card_boot_id,
                (uint64_t)esp_timer_get_time() / 1000U, card_online_since_ms()) ? SD_UPLOAD_LIVE : SD_UPLOAD_HISTORY);
    }
#endif
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (s_replay_route != SD_UPLOAD_LEGACY) {
        /* Generated session IDs are 24 characters. Never truncate a corrupt
         * or foreign header into another file's idempotency identity. */
        if (!memchr(header.session_id, 0, sizeof(header.session_id)) ||
            !sd_audio_file_key(segment_id, header.session_id, header.segment_index))
            return ESP_ERR_INVALID_SIZE;
    }
#endif
    char request_id[48] = {0};
    uint32_t offset = 0U;
    esp_err_t err = ESP_OK;
    const bool monitoring_active =
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        nuwax_sd_session_active();
#else
        false;
#endif
    const bool bulk = replay_bulk_window_allowed(independent_transport, s_replay_route, monitoring_active);
    const bool file_ack = s_replay_route != SD_UPLOAD_LEGACY;
    const bool compact = bulk && replay_compact_protocol_allowed(independent_transport, s_replay_route);
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (file_ack) {
        err = card_scene_prepare(ws, epoch, independent_transport, &s_card_replay_session, &s_card_replay_segment, &offset);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SD file prepare failed stage=scene_begin err=%s session=%s index=%lu path=%s",
                esp_err_to_name(err), header.session_id, (unsigned long)header.segment_index, path);
            return err;
        }
    } else
#endif
    {
    uint32_t nonce = (uint32_t)esp_random();
    snprintf(request_id, sizeof(request_id), "offline_begin_%08lx",
             (unsigned long)nonce);
    cJSON *begin = cJSON_CreateObject();
    if (!begin) {
        return ESP_ERR_NO_MEM;
    }
    /* Keep the control JSON small.  The archive header is authoritative for
     * mode/session/audio parameters; this message only opens an identifiable,
     * resumable and integrity-checked byte transaction. */
    cJSON_AddStringToObject(begin, "type", sd_audio_type(s_replay_route, SD_BEGIN));
    /* Archive replay is storage-only.  The cloud must never route these bytes
     * through ASR, meeting transcription, or monitoring event detection. */
    if (s_replay_route == SD_UPLOAD_LEGACY) cJSON_AddStringToObject(begin, "mode", "archive_only");
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    else {
        cJSON_AddStringToObject(begin, "audio_session_id", header.session_id);
        cJSON_AddNumberToObject(begin, "segment_index", header.segment_index);
    }
#endif
    cJSON_AddStringToObject(begin, "request_id", request_id);
    if (s_replay_route == SD_UPLOAD_LEGACY) cJSON_AddStringToObject(begin, "segment_id", segment_id);
    cJSON_AddNumberToObject(begin, "total_bytes", total_bytes);
    cJSON_AddNumberToObject(begin, "crc32", footer.crc32);
    if (bulk && !file_ack) {
        if (!compact) {
            cJSON_AddNumberToObject(begin, "replay_version", 2U);
            cJSON_AddNumberToObject(begin, "replay_revision", 2U);
        }
        cJSON_AddNumberToObject(begin, "window_bytes", REPLAY_WINDOW_MAX_BYTES);
    }
    err = replay_arm_ack(epoch, request_id,
                                   sd_audio_type(s_replay_route, SD_BEGIN_ACK), segment_id);
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
    err = replay_wait_ack(ws, epoch, request_id, false, bulk, &offset);
    if (err != ESP_OK || offset > total_bytes) {
        ESP_LOGW(TAG,
                 "offline replay begin ACK failed segment=%s err=%s next_offset=%lu",
                 segment_id, esp_err_to_name(err), (unsigned long)offset);
        /* A bad peer offset must not quarantine a validated local archive. */
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }
    }
    portENTER_CRITICAL(&s_replay_lock);
    uint32_t window_limit = replay_bulk_window_allowed(independent_transport, s_replay_route,
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        nuwax_sd_session_active()
#else
        false
#endif
        ) ? (file_ack ? REPLAY_WINDOW_MAX_BYTES : s.replay_ack.window_bytes) : 0U;
    portEXIT_CRITICAL(&s_replay_lock);
    const bool compact_commit = !file_ack && compact && window_limit != 0U;
    ESP_LOGI(TAG, "offline replay start ack_scope=%s segment=%s resume=%lu buffered=%u compact=%u",
             file_ack ? "file_commit" : "legacy", segment_id,
             (unsigned long)offset, window_limit != 0U, compact_commit ? 1U : 0U);
    const int64_t payload_started = esp_timer_get_time();
    const uint32_t initial_offset = offset;
    bool shared_streamed = false;
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE && CONFIG_NUWAX_SD_SESSION_ENABLE
    if (file_ack && !independent_transport) {
        bool allocation_deferred = false;
        err = replay_send_shared_file(ws, epoch, file, segment_id, total_bytes,
            offset, &header, &footer, &allocation_deferred);
        if (err == ESP_ERR_NO_MEM && allocation_deferred) {
            ESP_LOGI(TAG, "SD formal prefetch reserve unavailable; single-pass bounded sender revision=216");
            err = replay_send_shared_bounded_file(ws, epoch, file, segment_id, total_bytes,
                offset, &header, &footer);
        }
        if (err != ESP_OK) return err;
        offset = total_bytes;
        shared_streamed = true;
    }
#endif
    if (window_limit) {
        bool allocation_deferred = false;
        err = replay_send_windowed(ws, epoch, file, segment_id, total_bytes, offset,
                                  window_limit, &header, &footer, &allocation_deferred,
                                  file_ack ? header.session_id : NULL);
        if (err == ESP_ERR_NO_MEM && allocation_deferred) {
            /* No data was sent. Use small buffers while preserving the
             * same file transaction and acknowledgement rules. */
            ESP_LOGW(TAG, "SD bulk allocation deferred; using bounded sender segment=%s ack_scope=%s",
                     segment_id, file_ack ? "file_commit" : "chunk");
            window_limit = 0U;
        } else {
            if (err != ESP_OK) return err;
            offset = total_bytes;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
            if (file_ack) card_scene_sent(offset);
#endif
        }
    }
    if (!window_limit && !shared_streamed) {
        /* The lightweight sender validates before streaming. */
        const atomic_bool *cancel = NULL;
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
        if (!independent_transport) cancel = &s_formal_replay_cancel_requested;
#endif
        err = validate_archive_records(file, &header, &footer,
            (long)total_bytes - FOOTER_BYTES, ws, epoch, cancel);
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
    offline_replay_budget_t serial_budget = {0};
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
        uint32_t version = file_ack ? 2U : 1U;
        uint32_t payload_offset = offset;
        uint32_t payload_len = (uint32_t)got;
        memcpy(chunk + 8U, &version, sizeof(version));
        memcpy(chunk + 12U, &payload_offset, sizeof(payload_offset));
        memcpy(chunk + 16U, &payload_len, sizeof(payload_len));
        if (!file_ack) snprintf(request_id, sizeof(request_id), "offline_chunk_%08lx",
                               (unsigned long)esp_random());
        /* Legacy packets carry request_id; scene packets below replace it
         * with audio_session_id and segment_index. */
        memset(chunk + 20U, 0, 40U);
        memcpy(chunk + 20U, request_id,
               strlen(request_id) < 40U ? strlen(request_id) : 40U);
        memset(chunk + 60U, 0, 4U);
        if (file_ack) sd_scene_packet(chunk, header.session_id, header.segment_index,
            offset, (uint32_t)got, offset + (uint32_t)got == total_bytes);
        uint32_t acknowledged = 0U;
        if (file_ack) {
#if CONFIG_NUWAX_SD_SESSION_ENABLE
            if (!independent_transport && offset == initial_offset && header.mode == NUWAX_OFFLINE_AUDIO_MODE_MONITORING)
                s_replay_route = sd_audio_segment_realtime(&s_card_replay_session,
                    &s_card_replay_segment, s_card_boot_id,
                    (uint64_t)esp_timer_get_time() / 1000U, card_online_since_ms())
                        ? SD_UPLOAD_LIVE : SD_UPLOAD_HISTORY;
#endif
            err = replay_send_frame(ws, epoch, chunk, got + 64U, true, independent_transport);
            /* Sent is not durable: keep the source until commit_ack. Any
             * interrupted transfer obtains its durable prefix from begin. */
            if (err == ESP_OK) acknowledged = offset + (uint32_t)got;
        } else {
            err = replay_exchange_chunk(ws, epoch, chunk, got + 64U,
                                         request_id, segment_id, independent_transport,
                                         &acknowledged);
        }
        const uint32_t sent_end = offset + (uint32_t)got;
        if (err != ESP_OK || acknowledged <= offset ||
            acknowledged > sent_end) {
            ESP_LOGW(TAG,
                     "offline replay data transfer failed segment=%s offset=%lu err=%s next_offset=%lu",
                     segment_id, (unsigned long)offset, esp_err_to_name(err),
                     (unsigned long)acknowledged);
            err = err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
            break;
        }
        offset = acknowledged;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        if (file_ack) card_scene_sent(offset);
        if (s_replay_route != SD_UPLOAD_LIVE && card_live_pending(ws, epoch)) {
            s_card_preempted = true;
            err = ESP_ERR_NOT_FINISHED;
            break;
        }
#endif
        /* 1 ms rounds to zero at 100 Hz. Bound uninterrupted work by bytes,
         * while shared-link admission is still rechecked before every frame. */
        if (offline_replay_budget_due(&serial_budget, got, 0U)) vTaskDelay(1U);
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
    cJSON *commit = sd_audio_commit_json(s_replay_route, request_id, segment_id,
        header.session_id, header.segment_index, total_bytes, footer.crc32, compact_commit);
    if (!commit) {
        return ESP_ERR_NO_MEM;
    }
    err = replay_arm_ack(epoch, request_id,
                         sd_audio_type(s_replay_route, SD_COMMIT_ACK), segment_id);
    if (err == ESP_OK) {
        err = replay_send_json(ws, epoch, commit, independent_transport);
    }
    cJSON_Delete(commit);
    const int64_t commit_sent = esp_timer_get_time();
    if (err == ESP_OK) {
        err = replay_wait_ack(ws, epoch, request_id, false, bulk, NULL);
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
    ESP_LOGI(TAG, "offline commit timing request=%s send_ms=%lu ack_wait_ms=%lu err=%s",
             request_id, (unsigned long)((commit_sent - commit_started) / 1000U),
             (unsigned long)((cleanup_started - commit_sent) / 1000U), esp_err_to_name(err));
    fclose(file);
    *file_closed = true;
    if (err == ESP_OK) {
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        if (s_replay_route != SD_UPLOAD_LEGACY) card_commit_stage();
#endif
    }
    /* A new recording may start while commit waits for its ACK. Keep the
     * local file until idle; begin/commit is idempotent on the next pass. */
    if (err == ESP_OK && nuwax_offline_audio_storage_busy()) {
        return ESP_ERR_NOT_FINISHED;
    }
    if (err == ESP_OK) {
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        if (s_replay_route != SD_UPLOAD_LEGACY) {
            if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_NOT_FINISHED;
            const esp_err_t saved = sd_audio_catalog_ack(header.session_id, header.segment_index);
            xSemaphoreGive(s.lock);
            if (saved != ESP_OK) return saved;
            card_commit_saved();
            card_scene_file_done();
        }
#endif
        if (remove(path) != 0) {
            ESP_LOGW(TAG,
                     "offline segment commit acknowledged but delete failed segment=%s errno=%d",
                     segment_id, errno);
            return ESP_FAIL;
        }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        if (s_replay_route != SD_UPLOAD_LEGACY && sd_audio_catalog_drop_backup(path) != ESP_OK)
            ESP_LOGW(TAG, "SD confirmed audio removed; local metadata cleanup deferred path=%s", path);
#endif
        if (!replay_path_quarantined(path) &&
#if CONFIG_NUWAX_SD_SESSION_ENABLE
            s_replay_route == SD_UPLOAD_LEGACY &&
#endif
            atomic_load_explicit(&s.ready_count, memory_order_acquire) > 0U) {
            (void)atomic_fetch_sub_explicit(&s.ready_count, 1U,
                                            memory_order_acq_rel);
        }
        atomic_store_explicit(&s_directory_dirty, true, memory_order_release);
        if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
#if CONFIG_NUWAX_SD_SESSION_ENABLE
            if (s_replay_route != SD_UPLOAD_LEGACY)
                sd_audio_catalog_note_deleted(header.session_id, header.segment_index);
#endif
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
        ESP_LOGI(TAG, "offline replay phases metadata_ms=%lu begin_ms=%lu payload_ms=%lu commit_ms=%lu cleanup_ms=%lu catalog_reused=%u",
                 (unsigned long)((begin_started - metadata_started) / 1000U),
                 (unsigned long)((payload_started - begin_started) / 1000U),
                 (unsigned long)((commit_started - payload_started) / 1000U),
                 (unsigned long)((cleanup_started - commit_started) / 1000U),
                 (unsigned long)((esp_timer_get_time() - cleanup_started) / 1000U), catalog_reused ? 1U : 0U);
        ESP_LOGI(TAG, "offline segment acknowledged and deleted segment=%s bytes=%lu",
                 segment_id, (unsigned long)total_bytes);
        const uint64_t full_us = esp_timer_get_time() - metadata_started;
        ESP_LOGI(TAG, "SD file confirmed route=%s bytes=%lu transferred=%lu total_ms=%llu attempt_bytes_per_sec=%llu",
            independent_transport ? "independent" : "formal", (unsigned long)total_bytes,
            (unsigned long)(total_bytes - initial_offset), (unsigned long long)(full_us / 1000U),
            (unsigned long long)(full_us ? (uint64_t)(total_bytes - initial_offset) * 1000000U / full_us : 0U));
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
#if defined(ESP_PLATFORM) && CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
    const uint32_t recovery_token = independent_transport ? 0U :
        nuwax_ws_archive_recovery_begin(ws, epoch);
#endif
    esp_err_t err = replay_one_ready_file(ws, epoch, path,
                                               independent_transport, file, &file_closed);
#if defined(ESP_PLATFORM) && CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
    nuwax_sd_tcp_recovery_metrics_t recovery = {0};
    nuwax_sd_tcp_recovery_end(recovery_token, &recovery);
    if (!independent_transport)
        ESP_LOGI(TAG, "SD formal TCP recovery revision=204 enabled=%u outputs=%lu nomem=%lu retries=%lu recovered=%lu exhausted=%lu post_failures=%lu stale=%lu zero_window=%lu rto_samples=%lu nrtx_peak=%lu queue_peak=%lu send_free_min=%lu cwnd_min=%lu",
            recovery_token != 0U, (unsigned long)recovery.outputs, (unsigned long)recovery.output_nomem,
            (unsigned long)recovery.retries, (unsigned long)recovery.recovered, (unsigned long)recovery.exhausted,
            (unsigned long)recovery.post_failures, (unsigned long)recovery.stale, (unsigned long)recovery.zero_window,
            (unsigned long)recovery.rto_samples, (unsigned long)recovery.nrtx_peak, (unsigned long)recovery.queue_peak,
            (unsigned long)recovery.send_free_min, (unsigned long)recovery.cwnd_min);
    if (!independent_transport && recovery_token)
        ESP_LOGI(TAG, "SD formal TCP flow revision=210 ack_events=%lu acked_bytes=%llu peer_window_min=%lu peer_window_max=%lu queued_peak=%lu cwnd_min=%lu cwnd_max=%lu inflight_peak=%lu ack_rtt_samples=%lu ack_rtt_avg_us=%llu ack_rtt_min_us=%lu ack_rtt_max_us=%lu",
            (unsigned long)recovery.ack_events, (unsigned long long)recovery.acked_bytes,
            (unsigned long)recovery.peer_window_min, (unsigned long)recovery.peer_window_max,
            (unsigned long)recovery.queued_bytes_peak, (unsigned long)recovery.cwnd_min,
            (unsigned long)recovery.cwnd_max, (unsigned long)recovery.inflight_peak,
            (unsigned long)recovery.rtt_samples,
            (unsigned long long)(recovery.rtt_samples ? recovery.rtt_total_us / recovery.rtt_samples : 0U),
            (unsigned long)recovery.rtt_min_us, (unsigned long)recovery.rtt_max_us);
#endif
    if (!file_closed) fclose(file);
    heap_caps_free(buffer);
    /* Recheck a high-speed CRC/size failure only after ordered slow remount,
     * never quarantine a possibly transient bus error at the same speed. */
    if (nuwax_sd_card_fallback_after_read_error(err) ||
        (err != ESP_OK && !nuwax_sd_card_is_mounted())) {
        atomic_store_explicit(&s_media_ready, false, memory_order_release);
        atomic_store_explicit(&s_replay_media_fault, true, memory_order_release);
        notify_writer_task();
        nuwax_offline_audio_supervisor_wake();
        err = ESP_FAIL;
    }
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        if (strstr(entry->d_name, "/card_") || !strncmp(entry->d_name, "card_", 5)) continue;
#endif
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

#include "sd_audio_session_replay.inc"

static esp_err_t replay_ready_impl(nuwax_ws_client_t *ws,
                                   uint32_t expected_epoch,
                                   bool independent_transport)
{
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (!atomic_load(&s_card_online)) return ESP_ERR_NOT_FINISHED;
#endif
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
        replay_ota_active() ||
        atomic_load_explicit(&s.media_recovery_requested, memory_order_acquire)) {
        atomic_store_explicit(&s.replay_running, false,
                              memory_order_release);
        return ESP_ERR_NOT_FINISHED;
    }
    esp_err_t result = ESP_OK;
    char after_name[128] = {0};
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    result = card_commit_flush();
    if (result != ESP_OK) {
        card_commit_release_idle();
        atomic_store_explicit(&s.replay_running, false, memory_order_release);
        return result;
    }
    while (xSemaphoreTake(s.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        result = replay_ack_cancel_status(ws, expected_epoch, false);
        if (result != ESP_OK) {
            atomic_store_explicit(&s.replay_running, false, memory_order_release);
            return result;
        }
        vTaskDelay(1U);
    }
    sd_audio_catalog_replay_reset();
    result = sd_audio_catalog_repair_reset();
    s_card_pass_error = ESP_OK;
    xSemaphoreGive(s.lock);
    if (result != ESP_OK) {
        atomic_store_explicit(&s.replay_running, false, memory_order_release);
        return result;
    }
#endif
    for (;;) {
 #if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
        if (atomic_load_explicit(&s_formal_replay_cancel_requested,
                                 memory_order_acquire)) {
            result = result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
            break;
        }
 #endif
#if defined(ESP_PLATFORM) && CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
        /* File/scene selection boundary: previous TX, reader, leases and ACK
         * persistence have finished. Never change owner/core mid-TLS frame. */
        if (!independent_transport && replay_formal_core_handoff()) {
            ESP_LOGI(TAG, "SD replay core handoff revision=202 from=%u to=%u; connection retained",
                (unsigned)xPortGetCoreID(), replay_formal_target_core());
            result = ESP_ERR_NOT_FINISHED; break;
        }
#endif
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        bool card_work = false;
        const esp_err_t card_err = card_replay_next(ws, expected_epoch, independent_transport, &card_work);
        if (card_err != ESP_OK) { result = card_err; break; }
        if (card_work) continue;
        if (s_card_pass_error != ESP_OK) { result = s_card_pass_error; break; }
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
                ESP_LOGI(TAG, "offline replay yielded to recording, storage or OTA; archive retained");
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
    /* No frame/reader is left in flight here. Release before publishing the
     * owner as idle, including cancellation, storage handoff and transport loss. */
#ifdef ESP_PLATFORM
    nuwax_tls_archive_crypto_release();
#endif
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    /* Close suspended repair DIRs before publishing this transport owner
     * idle. Cancellation never carries a native FILE/DMA operation forward. */
    while (xSemaphoreTake(s.lock, pdMS_TO_TICKS(100U)) != pdTRUE) vTaskDelay(1U);
    const esp_err_t repair_closed = sd_audio_catalog_repair_reset();
    xSemaphoreGive(s.lock);
    if (result == ESP_OK && repair_closed != ESP_OK) result = repair_closed;
    card_commit_release_idle();
#endif
    atomic_store_explicit(&s.replay_running, false, memory_order_release);
    return result;
}

#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
static uint32_t s_formal_retry_backoff_ms;
static void formal_replay_task(void *arg)
{
    (void)arg;
    const formal_replay_context_t context = s_formal_replay_context;
    atomic_store_explicit(&s_formal_worker_identity, (uintptr_t)xTaskGetCurrentTaskHandle(), memory_order_release);
    ESP_LOGI(TAG, "SD replay worker routing revision=202 core=%u capture=%u",
        context.core, atomic_load_explicit(&s.capture_running, memory_order_acquire) ? 1U : 0U);
    const esp_err_t result = replay_ready_impl(context.ws, context.epoch, false);
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    /* Our ACK/cursor writes belong to this pass. Only a later catalog change
     * may bypass its retry deadline, e.g. a newly sealed file in the other lane. */
    s_replay_launch_revision = sd_audio_catalog_live_revision();
#endif
    if (result == ESP_OK) s_formal_retry_backoff_ms = 0;
    else if (result != ESP_ERR_NOT_FINISHED) s_formal_retry_backoff_ms =
        s_formal_retry_backoff_ms == 0 ? 1000U : (s_formal_retry_backoff_ms < 16000U ? s_formal_retry_backoff_ms * 2U : 30000U);
    s_replay_launch_deadline_ms = offline_now_ms() + s_formal_retry_backoff_ms;
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
        s_replay_launch_deadline_ms = offline_now_ms() + 100U;
        atomic_store_explicit(&s_replay_retry_required, true,
                              memory_order_release);
    }
    atomic_store_explicit(&s_formal_worker_identity, 0U, memory_order_release);
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
    /* Server support is deployed. The authenticated connection owner schedules
     * begin/chunk/commit directly; no SD-specific hello or capability ACK. */
    if (!nuwax_offline_audio_has_ready() ||
        atomic_load_explicit(&s_formal_replay_running, memory_order_acquire)) {
        return ESP_OK;
    }
    const uint32_t launch_now = offline_now_ms();
    uint32_t revision = 0U;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    revision = sd_audio_catalog_live_revision();
#endif
    if (!atomic_load_explicit(&s_media_ready, memory_order_acquire) ||
        atomic_load_explicit(&s_replay_media_fault, memory_order_acquire) ||
        !nuwax_sd_card_is_mounted() ||
        !nuwax_audio_uplink_background_ready() ||
        (s_replay_launch_epoch == expected_epoch && revision == s_replay_launch_revision &&
         !offline_deadline_elapsed(launch_now, s_replay_launch_deadline_ms))) {
        return ESP_ERR_NOT_FINISHED;
    }
    s_replay_launch_epoch = expected_epoch;
    s_replay_launch_revision = revision;
    s_replay_launch_deadline_ms = launch_now + 5000U;
    bool expected_running = false;
    if (!atomic_compare_exchange_strong_explicit(
            &s_formal_replay_running, &expected_running, true,
            memory_order_acq_rel, memory_order_acquire)) {
        return ESP_OK;
    }
    if (replay_ota_active() || atomic_load_explicit(&s.media_recovery_requested,
                             memory_order_acquire)) {
        atomic_store_explicit(&s_formal_replay_running, false,
                              memory_order_release);
        return ESP_ERR_NOT_FINISHED;
    }
    atomic_store_explicit(&s_formal_replay_cancel_requested, false,
                          memory_order_release);
    s_formal_replay_context.ws = ws;
    s_formal_replay_context.epoch = expected_epoch;
    s_formal_replay_context.core = replay_formal_target_core();
    if (xTaskCreatePinnedToCoreWithCaps(formal_replay_task,
                            "offline_replay_formal",
                            OFFLINE_REPLAY_FORMAL_TASK_STACK_BYTES,
                            NULL,
                            OFFLINE_REPLAY_FORMAL_TASK_PRIORITY,
                            NULL,
                            (BaseType_t)s_formal_replay_context.core,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        atomic_store_explicit(&s_formal_replay_running, false,
                              memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "offline formal replay scheduled epoch=%lu stack_bytes=%u core=%u priority=%u",
             (unsigned long)expected_epoch,
             (unsigned)OFFLINE_REPLAY_FORMAL_TASK_STACK_BYTES,
             s_formal_replay_context.core,
             (unsigned)OFFLINE_REPLAY_FORMAL_TASK_PRIORITY);
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
/* Test-only routing identity travels in the URL, not a replay hello. The
 * formal cloud continues to use its existing authenticated device identity. */
#if CONFIG_NUWAX_SD_GATEWAY_DISCOVERY_ENABLE
static char s_replay_test_gateway_url[256];
#endif
static esp_err_t replay_test_build_url(char *url, size_t capacity)
{
    const char *device_id = nuwax_device_uid_string();
    if (!url || capacity == 0U || !device_id || device_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    /* The cached eFuse-derived UID uses URL-safe uppercase hex and hyphens. */
    const char *base = CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_URL;
#if CONFIG_NUWAX_SD_GATEWAY_DISCOVERY_ENABLE
    base = s_replay_test_gateway_url;
#endif
    if (!base[0]) return ESP_ERR_INVALID_STATE;
    const int written = snprintf(url, capacity, "%s%cdevice_id=%s",
                                 base, strchr(base, '?') ? '&' : '?', device_id);
    if (written <= 0 || (size_t)written >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void replay_test_on_text(const char *payload, int len, void *user_ctx)
{
    nuwax_ws_client_t *ws = (nuwax_ws_client_t *)user_ctx;
    const uint32_t epoch = ws ? nuwax_ws_connection_epoch(ws) : 0U;
    (void)nuwax_offline_audio_handle_protocol_text(payload, len, epoch);
}

static bool replay_test_connect_cancelled(void *ctx)
{
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (!atomic_load(&s_card_online)) return true;
#endif
    (void)ctx;
    return nuwax_power_ctrl_shutdown_preparing() ||
           nuwax_offline_audio_storage_busy() || replay_ota_active();
}

static void replay_test_task(void *arg)
{
    (void)arg;
    replay_checkpoint_invalidate();
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
    if (CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_TOKEN[0] == '\0'
#if !CONFIG_NUWAX_SD_GATEWAY_DISCOVERY_ENABLE
        || CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_URL[0] == '\0'
#endif
        ) {
        ESP_LOGW(TAG, "offline replay test disabled at runtime; URL/token missing");
        goto done;
    }

    char replay_url[384];
    const nuwax_ws_config_t config = {
        .url = replay_url,
        .token = CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_TOKEN,
        .on_text = replay_test_on_text,
        .on_binary = NULL,
        .on_rx_activity = NULL,
        .on_handshake_verified = NULL,
        .connect_cancelled = replay_test_connect_cancelled,
        .tcp_connect_timeout_ms = 3000U,
        .local_connect_diagnostics_only = true,
        .plain_event_rx = true,
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
#if CONFIG_NUWAX_SD_GATEWAY_DISCOVERY_ENABLE
        err = nuwax_sd_gateway_discover(s_replay_test_gateway_url, sizeof(s_replay_test_gateway_url),
                                        replay_test_connect_cancelled, NULL);
        if (err != ESP_OK) break;
#endif
        err = replay_test_build_url(replay_url, sizeof(replay_url));
        if (err != ESP_OK) break;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        card_scene_reset();
#endif
        err = nuwax_ws_connect(replay_ws, &config);
        const bool connect_failed = err != ESP_OK;
        if (replay_test_connect_cancelled(NULL)) err = ESP_ERR_NOT_FINISHED;
        if (err == ESP_OK) {
            const uint32_t epoch = nuwax_ws_connection_epoch(replay_ws);
            do {
                err = replay_ready_impl(replay_ws, epoch, true);
#if CONFIG_NUWAX_SD_SESSION_ENABLE
                if (err == ESP_OK && nuwax_sd_session_active() && atomic_load(&s_card_online) &&
                    nuwax_offline_audio_has_ready()) {
                    vTaskDelay(pdMS_TO_TICKS(50U));
                    continue;
                }
#endif
                break;
            } while (nuwax_ws_is_connected(replay_ws) && !replay_test_connect_cancelled(NULL));
            ESP_LOGI(TAG,
                     "offline replay test attempt=%lu finished err=%s epoch=%lu stack_min_free=%u",
                     (unsigned long)attempt,
                     esp_err_to_name(err),
                     (unsigned long)epoch,
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
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
        delay_ms = delay_ms == 0U ? 1000U : (delay_ms < 16000U ? delay_ms * 2U : 30000U);
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
    ESP_LOGI(TAG, "offline replay test scheduled discovery=%d fallback_url=%s",
#if CONFIG_NUWAX_SD_GATEWAY_DISCOVERY_ENABLE
             1,
#else
             0,
#endif
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

/* Normal replay yields to an OTA reservation; an unreaped transport cannot.
 * Keep replay_running strict for storage owners and the OTA join below. */
bool nuwax_offline_audio_replay_blocks_ota(void) {
    if (!enabled() || !runtime_initialized()) return false;
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE
    return atomic_load_explicit(&s_replay_test_transport_quarantined, memory_order_acquire);
#else
    return false;
#endif
}

esp_err_t nuwax_offline_audio_replay_prepare_ota(uint32_t timeout_ms) {
    if (!replay_ota_active()) return ESP_ERR_INVALID_STATE;
    if (nuwax_offline_audio_replay_blocks_ota()) return ESP_ERR_INVALID_STATE;
    const uint32_t started = offline_now_ms();
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    /* Synchronize with a creator that checked OTA before it became active.
     * Do not return ready until its running flag/task publication is visible.
     * The OTA-active flag prevents any later admission; joining is outside. */
    if (runtime_initialized()) {
        if (!s.admission_lock ||
            xSemaphoreTake(s.admission_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
            return ESP_ERR_TIMEOUT;
        atomic_store_explicit(&s_offline_verify_cancel, true, memory_order_release);
        xSemaphoreGive(s.admission_lock);
    } else {
        atomic_store_explicit(&s_offline_verify_cancel, true, memory_order_release);
    }
#endif
    const bool was_running = nuwax_offline_audio_replay_running()
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        || atomic_load_explicit(&s_offline_verify_running, memory_order_acquire)
#endif
        ;
    if (was_running) ESP_LOGI(TAG, "OTA waiting for SD replay release; uncommitted archives retained");
    while (nuwax_offline_audio_replay_running()
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        || atomic_load_explicit(&s_offline_verify_running, memory_order_acquire)
#endif
        ) {
        if (nuwax_offline_audio_replay_blocks_ota()) return ESP_ERR_INVALID_STATE;
        if ((uint32_t)(offline_now_ms() - started) >= timeout_ms) {
            ESP_LOGW(TAG, "OTA SD replay release timed out; firmware write blocked");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20U));
    }
    if (was_running) ESP_LOGI(TAG, "OTA SD replay released elapsed_ms=%lu",
                             (unsigned long)(offline_now_ms() - started));
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
    cJSON *root = nuwax_json_parse_with_length(payload, len);
    if (!root) {
        return false;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *request = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    const bool is_ack = cJSON_IsString(type) && cJSON_IsString(request) &&
                        (sd_audio_type_is(type->valuestring, SD_BEGIN_ACK) ||
                         sd_audio_type_is(type->valuestring, SD_CHUNK_ACK) ||
                         sd_audio_type_is(type->valuestring, SD_COMMIT_ACK) ||
                         sd_audio_end_ack(type->valuestring));
    if (!is_ack) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *next = cJSON_GetObjectItemCaseSensitive(root, "next_offset");
    const bool legacy = strncmp(type->valuestring, "offline_audio_", 14) == 0;
    const bool scene_begin = !legacy && sd_audio_type_is(type->valuestring, SD_BEGIN_ACK);
    const cJSON *segment = cJSON_GetObjectItemCaseSensitive(root,
        (scene_begin || sd_audio_end_ack(type->valuestring)) ? "audio_session_id" : "segment_id");
    char file_key[48] = {0};
    const char *ack_key = cJSON_IsString(segment) ? segment->valuestring : NULL;
    if (!legacy && !scene_begin && !sd_audio_end_ack(type->valuestring)) {
        const cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "audio_session_id");
        const cJSON *index = cJSON_GetObjectItemCaseSensitive(root, "segment_index");
        const double number = cJSON_IsNumber(index) ? index->valuedouble : -1.0;
        ack_key = cJSON_IsString(session) && number >= 0.0 && number < 1000000.0 &&
            number == (double)(uint32_t)number &&
            sd_audio_file_key(file_key, session->valuestring, (uint32_t)number) ? file_key : NULL;
    }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "replay_version");
    const cJSON *window = cJSON_GetObjectItemCaseSensitive(root, "window_bytes");
    const bool requires_offset =
        (legacy && sd_audio_type_is(type->valuestring, SD_BEGIN_ACK)) || sd_audio_type_is(type->valuestring, SD_CHUNK_ACK);
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
            (legacy && !cJSON_IsString(segment)) ||
            (ack_key && strcmp(s.replay_ack.segment_id, ack_key) == 0);
        if (epoch_matches && type_matches && segment_matches && valid_offset && cJSON_IsBool(ok) &&
            (!scene_begin || !cJSON_IsTrue(ok) || sd_scene_parse_resume(root, &s.replay_ack.scene))) {
            s.replay_ack.received_ms = offline_now_ms();
            s.replay_ack.ok = cJSON_IsTrue(ok);
            s.replay_ack.scene.error = cJSON_IsTrue(ok) ? SD_SCENE_RETRY : sd_scene_error(root);
            if (legacy && replay_window_ack_type(type->valuestring) &&
                ((cJSON_IsNumber(version) && version->valuedouble == 2.0) ||
                 (!version && replay_compact_protocol_allowed(true,
                     strncmp(type->valuestring, "offline_audio_", 14) ? SD_UPLOAD_MEETING : SD_UPLOAD_LEGACY))) &&
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
    if (accepted && !cJSON_IsTrue(ok)) {
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "error_code");
        if (!cJSON_IsString(reason)) reason = cJSON_GetObjectItemCaseSensitive(root, "error");
        char safe_reason[128];
        const char *source = cJSON_IsString(reason) ? reason->valuestring : "unspecified";
        size_t n = 0;
        for (; source[n] && n + 1U < sizeof(safe_reason); ++n)
            safe_reason[n] = (unsigned char)source[n] < 32U ? '?' : source[n];
        safe_reason[n] = '\0';
        ESP_LOGW(TAG, "offline replay peer refused type=%s request=%s reason=%s",
                 type->valuestring, request->valuestring, safe_reason);
    }
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
                 ack_key ? ack_key : "-");
    }
    cJSON_Delete(root);
    return true;
}

#include "sd_audio_offline_verify.inc"
