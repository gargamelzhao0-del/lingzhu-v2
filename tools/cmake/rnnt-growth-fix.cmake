# Pinned binary repair because ESP-SR does not ship the RNNT C source.
# No vendor files change. An unreviewed library fails closed at build time.
if(CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET)
    if(NOT CONFIG_IDF_TARGET_ESP32S3 OR NOT TARGET multinet)
        message(FATAL_ERROR "RNNT growth fix is reviewed only for ESP32-S3 / ESP-SR 2.4.7")
    endif()
    idf_build_get_property(rnnt_python PYTHON)
    idf_build_get_property(rnnt_elf EXECUTABLE)
    get_target_property(rnnt_original multinet IMPORTED_LOCATION)
    set(rnnt_script "${CMAKE_CURRENT_LIST_DIR}/../patch-rnnt-growth.py")
    set(rnnt_dir "${CMAKE_BINARY_DIR}/rnnt-fix")
    set(rnnt_patched "${rnnt_dir}/libmultinet.a")
    add_custom_command(OUTPUT "${rnnt_patched}" "${rnnt_dir}/patch-report.json"
        COMMAND "${rnnt_python}" "${rnnt_script}" patch --input "${rnnt_original}"
            --output "${rnnt_patched}" --report "${rnnt_dir}/patch-report.json"
        DEPENDS "${rnnt_original}" "${rnnt_script}" VERBATIM)
    add_custom_target(nuwax_rnnt_growth_fix DEPENDS "${rnnt_patched}" "${rnnt_dir}/patch-report.json")
    set_property(TARGET multinet PROPERTY IMPORTED_LOCATION "${rnnt_patched}")
    add_dependencies(${rnnt_elf} nuwax_rnnt_growth_fix)
    add_custom_command(TARGET ${rnnt_elf} POST_BUILD
        COMMAND "${rnnt_python}" "${rnnt_script}" verify --input "$<TARGET_FILE:${rnnt_elf}>"
            --objdump "${CMAKE_OBJDUMP}" --report "${rnnt_dir}/elf-verification.json"
        VERBATIM)
    # Re-run the linked verification when its verifier changes too.
    set_property(TARGET ${rnnt_elf} APPEND PROPERTY LINK_DEPENDS "${rnnt_script}")
endif()
