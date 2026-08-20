#include <stdbool.h>
#include <stdio.h>

#include "nuwax_ui_v28/nuwax_face.h"
#include "nuwax_ui_v28/face/emotion_animation_player.h"

static esp_err_t s_player_init_result = ESP_OK;
static esp_err_t s_player_set_result = ESP_OK;
static esp_err_t s_player_tick_result = ESP_OK;
static esp_err_t s_player_deinit_result = ESP_OK;
static esp_err_t s_player_reap_result = ESP_OK;
static nuwax_emotion_asset_id_t s_asset;
static unsigned s_touch_count;
static unsigned s_player_deinit_count;
static unsigned s_player_tick_count;
static unsigned s_player_reap_count;
static int s_failures;

static void expect_asset(const char *name, nuwax_emotion_asset_id_t expected)
{
    if (s_asset != expected) {
        fprintf(stderr, "[FAIL] %s asset=%d expected=%d\n",
                name, (int)s_asset, (int)expected);
        s_failures++;
    } else {
        printf("[PASS] %s asset=%d\n", name, (int)s_asset);
    }
}

static void expect_true(const char *name, bool value)
{
    if (!value) {
        fprintf(stderr, "[FAIL] %s\n", name);
        s_failures++;
    } else {
        printf("[PASS] %s\n", name);
    }
}

esp_err_t nuwax_emotion_animation_player_init(lv_obj_t *parent)
{
    if (!parent) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_player_init_result == ESP_OK) {
        s_asset = NUWAX_EMOTION_ASSET_HAPPY;
    }
    return s_player_init_result;
}

esp_err_t nuwax_emotion_animation_player_deinit(void)
{
    s_player_deinit_count++;
    return s_player_deinit_result;
}

esp_err_t nuwax_emotion_animation_player_reap(void)
{
    s_player_reap_count++;
    return s_player_reap_result;
}

esp_err_t nuwax_emotion_animation_player_set_asset(
    nuwax_emotion_asset_id_t asset_id)
{
    if (s_player_set_result == ESP_OK) {
        s_asset = asset_id;
    }
    return s_player_set_result;
}

esp_err_t nuwax_emotion_animation_player_trigger_touch(void)
{
    s_touch_count++;
    s_asset = NUWAX_EMOTION_ASSET_TOUCH;
    return ESP_OK;
}

esp_err_t nuwax_emotion_animation_player_tick(void)
{
    s_player_tick_count++;
    return s_player_tick_result;
}

void nuwax_emotion_animation_player_set_interaction_active(bool active)
{
    (void)active;
}

void nuwax_emotion_animation_player_set_overlay_progress(
    uint16_t progress_permille)
{
    (void)progress_permille;
}

bool nuwax_emotion_animation_player_ready(void)
{
    return true;
}

esp_err_t nuwax_emotion_animation_player_freeze(void)
{
    return ESP_OK;
}

bool nuwax_emotion_animation_player_frozen_frame_ready(void)
{
    return true;
}

const uint16_t *nuwax_emotion_animation_player_frozen_rgb565(
    size_t *stride_pixels)
{
    static uint16_t frame[360];
    if (stride_pixels) *stride_pixels = 360U;
    return frame;
}

bool nuwax_emotion_animation_player_draw_frozen(void *layer)
{
    return layer != NULL;
}

