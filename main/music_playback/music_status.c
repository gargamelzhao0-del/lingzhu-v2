#include "music_status.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "session_protocol.h"

#define MUSIC_STATUS_SLOTS 8U
typedef struct {
    bool used, ready, terminal;
    nuwax_ws_client_t *ws;
    uint32_t epoch, revision, attempts;
    int64_t retry_us, expires_us;
    char id[80], state[16], reason[48];
} music_status_entry_t;
static music_status_entry_t *s_entries;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static const char *TAG = "music_status";
static void status_task(void *unused) {
    (void)unused;
    while (true) {
        nuwax_ws_client_t *ws = NULL;
        portENTER_CRITICAL(&s_status_lock);
        for (unsigned i = 0; i < MUSIC_STATUS_SLOTS; ++i)
            if (s_entries[i].used) { ws = s_entries[i].ws; break; }
        portEXIT_CRITICAL(&s_status_lock);
        nuwax_music_status_service(ws);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t nuwax_music_status_init(void) {
    if (s_entries) return ESP_OK;
    s_entries = heap_caps_calloc(MUSIC_STATUS_SLOTS, sizeof(*s_entries),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_entries) return ESP_ERR_NO_MEM;
    TaskHandle_t task = NULL;
    if (xTaskCreateWithCaps(status_task, "music_status", 6144U, NULL, 2U, &task,
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) return ESP_OK;
    heap_caps_free(s_entries); s_entries = NULL;
    return ESP_ERR_NO_MEM;
}
static music_status_entry_t *find_entry(nuwax_ws_client_t *ws, uint32_t epoch, const char *id) {
    for (unsigned i = 0; i < MUSIC_STATUS_SLOTS; ++i)
        if (s_entries[i].used && s_entries[i].ws == ws && s_entries[i].epoch == epoch &&
            !strcmp(s_entries[i].id, id)) return &s_entries[i];
    return NULL;
}
bool nuwax_music_status_reserve(nuwax_ws_client_t *ws, uint32_t epoch, const char *id) {
    if (!s_entries || !ws || !epoch || !id || !id[0] || strlen(id) >= 80U) return false;
    portENTER_CRITICAL(&s_status_lock);
    music_status_entry_t *entry = find_entry(ws, epoch, id);
    if (!entry) {
        for (unsigned i = 0; i < MUSIC_STATUS_SLOTS; ++i) {
            if (s_entries[i].used) continue;
            entry = &s_entries[i];
            /* Revision is never reset: an in-flight send must not retire a reused slot. */
            uint32_t revision = entry->revision + 1U;
            memset(entry, 0, sizeof(*entry));
            entry->used = true; entry->ws = ws; entry->epoch = epoch;
            entry->revision = revision;
            strlcpy(entry->id, id, sizeof(entry->id));
            break;
        }
    }
    portEXIT_CRITICAL(&s_status_lock);
    return entry != NULL;
}
void nuwax_music_status_post(nuwax_ws_client_t *ws, uint32_t epoch,
                             const char *id, const char *state, const char *reason) {
    if (!s_entries || !id || !state) return;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_status_lock);
    music_status_entry_t *entry = find_entry(ws, epoch, id);
    if (entry) {
        entry->ready = true;
        entry->terminal = strcmp(state, "playing") != 0;
        entry->revision++; entry->attempts = 0;
        entry->retry_us = now; entry->expires_us = now + 15000000LL;
        strlcpy(entry->state, state, sizeof(entry->state));
        strlcpy(entry->reason, reason ? reason : "", sizeof(entry->reason));
    }
    portEXIT_CRITICAL(&s_status_lock);
    if (!entry) ESP_LOGE(TAG, "unreserved music status request=%s", id);
}
void nuwax_music_status_service(nuwax_ws_client_t *ws) {
    if (!s_entries || !ws) return;
    const uint32_t epoch = nuwax_ws_connection_epoch(ws);
    const bool connected = nuwax_ws_is_connected(ws);
    const int64_t now = esp_timer_get_time();
    music_status_entry_t send = {0};
    unsigned slot = MUSIC_STATUS_SLOTS;
    portENTER_CRITICAL(&s_status_lock);
    for (unsigned i = 0; i < MUSIC_STATUS_SLOTS; ++i) {
        music_status_entry_t *entry = &s_entries[i];
        if (!entry->used) continue;
        if (entry->ws != ws || entry->epoch != epoch || !connected) {
            entry->used = false; entry->revision++; continue;
        }
        if (slot == MUSIC_STATUS_SLOTS && entry->ready && now >= entry->retry_us) {
            slot = i; send = *entry;
        }
    }
    portEXIT_CRITICAL(&s_status_lock);
    if (slot == MUSIC_STATUS_SLOTS) return;
    const esp_err_t err = now >= send.expires_us ? ESP_ERR_TIMEOUT :
        nuwax_session_send_music_playback_status_until_if_epoch(
            ws, send.id, send.state, send.reason[0] ? send.reason : NULL,
            esp_timer_get_time() + 200000LL, epoch);
    bool expired = false;
    portENTER_CRITICAL(&s_status_lock);
    music_status_entry_t *entry = &s_entries[slot];
    if (entry->used && entry->revision == send.revision) {
        expired = esp_timer_get_time() >= entry->expires_us;
        if (err == ESP_OK || expired) {
            entry->ready = false;
            /* A playing reservation is retained for its eventual terminal state. */
            if (entry->terminal) entry->used = false;
        } else {
            entry->attempts++;
            unsigned shift = entry->attempts > 3U ? 3U : entry->attempts;
            entry->retry_us = esp_timer_get_time() + (250000LL << shift);
        }
    }
    portEXIT_CRITICAL(&s_status_lock);
    if (expired && err != ESP_OK)
        ESP_LOGW(TAG, "music status delivery expired request=%s state=%s", send.id, send.state);
}
