/* Host-only checks for complete hello construction on new connections. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "session_protocol.h"
#include "board/audio_board.h"
#include "cJSON.h"

static char wire[4096];
static size_t calls, live, fail_at;
static unsigned sends;
static esp_err_t send_result;

static void *test_malloc(size_t n) {
    if (++calls == fail_at) return NULL;
    void *p = malloc(n);
    if (p) ++live;
    return p;
}
static void test_free(void *p) { if (p) { assert(live); --live; free(p); } }
const char *nuwax_device_uid_string(void) { return "test-device"; }
const nuwax_audio_board_caps_t *nuwax_audio_board_get_caps(void) {
    static const nuwax_audio_board_caps_t caps = {.hardware_profile = "host"};
    return &caps;
}
nuwax_hardware_ref_health_t nuwax_audio_board_get_hardware_ref_health(void) {
    return NUWAX_HARDWARE_REF_HEALTH_UNTESTED;
}
esp_err_t nuwax_ws_send_text(nuwax_ws_client_t *ws, const char *payload) {
    assert(ws && strlen(payload) < sizeof(wire));
    ++sends;
    strcpy(wire, payload);
    return send_result;
}
esp_err_t nuwax_ws_send_text_until_if_epoch(nuwax_ws_client_t *ws, const char *payload,
                                            int64_t deadline, uint32_t expected) {
    (void)ws; (void)payload; (void)deadline; (void)expected;
    assert(false); return ESP_FAIL;
}
/* Other session_protocol entry points are linked on MinGW, but are not used
 * in this test. Fail if a hello unexpectedly takes any of these paths. */
esp_err_t nuwax_ws_send_text_until(nuwax_ws_client_t *ws, const char *p, int64_t d) {
    (void)ws; (void)p; (void)d; assert(false); return ESP_FAIL;
}
esp_err_t nuwax_ws_send_keepalive_until_if_epoch(nuwax_ws_client_t *ws,
    const char *p, int64_t d, uint32_t e) {
    (void)ws; (void)p; (void)d; (void)e; assert(false); return ESP_FAIL;
}
esp_err_t nuwax_ws_send_text_with_timeout_if_epoch(nuwax_ws_client_t *ws,
    const char *p, uint32_t d, uint32_t e) {
    (void)ws; (void)p; (void)d; (void)e; assert(false); return ESP_FAIL;
}
bool nuwax_ws_heartbeat_healthy_if_epoch(const nuwax_ws_client_t *ws, uint32_t e) {
    (void)ws; (void)e; assert(false); return false;
}
nuwax_cloud_message_kind_t nuwax_cloud_classify_message(const char *p, int len, const char *id) {
    (void)p; (void)len; (void)id; assert(false); return NUWAX_CLOUD_MESSAGE_NONE;
}
uint32_t nuwax_ws_connection_epoch(const nuwax_ws_client_t *ws) { (void)ws; return 9U; }
bool nuwax_ws_is_connected(const nuwax_ws_client_t *ws) { return ws != NULL; }
int64_t esp_timer_get_time(void) { return 0; }

static void check_wire(bool card) {
    cJSON *r = cJSON_Parse(wire);
    assert(r);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(r, "has_sd_card");
    assert(cJSON_IsBool(v) && cJSON_IsTrue(v) == card);
    assert(!strcmp(cJSON_GetObjectItem(r, "type")->valuestring, "hello"));
    assert(!strcmp(cJSON_GetObjectItem(r, "device_secret")->valuestring, "test-secret"));
    assert(cJSON_IsObject(cJSON_GetObjectItem(r, "features")));
    assert(cJSON_IsObject(cJSON_GetObjectItem(r, "audio_params")));
    cJSON_Delete(r);
    assert(!live);
}

#include "hello-connection-check.inc"

int main(void) {
    cJSON_Hooks hooks = {.malloc_fn = test_malloc, .free_fn = test_free};
    cJSON_InitHooks(&hooks);
    nuwax_ws_client_t ws = {0};
    assert(nuwax_session_send_hello(&ws, "test-secret", true) == ESP_OK);
    const size_t build_allocations = calls;
    check_wire(true);
    assert(nuwax_session_send_hello(&ws, "test-secret", false) == ESP_OK);
    check_wire(false);
    const unsigned old_sends = sends;
    assert(nuwax_session_send_hello(NULL, "test-secret", true) == ESP_ERR_INVALID_ARG);
    assert(nuwax_session_send_hello(&ws, NULL, true) == ESP_ERR_INVALID_ARG);
    assert(nuwax_session_send_hello(&ws, "", true) == ESP_ERR_INVALID_ARG);
    assert(sends == old_sends && !live);
    send_result = ESP_ERR_TIMEOUT;
    assert(nuwax_session_send_hello(&ws, "test-secret", false) == ESP_ERR_TIMEOUT);
    assert(!live);
    send_result = ESP_OK;
    for (size_t i = 1; i <= build_allocations; ++i) {
        calls = 0; fail_at = i;
        const unsigned before = sends;
        assert(nuwax_session_send_hello(&ws, "test-secret", true) == ESP_ERR_NO_MEM);
        assert(sends == before && !live); /* no partial hello, no leak */
    }
    fail_at = 0;
    check_connection_hello();
    printf("PASS: connection hello SD true/false, %zu allocation failures, no partial message or leak, send failure propagation\n", build_allocations);
}
