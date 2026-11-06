"""Execute current retry/UI functions, with a persistent SD-init fault and recovery controls.

Only audit outputs are written. This is a host fault-injection test, not a board test.
"""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'main/nuwax_ai_partner_main.c').is_file())
OUT = Path(__file__).resolve().parent
MAIN = ROOT / "main/nuwax_ai_partner_main.c"
SPOOL = ROOT / "main/offline_audio_spool.c"
source = MAIN.read_text(encoding="utf-8-sig")


def function(signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


signatures = [
    "static void reset_offline_capture_retry_state(void)\n",
    "static nuwax_audio_mode_identity_t *offline_mode_identity(void)\n",
    "static bool service_offline_duration_limit(void)\n",
    "static void service_offline_capture_retry_disconnected(void)\n",
    "static void sync_exclusive_audio_mode_ui_state(void) {",
]
production = "\n\n".join(function(s) for s in signatures)
(OUT / "production_retry_ui.inc").write_text(production, encoding="utf-8")
definitions = "\n".join(re.findall(r"^#define NUWAX_OFFLINE_CAPTURE_RETRY_(?:INITIAL|MAX)_MS .*", source, re.M))
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE 1
#define CONFIG_NUWAX_OTA_ENABLE 0
#define CONFIG_NUWAX_VOICE_SAMPLE_RATE 16000U
#define CONFIG_NUWAX_VOICE_CHANNELS 1U
#define CONFIG_NUWAX_VOICE_FRAME_DURATION_MS 20U
#define NUWAX_DEVICE_ERROR_RESULT_BYTES 160U
#define NUWAX_OFFLINE_AUDIO_MODE_MEETING 0
#define NUWAX_OFFLINE_AUDIO_MODE_MONITORING 1
#define TAG "audit"
static void ignored_log(const char *tag,const char *fmt,...) {(void)tag;(void)fmt;}
#define ESP_LOGI ignored_log
#define ESP_LOGW ignored_log
typedef enum { NUWAX_MEETING_STATE_DISABLED, NUWAX_MEETING_STATE_STARTING,
    NUWAX_MEETING_STATE_ACTIVE, NUWAX_MEETING_STATE_STOPPING, NUWAX_MEETING_STATE_ERROR } nuwax_meeting_state_t;
typedef enum { NUWAX_MONITORING_STATE_DISABLED, NUWAX_MONITORING_STATE_STARTING,
    NUWAX_MONITORING_STATE_ACTIVE, NUWAX_MONITORING_STATE_STOPPING, NUWAX_MONITORING_STATE_ERROR } nuwax_monitoring_state_t;
typedef nuwax_meeting_state_t nuwax_meeting_ui_state_t;
typedef nuwax_monitoring_state_t nuwax_monitoring_ui_state_t;
typedef struct {uint32_t next_capture_sequence; bool resuming; char session_id[48];} nuwax_audio_mode_identity_t;
typedef struct {int state; nuwax_audio_mode_identity_t identity;} mode_t;
static mode_t s_meeting_mode, s_monitoring_mode;
static atomic_uint s_ui_lifecycle_generation;
static atomic_bool s_offline_mode_capture_failed,s_offline_mode_cancel_to_sleep,s_reprovision_quiesce_requested;
static char s_offline_mode_resume_session[48]="audit_original_session";
static bool s_offline_mode_capture_started;
static uint32_t s_offline_capture_retry_next_ms,s_offline_capture_retry_backoff_ms,s_offline_capture_retry_attempts;
static bool lease, capture, expired, meeting_resume,monitoring_resume;
static int mode_kind,init_error,last_meeting,last_monitoring;
static uint32_t clock_ms,init_calls,start_calls,warnings;
static bool offline_mode_lease_pending(void){return lease;}
static void offline_mode_lease_store(bool value){lease=value;}
static int offline_mode_kind(void){return mode_kind;}
static bool nuwax_offline_audio_capture_running(void){return capture;}
static bool nuwax_offline_audio_limit_reached(void){return expired;}
static uint32_t nuwax_offline_audio_next_capture_sequence(void){return 42;}
static void nuwax_audio_mode_identity_clear(nuwax_audio_mode_identity_t *identity){memset(identity,0,sizeof(*identity));}
static uint32_t monotonic_ms(void){return clock_ms;}
static bool elapsed_ms(uint32_t now,uint32_t deadline){return (int32_t)(now-deadline)>=0;}
static esp_err_t nuwax_offline_audio_init(void){init_calls++;return init_error;}
static esp_err_t nuwax_offline_audio_start_capture_continuation(int mode,const char *session,uint32_t rate,uint8_t ch,uint16_t frame,uint32_t seq){
    assert(mode==mode_kind && session[0] && rate==16000 && ch==1 && frame==20);
    assert(seq==0 || seq==42);start_calls++;capture=true;return ESP_OK;
}
typedef struct {unsigned internal_free,internal_largest,dma_largest;} nuwax_audio_runtime_memory_t;
static nuwax_audio_runtime_memory_t exclusive_audio_memory_snapshot(void){return (nuwax_audio_runtime_memory_t){100000,50000,30000};}
static const char *esp_err_to_name(esp_err_t err){return err?"INJECTED_MISSING_SD":"ESP_OK";}
static bool nuwax_device_warning_report_enqueue_detail(const char *component,const char *code,esp_err_t err,const char *detail,const char *state,uint32_t generation,const char *turn){
    assert(!strcmp(component,"audio") && !strcmp(code,"offline_capture_retry") && err && detail && !state && !generation && !turn);warnings++;return true;
}
static bool ui_ready_snapshot(void){return true;}
static nuwax_meeting_state_t nuwax_meeting_mode_state(const mode_t *m){return m->state;}
static nuwax_monitoring_state_t nuwax_monitoring_mode_state(const mode_t *m){return m->state;}
static bool nuwax_meeting_mode_offline_resume_pending(const mode_t *m){(void)m;return meeting_resume;}
static bool nuwax_monitoring_mode_offline_resume_pending(const mode_t *m){(void)m;return monitoring_resume;}
static void nuwax_app_shell_set_meeting_state(nuwax_meeting_ui_state_t state){last_meeting=state;}
static void nuwax_app_shell_set_monitoring_state(nuwax_monitoring_ui_state_t state){last_monitoring=state;}
#include "production_retry_ui.inc"
int main(void){
 for(mode_kind=0;mode_kind<2;mode_kind++){
  lease=true;capture=false;expired=false;meeting_resume=monitoring_resume=false;
  init_error=ESP_FAIL;clock_ms=1;init_calls=start_calls=warnings=0;
  s_meeting_mode.state=s_monitoring_mode.state=0;
  s_offline_mode_capture_started=false;
  strcpy(s_offline_mode_resume_session,"audit_original_session");
  reset_offline_capture_retry_state();
  atomic_fetch_add(&s_ui_lifecycle_generation,1);
  for(unsigned tick=0;tick<6000;tick++,clock_ms+=100){
    service_offline_capture_retry_disconnected();
    sync_exclusive_audio_mode_ui_state();
    assert(lease && !capture && !start_calls);
    assert(atomic_load(&s_offline_mode_capture_failed));
    assert(mode_kind?last_monitoring==NUWAX_MONITORING_STATE_ACTIVE:last_meeting==NUWAX_MEETING_STATE_ACTIVE);
  }
  printf("CONFIRMED mode=%s duration_ms=600000 missing_SD init_retries=%u warning_enqueue_calls=%u capture_starts=%u UI_ACTIVE=1 failure_flag=1\n",mode_kind?"monitoring":"meeting",init_calls,warnings,start_calls);
  init_error=ESP_OK;clock_ms+=6000;
  service_offline_capture_retry_disconnected();sync_exclusive_audio_mode_ui_state();
  assert(capture && start_calls==1 && !atomic_load(&s_offline_mode_capture_failed));
  puts("CONTROL media_restored: retry launches capture and clears failure; earlier missing interval cannot be recovered");
  capture=false;expired=true;
  service_offline_capture_retry_disconnected();sync_exclusive_audio_mode_ui_state();
  assert(!lease && !last_meeting && !last_monitoring);
  puts("CONTROL explicit_duration_expiry: lease cleared and modes display DISABLED");
 }
}
'''
(OUT / "persistent_missing_sd.c").write_text(definitions + "\n" + harness, encoding="utf-8")
command = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(OUT),
           str(OUT / "persistent_missing_sd.c"), "-o", str(OUT / "persistent_missing_sd.exe")]
build = subprocess.run(command, capture_output=True, text=True)
(OUT / "persistent-missing-sd-build.log").write_text(build.stdout+build.stderr, encoding="utf-8")
build.check_returncode()
run = subprocess.run([str(OUT / "persistent_missing_sd.exe")], capture_output=True, text=True)
(OUT / "persistent-missing-sd.log").write_text(run.stdout+run.stderr, encoding="utf-8")
manifest = {
    "tested_at_utc": datetime.now(timezone.utc).isoformat(),
    "evidence_level": "production retry, expiry, identity, UI functions executed with deterministic SD fault injection",
    "source_sha256": {str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in (MAIN,SPOOL,ROOT/'main/device_error_report.c')},
    "extracted_functions": signatures,
    "assumptions": [
        "Initial lease exists after an online authorized meeting/monitoring disconnect; current main lines 34975-35013 explicitly retain it despite init failure.",
        "No card causes SD mount/recovery to fail: production init returns media_result at offline_audio_spool.c:1581; test injects that result, not SD hardware.",
        "No deliberate stop, expiry, OTA or reprovision; real user stop and duration expiry are legitimate reasons to clear lease.",
        "Warning enqueue accepts every call in this harness; real queue deduplicates/drops advisory warnings and cannot transmit while disconnected.",
        "No capture has begun, so offline MIC-ownership time budget is not consumed; test does not treat monitoring VAD silence as failed capture.",
        "Clock ticks are deterministic and do not measure actual scheduler or hardware latency."
    ],
    "run_exit_code":run.returncode,
    "production_source_modified":False,
}
(OUT / "persistent-missing-sd-evidence.json").write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
print(run.stdout,end="")
run.check_returncode()
