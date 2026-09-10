cmake_minimum_required(VERSION 3.16)
project(psych_gpu_chart_compiler LANGUAGES CXX)

add_executable(chart_compiler chart_compiler.cpp)
target_compile_features(chart_compiler PRIVATE cxx_std_17)
target_include_directories(chart_compiler PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../native/psychgpu/include"
)

if(MSVC)
    target_compile_options(chart_compiler PRIVATE /W4 /permissive- /EHsc /O2)
else()
    target_compile_options(chart_compiler PRIVATE
        -Wall -Wextra -Wpedantic -O3)
endif()
