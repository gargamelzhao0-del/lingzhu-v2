/* Each harness includes its existing platform stubs and links the actual
 * controller/protocol source with the SD feature enabled. */
#include "sd_audio_session.h"
static bool card_available = true, card_active, capture_live;
static bool card_duration_limit;
static bool card_detected;
static esp_err_t card_media_fault;
static unsigned card_starts, card_stops;
static bool card_replay_busy;
static int64_t card_identity_ready_at;
static unsigned card_start_leases;
esp_err_t nuwax_sd_session_prepare_start(nuwax_offline_audio_mode_t mode, int64_t deadline)
{
    if (deadline && esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
    card_start_leases |= 1U << (unsigned)mode;
    return card_replay_busy ? ESP_ERR_NOT_FINISHED : ESP_OK;
}
void nuwax_sd_session_release_start(nuwax_offline_audio_mode_t mode)
{
    card_start_leases &= ~(1U << (unsigned)mode);
}
static char card_used_ids[64][48];
static unsigned card_used_count;
static nuwax_offline_capture_health_t card_health = NUWAX_OFFLINE_CAPTURE_STARTING;
bool nuwax_sd_session_available(void) { return card_available; }
bool nuwax_sd_session_prefers_local(void) { return card_available || card_detected; }
esp_err_t nuwax_sd_session_start_fault(void) { return card_media_fault; }
bool nuwax_sd_session_active(void) { return card_active; }
static bool card_complete = true;
bool nuwax_sd_session_complete(void) { return card_complete; }
bool nuwax_sd_session_stopped(void) { return !card_active && !capture_live; }
bool nuwax_sd_session_monitoring(void) { return card_active; }
void nuwax_sd_session_network(bool online) { (void)online; }
esp_err_t nuwax_sd_session_start(nuwax_offline_audio_mode_t mode, char id[48])
{
    (void)mode; assert(!card_active);
    if (!card_available) return ESP_ERR_NOT_FINISHED;
    if (id[0] && !sd_audio_identity_valid(id)) return ESP_ERR_INVALID_ARG;
    if (!id[0]) snprintf(id, 48, "session_%u", card_starts + 1U);
    for (unsigned i = 0; i < card_used_count; ++i)
        if (!strcmp(card_used_ids[i], id)) return ESP_ERR_INVALID_STATE;
    assert(card_used_count < 64U);
    strcpy(card_used_ids[card_used_count++], id);
    ++card_starts;
    card_duration_limit = false;
    card_active = capture_live = true; return ESP_OK;
}
static esp_err_t (*card_identity_hook)(const char *);
esp_err_t nuwax_sd_session_poll_identity(nuwax_offline_audio_mode_t mode, const char *id, int64_t deadline)
{
    (void)mode;
    if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
    if (esp_timer_get_time() < card_identity_ready_at) return ESP_ERR_NOT_FINISHED;
    if (card_identity_hook) return card_identity_hook(id);
    if (!card_available) return ESP_ERR_NOT_FINISHED;
    for (unsigned i = 0; i < card_used_count; ++i)
        if (!strcmp(card_used_ids[i], id)) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}
esp_err_t nuwax_sd_session_start_until(nuwax_offline_audio_mode_t mode, char id[48], int64_t deadline)
{
    if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
    return nuwax_sd_session_start(mode, id);
}
void nuwax_sd_session_stop(bool complete)
{
    (void)complete; ++card_stops; capture_live = false;
    /* Capture stopped, writer still owns the final seal. */
}
nuwax_offline_capture_health_t nuwax_offline_audio_capture_health(void) { return card_health; }
bool nuwax_offline_audio_capture_running(void) { return capture_live; }
bool nuwax_offline_audio_limit_reached(void) { return card_duration_limit; }

static void sync_reply_session(MODE_TYPE *mode, nuwax_ws_client_t *ws, bool ok,
                               const char *session)
{
    char json[320];
    assert(mode->sd_sync.request_id[0]);
    snprintf(json, sizeof(json), "{\"type\":\"audio_mode_request_ack\",\"request_id\":\"%s\","
        "\"mode\":\"" MODE_NAME "\",\"state\":\"enabled\",\"ok\":%s,\"audio_session_id\":\"%s\"}",
        mode->sd_sync.request_id, ok ? "true" : "false", session);
    send_protocol(mode, ws, json);
}

static void sync_reply(MODE_TYPE *mode, nuwax_ws_client_t *ws, bool ok)
{
    sync_reply_session(mode, ws, ok, mode->audio_session_id);
}

static void cloud_stop(MODE_TYPE *mode, nuwax_ws_client_t *ws,
                       const char *request, const char *session)
{
    char json[320];
    snprintf(json, sizeof(json), "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
        "\"request_id\":\"%s\",\"mode\":\"" MODE_NAME "\",\"state\":\"disabled\",\"audio_session_id\":\"%s\"}",
        request, session);
    send_protocol(mode, ws, json);
}

static void check_sd_reconnect_controls(const CALLBACK_TYPE *callbacks)
{
    for (unsigned variant = 0; variant < 4U; ++variant) {
        card_complete = variant != 2U;
        MODE_TYPE mode;
        nuwax_ws_client_t ws = {.connected = false, .connection_epoch = 41 + variant};
        card_available = card_detected = true; card_active = capture_live = false;
        card_used_count = 0; card_health = NUWAX_OFFLINE_CAPTURE_RECORDING;
        reset_sent_messages(); assert(MODE_INIT(&mode, callbacks) == ESP_OK);
        assert(MODE_UI(&mode, NULL, true) == ESP_OK); MODE_SERVICE(&mode, NULL);
        assert(mode.state == STATE_ACTIVE && mode.sd_sync.attempts == 0);
        char id[48]; strcpy(id, mode.audio_session_id);
        const unsigned starts = card_starts, stops = card_stops;
        ws.connected = true;
        if (variant) {
            MODE_SERVICE(&mode, &ws);
            assert(sent_message_contains(mode.sd_sync.request_id, id, "audio_mode_request"));
            if (variant < 3) {
                sync_reply(&mode, &ws, variant == 2); MODE_SERVICE(&mode, &ws);
                assert(mode.state == STATE_ACTIVE && capture_live);
            } else {
                char request[48]; strcpy(request, mode.sd_sync.request_id);
                for (unsigned i=0; i<6; ++i) { s_now_us += 1000000LL; MODE_SERVICE(&mode, &ws); }
                assert(mode.sd_sync.attempts == 3 && !strcmp(request, mode.sd_sync.request_id));
                assert(mode.state == STATE_ACTIVE && capture_live);
            }
        }
        /* This is the deployed cloud format: the field is absent, not merely
         * empty. A bootstrap assignment and an App assignment are identical. */
        if (variant == 0 || variant == 1)
            send_protocol(&mode, &ws, "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
                "\"request_id\":\"state_stop\",\"mode\":\"" MODE_NAME "\",\"state\":\"disabled\"}");
        else cloud_stop(&mode, &ws, "state_stop", variant == 2 ? "old_scene" : "");
        MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_STOPPING && !capture_live && card_stops == stops + 1U);
        assert(card_active && card_starts == starts && !strcmp(id, mode.audio_session_id));
        assert(!sent_message_contains("state_stop", "\"ok\":true", NULL));
        card_active = false; MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_DISABLED);
        assert(sent_message_contains("state_stop", "\"ok\":true", "\"state\":\"disabled\""));
        MODE_DEINIT(&mode);
    }
    card_complete = true;
    nuwax_sd_control_sync_t policy = {0};
    nuwax_sd_control_sync_connect(&policy, 123, UINT32_MAX - 999U, UINT32_MAX - 4U);
    assert(!nuwax_sd_control_disable_rejection(&policy, UINT32_MAX-2U, 100U, "before_ack", "", "local"));
    nuwax_sd_control_sync_confirm(&policy, UINT32_MAX-1U, "cloud_alias");
    assert(!nuwax_sd_control_disable_rejection(&policy, 1U, 101U, "after_ack", "other", "local"));
    puts("PASS: desired-state close without ID before sync, after rejected/successful sync and timeout; seal-before-ACK, stable recording ID and bounded reports");
}

