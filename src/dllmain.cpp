#include "common.h"
#include "dx12_hook.h"
#include "input_hook.h"
#include "spell_manager.h"

#include <MinHook.h>
#include <windows.h>

namespace {

DWORD WINAPI InitializeRadialSpellMenu(LPVOID)
{
    if (MH_Initialize() != MH_OK) {
        radial_spell_menu::Log("MinHook initialization failed.");
        return 0;
    }

    radial_spell_menu::InitializeSpellManager();
    radial_spell_menu::input_hook::Install();
    radial_spell_menu::dx12_hook::Install();
    radial_spell_menu::Log("Initialization completed.");
    return 0;
}

void ShutdownRadialSpellMenu()
{
    radial_spell_menu::dx12_hook::Shutdown();
    radial_spell_menu::input_hook::Shutdown();
    MH_Uninitialize();
    radial_spell_menu::Log("Shutdown completed.");
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, &InitializeRadialSpellMenu, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
        break;
    }
    case DLL_PROCESS_DETACH:
        ShutdownRadialSpellMenu();
        break;
    default:
        break;
    }

    return TRUE;
}
