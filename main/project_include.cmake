# esp-sr 2.4.7 invokes a bare `python` command while packing models. On
# Windows that command may not be visible to Ninja's cmd.exe subprocess even
# though ESP-IDF has already resolved its Python interpreter. Patch only the
# known command before component CMakeLists files are processed.
set(nuwax_esp_sr_cmake
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp-sr/CMakeLists.txt")
if(EXISTS "${nuwax_esp_sr_cmake}")
    file(READ "${nuwax_esp_sr_cmake}" nuwax_esp_sr_cmake_content)
    set(nuwax_esp_sr_bare_python
        [=[COMMAND python ${MVMODEL_EXE}]=])
    set(nuwax_esp_sr_idf_python
        [=[COMMAND ${python} ${MVMODEL_EXE}]=])
    string(FIND "${nuwax_esp_sr_cmake_content}"
           "${nuwax_esp_sr_bare_python}"
           nuwax_esp_sr_bare_python_index)
    if(NOT nuwax_esp_sr_bare_python_index EQUAL -1)
        set(nuwax_esp_sr_build_dir_property
            [=[idf_build_get_property(build_dir BUILD_DIR)]=])
        set(nuwax_esp_sr_python_properties
            [=[idf_build_get_property(build_dir BUILD_DIR)
        idf_build_get_property(python PYTHON)]=])
        string(REPLACE
               "${nuwax_esp_sr_build_dir_property}"
               "${nuwax_esp_sr_python_properties}"
               nuwax_esp_sr_cmake_content
               "${nuwax_esp_sr_cmake_content}")
        string(REPLACE
               "${nuwax_esp_sr_bare_python}"
               "${nuwax_esp_sr_idf_python}"
               nuwax_esp_sr_cmake_content
               "${nuwax_esp_sr_cmake_content}")
        file(WRITE "${nuwax_esp_sr_cmake}"
             "${nuwax_esp_sr_cmake_content}")
        message(STATUS
            "Nuwax esp-sr Windows fix: model packer uses ESP-IDF Python")
    endif()

    file(READ "${nuwax_esp_sr_cmake}" nuwax_esp_sr_cmake_content)
    string(FIND "${nuwax_esp_sr_cmake_content}"
           "${nuwax_esp_sr_idf_python}"
           nuwax_esp_sr_idf_python_index)
    string(FIND "${nuwax_esp_sr_cmake_content}"
           "idf_build_get_property(python PYTHON)"
           nuwax_esp_sr_python_property_index)
    if(nuwax_esp_sr_idf_python_index EQUAL -1 OR
       nuwax_esp_sr_python_property_index EQUAL -1)
        message(FATAL_ERROR
            "esp-sr model packer layout changed; update the Windows Python fix")
    endif()
endif()