static void unified_open(MODE_TYPE *mode, nuwax_ws_client_t *ws,
                         const char *request, const char *id)
{
    char json[400];
    if (!id) {
        snprintf(json, sizeof(json), "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
            "\"request_id\":\"%s\",\"mode\":\"" MODE_NAME "\",\"state\":\"enabled\"}", request);
        send_protocol(mode, ws, json);
        return;
    }
    snprintf(json, sizeof(json), "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
        "\"request_id\":\"%s\",\"mode\":\"" MODE_NAME "\",\"state\":\"enabled\",\"audio_session_id\":\"%s\"}",
        request, id);
    send_protocol(mode, ws, json);
}

static void unified_stop(MODE_TYPE *mode, nuwax_ws_client_t *ws)
{
    assert(MODE_UI(mode, ws, false) == ESP_OK);
    MODE_SERVICE(mode, ws); card_active = false; MODE_SERVICE(mode, ws);
    assert(mode->state == STATE_DISABLED);
}

static void check_reconnected_control_identity(const CALLBACK_TYPE *callbacks)
{
    MODE_TYPE mode;
    nuwax_ws_client_t ws = {.connected = false, .connection_epoch = 700};
    card_available = card_detected = true; card_active = capture_live = false;
    card_used_count = 0; card_health = NUWAX_OFFLINE_CAPTURE_RECORDING;
    reset_sent_messages(); assert(MODE_INIT(&mode, callbacks) == ESP_OK);
    assert(MODE_UI(&mode, NULL, true) == ESP_OK); MODE_SERVICE(&mode, NULL);
    char local[48]; strcpy(local, mode.audio_session_id);
    const unsigned starts = card_starts, stops = card_stops;
    ws.connected = true; MODE_SERVICE(&mode, &ws);
    send_protocol(&mode, &ws, "{\"type\":\"audio_mode_request_ack\","
        "\"request_id\":\"unrelated_request\",\"mode\":\"" MODE_NAME "\","
        "\"state\":\"disabled\",\"ok\":true,\"audio_session_id\":\"foreign_scene\"}");
    sync_reply_session(&mode, &ws, false, "foreign_scene"); MODE_SERVICE(&mode, &ws);
    assert(mode.state == STATE_ACTIVE && !mode.sd_sync.confirmed && card_stops == stops);
    unified_open(&mode, &ws, "matching_enable_before_ack", NULL); MODE_SERVICE(&mode, &ws);
    assert(mode.sd_sync.confirmed && !strcmp(mode.audio_session_id, local));
    sync_reply_session(&mode, &ws, true, "reply_alias"); MODE_SERVICE(&mode, &ws);
    const uint32_t barrier = mode.sd_sync.confirmed_order;
    sync_reply_session(&mode, &ws, true, "different_reply"); MODE_SERVICE(&mode, &ws);
    assert(mode.sd_sync.confirmed_order == barrier && !strcmp(mode.sd_sync.reply_session_id, "reply_alias"));
    sync_reply_session(&mode, &ws, true, "old_epoch_reply");
    ws.connected = false; MODE_DISCONNECT(&mode);
    ws.connected = true; ++ws.connection_epoch; MODE_SERVICE(&mode, &ws);
    assert(!mode.sd_sync.confirmed && !strcmp(mode.audio_session_id, local));
    sync_reply_session(&mode, &ws, true, "latest_reply"); MODE_SERVICE(&mode, &ws);
    unified_open(&mode, &ws, "different_scene_start", "foreign_scene"); MODE_SERVICE(&mode, &ws);
    assert(mode.state == STATE_ACTIVE && card_starts == starts && card_stops == stops);
    assert(sent_message_contains("different_scene_start", "already_active", local));
    cloud_stop(&mode, &ws, "previous_scene_stop", "old_epoch_reply"); MODE_SERVICE(&mode, &ws);
    assert(mode.state == STATE_STOPPING && !capture_live && !strcmp(local, mode.audio_session_id));
    card_active = false; MODE_SERVICE(&mode, &ws);
    assert(mode.state == STATE_DISABLED);
    /* A duplicate accepted enable cannot resurrect the closed recording. */
    unified_open(&mode, &ws, "different_scene_start", "foreign_scene");
    assert(MODE_RUN(&mode, &ws) == ESP_OK);
    assert(mode.state == STATE_DISABLED && card_starts == starts);
    MODE_DEINIT(&mode);
    puts("PASS: sync replies never switch state; unrelated/failed/duplicate/old-epoch replies retain the recording; cloud state assignments keep actual ID and retire successful enable retries");
}

