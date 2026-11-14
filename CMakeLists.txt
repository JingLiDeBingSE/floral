cmake_minimum_required(VERSION 3.20)

# Default to a Release build. Set before project(), which initializes an unset build type to Debug for MSVC
if(NOT DEFINED CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")
endif()

project(Floral VERSION 1.0 LANGUAGES CXX)

# ─────────────────────────────────────────────
#  Floral — Win32 + Direct2D music player
#
#  Builds with MinGW or MSVC:
#    cmake -S . -B build-cmake -G "MinGW Makefiles"
#    cmake --build build-cmake
#
#  Options:
#    -DFLORAL_PORTABLE=ON     static runtime, single-file portable build
#    -DFLORAL_NO_TOGETHER=ON  drop "Listen Together" (public release build)
#
#  See scripts/build-cmake.ps1 for equivalent PowerShell commands
# ─────────────────────────────────────────────

option(FLORAL_PORTABLE    "静态链接运行时，产出单文件便携版" OFF)
option(FLORAL_NO_TOGETHER "不编译「一起听」功能" OFF)
option(FLORAL_EMBED_SERVICES "把接口服务内嵌进 exe" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FLORAL_SOURCES
    src/main.cpp
    src/together.cpp
    src/log.cpp
    src/theme.cpp
    src/data.cpp
    src/audio.cpp
    src/kugou.cpp
    src/lang.cpp
    src/tiles.cpp
    src/vinyl.cpp
)

add_executable(floral WIN32 ${FLORAL_SOURCES})

# Add include dirs for CXX only: the RC compiler (windres forwards to cc1)
# chokes on absolute -I paths with spaces; gen_app.rc includes by absolute path, so it is not needed
target_include_directories(floral PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

if(FLORAL_NO_TOGETHER)
    target_compile_definitions(floral PRIVATE FLORAL_NO_TOGETHER)
endif()

# ── Embedded API services ──
# Paths in app.rc are relative to the PowerShell script; CMake generates a
# copy with absolute paths so rc/windres find the files regardless of cwd.
set(KUGOU_EXE    "${CMAKE_CURRENT_SOURCE_DIR}/sidecars/kugou-api/bin/app_win.exe")
set(SIDECARS_ZIP "${CMAKE_CURRENT_SOURCE_DIR}/build/sidecars.zip")

if(FLORAL_EMBED_SERVICES)
    if(EXISTS "${KUGOU_EXE}" AND EXISTS "${SIDECARS_ZIP}")
        enable_language(RC)
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gen_app.rc"
"#include \"${CMAKE_CURRENT_SOURCE_DIR}/src/resource.h\"

IDI_APP         ICON    \"${CMAKE_CURRENT_SOURCE_DIR}/src/app.ico\"
IDR_KUGOU_API   RCDATA  \"${KUGOU_EXE}\"
IDR_SIDECARS    RCDATA  \"${SIDECARS_ZIP}\"
")
        target_sources(floral PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gen_app.rc")
        message(STATUS "内嵌接口服务：开启")
    else()
        message(WARNING "找不到 app_win.exe 或 sidecars.zip，跳过内嵌（运行时会要求本机已有服务）")
    endif()
endif()

# ── Libraries ──
target_link_libraries(floral PRIVATE
    d2d1 dwrite windowscodecs ole32 uuid shell32 gdi32 dwmapi winmm wininet ws2_32 iphlpapi
)

# ── Per-compiler flags ──
if(MSVC)
    target_compile_definitions(floral PRIVATE
        UNICODE _UNICODE
        _CRT_SECURE_NO_WARNINGS
    )
    target_compile_options(floral PRIVATE /W3 /utf-8 /permissive-)
    if(FLORAL_PORTABLE)
        # Static runtime: no VC++ Redistributable dependency
        set_property(TARGET floral PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    # Console window: the WIN32 target already uses /SUBSYSTEM:WINDOWS; wWinMain is the entry point
else()
    target_compile_definitions(floral PRIVATE UNICODE _UNICODE)
    target_compile_options(floral PRIVATE -Wall -O2)
    # -municode: use wWinMain; -mwindows: no console window
    target_link_options(floral PRIVATE -municode -mwindows)
    if(FLORAL_PORTABLE)
        target_link_options(floral PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()

# ── Output: everything goes to build/, same as the PowerShell script ──
set(FLORAL_OUT "${CMAKE_CURRENT_SOURCE_DIR}/build")
if(FLORAL_NO_TOGETHER)
    set(FLORAL_NAME floral_std_portable)
else()
    set(FLORAL_NAME floral_portable)
endif()
set_target_properties(floral PROPERTIES
    OUTPUT_NAME "${FLORAL_NAME}"
    RUNTIME_OUTPUT_DIRECTORY "${FLORAL_OUT}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${FLORAL_OUT}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${FLORAL_OUT}"
)
