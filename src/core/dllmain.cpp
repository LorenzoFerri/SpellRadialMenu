#include "core/common.h"
#include "game/equipment/spell_manager.h"
#include "game/input/game_input_probe.h"
#include "input/input_hook.h"
#include "render/d3d/dx12_hook.h"
#include "render/vfs/asset_reader.h"

#include <MinHook.h>
#include <windows.h>

namespace {

DWORD WINAPI InitializeRadialMenu(LPVOID)
{
    radial_menu_mod::Log("Initialization started.");
    if (MH_Initialize() != MH_OK) {
        radial_menu_mod::Log("MinHook initialization failed.");
        return 0;
    }

    const bool assets = radial_menu_mod::asset_reader::Install();
    const bool spells = radial_menu_mod::InitializeSpellManager();
    const bool game_input = radial_menu_mod::game_input_probe::Initialize();
    const bool input = radial_menu_mod::input_hook::Install();
    const bool d3d = radial_menu_mod::dx12_hook::Install();
    radial_menu_mod::Log("Initialization completed (asset_reader=%d spell_manager=%d game_input=%d input=%d d3d=%d).",
        static_cast<int>(assets),
        static_cast<int>(spells),
        static_cast<int>(game_input),
        static_cast<int>(input),
        static_cast<int>(d3d));
    return 0;
}

void ShutdownRadialMenu()
{
    radial_menu_mod::dx12_hook::Shutdown();
    radial_menu_mod::asset_reader::Shutdown();
    radial_menu_mod::input_hook::Shutdown();
    MH_Uninitialize();
    radial_menu_mod::Log("Shutdown completed.");
    radial_menu_mod::ShutdownLog();
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, &InitializeRadialMenu, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
        break;
    }
    case DLL_PROCESS_DETACH:
        ShutdownRadialMenu();
        break;
    default:
        break;
    }

    return TRUE;
}