static void check_unified_ids(const CALLBACK_TYPE *callbacks)
{
    MODE_TYPE mode;
    nuwax_ws_client_t ws = {.connected = true, .connection_epoch = 300};
    card_available = card_detected = true; card_active = capture_live = false;
    card_health = NUWAX_OFFLINE_CAPTURE_STARTING;
    reset_sent_messages(); assert(MODE_INIT(&mode, callbacks) == ESP_OK);
    const char *invalid[] = {NULL, "", "bad.id", "../bad", "bad id", "123456789012345678901234567890123456"};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        card_health = NUWAX_OFFLINE_CAPTURE_RECORDING;
        char request[48]; snprintf(request, sizeof(request), "missing_open_%u", i);
        reset_sent_messages(); unified_open(&mode, &ws, request, invalid[i]);
        assert(MODE_RUN(&mode, &ws) == ESP_OK);
        assert(mode.state == STATE_ACTIVE && sd_audio_identity_valid(mode.audio_session_id));
        assert(sent_message_contains(request, "\"ok\":true", mode.audio_session_id));
        send_protocol(&mode, &ws, "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
            "\"request_id\":\"missing_close\",\"mode\":\"" MODE_NAME "\",\"state\":\"disabled\"}");
        MODE_SERVICE(&mode, &ws); assert(mode.state == STATE_STOPPING && !capture_live);
        card_active = false; MODE_SERVICE(&mode, &ws); assert(mode.state == STATE_DISABLED);
    }
    card_health = NUWAX_OFFLINE_CAPTURE_STARTING;
    const unsigned before = card_starts;

    reset_sent_messages();
    unified_open(&mode, &ws, "unified_open_A", "Cloud_A-001");
    unified_open(&mode, &ws, "unified_open_A", "Cloud_A-001");
    assert(MODE_RUN(&mode, &ws) == ESP_OK);
    assert(mode.state == STATE_STARTING && card_starts == before + 1U);
    assert(!strcmp(mode.audio_session_id, "Cloud_A-001"));
    assert(!sent_message_contains("unified_open_A", "\"ok\":true", NULL));
    card_health = NUWAX_OFFLINE_CAPTURE_RECORDING; MODE_SERVICE(&mode, &ws);
    assert(mode.state == STATE_ACTIVE && !mode.pending_command_ack);
    assert(sent_message_contains("unified_open_A", "\"audio_session_id\":\"Cloud_A-001\"", "\"ok\":true"));
    reset_sent_messages(); unified_open(&mode, &ws, "unified_open_A", "Cloud_A-001");
    MODE_SERVICE(&mode, &ws);
    assert(card_starts == before + 1U);
    assert(sent_message_contains("unified_open_A", "already_active", "\"audio_session_id\":\"Cloud_A-001\""));
    unified_open(&mode, &ws, "different_scene", "Cloud_B-001"); MODE_SERVICE(&mode, &ws);
    assert(card_starts == before + 1U && !strcmp(mode.audio_session_id, "Cloud_A-001"));
    assert(sent_message_contains("different_scene", "already_active", "Cloud_A-001"));

    reset_sent_messages(); ws.connected = false; MODE_DISCONNECT(&mode);
    ws.connected = true; ++ws.connection_epoch; MODE_SERVICE(&mode, &ws);
    char request[48]; strcpy(request, mode.sd_sync.request_id);
    assert(sent_message_contains(request, "Cloud_A-001", "audio_mode_request"));
    sync_reply(&mode, &ws, true); MODE_SERVICE(&mode, &ws);
    assert(mode.sd_sync.confirmed && !strcmp(mode.audio_session_id, "Cloud_A-001"));
    ws.connected = false; MODE_DISCONNECT(&mode);
    ws.connected = true; ++ws.connection_epoch; MODE_SERVICE(&mode, &ws);
    assert(strcmp(request, mode.sd_sync.request_id));
    assert(!strcmp(mode.audio_session_id, "Cloud_A-001"));
    cloud_stop(&mode, &ws, "unified_stop", "Cloud_A-001"); MODE_SERVICE(&mode, &ws);
    assert(mode.state == STATE_STOPPING);
    card_active = false; MODE_SERVICE(&mode, &ws);
    assert(mode.state == STATE_DISABLED);
    reset_sent_messages(); unified_open(&mode, &ws, "unified_open_A", "Cloud_A-001");
    assert(MODE_RUN(&mode, &ws) == ESP_OK);
    assert(mode.state == STATE_DISABLED && card_starts == before + 1U);
    /* A completed opening command is retired, including retransmissions after
     * its disable command. It cannot restart or publish stale active state. */
    assert(!sent_message_contains("unified_open_A", "\"ok\":true", NULL));
    /* A new command carrying an old cloud scene gets a new recording ID. */
    unified_open(&mode, &ws, "reuse_cloud_scene", "Cloud_A-001");
    assert(MODE_RUN(&mode, &ws) == ESP_OK); MODE_SERVICE(&mode, &ws);
    assert(mode.state == STATE_ACTIVE && card_starts == before + 2U);
    assert(strcmp(mode.audio_session_id, "Cloud_A-001") && sd_audio_identity_valid(mode.audio_session_id));
    assert(sent_message_contains("reuse_cloud_scene", "\"ok\":true", mode.audio_session_id));
    unified_stop(&mode, &ws);
    MODE_DEINIT(&mode);

    /* Failed start ACK survives disconnection while the original capture runs. */
    reset_sent_messages(); assert(MODE_INIT(&mode, callbacks) == ESP_OK);
    card_health = NUWAX_OFFLINE_CAPTURE_STARTING;
    unified_open(&mode, &ws, "unified_lost_ack", "Cloud_B-001"); assert(MODE_RUN(&mode, &ws) == ESP_OK);
    sd_fail_command_ack = true; card_health = NUWAX_OFFLINE_CAPTURE_RECORDING;
    MODE_SERVICE(&mode, &ws);
    assert(mode.state == STATE_ACTIVE && mode.pending_command_ack);
    ws.connected = false; MODE_DISCONNECT(&mode);
    assert(mode.pending_command_ack && !strcmp(mode.pending_command_request_id, "unified_lost_ack"));
    sd_fail_command_ack = false; ws.connected = true; ++ws.connection_epoch;
    s_now_us += 2000000LL; MODE_SERVICE(&mode, &ws);
    assert(!mode.pending_command_ack);
    assert(sent_message_contains("unified_lost_ack", "\"audio_session_id\":\"Cloud_B-001\"", "\"ok\":true"));
    reset_sent_messages(); unified_open(&mode, &ws, "unified_open_A", "Cloud_A-001");
    MODE_SERVICE(&mode, &ws);
    assert(sent_message_contains("unified_open_A", "already_active", "Cloud_B-001"));
    assert(!strcmp(mode.audio_session_id, "Cloud_B-001"));
    unified_stop(&mode, NULL); MODE_DEINIT(&mode);

    /* Wholly offline scene later synchronizes the device-generated identity. */
    reset_sent_messages(); assert(MODE_INIT(&mode, callbacks) == ESP_OK);
    assert(MODE_UI(&mode, NULL, true) == ESP_OK); MODE_SERVICE(&mode, NULL);
    char local[48]; strcpy(local, mode.audio_session_id);
    assert(mode.state == STATE_ACTIVE && sd_audio_identity_valid(local));
    ws.connection_epoch++; MODE_SERVICE(&mode, &ws);
    assert(sent_message_contains(mode.sd_sync.request_id, local, "audio_mode_request"));
    unified_stop(&mode, &ws); MODE_DEINIT(&mode);
    puts("PASS: optional control IDs in both configurations, valid unused ID adoption, used-ID renewal, readiness ACK, already-active state, retired retries, reconnect and local scene sync");
}

