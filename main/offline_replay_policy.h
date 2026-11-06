#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sd_audio_protocol.h"

/* Leave room for the 768-frame recording staging ring (~489 KiB), plus
 * >256 KiB for capture startup, networking and control. Internal DMA/task
 * reserves are independent and must not be borrowed by the uploader. */
#define REPLAY_PSRAM_RESERVE_BYTES (768U * 1024U)
#define REPLAY_READER_HEADROOM_BYTES 16384U
#define REPLAY_SHARED_ADMISSION_RETRY_MS 2500U
/* Additional Wi-Fi cache slots are allocated on demand. Budget their peak
 * growth before choosing read slots, instead of borrowing the recording
 * reserve later. 2 KiB per extra slot covers a full Ethernet frame plus
 * allocation metadata; the original 16-slot profile retains its old floor. */
#if CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM > 16
#define REPLAY_NETWORK_HEADROOM_BYTES ((CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM - 16U) * 2048U)
#else
#define REPLAY_NETWORK_HEADROOM_BYTES 0U
#endif
/* Larger TCP limits allocate pbufs on demand. Reserve their EXTRA peak before
 * choosing reader slots, including metadata, instead of borrowing capture RAM
 * after the queue fills. The previous 45 KiB socket budget is already covered. */
#if CONFIG_LWIP_TCP_SND_BUF_DEFAULT > 46080
#define REPLAY_TCP_HEADROOM_BYTES ((CONFIG_LWIP_TCP_SND_BUF_DEFAULT - 46080U) + \
    ((CONFIG_LWIP_TCP_SND_BUF_DEFAULT - 46080U + 1439U) / 1440U) * 256U)
#else
#define REPLAY_TCP_HEADROOM_BYTES 0U
#endif
#define REPLAY_WORKER_HEADROOM_BYTES (REPLAY_READER_HEADROOM_BYTES + REPLAY_NETWORK_HEADROOM_BYTES + REPLAY_TCP_HEADROOM_BYTES)

/* Credit at most one already allocated recording ring. A closing ring and a
 * new capture can coexist; never credit both or borrow the control reserve. */
static inline size_t replay_shared_reserve_bytes(size_t held_rings, size_t ring_bytes)
{
    const size_t maximum_credit = REPLAY_PSRAM_RESERVE_BYTES - 256U * 1024U;
    const size_t credit = held_rings ? (ring_bytes < maximum_credit ? ring_bytes : maximum_credit) : 0U;
    return REPLAY_PSRAM_RESERVE_BYTES - credit + REPLAY_WORKER_HEADROOM_BYTES;
}

/* Keep live monitoring's previous short frame lease. History can occupy one
 * full TLS record without increasing the legacy/realtime sender's stack. */
static inline size_t replay_shared_payload_limit(bool latency_sensitive)
{
#if CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN > 1024
    const size_t record = CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN;
#else
    const size_t record = 1024U;
#endif
    const size_t frame = latency_sensitive && record > 3968U ? 3968U : record;
    const size_t payload = frame - 64U - 8U;
    return payload < 8064U ? payload : 8064U;
}

typedef struct { bool fast; } replay_shared_governor_t;
/* Decide only between complete WS frames. The fixed wire maximum and TLS
 * inner-content allowance are unchanged. Hysteresis protects the writer. */
static inline size_t replay_shared_payload_admit(replay_shared_governor_t *g,
    bool monitoring, uint32_t queued, size_t internal_free, uint32_t wake_backlog,
    size_t crypto_capacity)
{
    if (!monitoring) g->fast = true;
    else {
        if (g->fast && (queued >= 48U || internal_free < 16384U || wake_backlog >= 60U)) g->fast = false;
        if (!g->fast && queued <= 32U && internal_free >= 20480U && wake_backlog <= 30U)
            g->fast = true;
    }
    size_t limit = replay_shared_payload_limit(monitoring && !g->fast);
    /* WS may span TLS records. A 4K accelerator does not require 4K WS. */
    if (crypto_capacity && crypto_capacity < 1025U) return 0U;
    return limit;
}

/* Eight 8064-byte history frames fit in 63 KiB. A 64 KiB read slot would
 * force a ninth 1 KiB TLS record at every boundary. Keep sector alignment;
 * small-memory and live-monitor profiles retain their existing geometry. */
static inline size_t replay_shared_read_capacity(size_t capacity, bool latency_sensitive)
{
    const size_t quantum = 8064U * 4U; /* 63 SD sectors, four history frames. */
    if (!latency_sensitive && replay_shared_payload_limit(false) == 8064U && capacity >= quantum)
        return capacity / quantum * quantum;
    return capacity;
}

static inline uint32_t replay_ack_timeout_ms(const char *type, bool bulk_wait)
{
    /* File-level confirmation also applies to live monitoring and the
     * bounded sender. Do not tie its deadline to bulk cancellation policy. */
    if (type && strncmp(type, "offline_audio_", 14) != 0 &&
        (sd_audio_type_is(type, SD_BEGIN_ACK) || sd_audio_type_is(type, SD_COMMIT_ACK)))
        return 6000U;
    /* A long monitoring scene can have tens of thousands of index records.
     * Only its final durable summary gets this budget; cancellation and
     * connection checks still run every 100 ms. Only independent historical
     * bulk transactions get TCP recovery time; live/serial ACKs stay short. */
    return type && (!strcmp(type, "meeting_audio_session_end_ack") ||
        !strcmp(type, "monitoring_audio_session_end_ack")) ? 60000U :
        bulk_wait ? 6000U : 2500U;
}

static inline bool replay_bulk_window_allowed(bool independent, unsigned route,
                                               bool monitoring_active)
{
    /* Resource policy depends on active recording, not the capture-origin
     * label. Queued online files can use the bulk path after capture stops. */
    (void)route;
    return independent && !monitoring_active;
}

static inline bool replay_window_ack_type(const char *type)
{
    return sd_audio_type_is(type, SD_BEGIN_ACK);
}

/* Paired PC receiver only. Formal-cloud and legacy transactions retain their
 * original fields until their receivers explicitly adopt the compact form. */
static inline bool replay_compact_protocol_allowed(bool independent, unsigned route)
{
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE
    return independent && route != SD_UPLOAD_LEGACY;
#else
    (void)independent; (void)route;
    return false;
#endif
}
