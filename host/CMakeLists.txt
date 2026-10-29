cmake_minimum_required(VERSION 3.21)
project(secondaryscreen-host LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "SecondaryScreen host requires Windows and MSVC (Visual Studio 2022).")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(secondaryscreen-core STATIC
    src/app/cli_options.cpp
    src/display/adaptive_mode.cpp
    src/protocol/protocol.cpp
    src/capture/gdi_capture.cpp
    src/capture/wgc_capture.cpp
    src/capture/gdi_monitor_capture.cpp
    src/display/monitor_enum.cpp
    src/encode/jpeg_encoder.cpp
    src/encode/mf_h264_encoder.cpp
    src/encode/mf_event_waiter.cpp
    src/encode/encoder_factory.cpp
    src/encode/h264_annex_b.cpp
    src/encode/gpu_nv12_converter.cpp
    src/util/gpu.cpp
    src/net/tcp_server.cpp
    src/net/frame_sender.cpp
    src/net/pipeline_session.cpp
)
target_include_directories(secondaryscreen-core PUBLIC src)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/encode/bgra_to_nv12.hlsl" NV12_SHADER_SOURCE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/encode/bgra_to_nv12.hlsl")
configure_file(cmake/bgra_to_nv12_source.h.in generated/bgra_to_nv12_source.h @ONLY)
target_include_directories(secondaryscreen-core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(secondaryscreen-core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_compile_options(secondaryscreen-core PUBLIC /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(secondaryscreen-core PUBLIC ws2_32 windowscodecs ole32 oleaut32 gdi32 user32
    mfplat mfuuid mfreadwrite d3d11 dxgi d3dcompiler windowsapp runtimeobject winmm)
add_executable(secondaryscreen-host src/main.cpp)
target_link_libraries(secondaryscreen-host PRIVATE secondaryscreen-core)

add_library(secondaryscreen-app STATIC src/app/process.cpp src/app/config.cpp src/app/host_service.cpp src/adb/adb_manager.cpp)
target_link_libraries(secondaryscreen-app PUBLIC secondaryscreen-core shell32 advapi32)
target_include_directories(secondaryscreen-app SYSTEM PRIVATE third_party)
option(SECONDARYSCREEN_BUILD_GUI "Build Qt tray application" OFF)
if(SECONDARYSCREEN_BUILD_GUI)
    add_subdirectory(gui)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(secondaryscreen-stage6-tests tests/stage6_tests.cpp)
    target_link_libraries(secondaryscreen-stage6-tests PRIVATE secondaryscreen-app)
    add_test(NAME stage6-adaptation COMMAND secondaryscreen-stage6-tests)
    set_tests_properties(stage6-adaptation PROPERTIES TIMEOUT 30)
    add_executable(secondaryscreen-stage5-tests tests/stage5_tests.cpp)
    target_link_libraries(secondaryscreen-stage5-tests PRIVATE secondaryscreen-app)
    add_test(NAME stage5-shell COMMAND secondaryscreen-stage5-tests)
    set_tests_properties(stage5-shell PROPERTIES TIMEOUT 30)
    add_executable(secondaryscreen-tests tests/stage1_tests.cpp)
    target_link_libraries(secondaryscreen-tests PRIVATE secondaryscreen-core psapi)
    add_test(NAME stage1-core COMMAND secondaryscreen-tests)
    add_executable(secondaryscreen-stage2-tests tests/stage2_tests.cpp)
    target_link_libraries(secondaryscreen-stage2-tests PRIVATE secondaryscreen-core psapi)
    add_test(NAME stage2-hardware COMMAND secondaryscreen-stage2-tests)
    add_executable(secondaryscreen-stage3-tests tests/stage3_tests.cpp)
    target_link_libraries(secondaryscreen-stage3-tests PRIVATE secondaryscreen-core)
    add_test(NAME stage3-display COMMAND secondaryscreen-stage3-tests)
    add_executable(secondaryscreen-stage4-tests tests/stage4_tests.cpp)
    target_link_libraries(secondaryscreen-stage4-tests PRIVATE secondaryscreen-core)
    add_test(NAME stage4-cadence COMMAND secondaryscreen-stage4-tests)
    add_executable(secondaryscreen-stage4-handshake-tests tests/stage4_handshake_tests.cpp)
    target_link_libraries(secondaryscreen-stage4-handshake-tests PRIVATE secondaryscreen-core)
    add_test(NAME stage4-handshake COMMAND secondaryscreen-stage4-handshake-tests)
    set_tests_properties(stage4-handshake PROPERTIES TIMEOUT 20)
    add_executable(secondaryscreen-stage2-probe tests/stage2_probe.cpp)
    target_link_libraries(secondaryscreen-stage2-probe PRIVATE secondaryscreen-core psapi)
endif()
