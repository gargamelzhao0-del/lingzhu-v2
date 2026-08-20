set(SRCS_COMMON
    "app_state.c"
    "app_shell.c"
    "audio_frame.c"
    "board/audio_board.c"
    "board/battery_service.c"
    "board/battery_insights.c"
    "board/board_gpio_inputs.c"
    "board/hardware_selftest.c"
    "board/i2c_bus_manager.c"
    "board/nuwax_board.c"
    "board/shake_detector.c"
    "board/nuwax_power_ctrl.c"
    "board/vocat_audio_board.c"
    "board/qmi8658a_driver.c"
    "board/bq27220_driver.c"
    "board/led_driver.c"
    "board/led_status.c"
    "board/sd_card_driver.c"
    "board/uart_driver.c"
    "cloud_protocol_compat.c"
    "ctype_compat.c"
    "diagnostics.c"
    "display.c"
    "mic_input.c"
    "mcp_server.c"
    "nuwax_ai_partner_main.c"
    "lvgl_env_probe.c"
    "lvgl_psram_mem.c"
    "opus_encoder_wrapper.c"
    "p3_afe_l3.c"
    "p3_nearend_candidate.c"
    "p3_softref_aec_diag.c"
    "p3_software_ref.c"
    "product_ready.c"
    "p6_gray_confirm.c"
    "conversation/audio_uplink.c"
    "conversation/audio_uplink_terminal.c"
    "conversation/audio_terminal_recovery.c"
    "conversation/adaptive_endpoint.c"
    "conversation/audio_cleaner.c"
    "conversation/awake_session_state.c"
    "conversation/aec_delay_shadow.c"
    "conversation/aec_fir_delay_control.c"
    "conversation/barge_in_controller.c"
    "conversation/barge_in_dispatcher.c"
    "conversation/barge_in_detector.c"
    "conversation/conversation_audio_health.c"
    "conversation/conversation_metrics.c"
    "conversation/conversation_state.c"
    "conversation/head_touch_control.c"
    "conversation/head_touch_control_policy.c"
    "conversation/head_touch_agent_intent.c"
    "conversation/listen_resume_state.c"
    "conversation/live_aec.c"
    "conversation/live_aec_output_provenance.c"
    "conversation/mic_level.c"
    "conversation/notification_playback.c"
    "conversation/notification_rx_barrier.c"
    "conversation/p5_fullduplex_overlap_probe.c"
    "conversation/playback_terminal_guard.c"
    "conversation/prelisten_state.c"
    "conversation/preroll_window.c"
    "conversation/shell_action_dispatch.c"
    "conversation/session_dispatch.c"
    "conversation/sleep_transaction.c"
    "conversation/short_speech_tracker.c"
    "conversation/speech_confirmation.c"
    "conversation/speaker_playback.c"
    "conversation/touch_cancel_recovery.c"
    "conversation/touch_gesture.c"
    "conversation/turn_guard.c"
    "conversation/wake_word_config.c"
    "conversation/wake_word_interrupt_filter.c"
    "audio_modes/meeting/meeting_mode.c"
    "audio_modes/meeting/meeting_processor.c"
    "audio_modes/meeting/meeting_protocol.c"
    "audio_modes/monitoring/monitoring_mode.c"
    "audio_modes/monitoring/monitoring_protocol.c"
    "session_protocol.c"
    "speaker_output.c"
    "synthetic_audio.c"
    "time_sync.c"
    "touch_input.c"
    "wake_word_detector.c"
    "wifi_manager.c"
    "ws_transport.c"
    "nuwax_provision_new/nuwax_cloud_config.c"
    "nuwax_provision_new/nuwax_device_uid.c"
    "nuwax_provision_new/nuwax_provision_main.c"
    "nuwax_provision_new/nuwax_provision_usb_control.c"
)

if(CONFIG_NUWAX_OTA_ENABLE)
    if(CONFIG_NUWAX_P1_HW_DIAG_MODE OR
       CONFIG_NUWAX_P2_AUDIO_SMOKE_MODE OR
       CONFIG_NUWAX_HW_ACCEPTANCE_MODE OR
       CONFIG_NUWAX_P3_AFE_MR_PROTOTYPE_ENABLE OR
       CONFIG_NUWAX_P3_REF_TDM_REFERENCE_STYLE OR
       CONFIG_NUWAX_P3_VENDOR_V2_AUDIO_PROFILE OR
       CONFIG_NUWAX_P3_SOFTWARE_REF_AEC_AB_ENABLE OR
       CONFIG_NUWAX_P3_HARDREF_AEC_AB_ENABLE)
        message(FATAL_ERROR
            "Application OTA product mode cannot be combined with isolated hardware/audio diagnostics")
    endif()
    list(APPEND SRCS_COMMON
        "nuwax_ota_new/nuwax_ota.c"
        "nuwax_ota_new/nuwax_ota_manifest.c"
        "nuwax_ota_new/nuwax_ota_transport.c"
        "nuwax_ota_new/nuwax_ota_main.c"
        "nuwax_ota_new/nuwax_ota_device.c"
        "nuwax_ota_new/nuwax_firmware_report.c"
    )
