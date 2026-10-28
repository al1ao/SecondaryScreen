cmake_minimum_required(VERSION 3.21)
project(padscreen-host LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "PadScreen host requires Windows and MSVC (Visual Studio 2022).")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(padscreen-core STATIC
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
target_include_directories(padscreen-core PUBLIC src)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/encode/bgra_to_nv12.hlsl" NV12_SHADER_SOURCE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/encode/bgra_to_nv12.hlsl")
configure_file(cmake/bgra_to_nv12_source.h.in generated/bgra_to_nv12_source.h @ONLY)
target_include_directories(padscreen-core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(padscreen-core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_compile_options(padscreen-core PUBLIC /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(padscreen-core PUBLIC ws2_32 windowscodecs ole32 oleaut32 gdi32 user32
    mfplat mfuuid mfreadwrite d3d11 dxgi d3dcompiler windowsapp runtimeobject winmm)
add_executable(padscreen-host src/main.cpp)
target_link_libraries(padscreen-host PRIVATE padscreen-core)

include(CTest)
if(BUILD_TESTING)
    add_executable(padscreen-tests tests/stage1_tests.cpp)
    target_link_libraries(padscreen-tests PRIVATE padscreen-core psapi)
    add_test(NAME stage1-core COMMAND padscreen-tests)
    add_executable(padscreen-stage2-tests tests/stage2_tests.cpp)
    target_link_libraries(padscreen-stage2-tests PRIVATE padscreen-core psapi)
    add_test(NAME stage2-hardware COMMAND padscreen-stage2-tests)
    add_executable(padscreen-stage3-tests tests/stage3_tests.cpp)
    target_link_libraries(padscreen-stage3-tests PRIVATE padscreen-core)
    add_test(NAME stage3-display COMMAND padscreen-stage3-tests)
    add_executable(padscreen-stage4-tests tests/stage4_tests.cpp)
    target_link_libraries(padscreen-stage4-tests PRIVATE padscreen-core)
    add_test(NAME stage4-cadence COMMAND padscreen-stage4-tests)
    add_executable(padscreen-stage2-probe tests/stage2_probe.cpp)
    target_link_libraries(padscreen-stage2-probe PRIVATE padscreen-core psapi)
endif()
