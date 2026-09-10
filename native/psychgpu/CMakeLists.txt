cmake_minimum_required(VERSION 3.16)
project(psychgpu_native LANGUAGES CXX)

option(PSYCHGPU_BUILD_TESTS "Build native mapped-note tests" ON)

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

if(PSYCHGPU_BUILD_TESTS)
    enable_testing()
    add_executable(mapped_note_buffer_test
        tests/mapped_note_buffer_test.cpp
    )
    target_link_libraries(mapped_note_buffer_test PRIVATE psychgpu_native)
    add_test(NAME mapped_note_buffer_test COMMAND mapped_note_buffer_test)
endif()
