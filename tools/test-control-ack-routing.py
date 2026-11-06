"""Execute the production capture-aware ACK routing function without a device."""
from pathlib import Path
import ctypes
import subprocess
import tempfile

from c_source_extract import function

if hasattr(ctypes, "windll"):
    ctypes.windll.kernel32.SetErrorMode(0x8003)

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "main/nuwax_ai_partner_main.c").read_text(encoding="utf-8-sig")
ROUTE = function(MAIN, "session_dispatch_send_ack")

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "session_protocol.h"

static bool capture_busy;
static unsigned enqueue_calls, init_calls, sync_calls;
static esp_err_t enqueue_result, init_result, sync_result;
static int64_t now_us, init_elapsed_us;
static nuwax_ws_client_t *expected_ws;
static const nuwax_session_command_t *expected_command;
static const char *expected_message;
static bool expected_ok;
static uint32_t expected_epoch;

static bool conversation_capture_busy_for_sleep(void) { return capture_busy; }
static int64_t esp_timer_get_time(void) { return now_us; }

static esp_err_t nuwax_control_ack_tx_init(void) {
    assert(!capture_busy); /* Never allocate/create a worker on live capture. */
    ++init_calls;
    now_us += init_elapsed_us;
    return init_result;
}

static esp_err_t nuwax_control_ack_tx_enqueue(
    nuwax_ws_client_t *ws, const nuwax_session_command_t *command,
    bool ok, const char *message, uint32_t epoch) {
    assert(capture_busy);
    assert(ws == expected_ws && command == expected_command);
    assert(ok == expected_ok && message == expected_message);
    assert(epoch == expected_epoch);
    ++enqueue_calls;
    return enqueue_result;
}

esp_err_t nuwax_session_send_command_ack_until_if_epoch(
    nuwax_ws_client_t *ws, const char *request_id, const char *command,
    bool ok, const char *message, int64_t deadline_us, uint32_t epoch) {
    assert(!capture_busy);
    assert(ws == expected_ws);
    assert(request_id == expected_command->request_id);
    assert(command == expected_command->command);
    assert(ok == expected_ok && message == expected_message);
    assert(epoch == expected_epoch);
    assert(deadline_us == now_us + 500000LL);
    ++sync_calls;
    return sync_result;
}
'''

TESTS = r'''
int main(void) {
    nuwax_ws_client_t ws = {0};
    nuwax_session_command_t command = {0};
    snprintf(command.request_id, sizeof(command.request_id), "%s", "request_routing_42");
    snprintf(command.command, sizeof(command.command), "%s", "set_emotion");
    expected_ws = &ws;
    expected_command = &command;
    expected_epoch = 42U;
    now_us = 1000000LL;
    const esp_err_t admissions[] = {
        ESP_OK, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM,
    };
    capture_busy = true;
    for (unsigned i = 0U; i < sizeof(admissions) / sizeof(admissions[0]); ++i) {
        for (unsigned ok = 0U; ok < 2U; ++ok) {
            expected_message = ok ? "accepted" : NULL;
            expected_ok = ok != 0U;
            enqueue_result = admissions[i];
            assert(session_dispatch_send_ack(
                &ws, &command, expected_ok, expected_message, expected_epoch) ==
                enqueue_result);
            assert(init_calls == 0U && sync_calls == 0U);
            assert(now_us == 1000000LL);
        }
    }
    assert(enqueue_calls == 8U);

    /* Startup may run out of memory. Idle traffic retries worker creation but
     * keeps the existing synchronous ACK available even while retry fails. */
    capture_busy = false;
    init_result = ESP_ERR_NO_MEM;
    init_elapsed_us = 17000LL;
    expected_ok = false;
    expected_message = "rejected";
    sync_result = ESP_OK;
    assert(session_dispatch_send_ack(
        &ws, &command, expected_ok, expected_message, expected_epoch) == ESP_OK);
    assert(init_calls == 1U && sync_calls == 1U && enqueue_calls == 8U);

    init_result = ESP_OK;
    init_elapsed_us = 0LL;
    expected_ok = true;
    expected_message = NULL;
    expected_epoch = 43U;
    sync_result = ESP_ERR_TIMEOUT;
    assert(session_dispatch_send_ack(
        &ws, &command, expected_ok, expected_message, expected_epoch) == ESP_ERR_TIMEOUT);
    assert(init_calls == 2U && sync_calls == 2U && enqueue_calls == 8U);

    capture_busy = true;
    enqueue_result = ESP_OK;
    assert(session_dispatch_send_ack(
        &ws, &command, expected_ok, expected_message, expected_epoch) == ESP_OK);
    assert(init_calls == 2U && sync_calls == 2U && enqueue_calls == 9U);
    puts("PASS: production ACK routing preserves capture isolation, idle init retry, identity and 500ms epoch-bound send");
    return 0;
}
'''

mutations = {
    "capture_sync_fallback": (
        "return nuwax_control_ack_tx_enqueue(ws, command, ok, message, epoch);",
        "(void)nuwax_control_ack_tx_enqueue(ws, command, ok, message, epoch);",
    ),
    "missing_idle_init_retry": ("(void)nuwax_control_ack_tx_init();", ""),
    "wrong_epoch": ("message, epoch);", "message, 0U);"),
    "wrong_sync_deadline": ("+ 500000LL, epoch);", "+ 50000LL, epoch);"),
}

with tempfile.TemporaryDirectory(prefix="control-ack-routing-") as directory:
    source = Path(directory) / "routing.c"
    executable = source.with_suffix(".exe")
    for name in ["production", *mutations]:
        body = ROUTE
        if name != "production":
            before, after = mutations[name]
            assert before in body, f"mutation anchor missing: {name}"
            body = body.replace(before, after, 1)
        source.write_text(PREFIX + body + TESTS, encoding="utf-8")
        compiled = subprocess.run([
            "gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function",
            "-I", str(ROOT / "main/session_protocol_host_stubs"),
            "-I", str(ROOT / "main"), str(source), "-o", str(executable),
        ], capture_output=True, text=True)
        if compiled.returncode:
            raise RuntimeError(compiled.stdout + compiled.stderr)
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
        if (result.returncode == 0) != (name == "production"):
            raise RuntimeError(name + "\n" + result.stdout + result.stderr)
        print(result.stdout.strip() if name == "production" else "PASS: rejected " + name)

startup = function(MAIN, "app_main")
init = startup.index("nuwax_control_ack_tx_init()")
registration = startup.index("nuwax_session_dispatch_set_ack_sender(session_dispatch_send_ack)")
assert startup.index("nuwax_ws_init(&ws)") < init < registration
assert registration < startup.index("atomic_store_explicit(&s_cloud_owner_ready, true")
print("PASS: startup initializes ACK worker and registers capture-aware route before cloud admission")
