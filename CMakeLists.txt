# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "7.0.223")
endif()

if(NOT PROJECT_VER MATCHES "^7\\.0\\.([0-9]+)$")
    message(FATAL_ERROR
        "PROJECT_VER must use the 7.0.<release> format for OTA identity")
endif()
math(EXPR NUWAX_EXPECTED_OTA_RELEASE_SEQUENCE
     "7000000 + ${CMAKE_MATCH_1}")
if(NOT DEFINED NUWAX_OTA_RELEASE_SEQUENCE)
    set(NUWAX_OTA_RELEASE_SEQUENCE
        "${NUWAX_EXPECTED_OTA_RELEASE_SEQUENCE}")
elseif(NOT NUWAX_OTA_RELEASE_SEQUENCE STREQUAL
           NUWAX_EXPECTED_OTA_RELEASE_SEQUENCE)
    message(FATAL_ERROR
        "PROJECT_VER ${PROJECT_VER} requires OTA release sequence ${NUWAX_EXPECTED_OTA_RELEASE_SEQUENCE}, got ${NUWAX_OTA_RELEASE_SEQUENCE}")
endif()
message(STATUS
    "Nuwax firmware identity: version=${PROJECT_VER} release_sequence=${NUWAX_OTA_RELEASE_SEQUENCE}")

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/lib/lvgl")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

# The production face pack is a raw data partition, not part of either OTA
# application image. Keep its digest pinned so a stale simulator artifact
# cannot silently enter a device build. This contract must be resolved before
# project() configures components because main/CMakeLists.txt consumes it when
# creating the firmware compile definitions.
set(NUWAX_EMOTION_PACK
    "${CMAKE_CURRENT_LIST_DIR}/assets/emotion/emotion-assets.bin")
set(NUWAX_EMOTION_PACK_SHA256
    "c8e9258f5963f6b08089ef80af6190ba1053e3e73dfc990d529d090a6f752897")
set(NUWAX_EMOTION_PREVIOUS_RELEASE_V4_HYBRID_SHA256
    "ad780e8eb81c976c37402a42f4576c748440d021990936991943481e6a773f66")
set(NUWAX_EMOTION_PREVIOUS_RELEASE_V4_SHA256
    "a927fcee8fdd7562f8f419e97c618d63d5a3ca1e1d93e9da54f4d857018b4a56")
set(NUWAX_EMOTION_PREVIOUS_RELEASE_V3_SHA256
    "262cfa59cb200f7a0e147ed8ac25dd8659edc3e73c758365a209ffd3f53c367e")
set(NUWAX_EMOTION_PREVIOUS_RELEASE_V2_SHA256
    "de648186afa62dd7f6ca6e9a2743d5dac69dfe0117dc42e29147a57878666902")
set(NUWAX_EMOTION_PREVIOUS_V2_SHA256
    "007f573cc5c52821fcd3cd1534f79e7c8acd1305acc4a1f9c5047b63b63e3552")
set(NUWAX_EMOTION_LEGACY_V1_SHA256
    "1c8461be24f2e2554b8a11d3c7b751cf05867dd511f66e05f2f4a358c24b44dc")
set(NUWAX_EMOTION_COMPATIBLE_SHA256
    "${NUWAX_EMOTION_PACK_SHA256};${NUWAX_EMOTION_PREVIOUS_RELEASE_V4_HYBRID_SHA256};${NUWAX_EMOTION_PREVIOUS_RELEASE_V4_SHA256};${NUWAX_EMOTION_PREVIOUS_RELEASE_V3_SHA256};${NUWAX_EMOTION_PREVIOUS_RELEASE_V2_SHA256};${NUWAX_EMOTION_PREVIOUS_V2_SHA256};${NUWAX_EMOTION_LEGACY_V1_SHA256}" CACHE STRING
    "Semicolon-separated emotion pack digests accepted by this firmware")
if(NOT EXISTS "${NUWAX_EMOTION_PACK}")
    message(FATAL_ERROR "Production emotion pack is missing: ${NUWAX_EMOTION_PACK}")
endif()
file(SHA256 "${NUWAX_EMOTION_PACK}" NUWAX_EMOTION_PACK_ACTUAL_SHA256)
if(NOT NUWAX_EMOTION_PACK_ACTUAL_SHA256 STREQUAL NUWAX_EMOTION_PACK_SHA256)
    message(FATAL_ERROR
        "Production emotion pack digest mismatch: expected ${NUWAX_EMOTION_PACK_SHA256}, got ${NUWAX_EMOTION_PACK_ACTUAL_SHA256}")
endif()
set(NUWAX_EMOTION_COMPATIBLE_SHA256_LIST
    ${NUWAX_EMOTION_COMPATIBLE_SHA256}
    "${NUWAX_EMOTION_PACK_SHA256}"
    "${NUWAX_EMOTION_PREVIOUS_RELEASE_V4_HYBRID_SHA256}"
    "${NUWAX_EMOTION_PREVIOUS_RELEASE_V4_SHA256}"
    "${NUWAX_EMOTION_PREVIOUS_RELEASE_V3_SHA256}"
    "${NUWAX_EMOTION_PREVIOUS_RELEASE_V2_SHA256}"
    "${NUWAX_EMOTION_PREVIOUS_V2_SHA256}"
    "${NUWAX_EMOTION_LEGACY_V1_SHA256}")