int main(void)
{
    lv_obj_t parent = {0};
    expect_true("animated backend starts",
                nuwax_face_create(&parent) == ESP_OK);

    nuwax_device_state_t state = {
        .emotion = NUWAX_EMOTION_HAPPY,
        .wifi = NUWAX_WIFI_CONNECTED,
        .cloud = NUWAX_CLOUD_CONNECTED,
        .dialog = NUWAX_DIALOG_READY,
        .meeting = NUWAX_MEETING_UI_DISABLED,
        .monitoring = NUWAX_MONITORING_UI_DISABLED,
    };
    nuwax_face_update(&state);
    expect_asset("ready happy", NUWAX_EMOTION_ASSET_HAPPY);

    state.provisioning_active = true;
    state.monitoring = NUWAX_MONITORING_UI_ACTIVE;
    nuwax_face_update(&state);
    expect_asset("provisioning has highest priority",
                 NUWAX_EMOTION_ASSET_PROVISIONING);

    state.provisioning_active = false;
    nuwax_face_update(&state);
    expect_asset("monitoring priority", NUWAX_EMOTION_ASSET_MONITORING);

    state.monitoring = NUWAX_MONITORING_UI_DISABLED;
    state.meeting = NUWAX_MEETING_UI_ACTIVE;
    nuwax_face_update(&state);
    expect_asset("meeting priority", NUWAX_EMOTION_ASSET_MEETING);

    state.meeting = NUWAX_MEETING_UI_DISABLED;
    state.dialog = NUWAX_DIALOG_SLEEP;
    state.app_low_power = true;
    nuwax_face_update(&state);
    expect_asset("sleep default", NUWAX_EMOTION_ASSET_SLEEPING);

    state.semantic_emotion_active = true;
    state.emotion = NUWAX_EMOTION_ANGRY;
    state.touch_emotion_active = true;
    nuwax_face_update(&state);
    expect_asset("sleep permits an active leased semantic emotion",
                 NUWAX_EMOTION_ASSET_ANGRY);
    const unsigned sleep_touch_count = s_touch_count;
    state.touch_sequence++;
    state.touch_emotion_active = false;
    nuwax_face_update(&state);
    expect_true("coalesced sleep touch cannot start a transient",
                s_touch_count == sleep_touch_count);
    state.touch_emotion_active = false;

    state.meeting = NUWAX_MEETING_UI_ACTIVE;
    nuwax_face_update(&state);
    expect_asset("real meeting overrides semantic face",
                 NUWAX_EMOTION_ASSET_MEETING);
    expect_true("semantic meeting test does not alter business state",
                state.monitoring == NUWAX_MONITORING_UI_DISABLED &&
                    !state.provisioning_active);
    state.meeting = NUWAX_MEETING_UI_DISABLED;
    state.semantic_emotion_active = false;
    state.emotion = NUWAX_EMOTION_HAPPY;

    state.dialog = NUWAX_DIALOG_READY;
    state.app_low_power = true;
    nuwax_face_update(&state);
    expect_asset("app low power uses sleeping face",
                 NUWAX_EMOTION_ASSET_SLEEPING);
    state.app_low_power = false;

    state.wifi = NUWAX_WIFI_CONNECTING;
    nuwax_face_update(&state);
    expect_asset("Wi-Fi connecting is unavailable",
                 NUWAX_EMOTION_ASSET_DISCONNECTED);

    state.wifi = NUWAX_WIFI_WEAK;
    state.emotion = NUWAX_EMOTION_ANGRY;
    nuwax_face_update(&state);
    expect_asset("weak Wi-Fi remains connected",
                 NUWAX_EMOTION_ASSET_ANGRY);

    state.cloud = NUWAX_CLOUD_CONNECTING;
    nuwax_face_update(&state);
    expect_asset("cloud connecting preserves local wake-ready face",
                 NUWAX_EMOTION_ASSET_ANGRY);
    state.touch_sequence++;
    state.touch_emotion_active = true;
    nuwax_face_update(&state);
    expect_asset("local touch remains visible while cloud reconnects",
                 NUWAX_EMOTION_ASSET_TOUCH);
    state.touch_emotion_active = false;
    nuwax_face_update(&state);
    expect_asset("touch expiry restores local wake-ready face",
                 NUWAX_EMOTION_ASSET_ANGRY);
    state.cloud = NUWAX_CLOUD_CONNECTED;

    const unsigned expired_touch_before = s_touch_count;
    state.touch_sequence++;
    state.touch_emotion_active = false;
    nuwax_face_update(&state);
    expect_true("coalesced expired touch cannot start late",
                s_touch_count == expired_touch_before);

    state.dialog = NUWAX_DIALOG_ERROR;
    state.touch_sequence++;
    state.touch_emotion_active = true;
    nuwax_face_update(&state);
    expect_asset("local touch remains visible in cloud error state",
                 NUWAX_EMOTION_ASSET_TOUCH);
    state.touch_emotion_active = false;

    state.dialog = NUWAX_DIALOG_LISTENING;
    nuwax_face_update(&state);
    expect_asset("listening face", NUWAX_EMOTION_ASSET_LISTENING);
    state.dialog = NUWAX_DIALOG_SPEAKING;
    state.emotion = NUWAX_EMOTION_HAPPY;
    nuwax_face_update(&state);
    expect_asset("speaking defaults to happy",
                 NUWAX_EMOTION_ASSET_HAPPY);

    state.dialog = NUWAX_DIALOG_THINKING;
    state.emotion = NUWAX_EMOTION_QUESTION;
    nuwax_face_update(&state);
    expect_asset("waiting lifecycle uses question",
                 NUWAX_EMOTION_ASSET_QUESTION);
    state.semantic_emotion_active = true;
    state.touch_emotion_active = true;
    state.touch_sequence++;
    state.emotion = NUWAX_EMOTION_TOUCH;
    nuwax_face_update(&state);
    expect_asset("touch overrides cloud thinking",
                 NUWAX_EMOTION_ASSET_TOUCH);
    state.emotion = NUWAX_EMOTION_DIZZINESS;
    state.touch_sequence++;
    nuwax_face_update(&state);
    expect_asset("shake overrides cloud thinking",
                 NUWAX_EMOTION_ASSET_DIZZINESS);
    state.touch_emotion_active = false;
    state.emotion = NUWAX_EMOTION_THOUGHT_COMPLETE;
    nuwax_face_update(&state);
    expect_asset("explicit thought-complete",
                 NUWAX_EMOTION_ASSET_THOUGHT_COMPLETE);
    state.touch_emotion_active = false;

    static const struct {
        nuwax_emotion_t emotion;
        nuwax_emotion_asset_id_t asset;
        const char *name;
    } semantic_cases[] = {
        {NUWAX_EMOTION_HAPPY, NUWAX_EMOTION_ASSET_HAPPY,
         "canonical happy"},
        {NUWAX_EMOTION_DIZZINESS, NUWAX_EMOTION_ASSET_DIZZINESS,
         "canonical dizziness"},
        {NUWAX_EMOTION_EXCITED, NUWAX_EMOTION_ASSET_EXCITED,
         "canonical excited"},
        {NUWAX_EMOTION_SLEEPING_FACE, NUWAX_EMOTION_ASSET_SLEEPING,
         "canonical sleeping"},
        {NUWAX_EMOTION_PROVISIONING, NUWAX_EMOTION_ASSET_PROVISIONING,
         "canonical provisioning is UI only"},
        {NUWAX_EMOTION_DISCONNECTED_FACE,
         NUWAX_EMOTION_ASSET_DISCONNECTED,
         "canonical disconnected"},
        {NUWAX_EMOTION_THINKING, NUWAX_EMOTION_ASSET_THINKING,
         "canonical thinking"},
        {NUWAX_EMOTION_THOUGHT_COMPLETE,
         NUWAX_EMOTION_ASSET_THOUGHT_COMPLETE,
         "canonical thought_complete"},
        {NUWAX_EMOTION_MEETING_FACE, NUWAX_EMOTION_ASSET_MEETING,
         "canonical meeting is UI only"},
        {NUWAX_EMOTION_MONITORING_FACE, NUWAX_EMOTION_ASSET_MONITORING,
         "canonical monitoring is UI only"},
        {NUWAX_EMOTION_PLAYFUL, NUWAX_EMOTION_ASSET_PLAYFUL,
         "canonical playful"},
        {NUWAX_EMOTION_LIKE, NUWAX_EMOTION_ASSET_LIKE,
         "canonical like"},
        {NUWAX_EMOTION_ANGRY, NUWAX_EMOTION_ASSET_ANGRY,
         "canonical angry"},
        {NUWAX_EMOTION_QUESTION, NUWAX_EMOTION_ASSET_QUESTION,
         "canonical question"},
    };
    state.dialog = NUWAX_DIALOG_READY;
    state.semantic_emotion_active = true;
    for (size_t i = 0;
         i < sizeof(semantic_cases) / sizeof(semantic_cases[0]);
         ++i) {
        state.emotion = semantic_cases[i].emotion;
        nuwax_face_update(&state);
        expect_asset(semantic_cases[i].name, semantic_cases[i].asset);
    }

    state.emotion = NUWAX_EMOTION_TOUCH;
    nuwax_face_update(&state);
    expect_asset("canonical cloud touch uses timed asset",
                 NUWAX_EMOTION_ASSET_TOUCH);
    /* End the catalog on question so the manual-preview ordering below stays
     * deterministic after the transient touch expression. */
    state.emotion = NUWAX_EMOTION_QUESTION;
    nuwax_face_update(&state);

    expect_true("manual preview advances expression",
                nuwax_face_next_expression());
    expect_asset("manual preview selected next expression",
                 NUWAX_EMOTION_ASSET_DIZZINESS);
    state.battery_percent++;
    nuwax_face_update(&state);
    expect_asset("unrelated state refresh preserves manual preview",
                 NUWAX_EMOTION_ASSET_DIZZINESS);
    state.emotion = NUWAX_EMOTION_HAPPY;
    nuwax_face_update(&state);
    expect_asset("semantic state change releases manual preview",
                 NUWAX_EMOTION_ASSET_HAPPY);

    const unsigned touch_before = s_touch_count;
    state.dialog = NUWAX_DIALOG_READY;
    state.emotion = NUWAX_EMOTION_HAPPY;
    state.semantic_emotion_active = false;
    state.touch_sequence++;
    state.touch_emotion_active = true;
    nuwax_face_update(&state);
    expect_true("accepted touch sequence triggers transient",
                s_touch_count == touch_before + 1U);
    state.touch_emotion_active = false;
    nuwax_face_update(&state);
    expect_asset("touch deadline restores lifecycle face",
                 NUWAX_EMOTION_ASSET_HAPPY);

    s_player_set_result = ESP_FAIL;
    state.emotion = NUWAX_EMOTION_QUESTION;
    nuwax_face_update(&state);
    expect_true("state failure cleans animated backend",
                s_player_deinit_count == 1U);
    expect_true("state failure enters non-animated failsafe",
                !nuwax_face_next_expression());
    const unsigned ticks_after_failure = s_player_tick_count;
    const unsigned reaps_after_failure = s_player_reap_count;
    nuwax_face_tick();
    expect_true("failsafe performs no decoder work",
                s_player_tick_count == ticks_after_failure);
    expect_true("failsafe services quarantined player cleanup",
                s_player_reap_count == reaps_after_failure + 1U);
    expect_true("state-failure failsafe deinitializes",
                nuwax_face_deinit() == ESP_OK);

    s_player_set_result = ESP_OK;
    s_player_tick_result = ESP_FAIL;
    expect_true("animated backend restarts for decode test",
                nuwax_face_create(&parent) == ESP_OK);
    nuwax_face_tick();
    expect_true("decode failure cleans animated backend",
                s_player_deinit_count == 2U);
    expect_true("decode failure enters non-animated failsafe",
                !nuwax_face_next_expression());
    expect_true("decode-failure failsafe deinitializes",
                nuwax_face_deinit() == ESP_OK);

    s_player_init_result = ESP_FAIL;
    s_player_tick_result = ESP_OK;
    expect_true("startup failure preserves the rest of the UI",
                nuwax_face_create(&parent) == ESP_OK);
    expect_true("startup failure cannot invoke a retired backend",
                !nuwax_face_next_expression());
    expect_true("startup failsafe deinitializes",
                nuwax_face_deinit() == ESP_OK);

    s_player_init_result = ESP_OK;
    s_player_set_result = ESP_FAIL;
    s_player_deinit_result = ESP_FAIL;
    s_player_reap_result = ESP_ERR_INVALID_STATE;
    expect_true("animated backend starts for delayed cleanup",
                nuwax_face_create(&parent) == ESP_OK);
    state.emotion = NUWAX_EMOTION_ANGRY;
    nuwax_face_update(&state);
    const unsigned delayed_reap_before = s_player_reap_count;
    nuwax_face_tick();
    expect_true("delayed cleanup is serviced without decoder work",
                s_player_reap_count == delayed_reap_before + 1U);
    expect_true("UI teardown waits for quarantined worker",
                nuwax_face_deinit() == ESP_ERR_INVALID_STATE);
    s_player_reap_result = ESP_OK;
    expect_true("UI teardown completes after worker exit",
                nuwax_face_deinit() == ESP_OK);

    s_player_set_result = ESP_OK;
    s_player_deinit_result = ESP_OK;
    expect_true("animated backend can restart after delayed cleanup",
                nuwax_face_create(&parent) == ESP_OK);
    expect_true("post-cleanup animated backend deinitializes",
                nuwax_face_deinit() == ESP_OK);

    printf("emotion face policy host check: %s (%d failures)\n",
           s_failures ? "FAIL" : "PASS", s_failures);
    return s_failures ? 1 : 0;
}
