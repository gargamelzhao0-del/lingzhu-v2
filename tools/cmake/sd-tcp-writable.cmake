# Both SD replay routes can opt in. This changes a socket readiness
# notification threshold, not TCP window sizes, wire format or allocation.
# Keep the installed ESP-IDF pristine and export the same value to consumers
# of lwIP headers (including the startup diagnostic).
if(CONFIG_NUWAX_SD_REPLAY_EARLY_TCP_WRITABLE)
    if(NOT CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE AND
       NOT CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE)
        message(FATAL_ERROR "Early TCP writable policy requires an SD replay route")
    endif()
    math(EXPR nuwax_tcp_lowat "2 * ${CONFIG_LWIP_TCP_MSS} + 1")
    if(CONFIG_LWIP_TCP_SND_BUF_DEFAULT LESS_EQUAL nuwax_tcp_lowat)
        message(FATAL_ERROR "TCP send buffer must exceed the early writable threshold")
    endif()
    idf_component_get_property(nuwax_lwip_lib lwip COMPONENT_LIB)
    target_compile_definitions(${nuwax_lwip_lib} PUBLIC "TCP_SNDLOWAT=${nuwax_tcp_lowat}")
    message(STATUS "SD TCP writable threshold: ${nuwax_tcp_lowat}; send buffer unchanged")
endif()

if(CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE)
    idf_component_get_property(nuwax_wait_lib main COMPONENT_LIB)
    target_link_options(${nuwax_wait_lib} INTERFACE
        "-Wl,--wrap=sys_arch_sem_wait" "-u __wrap_sys_arch_sem_wait")
    if(NOT CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE)
        target_link_options(${nuwax_wait_lib} INTERFACE
            "-Wl,--wrap=tcp_output" "-u __wrap_tcp_output")
    endif()
endif()

if(CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE)
    # Wrappers preserve SDK results. A separate bounded TCP/IP callback may
    # retry rejected output for the PC gateway; keep IDF sources pristine.
    idf_component_get_property(nuwax_probe_lib main COMPONENT_LIB)
    target_link_options(${nuwax_probe_lib} INTERFACE
        "-Wl,--wrap=esp_netif_transmit_wrap" "-Wl,--wrap=tcp_output"
        "-Wl,--wrap=tcp_input")
endif()
