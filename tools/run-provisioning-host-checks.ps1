param(
    [string]$Gcc = "gcc"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $projectRoot "build-host-checks\provisioning"
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$policyExe = Join-Path $outputDir "nuwax_provision_policy_host_check.exe"
& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main/nuwax_provision_new')" `
    (Join-Path $projectRoot "main/nuwax_provision_new/nuwax_provision_policy_host_check.c") `
    -o $policyExe
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
& $policyExe
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$qrPayloadExe = Join-Path $outputDir "nuwax_provision_qr_payload_host_check.exe"
& $Gcc -std=c11 -Wall -Wextra -Werror `
    "-I$(Join-Path $projectRoot 'main/nuwax_provision_new')" `
    (Join-Path $projectRoot "main/nuwax_provision_new/nuwax_provision_qr_payload_host_check.c") `
    -o $qrPayloadExe
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
& $qrPayloadExe
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$provision = Get-Content -Raw -LiteralPath `
    (Join-Path $projectRoot "main/nuwax_provision_new/nuwax_provision_main.c")
$provisionHeader = Get-Content -Raw -LiteralPath `
    (Join-Path $projectRoot "main/nuwax_provision_new/nuwax_provision_main.h")
$appShell = Get-Content -Raw -LiteralPath `
    (Join-Path $projectRoot "main/app_shell.c")
$appShellHeader = Get-Content -Raw -LiteralPath `
    (Join-Path $projectRoot "main/app_shell.h")
$drawer = Get-Content -Raw -Encoding UTF8 -LiteralPath `
    (Join-Path $projectRoot "main/nuwax_ui_v28/nuwax_drawer.c")
$drawerHeader = Get-Content -Raw -LiteralPath `
    (Join-Path $projectRoot "main/nuwax_ui_v28/nuwax_drawer.h")
$cloud = Get-Content -Raw -LiteralPath `
    (Join-Path $projectRoot "main/nuwax_provision_new/nuwax_cloud_config.c")
$appMain = Get-Content -Raw -LiteralPath `
    (Join-Path $projectRoot "main/nuwax_ai_partner_main.c")
$wifiManager = Get-Content -Raw -LiteralPath `
    (Join-Path $projectRoot "main/wifi_manager.c")
$powerCtrl = Get-Content -Raw -LiteralPath `
    (Join-Path $projectRoot "main/board/nuwax_power_ctrl.c")
$otaDevice = Get-Content -Raw -LiteralPath `
    (Join-Path $projectRoot "main/nuwax_ota_new/nuwax_ota_device.c")
$lvglPort = Get-Content -Raw -LiteralPath `
    (Join-Path $projectRoot "main/nuwax_lvgl_port.c")
$defaultFiles = @(
    (Join-Path $projectRoot "sdkconfig.defaults"),
    (Join-Path $projectRoot "sdkconfig.product-ota.defaults"),
    (Join-Path $projectRoot "sdkconfig.barge-final")
)

$credentialSuccess = [regex]::Match(
    $provision,
    '(?s)case NETWORK_PROV_WIFI_CRED_SUCCESS:.*?break;').Value
if ($credentialSuccess -match 'NUWAX_PROVISION_OUTCOME_SUCCESS') {
    throw "GOT_IP must not publish success before transaction finalization."
}
if ($cloud -notmatch 'nuwax_cloud_config_stage' -or
    $cloud -notmatch 'nuwax_cloud_config_commit_pending' -or
    $cloud -notmatch 'nuwax_cloud_config_discard_pending' -or
    $provision -notmatch '(?s)nuwax_wifi_import_driver_credentials.*?nuwax_cloud_config_commit_pending.*?retry_write_wifi_verified_marker' -or
    $provision -notmatch '(?s)nuwax_wifi_import_driver_credentials.*?write_wifi_commit_marker.*?nuwax_cloud_config_commit_pending' -or
    $provision -notmatch '(?s)wifi_commit_marker_matches.*?!nuwax_cloud_config_has_pending.*?clear_reprovision_record' -or
    $provision -notmatch '(?s)wait_for_wifi_connection_to.*?nuwax_wifi_import_driver_credentials.*?retry_write_wifi_verified_marker.*?clear_reprovision_record') {
    throw "Provisioning transaction or verified rollback ordering regressed."
}
if ($provision -notmatch '(?s)xQueueCreateWithCaps\(.*?s_vox_config_jobs.*?vQueueDeleteWithCaps\(s_vox_config_jobs\).*?ESP_ERR_NO_MEM' -or
    $provision -match '(?s)nuwax_provision_main_stop\(void\).*?vQueueDeleteWithCaps\(s_vox_config_jobs\)' -or
    $provision -match '(?s)nuwax_provision_main_stop\(void\).*?vSemaphoreDelete\(s_vox_config_gate\)' -or
    $provision -notmatch '(?s)close_and_drain_vox_config_jobs\(void\).*?xQueueReceive\(s_vox_config_jobs.*?ESP_ERR_INVALID_STATE') {
    throw "vox-config WithCaps queue ownership or partial-init rollback regressed."
}
if ($provision -match 'xSemaphoreTake\(job->done,\s*portMAX_DELAY\)' -or
    $provision -notmatch 'CONFIG_NUWAX_PROVISION_VOX_CONFIG_JOB_TIMEOUT_MS' -or
    $provision -notmatch 'CONFIG_NUWAX_PROVISION_APP_PRECONFIG_IDLE_TIMEOUT_MS' -or
    $provision -notmatch 'CONFIG_NUWAX_PROVISION_APP_SESSION_IDLE_TIMEOUT_MS' -or
    $provision -notmatch 'CONFIG_NUWAX_PROVISION_RUNTIME_IDLE_TIMEOUT_MS' -or
    $provision -notmatch 'CONFIG_NUWAX_PROVISION_FAILURE_STATUS_HOLD_MS' -or
    $provision -notmatch 'failed_attempt_release_pending' -or
    $provision -notmatch 'nuwax_provision_failed_session_release_due' -or
    $provision -notmatch '(?s)s_state\.wifi_credentials_started = true;.*?s_state\.failed_attempt_release_pending = false;.*?s_state\.failed_attempt_release_deadline_us = 0;' -or
    $provision -notmatch 'nuwax_provision_attempt_belongs_to_owner' -or
    $provision -notmatch 'nuwax_provision_app_session_idle_timed_out' -or
    $provision -notmatch '"state",\s*"active"' -or
    $provision -match '"state",\s*"pending"' -or
    $provision -notmatch 'PROTOCOMM_TRANSPORT_BLE_CONNECTED' -or
    $provision -notmatch 'PROTOCOMM_TRANSPORT_BLE_DISCONNECTED' -or
    $provision -notmatch 'PROTOCOMM_SECURITY_SESSION_SETUP_OK' -or
    $provision -notmatch 'network_prov_mgr_reset_wifi_sm_state_for_reprovision' -or
    $provision -notmatch 'network_prov_mgr_reset_wifi_sm_state_on_failure' -or
    $provision -notmatch 'nuwax_provision_app_can_accept_ble' -or
    $provision -notmatch 'PROVISION_RESET_RESTART_FAILURES' -or
    $provision -notmatch 'PROVISION_FINALIZE_RESTART_FAILURES' -or
    $provision -notmatch 'REPROVISION_RECORD_VERSION' -or
    $provision -notmatch 'nuwax_provision_old_wifi_is_terminal_candidate' -or
    $provision -notmatch 'nuwax_provision_wifi_attempt_timed_out' -or
    $provision -notmatch '(?s)reset_manager_wifi_state\(kind, reason\).*?restore_flash_credentials_after_candidate' -or
    $provision -notmatch '(?s)nuwax_cloud_config_discard_pending\(\).*?restore_flash_credentials_after_candidate\(\).*?network_prov_mgr_start_provisioning' -or
    $provision -notmatch 'NUWAX_PROVISION_OUTCOME_OLD_WIFI_RECOVERED') {
    throw "Continuous provisioning arbitration or bounded job lifecycle regressed."
}
if ($provision -notmatch '(?s)nuwax_provision_main_run_bounded\(.*?overall_timeout_ms.*?provision_main_run_internal\(focused_wait_ms, overall_timeout_ms,\s*true\)' -or
    $provision -notmatch '(?s)overall_deadline.*?runtime_total_timeout.*?NUWAX_PROVISION_OUTCOME_TIMEOUT' -or
    $provision -notmatch '(?s)nuwax_provision_main_run\(uint32_t focused_wait_ms\).*?provision_main_run_internal\(focused_wait_ms, 0U, true\)' -or
    $provision -notmatch '(?s)nuwax_provision_main_run_runtime_rebind\(.*?provision_main_run_internal\(focused_wait_ms, 0U, false\)' -or
    $provision -notmatch 'nuwax_provision_runtime_idle_timed_out' -or
    $provision -notmatch 'runtime_idle_timeout' -or
    $provision -notmatch '(?s)allow_old_wifi_recovery.*?finalize_old_wifi_recovery' -or
    $provision -notmatch '(?s)allow_old_wifi_recovery.*?start_old_wifi_attempt' -or
    $provision -notmatch 'nuwax_provision_main_supervisor_heartbeat_us' -or
    $appMain -notmatch 'NUWAX_REPROVISION_WATCHDOG_HEARTBEAT_MS' -or
    $appMain -notmatch 'nuwax_provision_main_supervisor_heartbeat_us' -or
    $appMain -notmatch 'nuwax_provision_main_run_runtime_rebind\(\s*NUWAX_REPROVISION_FOCUSED_WAIT_MS\s*\)' -or
    $provision -notmatch 'nuwax_provision_main_run_runtime_rebind' -or
    $appMain -match 'NUWAX_REPROVISION_OVERALL_TIMEOUT_MS') {
    throw "Runtime provisioning must remain APP-activity bounded while retaining a supervisor progress watchdog."
}
$runtimeReprovision = [regex]::Match(
    $appMain,
    '(?s)static esp_err_t run_runtime_ble_reprovision\(void\).*?(?=static bool ota_continuous_audio_busy)').Value
$wakeInit = [regex]::Match(
    $appMain,
    '(?s)static esp_err_t init_wake_model_serialized\(void\).*?(?=static esp_err_t start_wake_model_serialized)').Value
$wakeStart = [regex]::Match(
    $appMain,
    '(?s)static esp_err_t start_wake_model_serialized\(.*?(?=static esp_err_t deinit_wake_model_serialized)').Value
$wakeRestoreOwner = [regex]::Match(
    $appMain,
    '(?s)static bool service_wake_model_restore_owner\(void\).*?(?=static uint32_t next_wake_detector_token_locked)').Value
$wakeSleepStart = [regex]::Match(
    $appMain,
    '(?s)static bool start_sleep_wake_word_detector\(.*?(?=static void ensure_sleep_wake_word_detector)').Value
$wakeSupervisor = [regex]::Match(
    $appMain,
    '(?s)static void wake_word_supervisor_task\(void \*arg\).*?(?=static void remember_sleep_request)').Value
if ($runtimeReprovision -notmatch '(?s)nuwax_provision_main_stop\(\).*?goto cleanup;' -or
    $runtimeReprovision -notmatch '(?s)nuwax_lvgl_port_set_runtime_provisioning_memory\(true\).*?nuwax_provision_main_start\("runtime_reprovision"\).*?cleanup:.*?nuwax_lvgl_port_set_runtime_provisioning_memory\(false\)' -or
    $runtimeReprovision -notmatch '(?s)deinit_wake_model_serialized\(\).*?conversation_live_aec_pool_suspend_for_provisioning\(\).*?nuwax_speaker_playback_deinit_output\(\)' -or
    $runtimeReprovision -notmatch '(?s)cleanup:.*?nuwax_provision_main_stop\(\).*?restore_audio_after_runtime_provisioning\(\).*?nuwax_provision_main_rollback_wifi_credentials\(\).*?clear_reprovision_reservation\(true\)' -or
    $runtimeReprovision -match 'nuwax_wake_word_detector_init\(' -or
    $appMain -notmatch '(?s)restore_audio_after_runtime_provisioning\(void\).*?request_wake_model_restore\(.*?NUWAX_REPROVISION_WAKE_MODEL_RESTORE_TIMEOUT_MS' -or
    $appMain -notmatch '(?s)request_wake_model_restore\(uint32_t timeout_ms\).*?s_wake_model_restore_mutex.*?wake model restore joining in-flight.*?s_wake_model_restore_completed.*?xSemaphoreGive\(s_wake_model_restore_mutex\)' -or
    $wakeInit -notmatch '(?s)esp_ptr_in_dram\(.*?s_wake_model_operation_mutex.*?nuwax_wake_word_detector_init\(.*?xSemaphoreGive\(s_wake_model_operation_mutex\)' -or
    $wakeStart -notmatch '(?s)s_wake_model_operation_mutex.*?s_reprovision_quiesce_requested.*?nuwax_wake_word_detector_start_with_context\(context\).*?xSemaphoreGive\(s_wake_model_operation_mutex\)' -or
    $wakeStart -match 'nuwax_wake_word_detector_init\(' -or
    $appMain -notmatch '(?s)deinit_wake_model_serialized\(void\).*?s_wake_model_operation_mutex.*?nuwax_wake_word_detector_deinit\(\).*?nuwax_wake_word_detector_is_running\(\).*?xSemaphoreGive\(s_wake_model_operation_mutex\)' -or
    $wakeRestoreOwner -notmatch '(?s)s_conversation_owner_task.*?xTaskGetCurrentTaskHandle\(\).*?init_wake_model_serialized\(\).*?s_wake_model_restore_result.*?s_wake_model_restore_completed.*?CONFIG_ESP_MAIN_TASK_STACK_SIZE' -or
    $wakeSupervisor -match 'init_wake_model_serialized\(' -or
    $wakeSleepStart -notmatch '(?s)err == ESP_ERR_INVALID_STATE.*?queue_wake_model_restore_nonblocking' -or
    $appMain -notmatch '(?s)while \(atomic_load_explicit\(&s_reprovision_quiesce_requested,.*?service_wake_model_restore_owner\(\).*?vTaskDelay' -or
    $appMain -notmatch '#define NUWAX_WAKE_SUPERVISOR_TASK_STACK_BYTES 16384U' -or
    $appMain -notmatch '(?s)xSemaphoreCreateMutexStatic\(\s*&s_wake_model_restore_mutex_storage\).*?xSemaphoreCreateMutexStatic\(\s*&s_wake_model_operation_mutex_storage\).*?create_app_task\(wake_word_supervisor_task.*?if \(wake_init_err == ESP_OK\)' -or
    $runtimeReprovision -notmatch '(?s)audio_restore_err == ESP_ERR_TIMEOUT.*?runtime_provision_wake_restore_restart.*?esp_restart\(\).*?clear_reprovision_reservation\(true\)' -or
    $runtimeReprovision -notmatch '(?s)clear_reprovision_reservation\(true\).*?request_sleep_wake_word_detector\("ble_reprovision_complete"\).*?wait_for_sleep_wake_word_ready' -or
    $runtimeReprovision -notmatch 'Provisioning failed; previous Wi-Fi unavailable' -or
    $appMain -notmatch '(?s)static void set_wifi_result_from_current_link.*?nuwax_app_shell_set_wifi_result.*?nuwax_wifi_is_connected\(\).*?nuwax_app_shell_set_wifi_status' -or
    $lvglPort -notmatch '(?s)nuwax_lvgl_port_set_runtime_provisioning_memory\(bool enabled\).*?lv_display_set_buffers.*?heap_caps_free\(previous1\)') {
    throw "Runtime provisioning errors must converge on rollback, audio restoration, and reservation release."
}
if ($lvglPort -notmatch '(?s)task_stack_allocation = heap_caps_aligned_alloc\(\s*16, LVGL_TASK_STACK_GUARD_BYTES \+ LVGL_TASK_STACK_SIZE,\s*MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT\)' -or
    $lvglPort -notmatch '(?s)stack_cache_safe =.*?esp_ptr_in_dram\(task_stack_allocation\).*?esp_ptr_in_dram\(task_stack\).*?esp_ptr_in_dram\(\(const uint8_t \*\)task_stack \+.*?LVGL_TASK_STACK_SIZE - 1U\)' -or
    $lvglPort -notmatch 'LVGL task stack=internal cache_safe=1' -or
    $lvglPort -match 'task_stack_allocation = heap_caps_aligned_alloc\([^;]*MALLOC_CAP_SPIRAM') {
    throw "LVGL task stack must remain entirely in cache-safe internal DRAM while the emotion partition is read."
}
$wifiHandoff = [regex]::Match(
    $wifiManager,
    '(?s)void nuwax_wifi_request_provisioning_handoff.*?(?=bool nuwax_wifi_cancel_provisioning_handoff)').Value
$wifiHandoffCancel = [regex]::Match(
    $wifiManager,
    '(?s)bool nuwax_wifi_cancel_provisioning_handoff.*?(?=esp_err_t nuwax_wifi_set_provisioning_active)').Value
$wifiOwnerClaim = [regex]::Match(
    $wifiManager,
    '(?s)esp_err_t nuwax_wifi_set_provisioning_active.*?(?=bool nuwax_wifi_provisioning_active)').Value
$wifiOwnerError = [regex]::Match(
    $wifiOwnerClaim,
    '(?s)if .*?owner_err != ESP_OK.*?return owner_err;').Value
$runtimeRequest = [regex]::Match(
    $appMain,
    '(?s)static bool queue_ble_reprovision_request.*?(?=static bool usb_control_request_ble_reprovision)').Value
$activeOfficialServiceJoin = [regex]::Match(
    $runtimeRequest,
    '(?s)if \(nuwax_provision_main_is_active\(\)\).*?return true;\s*}').Value
if ($provisionHeader -notmatch 'bool nuwax_provision_main_is_active\(void\);' -or
    $provision -notmatch 'static atomic_bool s_started\s*=\s*ATOMIC_VAR_INIT\(false\)' -or
    $provision -notmatch '(?s)bool nuwax_provision_main_is_active\(void\).*?atomic_load_explicit\(&s_started, memory_order_acquire\)' -or
    $provision -match '(?m)^\s*(if|while)\s*\(\s*!?\s*s_started\b' -or
    [string]::IsNullOrEmpty($activeOfficialServiceJoin) -or
    $activeOfficialServiceJoin -notmatch 'nuwax_app_shell_set_wifi_connecting' -or
    $activeOfficialServiceJoin -match 'nuwax_wifi_request_provisioning_handoff|nuwax_power_ctrl_acquire_shutdown_inhibit|clear_reprovision_reservation|xTaskNotify' -or
    $runtimeRequest.IndexOf('nuwax_provision_main_is_active()') -gt
        $runtimeRequest.IndexOf('reprovision_hard_busy_reason(true)') -or
    $runtimeRequest.IndexOf('nuwax_provision_main_is_active()') -gt
        $runtimeRequest.IndexOf('nuwax_wifi_request_provisioning_handoff()')) {
    throw "An active official provisioning service must be joined before runtime reservation or Wi-Fi ownership changes."
}
if ($wifiHandoff -notmatch 's_provisioning_state' -or
    $wifiHandoff -notmatch 's_wifi_connect_requested' -or
    $wifiHandoff -notmatch 's_provisioning_saved_connect_requested' -or
    $wifiHandoff -notmatch 's_synchronous_connect_active' -or
    $wifiHandoff -notmatch 'WIFI_FALLBACK_BIT' -or
    $wifiHandoffCancel -notmatch 'atomic_compare_exchange_strong_explicit' -or
    $wifiHandoffCancel -notmatch 's_provisioning_saved_connect_requested' -or
    $wifiHandoffCancel -notmatch 's_wifi_connect_requested' -or
    $wifiHandoffCancel -notmatch 'WIFI_FAIL_BIT \| WIFI_FALLBACK_BIT' -or
    $wifiHandoffCancel -notmatch 'WIFI_RECONNECT_BIT' -or
    $wifiOwnerClaim -notmatch '(?s)nuwax_wifi_request_provisioning_handoff.*?wifi_operation_lock_for' -or
    $wifiOwnerClaim -notmatch 'WIFI_PROVISIONING_CLAIM_LOCK_TIMEOUT_MS' -or
    $wifiOwnerClaim -notmatch 'atomic_compare_exchange_strong_explicit' -or
    $wifiOwnerError -match 'WIFI_PROVISIONING_NONE' -or
    $wifiManager -notmatch 'nuwax_wifi_cancel_provisioning_handoff' -or
    $wifiManager -notmatch 'nuwax_wifi_provisioning_owner_active' -or
    $wifiManager -notmatch 'WIFI_PROVISIONING_HANDOFF' -or
    $wifiManager -notmatch 'WIFI_PROVISIONING_OWNER' -or
    $wifiManager -notmatch '(?s)WIFI_RECONNECT_BIT.*?wifi_operation_lock_for\(WIFI_RECONNECT_LOCK_TIMEOUT_MS\).*?esp_wifi_connect\(\)' -or
    $wifiManager -notmatch 'Wi-Fi connect cancelled after STA start' -or
    $runtimeRequest -notmatch '(?s)nuwax_wifi_request_provisioning_handoff.*?xTaskNotify' -or
    $appMain -notmatch 'NUWAX_REPROVISION_PHASE_WAIT_READY' -or
    $appMain -notmatch 'wait_for_reprovision_transient_busy' -or
    $appMain -notmatch 'ble_reprovision_audio_quiesce_timeout' -or
    $appMain -notmatch 'release_reprovision_wifi_interlock') {
    throw "Runtime reprovision request fencing or bounded quiesce recovery regressed."
}
$wifiReconnectWatcher = [regex]::Match(
    $wifiManager,
    '(?s)static void wifi_fallback_watcher_task.*?(?=esp_err_t nuwax_wifi_start_fallback_watcher)').Value
if ($wifiManager -notmatch 'WIFI_RECONNECT_BACKOFF_BASE_MS' -or
    $wifiManager -notmatch 'WIFI_RECONNECT_BACKOFF_MAX_MS' -or
    $wifiManager -notmatch 'WIFI_RECONNECT_MAX_DRIVER_FAILURES' -or
    $wifiManager -notmatch 'wifi_reconnect_backoff_ms' -or
    $wifiReconnectWatcher -notmatch 's_deferred_reconnect_driver_failures' -or
    $wifiReconnectWatcher -notmatch 'failures < WIFI_RECONNECT_MAX_DRIVER_FAILURES' -or
    $wifiReconnectWatcher -notmatch '(?s)wifi_operation_unlock().*?vTaskDelay' -or
    $wifiReconnectWatcher -notmatch '(?s)!nuwax_wifi_provisioning_active().*?s_wifi_connect_requested.*?!nuwax_wifi_is_connected().*?WIFI_RECONNECT_BIT' -or
    $wifiOwnerClaim -notmatch '(?s)esp_wifi_connect().*?WIFI_RECONNECT_BIT') {
    throw "Deferred Wi-Fi reconnect must use bounded driver retries outside the Wi-Fi owner lock."
}
$orphanRecovery = [regex]::Match(
    $appMain,
    '(?s)recover_orphaned_reprovision_owner\(const char \*source\).*?(?=static void service_reprovision_watchdog)').Value
if ($powerCtrl -notmatch 'NUWAX_SHUTDOWN_GATE_CLAIMED' -or
    $powerCtrl -notmatch 'atomic_compare_exchange_strong_explicit' -or
    $powerCtrl -notmatch 'nuwax_power_ctrl_acquire_shutdown_inhibit' -or
    $powerCtrl -notmatch 'nuwax_power_ctrl_release_shutdown_inhibit' -or
    $otaDevice -notmatch 'NUWAX_POWER_INHIBIT_OTA_BOOT' -or
    $otaDevice -notmatch 'NUWAX_POWER_INHIBIT_OTA_UPDATE' -or
    $appMain -notmatch 'NUWAX_POWER_INHIBIT_REPROVISION' -or
    $appMain -match 'nuwax_power_ctrl_set_shutdown_inhibited' -or
    $otaDevice -match 'nuwax_power_ctrl_set_shutdown_inhibited') {
    throw "SW3 shutdown arbitration must use independent OTA and provisioning owners."
}
if ($orphanRecovery -notmatch 's_reprovision_quiesce_requested' -or
    $orphanRecovery -notmatch 'nuwax_wifi_provisioning_active' -or
    $orphanRecovery -notmatch 'nuwax_wifi_set_provisioning_active\(false\)' -or
    $orphanRecovery -notmatch 'nuwax_power_ctrl_release_shutdown_inhibit' -or
    $orphanRecovery -notmatch 's_reprovision_orphan_recovery_active' -or
    $orphanRecovery -notmatch 's_reprovision_orphan_release_failures' -or
    $orphanRecovery.IndexOf('nuwax_wifi_set_provisioning_active(false)') -gt
        $orphanRecovery.IndexOf('nuwax_power_ctrl_release_shutdown_inhibit') -or
    $orphanRecovery.IndexOf('nuwax_power_ctrl_release_shutdown_inhibit') -gt
        $orphanRecovery.IndexOf('s_reprovision_quiesce_requested, false')) {
    throw "Orphaned runtime provisioning must release cloud/audio, Wi-Fi, and SW3 ownership together."
}
if ($appMain -notmatch 'NUWAX_REPROVISION_ORPHAN_RELEASE_RESTART_FAILURES' -or
    $appMain -notmatch 'reprovision_orphan_restart' -or
    $runtimeRequest -notmatch 'NUWAX_REPROVISION_ORPHAN_RETRY_REQUIRED' -or
    $runtimeRequest -notmatch '!s_reprovision_orphan_recovery_active') {
    throw "Orphaned runtime provisioning recovery must remain serialized, fail closed, and restart after bounded failures."
}
if ($provision -notmatch '(?s)internal_largest_reserve = heap_caps_malloc\(.*?PROVISION_INTERNAL_STARTUP_RESERVE_BYTES.*?network_prov_mgr_start_provisioning\(.*?network_prov_mgr_endpoint_register\(DEVICE_INFO_EP.*?network_prov_mgr_endpoint_register\(VOX_CONFIG_EP.*?heap_caps_free\(internal_largest_reserve\).*?BLE provisioning steady-state memory guard.*?provision_memory_is_low' -or
    $provision -notmatch '#define PROVISION_INTERNAL_STARTUP_RESERVE_BYTES 15872U') {
    throw "BLE provisioning must preserve a contiguous internal block across NimBLE startup and validate the final endpoint footprint."
}
$incompleteBootRecovery = [regex]::Match(
    $provision,
    '(?s)s_provisioned_pair_ready = nuwax_provision_pair_ready\(.*?s_initialized = true;').Value
if ($incompleteBootRecovery -match 'save_reprovision_record\(\)') {
    throw "Unverified boot credentials must not become a rollback network."
}
if ($provision -notmatch 'nuwax_provision_boot_profile_required' -or
    $provision -notmatch '(?s)nuwax_provision_pair_ready\(\s*has_driver_credentials,\s*verified,\s*cloud_ready\)' -or
    $provision -notmatch '(?s)nuwax_provision_main_should_auto_enter\(void\).*?!s_provisioned_pair_ready' -or
    $provision -notmatch 'nuwax_wifi_has_saved_credentials' -or
    $provision -notmatch 'LOW_MEMORY_BOOT_NVS_KEY' -or
    $provision -notmatch '(?s)has_driver_credentials && verified &&.*?!nuwax_wifi_saved_credentials_match.*?nuwax_wifi_import_driver_credentials' -or
    $appMain -notmatch '(?s)if \(!provisioning_boot_profile\).*?nuwax_provision_main_request_low_memory_boot_profile\(\).*?nuwax_provision_main_stop\(\).*?esp_restart\(\);') {
    throw "Late provisioning discovery can enter BLE without the low-memory reboot profile."
}

if ($provisionHeader -notmatch 'nuwax_provision_main_request_runtime_cancel' -or
    $provisionHeader -notmatch 'nuwax_provision_main_has_complete_pair' -or
    $provision -notmatch 'static atomic_int s_cancel_state' -or
    $provision -notmatch 'static atomic_bool s_force_reprovision' -or
    $provision -notmatch '(?s)force_reprovision_requested\(void\).*?atomic_load_explicit\(&s_force_reprovision' -or
    $provision -notmatch '(?s)set_force_reprovision\(bool enabled\).*?atomic_store_explicit\(&s_force_reprovision' -or
    $provision -notmatch 'PROVISION_CANCEL_REQUESTED' -or
    $provision -notmatch '(?s)provisioning_cancel_requested\(\).*?finish_runtime_cancel\(\)' -or
    $appShellHeader -notmatch 'nuwax_app_shell_reprovision_cb_t\)\(void \*ctx, bool enabled\)' -or
    $appShell -notmatch '(?s)result->action == NUWAX_APP_SHELL_ACTION_REPROVISION_WIFI.*?callback\(callback_ctx, result->index != 0\);.*?return true;.*?xQueueSend\(queue, result, wait_ticks\)' -or
    $appMain -notmatch '(?s)shell_dispatch_set_reprovisioning\(void \*ctx, bool enabled\).*?if \(!enabled\).*?nuwax_provision_main_request_runtime_cancel' -or
    $appMain -notmatch '(?s)nuwax_app_shell_set_reprovision_callback\(\s*shell_dispatch_set_reprovisioning, NULL\)' -or
    $drawer -notmatch '(?s)s_provision_pending_target = !provisioning_active\(\);.*?result\.value = s_provision_pending_target \? 1 : 0;' -or
    $appShell -notmatch 'nuwax_app_shell_schedule_provisioning_qr' -or
    $appShell -notmatch '(?s)s_pending_provisioning_qr_deadline_us =\s*provisioning_qr_retry_deadline_us.*?if \(shown\).*?s_pending_provisioning_qr = false;' -or
    $appShell -notmatch '(?s)if \(was_active\).*?s_pending_provisioning_qr_close = true;' -or
    $drawerHeader -notmatch 'nuwax_drawer_hide_provisioning_qr' -or
    $drawer -notmatch '(?s)nuwax_drawer_hide_provisioning_qr\(void\).*?s_detail != CC_DETAIL_QR.*?nuwax_drawer_close\(\);' -or
    $drawer -notmatch '#define CC_PROVISION_PENDING_TIMEOUT_MS 15000U' -or
    $drawer -notmatch '(?s)s_provision_pending = true;.*?s_provision_pending_deadline_us =.*?CC_PROVISION_PENDING_TIMEOUT_MS' -or
    $appMain -notmatch '!nuwax_provision_main_has_complete_pair\(\)' -or
    $drawer -notmatch '(?s)nuwax_drawer_begin_pull\(void\).*?set_page\(CC_PAGE_MENU, false\);.*?render_page_if_dirty\(CC_PAGE_MENU\);' -or
    $appShell -notmatch '(?s)event == NUWAX_APP_SHELL_EVENT_SWIPE_DOWN && y < NUWAX_GESTURE_TOP_BAND\).*?nuwax_drawer_open_menu\(\);' -or
    $drawer -notmatch 'state->provisioning_active\) return "\u914D\u7F51\u4E2D";' -or
    $drawer -notmatch '(?s)nuwax_drawer_show_provisioning_qr\(void\).*?if \(!s_open\).*?open_control_center\(CC_PAGE_MENU\);.*?s_page != CC_PAGE_MENU.*?show_qr\(\);' -or
    $appMain -notmatch '(?s)nuwax_provision_main_start\("runtime_reprovision"\).*?if \(result != ESP_OK\).*?goto cleanup;.*?nuwax_app_shell_schedule_provisioning_qr\(0U\);' -or
    $appMain -notmatch '(?s)nuwax_provision_main_start\("first_boot"\).*?if \(provision_start_err != ESP_OK\).*?else \{.*?nuwax_app_shell_schedule_provisioning_qr\(0U\);') {
    throw "Target-state provisioning cancellation or boot QR lifecycle regressed."
}
foreach ($defaultFile in $defaultFiles) {
    $profile = Get-Content -Raw -LiteralPath $defaultFile
    if ($profile -notmatch 'CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=256' -or
        $profile -notmatch 'CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6' -or
        $profile -notmatch 'CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16' -or
        $profile -notmatch 'CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=6' -or
        $profile -notmatch 'CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=16') {
        throw "Provisioning memory profile defaults regressed in $defaultFile."
    }
}

Write-Host "provisioning transaction invariant check: PASS"
