cmake_minimum_required(VERSION 3.21)

project(vt3rt
    VERSION 0.1.0
    DESCRIPTION "Native runtime for the recompiled Virtua Tennis 3 (PC)"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# SDL3 from Homebrew (sdl3 3.4.x). Its CMake config ships with the formula.
find_package(SDL3 REQUIRED CONFIG)

set(VT3_WARNINGS "")
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set(VT3_WARNINGS -Wall -Wextra -Wno-unused-parameter)
endif()

# Platform layer, renderer, audio, input, and the ADX decoder.
add_library(vt3core STATIC
    src/core/log.c
    src/core/guest.c
    src/render/letterbox.c
    src/render/renderer.c
    src/render/gl33.c
    src/audio/ringbuf.c
    src/audio/audio_sdl.c
    src/audio/adx.c
    src/input/input.c
    src/input/input_sdl.c
    src/platform/platform_sdl.c
)
target_include_directories(vt3core PUBLIC include)
target_link_libraries(vt3core PUBLIC SDL3::SDL3)
target_compile_options(vt3core PRIVATE ${VT3_WARNINGS})

# Win32/D3D9 shim: headers plus skeleton implementations.
add_library(vt3shim STATIC
    shim/src/shim_core.c
    shim/src/kernel32_heap.c
    shim/src/kernel32_file.c
    shim/src/kernel32_time.c
    shim/src/d3d9_stub.c
    shim/src/dsound_stub.c
)
target_include_directories(vt3shim PUBLIC shim/include)
target_link_libraries(vt3shim PUBLIC vt3core)
target_compile_options(vt3shim PRIVATE ${VT3_WARNINGS})

# The runtime executable. It also carries the self-test.
add_executable(vt3rt
    src/main.c
    tests/selftest.c
)
target_include_directories(vt3rt PRIVATE tests)
target_link_libraries(vt3rt PRIVATE vt3shim)
target_compile_options(vt3rt PRIVATE ${VT3_WARNINGS})

enable_testing()
add_test(NAME vt3_selftest COMMAND vt3rt --selftest)
set_tests_properties(vt3_selftest PROPERTIES SKIP_RETURN_CODE 77)