list(REMOVE_DUPLICATES NUWAX_EMOTION_COMPATIBLE_SHA256_LIST)
# CMake preserves cache values across builds. Normalize it so a build tree
# created by the v1 release cannot reject the new release digest, while any
# explicitly added field-compatibility digests remain accepted.
set(NUWAX_EMOTION_COMPATIBLE_SHA256
    "${NUWAX_EMOTION_COMPATIBLE_SHA256_LIST}" CACHE STRING
    "Semicolon-separated emotion pack digests accepted by this firmware" FORCE)
foreach(compatible_sha IN LISTS NUWAX_EMOTION_COMPATIBLE_SHA256_LIST)
    string(LENGTH "${compatible_sha}" compatible_sha_length)
    if(NOT compatible_sha_length EQUAL 64 OR
       NOT compatible_sha MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "Invalid compatible emotion SHA-256: ${compatible_sha}")
    endif()
endforeach()
list(FIND NUWAX_EMOTION_COMPATIBLE_SHA256_LIST
     "${NUWAX_EMOTION_PACK_SHA256}" NUWAX_EMOTION_RELEASE_SHA_INDEX)
if(NUWAX_EMOTION_RELEASE_SHA_INDEX EQUAL -1)
    message(FATAL_ERROR
        "The release emotion SHA-256 must be present in NUWAX_EMOTION_COMPATIBLE_SHA256")
endif()
string(JOIN "," NUWAX_EMOTION_COMPATIBLE_SHA256_CSV
       ${NUWAX_EMOTION_COMPATIBLE_SHA256_LIST})

project(NuwaxAIPartner)

# Reject stale sdkconfig files instead of silently shipping the cJSON default
# of 1000 recursive levels into the 16 KiB WebSocket RX task.
if(CONFIG_CJSON_NESTING_LIMIT GREATER 32 OR CONFIG_CJSON_NESTING_LIMIT LESS 1)
    message(FATAL_ERROR "Set CONFIG_CJSON_NESTING_LIMIT=32 (supported range 1..32); unsafe recursive JSON budget")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/tools/cmake/protocomm-lifecycle-fix.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/tools/cmake/rnnt-growth-fix.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/tools/cmake/sd-event-profile.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/tools/cmake/sd-status-fix.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/tools/cmake/sd-tcp-writable.cmake")

esptool_py_flash_to_partition(flash "emotion_0" "${NUWAX_EMOTION_PACK}")
message(STATUS
    "Nuwax emotion pack: partition=emotion_0 sha256=${NUWAX_EMOTION_PACK_SHA256} compatible=${NUWAX_EMOTION_COMPATIBLE_SHA256_CSV}")

# esp_audio_codec 1.0.1 publishes an unquoted -L/-l pair. On Windows, that
# pair is split when the project path contains spaces. Keep the managed
# component pristine and replace only those interface entries with a
# project-owned imported archive target.
idf_component_get_property(esp_audio_codec_component espressif__esp_audio_codec COMPONENT_LIB)
idf_component_get_property(esp_audio_codec_dir espressif__esp_audio_codec COMPONENT_DIR)
set(esp_audio_codec_archive
    "${esp_audio_codec_dir}/lib/${CONFIG_IDF_TARGET}/libesp_audio_codec.a")
if(NOT TARGET ${esp_audio_codec_component} OR NOT EXISTS "${esp_audio_codec_archive}")
    message(FATAL_ERROR "esp_audio_codec 1.0.1 layout changed; update the Windows path fix")
endif()
add_prebuilt_library(nuwax_esp_audio_codec_prebuilt "${esp_audio_codec_archive}")

get_target_property(esp_audio_codec_links ${esp_audio_codec_component} INTERFACE_LINK_LIBRARIES)
if(NOT esp_audio_codec_links OR esp_audio_codec_links MATCHES "-NOTFOUND$")
    set(esp_audio_codec_links "")
endif()
list(FILTER esp_audio_codec_links EXCLUDE REGEX "^-L")
list(FILTER esp_audio_codec_links EXCLUDE REGEX "^esp_audio_codec$")
list(APPEND esp_audio_codec_links nuwax_esp_audio_codec_prebuilt)
set_property(TARGET ${esp_audio_codec_component}
             PROPERTY INTERFACE_LINK_LIBRARIES "${esp_audio_codec_links}")

# Fix esp-dsp C++26 compatibility: std::cos/sin/atan require <cmath>
idf_component_get_property(esp_dsp_lib espressif__esp-dsp COMPONENT_LIB)
if(TARGET ${esp_dsp_lib})
    target_compile_options(${esp_dsp_lib} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-include cmath>)
endif()
