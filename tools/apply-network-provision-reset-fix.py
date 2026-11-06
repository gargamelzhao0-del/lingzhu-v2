"""Reapply the scoped local fix after managed components are regenerated."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
path = root / 'managed_components/espressif__network_provisioning/src/manager.c'
source = path.read_text(encoding='utf-8-sig')
a = source.index('esp_err_t network_prov_mgr_reset_wifi_sm_state_for_reprovision(void)')
b = source.index('\n#endif', a)
old = source[a:b]
new = old
if '    /* Quiesce authentication before replacing credentials.' not in new:
    config = '    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg_empty);'
    disconnect = '''    ret = esp_wifi_disconnect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disconnect wifi, 0x%x", ret);
        goto exit;
    }

'''
    assert new.count(config) == 1 and disconnect in new, 'Unknown manager reset; review upstream'
    new = new.replace(disconnect, '')
    new = new.replace(config, '''    /* Quiesce authentication before replacing credentials. Holding the
     * manager lock prevents its disconnect callback from restarting STA. */
    ret = esp_wifi_disconnect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGE(TAG, "Failed to disconnect wifi before reset, 0x%x", ret);
        goto exit;
    }
    prov_ctx->prov_state = NETWORK_PROV_STATE_STARTED;
    prov_ctx->connection_attempts_completed = 0;
''' + config)
    new = new.replace('        esp_wifi_set_storage(WIFI_STORAGE_FLASH);',
                      '        const esp_err_t storage_err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);\n'
                      '        if (ret == ESP_OK) ret = storage_err;')
source = source[:a] + new + source[b:]
event = '    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {\n'
guard = '''        /* Local cancellation is not a credential failure. The notification
         * may be queued until after the next credentials are admitted. */
        const wifi_event_sta_disconnected_t *event = event_data;
        if (event && event->reason == WIFI_REASON_STA_LEAVING) {
            RELEASE_LOCK(prov_ctx_lock);
            return;
        }
'''
assert source.count(event) == 1, 'Unknown manager event handler; review upstream'
if event + guard not in source:
    source = source.replace(event, event + guard)
if source != path.read_text(encoding='utf-8-sig'):
    path.write_text(source, encoding='utf-8')
print('PASS: provisioning reset order and intentional-disconnect guard installed')
