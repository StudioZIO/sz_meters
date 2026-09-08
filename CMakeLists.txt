cmake_minimum_required(VERSION 3.15)

project(sz_meters
        VERSION 1.0.0
        DESCRIPTION "BS.1770-4 loudness, inter-sample true peak and AES17 signal metering"
        LANGUAGES CXX)

# The DSP is header-only and depends on nothing but the standard library, so
# consuming this from another project is one target_link_libraries away.
add_library(sz_meters INTERFACE)
add_library(sz_meters::sz_meters ALIAS sz_meters)

target_include_directories(sz_meters INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

target_compile_features(sz_meters INTERFACE cxx_std_17)

# Tests are built when this is the top-level project, and skipped when it is
# pulled in as a subdirectory - a consumer should not pay for our test binary.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()

    add_executable(sz_meters_tests tests/test_main.cpp)
    target_link_libraries(sz_meters_tests PRIVATE sz_meters)

    if(MSVC)
        target_compile_options(sz_meters_tests PRIVATE /W4)
    else()
        target_compile_options(sz_meters_tests PRIVATE -Wall -Wextra)
    endif()

    add_test(NAME sz_meters_tests COMMAND sz_meters_tests)
endif()
