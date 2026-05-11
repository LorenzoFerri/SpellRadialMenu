cmake_minimum_required(VERSION 3.24)

project(RadialMenu LANGUAGES C CXX)

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
        message(FATAL_ERROR "Missing vendored dependency file: ${required_path}. Run build.sh on Linux or build.ps1 on Windows.")
    endif()
endforeach()

set(RSM_CORE_SOURCES
    src/core/dllmain.cpp
    src/core/common.h
    src/core/log.cpp
)

set(RSM_GAME_SOURCES
    src/game/equipment/equip_access.cpp
    src/game/equipment/equip_access.h
    src/game/equipment/spell_manager.cpp
    src/game/equipment/spell_manager.h
    src/game/input/game_input_probe.cpp
    src/game/input/game_input_probe.h
    src/game/input/in_game_pad.cpp
    src/game/input/in_game_pad.h
    src/game/input/radial_camera.cpp
    src/game/input/radial_camera.h
    src/game/input/radial_switch.cpp
    src/game/input/radial_switch.h
    src/game/messages/message_repository.cpp
    src/game/messages/message_repository.h
    src/game/metadata/seamless_coop_metadata.cpp
    src/game/metadata/seamless_coop_metadata.h
    src/game/metadata/spell_metadata.cpp
    src/game/metadata/spell_metadata.h
    src/game/params/param_repository.cpp
    src/game/params/param_repository.h
    src/game/state/gameplay_state.cpp
    src/game/state/gameplay_state.h
    src/game/state/singleton_resolver.cpp
    src/game/state/singleton_resolver.h
)

set(RSM_INPUT_SOURCES
    src/input/input_hook.cpp
    src/input/input_hook.h
    src/input/radial_input.cpp
    src/input/radial_input.h
)

set(RSM_RENDER_SOURCES
    src/render/assets/dcx.cpp
    src/render/assets/dcx.h
    src/render/assets/icon_assets.cpp
    src/render/assets/icon_assets.h
    src/render/d3d/d3d_texture_upload.cpp
    src/render/d3d/d3d_texture_upload.h
    src/render/d3d/dx12_hook.cpp
    src/render/d3d/dx12_hook.h
    src/render/d3d/dx12_vtable.cpp
    src/render/d3d/dx12_vtable.h
    src/render/icons/icon_loader.cpp
    src/render/icons/icon_loader.h
    src/render/ui/eldenring_font.h
    src/render/ui/radial_menu.cpp
    src/render/ui/radial_menu.h
    src/render/ui/radial_menu_draw.cpp
    src/render/ui/radial_menu_draw.h
    src/render/vfs/asset_reader.cpp
    src/render/vfs/asset_reader.h
    src/render/vfs/game_vfs.cpp
    src/render/vfs/game_vfs.h
    src/render/vfs/path_utils.cpp
    src/render/vfs/path_utils.h
)

set(RSM_VENDOR_SOURCES
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

add_library(RadialMenu SHARED
    ${RSM_CORE_SOURCES}
    ${RSM_GAME_SOURCES}
    ${RSM_INPUT_SOURCES}
    ${RSM_RENDER_SOURCES}
    ${RSM_VENDOR_SOURCES}
)

target_include_directories(RadialMenu PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${IMGUI_ROOT}"
    "${IMGUI_ROOT}/backends"
    "${MINHOOK_ROOT}/include"
    "${MINHOOK_ROOT}/src"
    "${MINHOOK_ROOT}/src/hde"
)

target_compile_definitions(RadialMenu PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
)

target_compile_options(RadialMenu PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)

target_link_libraries(RadialMenu PRIVATE
    d3d12
    d3dcompiler
    dxgi
    dwmapi
    bcrypt
    user32
    gdi32
    xinput
)

if(MINGW)
    target_link_options(RadialMenu PRIVATE
        -static-libgcc
        -static-libstdc++
    )
endif()

set_target_properties(RadialMenu PROPERTIES
    OUTPUT_NAME "RadialMenu"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/natives"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/natives"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
