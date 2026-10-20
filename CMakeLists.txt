# 构建入口（TASK-001 dsp-core 建立）。
# 模块级定义：apo/CMakeLists.txt（drc_dsp 静态库；apo/com 由 TASK-002 追加）
#              tests/dsp/CMakeLists.txt（ctest 断言宿主 T1–T6）
# 验收命令（仓根执行）：
#   cmake -S . -B build -A x64
#   cmake --build build --config Release
#   ctest --test-dir build -C Release --output-on-failure
cmake_minimum_required(VERSION 3.15)
project(drc LANGUAGES CXX)

enable_testing()

add_subdirectory(apo)
add_subdirectory(tests/dsp)

# TASK-006 audio-router：用户态虚拟声卡管道（router 卡写区 router/**、tests/router/**；
# 本文件的两行接入属跨模块集成点，已在该卡 devlog 报备）
add_subdirectory(router)
add_subdirectory(tests/router)
