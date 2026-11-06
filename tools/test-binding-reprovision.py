"""Run production binding/rollback functions with NVS faults and real SD controllers.

No device, APP, gateway or installed SDK is modified. Generated files stay in
build-host-checks/binding-reprovision. NVS writes are individually durable,
including cuts before nvs_commit, matching the boundary that matters here.
"""
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/binding-reprovision'
out.mkdir(parents=True, exist_ok=True)
app = (root / 'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')
prov = (root / 'main/nuwax_provision_new/nuwax_provision_main.c').read_text(encoding='utf-8-sig')
compat = (root / 'main/cloud_protocol_compat.c').read_text(encoding='utf-8-sig')
ws_source = (root / 'main/ws_transport.c').read_text(encoding='utf-8-sig')


def function(source, signature):
    start = source.rindex(signature)
    pos = source.index('{', start)
    depth, end = 1, pos + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def run(name, code, includes=(), sources=(), flags=(), negative=False):
    src, exe = out / (name + '.c'), out / (name + '.exe')
    src.write_text(code, encoding='utf-8')
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-unused-parameter',
                    *flags, *['-I' + str(p) for p in includes],
                    str(src), *map(str, sources), '-o', str(exe)], check=True)
    if negative:
        result = subprocess.run([str(exe)], capture_output=True)
        assert result.returncode != 0, 'Regression escaped: ' + name
        print('PASS: negative mutant rejected ' + name)
    else:
        subprocess.run([str(exe)], check=True)


# The persistence fixture now compiles both complete production storage modules,
# with repeated boot/sanitation cuts instead of a simplified cloud-key stub.
import sys
subprocess.run([sys.executable, str(root / 'tools/test-provision-pair-transaction.py')], check=True)

# Exact wire reason, current/stale epochs, no RX persistence, owner admission.
close_support = (root / 'tools/host-stubs/binding-close-support.h').read_text(encoding='utf-8')
prefix, tests = close_support.split('/* PRODUCTION_FUNCTIONS */')
close_code = prefix + function(compat, 'bool nuwax_cloud_peer_close_requires_provision(')
close_code += '\n' + function(app, 'static bool mark_cloud_provision_required_if_epoch(')
close_code += '\n' + function(app, 'static void on_ws_peer_close(')
close_code += '\n' + function(app, 'static bool cloud_connect_cancelled_for_reprovision(')
close_code += '\n' + function(app, 'static bool service_cloud_binding_reprovision(void)')
start = ws_source.index('            const uint16_t close_code =', ws_source.index('} else if (opcode == 0x8)'))
end = ws_source.index('        } else if (opcode == 0x9)', start)
close_code += '\nstatic const char *receive_close(nuwax_ws_client_t *client,const uint8_t *payload,size_t payload_size) {\n'
close_code += 'const char *stop_reason=NULL; do {\n' + ws_source[start:end] + '\n}while(0); return stop_reason; }\n'
run('close', close_code + tests, [root / 'main'])
# OTA-enabled owner routing: a pending-image recovery is owned by the OTA
# controller, while normal rebind still uses the existing BLE drain/park path.
ota_prefix = '''static bool ota_recovery_accept;
static unsigned ota_recovery_requests;
static void nuwax_ota_device_set_cloud_authenticated(bool ready,uint32_t epoch) {
    assert(!ready && !epoch);
}
static bool nuwax_ota_device_request_binding_recovery(void) {
    ++ota_recovery_requests; return ota_recovery_accept;
}
'''
ota_code = close_code.replace('#define CONFIG_NUWAX_OTA_ENABLE 0',
    '#define CONFIG_NUWAX_OTA_ENABLE 1\n' + ota_prefix)