endif()

set(SRCS_LVGL_UI
    "nuwax_lvgl_port.c"
    "nuwax_ui_v28/nuwax_app.c"
    "nuwax_ui_v28/nuwax_device_state.c"
    "nuwax_ui_v28/emotion_display_policy.c"
    "nuwax_ui_v28/nuwax_state_bridge.c"
    "nuwax_ui_v28/nuwax_face.c"
    "nuwax_ui_v28/nuwax_drawer.c"
    "nuwax_ui_v28/nuwax_gesture.c"
    "nuwax_ui_v28/nuwax_theme.c"
    "nuwax_ui_v28/nuwax_anim.c"
    "nuwax_ui_v28/menu/nuwax_device_qr_view.c"
    "nuwax_ui_v28/assets/nuwax_control_center_icons.c"
    "nuwax_ui_v28/assets/fonts/nuwax_font_control_center_cjk_7.c"
    "nuwax_ui_v28/assets/fonts/nuwax_font_control_center_cjk_8.c"
    "nuwax_ui_v28/assets/fonts/nuwax_font_control_center_cjk_9.c"
    "nuwax_ui_v28/assets/fonts/nuwax_font_control_center_cjk_10.c"
    "nuwax_ui_v28/assets/fonts/nuwax_font_control_center_cjk_11.c"
    "nuwax_ui_v28/assets/fonts/nuwax_font_control_center_cjk_12.c"
    "nuwax_ui_v28/assets/fonts/nuwax_font_control_center_cjk_13.c"
    "nuwax_ui_v28/face/emotion_asset_pack.c"
    "nuwax_ui_v28/face/emotion_animation_player.c"
)

set(SRCS ${SRCS_COMMON} ${SRCS_LVGL_UI})

# These retired prototypes predate the shared audio ownership and formal
# command-dispatch paths. Refuse accidental product linkage: they deliberately
# fail closed and are retained only to make the historical status explicit.
set(NUWAX_RETIRED_PRODUCT_SOURCES
    "audio_reliable_capture.c"
    "audio_reliable_playback.c"
    "state_cmd_fast.c"
)
foreach(retired_source IN LISTS NUWAX_RETIRED_PRODUCT_SOURCES)
    if(retired_source IN_LIST SRCS)
        message(FATAL_ERROR
            "Retired prototype ${retired_source} must not be linked into product firmware")
    endif()
endforeach()

set(INCLUDE_DIRS "." "conversation" "audio_modes/meeting" "audio_modes/monitoring" "nuwax_ui_v28" "nuwax_ui_v28/face" "nuwax_ui_v28/assets" "nuwax_provision_new" "nuwax_ota_new")

idf_component_register(SRCS ${SRCS}
                       PRIV_REQUIRES app_update bootloader_support bt esp_http_client esp_wifi esp_event esp_netif nvs_flash efuse cjson lwip esp_timer esp-tls mbedtls esp_app_format esp_hw_support esp_psram esp_ringbuf spi_flash esp_driver_i2s esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_ledc esp_driver_uart esp_driver_sdmmc esp_driver_usb_serial_jtag fatfs sdmmc lvgl esp-sr esp_audio_codec esp_codec_dev network_provisioning protocomm
                       INCLUDE_DIRS ${INCLUDE_DIRS})

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE=${NUWAX_OTA_RELEASE_SEQUENCE}ULL
    NUWAX_EMOTION_RELEASE_SHA256=\"${NUWAX_EMOTION_PACK_SHA256}\"
    NUWAX_EMOTION_COMPATIBLE_SHA256_CSV=\"${NUWAX_EMOTION_COMPATIBLE_SHA256_CSV}\")

if(CONFIG_NUWAX_PRODUCTION_SECURITY_REQUIRED)
    if(NOT CONFIG_SECURE_BOOT OR NOT CONFIG_FLASH_ENCRYPTION_ENABLED)
        message(FATAL_ERROR
            "Production security requires Secure Boot V2 and Flash Encryption; configure keys and the eFuse provisioning procedure before building")
    endif()
endif()

# Force _ctype_ symbol to be included (needed by esp-sr libfst.a with GCC 15.2)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u _ctype_")

# LVGL resolves custom allocator symbols from libmain.a after liblvgl.a has been
# scanned. Force one allocator symbol so the archive member is included.
if(CONFIG_LV_USE_CUSTOM_MALLOC)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")
endif()
