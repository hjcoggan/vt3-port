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

# Android builds use NDK toolchain (android.toolchain.cmake): NativeActivity, AAudio and Vulkan.
# Desktop builds use SDL3 from Homebrew (sdl3 3.4.x), whose CMake config ships with the formula.
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(VT3_ANDROID ON)
else()
    set(VT3_ANDROID OFF)
    find_package(SDL3 REQUIRED CONFIG)
    # The desktop build carries the Vulkan backend too (MoltenVK on macOS): it is selected at run
    # time with VT3_RENDERER=vulkan, while GL stays the default.
    find_package(Vulkan REQUIRED)
endif()

set(VT3_WARNINGS "")
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set(VT3_WARNINGS -Wall -Wextra -Wno-unused-parameter)
endif()

# Hot call paths: the shim and runtime call into libc constantly, and each cross-library call is
# a PLT hop (1.9% of in-match CPU on the Retroid). -fno-plt turns those into GOT-indirect calls.
# No -ftls-model=initial-exec: NativeActivity dlopen()s the game library, and at android-26 the
# NDK uses emulated TLS anyway (the __thread cost is handled in the adapters instead, see
# vt3s_guest_ctx_on). Android only: the desktop build is not the one short of CPU.
set(VT3_ANDROID_FASTCALL)
if(CMAKE_SYSTEM_NAME STREQUAL "Android" AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set(VT3_ANDROID_FASTCALL -fno-plt)
    # The NDK adds -fstack-protector-strong to everything; the desktop build never had it. The
    # translated game is thousands of small functions called millions of times a second, and every
    # one that owns a local array pays a canary store and check. The port reads its own game data
    # and has no untrusted input reaching those frames, so the canary buys nothing here.
    add_compile_options(-fno-stack-protector)
endif()

# Platform layer, renderer, audio, input, and the ADX decoder.
set(VT3_CORE_COMMON
    src/core/log.c
    src/core/guest.c
    src/render/letterbox.c
    src/render/sm2_glsl.c
    src/render/renderer.c
    src/render/builtin_glsl.c
    src/audio/ringbuf.c
    src/audio/adx.c
    src/input/input.c
    src/ui/touch_controls.c
)
if(VT3_ANDROID)
    set(VT3_CORE_PLATFORM
        src/audio/audio_android.c
        src/input/input_android.c
        src/platform/platform_android.c
        src/render/vk.c
    )
else()
    set(VT3_CORE_PLATFORM
        src/render/gl33.c
        src/render/vk.c
        src/audio/audio_sdl.c
        src/input/input_sdl.c
        src/platform/platform_sdl.c
    )
endif()
add_library(vt3core STATIC ${VT3_CORE_COMMON} ${VT3_CORE_PLATFORM})
target_include_directories(vt3core PUBLIC include)
if(VT3_ANDROID)
    target_link_libraries(vt3core PUBLIC android log aaudio m)
    target_include_directories(vt3core PRIVATE ${ANDROID_NDK}/sources/android/native_app_glue)
else()
    target_link_libraries(vt3core PUBLIC SDL3::SDL3 Vulkan::Vulkan)
endif()
target_compile_options(vt3core PRIVATE ${VT3_WARNINGS} ${VT3_ANDROID_FASTCALL})

# Win32/D3D9 shim: headers plus skeleton implementations.
add_library(vt3shim STATIC
    shim/src/shim_core.c
    shim/src/kernel32_heap.c
    shim/src/kernel32_file.c
    shim/src/kernel32_time.c
    shim/src/kernel32_std.c
    shim/src/kernel32_sys.c
    shim/src/kernel32_thread.c
    shim/src/winmm_timer.c
    shim/src/user32_stub.c
    shim/src/user32_window.c
    shim/src/user32_text.c
    shim/src/d3d9_object.c
    shim/src/d3d9_iface.c
    shim/src/d3d9_device.c
    shim/src/d3d9_com_unimpl.c
    shim/src/d3d9_com_generic.c
    shim/src/d3d9_format.c
    shim/src/d3d9_gpu.c
    shim/src/d3d9_resource.c
    shim/src/d3d9_draw.c
    shim/src/gfx_weak.c
    shim/src/d3dx9_math.c
    shim/src/d3dx9_compile.c
    shim/src/d3dx9_ctable.c
    shim/src/d3dx9_rebuilt.c
    shim/src/d3dx9_stub.c
    shim/src/dsound_stub.c
    shim/src/dinput8.c
)
target_include_directories(vt3shim PUBLIC shim/include)
target_link_libraries(vt3shim PUBLIC vt3core)
target_compile_options(vt3shim PRIVATE ${VT3_WARNINGS} ${VT3_ANDROID_FASTCALL})

if(VT3_ANDROID)
    # The game for the device: the loader, the adapters, the shim and the translated module, as a
    # shared library that NativeActivity loads. VT3_GAME_DIR holds vt3.c (the translated game) and
    # the generated adapters are built from the shim sources, as on the desktop (build_native.py).
    if(NOT VT3_GAME_DIR)
        message(FATAL_ERROR "Set -DVT3_GAME_DIR to the folder with the translated vt3.c")
    endif()
    set(VT3_RT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../tools/recomp/rt)
    set(VT3_ADAPTERS ${CMAKE_CURRENT_BINARY_DIR}/vt3_shim_adapters.c)
    set(VT3_SHIM_SOURCES
        shim/src/shim_core.c shim/src/kernel32_heap.c shim/src/kernel32_file.c shim/src/kernel32_time.c
        shim/src/kernel32_std.c shim/src/kernel32_sys.c shim/src/kernel32_thread.c shim/src/winmm_timer.c
        shim/src/user32_stub.c shim/src/user32_window.c shim/src/user32_text.c shim/src/d3d9_object.c
        shim/src/d3d9_iface.c shim/src/d3d9_device.c shim/src/d3d9_com_unimpl.c shim/src/d3d9_com_generic.c
        shim/src/d3d9_format.c shim/src/d3d9_gpu.c shim/src/d3d9_resource.c shim/src/d3d9_draw.c
        shim/src/gfx_weak.c shim/src/d3dx9_math.c shim/src/d3dx9_compile.c shim/src/d3dx9_ctable.c
        shim/src/d3dx9_rebuilt.c shim/src/d3dx9_stub.c shim/src/dsound_stub.c shim/src/dinput8.c)
    add_custom_command(
        OUTPUT ${VT3_ADAPTERS}
        COMMAND python3 ${VT3_RT_DIR}/gen_adapters.py ${CMAKE_CURRENT_SOURCE_DIR}/shim ${VT3_ADAPTERS} ${VT3_SHIM_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        DEPENDS ${VT3_RT_DIR}/gen_adapters.py ${VT3_SHIM_SOURCES}
        COMMENT "Generating the stdcall adapters")
    add_library(vt3android SHARED
        src/platform/android_entry.c
        ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
        ${VT3_RT_DIR}/vt3rt.c
        ${VT3_RT_DIR}/vt3simd.c
        ${VT3_RT_DIR}/vt3fpu.c
        ${VT3_ADAPTERS}
        ${VT3_GAME_DIR}/vt3.c
    )
    target_include_directories(vt3android PRIVATE ${VT3_RT_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${ANDROID_NDK}/sources/android/native_app_glue)
    target_compile_options(vt3android PRIVATE -O2 -std=gnu11 -w -ffp-contract=off ${VT3_ANDROID_FASTCALL})
    # vt3.c is about 100 MB. -O2 without debug info (the NDK adds -g; the library is stripped
    # anyway) compiles in 148 s at 5.7 GB peak, against 253 s and 7.6 GB for -O1 with it, and
    # the Retroid ran 2-4 fps faster in three rotated pairs.
    set(VT3_PGO "" CACHE STRING "gen, or the path of a merged .profdata to use")
    if(VT3_PGO STREQUAL "gen")
        set_source_files_properties(${VT3_GAME_DIR}/vt3.c PROPERTIES COMPILE_OPTIONS "-O2;-g0;-std=gnu11;-w;-ffp-contract=off;-fprofile-generate")
        target_compile_definitions(vt3android PRIVATE VT3_PGO_GEN)
        target_link_options(vt3android PRIVATE -fprofile-generate)
    elseif(VT3_PGO)
        set_source_files_properties(${VT3_GAME_DIR}/vt3.c PROPERTIES COMPILE_OPTIONS "-O2;-g0;-std=gnu11;-w;-ffp-contract=off;-fprofile-use=${VT3_PGO};-Wno-profile-instr-unprofiled")
    else()
        set_source_files_properties(${VT3_GAME_DIR}/vt3.c PROPERTIES COMPILE_OPTIONS "-O2;-g0;-std=gnu11;-w;-ffp-contract=off")
    endif()
    # The weak defaults in vt3shim must lose to the real renderer, platform and audio in vt3core, so
    # every member of both archives is linked in (as -all_load does on the desktop).
    target_link_libraries(vt3android PRIVATE "-Wl,--whole-archive" vt3shim vt3core "-Wl,--no-whole-archive"
        android log aaudio vulkan m)
    target_link_options(vt3android PRIVATE -Wl,-z,max-page-size=16384)

    # The self-test as a native executable, to run on the device with adb shell.
    add_executable(vt3selftest_android tests/android_selftest_main.c tests/selftest.c)
    target_include_directories(vt3selftest_android PRIVATE tests)
    target_link_libraries(vt3selftest_android PRIVATE vt3shim android log aaudio m)
    return()
endif()

# The runtime executable. It also carries the self-test.
add_executable(vt3rt
    src/main.c
    tests/selftest.c
    tests/layout_check.c   # compile-time only: checks the COM vtable layouts
    tests/sm2_glcheck.c    # --sm2-check: GL compile and link of SM2 shaders
)
if(APPLE)
    target_link_libraries(vt3rt PRIVATE "-framework OpenGL")
endif()
target_include_directories(vt3rt PRIVATE tests)
target_link_libraries(vt3rt PRIVATE vt3shim)
target_compile_options(vt3rt PRIVATE ${VT3_WARNINGS})
# The weak defaults in vt3shim must lose to the real renderer, platform and translator in vt3core.
# A static archive would not pull those members in, so force every member in.
if(APPLE)
    target_link_options(vt3rt PRIVATE "-Wl,-all_load")
endif()

enable_testing()
add_test(NAME vt3_selftest COMMAND vt3rt --selftest)
set_tests_properties(vt3_selftest PROPERTIES SKIP_RETURN_CODE 77)