#if CONFIG_NUWAX_SD_LEGACY_CONTROL_COMPAT
static void check_legacy_control_compat(const CALLBACK_TYPE *callbacks)
{
    /* Missing field, empty field, UUID, punctuation/path-like text and the
     * existing parser's 47-byte boundary. None may become an SD path/ID. */
    const char *ids[] = {NULL, "", "550e8400-e29b-41d4-a716-446655440000",
        "legacy.scene", "../legacy scene", "12345678901234567890123456789012345678901234567"};
    for (unsigned i = 0; i < sizeof(ids)/sizeof(ids[0]); ++i) {
        MODE_TYPE mode;
        nuwax_ws_client_t ws = {.connected = true, .connection_epoch = 500 + i};
        card_available = card_detected = true; card_active = capture_live = false;
        card_health = NUWAX_OFFLINE_CAPTURE_STARTING; sd_fail_command_ack = false;
        reset_sent_messages(); assert(MODE_INIT(&mode, callbacks) == ESP_OK);
        const unsigned before = card_starts;
        unified_open(&mode, &ws, "legacy_open", ids[i]);
        unified_open(&mode, &ws, "legacy_open", ids[i]);
        assert(MODE_RUN(&mode, &ws) == ESP_OK);
        assert(mode.state == STATE_STARTING && card_starts == before + 1U);
        char local[48]; strcpy(local, mode.audio_session_id);
        assert(sd_audio_identity_valid(local));
        assert(!strcmp(mode.sd_sync.cloud_session_id, ids[i] ? ids[i] : ""));
        assert(!sent_message_contains("legacy_open", "\"ok\":true", NULL));
        card_health = NUWAX_OFFLINE_CAPTURE_RECORDING;
        /* A lost success ACK must retry the same local ID after reconnect. */
        sd_fail_command_ack = i == 0;
        MODE_SERVICE(&mode, &ws); assert(mode.state == STATE_ACTIVE);
        if (sd_fail_command_ack) {
            assert(mode.pending_command_ack);
            ws.connected = false; MODE_DISCONNECT(&mode);
            sd_fail_command_ack = false; ws.connected = true; ++ws.connection_epoch;
            s_now_us += 2000000LL; MODE_SERVICE(&mode, &ws);
        }
        assert(!mode.pending_command_ack);
        assert(sent_message_contains("legacy_open", "\"ok\":true", local));
        unified_open(&mode, &ws, "legacy_duplicate", ids[i]); MODE_SERVICE(&mode, &ws);
        assert(sent_message_contains("legacy_duplicate", "already_active", local));
        assert(card_starts == before + 1U && !strcmp(local, mode.audio_session_id));
        unified_open(&mode, &ws, "wrong_scene_open", "Other_scene");
        MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_ACTIVE && capture_live);
        assert(sent_message_contains("wrong_scene_open", "already_active", local));

        ws.connected = false; MODE_DISCONNECT(&mode); MODE_SERVICE(&mode, NULL);
        assert(mode.state == STATE_ACTIVE && !strcmp(local, mode.audio_session_id));
        ws.connected = true; ++ws.connection_epoch; MODE_SERVICE(&mode, &ws);
        assert(sent_message_contains(mode.sd_sync.request_id, local, "audio_mode_request"));
        sync_reply(&mode, &ws, true); MODE_SERVICE(&mode, &ws);
        assert(!strcmp(mode.sd_sync.cloud_session_id, ids[i] && ids[i][0] ? ids[i] : local));
        cloud_stop(&mode, &ws, "legacy_stop", ids[i] ? ids[i] : ""); MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_STOPPING && !capture_live && card_active);
        assert(!sent_message_contains("legacy_stop", "\"ok\":true", NULL));
        card_active = false; MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_DISABLED && !mode.sd_sync.cloud_session_id[0]);
        assert(sent_message_contains("legacy_stop", "\"ok\":true", "\"state\":\"disabled\""));
        reset_sent_messages(); unified_open(&mode, &ws, "legacy_open", ids[i]);
        assert(MODE_RUN(&mode, &ws) == ESP_OK);
        assert(mode.state == STATE_DISABLED && card_starts == before + 1U);
        assert(!sent_message_contains("legacy_open", "\"ok\":true", NULL));
        /* A genuinely new opening, even with the same legacy alias, is a new
         * recording. UI stop also releases its alias after sealing. */
        unified_open(&mode, &ws, "legacy_next_open", ids[i]);
        assert(MODE_RUN(&mode, &ws) == ESP_OK);
        assert(mode.state == STATE_ACTIVE && card_starts == before + 2U);
        assert(strcmp(local, mode.audio_session_id));
        unified_stop(&mode, &ws); MODE_DEINIT(&mode);
    }
    /* Stop while starting: accept both an unscoped old command and a UUID
     * alias, without publishing successful start or skipping the final seal. */
    for (unsigned i = 0; i < 2; ++i) {
        MODE_TYPE mode; nuwax_ws_client_t ws = {.connected = true, .connection_epoch = 600 + i};
        card_available = card_detected = true; card_active = capture_live = false;
        card_health = NUWAX_OFFLINE_CAPTURE_STARTING;
        reset_sent_messages(); assert(MODE_INIT(&mode, callbacks) == ESP_OK);
        const char *id = i ? ids[2] : NULL;
        unified_open(&mode, &ws, "early_open", id); assert(MODE_RUN(&mode, &ws) == ESP_OK);
        assert(mode.state == STATE_STARTING && capture_live);
        cloud_stop(&mode, &ws, "early_stop", id ? id : ""); MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_STOPPING && !capture_live);
        card_active = false; MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_DISABLED);
        assert(!sent_message_contains("early_open", "\"ok\":true", NULL));
        assert(sent_message_contains("early_stop", "\"ok\":true", NULL));
        MODE_DEINIT(&mode);
    }
    for (unsigned i = 0; i < 2; ++i) {
        MODE_TYPE mode; nuwax_ws_client_t ws = {.connected = true, .connection_epoch = 700 + i};
        card_available = card_detected = card_active = capture_live = false;
        card_health = NUWAX_OFFLINE_CAPTURE_RECORDING;
        reset_sent_messages(); assert(MODE_INIT(&mode, callbacks) == ESP_OK);
        const char *id = i ? ids[2] : NULL;
        const unsigned before = card_starts;
        unified_open(&mode, &ws, "late_card_open", id);
        /* Dequeue the real control without running the no-card capture loop. */
#if MODE_TEST_MEETING
        nuwax_meeting_mode_poll_control_events(&mode, &ws);
#else
        nuwax_monitoring_mode_poll_control_events(&mode, &ws);
#endif
        assert(mode.state == STATE_STARTING && !mode.sd_local && card_starts == before);
        card_detected = true; MODE_SERVICE(&mode, &ws);
        char local[48]; strcpy(local, mode.audio_session_id);
        assert(sd_audio_identity_valid(local) && mode.sd_local && card_starts == before);
        card_available = true; MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_ACTIVE && card_starts == before + 1U);
        assert(!strcmp(local, mode.audio_session_id));
        assert(sent_message_contains("late_card_open", "\"ok\":true", local));
        cloud_stop(&mode, &ws, "late_card_stop", id ? id : ""); MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_STOPPING && !capture_live);
        card_active = false; MODE_SERVICE(&mode, &ws); MODE_DEINIT(&mode);
    }
    {
        MODE_TYPE mode; nuwax_ws_client_t ws = {.connected = true, .connection_epoch = 800};
        card_available = card_active = capture_live = false; card_detected = true;
        reset_sent_messages(); assert(MODE_INIT(&mode, callbacks) == ESP_OK);
        const unsigned before = card_starts;
        unified_open(&mode, &ws, "busy_legacy_open", NULL); assert(MODE_RUN(&mode, &ws) == ESP_OK);
        assert(mode.sd_local && mode.state == STATE_STARTING && card_starts == before);
        s_now_us += 21000000LL; MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_DISABLED && card_starts == before);
        assert(sent_message_contains("busy_legacy_open", "\"ok\":false", "mode_handoff_failed"));
        assert(!sent_message_contains("busy_legacy_open", "\"ok\":true", NULL));
        MODE_DEINIT(&mode);
    }
    puts("PASS: TEMP_SD_CONTROL_COMPAT: missing/empty/UUID/legacy IDs, readiness, duplicate opens, alias and UI stops, reconnect/lost ACK, stale isolation, new scenes, stop during start, late/busy card");
}
#endif

