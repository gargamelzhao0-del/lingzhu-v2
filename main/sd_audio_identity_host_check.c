/* Host-only: actual start admission plus actual durable catalog, real files. */
#include <assert.h>
#include <stdatomic.h>
#include "sd_audio_file_fault.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "sd_audio_session.h"
#include "sd_audio_catalog.h"
#include "board/sd_card_driver.h"

static struct { atomic_bool active, closing; int lock; } s;
static struct { atomic_bool pending; bool identity_valid, retry_probe, skip_retry; nuwax_sd_card_identity_t identity; } s_card_io_recovery;
static atomic_bool s_space_blocked, s_monitor_space_blocked;
static atomic_bool s_mirror_closers, s_card_active, s_card_created, s_card_finishing, s_card_failed;
static atomic_bool s_card_partial;
static atomic_uint s_capture_recovery_until;
static sd_audio_damage_window_t s_card_damage;
static atomic_int s_card_mode;
static sd_audio_session_t s_card_session;
static char s_card_mirror_session[48];
static sd_audio_session_time_t s_card_time;
static bool s_card_stopped, available = true, live;
bool nuwax_sd_card_get_identity(nuwax_sd_card_identity_t *identity) { memset(identity,0,sizeof(*identity));return available; }
static uint64_t s_card_stopped_ms;
static uint32_t s_card_boot_id = 7, random_value;
static unsigned starts, lock_depth, supervisor_wakes;
static int64_t now_us = 123456000, lock_delay_us;
static int s_card_network_lock;
static bool lock_ready = true, owner_ready = true;
static esp_err_t capture_result = ESP_OK;
static char captured_id[48];
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
static int xSemaphoreTake(int lock, unsigned ms)
{ (void)lock; (void)ms; if (!lock_ready) return 0; assert(!lock_depth); ++lock_depth;
  now_us += lock_delay_us; lock_delay_us = 0; return 1; }
static void xSemaphoreGive(int lock) { (void)lock; assert(lock_depth == 1); --lock_depth; }
static size_t strlcpy(char *dst, const char *src, size_t size)
{ const size_t n = strlen(src); assert(n < size); memcpy(dst, src, n + 1); return n; }
static uint32_t esp_random(void) { return ++random_value; }
static int64_t esp_timer_get_time(void) { return now_us; }
void nuwax_offline_audio_supervisor_wake(void) { ++supervisor_wakes; }
static bool card_owner(uint8_t owner[32]) { memset(owner, 0x31, 32); return owner_ready; }
bool nuwax_sd_session_available(void) { return available; }
bool nuwax_sd_session_active(void) { return atomic_load(&s_card_active); }
bool nuwax_offline_audio_capture_running(void) { return live; }
esp_err_t nuwax_offline_audio_start_capture(nuwax_offline_audio_mode_t mode,
    const char *id, uint32_t rate, uint8_t channels, uint16_t duration)
{
    assert(mode == 0 || mode == 1); assert(rate == 16000 && channels == 1 && duration == 20);
    assert(lock_depth == 1 && nuwax_sd_session_active());
    ++starts; strcpy(captured_id, id); return capture_result;
}
#include "sd_identity_start_host.inc"
#include "sd_identity_async_host.inc"

static void durable_finish(void)
{
    assert(sd_audio_catalog_create(&s_card_session) == ESP_OK);
    s_card_session.ended = s_card_session.done = 1;
    assert(sd_audio_catalog_save(&s_card_session) == ESP_OK);
    atomic_store(&s_card_active, false);
}

