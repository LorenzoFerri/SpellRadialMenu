#include "core/common.h"
#include "game/equipment/radial_slots.h"
#include "game/input/native_input.h"
#include "input/radial_input.h"
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
    const bool radial_slots = radial_menu_mod::InitializeRadialSlots();
    const bool native_input = radial_menu_mod::native_input::Initialize();
    const bool d3d = radial_menu_mod::dx12_hook::Install();
    radial_menu_mod::Log("Initialization completed (asset_reader=%d radial_slots=%d native_input=%d d3d=%d).",
        static_cast<int>(assets),
        static_cast<int>(radial_slots),
        static_cast<int>(native_input),
        static_cast<int>(d3d));
    return 0;
}

void ShutdownRadialMenu()
{
    radial_menu_mod::dx12_hook::Shutdown();
    radial_menu_mod::asset_reader::Shutdown();
    radial_menu_mod::radial_input::Reset();
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
