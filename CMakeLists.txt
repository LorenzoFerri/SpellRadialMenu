cmake_minimum_required(VERSION 3.24)

project(RadialSpellMenu LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT WIN32)
    message(FATAL_ERROR "Configure this project with the provided Windows mingw-w64 toolchain file.")
endif()

set(IMGUI_ROOT "${CMAKE_SOURCE_DIR}/vendor/imgui")
set(MINHOOK_ROOT "${CMAKE_SOURCE_DIR}/vendor/minhook")

foreach(required_path
        "${IMGUI_ROOT}/imgui.cpp"
        "${IMGUI_ROOT}/backends/imgui_impl_dx12.cpp"
        "${IMGUI_ROOT}/backends/imgui_impl_win32.cpp"
        "${MINHOOK_ROOT}/include/MinHook.h"
        "${MINHOOK_ROOT}/src/buffer.c")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR "Missing vendored dependency file: ${required_path}. Run ./build.sh to clone vendor libraries.")
    endif()
endforeach()

add_library(RadialSpellMenu SHARED
    src/dllmain.cpp
    src/common.h
    src/input_hook.cpp
    src/input_hook.h
    src/dx12_hook.cpp
    src/dx12_hook.h
    src/radial_menu.cpp
    src/radial_menu.h
    src/spell_metadata.cpp
    src/spell_metadata.h
    src/spell_manager.cpp
    src/spell_manager.h
    ${IMGUI_ROOT}/imgui.cpp
    ${IMGUI_ROOT}/imgui_draw.cpp
    ${IMGUI_ROOT}/imgui_tables.cpp
    ${IMGUI_ROOT}/imgui_widgets.cpp
    ${IMGUI_ROOT}/backends/imgui_impl_dx12.cpp
    ${IMGUI_ROOT}/backends/imgui_impl_win32.cpp
    ${MINHOOK_ROOT}/src/buffer.c
    ${MINHOOK_ROOT}/src/hook.c
    ${MINHOOK_ROOT}/src/trampoline.c
    ${MINHOOK_ROOT}/src/hde/hde64.c
)

target_include_directories(RadialSpellMenu PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${IMGUI_ROOT}"
    "${IMGUI_ROOT}/backends"
    "${MINHOOK_ROOT}/include"
    "${MINHOOK_ROOT}/src"
    "${MINHOOK_ROOT}/src/hde"
)

target_compile_definitions(RadialSpellMenu PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
)

target_compile_options(RadialSpellMenu PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)

target_link_libraries(RadialSpellMenu PRIVATE
    d3d12
    d3dcompiler
    dxgi
    dwmapi
    user32
    gdi32
    xinput
)

if(MINGW)
    target_link_options(RadialSpellMenu PRIVATE
        -static-libgcc
        -static-libstdc++
    )
endif()

set_target_properties(RadialSpellMenu PROPERTIES
    OUTPUT_NAME "RadialSpellMenu"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/natives"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/natives"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