static void check_reconnect_enable_then_stop(const CALLBACK_TYPE *callbacks)
{
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        MODE_TYPE mode;
        nuwax_ws_client_t ws = {.connected = false, .connection_epoch = 90U + scenario};
        card_available = card_detected = true; card_active = false;
        card_health = NUWAX_OFFLINE_CAPTURE_RECORDING; reset_sent_messages();
        assert(MODE_INIT(&mode, callbacks) == ESP_OK);
        assert(MODE_UI(&mode, NULL, true) == ESP_OK); MODE_SERVICE(&mode, NULL);
        assert(mode.state == STATE_ACTIVE);
        char id[48]; strcpy(id, mode.audio_session_id);
        const unsigned starts = card_starts, stops = card_stops;
        ws.connected = true; MODE_SERVICE(&mode, &ws);
        assert(!mode.sd_sync.confirmed && mode.sd_sync.request_id[0]);
        /* The separate ACK queue drains first. A rejected resume must neither
         * stop capture nor override a later accepted cloud enable. */
        sync_reply(&mode, &ws, false);
        const char *target = scenario == 1 ? "wrong_scene" :
            CONFIG_NUWAX_SD_LEGACY_CONTROL_COMPAT ? "" : id;
        unified_open(&mode, &ws, "reconnect_cloud_enable", target);
        sd_fail_command_ack = scenario == 3;
        MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_ACTIVE && card_starts == starts && card_stops == stops);
        assert(!strcmp(id, mode.audio_session_id));
        assert(mode.sd_sync.confirmed);
        const uint32_t barrier = mode.sd_sync.confirmed_order;
        sd_fail_command_ack = false;
        cloud_stop(&mode, &ws, "fresh_stop_after_enable", "");
        if (scenario == 2) {
            /* A later duplicate enable cannot move the cutoff past the stop. */
            unified_open(&mode, &ws, "duplicate_after_stop", target);
            sync_reply(&mode, &ws, false);
        }
        MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_STOPPING && !capture_live && card_stops == stops + 1U);
        assert(mode.sd_sync.confirmed_order == barrier);
        card_active = false; MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_DISABLED); MODE_DEINIT(&mode);
    }
    puts("PASS: reconnect enable with missing/different ID retains actual recording; rejected sync/lost ACK do not block App stop; duplicate enable during stop stays closed");
}

