# router/CMakeLists.txt — DrcRouter（用户态虚拟声卡管道，TASK-006）
#
# 写区：router/**（本卡）
#   drc_router_core STATIC  格式垫片 / IPC 通道 / DSP 桥 / WASAPI 封装 / 主引擎
#                            （离线可被 tests/router 驱动）
#   DrcRouter        EXE     控制台主程序
# 依赖：drc_dsp（apo/dsp 静态库，TASK-001 写区，本卡只编入不改其内容）
cmake_minimum_required(VERSION 3.15)
project(DrcRouter LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "DrcRouter 仅支持 MSVC（WASAPI/Windows）。")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2")

# ── dsp-core：复用 apo/CMakeLists.txt 已定义的 drc_dsp；独立配置时自行挂载 ──────
if(NOT TARGET drc_dsp)
    set(DRC_DSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../apo/dsp")
    file(GLOB_RECURSE DRC_DSP_SOURCES CONFIGURE_DEPENDS "${DRC_DSP_DIR}/*.cpp")
    file(GLOB_RECURSE DRC_DSP_HEADERS CONFIGURE_DEPENDS "${DRC_DSP_DIR}/*.h")
    if(NOT DRC_DSP_SOURCES)
        message(FATAL_ERROR "router 需要 dsp-core（apo/dsp）源文件。")
    endif()
    add_library(drc_dsp STATIC ${DRC_DSP_SOURCES} ${DRC_DSP_HEADERS})
    target_include_directories(drc_dsp PUBLIC "${DRC_DSP_DIR}/include")
    target_compile_options(drc_dsp PRIVATE /W4 /utf-8)
endif()

# ── router 核心静态库（离线可测） ─────────────────────────────────────────
add_library(drc_router_core STATIC
    src/sample_format.cpp
    src/shm_channel.cpp
    src/dsp_bridge.cpp
    src/wasapi.cpp
    src/router_engine.cpp
)
target_include_directories(drc_router_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../apo/include"   # drc_ipc.h（SPEC §4 唯一定义点，只 include）
)
target_link_libraries(drc_router_core PUBLIC drc_dsp ole32 uuid advapi32 winmm)
target_compile_definitions(drc_router_core PUBLIC UNICODE _UNICODE NOMINMAX)
target_compile_options(drc_router_core PRIVATE /W4 /utf-8)

# ── 变异开关（TASK-012 回归对照用，默认关）──────────────────────────────────
# 打开 = 让 DecideWarden 恒返回 Exit（= 端点丢失即退出的**修复前行为**）。
# 用来给「端点热插拔自愈」做有区分力的对照：变异体在实验里必须**死掉**，
# 修复版必须**存活并重连**。生产构建不得打开（R6A 会转红）。
option(DRC_WARDEN_FORCE_EXIT "mutation control: warden always Exit (pre-fix behavior)" OFF)
if(DRC_WARDEN_FORCE_EXIT)
    message(STATUS "!! DRC_WARDEN_FORCE_EXIT=ON —— 本次构建是回归变异体，不得交付")
    target_compile_definitions(drc_router_core PUBLIC DRC_WARDEN_FORCE_EXIT=1)
endif()

# ── DrcRouter.exe ───────────────────────────────────────────────────────
add_executable(DrcRouter src/DrcRouter/main.cpp)
target_link_libraries(DrcRouter PRIVATE drc_router_core)
target_compile_options(DrcRouter PRIVATE /W4 /utf-8)