int main(void)
{
    assert(sd_audio_catalog_init() == ESP_OK);
    assert(nuwax_sd_session_check_new_identity(NULL) == ESP_ERR_INVALID_ARG);
    assert(nuwax_sd_session_check_new_identity("new_scene") == ESP_OK);
    available = false;
    assert(nuwax_sd_session_check_new_identity("new_scene") == ESP_ERR_NOT_FINISHED);
    available = true; lock_ready = false;
    assert(nuwax_sd_session_check_new_identity("new_scene") == ESP_ERR_NOT_FINISHED);
    lock_ready = true;
    atomic_store(&s_card_active, true); strcpy(s_card_session.session_id, "New_Scene");
    assert(nuwax_sd_session_check_new_identity("new_scene") == ESP_ERR_INVALID_STATE);
    assert(nuwax_sd_session_check_new_identity("other_scene") == ESP_OK);
    atomic_store(&s_card_active, false);
    assert(!sd_audio_identity_valid(NULL) && !sd_audio_identity_valid(""));
    assert(sd_audio_identity_valid("A-z_09"));
    char maximum[48] = "12345678901234567890123456789012345";
    char too_long[48] = "123456789012345678901234567890123456";
    assert(sd_audio_identity_valid(maximum) && !sd_audio_identity_valid(too_long));
    char unterminated[48]; memset(unterminated, 'a', sizeof(unterminated));
    assert(!sd_audio_identity_valid(unterminated));
    assert(nuwax_sd_session_start(0, NULL) == ESP_ERR_INVALID_ARG);
    assert(nuwax_sd_session_start(0, too_long) == ESP_ERR_INVALID_ARG);
    assert(nuwax_sd_session_start(0, unterminated) == ESP_ERR_INVALID_ARG);
    char id[48] = "Cloud_scene-001";
    assert(nuwax_sd_session_start(9, id) == ESP_ERR_INVALID_ARG);
    assert(nuwax_sd_session_start(0, id) == ESP_OK);
    assert(!strcmp(id, "Cloud_scene-001") && !strcmp(captured_id, id));
    assert(!strcmp(s_card_session.session_id, id) && starts == 1);
    assert(s_card_session.start_ms == 123456 && s_card_session.boot_id == 7);
    durable_finish();
    sd_audio_session_t saved, reloaded;
    assert(sd_audio_catalog_load(id, &saved) == ESP_OK);
    assert(nuwax_sd_session_start(0, id) == ESP_ERR_INVALID_STATE && starts == 1);
    assert(!atomic_load(&s_card_active) && !lock_depth);
    /* Clear volatile state as at reboot; completed durable scene still owns ID. */
    memset(&s_card_session, 0, sizeof(s_card_session)); ++s_card_boot_id;
    assert(sd_audio_catalog_init() == ESP_OK);
    assert(nuwax_sd_session_start(1, id) == ESP_ERR_INVALID_STATE && starts == 1);
    assert(sd_audio_catalog_load(id, &reloaded) == ESP_OK && !memcmp(&saved, &reloaded, sizeof(saved)));
    assert(nuwax_sd_session_start(1, maximum) == ESP_OK && !strcmp(captured_id, maximum));
    durable_finish();
    /* Empty input is reserved for local starts; two offline scenes differ. */
    char local_a[48] = "", local_b[48] = "";
    assert(nuwax_sd_session_start(0, local_a) == ESP_OK && sd_audio_identity_valid(local_a));
    durable_finish();
    random_value = 0; /* Force the first random candidate to collide. */
    assert(nuwax_sd_session_start(0, local_b) == ESP_OK && strcmp(local_a, local_b));
    durable_finish();
    const unsigned before = starts;
    const char *suffix[] = {"creating", "index", "time", "blocked", "session.bad.73"};
    for (unsigned i = 0; i < sizeof(suffix)/sizeof(suffix[0]); ++i) {
        char path[128]; snprintf(id, sizeof(id), "orphan_%u", i);
        snprintf(path, sizeof(path), "catalog/%s.%s", id, suffix[i]);
        FILE *file = fopen(path, "wb"); assert(file); fputs("retained", file); assert(fclose(file) == 0);
        assert(nuwax_sd_session_start(0, id) == ESP_ERR_INVALID_STATE && starts == before);
        file = fopen(path, "rb"); char data[16] = {0}; assert(file);
        assert(fread(data, 1, 8, file) == 8 && !strcmp(data, "retained")); fclose(file);
        assert(!atomic_load(&s_card_active) && !lock_depth);
    }
    strcpy(id, "failure_cleanup");
    available = false; assert(nuwax_sd_session_start(0, id) == ESP_ERR_NOT_FINISHED); available = true;
    lock_ready = false; assert(nuwax_sd_session_start(0, id) == ESP_ERR_NOT_FINISHED); lock_ready = true;
    owner_ready = false; assert(nuwax_sd_session_start(0, id) == ESP_ERR_INVALID_STATE); owner_ready = true;
    assert(!atomic_load(&s_card_active) && starts == before);
    capture_result = ESP_FAIL; assert(nuwax_sd_session_start(0, id) == ESP_FAIL);
    assert(!atomic_load(&s_card_active) && !lock_depth && starts == before + 1);
    capture_result = ESP_OK;
    const unsigned async_starts = starts;
    int64_t deadline = now_us + 20000000;
    strcpy(id, "async_scene");
    assert(nuwax_sd_session_poll_identity(0, id, deadline) == ESP_ERR_NOT_FINISHED);
    assert(supervisor_wakes && !lock_depth && !atomic_load(&s_card_active));
    assert(nuwax_sd_session_start_until(0, id, deadline) == ESP_ERR_NOT_FINISHED);
    assert(starts == async_starts && !atomic_load(&s_card_active));
    card_identity_service();
    assert(nuwax_sd_session_poll_identity(0, id, deadline) == ESP_OK);
    assert(nuwax_sd_session_start_until(0, id, deadline) == ESP_OK);
    assert(starts == async_starts + 1 && s_card_start_deadline_us == deadline);
    durable_finish();
    /* Slow catalog response is checked after I/O and cannot start a new MIC. */
    strcpy(id, "slow_preflight"); deadline = now_us + 20000000;
    assert(nuwax_sd_session_poll_identity(0, id, deadline) == ESP_ERR_NOT_FINISHED);
    lock_delay_us = 21000000;
    card_identity_service();
    assert(nuwax_sd_session_poll_identity(0, id, deadline) == ESP_ERR_TIMEOUT);
    assert(nuwax_sd_session_start_until(0, id, deadline) == ESP_ERR_TIMEOUT);
    assert(starts == async_starts + 1 && !atomic_load(&s_card_active));
    /* A previous query's result cannot be used for a replacement request. */
    deadline = now_us + 20000000;
    assert(nuwax_sd_session_poll_identity(0, "replacement", deadline) == ESP_ERR_NOT_FINISHED);
    assert(nuwax_sd_session_poll_identity(1, "async_scene", deadline) == ESP_ERR_NOT_FINISHED);
    card_identity_service();
    assert(nuwax_sd_session_poll_identity(0, "replacement", deadline) == ESP_OK);
    assert(nuwax_sd_session_poll_identity(1, "async_scene", deadline) == ESP_ERR_INVALID_STATE);
    strcpy(id, "replacement");
    lock_delay_us = 21000000;
    assert(nuwax_sd_session_start_until(0, id, deadline) == ESP_ERR_TIMEOUT);
    assert(starts == async_starts + 1 && !atomic_load(&s_card_active) && !lock_depth);
    puts("PASS: actual SD start/catalog and async worker: IDs, collisions, durable reuse guard, queued preflight, slow I/O deadline, start-lock deadline, mode isolation and failure cleanup");
    return 0;
}
