"""Fault injection against real pair/cloud sources and production owner functions.

The fake NVS persists each write/erase immediately, including before commit().
Only the hardware/NVS adapters are fake; boot recovery and APP finalization are
extracted from the current firmware, and the two persistence modules are built
unchanged. No target device or gateway is touched.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/provision-pair-transaction'
out.mkdir(parents=True, exist_ok=True)
prov = (root / 'main/nuwax_provision_new/nuwax_provision_main.c').read_text(encoding='utf-8-sig')


def function(signature):
    start = prov.rindex(signature)
    pos = prov.index('{', start)
    depth, end = 1, pos + 1
    while depth:
        depth += (prov[end] == '{') - (prov[end] == '}')
        end += 1
    return prov[start:end]


foundation = (root / 'tools/host-stubs/pair-transaction-platform.h').read_text()
(out / 'platform.h').write_text(foundation)
for header in ['esp_err.h', 'esp_wifi.h', 'esp_check.h', 'esp_log.h',
               'esp_crc.h', 'esp_random.h', 'nvs.h', 'wifi_manager.h']:
    (out / header).write_text('#include "platform.h"\n')
support = (root / 'tools/host-stubs/pair-transaction-support.h').read_text()
stubs, tests = support.split('/* PRODUCTION_FUNCTIONS */')
record_types = prov[prov.index('#define REPROVISION_NVS_NAMESPACE'):prov.index('static bool s_event_handlers_registered;')]
cancel_type = prov[prov.index('typedef enum {\n    PROVISION_CANCEL_'):prov.index('static bool s_provisioned_pair_ready;')]
source_dir = root / 'main/nuwax_provision_new'
code = '#include "platform.h"\n#include "nuwax_provision_policy.h"\n#include "nuwax_pair_commit.h"\n'
code += '\n'.join(line for line in prov.splitlines() if line.startswith('#define PERSIST_')) + '\n'
code += record_types + cancel_type + stubs
code += '\n#define TAG cloud_tag\n#define s_initialized cloud_initialized\n'
code += f'#include "{(source_dir / "nuwax_cloud_config.c").as_posix()}"\n'
code += '#undef TAG\n#undef s_initialized\n#define TAG pair_tag\n'
code += f'#include "{(source_dir / "nuwax_pair_commit.c").as_posix()}"\n'
code += '#undef TAG\n#define TAG "fixture"\n'
signatures = [
    'static uint32_t wifi_credentials_hash(',
    'static esp_err_t clear_wifi_verified_marker(',
    'static esp_err_t write_wifi_verified_marker(',
    'static bool wifi_verified_marker_matches(',
    'static bool force_reprovision_requested(',
    'static void set_force_reprovision(',
    'bool nuwax_provision_main_binding_required(',
    'static bool previous_pair_unusable(',
    'void nuwax_provision_main_note_binding_rejected(',
    'static void binding_replaced_by_committed_pair(',
    'bool nuwax_provision_main_request_runtime_cancel(',
    'static bool provisioning_cancel_requested(',
    'static esp_err_t clear_reprovision_record(',
    'static bool reprovision_record_can_restore(',
    'static esp_err_t save_reprovision_record(',
    'static esp_err_t load_reprovision_record(',
    'static esp_err_t recover_provisioned_pair(',
    'static esp_err_t restore_flash_credentials_after_candidate(',
    'static esp_err_t discard_uncommitted_pair_candidate(',
    'static esp_err_t retry_persist_operation(',
    'static esp_err_t retry_write_wifi_verified_marker(',
    'static esp_err_t finalize_old_wifi_recovery(',
    'static esp_err_t finalize_new_provisioning(',
    'esp_err_t nuwax_provision_main_reset_wifi_credentials(',
    'esp_err_t nuwax_provision_main_commit_wifi_credentials(',
    'esp_err_t nuwax_provision_main_rollback_wifi_credentials(',
]
code += '\n'.join(function(s) for s in signatures) + tests


def run(name, source, negative=False):
    path = out / (name + '.c')
    path.write_text(source, encoding='utf-8')
    exe = out / (name + '.exe')
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-unused-variable',
                    '-I' + str(out), '-I' + str(source_dir), str(path), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    (out / (name + '.log')).write_text(result.stdout + result.stderr)
    if negative:
        assert result.returncode != 0, 'Regression escaped: ' + name
        print('PASS: negative mutant rejected ' + name)
    else:
        print(result.stdout, end='')
        if result.returncode:
            print(result.stderr)
            raise SystemExit(result.returncode)


if __name__ == '__main__':
    run('transaction', code)
    for name, old, new in [
        ('lost-rejection', 'record.flags |= REPROVISION_RECORD_FLAG_BINDING_REQUIRED;', '(void)record;'),
        ('candidate-erases-live-intent', 'if (nuwax_pair_commit_pending()) return ESP_ERR_INVALID_STATE;', '(void)0;'),
        ('release-before-commit', 'ESP_RETURN_ON_ERROR(nuwax_pair_commit_complete(write_wifi_verified_marker,', 'binding_replaced_by_committed_pair();\n    ESP_RETURN_ON_ERROR(nuwax_pair_commit_complete(write_wifi_verified_marker,'),
    ]:
        mutated = code.replace(old, new)
        assert mutated != code
        run(name, mutated, negative=True)
    pair_include = f'#include "{(source_dir / "nuwax_pair_commit.c").as_posix()}"'
    pair_source = (source_dir / 'nuwax_pair_commit.c').read_text()
    late = pair_source.replace('ESP_RETURN_ON_ERROR(persist_target(), TAG, "persist pair intent failed");', '(void)0;')
    late = late.replace('    ESP_RETURN_ON_ERROR(mark_verified(&s_target.wifi)', '    ESP_RETURN_ON_ERROR(persist_target(), TAG, "late intent");\n    ESP_RETURN_ON_ERROR(mark_verified(&s_target.wifi)')
    run('late-intent', code.replace(pair_include, late), negative=True)
    reverse = pair_source.replace('nvs_erase_key(nvs, PAIR_REQUEST_KEY)', 'nvs_erase_key(nvs, SWAP_KEY)')
    reverse = reverse.replace('nvs_erase_key(nvs, PAIR_NVS_KEY)', 'nvs_erase_key(nvs, PAIR_REQUEST_KEY)')
    reverse = reverse.replace('nvs_erase_key(nvs, SWAP_KEY)', 'nvs_erase_key(nvs, PAIR_NVS_KEY)')
    run('retire-journal-first', code.replace(pair_include, reverse), negative=True)
    # Wiring checks ensure the tested helper is on the actual boot/start path.
    init = function('esp_err_t nuwax_provision_main_init(')
    assert init.index('recover_provisioned_pair()') < init.index('network_prov_mgr_init(')
    assert 'discard_uncommitted_pair_candidate()' in function('esp_err_t nuwax_provision_main_start(')
    supervisor = function('static nuwax_provision_outcome_t provision_main_run_internal(')
    assert supervisor.index('if (nuwax_pair_commit_pending()) continue;') < supervisor.index('if (snapshot.app_claim_pending')
    assert '(nuwax_pair_commit_pending() ||' in supervisor
    assert 'if (nuwax_pair_commit_pending())' in function('static esp_err_t reset_manager_for_app(')
    print('PASS: production init/start/supervisor/reset wiring keeps recovery ahead of cleanup')
