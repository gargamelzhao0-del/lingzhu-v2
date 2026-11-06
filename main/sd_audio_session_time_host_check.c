#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sd_audio_catalog.c"
#include "sd_audio_scene_protocol.h"

static uint64_t fake_ms;
static bool active, held;
static int s_card_network_lock;
static bool nuwax_sd_session_active(void) { return active; }
static int64_t esp_timer_get_time(void) { assert(!held); return (int64_t)fake_ms * 1000; }
static void enter(int *lock) { (void)lock; assert(!held); held = true; }
static void leave(int *lock) { (void)lock; assert(held); held = false; }
#define portENTER_CRITICAL(lock) enter(lock)
#define portEXIT_CRITICAL(lock) leave(lock)
#include "sd_stop_time_host.inc"

static const uint64_t utc = UINT64_C(1792634400000);
static sd_audio_session_t scene(const char *id, uint32_t boot, uint64_t start)
{
    sd_audio_session_t s = {.boot_id = boot, .start_ms = start};
    snprintf(s.session_id, sizeof(s.session_id), "%s", id);
    memset(s.owner, 0x12, sizeof(s.owner));
    assert(sd_audio_catalog_create(&s) == ESP_OK);
    return s;
}
static sd_audio_session_time_t resolve(sd_audio_session_t *s, uint32_t boot,
    const nuwax_cloud_time_anchor_t *anchor, bool stopped, uint64_t stop)
{
    sd_audio_session_time_t t;
    assert(sd_audio_catalog_session_time(s, boot, anchor, stopped, stop, &t) == ESP_OK);
    return t;
}
static void corrupt_slot(const char *file, size_t offset)
{
    FILE *f = fopen(file, "r+b"); assert(f);
    assert(!fseek(f, (long)offset, SEEK_SET));
    assert(fputc(0x5a, f) != EOF); assert(!fclose(f));
}
int main(void)
{
    nuwax_cloud_time_anchor_t snapshot;
    assert(!nuwax_cloud_time_snapshot(&snapshot));
    assert(!nuwax_cloud_time_observe(1234567, 1000));
    assert(nuwax_cloud_time_observe(utc, 1000));
    assert(!nuwax_cloud_time_observe(utc + 10, 999));
    assert(nuwax_cloud_time_snapshot(&snapshot) && snapshot.utc_ms == utc);
    uint64_t value;
    assert(nuwax_cloud_time_number((double)utc, &value) && value == utc);
    assert(!nuwax_cloud_time_number(NAN, &value));
    assert(!nuwax_cloud_time_number(INFINITY, &value));
    assert(!nuwax_cloud_time_number((double)utc + 0.5, &value));
    assert(nuwax_cloud_time_project(&snapshot, 1000 + 8 * 3600000ULL, &value));
    assert(value == utc + 8 * 3600000ULL);
    assert(!nuwax_cloud_time_project(&snapshot, UINT64_MAX, &value));
    assert(!nuwax_cloud_time_project(&(nuwax_cloud_time_anchor_t){NUWAX_CLOUD_TIME_MIN_MS, 100}, 0, &value));
    assert(sd_audio_catalog_init() == ESP_OK);

    sd_audio_session_t offline = scene("offline", 7, 1000 + 8 * 3600000ULL);
    assert(sd_audio_catalog_clock_save(7, offline.owner, &snapshot) == ESP_OK);
    sd_audio_session_time_t t = resolve(&offline, 7, &snapshot, false, 0);
    assert(t.valid == SD_AUDIO_TIME_STARTED && t.started_at_ms == utc + 8 * 3600000ULL);
    const uint64_t fixed_start = t.started_at_ms;
    nuwax_cloud_time_anchor_t jumped = {utc + 3600000, 2000};
    t = resolve(&offline, 7, &jumped, true, offline.start_ms + 8 * 3600000ULL);
    assert(t.valid == 3 && t.started_at_ms == fixed_start && t.duration_ms == 8 * 3600000ULL);
    t = resolve(&offline, 8, &jumped, true, offline.start_ms + 999999999);
    assert(t.valid == 3 && t.started_at_ms == fixed_start && t.duration_ms == 8 * 3600000ULL);

    sd_audio_session_t early = scene("early", 7, 100);
    t = resolve(&early, 7, NULL, true, 1100);
    // A saved same-boot clock is available even when caller has no RAM snapshot.
    assert(t.valid == 3 && t.started_at_ms == utc - 900 && t.duration_ms == 1000);
    sd_audio_session_t later = scene("late_calibration", 9, 5000);
    t = resolve(&later, 9, NULL, true, 65000);
    assert(t.valid == SD_AUDIO_TIME_DURATION && t.duration_ms == 60000);
    nuwax_cloud_time_anchor_t late = {utc, 100000};
    t = resolve(&later, 9, &late, false, 0);
    assert(t.valid == 3 && t.started_at_ms == utc - 95000 && t.duration_ms == 60000);

    sd_audio_session_t saved = scene("old_boot", 7, 2000);
    t = resolve(&saved, 12, &jumped, false, 0);
    assert(t.valid == SD_AUDIO_TIME_STARTED && t.started_at_ms == utc + 1000);
    sd_audio_session_t unknown = scene("unknown", 13, 2000);
    t = resolve(&unknown, 14, &jumped, false, 0);
    assert(t.valid == 0); // A new boot cannot infer time spent powered off.
    sd_audio_session_t other_owner = scene("other_owner", 7, 2000);
    other_owner.owner[31] ^= 1; // Same filename hash prefix; full owner must match.
    t = resolve(&other_owner, 14, &jumped, false, 0);
    assert(t.valid == 0);

    sd_audio_session_t wrapped = scene("long_uptime", 7, UINT64_C(0x200000000));
    t = resolve(&wrapped, 7, &snapshot, true, wrapped.start_ms + UINT64_C(0x100000001));
    assert(t.valid == 3 && t.duration_ms == UINT64_C(0x100000001));
    assert(t.started_at_ms == utc + wrapped.start_ms - 1000);
    sd_audio_session_t zero = scene("empty", 7, 1000);
    t = resolve(&zero, 7, &snapshot, true, 1000);
    assert(t.valid == 3 && t.duration_ms == 0);
    cJSON *end = sd_audio_end_json(false, "end", "empty", 0, true, &t);
    assert(end);
    cJSON *time = cJSON_GetObjectItemCaseSensitive(end, "session_time");
    assert(cJSON_GetObjectItemCaseSensitive(time, "started_at_ms")->valuedouble == (double)utc);
    assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(time, "duration_ms")));
    cJSON_Delete(end);
    end = sd_audio_end_json(true, "end", "unknown", 0, false, NULL);
    time = cJSON_GetObjectItemCaseSensitive(end, "session_time");
    assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(time, "started_at_ms")));
    assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(time, "duration_ms")));
    cJSON_Delete(end);

    sd_audio_session_t torn = scene("torn", 7, 1000);
    (void)resolve(&torn, 7, &snapshot, false, 0); // generation 1, slot 1
    (void)resolve(&torn, 7, &snapshot, true, 3000); // generation 2, slot 0
    corrupt_slot("catalog/torn.time", 0);
    t = resolve(&torn, 8, NULL, false, 0);
    assert(t.valid == SD_AUDIO_TIME_STARTED && t.started_at_ms == utc);
    // Loss of the last slot cannot invent a final stop on a different boot.
    corrupt_slot("catalog/torn.time", sizeof(sd_time_record_t));
    assert(sd_audio_catalog_session_time(&torn, 8, &jumped, false, 0, &t) != ESP_OK);
    assert(t.valid == 0);
    sd_audio_session_t preserved;
    assert(sd_audio_catalog_load("torn", &preserved) == ESP_OK); // Audio catalog unaffected.

    sd_audio_session_t no_storage = scene("no_storage", 7, 1000);
    assert(!rename("catalog", "catalog-away"));
    assert(sd_audio_catalog_session_time(&no_storage, 7, &snapshot, true, 2000, &t) != ESP_OK);
    assert(!t.valid); // Never publish a calculated value that was not durable.
    assert(!rename("catalog-away", "catalog"));

    fake_ms = 1000; active = false; card_note_capture_stopped();
    assert(!card_stop_time(&value));
    active = true; card_note_capture_stopped(); fake_ms = 9000; card_note_capture_stopped();
    assert(card_stop_time(&value) && value == 1000); // SD drain/retry does not extend duration.
    assert(!s_card_time.valid);
    puts("PASS: heartbeat validation; 8-hour offline projection; late calibration; immutable retry; reboot/owner fences; 64-bit time; null/zero JSON; torn slots; storage failure; capture-stop timing");
    return 0;
}
