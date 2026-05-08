#include "input/input_hook.h"

#include "core/common.h"
#include "game/state/gameplay_state.h"
#include "input/radial_input.h"
#include "render/ui/radial_menu.h"

#include <MinHook.h>
#include <windows.h>
#include <xinput.h>

namespace radial_menu_mod::input_hook {

namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetStateFn g_original_xinput_get_state = nullptr;
LPVOID g_xinput_target = nullptr;

DWORD WINAPI HookedXInputGetState(DWORD user_index, XINPUT_STATE* state)
{
    const DWORD result = g_original_xinput_get_state(user_index, state);
    if (result == ERROR_SUCCESS) radial_input::HandleControllerState(user_index, state);

    return result;
}

}  // namespace

bool Install()
{
    if (g_xinput_target != nullptr) {
        return true;
    }

    constexpr const wchar_t* kXinputModules[] = {
        L"xinput1_4.dll",
        L"xinput1_3.dll",
        L"xinput9_1_0.dll",
    };

    for (const auto* module_name : kXinputModules) {
        HMODULE module = LoadLibraryW(module_name);
        if (module == nullptr) {
            continue;
        }

        auto* const target = reinterpret_cast<LPVOID>(GetProcAddress(module, "XInputGetState"));
        if (target == nullptr) {
            continue;
        }

        const MH_STATUS create_status = MH_CreateHook(target, reinterpret_cast<LPVOID>(&HookedXInputGetState),
            reinterpret_cast<LPVOID*>(&g_original_xinput_get_state));
        if (create_status != MH_OK) {
            Log("Failed to create XInput hook: %s", MH_StatusToString(create_status));
            return false;
        }

        const MH_STATUS enable_status = MH_EnableHook(target);
        if (enable_status != MH_OK) {
            Log("Failed to enable XInput hook: %s", MH_StatusToString(enable_status));
            return false;
        }

        g_xinput_target = target;
        Log("Installed XInputGetState hook from %ls.", module_name);
        return true;
    }

    Log("Unable to locate an XInput DLL to hook.");
    return false;
}

void Shutdown()
{
    if (g_xinput_target == nullptr) {
        return;
    }

    MH_DisableHook(g_xinput_target);
    MH_RemoveHook(g_xinput_target);
    radial_input::Reset();
    g_xinput_target = nullptr;
    g_original_xinput_get_state = nullptr;
}

bool IsMenuOpen()
{
    return radial_menu::IsOpen();
}

bool IsGameplayReady()
{
    return gameplay_state::IsNormalGameplayHudState();
}

const std::vector<SpellSlot>& GetOpenSpellSlots()
{
    return radial_input::GetOpenSpellSlots();
}

const char* GetOpenMenuTitle()
{
    return radial_input::GetOpenMenuTitle();
}

const char* GetOpenMenuControls()
{
    return radial_input::GetOpenMenuControls();
}

}  // namespace radial_menu_mod::input_hook