static void check_stop_disconnect_boundaries(const CALLBACK_TYPE *callbacks)
{
    /* Actual RX parser/queues and actual controllers. Each case starts a new
     * local scene; the storage double holds the seal until explicitly released. */
    for (unsigned variant = 0; variant < 14U; ++variant) {
        MODE_TYPE mode = {0};
        nuwax_ws_client_t ws = {.connected = true, .connection_epoch = 61};
        card_available = card_detected = true;
        card_active = capture_live = false;
        card_health = NUWAX_OFFLINE_CAPTURE_RECORDING;
        card_used_count = 0;
        reset_sent_messages();
        assert(MODE_INIT(&mode, callbacks) == ESP_OK);
        assert(MODE_UI(&mode, &ws, true) == ESP_OK);
        MODE_SERVICE(&mode, &ws);
        MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_ACTIVE && capture_live);
        if (variant != 7U) { s_now_us += 6000000LL; MODE_SERVICE(&mode, &ws); }
        const unsigned stops = card_stops;
        char scene[48]; strcpy(scene, mode.audio_session_id);
        const char *target = scene;
        if (variant == 4U) target = "unrelated_scene";
        if (variant == 7U || variant == 8U || variant == 10U) target = "";
        if (variant == 3U) {
            /* A correlated alias ACK immediately followed by App stop. */
            mode.sd_sync.attempts = 0; mode.sd_sync.next_send_ms = (uint32_t)(s_now_us / 1000);
            mode.sd_sync.started_ms = (uint32_t)(s_now_us / 1000);
            mode.sd_sync.confirmed = false; mode.sd_sync.reply_confirmed = false;
            MODE_SERVICE(&mode, &ws);
            sync_reply_session(&mode, &ws, true, "restored_cloud_alias");
            target = "restored_cloud_alias";
        }
        cloud_stop(&mode, &ws, "disconnect_stop", target);
        if (variant == 1U) MODE_SERVICE(&mode, &ws); /* Already executing stop. */
        if (variant == 5U) atomic_fetch_add(&mode.local_stop_generation, 1U);
        if (variant == 6U) s_now_us += 21000000LL; /* Expired before admission. */
        if (variant == 9U) cloud_stop(&mode, &ws, "disconnect_stop", target);
        if (variant == 10U || variant == 12U) {
            /* A newer observed control connection supersedes both scoped and
             * unscoped commands from an obsolete connection. */
            atomic_store(&mode.control_rx_epoch, 62);
        }
        ws.connected = false;
        if (variant != 2U) {
            MODE_DISCONNECT(&mode); /* Publish cancellation before reconnect work. */
            if (variant == 0U || variant == 1U || variant == 3U || variant == 9U)
                assert(card_stops > stops && !capture_live);
        }
        MODE_SERVICE(&mode, NULL);
        const bool should_stop = variant != 5U && variant != 6U && variant != 10U && variant != 12U;
        if (should_stop) {
            assert(mode.state == STATE_STOPPING && !capture_live && card_stops > stops);
            assert(!sent_message_contains("disconnect_stop", "\"ok\":true", "\"state\":\"disabled\""));
            if (variant == 11U) s_now_us += 21000000LL; /* Slow seal is not a failed start. */
            if (variant == 13U) s_now_us += 26000000LL; /* ACK timeout cannot undo a stop. */
            MODE_SERVICE(&mode, NULL);
            assert(mode.state == STATE_STOPPING && !strcmp(mode.stop_reason,
                MODE_TEST_MEETING ? "cloud_stop" : "cloud_disable"));
            card_active = false;
            MODE_SERVICE(&mode, NULL);
            assert(mode.state == STATE_DISABLED && mode.command_deadline.decided &&
                   mode.command_deadline.ok && !mode.command_deadline.enabled);
            ws.connected = true; ws.connection_epoch = 63;
#if MODE_TEST_MEETING
            nuwax_meeting_mode_service_command_deadline(&mode, &ws);
#else
            nuwax_monitoring_mode_service_command_deadline(&mode, &ws);
#endif
            assert(sent_message_contains("disconnect_stop", "\"ok\":true", "\"state\":\"disabled\"") == (variant != 13U));
            assert(MODE_UI(&mode, &ws, true) == ESP_OK);
            MODE_SERVICE(&mode, &ws); MODE_SERVICE(&mode, &ws);
            assert(mode.state == STATE_ACTIVE && strcmp(scene, mode.audio_session_id));
            cloud_stop(&mode, &ws, "old_scene_stop", scene);
            MODE_SERVICE(&mode, &ws);
            assert(mode.state == STATE_STOPPING && !capture_live);
        } else assert(mode.state == STATE_ACTIVE && capture_live && card_stops == stops);
        MODE_UI(&mode, NULL, false); MODE_SERVICE(&mode, NULL);
        card_active = false; MODE_SERVICE(&mode, NULL); MODE_DEINIT(&mode);
    }
    puts("PASS: queued/admitted/disconnected stop with any control ID, generation/epoch/expiry fences, seal-before-ACK, reconnect result and state close of a newer scene");
}

static void check_known_media_fault(const CALLBACK_TYPE *callbacks)
{
    MODE_TYPE mode; nuwax_ws_client_t ws={.connected=true,.connection_epoch=901};
    card_available=false;card_detected=true;card_active=capture_live=false;card_used_count=0;
    card_media_fault=ESP_FAIL;card_health=NUWAX_OFFLINE_CAPTURE_STARTING;
    reset_sent_messages();assert(MODE_INIT(&mode,callbacks)==ESP_OK);
    const unsigned starts=card_starts;const int64_t before=s_now_us;
    unified_open(&mode,&ws,"known_bad_card","scene_bad_card");
    assert(MODE_RUN(&mode,&ws)==ESP_OK);
    assert(mode.state==STATE_DISABLED&&!mode.sd_local&&!mode.capture_running&&card_starts==starts);
    assert(mode.command_deadline.decided&&!mode.command_deadline.ok&&!strcmp(mode.command_deadline.message,"sd_start_failed"));
    assert(sent_message_contains("known_bad_card","\"ok\":false","sd_start_failed"));
    assert(s_now_us-before<5000000LL&&!card_start_leases);
    /* Once a request failed, recovery cannot turn a duplicate into a start. */
    card_media_fault=ESP_OK;card_available=true;
    unified_open(&mode,&ws,"known_bad_card","scene_bad_card");
    assert(MODE_RUN(&mode,&ws)==ESP_OK&&mode.state==STATE_DISABLED&&card_starts==starts);
    unified_open(&mode,&ws,"after_media_recovery","scene_after_media_recovery");
    assert(MODE_RUN(&mode,&ws)==ESP_OK&&card_starts==starts+1U);
    card_health=NUWAX_OFFLINE_CAPTURE_RECORDING;MODE_SERVICE(&mode,&ws);assert(mode.state==STATE_ACTIVE);
    card_media_fault=ESP_FAIL;MODE_SERVICE(&mode,&ws);assert(mode.state==STATE_ACTIVE); /* Active fault recovery remains its owner's job. */
    card_media_fault=ESP_OK;MODE_UI(&mode,&ws,false);MODE_SERVICE(&mode,&ws);card_active=false;MODE_SERVICE(&mode,&ws);MODE_DEINIT(&mode);
    /* Offline/local entry also leaves STARTING immediately when the medium
     * already failed, preserving the existing UI/owner cleanup path. */
    card_available=false;card_detected=true;card_active=capture_live=false;card_media_fault=ESP_FAIL;
    assert(MODE_INIT(&mode,callbacks)==ESP_OK);
    assert(MODE_UI(&mode,NULL,true)==ESP_OK);MODE_SERVICE(&mode,NULL);
    assert(mode.state==STATE_DISABLED&&card_starts==starts+1U&&!card_start_leases);
    MODE_DEINIT(&mode);card_media_fault=ESP_OK;
    puts("PASS: known card I/O fault fails App admission before UI/MIC handoff; immutable duplicate, recovery/new request, local failure cleanup and active-scene isolation");
}

