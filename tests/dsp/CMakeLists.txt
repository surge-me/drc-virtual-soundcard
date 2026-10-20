# tests/dsp — ctest 断言宿主（T1 限幅 / T2 上抬 / T3 EQ / T4 旁路 / T5 热更新 / T6 负向对照）
cmake_minimum_required(VERSION 3.15)

# 独立配置（-S tests/dsp）时先挂上 apo 模块
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(drc_dsp_tests LANGUAGES CXX)
  enable_testing()
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../apo
                   ${CMAKE_CURRENT_BINARY_DIR}/apo)
endif()

add_executable(dsp_tests test_dsp.cpp)
target_link_libraries(dsp_tests PRIVATE drc_dsp)
target_compile_features(dsp_tests PRIVATE cxx_std_17)
set_target_properties(dsp_tests PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
  target_compile_options(dsp_tests PRIVATE /W4 /utf-8 $<$<CONFIG:Release>:/O2>)
else()
  target_compile_options(dsp_tests PRIVATE -Wall -Wextra $<$<CONFIG:Release>:-O2>)
endif()

foreach(t T1 T2 T3 T4 T5 T6 T7)
  add_test(NAME ${t} COMMAND dsp_tests ${t})
endforeach()
