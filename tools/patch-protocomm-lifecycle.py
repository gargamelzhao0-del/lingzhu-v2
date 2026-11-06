"""Generate a project-local NimBLE transport; never modify the shared IDF."""
import argparse
import hashlib
import json
from pathlib import Path


def patch(source):
    def replace(old, new):
        nonlocal source
        if source.count(old) != 1:
            raise ValueError(f'Unreviewed protocomm source: expected one {old[:90]!r}')
        source = source.replace(old, new)

    replace('#include <stdbool.h>', '#include <stdbool.h>\n#include <stdatomic.h>')
    replace('static uint16_t s_cached_conn_handle;', '''static uint16_t s_cached_conn_handle;
static atomic_bool s_nuwax_ble_stopping = ATOMIC_VAR_INIT(true);
extern uint32_t nuwax_provision_success_query_revision(void) __attribute__((weak));
extern void nuwax_provision_success_response_read(uint32_t, uint16_t) __attribute__((weak));''')
    replace('    uint16_t attr_handle;\n};', '    uint16_t attr_handle;\n    uint32_t success_query_revision;\n};')
    replace('simple_ble_advertise(void)\n{\n    int rc;', '''simple_ble_advertise(void)
{
    if (atomic_load_explicit(&s_nuwax_ble_stopping, memory_order_acquire) ||
        !protocomm_ble_transport_active() || !ble_hs_is_enabled()) {
        return;
    }
    int rc;''')
    # A stop can start during a host API: check again before subsequent APIs,
    # and suppress errors only for teardown (normal advertising errors remain).
    for call in [
        '    rc = ble_gap_adv_rsp_set_fields((const struct ble_hs_adv_fields *) &resp_data);',
        '    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,']:
        replace(call, '    if (atomic_load_explicit(&s_nuwax_ble_stopping, memory_order_acquire)) {\n        return;\n    }\n' + call)
    for message in ['Error setting advertisement data; rc = %d', 'Error in setting scan response; rc = %d']:
        line = f'        ESP_LOGE(TAG, "{message}", rc);'
        replace(line, '        if (!atomic_load_explicit(&s_nuwax_ble_stopping, memory_order_acquire)) {\n    ' + line + '\n        }')
    replace('        if (rc == BLE_HS_EDISABLED) {', '        if (rc == BLE_HS_EDISABLED || atomic_load_explicit(&s_nuwax_ble_stopping, memory_order_acquire)) {')
    replace('    esp_err_t err = simple_ble_start(ble_config);', '''    atomic_store_explicit(&s_nuwax_ble_stopping, false, memory_order_release);
    ESP_LOGI(TAG, "provision transport revision=20260923-provision-finish-v1");
    esp_err_t err = simple_ble_start(ble_config);''')
    replace('        ESP_LOGE(TAG, "simple_ble_start failed w/ error code 0x%x", err);', '''        atomic_store_explicit(&s_nuwax_ble_stopping, true, memory_order_release);
        ESP_LOGE(TAG, "simple_ble_start failed w/ error code 0x%x", err);''')
    replace('    esp_err_t rc = ble_gap_adv_stop();', '''    atomic_store_explicit(&s_nuwax_ble_stopping, true, memory_order_release);
    esp_err_t rc = ble_gap_adv_stop();''')
    replace('''        rc = os_mbuf_append(ctxt->om, temp_outbuf, temp_outlen);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;''', '''        rc = os_mbuf_append(ctxt->om, temp_outbuf, temp_outlen);
        /* Only a response fitting one ATT read can shorten the fallback.
         * Long reads may need additional fragments; retain the full window. */
        if (rc == 0 && temp_outlen > 0 && protoble_internal->gatt_mtu > 1 &&
            temp_outlen <= protoble_internal->gatt_mtu - 1 &&
            nuwax_provision_success_response_read) {
            struct data_mbuf *response = find_attr_with_handle(attr_handle);
            if (response && response->success_query_revision != 0) {
                nuwax_provision_success_response_read(response->success_query_revision,
                                                      conn_handle);
            }
        }
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;''')
    replace('''        ret = protocomm_req_handle(protoble_internal->pc_ble,
                                   uuid128_to_handler(uuid),''', '''        const char *endpoint = uuid128_to_handler(uuid);
        const uint32_t before_revision = nuwax_provision_success_query_revision
            ? nuwax_provision_success_query_revision() : 0;
        const bool status_endpoint = endpoint && strcmp(endpoint, "prov-config") == 0;
        /* A failed request must never leave an old success tag readable. */
        struct data_mbuf *old_response = find_attr_with_handle(attr_handle);
        if (old_response) {
            old_response->success_query_revision = 0;
        }
        ret = protocomm_req_handle(protoble_internal->pc_ble,
                                   endpoint,''')
    replace('''            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to set outbuf for characteristic with attr_handle = %d",''', '''            if (rc == 0) {
                struct data_mbuf *response = find_attr_with_handle(attr_handle);
                const uint32_t after_revision = nuwax_provision_success_query_revision
                    ? nuwax_provision_success_query_revision() : 0;
                if (response) {
                    response->success_query_revision = status_endpoint &&
                        after_revision != before_revision ? after_revision : 0;
                }
            }
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to set outbuf for characteristic with attr_handle = %d",''')
    return source


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        raise ValueError('Refusing to modify shared SDK source')
    original = args.input.read_bytes()
    result = patch(original.decode('utf-8').replace('\r\n', '\n'))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(result, encoding='utf-8', newline='\n')
    args.output.with_suffix('.json').write_text(json.dumps({
        'revision': '20260923-provision-finish-v1',
        'input': str(args.input), 'input_sha256': hashlib.sha256(original).hexdigest(),
        'output_sha256': hashlib.sha256(args.output.read_bytes()).hexdigest(),
    }, indent=2), encoding='utf-8')
