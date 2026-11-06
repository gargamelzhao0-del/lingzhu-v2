# Test-only timing; preserve the SDK and all command/queue/yield semantics.
if(CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE)
    idf_component_get_property(sd_profile_lib esp_driver_sdmmc COMPONENT_LIB)
    idf_component_get_property(sd_profile_root esp_driver_sdmmc COMPONENT_DIR)
    idf_build_get_property(sd_profile_python PYTHON)
    set(sd_profile_script "${CMAKE_CURRENT_LIST_DIR}/../prepare-sd-event-profile.py")
    set(sd_profile_output "${CMAKE_BINARY_DIR}/sd-event-profile")
    execute_process(COMMAND "${sd_profile_python}" "${sd_profile_script}"
        --component "${sd_profile_root}" --output "${sd_profile_output}"
        RESULT_VARIABLE sd_profile_result)
    if(NOT sd_profile_result EQUAL 0)
        message(FATAL_ERROR "SD profiling source verification failed; review SDK version")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${sd_profile_script}" "${sd_profile_root}/src/sd_host_sdmmc.c"
        "${sd_profile_root}/src/sd_trans_sdmmc.c"
        "${sd_profile_root}/include/esp_private/sd_host_private.h")
    get_target_property(sd_profile_sources ${sd_profile_lib} SOURCES)
    set(sd_profile_replaced 0)
    set(sd_profile_updated)
    foreach(sd_source IN LISTS sd_profile_sources)
        get_filename_component(sd_absolute "${sd_source}" ABSOLUTE BASE_DIR "${sd_profile_root}")
        if(sd_absolute STREQUAL "${sd_profile_root}/src/sd_host_sdmmc.c")
            list(APPEND sd_profile_updated "${sd_profile_output}/src/sd_host_sdmmc.c")
            math(EXPR sd_profile_replaced "${sd_profile_replaced}+1")
        elseif(sd_absolute STREQUAL "${sd_profile_root}/src/sd_trans_sdmmc.c")
            list(APPEND sd_profile_updated "${sd_profile_output}/src/sd_trans_sdmmc.c")
            math(EXPR sd_profile_replaced "${sd_profile_replaced}+1")
        else()
            list(APPEND sd_profile_updated "${sd_source}")
        endif()
    endforeach()
    if(NOT sd_profile_replaced EQUAL 2)
        message(FATAL_ERROR "SD profiling expected two SDK source substitutions")
    endif()
    set_property(TARGET ${sd_profile_lib} PROPERTY SOURCES "${sd_profile_updated}")
    target_include_directories(${sd_profile_lib} BEFORE PRIVATE
        "${sd_profile_output}/include" "${CMAKE_CURRENT_LIST_DIR}/../../main/board")
endif()
