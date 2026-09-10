cmake_minimum_required(VERSION 3.16)
project(psychgpu_native LANGUAGES CXX)

add_library(psychgpu_native STATIC
    src/mapped_note_buffer.cpp
)

target_compile_features(psychgpu_native PUBLIC cxx_std_17)
target_include_directories(psychgpu_native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(MSVC)
    target_compile_options(psychgpu_native PRIVATE
        /W4 /permissive- /EHsc /O2
    )
else()
    target_compile_options(psychgpu_native PRIVATE
        -Wall -Wextra -Wpedantic -O3
    )
endif()