ota_tests = tests.replace('now_ms=10000;admit=false;', '''now_ms=8000;admit=false;ota_recovery_accept=true;
    assert(service_cloud_binding_reprovision() && ota_recovery_requests==1 && requests==0);
    assert(service_cloud_binding_reprovision() && ota_recovery_requests==1 && requests==0);
    assert(binding_required && s_cloud_provision_required_epoch==5);
    ota_recovery_accept=false;now_ms=10000;admit=false;''')
assert ota_code != close_code and ota_tests != tests
run('close-ota', ota_code + ota_tests, [root / 'main'])
mutant = (close_code + tests).replace('memory_order_acquire)) return false;', 'memory_order_acquire)) return true;', 1)
assert mutant != close_code + tests
run('starved-owner', mutant, [root / 'main'], negative=True)

# Compile the actual owner helper and both actual production controllers.
for name, other in [('meeting', 'monitoring'), ('monitoring', 'meeting')]:
    mode_dir = root / 'main/audio_modes' / name
    definitions = {'MODE_TYPE': f'nuwax_{name}_mode_t', 'CALLBACK_TYPE': f'nuwax_{name}_mode_callbacks_t'}
    for macro, suffix in [('MODE_INIT', 'init'), ('MODE_DEINIT', 'deinit'), ('MODE_UI', 'request_ui'),
                          ('MODE_SERVICE', 'service_sd'), ('MODE_RUN', 'run'), ('MODE_DISCONNECT', 'notify_ws_disconnected')]:
        definitions[macro] = f'nuwax_{name}_mode_{suffix}'
    definitions['MODE_NAME'] = '"' + name + '"'
    definitions['MODE_TEST_MEETING'] = '1' if name == 'meeting' else '0'
    for state in ('STARTING', 'ACTIVE', 'STOPPING', 'DISABLED'):
        definitions['STATE_' + state] = f'NUWAX_{name.upper()}_STATE_{state}'
    code = '#define main legacy_main\n#define nuwax_ws_send_text sd_base_send_text\n'
    code += f'#include "{(mode_dir / (name + "_mode_host_check.c")).as_posix()}"\n'
    code += '#undef main\n#undef nuwax_ws_send_text\nstatic bool sd_fail_command_ack;\n'
    code += 'esp_err_t nuwax_ws_send_text(nuwax_ws_client_t *ws,const char *p){return sd_base_send_text(ws,p);}\n'
    code += '\n'.join(f'#define {k} {v}' for k, v in definitions.items())
    code += '\n#define main existing_sd_main\n#include "sd_audio_mode_host_check.inc"\n#undef main\n'
    code += f'''static MODE_TYPE s_{name}_mode;
static int s_{other}_mode;
#define NUWAX_{other.upper()}_STATE_DISABLED 0
#define NUWAX_{other.upper()}_STATE_STOPPING 3
static int nuwax_{other}_mode_state(void *m) {{ return 0; }}
static bool nuwax_{other}_mode_has_pending(void *m) {{ return false; }}
static int nuwax_{other}_mode_request_ui(void *m,void *w,bool e) {{ assert(!e); return 0; }}
static void nuwax_{other}_mode_notify_ws_disconnected(void *m) {{}}
static void nuwax_{other}_mode_service_sd(void *m,void *w) {{}}
static bool lease, replay;
static bool offline_mode_lease_pending(void) {{ return lease; }}
static void offline_mode_lease_store(bool value) {{ lease = value; }}
bool nuwax_offline_audio_is_active(void) {{ return card_active; }}
bool nuwax_offline_audio_replay_running(void) {{ return replay; }}
int nuwax_offline_audio_request_stop(bool complete) {{ assert(complete); return 0; }}
'''
    code += function(app, 'static bool reprovision_audio_mode_busy(void)') + '\n'
    code += function(app, 'static void service_reprovision_audio_modes(void)') + '\n'
    code += f'''int main(void) {{
    MODE_TYPE *m = &s_{name}_mode;
    const CALLBACK_TYPE callbacks = {{.can_start=can_start,.on_state_changed=on_state_changed}};
    for (int stage=0; stage<4; ++stage) {{
        card_available=card_detected=true;card_active=capture_live=false;
        card_health=NUWAX_OFFLINE_CAPTURE_RECORDING;
        assert(MODE_INIT(m,&callbacks)==ESP_OK);
        assert(MODE_UI(m,NULL,true)==ESP_OK);
        if(stage!=0) MODE_SERVICE(m,NULL);
        if(stage==2) MODE_DISCONNECT(m);
        if(stage==3) {{ capture_live=false;card_active=false; }}
        lease=true; replay=true;
        service_reprovision_audio_modes();
        assert(!lease && reprovision_audio_mode_busy());
        if(stage==1 || stage==2) {{
            assert(m->state==STATE_STOPPING && card_active);
            for(int i=0;i<20;i++) service_reprovision_audio_modes();
            assert(m->state==STATE_STOPPING && card_active);
        }}
        card_active=false;capture_live=false;
        service_reprovision_audio_modes();
        assert(m->state==STATE_DISABLED && reprovision_audio_mode_busy());
        replay=false;service_reprovision_audio_modes();
        assert(!reprovision_audio_mode_busy() && !m->sd_local && !m->identity.resuming);
        assert(!m->offline_resume_required);
        MODE_DEINIT(m);
    }}
    puts("PASS: {name} owner drains STARTING/ACTIVE/disconnected/already-stopped capture, waits for writer and replay, never resumes");
    return 0;
}}
'''
    sources = [mode_dir / f'{name}_mode.c', mode_dir / f'{name}_protocol.c',
               root / 'main/audio_modes/meeting/host_stubs/device_error_report.c',
               root / 'managed_components/espressif__cjson/cJSON/cJSON.c']
    if name == 'meeting': sources.append(mode_dir / 'meeting_processor.c')
    run(name, code, [root / 'main/audio_modes/meeting/host_stubs', mode_dir, root / 'main',
                    root / 'main/conversation', root / 'managed_components/espressif__cjson/cJSON'],
        sources, ['-DCONFIG_NUWAX_SD_SESSION_ENABLE=1', '-DCONFIG_NUWAX_OFFLINE_AUDIO_ENABLE=1', '-DCONFIG_NUWAX_SD_LEGACY_CONTROL_COMPAT=0'])
    if name == 'meeting':
        mutant = code.replace('nuwax_meeting_mode_service_sd(&s_meeting_mode, NULL);', '(void)0;', 1)
        assert mutant != code
        run('parked-without-drain', mutant,
            [root / 'main/audio_modes/meeting/host_stubs', mode_dir, root / 'main',
             root / 'main/conversation', root / 'managed_components/espressif__cjson/cJSON'],
            sources, ['-DCONFIG_NUWAX_SD_SESSION_ENABLE=1', '-DCONFIG_NUWAX_OFFLINE_AUDIO_ENABLE=1', '-DCONFIG_NUWAX_SD_LEGACY_CONTROL_COMPAT=0'], negative=True)

# Wiring invariants supplement the executable tests above.
ws = (root / 'main/ws_transport.c').read_text(encoding='utf-8')
assert 'client->on_peer_close = config->on_peer_close;' in ws
assert '.on_peer_close = on_ws_peer_close,' in app
park = app[app.index('const esp_err_t park_close_err'):app.index('"cloud owner parked; BLE provisioning owns network"')]
assert park.index('service_reprovision_audio_modes();') < park.index('if (reprovision_audio_mode_busy())') < park.index('&s_cloud_owner_parked, true')
assert 'nuwax_offline_audio_request_stop(false)' not in park
worker = function(app, 'static void flash_ops_task(void *arg)')
assert 'reprovision_audio_mode_busy()' not in worker
assert 'cloud_owner_parked = atomic_load_explicit(\n                    &s_cloud_owner_parked, memory_order_acquire)' in worker
print('PASS: transport callback wiring and owner park ordering')
