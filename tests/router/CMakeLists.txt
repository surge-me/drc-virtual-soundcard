# tests/router — audio-router 离线可机械验证宿主（TASK-006）
#   R1 DSP 接线 / R2 IPC 回路（离线）  R3 WASAPI 探测 / R4 loopback 自捕获（真实设备，只读不注入）
cmake_minimum_required(VERSION 3.15)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(drc_router_tests LANGUAGES CXX)
    enable_testing()
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../router
                     ${CMAKE_CURRENT_BINARY_DIR}/router)
endif()

add_executable(router_tests test_router.cpp)
target_link_libraries(router_tests PRIVATE drc_router_core)
target_compile_features(router_tests PRIVATE cxx_std_17)
set_target_properties(router_tests PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(router_tests PRIVATE /W4 /utf-8 $<$<CONFIG:Release>:/O2>)
else()
    target_compile_options(router_tests PRIVATE -Wall -Wextra $<$<CONFIG:Release>:-O2>)
endif()

foreach(t R1 R2 R3 R4 R5 R6)
    add_test(NAME ${t} COMMAND router_tests ${t})
endforeach()

# R3/R4/R6 依赖真实音频端点；无端点的会话（如断开的远程会话/无人登录的控制台）返回 77
# => ctest 记为 Skipped（非 PASS、非 FAIL），避免「环境缺设备」被误报成代码缺陷。
# R6 的 Part B（Run() 真的调用看护策略）需要能 Open 成功；Part A（策略映射）与设备无关。
set_tests_properties(R3 R4 R6 PROPERTIES SKIP_RETURN_CODE 77)
