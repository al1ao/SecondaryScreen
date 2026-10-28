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
    src/encode/jpeg_encoder.cpp
    src/net/tcp_server.cpp
    src/net/frame_sender.cpp
)
target_include_directories(padscreen-core PUBLIC src)
target_compile_definitions(padscreen-core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_compile_options(padscreen-core PUBLIC /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(padscreen-core PUBLIC ws2_32 windowscodecs ole32 oleaut32 gdi32 user32)
add_executable(padscreen-host src/main.cpp)
target_link_libraries(padscreen-host PRIVATE padscreen-core)

include(CTest)
if(BUILD_TESTING)
    add_executable(padscreen-tests tests/stage1_tests.cpp)
    target_link_libraries(padscreen-tests PRIVATE padscreen-core psapi)
    add_test(NAME stage1-core COMMAND padscreen-tests)
endif()
