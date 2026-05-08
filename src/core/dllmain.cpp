#include "core/common.h"
#include "game/equipment/spell_manager.h"
#include "input/input_hook.h"
#include "render/d3d/dx12_hook.h"
#include "render/vfs/asset_reader.h"

#include <MinHook.h>
#include <windows.h>

namespace {

DWORD WINAPI InitializeRadialMenu(LPVOID)
{
    if (MH_Initialize() != MH_OK) {
        radial_menu_mod::Log("MinHook initialization failed.");
        return 0;
    }

    radial_menu_mod::asset_reader::Install();
    radial_menu_mod::InitializeSpellManager();
    radial_menu_mod::input_hook::Install();
    radial_menu_mod::dx12_hook::Install();
    radial_menu_mod::Log("Initialization completed.");
    return 0;
}

void ShutdownRadialMenu()
{
    radial_menu_mod::dx12_hook::Shutdown();
    radial_menu_mod::asset_reader::Shutdown();
    radial_menu_mod::input_hook::Shutdown();
    MH_Uninitialize();
    radial_menu_mod::Log("Shutdown completed.");
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
