cmake_minimum_required(VERSION 3.20)
project(Floral VERSION 1.0 LANGUAGES CXX)

# ─────────────────────────────────────────────
#  Floral —— Win32 + Direct2D 音乐播放器
#
#  用 MinGW 或 MSVC 都能编：
#    cmake -S . -B build-cmake -G "MinGW Makefiles"
#    cmake --build build-cmake
#
#  选项：
#    -DFLORAL_PORTABLE=ON     静态链接运行时，单文件便携版
#    -DFLORAL_NO_TOGETHER=ON  去掉「一起听」（对外发布版）
#
#  PowerShell 的等价命令见 scripts/build-cmake.ps1
# ─────────────────────────────────────────────

option(FLORAL_PORTABLE    "静态链接运行时，产出单文件便携版" OFF)
option(FLORAL_NO_TOGETHER "不编译「一起听」功能" OFF)
option(FLORAL_EMBED_SERVICES "把接口服务内嵌进 exe" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(FLORAL_SOURCES
    src/main.cpp
    src/together.cpp
    src/log.cpp
    src/theme.cpp
    src/data.cpp
    src/audio.cpp
    src/kugou.cpp
    src/lang.cpp
)

add_executable(floral WIN32 ${FLORAL_SOURCES})

# 只给 CXX 加头文件目录：RC 编译器（windres 会转发给 cc1）遇到带空格的
# 绝对路径 -I 参数容易出错，而 gen_app.rc 里用的是绝对 #include，不需要它
target_include_directories(floral PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

if(FLORAL_NO_TOGETHER)
    target_compile_definitions(floral PRIVATE FLORAL_NO_TOGETHER)
endif()

# ── 内嵌接口服务 ──
# app.rc 里的路径是相对 PowerShell 脚本写的；CMake 这边生成一份用绝对路径的，
# 免得 rc/windres 的当前目录不同导致找不到文件。
set(KUGOU_EXE    "${CMAKE_CURRENT_SOURCE_DIR}/sidecars/kugou-api/bin/app_win.exe")
set(SIDECARS_ZIP "${CMAKE_CURRENT_SOURCE_DIR}/build/sidecars.zip")

if(FLORAL_EMBED_SERVICES)
    if(EXISTS "${KUGOU_EXE}" AND EXISTS "${SIDECARS_ZIP}")
        enable_language(RC)
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gen_app.rc"
"#include \"${CMAKE_CURRENT_SOURCE_DIR}/src/resource.h\"

IDR_KUGOU_API   RCDATA  \"${KUGOU_EXE}\"
IDR_SIDECARS    RCDATA  \"${SIDECARS_ZIP}\"
")
        target_sources(floral PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gen_app.rc")
        message(STATUS "内嵌接口服务：开启")
    else()
        message(WARNING "找不到 app_win.exe 或 sidecars.zip，跳过内嵌（运行时会要求本机已有服务）")
    endif()
endif()

# ── 库 ──
target_link_libraries(floral PRIVATE
    d2d1 dwrite windowscodecs ole32 uuid shell32 gdi32 dwmapi winmm wininet ws2_32 iphlpapi
)

# ── 各编译器的开关 ──
if(MSVC)
    target_compile_definitions(floral PRIVATE
        UNICODE _UNICODE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
    )
    target_compile_options(floral PRIVATE /W3 /utf-8 /permissive-)
    if(FLORAL_PORTABLE)
        # 静态运行库：不依赖 VC++ Redistributable
        set_property(TARGET floral PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    # 控制台窗口：WIN32 目标已经用 /SUBSYSTEM:WINDOWS，wWinMain 是入口
else()
    target_compile_definitions(floral PRIVATE UNICODE _UNICODE)
    target_compile_options(floral PRIVATE -Wall -O2)
    # -municode：走 wWinMain；-mwindows：不弹控制台
    target_link_options(floral PRIVATE -municode -mwindows)
    if(FLORAL_PORTABLE)
        target_link_options(floral PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()

# ── 输出位置：统一放到 build/，和 PowerShell 脚本一致 ──
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
