cmake_minimum_required(VERSION 3.16)
project(sd_archive_dev C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(sd_archive_store STATIC src/sd_archive_store.c)
target_include_directories(sd_archive_store PUBLIC include)

if(MSVC)
    target_compile_definitions(sd_archive_store PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(sd_archive_store PRIVATE -Wall -Wextra -Werror)
endif()

add_executable(test_sd_archive_store tests/test_sd_archive_store.c)
target_link_libraries(test_sd_archive_store PRIVATE sd_archive_store)
target_include_directories(test_sd_archive_store PRIVATE include)

if(NOT MSVC)
    target_compile_options(test_sd_archive_store PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME sd_archive_store COMMAND test_sd_archive_store)