static void test_card_meeting_duration_exit(void)
{
#if MODE_TEST_MEETING
    for (unsigned online = 0; online < 2; ++online) {
        card_used_count = 0;
        card_available = true; card_detected = false; card_active = capture_live = false;
        card_health = NUWAX_OFFLINE_CAPTURE_RECORDING;
        MODE_TYPE mode;
        const CALLBACK_TYPE callbacks = {.can_start = can_start, .on_state_changed = on_state_changed};
        assert(MODE_INIT(&mode, &callbacks) == ESP_OK);
        nuwax_ws_client_t ws = {.connected = online != 0, .connection_epoch = 100U + online};
        assert(MODE_UI(&mode, &ws, true) == ESP_OK);
        MODE_SERVICE(&mode, &ws); MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_ACTIVE && capture_live);
        const unsigned starts = card_starts;
        s_now_us += 28800000000LL;
        card_duration_limit = true; capture_live = false;
        MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_ACTIVE); /* Final seal still owns SD. */
        card_active = false; MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_DISABLED && !strcmp(mode.close_reason, "duration_limit"));
        assert(mode.pending_ui_close == !online);
        ws.connected = true; ++ws.connection_epoch;
        nuwax_meeting_mode_service_command_deadline(&mode, &ws);
        MODE_SERVICE(&mode, &ws); MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_DISABLED && card_starts == starts && !capture_live);
        assert(!mode.pending_ui_close);
        MODE_DEINIT(&mode);
    }
    card_duration_limit = false;
    puts("PASS: total meeting duration waits for seal, exits UI owner and does not restart on reconnect (online/offline)");
#endif
}

static void check_replay_start_barrier(const CALLBACK_TYPE *callbacks)
{
    for(unsigned app=0;app<2;app++)
    for(unsigned cancel=0;cancel<2;cancel++) {
        MODE_TYPE mode; nuwax_ws_client_t ws={.connected=true,.connection_epoch=130U+cancel};
        card_available=card_detected=true;card_active=capture_live=false;
        card_health=NUWAX_OFFLINE_CAPTURE_STARTING;card_replay_busy=true;card_start_leases=0;
        const unsigned starts=card_starts;
        assert(MODE_INIT(&mode,callbacks)==ESP_OK);
        if(app) {
            char json[320];
            snprintf(json,sizeof(json),"{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
                "\"request_id\":\"r192_open_%u\",\"mode\":\"" MODE_NAME "\",\"state\":\"enabled\","
                "\"audio_session_id\":\"r192_" MODE_NAME "_barrier_%u\"}",cancel,cancel);
            send_protocol(&mode,&ws,json);
            assert(MODE_RUN(&mode,&ws)==ESP_OK);
        } else assert(MODE_UI(&mode,&ws,true)==ESP_OK);
        MODE_SERVICE(&mode,&ws);
        assert(mode.state==STATE_STARTING && !capture_live && card_starts==starts && card_start_leases);
        if(cancel) {
            MODE_UI(&mode,&ws,false);MODE_SERVICE(&mode,&ws);
            assert(mode.state==STATE_DISABLED && !card_start_leases && card_starts==starts);
        } else {
            s_now_us+=400000;card_replay_busy=false;MODE_SERVICE(&mode,&ws);
            assert(mode.state==STATE_STARTING && capture_live && card_starts==starts+1 && card_start_leases);
            card_health=NUWAX_OFFLINE_CAPTURE_RECORDING;MODE_SERVICE(&mode,&ws);
            assert(mode.state==STATE_ACTIVE && !card_start_leases && ws.connected);
            MODE_UI(&mode,&ws,false);MODE_SERVICE(&mode,&ws);card_active=false;MODE_SERVICE(&mode,&ws);
            assert(mode.state==STATE_DISABLED && !card_start_leases);
        }
        card_replay_busy=false;MODE_DEINIT(&mode);
    }
    puts("PASS: actual SD App/local controller waits for replay release; keeps start lease until recording; cancellation releases it without a late start");
    {
        MODE_TYPE mode;
        nuwax_ws_client_t ws = {.connected = true, .connection_epoch = 990};
        card_available = card_detected = true; card_active = capture_live = false;
        card_health = NUWAX_OFFLINE_CAPTURE_RECORDING; card_replay_busy = true;
        card_identity_ready_at = s_now_us + 6000000LL;
        assert(MODE_INIT(&mode, callbacks) == ESP_OK);
        unified_open(&mode, &ws, "slow_identity_open", NULL);
        assert(MODE_RUN(&mode, &ws) == ESP_OK);
        assert(mode.state == STATE_STARTING && !mode.command_deadline.prepared);
        const int64_t deadline = nuwax_audio_command_execution_deadline(&mode.command_deadline);
        s_now_us += 6100000LL; MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_STARTING && mode.command_deadline.prepared && !capture_live);
        assert(nuwax_audio_command_execution_deadline(&mode.command_deadline) == deadline);
        card_replay_busy = false; s_now_us += 100000LL; MODE_SERVICE(&mode, &ws);
        assert(mode.state == STATE_ACTIVE && capture_live);
        assert(sent_message_contains("slow_identity_open", "\"ok\":true", mode.audio_session_id));
        unified_stop(&mode, &ws); MODE_DEINIT(&mode); card_identity_ready_at = 0;
    }
    puts("PASS: slow identity preflight does not consume the SD launch window or extend the 20s command deadline");
}

