# apo/CMakeLists.txt — dsp-core 静态库 + DrcApo.dll（COM 壳）
#
# 写区：apo/CMakeLists.txt、apo/com/**、apo/include/**（TASK-002）
#       apo/dsp/**、tests/dsp/**（TASK-001，本文件只「编入」其源集，不改其内容）
#
# 目标：
#   drc_dsp   STATIC  dsp-core（apo/dsp/**，纯 C++17、零 Windows 依赖）—— tests/dsp 依赖此名
#   DrcApo    SHARED  APO COM 壳（apo/com/**），**静态链入 drc_dsp**
#
cmake_minimum_required(VERSION 3.15)
project(DrcApo LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "DrcApo 仅支持 MSVC（Windows APO COM DLL）。")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# APO 由 audiodg.exe 加载 => 动态 CRT，避免跨模块堆问题
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# Release 显式 /O2（TASK-001 卡要求；MSVC Release 默认亦为 /O2）
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2")

# ── dsp-core（TASK-001 写区；本卡只编入，不改其内容） ────────────────────
set(DRC_DSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dsp")
file(GLOB_RECURSE DRC_DSP_SOURCES CONFIGURE_DEPENDS "${DRC_DSP_DIR}/*.cpp")
file(GLOB_RECURSE DRC_DSP_HEADERS CONFIGURE_DEPENDS "${DRC_DSP_DIR}/*.h")

if(NOT DRC_DSP_SOURCES)
    # dsp-core 尚未落地：以 INTERFACE 占位，保证 tests/dsp 与 DrcApo 仍可配置（壳直通）
    message(WARNING "apo/dsp 无源文件 => drc_dsp 以 INTERFACE 占位（壳直通）")
    add_library(drc_dsp INTERFACE)
    target_include_directories(drc_dsp INTERFACE "${DRC_DSP_DIR}/include")
else()
    add_library(drc_dsp STATIC ${DRC_DSP_SOURCES} ${DRC_DSP_HEADERS})
    target_include_directories(drc_dsp PUBLIC "${DRC_DSP_DIR}/include")
    target_compile_options(drc_dsp PRIVATE /W4 /utf-8)
endif()

# ── DrcApo.dll（COM 壳） ────────────────────────────────────────────────
add_library(DrcApo SHARED
    com/drc_apo.cpp
    com/drc_classfactory.cpp
    com/drc_dllmain.cpp
    com/drc_ipc_shared.cpp
    com/drc_dsp_adapter.cpp
    com/drc_apo_log.cpp
    com/drc_ipc_layout_check.cpp
    com/DrcApo.def
)

target_include_directories(DrcApo PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/com"
)

# advapi32：Global\ 共享内存的显式安全描述符（ConvertStringSecurityDescriptorToSecurityDescriptorW）
target_link_libraries(DrcApo PRIVATE drc_dsp ole32 uuid advapi32)

# DRC_APO_LOG=1：加载/初始化阶段诊断日志（TASK-002B，默认开；=0 时全为空操作）
target_compile_definitions(DrcApo PRIVATE UNICODE _UNICODE NOMINMAX DRC_APO_LOG=1)
# /utf-8 必须：源码含中文注释，否则中文 Windows 下按 GBK 解析可能报 C4819/解析错
target_compile_options(DrcApo PRIVATE /W4 /utf-8)

# dsp-core 真接线开关：源文件 + 公共头（dsp/include/drc/dsp.h）都就绪才开
if(DRC_DSP_SOURCES AND EXISTS "${DRC_DSP_DIR}/include/drc/dsp.h")
    target_compile_definitions(DrcApo PRIVATE DRC_HAVE_DSP_CORE=1)
    message(STATUS "DrcApo: dsp-core 已落地 => 启用 DRC_HAVE_DSP_CORE 真接线")
else()
    message(STATUS "DrcApo: dsp-core 未落地 => 壳直通（TASK-002 既定降级）")
endif()