int main(void)
{
    MODE_TYPE mode;
    const CALLBACK_TYPE callbacks = {.can_start = can_start, .on_state_changed = on_state_changed};
    assert(MODE_INIT(&mode, &callbacks) == ESP_OK);
    nuwax_ws_client_t ws = {.connected = true, .connection_epoch = 1};
    assert(MODE_UI(&mode, &ws, true) == ESP_OK);
    MODE_SERVICE(&mode, &ws);
    assert(card_starts == 1 && mode.sd_local && mode.state == STATE_STARTING);
    card_health = NUWAX_OFFLINE_CAPTURE_RECORDING;
    MODE_SERVICE(&mode, &ws);
    assert(mode.state == STATE_ACTIVE);
    char id[48]; strcpy(id, mode.audio_session_id);
    ws.connected = false; MODE_DISCONNECT(&mode);
    MODE_SERVICE(&mode, NULL);
    assert(mode.state == STATE_ACTIVE && card_starts == 1 && !strcmp(id, mode.audio_session_id));
    ws.connected = true; ws.connection_epoch++;
    MODE_SERVICE(&mode, &ws);
    assert(card_starts == 1 && !strcmp(id, mode.audio_session_id));
    assert(MODE_UI(&mode, &ws, false) == ESP_OK);
    MODE_SERVICE(&mode, &ws);
    assert(card_stops && mode.state == STATE_STOPPING);
    card_active = false;
    MODE_SERVICE(&mode, &ws);
    assert(mode.state == STATE_DISABLED && !mode.sd_local);
#if MODE_TEST_MEETING
    assert(mode.close_request_id[0]);
    char close_ack[256];
    snprintf(close_ack, sizeof(close_ack), "{\"type\":\"audio_mode_request_ack\",\"request_id\":\"%s\","
        "\"mode\":\"meeting\",\"state\":\"disabled\",\"ok\":true}", mode.close_request_id);
    send_protocol(&mode, &ws, close_ack); assert(MODE_RUN(&mode, &ws) == ESP_OK);
    assert(!mode.close_request_id[0] && mode.state == STATE_DISABLED);
#endif
    ws.connected = false;
    assert(MODE_UI(&mode, NULL, true) == ESP_OK);
    MODE_SERVICE(&mode, NULL);
    assert(card_starts == 2 && strcmp(id, mode.audio_session_id));
    assert(MODE_UI(&mode, NULL, false) == ESP_OK);
    MODE_SERVICE(&mode, NULL); card_active = false; MODE_SERVICE(&mode, NULL);
    assert(mode.state == STATE_DISABLED);
    card_available = false;
    assert(MODE_UI(&mode, NULL, true) == ESP_ERR_INVALID_STATE);
    assert(card_starts == 2);
    /* A no-card session reconnecting after insertion retains its live-cloud
     * path. Availability must not silently turn that continuation into SD. */
    card_available = true; ws.connected = true;
    nuwax_audio_mode_identity_remember(&mode.identity, "original_online_session");
    mode.identity.resuming = true;
    assert(MODE_UI(&mode, &ws, true) == ESP_OK);
    MODE_SERVICE(&mode, &ws);
    assert(!mode.sd_local && card_starts == 2);
    MODE_DEINIT(&mode);
    assert(MODE_INIT(&mode, &callbacks) == ESP_OK);
    card_health=NUWAX_OFFLINE_CAPTURE_STARTING;
    const char *open="{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
        "\"request_id\":\"app_open\",\"mode\":\"" MODE_NAME "\",\"state\":\"enabled\",\"audio_session_id\":\"app_scene\"}";
    send_protocol(&mode, &ws, open);
    assert(MODE_RUN(&mode, &ws)==ESP_OK);
    assert(mode.sd_local && mode.state==STATE_STARTING && mode.pending_command_ack && card_starts==3);
    card_health=NUWAX_OFFLINE_CAPTURE_RECORDING;
    assert(MODE_RUN(&mode, &ws)==ESP_OK);
    assert(mode.state==STATE_ACTIVE && !mode.pending_command_ack);
    strcpy(id, mode.audio_session_id);
    ws.connected=false; MODE_DISCONNECT(&mode); ws.connected=true; ws.connection_epoch++;
    send_protocol(&mode, &ws, open); assert(MODE_RUN(&mode, &ws)==ESP_OK);
    assert(card_starts==3 && !strcmp(id,mode.audio_session_id));
    /* The accepted repeated enable now confirms state directly; it must not
     * depend on an extra resume request/ACK before an App close can work. */
    assert(mode.sd_sync.confirmed);
    const char *close="{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
        "\"request_id\":\"app_close\",\"mode\":\"" MODE_NAME "\",\"state\":\"disabled\"}";
    send_protocol(&mode, &ws, close); assert(MODE_RUN(&mode, &ws)==ESP_OK);
    assert(mode.state==STATE_STOPPING);
    card_active=false; assert(MODE_RUN(&mode, &ws)==ESP_OK);
    assert(mode.state==STATE_DISABLED);
    MODE_DEINIT(&mode);
    /* Known card temporarily busy: latch the route BEFORE waiting. Recovery
     * must not accidentally start the legacy cloud capture path. */
    assert(MODE_INIT(&mode, &callbacks) == ESP_OK);
    card_available=false;card_detected=true;card_health=NUWAX_OFFLINE_CAPTURE_STARTING;
    unsigned starts=card_starts;
    assert(MODE_UI(&mode, &ws, true)==ESP_OK);
    assert(MODE_RUN(&mode, &ws)==ESP_OK);
    assert(mode.sd_local && mode.state==STATE_STARTING && card_starts==starts);
    s_now_us+=1000000LL;card_available=true;
    assert(MODE_RUN(&mode, &ws)==ESP_OK);
    assert(card_starts==starts+1 && mode.sd_local && mode.state==STATE_STARTING);
    card_health=NUWAX_OFFLINE_CAPTURE_RECORDING;MODE_SERVICE(&mode,&ws);
    assert(mode.state==STATE_ACTIVE);
    MODE_UI(&mode,&ws,false);MODE_SERVICE(&mode,&ws);card_active=false;MODE_SERVICE(&mode,&ws);
    MODE_DEINIT(&mode);
    /* The App entrance follows the same route. A busy identity preflight is
     * bounded by the command deadline and never reports recording. */
    assert(MODE_INIT(&mode, &callbacks) == ESP_OK);
    card_available=false;starts=card_starts;
    send_protocol(&mode,&ws,open);assert(MODE_RUN(&mode,&ws)==ESP_OK);
    assert(mode.sd_local && mode.state==STATE_STARTING && mode.pending_command_ack);
    s_now_us+=21000000LL;MODE_SERVICE(&mode,&ws);
    assert(mode.state==STATE_DISABLED && card_starts==starts && !mode.pending_command_ack);
    MODE_DEINIT(&mode);
    /* No card keeps the original cloud request path, and remains unavailable
     * offline. A stopped SD start does not pin future requests to that route. */
    assert(MODE_INIT(&mode, &callbacks) == ESP_OK);card_detected=false;
    assert(MODE_UI(&mode,NULL,true)==ESP_ERR_INVALID_STATE);
    assert(MODE_UI(&mode,&ws,true)==ESP_OK);
    MODE_SERVICE(&mode,&ws);assert(!mode.sd_local && card_starts==starts);
    MODE_DEINIT(&mode);
    puts("PASS: actual SD controllers: readiness ACK, stable IDs, stop/seal, multiple offline scenes, transient-card wait, App timeout, no-card live path and reconnect route");
    check_sd_reconnect_controls(&callbacks);
    check_unified_ids(&callbacks);
    check_reconnect_enable_then_stop(&callbacks);
    check_reconnected_control_identity(&callbacks);
    check_stop_disconnect_boundaries(&callbacks);
    test_card_meeting_duration_exit();
    check_replay_start_barrier(&callbacks);
    check_known_media_fault(&callbacks);
#if CONFIG_NUWAX_SD_LEGACY_CONTROL_COMPAT
    check_legacy_control_compat(&callbacks);
#endif
    return 0;
}
